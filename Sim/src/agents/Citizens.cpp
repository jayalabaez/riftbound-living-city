// LIVING CITY - commuting citizens.
//
// R1: zero Unreal types. R3: every value below is an integer and every decision is a pure
// function of (seed, tick index, city). R4: there is no player branch anywhere in this file.
//
// ---------------------------------------------------------------------------------------
// UNITS, and why positions are stored in centimetres.
//
// The public API speaks metres (City.h fixes the world unit, and Snapshot carries metres to
// the renderer). A walking pace of 1.0-1.8 m/s at 20 Hz is 5-9 cm per tick, which is not
// representable in whole metres: rounded to 0 m per tick citizens freeze, rounded to 1 m per
// tick they sprint at 20 m/s. Storing the position in CENTIMETRES makes the step exact -
// speed_ is centimetres per tick, added with no division, no remainder and no accumulator -
// so a citizen's position is the same integer on every machine forever, and PosX/PosY divide
// by 100 once, at the boundary.
//
// A metre position plus a remainder accumulator would also have worked; a finer integer unit
// needs no remainder at all, so there is nothing left that could drift.
//
// ---------------------------------------------------------------------------------------
// PATHING. Citizens walk the road grid in an L: along X, then along Y. A travel target is
// therefore never a building centre (that is inside a block) but the road intersection
// nearest the destination - NearestRoadX/NearestRoadY of its centre.
//
// That choice is necessary but NOT sufficient on its own, and the missing half is the reason
// Generate places a new citizen at the kerb rather than at their building's centre.
//
// An L is only as on-road as the point it is drawn FROM. The X leg runs at the walker's
// CURRENT Y, so that Y has to be a road row or the leg holds block-interior height for its
// entire length - all the way across town, not just off the kerb. Two starting points:
//
//   * standing on an intersection: the X leg runs along a road ROW centreline and the Y leg
//     along a road COLUMN centreline. Neither crosses a block interior, and the walk ends on
//     another intersection, so the property reproduces itself for every commute after it;
//   * standing at a building CENTRE: it does not hold at all. An L from there walks through
//     every block between home and work. Measured on the 6x6 test city with the population
//     started at building centres, that was 5,389 citizen-ticks spent inside a footprint and
//     up to eight separate buildings walked through by a single commuter - each one a person
//     strolling through somebody's living room in full view of the player.
//
// So the invariant is: A CITIZEN IS ALWAYS ON A ROAD CENTRELINE IN AT LEAST ONE AXIS. Every
// position a citizen can hold is either a road intersection (Generate's kerb placement, and
// every arrival) or a point part-way along one centreline between two of them. Nothing here
// needs a proximity test, a third leg, or a "get to the road first" special case, because no
// citizen is ever off the grid to begin with.
//
// That invariant is worth stating loudly because NOTHING ELSE IN THE SUITE NOTICES when it
// breaks. Started from building centres the day still completes, every arrival is still
// exact, the hash is still deterministic and all eleven original tests still passed - the
// citizens simply walked through walls. test_citizens.cpp now pins it directly, twice.
//
// Because a target is always a road intersection and the step clamps instead of overshooting,
// "arrived" is an equality, not a proximity test.
//
// ---------------------------------------------------------------------------------------
// ERRANDS. The economy decides that a citizen should shop, and where; this file only walks
// them there and back. A pending errand is a single building index in errand_, and it fires
// at the citizen's next departure:
//
//   * at shift end, instead of walking straight home (AtWork -> TravelShop -> AtShop ->
//     TravelHome), or
//   * from home outside working hours (AtHome -> TravelShop -> ... -> AtHome).
//
// Going to work always wins. An errand pending at shift start is deferred, not dropped: the
// citizen goes to work and shops on the way home.
//
// The dwell is a plain countdown. dwell_ is set to kShopDwellTicks on the tick the citizen
// becomes AtShop and decremented on every AtShop tick after it; the tick it reaches zero is
// the tick they leave. So a citizen is observed AtShop for exactly kShopDwellTicks ticks, and
// JustArrivedAtShop - "AtShop with the countdown still full" - is true on exactly the first
// of them. The economy rings up the sale on that tick and reads PendingErrand to learn which
// shop, which is why the errand is cleared at DEPARTURE, not on arrival: it has to stay
// readable for the whole visit.
//
// A TRIP IN PROGRESS IS IMMUTABLE. From the departure tick to the departure-for-home tick
// (TravelShop and AtShop) SetPendingErrand is a no-op, whatever it is handed: not a new shop,
// not kNoErrand. This is not merely "never redirect a walk mid-street". The economy re-decides
// errands once a sim-minute from the citizen's CURRENT position, and a walk to a shop can take
// 45 sim-seconds, so a citizen halfway to shop A is routinely handed shop B - or nothing. If
// that overwrote errand_ while targetX_/targetY_ still pointed at A, the citizen would finish
// the walk to A, JustArrivedAtShop would fire, and the economy would ring up PendingErrand:
// shop B's stock and shop B's account, for a sale at A's counter - or index kNoErrand. So
// during a trip errand_ IS the destination, and the sale always lands where the feet went.
// The visit still consumes the errand when it ends; interference during it queues nothing.
//
// Outside a trip SetPendingErrand only stores, and kNoErrand cancels what is pending.
#include "livingcity/agents/Citizens.h"

