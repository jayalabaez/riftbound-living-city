// Invariant tests for SimClock.
//
// The point of these tests is not that the getters return what was set. It is that:
//   * boundary predicates are exact at hand-computed tick indices,
//   * the calendar is a bijection over the representable tick range,
//   * advancing 100,000 times drifts by exactly zero,
//   * a simulated day contains exactly the number of boundaries it must,
//   * phase-offset gating actually spreads load instead of merely claiming to,
//   * the clock hash is a stable function of the tick and nothing else.
//
// Known-answer values below were produced by this implementation and are hard-coded on
// purpose: a refactor that changes any of them is a determinism change (I1) and must be a
// deliberate, reviewed act rather than a silent one.

#include "TestFramework.h"

#include "livingcity/core/Clock.h"

#include <cstdio>
#include <string>

// The sim core lives in namespace lc; tests read better unqualified.
using namespace lc;

namespace lc::test {

// Failure-message rendering for CalendarTime; LC_CHECK_EQ needs it.
//
// Deliberately `static` (internal linkage) rather than `inline`. LC_CHECK_EQ calls
// ::lc::test::Show by qualified name, so this only has to be visible in THIS translation
// unit — and if a second test file ever adds its own CalendarTime renderer, `inline` would
// make the two definitions an ODR violation that no compiler is required to diagnose.
// Internal linkage makes that collision impossible instead of merely unlikely.
static std::string Show(const CalendarTime& c) {
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "y%d m%02d d%02d %02d:%02d:%02d +%dt (doy %d, dow %d)",
                  c.year, c.month, c.day, c.hour, c.minute, c.second,
                  c.tickInSecond, c.dayOfYear, c.dayOfWeek);
    return std::string(buf);
}

} // namespace lc::test

namespace {

SimClock At(TickIndex t) {
    SimClock c;
    c.SetTick(t);
    return c;
}

u64 HashOf(const SimClock& c) {
    Hasher h;
    c.HashInto(h);
    return h.Value();
}

// Every boundary predicate at once, so a test can assert the whole set in one line.
struct BoundarySet {
    bool second = false;
    bool minute = false;
    bool hour   = false;
    bool day    = false;
    bool month  = false;
    bool year   = false;
};

BoundarySet BoundariesAt(TickIndex t) {
    const SimClock c = At(t);
    BoundarySet b;
    b.second = c.IsSecondBoundary();
    b.minute = c.IsMinuteBoundary();
    b.hour   = c.IsHourBoundary();
    b.day    = c.IsDayBoundary();
    b.month  = c.IsMonthBoundary();
    b.year   = c.IsYearBoundary();
    return b;
}

CalendarTime MakeCalendar(i32 year, i32 month, i32 day,
                          i32 hour, i32 minute, i32 second, i32 tickInSecond,
                          i32 dayOfYear, i32 dayOfWeek) {
    CalendarTime c;
    c.year         = year;
    c.month        = month;
    c.day          = day;
    c.hour         = hour;
    c.minute       = minute;
    c.second       = second;
    c.tickInSecond = tickInSecond;
    c.dayOfYear    = dayOfYear;
    c.dayOfWeek    = dayOfWeek;
    return c;
}

} // namespace

// ---------------------------------------------------------------------------------------
// The tick hierarchy constants themselves are a contract other systems budget against
// (D-008, R9). Pin them.
LC_TEST(clock_constants_are_the_documented_hierarchy) {
    LC_CHECK_EQ(kTicksPerSecond,    20ull);
    LC_CHECK_EQ(kTicksPerSimMinute, 1200ull);
    LC_CHECK_EQ(kTicksPerSimHour,   72000ull);
    LC_CHECK_EQ(kTicksPerSimDay,    1728000ull);
    LC_CHECK_EQ(kTicksPerSimWeek,   12096000ull);
    LC_CHECK_EQ(kTicksPerSimMonth,  51840000ull);
    LC_CHECK_EQ(kTicksPerSimYear,   622080000ull);

    LC_CHECK_EQ(kDaysPerYear, 360);
    LC_CHECK_EQ(kDaysPerMonth * kMonthsPerYear, kDaysPerYear);

    // The whole reason for the 12x30 calendar: every larger unit is an exact multiple of
    // every smaller one, so no accrual ever has a remainder that depends on month length.
    LC_CHECK_EQ(kTicksPerSimYear % kTicksPerSimMonth, 0ull);
    LC_CHECK_EQ(kTicksPerSimMonth % kTicksPerSimDay,  0ull);
    LC_CHECK_EQ(kTicksPerSimDay % kTicksPerSimHour,   0ull);
    LC_CHECK_EQ(kTicksPerSimHour % kTicksPerSimMinute, 0ull);
    LC_CHECK_EQ(kTicksPerSimMinute % kTicksPerSecond,  0ull);

    LC_CHECK_EQ(TicksPerRate(TickRate::Fine),      1ull);
    LC_CHECK_EQ(TicksPerRate(TickRate::Second),    kTicksPerSecond);
    LC_CHECK_EQ(TicksPerRate(TickRate::SimMinute), kTicksPerSimMinute);
    LC_CHECK_EQ(TicksPerRate(TickRate::Daily),     kTicksPerSimDay);
    LC_CHECK_EQ(TicksPerRate(TickRate::Monthly),   kTicksPerSimMonth);
}

