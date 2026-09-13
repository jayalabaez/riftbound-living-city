// Invariant tests for the commuting population.
//
// These are not getter tests. What they actually pin down:
//   * I1 - the same seed produces a bit-identical population and a bit-identical history of
//     it, and a different seed does not;
//   * I4 - EVERY citizen holds a valid home lot and a valid workplace lot, of the right uses.
//     A citizen with no address is the failure the rule exists to catch;
//   * that a whole simulated day completes: everyone reaches work, everyone gets home, and
//     nobody is left stranded mid-commute when the day ends. "One citizen is broken" is
//     invisible in a screenshot and fatal to the premise;
//   * that arrival is EXACT - a citizen standing at work is on the target coordinate, not
//     near it - and that no citizen moves further in one tick than their own speed;
//   * that citizens walk on the ROAD GRID and never through a building. Every other test here
//     passes with the population walking through walls, so this one is not redundant with any
//     of them - see the comment above citizens_stay_on_the_road_grid;
//   * that the HUD counts always account for exactly the whole population;
//   * I5 - Tick() allocates nothing in steady state;
//   * that shifts are genuinely staggered, so the streets fill up over the morning instead of
//     the entire city stepping outdoors on a single tick;
//   * that ERRANDS work as the economy contract says: a pending errand fires at the next
//     departure (shift end, or from home outside working hours), never instead of going to
//     work; the visit lasts exactly kShopDwellTicks; JustArrivedAtShop is true on exactly one
//     tick of it; the trip always ends at the citizen's own kerb; a trip in progress cannot
//     be redirected or cancelled, so the shop rung up is always the one the citizen is
//     standing outside; and both errand and dwell state are covered by the world hash
//     (I1, I7).
//
// The city is generated once per test from a fixed seed, so every expectation below is a
// function of (city seed, population seed) and nothing else.
//
// No main() here: the runner supplies one.
#include "TestFramework.h"

#include "livingcity/agents/Citizens.h"
#include "livingcity/core/Clock.h"
#include "livingcity/core/Core.h"
#include "livingcity/world/City.h"

#include <string>
#include <vector>

// The sim core lives in namespace lc; tests read better unqualified.
using namespace lc;

namespace {

// A city small enough to simulate a full day quickly, large enough to have real commutes.
constexpr u64 kCitySeed       = 1337ull;
constexpr i32 kCityBlocks     = 6;
constexpr u32 kTestPopulation = 128u;

// Ticks simulated at the top of each sim-hour. The longest possible walk in a 6x6 city is
// bounded: the grid spans 6 * City::kStride = 432 m in each axis, and the slowest citizen
// covers 100 cm per tick, so an L-shaped walk of 432 m + 432 m takes at most 864 ticks plus
// the two ticks that retire the legs. 2000 leaves better than a 2x margin, and the schedule
// cannot change inside the window because a sim-hour is 72,000 ticks long.
constexpr u64 kHourWindowTicks = 2000ull;

City MakeTestCity() {
    City c;
    c.Generate(kCitySeed, kCityBlocks, kCityBlocks);
    return c;
}

u64 HashOf(const Citizens& z) {
    Hasher h;
    z.HashInto(h);
    return h.Value();
}

// Tick n times from wherever the clock currently is. Tick-then-advance, so the tick the clock
// currently reads is the first one simulated.
void RunTicks(Citizens& z, const City& city, SimClock& clock, u64 n) {
    for (u64 k = 0; k < n; ++k) {
        z.Tick(clock, city);
        clock.Advance();
    }
}

// Jump the clock to the top of the given sim-hour of day 1. Seeking is legal precisely
// because the schedule is a pure function of the tick index: nothing in Citizens accumulates
// elapsed time, so a jump cannot desynchronise anything.
void EnterHour(SimClock& clock, i32 hour) {
    clock.SetTick(static_cast<u64>(hour) * kTicksPerSimHour);
}

u32 TotalCounted(const Citizens& z) {
    return z.CountIn(Activity::AtHome)
         + z.CountIn(Activity::TravelWork)
         + z.CountIn(Activity::AtWork)
         + z.CountIn(Activity::TravelHome)
         + z.CountIn(Activity::TravelShop)
         + z.CountIn(Activity::AtShop);
}

i32 AbsI(i32 v) { return (v < 0) ? -v : v; }

// Where a citizen must be standing once they have arrived at a lot: the road intersection
// outside it. Computed here from the City alone, so it is an independent expectation rather
// than a restatement of whatever Citizens happens to do.
i32 RestX(const City& city, u32 building) { return city.NearestRoadX(city.At(building).centreX); }
i32 RestY(const City& city, u32 building) { return city.NearestRoadY(city.At(building).centreY); }

bool SameKerb(const City& city, u32 a, u32 b) {
    return RestX(city, a) == RestX(city, b) && RestY(city, a) == RestY(city, b);
}

// A shop this citizen would genuinely have to WALK to: its kerb is neither their home kerb
// nor their work kerb, so both legs of a visit are real trips rather than zero-length ones.
// Lowest such shop index, or Citizens::kNoErrand if the city has none - the caller checks.
u32 PickShopFor(const City& city, const Citizens& z, u32 i) {
    const std::vector<u32>& shops = city.OfUse(LotUse::Shop);
    for (const u32 shop : shops) {
        if (!SameKerb(city, shop, z.HomeBuilding(i)) && !SameKerb(city, shop, z.WorkBuilding(i))) {
            return shop;
        }
    }
    return Citizens::kNoErrand;
}

// The dwell length as the tests compare it: a tick count, not a u16.
constexpr u32 kDwell = static_cast<u32>(Citizens::kShopDwellTicks);

} // namespace

// ---------------------------------------------------------------------------------------
// I4: every citizen has a valid home address and an explicit workplace.
LC_TEST(citizens_everyone_has_a_home_and_a_job) {
    const City city = MakeTestCity();
    Citizens   z;
    z.Generate(20260909ull, city, kTestPopulation);

    LC_CHECK_EQ(z.Count(), kTestPopulation);
    const u32 buildings = city.Count();
    LC_CHECK(buildings > 0u);

    for (u32 i = 0; i < z.Count(); ++i) {
        const u32 home = z.HomeBuilding(i);
        const u32 work = z.WorkBuilding(i);

        LC_CHECK(home < buildings);
        LC_CHECK(work < buildings);

        // A home is a Home lot. Nothing else will do - this is the address, not a hint.
        LC_CHECK(city.At(home).use == LotUse::Home);

        // A workplace is somewhere people are employed: a Work lot or a Shop.
        const LotUse workUse = city.At(work).use;
        LC_CHECK(workUse == LotUse::Work || workUse == LotUse::Shop);

        // The shift must be a real, non-wrapping working day inside one calendar day.
        const i32 start = z.ShiftStartHour(i);
        const i32 end   = z.ShiftEndHour(i);
        LC_CHECK(start >= 6 && start <= 14);   // morning band 6-10, afternoon band 11-14
        LC_CHECK(end - start >= 8 && end - start <= 9);
        LC_CHECK(end < kHoursPerDay);
    }
}

