// LIVING CITY — simulation clock and tick hierarchy.
//
// R1: zero Unreal types. R3: determinism is the foundation.
//
// Everything here is a PURE FUNCTION of a single u64 tick counter. There is no wall-clock
// source of any kind, no accumulated fractional seconds, and no per-rate counters that could
// drift apart from each other after a save/load or a rewind. Set the tick index and the
// entire calendar and every boundary predicate follow deterministically.
//
// D-008 — the tick hierarchy. The sim does not run everything at 20 Hz. A year at 20 Hz is
// 622,080,000 fine ticks; running markets and demography at that rate makes a simulated year
// take hours of wall time. Instead:
//
//     20 Hz            embodied agents, physics-adjacent state, observation and law events
//     1 Hz             Tier 1 needs
//     1 / sim-minute   market clearing, wages, rents, utilities, demography
//     daily / monthly  taxes, budgets, mortgages, elections
//
// See TickRate and SimClock::ShouldRun.
#pragma once

#include "livingcity/core/Core.h"

namespace lc {

// ---------------------------------------------------------------- calendar shape
//
// LIVING CITY uses a 360-day calendar: 12 months of exactly 30 days.
//
// This is a deliberate simulation decision, not an oversight. With 12x30:
//   * kTicksPerSimMonth and kTicksPerSimYear are exact multiples of kTicksPerSimDay, so
//     "is this tick a month boundary" is one modulo instead of a table lookup,
//   * monthly and yearly accruals (rent, wages, taxes, interest) divide evenly, so integer
//     money maths has no rounding residue that varies by month length,
//   * there are no leap years, no February, and no 28/29/30/31 special cases to get wrong.
// None of the gameplay in the phase plan depends on real calendar irregularity, and every
// one of those irregularities is a source of off-by-one determinism bugs.
//
// Weeks are 7 days and are NOT aligned to months or years: 360 = 51 weeks + 3 days, so the
// week drifts through the year exactly as it does in reality. Nothing in the sim may assume
// that a year or a month contains a whole number of weeks.
constexpr i32 kSecondsPerMinute = 60;
constexpr i32 kMinutesPerHour   = 60;
constexpr i32 kHoursPerDay      = 24;
constexpr i32 kDaysPerMonth     = 30;
constexpr i32 kMonthsPerYear    = 12;
constexpr i32 kDaysPerYear      = kDaysPerMonth * kMonthsPerYear;  // 360
constexpr i32 kDaysPerWeek      = 7;

// ---------------------------------------------------------------- tick constants
// The fine tick rate. R3 fixes this at 20 Hz; it is a compile-time integer constant, never
// a configurable fractional rate.
constexpr u64 kTicksPerSecond    = 20u;
constexpr u64 kTicksPerSimMinute = kTicksPerSecond    * static_cast<u64>(kSecondsPerMinute);  //          1,200
constexpr u64 kTicksPerSimHour   = kTicksPerSimMinute * static_cast<u64>(kMinutesPerHour);    //         72,000
constexpr u64 kTicksPerSimDay    = kTicksPerSimHour   * static_cast<u64>(kHoursPerDay);       //      1,728,000
constexpr u64 kTicksPerSimWeek   = kTicksPerSimDay    * static_cast<u64>(kDaysPerWeek);       //     12,096,000
constexpr u64 kTicksPerSimMonth  = kTicksPerSimDay    * static_cast<u64>(kDaysPerMonth);      //     51,840,000
constexpr u64 kTicksPerSimYear   = kTicksPerSimMonth  * static_cast<u64>(kMonthsPerYear);     //    622,080,000

// Largest tick whose calendar year still fits in the i32 CalendarTime::year field.
// ~1.34e18 ticks = 2.1 billion sim years; a run cannot reach it, but ToCalendar asserts on
// it rather than silently wrapping (I4: fail loudly, never paper over).
constexpr TickIndex kMaxTick = 2147483647ull * kTicksPerSimYear + (kTicksPerSimYear - 1u);

// ---------------------------------------------------------------- calendar decomposition
// Pure integer decomposition of a tick index. Held as i32 rather than u8 fields so callers
// can do arithmetic on them without a pile of casts; all values are small and non-negative.
//
// Field ranges:
//   year          0..2147483647   (0-based: the sim epoch is year 0, month 1, day 1, 00:00:00)
//   month         1..12
//   day           1..30           (day of month)
//   hour          0..23
//   minute        0..59
//   second        0..59
//   tickInSecond  0..19
//   dayOfYear     1..360          derived
//   dayOfWeek     0..6            derived; 0 is the weekday of the epoch day
//
// dayOfYear and dayOfWeek are DERIVED outputs. FromCalendar ignores them entirely, so a
// caller that hand-builds a CalendarTime cannot desynchronise them from year/month/day.
struct CalendarTime {
    i32 year         = 0;
    i32 month        = 1;
    i32 day          = 1;
    i32 hour         = 0;
    i32 minute       = 0;
    i32 second       = 0;
    i32 tickInSecond = 0;
    i32 dayOfYear    = 1;
    i32 dayOfWeek    = 0;