// ---------------------------------------------------------------------------------------
// The documented convention: tick 0 begins every unit, so it is a boundary of every rate.
LC_TEST(clock_tick_zero_is_a_boundary_of_every_rate) {
    SimClock c;
    LC_CHECK_EQ(c.CurrentTick(), 0ull);

    LC_CHECK(c.IsSecondBoundary());
    LC_CHECK(c.IsMinuteBoundary());
    LC_CHECK(c.IsHourBoundary());
    LC_CHECK(c.IsDayBoundary());
    LC_CHECK(c.IsMonthBoundary());
    LC_CHECK(c.IsYearBoundary());

    LC_CHECK(c.ShouldRun(TickRate::Fine));
    LC_CHECK(c.ShouldRun(TickRate::Second));
    LC_CHECK(c.ShouldRun(TickRate::SimMinute));
    LC_CHECK(c.ShouldRun(TickRate::Daily));
    LC_CHECK(c.ShouldRun(TickRate::Monthly));
}

// ---------------------------------------------------------------------------------------
// Hand-computed indices for every rate, including the tick immediately before each boundary
// (which must be false) and the boundary itself (true for that rate and every coarser-than
// it divides, false for everything larger).
LC_TEST(clock_boundaries_at_hand_computed_ticks) {
    // Nothing at all between second boundaries.
    for (TickIndex t : {TickIndex{1}, TickIndex{7}, TickIndex{19}}) {
        const BoundarySet b = BoundariesAt(t);
        LC_CHECK_FALSE(b.second);
        LC_CHECK_FALSE(b.minute);
        LC_CHECK_FALSE(b.hour);
        LC_CHECK_FALSE(b.day);
        LC_CHECK_FALSE(b.month);
        LC_CHECK_FALSE(b.year);
    }

    // 20 = one sim second.
    {
        const BoundarySet b = BoundariesAt(20);
        LC_CHECK(b.second);
        LC_CHECK_FALSE(b.minute);
        LC_CHECK_FALSE(b.hour);
        LC_CHECK_FALSE(b.day);
        LC_CHECK_FALSE(b.month);
        LC_CHECK_FALSE(b.year);
    }

    // 1,199 is the last tick of sim-minute 0; 1,200 is the first tick of sim-minute 1.
    {
        const BoundarySet b = BoundariesAt(1199);
        LC_CHECK_FALSE(b.second);
        LC_CHECK_FALSE(b.minute);
    }
    {
        const BoundarySet b = BoundariesAt(1200);
        LC_CHECK(b.second);
        LC_CHECK(b.minute);
        LC_CHECK_FALSE(b.hour);
        LC_CHECK_FALSE(b.day);
        LC_CHECK_FALSE(b.month);
        LC_CHECK_FALSE(b.year);
    }

    // 72,000 = one sim hour.
    {
        const BoundarySet b = BoundariesAt(71999);
        LC_CHECK_FALSE(b.hour);
    }
    {
        const BoundarySet b = BoundariesAt(72000);
        LC_CHECK(b.second);
        LC_CHECK(b.minute);
        LC_CHECK(b.hour);
        LC_CHECK_FALSE(b.day);
        LC_CHECK_FALSE(b.month);
        LC_CHECK_FALSE(b.year);
    }

    // 1,728,000 = one sim day.
    {
        const BoundarySet b = BoundariesAt(1727999);
        LC_CHECK_FALSE(b.day);
    }
    {
        const BoundarySet b = BoundariesAt(1728000);
        LC_CHECK(b.second);
        LC_CHECK(b.minute);
        LC_CHECK(b.hour);
        LC_CHECK(b.day);
        LC_CHECK_FALSE(b.month);
        LC_CHECK_FALSE(b.year);
    }

    // Mid-month day boundary: a day, not a month.
    {
        const BoundarySet b = BoundariesAt(kTicksPerSimDay * 15u);
        LC_CHECK(b.day);
        LC_CHECK_FALSE(b.month);
        LC_CHECK_FALSE(b.year);
    }

    // 51,840,000 = one sim month (30 days exactly).
    {
        const BoundarySet b = BoundariesAt(51839999);
        LC_CHECK_FALSE(b.month);
    }
    {
        const BoundarySet b = BoundariesAt(51840000);
        LC_CHECK(b.day);
        LC_CHECK(b.month);
        LC_CHECK_FALSE(b.year);
    }

    // Mid-year month boundary: a month, not a year.
    {
        const BoundarySet b = BoundariesAt(kTicksPerSimMonth * 6u);
        LC_CHECK(b.month);
        LC_CHECK_FALSE(b.year);
    }

    // 622,080,000 = one sim year (12 months exactly).
    {
        const BoundarySet b = BoundariesAt(622079999);
        LC_CHECK_FALSE(b.year);
        LC_CHECK_FALSE(b.month);
        LC_CHECK_FALSE(b.day);
    }
    {
        const BoundarySet b = BoundariesAt(622080000);
        LC_CHECK(b.second);
        LC_CHECK(b.minute);
        LC_CHECK(b.hour);
        LC_CHECK(b.day);
        LC_CHECK(b.month);
        LC_CHECK(b.year);
    }
}