// Everyone begins the world at home, standing at the kerb outside their own address.
LC_TEST(citizens_start_at_home_at_their_own_address) {
    const City city = MakeTestCity();
    Citizens   z;
    z.Generate(4242ull, city, kTestPopulation);

    LC_CHECK_EQ(z.CountIn(Activity::AtHome), z.Count());
    LC_CHECK_EQ(TotalCounted(z), z.Count());

    for (u32 i = 0; i < z.Count(); ++i) {
        LC_CHECK(z.State(i) == Activity::AtHome);

        // At the kerb outside their own home, NOT at the building's centre. A citizen placed
        // at the centre is standing inside the geometry - invisible to the player and buried
        // in a wall. This is the same RestX/RestY position an arrival settles on, so "at
        // home" means one thing everywhere: the first tick of the world and every evening
        // after it agree.
        LC_CHECK_EQ(z.PosX(i), RestX(city, z.HomeBuilding(i)));
        LC_CHECK_EQ(z.PosY(i), RestY(city, z.HomeBuilding(i)));
    }
}

// ---------------------------------------------------------------------------------------
// I1: same seed, bit-identical population and bit-identical history.
LC_TEST(citizens_same_seed_produces_an_identical_history) {
    const City city = MakeTestCity();

    Citizens a;
    Citizens b;
    a.Generate(777ull, city, kTestPopulation);
    b.Generate(777ull, city, kTestPopulation);

    const u64 generated = HashOf(a);
    LC_CHECK_EQ(generated, HashOf(b));

    // Regenerating over an existing population must land on the same state, not accumulate
    // onto it: Generate clears first.
    a.Generate(777ull, city, kTestPopulation);
    LC_CHECK_EQ(HashOf(a), generated);

    SimClock ca;
    SimClock cb;
    EnterHour(ca, 6);
    EnterHour(cb, 6);

    // Checkpoint the hash repeatedly rather than only at the end: a divergence is worth far
    // more when it names the window it appeared in.
    for (int checkpoint = 0; checkpoint < 5; ++checkpoint) {
        RunTicks(a, city, ca, 300ull);
        RunTicks(b, city, cb, 300ull);
        LC_CHECK_EQ(HashOf(a), HashOf(b));
    }

    // Guard against the vacuous version of this test: the world must actually have moved.
    LC_CHECK_NE(HashOf(a), generated);
    LC_CHECK_EQ(ca.CurrentTick(), cb.CurrentTick());
}

LC_TEST(citizens_different_seeds_produce_different_populations) {
    const City city = MakeTestCity();

    Citizens a;
    Citizens b;
    a.Generate(1ull, city, kTestPopulation);
    b.Generate(2ull, city, kTestPopulation);
    LC_CHECK_NE(HashOf(a), HashOf(b));

    SimClock ca;
    SimClock cb;
    EnterHour(ca, 6);
    EnterHour(cb, 6);
    RunTicks(a, city, ca, 900ull);
    RunTicks(b, city, cb, 900ull);
    LC_CHECK_NE(HashOf(a), HashOf(b));
}

// ---------------------------------------------------------------------------------------
// The whole point of the slice: a full simulated day in which every single citizen gets to
// work and back. One citizen stuck halfway across town at midnight is a broken world, not a
// rounding error, so this checks every citizen individually rather than checking aggregates.
LC_TEST(citizens_complete_a_full_day_and_all_get_home) {
    const City city = MakeTestCity();
    Citizens   z;
    z.Generate(555ull, city, kTestPopulation);

    const u32 n = z.Count();
    LC_CHECK_EQ(n, kTestPopulation);

    std::vector<u8>  reachedWork(n, static_cast<u8>(0));
    std::vector<i32> departureHour(n, -1);

    SimClock clock;
    for (i32 hour = 0; hour < kHoursPerDay; ++hour) {
        EnterHour(clock, hour);

        // The first tick of the hour is where the schedule fires, so sample departures right
        // after it - a commute between neighbouring lots can be over within a few ticks.
        z.Tick(clock, city);
        clock.Advance();
        for (u32 i = 0; i < n; ++i) {
            if (z.State(i) == Activity::TravelWork && departureHour[i] < 0) {
                departureHour[i] = hour;
            }
        }

        RunTicks(z, city, clock, kHourWindowTicks - 1ull);

        // By the end of the window every walk that this hour started has finished. Nobody may
        // still be in transit: that is the "stranded mid-commute" failure, caught the hour it
        // happens rather than at midnight.
        for (u32 i = 0; i < n; ++i) {
            const Activity a = z.State(i);
            LC_CHECK(a == Activity::AtHome || a == Activity::AtWork);
            if (a == Activity::AtWork) reachedWork[i] = static_cast<u8>(1);
        }
        LC_CHECK_EQ(TotalCounted(z), n);
    }

    for (u32 i = 0; i < n; ++i) {
        // Everyone worked today...
        LC_CHECK(reachedWork[i] != 0);
        // ...left exactly when their own shift said to...
        LC_CHECK_EQ(departureHour[i], z.ShiftStartHour(i));
        // ...and is home, at their own front door, at the end of the day.
        LC_CHECK(z.State(i) == Activity::AtHome);
        LC_CHECK_EQ(z.PosX(i), RestX(city, z.HomeBuilding(i)));
        LC_CHECK_EQ(z.PosY(i), RestY(city, z.HomeBuilding(i)));
    }

    LC_CHECK_EQ(z.CountIn(Activity::AtHome), n);
    LC_CHECK_EQ(z.CountIn(Activity::TravelWork), 0u);
    LC_CHECK_EQ(z.CountIn(Activity::AtWork), 0u);
    LC_CHECK_EQ(z.CountIn(Activity::TravelHome), 0u);
    // Nobody was given an errand, so nobody may have invented one.
    LC_CHECK_EQ(z.CountIn(Activity::TravelShop), 0u);
    LC_CHECK_EQ(z.CountIn(Activity::AtShop), 0u);
}