    // Compares every field, derived ones included: two CalendarTimes that disagree on
    // dayOfYear are not equal even if they agree on year/month/day, which is what makes the
    // round-trip test meaningful.
    constexpr bool operator==(const CalendarTime& o) const {
        return year == o.year && month == o.month && day == o.day && hour == o.hour &&
               minute == o.minute && second == o.second && tickInSecond == o.tickInSecond &&
               dayOfYear == o.dayOfYear && dayOfWeek == o.dayOfWeek;
    }
    constexpr bool operator!=(const CalendarTime& o) const { return !(*this == o); }
};

// CalendarTime is nine i32 fields and nothing else, so it contains no padding. Keep it that
// way. Adding a narrower field (a u8 season, a bool flag) would introduce padding bytes,
// and padding is indeterminate — any later code that hashes or serialises this struct
// byte-wise would then produce different results on two machines that agree on every single
// field, which is exactly the kind of invisible desync I1 exists to prevent.
static_assert(sizeof(CalendarTime) == 9 * sizeof(i32),
              "CalendarTime must stay padding-free; see the note above this assertion");

// ---------------------------------------------------------------- tick hierarchy
// The LOD rates from CLAUDE.md section 6 and DECISIONS.md D-008.
enum class TickRate : u8 {
    Fine      = 0,  // every tick, 20 Hz — embodied agents, law observation
    Second    = 1,  // 1 Hz          — Tier 1 needs
    SimMinute = 2,  // 1 / sim-minute — markets, wages, rents, utilities, demography
    Daily     = 3,  // 1 / sim-day    — taxes, budgets, mortgages
    Monthly   = 4,  // 1 / sim-month  — elections, long-horizon bookkeeping
};

// Period of a rate, in fine ticks. Always >= 1, so it is safe as a modulus.
//
// TickRate has a u8 underlying type so it can be stored in a config or a save, which means
// a corrupt byte or a bad cast can hand this function a value that is none of the five
// enumerators. That must NOT fall through to "every tick": a monthly system silently
// promoted to 20 Hz would blow the frame budget (R9) and change what the world does, with
// nothing anywhere to say why. I4 — fail loudly rather than paper over.
constexpr u64 TicksPerRate(TickRate rate) {
    switch (rate) {
        case TickRate::Fine:      return 1u;
        case TickRate::Second:    return kTicksPerSecond;
        case TickRate::SimMinute: return kTicksPerSimMinute;
        case TickRate::Daily:     return kTicksPerSimDay;
        case TickRate::Monthly:   return kTicksPerSimMonth;

        // `default` is required, not decorative. Without it MSVC decides the switch already
        // covers every case, calls everything below unreachable, and raises C4702 - which
        // Unreal compiles as an error. The out-of-range guard would then have to be deleted,
        // which is precisely the silent fallback this function exists to prevent.
        default: break;
    }
    // Reached only for a value outside the enumeration. In a constant expression this is a
    // compile error; at runtime it aborts. Either way it is never a silent 20 Hz.
    LC_FATAL("TicksPerRate: TickRate value is not one of the five enumerators");
    // No trailing return: LC_FATAL is [[noreturn]], so control cannot reach here and a
    // return statement would be dead code.
}

// ---------------------------------------------------------------- SimClock
//
// BOUNDARY CONVENTION — read this before using any Is*Boundary or ShouldRun.
//
// A boundary predicate answers "does this tick BEGIN a new unit of that size?", which is
// exactly `tick % period == 0`. Therefore:
//
//   * TICK 0 IS A BOUNDARY OF EVERY RATE. It begins second 0, minute 0, hour 0, day 1,
//     month 1 and year 0 simultaneously.
//   * A system gated on a boundary therefore runs once at startup, then every period.
//     If a system must not run on the very first tick, it checks the tick index itself;
//     the clock will not encode that policy for it.
//   * The tick that ENDS a period is period-1, and it is not a boundary. Over any half-open
//     range of exactly N periods starting at a boundary, a boundary fires exactly N times.
//
// This convention is the reason a full sim day [0, kTicksPerSimDay) contains exactly 86,400
// second boundaries and exactly 1 day boundary, and it is asserted in test_clock.cpp.
class LC_API SimClock {
public:
    SimClock() = default;