// ---------------------------------------------------------------------------------------
// Round-trip: the calendar must be a bijection, or save/load and any UI that edits time
// would quietly move the world (I7).
LC_TEST(clock_calendar_round_trips_exactly) {
    const TickIndex samples[] = {
        0ull, 1ull, 19ull, 20ull, 21ull,
        1199ull, 1200ull, 1201ull,
        71999ull, 72000ull, 72001ull,
        1727999ull, 1728000ull, 1728001ull,
        51839999ull, 51840000ull, 51840001ull,
        622079999ull, 622080000ull, 622080001ull,
        999999999ull,             // deep inside year 1
        1234567890123ull,         // ~1,984 sim years
        kMaxTick,                 // the last representable tick
    };

    for (TickIndex t : samples) {
        const CalendarTime c = SimClock::ToCalendar(t);
        LC_CHECK_EQ(SimClock::FromCalendar(c), t);
        // ...and the other direction, which catches a decomposition that loses the derived
        // fields rather than the primary ones.
        LC_CHECK_EQ(SimClock::ToCalendar(SimClock::FromCalendar(c)), c);

        // Field ranges are part of the contract other systems index arrays with.
        LC_CHECK(c.year >= 0);
        LC_CHECK(c.month >= 1 && c.month <= 12);
        LC_CHECK(c.day >= 1 && c.day <= 30);
        LC_CHECK(c.hour >= 0 && c.hour <= 23);
        LC_CHECK(c.minute >= 0 && c.minute <= 59);
        LC_CHECK(c.second >= 0 && c.second <= 59);
        LC_CHECK(c.tickInSecond >= 0 && c.tickInSecond <= 19);
        LC_CHECK(c.dayOfYear >= 1 && c.dayOfYear <= 360);
        LC_CHECK(c.dayOfWeek >= 0 && c.dayOfWeek <= 6);
        // dayOfYear is fully determined by month and day.
        LC_CHECK_EQ(c.dayOfYear, (c.month - 1) * kDaysPerMonth + c.day);
    }

    // An exhaustive stretch, not just landmarks: every tick across a sim-hour boundary.
    u64 mismatches = 0;
    for (TickIndex t = kTicksPerSimHour - 5u; t <= kTicksPerSimHour + 5u; ++t) {
        if (SimClock::FromCalendar(SimClock::ToCalendar(t)) != t) ++mismatches;
    }
    LC_CHECK_EQ(mismatches, 0ull);
}