// The HUD reads these six numbers and nothing else, so they must account for every citizen
// on every tick - including the ticks in the middle of a walk, and including the two errand
// states. A quarter of the population is handed an errand so that all six are exercised.
LC_TEST(citizens_activity_counts_always_account_for_everyone) {
    const City city = MakeTestCity();
    Citizens   z;
    z.Generate(8080ull, city, 96u);

    const std::vector<u32>& shops = city.OfUse(LotUse::Shop);
    LC_CHECK(!shops.empty());
    for (u32 i = 0; i < z.Count() && !shops.empty(); i += 4u) {
        z.SetPendingErrand(i, shops[(i / 4u) % static_cast<u32>(shops.size())]);
    }

    SimClock clock;
    for (i32 hour = 6; hour <= 15; ++hour) {
        EnterHour(clock, hour);
        for (u64 t = 0; t < 1200ull; ++t) {
            z.Tick(clock, city);
            clock.Advance();
            LC_CHECK_EQ(TotalCounted(z), z.Count());
        }
    }

    // And the counts must agree with the per-citizen state they are derived from.
    u32 tally[static_cast<u32>(Activity::Count)] = {0u, 0u, 0u, 0u, 0u, 0u};
    for (u32 i = 0; i < z.Count(); ++i) ++tally[static_cast<u32>(z.State(i))];
    LC_CHECK_EQ(z.CountIn(Activity::AtHome),     tally[0]);
    LC_CHECK_EQ(z.CountIn(Activity::TravelWork), tally[1]);
    LC_CHECK_EQ(z.CountIn(Activity::AtWork),     tally[2]);
    LC_CHECK_EQ(z.CountIn(Activity::TravelHome), tally[3]);
    LC_CHECK_EQ(z.CountIn(Activity::TravelShop), tally[4]);
    LC_CHECK_EQ(z.CountIn(Activity::AtShop),     tally[5]);
}

// ---------------------------------------------------------------------------------------
// Movement is clamped, not approximate. A citizen who has arrived is ON the target, and one
// who is walking never covers more ground in a tick than their own speed allows.
LC_TEST(citizens_arrive_exactly_and_never_overshoot) {
    const City city = MakeTestCity();
    Citizens   z;
    z.Generate(31337ull, city, 96u);

    const u32 n = z.Count();
    std::vector<i32> prevX(n, 0);
    std::vector<i32> prevY(n, 0);
    std::vector<u8>  sawArrival(n, static_cast<u8>(0));
    for (u32 i = 0; i < n; ++i) {
        prevX[i] = z.PosX(i);
        prevY[i] = z.PosY(i);
    }

    // The morning commute, hour by hour, so every shift start is covered.
    SimClock clock;
    for (i32 hour = 6; hour <= 14; ++hour) {   // both shift bands: every start hour is covered
        EnterHour(clock, hour);
        for (u64 t = 0; t < kHourWindowTicks; ++t) {
            z.Tick(clock, city);
            clock.Advance();

            for (u32 i = 0; i < n; ++i) {
                const i32 x  = z.PosX(i);
                const i32 y  = z.PosY(i);
                const i32 dx = x - prevX[i];
                const i32 dy = y - prevY[i];

                // Top speed is 180 cm per tick; quantising centimetres to whole metres can
                // add at most one more metre to the reported step. Anything above 2 m in a
                // tick is a teleport, not a walk.
                LC_CHECK(AbsI(dx) <= 2);
                LC_CHECK(AbsI(dy) <= 2);

                // The path is an L walked one leg at a time, so a citizen never moves
                // diagonally: at most one axis changes on any tick.
                LC_CHECK(dx == 0 || dy == 0);

                if (z.State(i) == Activity::AtWork) {
                    // Arrived means EQUAL, not close.
                    LC_CHECK_EQ(x, RestX(city, z.WorkBuilding(i)));
                    LC_CHECK_EQ(y, RestY(city, z.WorkBuilding(i)));
                    sawArrival[i] = static_cast<u8>(1);
                }

                prevX[i] = x;
                prevY[i] = y;
            }
        }
    }

    for (u32 i = 0; i < n; ++i) LC_CHECK(sawArrival[i] != 0);
}