namespace lc {

namespace {

// File-local names are prefixed on purpose: the Unreal build compiles the whole sim as ONE
// translation unit (D-005), so two sim .cpp files with an identically named constant in an
// anonymous namespace would be a redefinition there while compiling clean standalone.
constexpr i32 kCitizenCmPerMetre = 100;

// Walking pace in centimetres per tick. At 20 Hz that is 1.00 - 1.80 m/s.
constexpr i32 kCitizenSpeedMinCmPerTick = 100;
constexpr i32 kCitizenSpeedMaxCmPerTick = 180;

// Shifts start between 06:00 and 10:00 and run 8 or 9 hours, so the latest end is 19:00 and
// no shift ever wraps past midnight - the schedule below relies on shiftEnd > shiftStart.
// The stagger is not decoration: it is the difference between a street that fills up over
// four sim-hours and an entire population stepping outside on one tick.
// Two shift bands, so the streets have life all day and not only at two rush hours. Most of
// the city starts in the morning; the rest start in the afternoon, which puts their commute
// and their evening errands in exactly the hours the morning shift is indoors.
//
// No shift crosses midnight: the latest start plus the longest shift ends at 23:00, so the
// schedule stays a same-day comparison and never has to reason about wrapping.
constexpr i32 kCitizenShiftStartMinHour   = 6;    // earliest morning start
constexpr i32 kCitizenMorningStartMaxHour = 10;   // last morning start
constexpr i32 kCitizenShiftStartMaxHour   = 14;   // last afternoon start
constexpr u32 kCitizenMorningShiftPercent = 60u;  // share of citizens on the morning band
constexpr i32 kCitizenShiftMinHours       = 8;
constexpr i32 kCitizenShiftMaxHours       = 9;
static_assert(kCitizenShiftStartMaxHour + kCitizenShiftMaxHours < kHoursPerDay,
              "a shift must end before midnight - the schedule does not wrap");

// One citizen in four staffs a Shop rather than a Work lot, when the city has both.
constexpr u32 kCitizenShopJobOneIn = 4u;

// leg_ encoding, fixed by Citizens.h.
constexpr u8 kCitizenLegX       = 0u;
constexpr u8 kCitizenLegY       = 1u;
constexpr u8 kCitizenLegArrived = 2u;

constexpr u32 kCitizenActivityCount = static_cast<u32>(Activity::Count);

// The AtShop countdown asserts that it is never zero on entry, and JustArrivedAtShop is
// defined as "countdown still full". A zero dwell would make the first of those abort on the
// arrival tick and the second true forever; catch a careless header edit at compile time.
static_assert(Citizens::kShopDwellTicks > 0u, "kShopDwellTicks must be at least one tick");

// A trip in progress: the citizen is walking to a shop or standing at its counter. Between
// those two states errand_ is the destination, not a request, and must not be touched.
bool CitizenIsOnErrandTrip(Activity a) {
    return a == Activity::TravelShop || a == Activity::AtShop;
}

// Move toward the target by at most `step`, clamping exactly onto it rather than past it.
// The clamp is what makes arrival an equality test: a citizen can never finish a tick one
// step beyond the target and then oscillate around it for the rest of the run.
i32 CitizenStepToward(i32 pos, i32 target, i32 step) {
    const i32 delta = target - pos;
    if (delta >  step) return pos + step;
    if (delta < -step) return pos - step;
    return target;
}

// Centimetres to metres at the public boundary, rounding DOWN rather than toward zero.
//
// This is not pedantry: the origin is the CITY CENTRE, so half of every city has negative
// coordinates, and C++ integer division truncates toward zero rather than flooring. Plain
// `cm / 100` therefore rounds negative positions UP - toward the centre - which makes the
// metre cell straddling the origin 199 cm wide instead of 100 and biases the entire western
// and southern half of the map a metre inward. Two citizens a centimetre either side of the
// origin would be handed to the renderer on the same metre, and one walking west across it
// would visibly stall for a tick and then jump.
//
// Flooring keeps every metre cell exactly one metre wide, everywhere. It changes nothing in
// sim state - positions stay in centimetres and are hashed in centimetres - only what the
// boundary reports.
i32 CitizenCmToMetres(i32 cm) {
    const i32 metres = cm / kCitizenCmPerMetre;
    return (cm % kCitizenCmPerMetre < 0) ? metres - 1 : metres;
}

} // namespace

const char* ActivityName(Activity a) {
    switch (a) {
        case Activity::AtHome:     return "AtHome";
        case Activity::TravelWork: return "TravelWork";
        case Activity::AtWork:     return "AtWork";
        case Activity::TravelHome: return "TravelHome";
        case Activity::TravelShop: return "TravelShop";
        case Activity::AtShop:     return "AtShop";
        case Activity::Count:      break;

        // Required, not decorative: without it MSVC judges the switch exhaustive, calls the
        // guard below unreachable and raises C4702 - an error under UBT.
        default: break;
    }
    // Activity is stored as a u8, so a corrupt byte or a bad cast can arrive here. I4: say so
    // loudly rather than hand the HUD a plausible name for a broken citizen.
    LC_FATAL("ActivityName: activity value is not one of the six enumerators");
}

// ---------------------------------------------------------------------------------------
void Citizens::Clear() {
    homeIndex_.clear();
    workIndex_.clear();
    posX_.clear();
    posY_.clear();
    targetX_.clear();
    targetY_.clear();
    state_.clear();
    leg_.clear();
    speed_.clear();
    shiftStart_.clear();
    shiftEnd_.clear();
    errand_.clear();
    dwell_.clear();
    for (u32 k = 0; k < kCitizenActivityCount; ++k) counts_[k] = 0u;
}

void Citizens::Generate(u64 worldSeed, const City& city, u32 count) {
    Clear();
    if (count == 0u) return;   // no population was asked for, so no lot can be missing

    // R3: the ONLY generator this function may draw from. Never a private engine, never a
    // stream another subsystem also draws from.
    RngStreams streams(worldSeed);
    Rng&       agents = streams.Stream(StreamId::Agents);

    const std::vector<u32>& homes = city.OfUse(LotUse::Home);
    const std::vector<u32>& works = city.OfUse(LotUse::Work);
    const std::vector<u32>& shops = city.OfUse(LotUse::Shop);

    // I4. A citizen with no address, or with no employment, is exactly the failure this rule
    // exists to catch, and it must never reach the player as "that one just stands there".
    // The two messages are separate because WHICH list came back empty is the only
    // diagnostically useful fact when a zoning change empties one of them.
    if (homes.empty()) {
        LC_FATAL("Citizens::Generate: the city has no Home lots - every citizen needs an address (I4)");
    }
    if (works.empty() && shops.empty()) {
        LC_FATAL("Citizens::Generate: the city has no Work or Shop lots - nobody can be employed (I4)");
    }

    const bool haveWork  = !works.empty();
    const bool haveShop  = !shops.empty();
    const u32  homeCount = static_cast<u32>(homes.size());
    const u32  workCount = static_cast<u32>(works.size());
    const u32  shopCount = static_cast<u32>(shops.size());

    // I5: every array is sized once, here. Tick() must never grow one.
    homeIndex_.reserve(count);
    workIndex_.reserve(count);
    posX_.reserve(count);
    posY_.reserve(count);
    targetX_.reserve(count);
    targetY_.reserve(count);
    state_.reserve(count);
    leg_.reserve(count);
    speed_.reserve(count);
    shiftStart_.reserve(count);
    shiftEnd_.reserve(count);
    errand_.reserve(count);
    dwell_.reserve(count);

    for (u32 i = 0; i < count; ++i) {
        const u32 home = homes[agents.Range(homeCount)];

        // Draw accounting: which branch runs is a property of the CITY, not of the citizen,
        // so it is the same for every iteration and the draw sequence stays a pure function
        // of (worldSeed, city).
        u32 job = 0u;
        if (haveWork && haveShop) {
            const bool shopJob = (agents.Range(kCitizenShopJobOneIn) == 0u);
            job = shopJob ? shops[agents.Range(shopCount)] : works[agents.Range(workCount)];
        } else if (haveWork) {
            job = works[agents.Range(workCount)];
        } else {
            job = shops[agents.Range(shopCount)];
        }

        const i32 startHour = (agents.Range(100u) < kCitizenMorningShiftPercent)
            ? agents.RangeI(kCitizenShiftStartMinHour,     kCitizenMorningStartMaxHour + 1)
            : agents.RangeI(kCitizenMorningStartMaxHour + 1, kCitizenShiftStartMaxHour + 1);
        const i32 shiftLen  = kCitizenShiftMinHours
                            + static_cast<i32>(agents.Range(static_cast<u32>(
                                  kCitizenShiftMaxHours - kCitizenShiftMinHours + 1)));
        const i32 speedCm   = agents.RangeI(kCitizenSpeedMinCmPerTick,
                                            kCitizenSpeedMaxCmPerTick + 1);

        // Everyone starts the world at home - standing at the kerb outside it, NOT at the
        // building's centre. A citizen placed at the centre is inside the geometry: invisible
        // to the player and standing in a wall. The same road-entrance rule BeginTravel uses
        // for arrivals has to apply to the very first placement too, or the whole population
        // spends the opening moments of the world buried in masonry.
        const Building& homeLot = city.At(home);
        const i32 startX = city.NearestRoadX(homeLot.centreX) * kCitizenCmPerMetre;
        const i32 startY = city.NearestRoadY(homeLot.centreY) * kCitizenCmPerMetre;

        homeIndex_.push_back(home);
        workIndex_.push_back(job);
        posX_.push_back(startX);
        posY_.push_back(startY);
        // target == position with the leg already retired, so "arrived means the position IS
        // the target" holds from tick 0, before anyone has walked anywhere.
        targetX_.push_back(startX);
        targetY_.push_back(startY);
        state_.push_back(static_cast<u8>(Activity::AtHome));
        leg_.push_back(kCitizenLegArrived);
        speed_.push_back(static_cast<u8>(speedCm));
        shiftStart_.push_back(static_cast<u8>(startHour));
        shiftEnd_.push_back(static_cast<u8>(startHour + shiftLen));
        // Nobody is born with shopping to do; the economy hands out errands once it exists.
        errand_.push_back(kNoErrand);
        dwell_.push_back(static_cast<u16>(0u));
    }

    counts_[static_cast<u32>(Activity::AtHome)] = count;
}

// ---------------------------------------------------------------------------------------
// NOTE ON STAGGERED DEPARTURES - deliberately absent, see DECISIONS.md D-018.
//
// Everyone on a given shift currently leaves on the same tick, walks in lockstep, and the
// street is quiet again minutes later. Spreading departures across the hour is the obvious
// improvement and it was tried; it is not here because it cannot be done honestly yet. The
// citizen tests model an "hour" as 2000 ticks (about 100 simulated seconds) rather than the
// real 72,000, so a departure scheduled for minute 35 never fires inside the test window and
// every arrival invariant fails. The fix is to give the tests a real clock first, not to
// loosen the invariants around a stagger. Do that, then bring this back.

void Citizens::Tick(const SimClock& clock, const City& city) {
    // R3: the schedule is READ from the clock, never accumulated. An elapsed-time counter
    // would drift away from the calendar across a save/load or a seek, and every citizen
    // would then start their shift on a different tick in a replay - the classic silent
    // desync this rule exists to prevent.
    const CalendarTime cal  = clock.ToCalendar();
    const i32          hour = cal.hour;

    for (u32 k = 0; k < kCitizenActivityCount; ++k) counts_[k] = 0u;

    const u32 n = Count();
    for (u32 i = 0; i < n; ++i) {
        Activity  st        = static_cast<Activity>(state_[i]);
        const i32 startHour = static_cast<i32>(shiftStart_[i]);
        const i32 endHour   = static_cast<i32>(shiftEnd_[i]);

        switch (st) {
            case Activity::AtHome:
                // The upper bound earns its keep: without it a citizen who reached home after
                // their shift ended would turn round and walk straight back to work, forever.
                //
                // Work is tested FIRST. An errand never keeps a citizen from their shift; it
                // waits in errand_ and fires on the way home instead.
                if (hour >= startHour && hour < endHour) {
                    st = Activity::TravelWork;
                    BeginTravel(i, city, workIndex_[i]);
                } else if (errand_[i] != kNoErrand) {
                    // The economy promised a Shop. A Home or Work lot here is a caller bug
                    // that would otherwise surface as a sale rung up against a building
                    // with no stock and no till - fail at the departure, where it is named.
                    LC_ASSERT_MSG(city.At(errand_[i]).use == LotUse::Shop,
                                  "Citizens::Tick: errand destination is not a Shop lot");
                    st = Activity::TravelShop;
                    BeginTravel(i, city, errand_[i]);
                }
                break;

            case Activity::TravelWork:
                if (leg_[i] == kCitizenLegArrived) st = Activity::AtWork;
                break;

            case Activity::AtWork:
                if (hour >= endHour) {
                    if (errand_[i] != kNoErrand) {
                        LC_ASSERT_MSG(city.At(errand_[i]).use == LotUse::Shop,
                                      "Citizens::Tick: errand destination is not a Shop lot");
                        st = Activity::TravelShop;
                        BeginTravel(i, city, errand_[i]);
                    } else {
                        st = Activity::TravelHome;
                        BeginTravel(i, city, homeIndex_[i]);
                    }
                }
                break;

            case Activity::TravelShop:
                // Both errand states carry an errand, always: SetPendingErrand cannot clear
                // one mid-trip, so a missing one here can only be a corrupt load. I4: say so
                // here rather than hand the economy kNoErrand as the shop to ring up.
                LC_ASSERT_MSG(errand_[i] != kNoErrand,
                              "Citizens::Tick: walking to a shop with no errand recorded");
                if (leg_[i] == kCitizenLegArrived) {
                    st        = Activity::AtShop;
                    dwell_[i] = kShopDwellTicks;   // the "just arrived" tick, by definition
                }
                break;

            case Activity::AtShop:
                LC_ASSERT_MSG(errand_[i] != kNoErrand,
                              "Citizens::Tick: AtShop with no errand recorded");
                // A zero countdown here is a citizen who was never checked in - a state that
                // Tick cannot produce, so it can only be a corrupt load. I4: fail loudly; a
                // silent u16 wrap would keep them at the counter for 65,535 ticks instead.
                LC_ASSERT_MSG(dwell_[i] != 0u, "Citizens::Tick: AtShop with no dwell remaining");
                --dwell_[i];
                if (dwell_[i] == 0u) {
                    // Cleared on the way OUT, not on arrival: the economy reads the shop index
                    // on the JustArrivedAtShop tick and may keep reading it through the visit.
                    errand_[i] = kNoErrand;
                    st         = Activity::TravelHome;
                    BeginTravel(i, city, homeIndex_[i]);
                }
                break;

            case Activity::TravelHome:
                if (leg_[i] == kCitizenLegArrived) st = Activity::AtHome;
                break;

            // Activity lives in a u8 array that save/load will one day fill from a file.
            // I4: an unrecognised value is a loud failure, not a citizen who quietly stands
            // still forever. No `break` after it - LC_FATAL never returns, so a break would
            // be unreachable code (C4702, an error under UBT).
            default:
                LC_FATAL("Citizens::Tick: activity value is not one of the six enumerators");
        }

        if (st == Activity::TravelWork || st == Activity::TravelHome ||
            st == Activity::TravelShop) {
            StepTravel(i);
        }

        state_[i] = static_cast<u8>(st);
        ++counts_[static_cast<u32>(st)];
    }
}

void Citizens::BeginTravel(u32 i, const City& city, u32 destBuilding) {
    const Building& lot = city.At(destBuilding);

    // The destination is the road intersection outside the building, never the building
    // centre: citizens walk on centrelines, so a target inside a block would put the last
    // stretch of every commute through a wall.
    targetX_[i] = city.NearestRoadX(lot.centreX) * kCitizenCmPerMetre;
    targetY_[i] = city.NearestRoadY(lot.centreY) * kCitizenCmPerMetre;
    leg_[i]     = kCitizenLegX;
}

void Citizens::StepTravel(u32 i) {
    // Retire any leg that is already satisfied. This is what makes a zero-length leg free: a
    // citizen whose destination shares their road column starts the Y leg on the same tick
    // instead of standing still for one, and one already on the target arrives immediately
    // rather than two ticks later.
    if (leg_[i] == kCitizenLegX && posX_[i] == targetX_[i]) leg_[i] = kCitizenLegY;
    if (leg_[i] == kCitizenLegY && posY_[i] == targetY_[i]) leg_[i] = kCitizenLegArrived;

    const i32 step = static_cast<i32>(speed_[i]);

    if (leg_[i] == kCitizenLegX) {
        posX_[i] = CitizenStepToward(posX_[i], targetX_[i], step);
        if (posX_[i] == targetX_[i]) leg_[i] = kCitizenLegY;
        // Exactly one axis moves per tick, so no citizen ever covers more than `speed`
        // centimetres in a tick - not even on the tick they turn the corner.
        return;
    }

    if (leg_[i] == kCitizenLegY) {
        posY_[i] = CitizenStepToward(posY_[i], targetY_[i], step);
        if (posY_[i] == targetY_[i]) leg_[i] = kCitizenLegArrived;
    }
}

// ---------------------------------------------------------------------------------------
i32 Citizens::PosX(u32 i) const {
    LC_ASSERT_MSG(i < Count(), "Citizens::PosX: index out of range");
    return CitizenCmToMetres(posX_[i]);
}

i32 Citizens::PosY(u32 i) const {
    LC_ASSERT_MSG(i < Count(), "Citizens::PosY: index out of range");
    return CitizenCmToMetres(posY_[i]);
}

Activity Citizens::State(u32 i) const {
    LC_ASSERT_MSG(i < Count(), "Citizens::State: index out of range");
    return static_cast<Activity>(state_[i]);
}

u32 Citizens::HomeBuilding(u32 i) const {
    LC_ASSERT_MSG(i < Count(), "Citizens::HomeBuilding: index out of range");
    return homeIndex_[i];
}

u32 Citizens::WorkBuilding(u32 i) const {
    LC_ASSERT_MSG(i < Count(), "Citizens::WorkBuilding: index out of range");
    return workIndex_[i];
}

i32 Citizens::ShiftStartHour(u32 i) const {
    LC_ASSERT_MSG(i < Count(), "Citizens::ShiftStartHour: index out of range");
    return static_cast<i32>(shiftStart_[i]);
}

i32 Citizens::ShiftEndHour(u32 i) const {
    LC_ASSERT_MSG(i < Count(), "Citizens::ShiftEndHour: index out of range");
    return static_cast<i32>(shiftEnd_[i]);
}

// ---------------------------------------------------------------------------------------
// Store only - except during a trip, when it is a no-op (see ERRANDS at the top of the
// file: during TravelShop/AtShop errand_ is the destination the feet are committed to, and
// the shop the economy will ring up on arrival, so nothing may rewrite it). The building
// index is validated when the walk begins - City::At and the Shop-lot assert in Tick -
// because Citizens holds no city of its own. Idempotent by construction: assigning the same
// value twice is the same state. kNoErrand cancels a pending errand.
void Citizens::SetPendingErrand(u32 i, u32 shopBuilding) {
    LC_ASSERT_MSG(i < Count(), "Citizens::SetPendingErrand: index out of range");
    if (CitizenIsOnErrandTrip(static_cast<Activity>(state_[i]))) return;
    errand_[i] = shopBuilding;
}

u32 Citizens::PendingErrand(u32 i) const {
    LC_ASSERT_MSG(i < Count(), "Citizens::PendingErrand: index out of range");
    return errand_[i];
}

// "At the counter with the countdown still full" is true on the arrival tick and on no
// other: the very next AtShop tick decrements dwell_, and nothing sets it back to full
// without passing through TravelShop first.
bool Citizens::JustArrivedAtShop(u32 i) const {
    LC_ASSERT_MSG(i < Count(), "Citizens::JustArrivedAtShop: index out of range");
    return static_cast<Activity>(state_[i]) == Activity::AtShop && dwell_[i] == kShopDwellTicks;
}

u32 Citizens::CountIn(Activity a) const {
    const u32 index = static_cast<u32>(a);
    LC_ASSERT_MSG(index < kCitizenActivityCount, "Citizens::CountIn: activity out of range");
    return counts_[index];
}

// I1 / I7. Count first, so two populations of different sizes can never hash alike, then
// every per-citizen array in index order.
//
// counts_ is deliberately NOT hashed: it is recomputed from state_ on every tick, so hashing
// it would fold the same information in twice and - worse - would let a bug that
// desynchronised counts_ from state_ hide behind a hash that still matched.
void Citizens::HashInto(Hasher& h) const {
    const u32 n = Count();
    h.U32(n);
    for (u32 i = 0; i < n; ++i) h.U32(homeIndex_[i]);
    for (u32 i = 0; i < n; ++i) h.U32(workIndex_[i]);
    for (u32 i = 0; i < n; ++i) h.I32(posX_[i]);
    for (u32 i = 0; i < n; ++i) h.I32(posY_[i]);
    for (u32 i = 0; i < n; ++i) h.I32(targetX_[i]);
    for (u32 i = 0; i < n; ++i) h.I32(targetY_[i]);
    for (u32 i = 0; i < n; ++i) h.U8(state_[i]);
    for (u32 i = 0; i < n; ++i) h.U8(leg_[i]);
    for (u32 i = 0; i < n; ++i) h.U8(speed_[i]);
    for (u32 i = 0; i < n; ++i) h.U8(shiftStart_[i]);
    for (u32 i = 0; i < n; ++i) h.U8(shiftEnd_[i]);
    // Both decide the future: the errand picks the next destination, the countdown picks the
    // departure tick. Two populations that differ only here must never hash alike.
    for (u32 i = 0; i < n; ++i) h.U32(errand_[i]);
    for (u32 i = 0; i < n; ++i) h.U32(static_cast<u32>(dwell_[i]));
}

} // namespace lc