// ---------------------------------------------------------------------------------------
// Known answers, hard-coded from this implementation's behaviour.
LC_TEST(clock_calendar_known_answers) {
    // The epoch.
    LC_CHECK_EQ(SimClock::ToCalendar(0), MakeCalendar(0, 1, 1, 0, 0, 0, 0, 1, 0));

    // 100,000 ticks = 5,000 sim seconds = 01:23:20 on the first day.
    LC_CHECK_EQ(SimClock::ToCalendar(100000), MakeCalendar(0, 1, 1, 1, 23, 20, 0, 1, 0));

    // One of each unit stacked up: year 1, month 3, day 4, 04:05:06 + 7 ticks.
    // Day-of-year = 2*30 + 3 + 1 = 64. Days since epoch = 360 + 60 + 3 = 423; 423 % 7 = 3.
    {
        const TickIndex t = kTicksPerSimYear
                          + kTicksPerSimMonth * 2u
                          + kTicksPerSimDay * 3u
                          + kTicksPerSimHour * 4u
                          + kTicksPerSimMinute * 5u
                          + kTicksPerSecond * 6u
                          + 7u;
        LC_CHECK_EQ(SimClock::ToCalendar(t), MakeCalendar(1, 3, 4, 4, 5, 6, 7, 64, 3));
    }

    // The week is 7 days and is NOT month- or year-aligned; it simply counts from the epoch.
    LC_CHECK_EQ(SimClock::ToCalendar(kTicksPerSimDay * 6u).dayOfWeek, 6);
    LC_CHECK_EQ(SimClock::ToCalendar(kTicksPerSimDay * 7u).dayOfWeek, 0);
    LC_CHECK_EQ(SimClock::ToCalendar(kTicksPerSimDay * 7u).day, 8);
    // 360 days per year is 51 weeks + 3 days, so a new year does not start on the same
    // weekday as the last one. This is deliberate; assert it so nobody "fixes" it.
    LC_CHECK_EQ(SimClock::ToCalendar(kTicksPerSimYear).dayOfWeek, 3);
    LC_CHECK_NE(SimClock::ToCalendar(kTicksPerSimYear).dayOfWeek,
                SimClock::ToCalendar(0).dayOfWeek);

    // The last tick of year 2 is the tick before year 3 begins.
    const CalendarTime endOfYear2 = MakeCalendar(2, 12, 30, 23, 59, 59, 19, 360, 0);
    LC_CHECK_EQ(SimClock::FromCalendar(endOfYear2), 1866239999ull);
    LC_CHECK_EQ(SimClock::FromCalendar(endOfYear2), kTicksPerSimYear * 3u - 1u);
    // FromCalendar ignores the derived fields, so a wrong dayOfYear cannot move the tick.
    CalendarTime lying = endOfYear2;
    lying.dayOfYear = 1;
    lying.dayOfWeek = 5;
    LC_CHECK_EQ(SimClock::FromCalendar(lying), SimClock::FromCalendar(endOfYear2));
}

// ---------------------------------------------------------------------------------------
// No drift. The whole reason boundaries are pure functions of the index rather than
// accumulated counters.
LC_TEST(clock_advancing_100k_ticks_does_not_drift) {
    constexpr u64 kSteps = 100000ull;

    SimClock c;
    u64 secondBoundaries = 0;
    u64 minuteBoundaries = 0;
    for (u64 i = 0; i < kSteps; ++i) {
        c.Advance();
        if (c.IsSecondBoundary()) ++secondBoundaries;
        if (c.IsMinuteBoundary()) ++minuteBoundaries;
    }

    LC_CHECK_EQ(c.CurrentTick(), kSteps);

    // Ticks 1..100,000 contain exactly 100000/20 second boundaries and 100000/1200
    // minute boundaries (1200 * 83 = 99,600 <= 100,000 < 100,800).
    LC_CHECK_EQ(secondBoundaries, 5000ull);
    LC_CHECK_EQ(minuteBoundaries, 83ull);

    // The instance calendar must agree with the pure static one, and both with the
    // independently hand-computed value.
    LC_CHECK_EQ(c.ToCalendar(), SimClock::ToCalendar(kSteps));
    LC_CHECK_EQ(c.ToCalendar(), MakeCalendar(0, 1, 1, 1, 23, 20, 0, 1, 0));

    // Seeking must be indistinguishable from stepping: no hidden accumulated state.
    SimClock seeked;
    seeked.SetTick(kSteps);
    LC_CHECK_EQ(seeked.CurrentTick(), c.CurrentTick());
    LC_CHECK_EQ(seeked.ToCalendar(), c.ToCalendar());
    LC_CHECK_EQ(HashOf(seeked), HashOf(c));
}