// A citizen must never wander off the map. The bound is derived from the buildings the city
// actually placed plus one block-and-road stride, which is the furthest a road centreline can
// be snapped from a building centre - so this pins CITIZEN behaviour rather than restating
// the generator's choice of origin.
LC_TEST(citizens_never_leave_the_city) {
    const City city = MakeTestCity();
    LC_CHECK(city.Count() > 0u);

    i32 loX = city.At(0u).centreX, hiX = loX;
    i32 loY = city.At(0u).centreY, hiY = loY;
    for (u32 b = 0; b < city.Count(); ++b) {
        const Building& lot = city.At(b);
        if (lot.centreX < loX) loX = lot.centreX;
        if (lot.centreX > hiX) hiX = lot.centreX;
        if (lot.centreY < loY) loY = lot.centreY;
        if (lot.centreY > hiY) hiY = lot.centreY;
    }
    loX -= City::kStride;  hiX += City::kStride;
    loY -= City::kStride;  hiY += City::kStride;

    // The city half-extent is the same order of magnitude; if it were not, one of the two
    // notions of "the city" would be wrong.
    LC_CHECK(hiX - loX <= 4 * city.ExtentX() + 4 * City::kStride);
    LC_CHECK(hiY - loY <= 4 * city.ExtentY() + 4 * City::kStride);

    Citizens z;
    z.Generate(606ull, city, 64u);

    SimClock clock;
    for (i32 hour = 6; hour <= 9; ++hour) {
        EnterHour(clock, hour);
        for (u64 t = 0; t < kHourWindowTicks; ++t) {
            z.Tick(clock, city);
            clock.Advance();
            for (u32 i = 0; i < z.Count(); ++i) {
                LC_CHECK(z.PosX(i) >= loX && z.PosX(i) <= hiX);
                LC_CHECK(z.PosY(i) >= loY && z.PosY(i) <= hiY);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------
// THE PATHING INVARIANT: a citizen is always on a road centreline in at least one axis.
//
// This is the check that was missing, and its absence is why citizens walked through
// buildings for the life of the file without a single test going red. An L-shaped walk runs
// its X leg at the walker's CURRENT Y, so if a citizen ever stands somewhere that is not a
// road row, the whole of their next X leg is at that height - the length of the city, through
// every block in between. Placing the population at building centres did exactly that.
//
// What makes it worth its own test is that NOTHING ELSE NOTICES. With citizens started inside
// their houses the day still completed, every arrival was still exact to the centimetre, the
// counts still summed, the hash was still bit-identical run to run, and all eleven of the
// other tests here still passed. The only visible symptom is on screen, where a reviewer
// reading a green test log will never look.
LC_TEST(citizens_stay_on_the_road_grid) {
    const City city = MakeTestCity();
    Citizens   z;
    z.Generate(2468ull, city, kTestPopulation);

    // Before anybody has taken a step: the starting placement is part of the invariant, not
    // an exception to it.
    for (u32 i = 0; i < z.Count(); ++i) {
        LC_CHECK(city.IsOnRoad(z.PosX(i), z.PosY(i)));
    }

    // Departure hours, mid-commute hours and the evening return, so both directions of the
    // walk and both ends of it are covered. A window need only be long enough to contain a
    // whole walk (about 900 ticks at the slowest pace); being on the road is required on
    // every tick regardless of whether the walk has finished.
    SimClock  clock;
    const i32 hours[] = {6, 8, 10, 15, 19};
    for (const i32 hour : hours) {
        EnterHour(clock, hour);
        for (u64 t = 0; t < 1200ull; ++t) {
            z.Tick(clock, city);
            clock.Advance();
            for (u32 i = 0; i < z.Count(); ++i) {
                LC_CHECK(city.IsOnRoad(z.PosX(i), z.PosY(i)));
            }
        }
    }
}

// The same invariant said the way a player would say it, and without relying on the reader
// believing that "on a road corridor" implies "clear of every footprint". This one simply
// asks every building whether the citizen is inside it.
//
// O(citizens x buildings) per tick, so it runs a small population over one commute rather
// than the whole day - it is the human-readable statement of the check above, not a
// replacement for it.
LC_TEST(citizens_never_walk_through_a_building) {
    const City city = MakeTestCity();
    Citizens   z;
    z.Generate(1357ull, city, 32u);

    const std::vector<Building>& lots = city.All();

    SimClock clock;
    EnterHour(clock, 6);
    for (u64 t = 0; t < 1200ull; ++t) {
        z.Tick(clock, city);
        clock.Advance();
        for (u32 i = 0; i < z.Count(); ++i) {
            const i32 x = z.PosX(i);
            const i32 y = z.PosY(i);
            for (const Building& lot : lots) {
                // Strictly inside the footprint. A citizen standing exactly on a wall line is
                // in a doorway; one strictly inside it is in the lounge.
                const bool inside = AbsI(x - lot.centreX) < lot.halfX &&
                                    AbsI(y - lot.centreY) < lot.halfY;
                LC_CHECK(!inside);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------
// I5: the 20 Hz path allocates nothing. Every array is sized in Generate; if Tick ever grows
// one, thousands of citizens will hit the allocator inside the 10 ms budget (R9).
LC_TEST(citizens_tick_allocates_nothing) {
    if (!AllocTrackingEnabled()) {
        // Tracking is compiled out (e.g. under UBT). The loop still runs, so an assert or a
        // crash would still be caught here.
        LC_CHECK(true);
    }

    const City city = MakeTestCity();
    Citizens   z;
    z.Generate(9001ull, city, 256u);

    SimClock clock;
    EnterHour(clock, 6);

    // Warm the walk up first: the measured window must be steady state, not the tick where
    // everyone begins travelling.
    RunTicks(z, city, clock, 200ull);

    const NoAllocScope guard("Citizens::Tick steady state");
    // Deliberately no LC_CHECK inside this loop - the failure macros build a std::string.
    for (u64 t = 0; t < 3000ull; ++t) {
        z.Tick(clock, city);
        clock.Advance();
    }
    if (AllocTrackingEnabled()) {
        LC_CHECK_EQ(guard.AllocationsSinceStart(), 0ull);
        LC_CHECK(guard.Clean());
    }
    LC_CHECK_EQ(TotalCounted(z), z.Count());
}

// ---------------------------------------------------------------------------------------
// Staggered shifts. If the whole population changed state on one tick the streets would fill
// and empty like a light switch, and one tick per sim-day would carry every commute in the
// city. This checks the stagger is real, at the exact tick it would collapse.
LC_TEST(citizens_shifts_are_staggered_across_the_morning) {
    const City city = MakeTestCity();
    Citizens   z;
    z.Generate(1234ull, city, kTestPopulation);

    const u32 n = z.Count();

    u32 startingAtSix = 0u;
    for (u32 i = 0; i < n; ++i) {
        if (z.ShiftStartHour(i) == 6) ++startingAtSix;
    }
    // With 128 citizens drawn over five possible start hours, both groups exist.
    LC_CHECK(startingAtSix > 0u);
    LC_CHECK(startingAtSix < n);

    SimClock clock;
    EnterHour(clock, 6);
    z.Tick(clock, city);
    clock.Advance();

    u32 departedOnThisTick = 0u;
    for (u32 i = 0; i < n; ++i) {
        if (z.State(i) == Activity::TravelWork) ++departedOnThisTick;
    }
    // Exactly the 06:00 shift left, and that is not everybody.
    LC_CHECK_EQ(departedOnThisTick, startingAtSix);
    LC_CHECK(departedOnThisTick < n);
    LC_CHECK(z.CountIn(Activity::AtHome) > 0u);

    // Walk the rest of the morning and confirm the population is genuinely mixed at 08:00 -
    // some citizens at work, some still at home - and that everyone is eventually employed
    // somewhere by 10:00.
    RunTicks(z, city, clock, kHourWindowTicks);
    for (i32 hour = 7; hour <= 8; ++hour) {
        EnterHour(clock, hour);
        RunTicks(z, city, clock, kHourWindowTicks);
    }

    u32 startedByEight = 0u;
    for (u32 i = 0; i < n; ++i) {
        if (z.ShiftStartHour(i) <= 8) ++startedByEight;
    }
    LC_CHECK(startedByEight > 0u);
    LC_CHECK(startedByEight < n);
    LC_CHECK_EQ(z.CountIn(Activity::AtWork), startedByEight);
    LC_CHECK_EQ(z.CountIn(Activity::AtHome), n - startedByEight);

    for (i32 hour = 9; hour <= 10; ++hour) {
        EnterHour(clock, hour);
        RunTicks(z, city, clock, kHourWindowTicks);
    }

    // By 10:00 the whole MORNING band is at work and the afternoon band is still at home -
    // the two-band schedule is what keeps the streets alive between the rush hours.
    u32 morningBand = 0u;
    for (u32 i = 0; i < n; ++i) {
        if (z.ShiftStartHour(i) <= 10) ++morningBand;
    }
    LC_CHECK(morningBand > 0u);
    LC_CHECK(morningBand < n);
    LC_CHECK_EQ(z.CountIn(Activity::AtWork), morningBand);
    LC_CHECK_EQ(z.CountIn(Activity::AtHome), n - morningBand);

    // ...and by 14:00 the afternoon band has arrived too. Not "everyone is at work": the
    // earliest morning workers (06:00 start, 8-hour shift) are already walking home at 14:00.
    // What must hold is that everyone whose shift COVERS 14:00 is at work, and nobody is
    // still on their way there.
    for (i32 hour = 11; hour <= 14; ++hour) {
        EnterHour(clock, hour);
        RunTicks(z, city, clock, kHourWindowTicks);
    }
    u32 onShiftAtFourteen = 0u;
    for (u32 i = 0; i < n; ++i) {
        if (z.ShiftStartHour(i) <= 14 && z.ShiftEndHour(i) > 14) ++onShiftAtFourteen;
    }
    LC_CHECK(onShiftAtFourteen > 0u);
    LC_CHECK_EQ(z.CountIn(Activity::AtWork), onShiftAtFourteen);
    LC_CHECK_EQ(z.CountIn(Activity::TravelWork), 0u);
}

// The HUD and the logs read these names; a mismatch is a silent labelling bug, so pin them.
LC_TEST(citizens_activity_names_are_stable) {
    LC_CHECK_EQ(std::string(ActivityName(Activity::AtHome)),     std::string("AtHome"));
    LC_CHECK_EQ(std::string(ActivityName(Activity::TravelWork)), std::string("TravelWork"));
    LC_CHECK_EQ(std::string(ActivityName(Activity::AtWork)),     std::string("AtWork"));
    LC_CHECK_EQ(std::string(ActivityName(Activity::TravelHome)), std::string("TravelHome"));
    LC_CHECK_EQ(std::string(ActivityName(Activity::TravelShop)), std::string("TravelShop"));
    LC_CHECK_EQ(std::string(ActivityName(Activity::AtShop)),     std::string("AtShop"));
}

// ---------------------------------------------------------------------------------------
// ERRANDS.
//
// The economy decides who shops and where; Citizens executes the walk. These tests pin the
// execution contract from Citizens.h, one clause each, from the outside: only public state
// is read, and every position expectation is computed from the City independently.

// Clause 1: an errand pending at shift end is a detour, not a cancellation of going home.
// AtWork -> TravelShop -> AtShop for exactly kShopDwellTicks -> TravelHome -> AtHome, in that
// order and no other, ending on the citizen's OWN kerb with nothing left pending.
LC_TEST(citizens_errand_at_shift_end_detours_via_the_shop_then_home) {
    const City city = MakeTestCity();
    Citizens   z;
    z.Generate(7001ull, city, 64u);

    const u32 i    = 0u;
    const u32 shop = PickShopFor(city, z, i);
    LC_CHECK(shop != Citizens::kNoErrand);
    if (shop == Citizens::kNoErrand) return;
    LC_CHECK(city.At(shop).use == LotUse::Shop);

    // A normal morning first: nothing pending, so nothing about the commute changes.
    SimClock clock;
    EnterHour(clock, z.ShiftStartHour(i));
    RunTicks(z, city, clock, kHourWindowTicks);
    LC_CHECK(z.State(i) == Activity::AtWork);

    z.SetPendingErrand(i, shop);
    LC_CHECK(z.HasPendingErrand(i));
    LC_CHECK_EQ(z.PendingErrand(i), shop);

    EnterHour(clock, z.ShiftEndHour(i));

    u32      ticksIn[static_cast<u32>(Activity::Count)] = {0u, 0u, 0u, 0u, 0u, 0u};
    u32      arrivals = 0u;
    bool     orderOk  = true;
    Activity prev     = Activity::AtWork;
    for (u64 t = 0; t < kHourWindowTicks; ++t) {
        z.Tick(clock, city);
        clock.Advance();

        const Activity a = z.State(i);
        ++ticksIn[static_cast<u32>(a)];

        // The only legal moves, and each of them only forward.
        const bool legal = (a == prev)
                        || (prev == Activity::AtWork     && a == Activity::TravelShop)
                        || (prev == Activity::TravelShop && a == Activity::AtShop)
                        || (prev == Activity::AtShop     && a == Activity::TravelHome)
                        || (prev == Activity::TravelHome && a == Activity::AtHome);
        if (!legal) orderOk = false;
        prev = a;

        if (a == Activity::AtShop) {
            // "At the counter" is the shop's kerb, exactly, for the whole visit - and the shop
            // index stays readable for the whole visit, because the economy needs it.
            LC_CHECK_EQ(z.PosX(i), RestX(city, shop));
            LC_CHECK_EQ(z.PosY(i), RestY(city, shop));
            LC_CHECK_EQ(z.PendingErrand(i), shop);
        }
        if (z.JustArrivedAtShop(i)) ++arrivals;

        // Nobody else was given an errand, so nobody else may be running one.
        LC_CHECK(z.CountIn(Activity::TravelShop) + z.CountIn(Activity::AtShop) <= 1u);
    }

    LC_CHECK(orderOk);
    LC_CHECK(ticksIn[static_cast<u32>(Activity::TravelShop)] > 0u);
    LC_CHECK_EQ(ticksIn[static_cast<u32>(Activity::AtShop)], kDwell);
    LC_CHECK(ticksIn[static_cast<u32>(Activity::TravelHome)] > 0u);
    LC_CHECK(ticksIn[static_cast<u32>(Activity::AtHome)] > 0u);
    LC_CHECK_EQ(ticksIn[static_cast<u32>(Activity::TravelWork)], 0u);
    LC_CHECK_EQ(arrivals, 1u);

    LC_CHECK(z.State(i) == Activity::AtHome);
    LC_CHECK_EQ(z.PosX(i), RestX(city, z.HomeBuilding(i)));
    LC_CHECK_EQ(z.PosY(i), RestY(city, z.HomeBuilding(i)));
    LC_CHECK_FALSE(z.HasPendingErrand(i));
    LC_CHECK_EQ(z.PendingErrand(i), Citizens::kNoErrand);
}

// Clause 2: JustArrivedAtShop is the economy's "ring up the sale" signal, so it must fire
// once per visit - not zero times, not once per dwell tick. Counted across two visits by
// the same citizen (one from work, one from home) and across the whole population, which
// also pins that it is false for everyone who is NOT arriving anywhere.
LC_TEST(citizens_just_arrived_at_shop_is_true_on_exactly_one_tick_per_visit) {
    const City city = MakeTestCity();
    Citizens   z;
    z.Generate(7002ull, city, 48u);

    const u32 i    = 5u;
    const u32 shop = PickShopFor(city, z, i);
    LC_CHECK(shop != Citizens::kNoErrand);
    if (shop == Citizens::kNoErrand) return;

    SimClock clock;
    EnterHour(clock, z.ShiftStartHour(i));
    RunTicks(z, city, clock, kHourWindowTicks);
    LC_CHECK(z.State(i) == Activity::AtWork);

    // Visit one: from work, at shift end. Visit two: from home, later the same evening.
    const i32 visitHours[2] = {z.ShiftEndHour(i), 22};
    for (const i32 hour : visitHours) {
        z.SetPendingErrand(i, shop);
        EnterHour(clock, hour);

        u32      arrivalsForI   = 0u;
        u32      arrivalsForAll = 0u;
        u32      atShopTicks    = 0u;
        bool     firstAtShopTickWasTheArrival = false;
        Activity prev = z.State(i);
        for (u64 t = 0; t < kHourWindowTicks; ++t) {
            z.Tick(clock, city);
            clock.Advance();

            const Activity a = z.State(i);
            if (a == Activity::AtShop) ++atShopTicks;
            if (z.JustArrivedAtShop(i)) {
                ++arrivalsForI;
                // The arrival tick is the FIRST AtShop tick: the one right after TravelShop.
                if (prev == Activity::TravelShop && atShopTicks == 1u) {
                    firstAtShopTickWasTheArrival = true;
                }
                LC_CHECK_EQ(z.PosX(i), RestX(city, shop));
                LC_CHECK_EQ(z.PosY(i), RestY(city, shop));
            }
            for (u32 j = 0; j < z.Count(); ++j) {
                if (z.JustArrivedAtShop(j)) ++arrivalsForAll;
            }
            prev = a;
        }

        LC_CHECK_EQ(arrivalsForI, 1u);
        LC_CHECK_EQ(arrivalsForAll, 1u);
        LC_CHECK(firstAtShopTickWasTheArrival);
        LC_CHECK_EQ(atShopTicks, kDwell);
        LC_CHECK(z.State(i) == Activity::AtHome);
        LC_CHECK_FALSE(z.HasPendingErrand(i));
        LC_CHECK_FALSE(z.JustArrivedAtShop(i));
    }
}

// Clause 3: work always wins. An errand handed out before the shift starts must not divert
// the morning commute - and must not be LOST either: it stays pending through the whole
// shift and fires on the way home.
LC_TEST(citizens_going_to_work_beats_a_pending_errand) {
    const City city = MakeTestCity();
    Citizens   z;
    z.Generate(7003ull, city, 64u);

    const u32 i    = 2u;
    const u32 shop = PickShopFor(city, z, i);
    LC_CHECK(shop != Citizens::kNoErrand);
    if (shop == Citizens::kNoErrand) return;

    // Set while AtHome, before the shift, so that the very first thing the schedule sees is a
    // citizen with BOTH a shift to go to and an errand to run.
    z.SetPendingErrand(i, shop);

    SimClock clock;
    EnterHour(clock, z.ShiftStartHour(i));
    z.Tick(clock, city);
    clock.Advance();
    LC_CHECK(z.State(i) == Activity::TravelWork);
    LC_CHECK(z.HasPendingErrand(i));   // deferred, not dropped

    RunTicks(z, city, clock, kHourWindowTicks - 1ull);
    LC_CHECK(z.State(i) == Activity::AtWork);
    LC_CHECK_EQ(z.PosX(i), RestX(city, z.WorkBuilding(i)));
    LC_CHECK_EQ(z.PosY(i), RestY(city, z.WorkBuilding(i)));
    LC_CHECK_EQ(z.PendingErrand(i), shop);

    // Through every remaining hour of the shift they stay at their desk with it still pending.
    for (i32 hour = z.ShiftStartHour(i) + 1; hour < z.ShiftEndHour(i); ++hour) {
        EnterHour(clock, hour);
        RunTicks(z, city, clock, 200ull);
        LC_CHECK(z.State(i) == Activity::AtWork);
        LC_CHECK_EQ(z.PendingErrand(i), shop);
    }

    // And at shift end it is the errand, not home, that they leave for.
    EnterHour(clock, z.ShiftEndHour(i));
    z.Tick(clock, city);
    clock.Advance();
    LC_CHECK(z.State(i) == Activity::TravelShop);
    LC_CHECK_EQ(z.PendingErrand(i), shop);
}

// Clause 4: from home outside working hours an errand fires immediately, goes to the shop,
// and comes straight back - without anyone going to work, on either side of the shift.
LC_TEST(citizens_errand_from_home_outside_working_hours_goes_to_the_shop_and_back) {
    const City city = MakeTestCity();
    Citizens   z;
    z.Generate(7004ull, city, 64u);

    const u32 i    = 7u;
    const u32 shop = PickShopFor(city, z, i);
    LC_CHECK(shop != Citizens::kNoErrand);
    if (shop == Citizens::kNoErrand) return;

    const u32 n = z.Count();

    // Two hours in which NOBODY in the city works, walks to work, or walks home from it. With
    // the two-band schedule (morning starts 06-10, afternoon starts 11-14, shifts of 8-9 h)
    // someone is at work or in transit during every hour from 06:00 to 23:00, and the
    // afternoon band is still walking home after 23:00 - so the only city-wide quiet hours
    // are 00:00 to 05:00. Both samples are taken there. Before the afternoon band existed the
    // second sample was 21:00; it is not quiet any more.
    const i32 offHours[2] = {2, 5};
    SimClock  clock;
    for (const i32 hour : offHours) {
        LC_CHECK(z.State(i) == Activity::AtHome);
        z.SetPendingErrand(i, shop);
        EnterHour(clock, hour);

        // Fires on the first tick: nobody else so much as moves.
        z.Tick(clock, city);
        clock.Advance();
        LC_CHECK(z.State(i) == Activity::TravelShop);
        LC_CHECK_EQ(z.CountIn(Activity::AtHome), n - 1u);
        LC_CHECK_EQ(z.CountIn(Activity::TravelShop), 1u);

        // The first tick of the window was simulated above and found them in TravelShop.
        u32      ticksIn[static_cast<u32>(Activity::Count)] = {0u, 0u, 0u, 0u, 0u, 0u};
        ticksIn[static_cast<u32>(Activity::TravelShop)] = 1u;
        bool     orderOk = true;
        Activity prev    = Activity::TravelShop;
        for (u64 t = 1; t < kHourWindowTicks; ++t) {
            z.Tick(clock, city);
            clock.Advance();

            const Activity a = z.State(i);
            ++ticksIn[static_cast<u32>(a)];
            const bool legal = (a == prev)
                            || (prev == Activity::TravelShop && a == Activity::AtShop)
                            || (prev == Activity::AtShop     && a == Activity::TravelHome)
                            || (prev == Activity::TravelHome && a == Activity::AtHome);
            if (!legal) orderOk = false;
            prev = a;

            // Off hours for the whole city: nobody works, and the counts still cover everyone.
            LC_CHECK_EQ(z.CountIn(Activity::TravelWork), 0u);
            LC_CHECK_EQ(z.CountIn(Activity::AtWork), 0u);
            LC_CHECK_EQ(TotalCounted(z), n);
        }

        LC_CHECK(orderOk);
        LC_CHECK(ticksIn[static_cast<u32>(Activity::TravelShop)] > 0u);
        LC_CHECK_EQ(ticksIn[static_cast<u32>(Activity::AtShop)], kDwell);
        LC_CHECK(ticksIn[static_cast<u32>(Activity::TravelHome)] > 0u);
        LC_CHECK(z.State(i) == Activity::AtHome);
        LC_CHECK_EQ(z.PosX(i), RestX(city, z.HomeBuilding(i)));
        LC_CHECK_EQ(z.PosY(i), RestY(city, z.HomeBuilding(i)));
        LC_CHECK_FALSE(z.HasPendingErrand(i));
        LC_CHECK_EQ(z.CountIn(Activity::AtHome), n);
    }
}

// Clause 5: a trip in progress is immutable. The economy re-decides errands once a sim-minute
// from the citizen's CURRENT position, and a walk to a shop can straddle that boundary, so a
// citizen halfway to shop A will be handed shop B - or nothing, if they stopped being hungry.
// SetPendingErrand must not redirect the walk (it never did), and it must not change what
// PendingErrand reports either: on the JustArrivedAtShop tick the economy rings up
// PendingErrand, so if that could be B while the citizen stands outside A, B's stock and
// B's account take a sale that happened at A. Whichever shop the feet went to is the one
// the money goes to, and a cancel mid-visit is the same no-op.
//
// Then, once home, the citizen is ordinary again: a new errand fires as normal, and one that
// is cancelled while genuinely pending (before departure) really is cancelled.
LC_TEST(citizens_errand_in_progress_cannot_be_redirected_or_cancelled) {
    const City city = MakeTestCity();
    Citizens   z;
    z.Generate(7007ull, city, 64u);

    const u32 i     = 4u;
    const u32 shopA = PickShopFor(city, z, i);
    LC_CHECK(shopA != Citizens::kNoErrand);
    if (shopA == Citizens::kNoErrand) return;

    // A second shop whose kerb is not A's, so "they arrived at A, not B" is a real position
    // check and not a coincidence of two shops sharing an intersection.
    u32 shopB = Citizens::kNoErrand;
    for (const u32 shop : city.OfUse(LotUse::Shop)) {
        if (shop != shopA && !SameKerb(city, shop, shopA)) { shopB = shop; break; }
    }
    LC_CHECK(shopB != Citizens::kNoErrand);
    if (shopB == Citizens::kNoErrand) return;

    // ---- pending, before departure: cancel really cancels --------------------------------
    SimClock clock;
    EnterHour(clock, z.ShiftStartHour(i));
    RunTicks(z, city, clock, kHourWindowTicks);
    LC_CHECK(z.State(i) == Activity::AtWork);

    z.SetPendingErrand(i, shopA);
    z.SetPendingErrand(i, Citizens::kNoErrand);
    LC_CHECK_FALSE(z.HasPendingErrand(i));

    EnterHour(clock, z.ShiftEndHour(i));
    z.Tick(clock, city);
    clock.Advance();
    LC_CHECK(z.State(i) == Activity::TravelHome);   // straight home: nothing was pending
    RunTicks(z, city, clock, kHourWindowTicks - 1ull);
    LC_CHECK(z.State(i) == Activity::AtHome);

    // ---- in progress: neither a new shop nor a cancel changes anything -------------------
    EnterHour(clock, 21);
    z.SetPendingErrand(i, shopA);
    z.Tick(clock, city);
    clock.Advance();
    LC_CHECK(z.State(i) == Activity::TravelShop);
    LC_CHECK_EQ(z.PendingErrand(i), shopA);

    // Re-issued to B on the very first tick of the walk, and to kNoErrand on the next.
    z.SetPendingErrand(i, shopB);
    LC_CHECK_EQ(z.PendingErrand(i), shopA);
    z.Tick(clock, city);
    clock.Advance();
    z.SetPendingErrand(i, Citizens::kNoErrand);
    LC_CHECK_EQ(z.PendingErrand(i), shopA);
    LC_CHECK(z.HasPendingErrand(i));

    u32  arrivals    = 0u;
    u32  atShopTicks = 0u;
    bool errandOk    = true;
    for (u64 t = 0; t < kHourWindowTicks; ++t) {
        z.Tick(clock, city);
        clock.Advance();

        const Activity a = z.State(i);
        if (a == Activity::TravelShop || a == Activity::AtShop) {
            // Every tick of the trip, and both kinds of interference, every few ticks.
            if (z.PendingErrand(i) != shopA) errandOk = false;
            if ((t & 7ull) == 3ull) z.SetPendingErrand(i, shopB);
            if ((t & 7ull) == 5ull) z.SetPendingErrand(i, Citizens::kNoErrand);
            if (z.PendingErrand(i) != shopA) errandOk = false;
        }
        if (a == Activity::AtShop) ++atShopTicks;
        if (z.JustArrivedAtShop(i)) {
            ++arrivals;
            // The sale is rung up HERE, from PendingErrand, at the kerb of that same shop.
            LC_CHECK_EQ(z.PendingErrand(i), shopA);
            LC_CHECK_EQ(z.PosX(i), RestX(city, shopA));
            LC_CHECK_EQ(z.PosY(i), RestY(city, shopA));
        }
    }
    LC_CHECK(errandOk);
    LC_CHECK_EQ(arrivals, 1u);
    LC_CHECK_EQ(atShopTicks, kDwell);
    LC_CHECK(z.State(i) == Activity::AtHome);
    LC_CHECK_EQ(z.PosX(i), RestX(city, z.HomeBuilding(i)));
    LC_CHECK_EQ(z.PosY(i), RestY(city, z.HomeBuilding(i)));

    // The visit consumed the errand; the interference during it did not queue a second one.
    LC_CHECK_FALSE(z.HasPendingErrand(i));
    RunTicks(z, city, clock, 20ull);
    LC_CHECK(z.State(i) == Activity::AtHome);

    // ---- and afterwards a new errand is accepted like any other -------------------------
    z.SetPendingErrand(i, shopB);
    z.Tick(clock, city);
    clock.Advance();
    LC_CHECK(z.State(i) == Activity::TravelShop);
    LC_CHECK_EQ(z.PendingErrand(i), shopB);
}

// I1 / I7: errand_ and dwell_ both decide the future, so both must be in the hash. Two
// populations that are identical in every other respect and differ in one pending errand
// must hash differently; two snapshots of the same visit one dwell tick apart must too.
LC_TEST(citizens_errand_and_dwell_state_are_hashed) {
    const City city = MakeTestCity();

    const std::vector<u32>& shops = city.OfUse(LotUse::Shop);
    LC_CHECK(shops.size() >= 2u);
    if (shops.size() < 2u) return;

    // ---- errand_ -------------------------------------------------------------------------
    Citizens a;
    Citizens b;
    a.Generate(7005ull, city, 16u);
    b.Generate(7005ull, city, 16u);
    LC_CHECK_EQ(HashOf(a), HashOf(b));

    a.SetPendingErrand(3u, shops[0]);
    LC_CHECK_NE(HashOf(a), HashOf(b));

    b.SetPendingErrand(3u, shops[0]);
    LC_CHECK_EQ(HashOf(a), HashOf(b));

    // Idempotent: setting the same errand again is the same state.
    a.SetPendingErrand(3u, shops[0]);
    LC_CHECK_EQ(HashOf(a), HashOf(b));

    // WHICH shop is state too, not merely whether there is one.
    a.SetPendingErrand(3u, shops[1]);
    LC_CHECK_NE(HashOf(a), HashOf(b));
    b.SetPendingErrand(3u, shops[1]);
    LC_CHECK_EQ(HashOf(a), HashOf(b));

    // kNoErrand cancels, and a cancelled errand is a different state from a pending one.
    a.SetPendingErrand(3u, Citizens::kNoErrand);
    LC_CHECK_FALSE(a.HasPendingErrand(3u));
    LC_CHECK_NE(HashOf(a), HashOf(b));
    b.SetPendingErrand(3u, Citizens::kNoErrand);
    LC_CHECK_EQ(HashOf(a), HashOf(b));

    // ---- dwell_ --------------------------------------------------------------------------
    // A population of ONE, so that between two consecutive ticks at the counter nothing else
    // in the whole object can change: not a position, not a state, not another citizen.
    Citizens solo;
    solo.Generate(7006ull, city, 1u);
    const u32 shop = PickShopFor(city, solo, 0u);
    LC_CHECK(shop != Citizens::kNoErrand);
    if (shop == Citizens::kNoErrand) return;

    SimClock clock;
    EnterHour(clock, 21);
    solo.SetPendingErrand(0u, shop);

    u64 walked = 0;
    while (solo.State(0u) != Activity::AtShop && walked < kHourWindowTicks) {
        solo.Tick(clock, city);
        clock.Advance();
        ++walked;
    }
    LC_CHECK(solo.State(0u) == Activity::AtShop);
    LC_CHECK(solo.JustArrivedAtShop(0u));

    const u64 atArrival = HashOf(solo);
    const i32 x0        = solo.PosX(0u);
    const i32 y0        = solo.PosY(0u);

    solo.Tick(clock, city);
    clock.Advance();

    // Same place, same state, same errand. The only state that moved is the countdown.
    LC_CHECK(solo.State(0u) == Activity::AtShop);
    LC_CHECK_FALSE(solo.JustArrivedAtShop(0u));
    LC_CHECK_EQ(solo.PosX(0u), x0);
    LC_CHECK_EQ(solo.PosY(0u), y0);
    LC_CHECK_EQ(solo.PendingErrand(0u), shop);
    LC_CHECK_NE(HashOf(solo), atArrival);
}

// I5 again, with the errand paths live: the whole population runs two errands each - one
// from a cold start, one handed out mid-window through the same call the economy will use -
// and neither Tick nor SetPendingErrand may touch the allocator.
LC_TEST(citizens_tick_allocates_nothing_with_errands_active) {
    if (!AllocTrackingEnabled()) {
        // Tracking is compiled out (e.g. under UBT). The loop still runs, so an assert or a
        // crash would still be caught here.
        LC_CHECK(true);
    }

    const City city = MakeTestCity();
    const std::vector<u32>& shops = city.OfUse(LotUse::Shop);
    LC_CHECK(!shops.empty());
    if (shops.empty()) return;
    const u32 shopCount = static_cast<u32>(shops.size());

    Citizens z;
    z.Generate(9002ull, city, 256u);
    for (u32 i = 0; i < z.Count(); ++i) z.SetPendingErrand(i, shops[i % shopCount]);

    // Outside every shift, so the entire population leaves for the shops at once. 02:00, not
    // 21:00: with the afternoon band, shifts run as late as 23:00.
    SimClock clock;
    EnterHour(clock, 2);
    RunTicks(z, city, clock, 200ull);
    LC_CHECK(z.CountIn(Activity::TravelShop) + z.CountIn(Activity::AtShop) > 0u);

    const NoAllocScope guard("Citizens::Tick with errands active");
    // Deliberately no LC_CHECK inside this region - the failure macros build a std::string.
    // 3000 ticks covers the longest possible walk, the dwell, and the walk home.
    for (u64 t = 0; t < 3000ull; ++t) {
        z.Tick(clock, city);
        clock.Advance();
    }
    for (u32 i = 0; i < z.Count(); ++i) z.SetPendingErrand(i, shops[(i + 1u) % shopCount]);
    for (u64 t = 0; t < 3000ull; ++t) {
        z.Tick(clock, city);
        clock.Advance();
    }
    if (AllocTrackingEnabled()) {
        LC_CHECK_EQ(guard.AllocationsSinceStart(), 0ull);
        LC_CHECK(guard.Clean());
    }

    // The window genuinely contained errands: the only thing that clears one is finishing the
    // visit, and everybody's second one is gone with everybody back at home.
    LC_CHECK_EQ(TotalCounted(z), z.Count());
    LC_CHECK_EQ(z.CountIn(Activity::AtHome), z.Count());
    for (u32 i = 0; i < z.Count(); ++i) LC_CHECK_FALSE(z.HasPendingErrand(i));
}
