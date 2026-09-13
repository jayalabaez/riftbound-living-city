#include "livingcity/core/Clock.h"

namespace lc {

// All calendar maths is unsigned integer division and modulo on the tick index. There is no
// fractional arithmetic, no lookup table of month lengths, and no branch on "is this a leap
// year" — the 12x30 calendar documented in Clock.h makes every one of those unnecessary,
// which is exactly why it was chosen.

CalendarTime SimClock::ToCalendar(TickIndex tick) {
    LC_ASSERT_MSG(tick <= kMaxTick, "tick beyond the representable calendar range");

    const u64 daysPerYear  = static_cast<u64>(kDaysPerYear);
    const u64 daysPerMonth = static_cast<u64>(kDaysPerMonth);
    const u64 daysPerWeek  = static_cast<u64>(kDaysPerWeek);

    const u64 tickInSecond = tick % kTicksPerSecond;
    const u64 totalSeconds = tick / kTicksPerSecond;

    const u64 second       = totalSeconds % static_cast<u64>(kSecondsPerMinute);
    const u64 totalMinutes = totalSeconds / static_cast<u64>(kSecondsPerMinute);

    const u64 minute     = totalMinutes % static_cast<u64>(kMinutesPerHour);
    const u64 totalHours = totalMinutes / static_cast<u64>(kMinutesPerHour);

    const u64 hour      = totalHours % static_cast<u64>(kHoursPerDay);
    const u64 totalDays = totalHours / static_cast<u64>(kHoursPerDay);  // days since the epoch

    const u64 year       = totalDays / daysPerYear;
    const u64 dayOfYear0 = totalDays % daysPerYear;   // 0..359
    const u64 month0     = dayOfYear0 / daysPerMonth; // 0..11
    const u64 dayOfMonth0 = dayOfYear0 % daysPerMonth; // 0..29

    CalendarTime c;
    c.year         = static_cast<i32>(year);
    c.month        = static_cast<i32>(month0) + 1;
    c.day          = static_cast<i32>(dayOfMonth0) + 1;
    c.hour         = static_cast<i32>(hour);
    c.minute       = static_cast<i32>(minute);
    c.second       = static_cast<i32>(second);
    c.tickInSecond = static_cast<i32>(tickInSecond);
    c.dayOfYear    = static_cast<i32>(dayOfYear0) + 1;
    // The week is not aligned to the month or the year (360 = 51 weeks + 3 days), so the
    // weekday is counted from the epoch day and never from the start of the year.
    c.dayOfWeek    = static_cast<i32>(totalDays % daysPerWeek);
    return c;
}

CalendarTime SimClock::ToCalendar() const {
    return ToCalendar(tick_);
}

TickIndex SimClock::FromCalendar(const CalendarTime& c) {
    // A malformed CalendarTime is a programmer error, not a runtime condition: no exceptions
    // (R3/D-005), so it fails loudly here rather than producing a plausible-looking wrong tick.
    LC_ASSERT_MSG(c.year >= 0, "CalendarTime.year must be >= 0");
    LC_ASSERT_MSG(c.month >= 1 && c.month <= kMonthsPerYear, "CalendarTime.month out of range");
    LC_ASSERT_MSG(c.day >= 1 && c.day <= kDaysPerMonth, "CalendarTime.day out of range");
    LC_ASSERT_MSG(c.hour >= 0 && c.hour < kHoursPerDay, "CalendarTime.hour out of range");
    LC_ASSERT_MSG(c.minute >= 0 && c.minute < kMinutesPerHour, "CalendarTime.minute out of range");
    LC_ASSERT_MSG(c.second >= 0 && c.second < kSecondsPerMinute, "CalendarTime.second out of range");
    LC_ASSERT_MSG(c.tickInSecond >= 0 &&
                  static_cast<u64>(c.tickInSecond) < kTicksPerSecond,
                  "CalendarTime.tickInSecond out of range");

    // dayOfYear and dayOfWeek are derived, so they are deliberately not read here.
    const u64 days = static_cast<u64>(c.year) * static_cast<u64>(kDaysPerYear) +
                     static_cast<u64>(c.month - 1) * static_cast<u64>(kDaysPerMonth) +
                     static_cast<u64>(c.day - 1);

    const u64 tick = days * kTicksPerSimDay +
                     static_cast<u64>(c.hour) * kTicksPerSimHour +
                     static_cast<u64>(c.minute) * kTicksPerSimMinute +
                     static_cast<u64>(c.second) * kTicksPerSecond +
                     static_cast<u64>(c.tickInSecond);

    LC_ASSERT_MSG(tick <= kMaxTick, "CalendarTime is beyond the representable tick range");
    return tick;
}

void SimClock::HashInto(Hasher& h) const {
    // The tick index is the whole of the clock's state; everything else the clock exposes is
    // a pure function of it. Hashing the derived calendar as well would add nothing but a
    // second way for the same state to disagree with itself.
    h.U64(tick_);
}

} // namespace lc