// ---------------------------------------------------------------------------------------
// Exact boundary counts over exactly one simulated day. This is the off-by-one net:
// the [0, day) range includes tick 0 (a boundary of everything), the [1, day] range does not.
LC_TEST(clock_boundary_counts_over_one_sim_day) {
    // Range A: ticks 0 .. kTicksPerSimDay-1, i.e. the first full day including the epoch.
    {
        SimClock c;
        u64 sec = 0, min = 0, hr = 0, day = 0, mon = 0, yr = 0;
        for (u64 i = 0; i < kTicksPerSimDay; ++i) {
            if (c.IsSecondBoundary()) ++sec;
            if (c.IsMinuteBoundary()) ++min;
            if (c.IsHourBoundary())   ++hr;
            if (c.IsDayBoundary())    ++day;
            if (c.IsMonthBoundary())  ++mon;
            if (c.IsYearBoundary())   ++yr;
            c.Advance();
        }
        LC_CHECK_EQ(c.CurrentTick(), kTicksPerSimDay);
        LC_CHECK_EQ(sec, 86400ull);
        LC_CHECK_EQ(min, 1440ull);
        LC_CHECK_EQ(hr,  24ull);
        LC_CHECK_EQ(day, 1ull);   // tick 0 only
        LC_CHECK_EQ(mon, 1ull);   // tick 0 only
        LC_CHECK_EQ(yr,  1ull);   // tick 0 only
    }

    // Range B: ticks 1 .. kTicksPerSimDay, the same number of ticks shifted by one. The
    // per-second/minute/hour/day counts are unchanged, but the month and year boundaries
    // that only existed at tick 0 are gone.
    {
        SimClock c;
        c.SetTick(1);
        u64 sec = 0, min = 0, hr = 0, day = 0, mon = 0, yr = 0;
        for (u64 i = 0; i < kTicksPerSimDay; ++i) {
            if (c.IsSecondBoundary()) ++sec;
            if (c.IsMinuteBoundary()) ++min;
            if (c.IsHourBoundary())   ++hr;
            if (c.IsDayBoundary())    ++day;
            if (c.IsMonthBoundary())  ++mon;
            if (c.IsYearBoundary())   ++yr;
            c.Advance();
        }
        LC_CHECK_EQ(c.CurrentTick(), kTicksPerSimDay + 1u);
        LC_CHECK_EQ(sec, 86400ull);
        LC_CHECK_EQ(min, 1440ull);
        LC_CHECK_EQ(hr,  24ull);
        LC_CHECK_EQ(day, 1ull);   // the day boundary at kTicksPerSimDay
        LC_CHECK_EQ(mon, 0ull);
        LC_CHECK_EQ(yr,  0ull);
    }
}

// ---------------------------------------------------------------------------------------
LC_TEST(clock_should_run_agrees_with_boundary_queries) {
    u64 mismatches = 0;
    u64 fineRuns   = 0;
    SimClock c;
    for (TickIndex t = 0; t < kTicksPerSimMinute * 2u; ++t) {
        c.SetTick(t);
        if (c.ShouldRun(TickRate::Fine)) ++fineRuns;
        if (c.ShouldRun(TickRate::Second)    != c.IsSecondBoundary()) ++mismatches;
        if (c.ShouldRun(TickRate::SimMinute) != c.IsMinuteBoundary()) ++mismatches;
        if (c.ShouldRun(TickRate::Daily)     != c.IsDayBoundary())    ++mismatches;
        if (c.ShouldRun(TickRate::Monthly)   != c.IsMonthBoundary())  ++mismatches;
    }
    LC_CHECK_EQ(mismatches, 0ull);
    LC_CHECK_EQ(fineRuns, kTicksPerSimMinute * 2u);  // Fine means every single tick.

    // Coarse rates, checked where a two-sim-minute window cannot reach.
    LC_CHECK(At(kTicksPerSimDay).ShouldRun(TickRate::Daily));
    LC_CHECK_FALSE(At(kTicksPerSimDay + 1u).ShouldRun(TickRate::Daily));
    LC_CHECK(At(kTicksPerSimMonth).ShouldRun(TickRate::Monthly));
    LC_CHECK_FALSE(At(kTicksPerSimMonth - 1u).ShouldRun(TickRate::Monthly));
    // A month boundary is also a day boundary; the reverse is not true.
    LC_CHECK(At(kTicksPerSimMonth).ShouldRun(TickRate::Daily));
    LC_CHECK_FALSE(At(kTicksPerSimDay).ShouldRun(TickRate::Monthly));
}