    TickIndex CurrentTick() const { return tick_; }

    // One fine tick. The only mutator the fixed-timestep loop calls in steady state:
    // no accumulator, no delta time, no float.
    void Advance() {
        LC_ASSERT_MSG(tick_ < kMaxTick, "sim clock exhausted the representable tick range");
        ++tick_;
    }

    // Used by save/load and by the replay harness to seek. Setting the tick fully determines
    // the clock: there is no other state to restore (I7).
    void SetTick(TickIndex t) {
        LC_ASSERT_MSG(t <= kMaxTick, "tick beyond the representable calendar range");
        tick_ = t;
    }

    // ---------------------------------------------------------- boundary queries
    // Pure functions of the tick index. Never derived from counters that accumulate, which
    // drift apart under seek/rewind and break I1.
    bool IsSecondBoundary() const { return tick_ % kTicksPerSecond    == 0u; }
    bool IsMinuteBoundary() const { return tick_ % kTicksPerSimMinute == 0u; }
    bool IsHourBoundary()   const { return tick_ % kTicksPerSimHour   == 0u; }
    bool IsDayBoundary()    const { return tick_ % kTicksPerSimDay    == 0u; }
    bool IsMonthBoundary()  const { return tick_ % kTicksPerSimMonth  == 0u; }
    bool IsYearBoundary()   const { return tick_ % kTicksPerSimYear   == 0u; }

    // ---------------------------------------------------------- LOD gating
    // Equivalent to the matching Is*Boundary query; ShouldRun(TickRate::Fine) is always true.
    bool ShouldRun(TickRate rate) const { return ShouldRun(rate, 0u); }

    // Phase-offset gating.
    //
    // INTENT: if every sim-minute system tests `tick % 1200 == 0`, all of them fire on the
    // same tick and that one tick blows the 10 ms budget (R9) while the other 1,199 sit idle.
    // A system instead picks a fixed offset within its own period and fires at
    // `tick % period == offset % period`, so the periodic work is spread evenly over the
    // period and the per-tick cost is flat.
    //
    // The offset must be a CONSTANT of the system, not something derived at runtime from
    // load or wall-clock time — otherwise which tick a system runs on stops being a function
    // of the seed and I1 dies. A stable way to pick one is a compile-time string hash of the
    // system's name, e.g.
    //     static constexpr u32 kPhase =
    //         static_cast<u32>(HashString("LaborMarket") % kTicksPerSimMinute);
    //
    // Offsets are taken modulo the period, so an offset larger than the period is legal and
    // wraps. For TickRate::Fine the period is 1 and every offset collapses to 0: phase
    // offsetting is a no-op there by construction, because a 20 Hz system runs every tick.
    //
    // STARTUP, and the one thing that catches people out: the boundary convention above says
    // tick 0 begins every unit, so a system gated at offset 0 runs on the very first tick.
    // A system with a NON-ZERO offset does not. Its first run is the first tick where
    // `tick % period == offset % period`, i.e. tick `offset % period` — which for a
    // hash-derived monthly offset can be most of a sim-month after the world starts.
    // Adding a phase offset to an existing system therefore silently removes its startup
    // pass. If a system needs both load-spreading and a run at startup, it must do the
    // startup work explicitly; the clock cannot tell the two intentions apart.
    bool ShouldRun(TickRate rate, u32 phaseOffset) const {
        const u64 period = TicksPerRate(rate);
        return tick_ % period == static_cast<u64>(phaseOffset) % period;
    }

    // ---------------------------------------------------------- calendar
    CalendarTime        ToCalendar() const;
    static CalendarTime ToCalendar(TickIndex tick);

    // Inverse of ToCalendar. dayOfYear and dayOfWeek in the input are ignored; the tick is
    // built from year/month/day/hour/minute/second/tickInSecond only. Asserts on any field
    // outside its documented range.
    static TickIndex FromCalendar(const CalendarTime& c);

    // ---------------------------------------------------------- hashing (I1, I7)
    // The tick index is the clock's entire state, so this is what the world hash needs.
    void HashInto(Hasher& h) const;

private:
    TickIndex tick_ = 0;
};

} // namespace lc