// ---------------------------------------------------------------------------------------
// Phase offsets exist to keep periodic systems off the same tick (R9). Assert the property,
// not the implementation.
LC_TEST(clock_phase_offsets_spread_load) {
    // Two sim-minute systems with different offsets: each fires exactly once per sim-minute,
    // and never on the same tick as the other.
    {
        const u32 offsetA = 0u;
        const u32 offsetB = 137u;
        u64 firesA = 0, firesB = 0, collisions = 0;
        TickIndex tickA = 0, tickB = 0;
        SimClock c;
        for (TickIndex t = 0; t < kTicksPerSimMinute; ++t) {
            c.SetTick(t);
            const bool a = c.ShouldRun(TickRate::SimMinute, offsetA);
            const bool b = c.ShouldRun(TickRate::SimMinute, offsetB);
            if (a && b) ++collisions;
            if (a) { ++firesA; tickA = t; }
            if (b) { ++firesB; tickB = t; }
        }
        LC_CHECK_EQ(collisions, 0ull);
        LC_CHECK_EQ(firesA, 1ull);
        LC_CHECK_EQ(firesB, 1ull);
        LC_CHECK_EQ(tickA, 0ull);
        LC_CHECK_EQ(tickB, 137ull);
    }

    // The strong form: 20 one-second systems, one per possible offset, over a sim-minute.
    // Exactly one fires on every single tick and each fires exactly 60 times — perfectly
    // flat load rather than a 20x spike once a second.
    {
        constexpr u32 kOffsetCount = 20u;
        u64 fires[kOffsetCount] = {};
        u64 ticksWithoutExactlyOne = 0;
        SimClock c;
        for (TickIndex t = 0; t < kTicksPerSimMinute; ++t) {
            c.SetTick(t);
            u64 firingThisTick = 0;
            for (u32 o = 0; o < kOffsetCount; ++o) {
                if (c.ShouldRun(TickRate::Second, o)) {
                    ++fires[o];
                    ++firingThisTick;
                }
            }
            if (firingThisTick != 1ull) ++ticksWithoutExactlyOne;
        }
        LC_CHECK_EQ(ticksWithoutExactlyOne, 0ull);
        for (u32 o = 0; o < kOffsetCount; ++o) {
            LC_CHECK_EQ(fires[o], 60ull);
        }
    }

    // Offsets are taken modulo the period, so an out-of-range offset wraps rather than
    // silently never firing.
    for (TickIndex t = 0; t < 60ull; ++t) {
        const SimClock c = At(t);
        LC_CHECK_EQ(c.ShouldRun(TickRate::Second, 20u), c.ShouldRun(TickRate::Second, 0u));
        LC_CHECK_EQ(c.ShouldRun(TickRate::Second, 4001u), c.ShouldRun(TickRate::Second, 1u));
    }

    // Zero offset is exactly the un-offset overload.
    u64 mismatches = 0;
    SimClock c;
    for (TickIndex t = 0; t < kTicksPerSimMinute; ++t) {
        c.SetTick(t);
        if (c.ShouldRun(TickRate::Second, 0u) != c.ShouldRun(TickRate::Second)) ++mismatches;
        // Fine has period 1: phase offsetting is a documented no-op there.
        if (!c.ShouldRun(TickRate::Fine, 7u)) ++mismatches;
    }
    LC_CHECK_EQ(mismatches, 0ull);
}

// ---------------------------------------------------------------------------------------
// I1/I7: the clock's contribution to the world hash must be a stable function of the tick.
LC_TEST(clock_hash_is_stable_and_tick_sensitive) {
    // Equal clocks hash equal, regardless of how they got there.
    SimClock stepped;
    for (int i = 0; i < 1000; ++i) stepped.Advance();
    SimClock seeked;
    seeked.SetTick(1000);
    LC_CHECK_EQ(HashOf(stepped), HashOf(seeked));

    // Different ticks hash differently across a spread of values.
    const TickIndex samples[] = {0ull, 1ull, 2ull, 20ull, 1200ull, 1728000ull,
                                 51840000ull, 622080000ull};
    constexpr std::size_t kCount = sizeof(samples) / sizeof(samples[0]);
    u64 hashes[kCount] = {};
    for (std::size_t i = 0; i < kCount; ++i) hashes[i] = HashOf(At(samples[i]));
    u64 duplicatePairs = 0;
    for (std::size_t i = 0; i < kCount; ++i) {
        for (std::size_t j = i + 1; j < kCount; ++j) {
            if (hashes[i] == hashes[j]) ++duplicatePairs;
        }
    }
    LC_CHECK_EQ(duplicatePairs, 0ull);

    // Known answers. FNV-1a 64 over the little-endian bytes of the tick index. If these
    // change, the world hash changed and every recorded replay is invalidated.
    LC_CHECK_EQ(HashOf(At(0)),      5187598658539770339ull);
    LC_CHECK_EQ(HashOf(At(1)),      2955283251572180930ull);
    LC_CHECK_EQ(HashOf(At(100000)), 1624022991790743020ull);

    // HashInto must feed the hasher, not reset it: hashing two clocks in sequence is
    // order-sensitive and differs from hashing either alone.
    Hasher h;
    At(1).HashInto(h);
    At(2).HashInto(h);
    Hasher reversed;
    At(2).HashInto(reversed);
    At(1).HashInto(reversed);
    LC_CHECK_NE(h.Value(), reversed.Value());
    LC_CHECK_NE(h.Value(), HashOf(At(2)));
}

// ---------------------------------------------------------------------------------------
// The boundary predicates and ToCalendar are two INDEPENDENT derivations of the same fact:
// one is a modulo on the tick index, the other a chain of divisions. Every other test in
// this file checks one or the other, never that they agree. A constant that drifted out of
// the hierarchy, or a decomposition that lost a carry at a rollover, can satisfy both halves
// separately while they contradict each other — and that contradiction is what would put a
// monthly accrual on a day the calendar calls the 30th.
LC_TEST(clock_boundaries_agree_with_the_calendar) {
    u64 mismatches = 0;
    SimClock c;

    const auto Check = [&](TickIndex t) {
        c.SetTick(t);
        const CalendarTime cal = c.ToCalendar();
        const bool midnight = cal.hour == 0 && cal.minute == 0 &&
                              cal.second == 0 && cal.tickInSecond == 0;
        if (c.IsSecondBoundary() != (cal.tickInSecond == 0))                        ++mismatches;
        if (c.IsMinuteBoundary() != (cal.second == 0 && cal.tickInSecond == 0))     ++mismatches;
        if (c.IsHourBoundary()   != (cal.minute == 0 && cal.second == 0 &&
                                     cal.tickInSecond == 0))                        ++mismatches;
        if (c.IsDayBoundary()    != midnight)                                       ++mismatches;
        if (c.IsMonthBoundary()  != (midnight && cal.day == 1))                     ++mismatches;
        if (c.IsYearBoundary()   != (midnight && cal.day == 1 && cal.month == 1))   ++mismatches;
    };

    // Dense: every tick of two sim-minutes, so second/minute rollovers are exhaustive.
    for (TickIndex t = 0; t < kTicksPerSimMinute * 2u; ++t) Check(t);

    // Every day boundary of two whole years, from both sides. This crosses all 24 month
    // rollovers and both year rollovers, which is where a carry gets lost.
    for (u64 d = 0; d < static_cast<u64>(kDaysPerYear) * 2u; ++d) {
        const TickIndex start = kTicksPerSimDay * d;
        if (start > 0u) Check(start - 1u);
        Check(start);
        Check(start + 1u);
    }
    LC_CHECK_EQ(mismatches, 0ull);

    // The week must keep counting straight through month and year ends: dayOfWeek advances
    // by exactly one per day and never resets. The existing known-answer test pins two
    // values; this pins the property those values are evidence of.
    u64 weekdayBreaks = 0;
    i32 previous = SimClock::ToCalendar(0).dayOfWeek;
    for (u64 d = 1; d < static_cast<u64>(kDaysPerYear) * 2u; ++d) {
        const i32 today = SimClock::ToCalendar(kTicksPerSimDay * d).dayOfWeek;
        if (today != (previous + 1) % kDaysPerWeek) ++weekdayBreaks;
        previous = today;
    }
    LC_CHECK_EQ(weekdayBreaks, 0ull);
}

// ---------------------------------------------------------------------------------------
// The capacity boundary. kMaxTick is documented as the LAST representable tick, and three
// separate guards (Advance, SetTick, ToCalendar) have to agree with that word "last".
// Nothing else here exercises them: the round-trip test calls the static ToCalendar(kMaxTick)
// without ever putting a clock there. An off-by-one in Advance's guard silently costs the
// world its final tick; one in SetTick's aborts a load of a save that was in range.
LC_TEST(clock_reaches_exactly_kMaxTick) {
    SimClock stepped;
    stepped.SetTick(kMaxTick - 1u);
    stepped.Advance();                          // must not trip the Advance guard
    LC_CHECK_EQ(stepped.CurrentTick(), kMaxTick);

    SimClock seeked;
    seeked.SetTick(kMaxTick);                   // must not trip the SetTick guard
    LC_CHECK_EQ(seeked.CurrentTick(), kMaxTick);
    LC_CHECK_EQ(seeked.ToCalendar(), stepped.ToCalendar());

    // The last tick is the last instant of the last year the i32 year field can hold, and
    // the largest legal CalendarTime maps back to exactly it. That the two meet with no gap
    // is what fixes kMaxTick's value; if they did not, some legal calendar dates would be
    // unreachable, or some reachable ticks would have no calendar.
    const CalendarTime last = MakeCalendar(2147483647, 12, 30, 23, 59, 59, 19, 360, 5);
    LC_CHECK_EQ(SimClock::ToCalendar(kMaxTick), last);
    LC_CHECK_EQ(SimClock::FromCalendar(last), kMaxTick);
    LC_CHECK_FALSE(stepped.IsSecondBoundary());   // kMaxTick ends a second, never begins one
}

// ---------------------------------------------------------------------------------------
// Rewind. Save/load and the replay harness both seek BACKWARDS, and the forward case in
// clock_advancing_100k_ticks_does_not_drift cannot catch accumulated state: going forwards,
// an accumulator and a pure function agree. Going backwards they do not, because nothing
// runs in reverse to unwind the accumulator.
LC_TEST(clock_rewind_is_indistinguishable_from_never_having_run) {
    SimClock stepped;
    for (u64 i = 0; i < 4321ull; ++i) stepped.Advance();

    SimClock rewound;
    rewound.SetTick(kTicksPerSimYear + 999983ull);   // far into the future...
    for (u64 i = 0; i < 50ull; ++i) rewound.Advance();
    rewound.SetTick(4321ull);                        // ...then back behind where it started

    LC_CHECK_EQ(rewound.CurrentTick(), stepped.CurrentTick());
    LC_CHECK_EQ(rewound.ToCalendar(), stepped.ToCalendar());
    LC_CHECK_EQ(HashOf(rewound), HashOf(stepped));
    LC_CHECK_EQ(rewound.IsSecondBoundary(), stepped.IsSecondBoundary());
    LC_CHECK(stepped.ShouldRun(TickRate::SimMinute, 721u));   // 4321 % 1200 == 721
    LC_CHECK_EQ(rewound.ShouldRun(TickRate::SimMinute, 721u),
                stepped.ShouldRun(TickRate::SimMinute, 721u));

    // All the way back to the epoch: a rewound clock must be byte-for-byte a fresh one.
    rewound.SetTick(0);
    const SimClock fresh;
    LC_CHECK_EQ(rewound.CurrentTick(), fresh.CurrentTick());
    LC_CHECK_EQ(rewound.ToCalendar(), fresh.ToCalendar());
    LC_CHECK_EQ(HashOf(rewound), HashOf(fresh));
    LC_CHECK(rewound.IsYearBoundary());
}

// ---------------------------------------------------------------------------------------
// I5: a sim tick allocates zero bytes in steady state. The clock is the innermost part of
// that loop, so it must be allocation-free even in a build that tracks allocations.
LC_TEST(clock_tick_allocates_nothing) {
    if (!AllocTrackingEnabled()) {
        // Tracking is compiled out (e.g. under UBT). Nothing to assert; the test still runs
        // the loop below so a crash or assert would still be caught.
        LC_CHECK(true);
    }

    SimClock c;
    NoAllocScope guard("SimClock steady-state tick");
    u64 sink = 0;
    for (u64 i = 0; i < 10000ull; ++i) {
        c.Advance();
        if (c.ShouldRun(TickRate::Second, 3u)) ++sink;
        if (c.ShouldRun(TickRate::SimMinute, 917u)) ++sink;
        const CalendarTime cal = c.ToCalendar();
        sink += static_cast<u64>(cal.second);
    }
    LC_CHECK_EQ(c.CurrentTick(), 10000ull);
    LC_CHECK(sink > 0ull);
    if (AllocTrackingEnabled()) {
        LC_CHECK_EQ(guard.AllocationsSinceStart(), 0ull);
    }
}
