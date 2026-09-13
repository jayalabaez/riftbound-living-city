// LIVING CITY - per-system tick timing implementation.
//
// REMINDER, because it is the whole reason this file is shaped the way it is:
// nothing here may ever be read back by simulation logic. No std::chrono, no time(), no
// wall-clock of any kind in this translation unit - the caller measures, we only accumulate.
// A branch anywhere in the sim on a value produced here breaks determinism and therefore I1.
#include "livingcity/sim/Telemetry.h"

#include <algorithm>
#include <cstdio>

namespace lc {

namespace {

// Index helper with a loud failure on the Count sentinel: passing Count is a programming
// error, not a runtime condition, so it asserts rather than clamping silently (I4's spirit).
std::size_t SystemIndex(SimSystem system) {
    const std::size_t i = static_cast<std::size_t>(system);
    LC_ASSERT_MSG(i < kSimSystemCount, "SimSystem::Count is not a system");
    return i < kSimSystemCount ? i : 0;
}

// CLAUDE.md section 4, in nanoseconds. Agents 3.5 ms, Transport 2.0, Economy 1.5, Law 0.8,
// World 1.2, Events 0.5, headroom 0.5 (parked on Other) => 10.0 ms.
constexpr u64 kDefaultBudgetNs[kSimSystemCount] = {
    3'500'000ull,  // Agents    - needs, utility scoring, planning
    2'000'000ull,  // Transport - traffic
    1'500'000ull,  // Economy
      800'000ull,  // Law       - observation
    1'200'000ull,  // World     - streaming coordination + collapse queue
      500'000ull,  // Events    - journaling, bookkeeping
      500'000ull,  // Other     - the headroom line
};

constexpr const char* kSystemNames[kSimSystemCount] = {
    "agents", "transport", "economy", "law", "world", "events", "other",
};

} // namespace

const char* SimSystemName(SimSystem system) {
    const std::size_t i = static_cast<std::size_t>(system);
    return i < kSimSystemCount ? kSystemNames[i] : "count";
}

u64 DefaultBudgetNs(SimSystem system) {
    const std::size_t i = static_cast<std::size_t>(system);
    return i < kSimSystemCount ? kDefaultBudgetNs[i] : 0ull;
}

// ---------------------------------------------------------------- lifetime
Telemetry::Telemetry() {
    // The one and only allocation this class ever makes, and it happens at construction -
    // never inside a tick (I5). Everything after this point indexes into ring_.
    ring_.resize(kRingCapacity);
    for (std::size_t i = 0; i < kSimSystemCount; ++i) {
        budgetNs_[i] = kDefaultBudgetNs[i];
        overruns_[i] = 0;
    }
}

void Telemetry::Reset() {
    // Overwrite in place: the ring keeps its buffer, so Reset frees nothing and allocates
    // nothing and can safely be called between runs inside a no-allocation region.
    for (std::size_t i = 0; i < ring_.size(); ++i) ring_[i] = TickSample{};
    head_        = 0;
    sampleCount_ = 0;
    totalTicks_  = 0;
    current_     = TickSample{};
    inTick_      = false;
    lastTick_    = 0;
    for (std::size_t i = 0; i < kSimSystemCount; ++i) overruns_[i] = 0;
    // budgetNs_ deliberately untouched - see the header.
}

// ---------------------------------------------------------------- recording
void Telemetry::BeginTick(TickIndex tick) {
    LC_ASSERT_MSG(!inTick_, "Telemetry::BeginTick called twice without EndTick");
    current_      = TickSample{};
    current_.tick = tick;
    inTick_       = true;
}

void Telemetry::RecordSystem(SimSystem system, u64 nanoseconds) {
    LC_ASSERT_MSG(inTick_, "Telemetry::RecordSystem outside BeginTick/EndTick");
    const std::size_t i = SystemIndex(system);
    current_.systemNs[i] += nanoseconds;
    current_.totalNs     += nanoseconds;
}

void Telemetry::EndTick() {
    LC_ASSERT_MSG(inTick_, "Telemetry::EndTick without BeginTick");
    inTick_   = false;
    lastTick_ = current_.tick;

    ring_[head_] = current_;
    head_        = (head_ + 1) % kRingCapacity;
    if (sampleCount_ < kRingCapacity) ++sampleCount_;
    ++totalTicks_;

    // Budget accounting. This is bookkeeping only: the count is reported to a human, it does
    // not throttle, skip, or reorder anything. Saturating so a very long run cannot wrap.
    for (std::size_t i = 0; i < kSimSystemCount; ++i) {
        if (budgetNs_[i] != 0 && current_.systemNs[i] > budgetNs_[i]) {
            if (overruns_[i] != 0xFFFFFFFFu) ++overruns_[i];
        }
    }
}

// ---------------------------------------------------------------- queries
u64 Telemetry::LastTickTotalNs() const {
    if (sampleCount_ == 0) return 0;
    const std::size_t last = (head_ + kRingCapacity - 1) % kRingCapacity;
    return ring_[last].totalNs;
}

// Valid slots in the ring, hard-bounded by its capacity.
//
// EndTick maintains sampleCount_ <= kRingCapacity, so this clamp never fires today. It is
// here because the two readers below index a FIXED stack array and a fixed-size vector with
// it: if a future edit ever broke that invariant (`<=` for `<` in EndTick is the classic
// one), an unclamped count would turn a wrong percentile into a stack smash. A capacity
// bound at the point of use costs one compare per query and removes that whole failure mode.
std::size_t Telemetry::LiveSampleCount() const {
    LC_ASSERT_MSG(sampleCount_ <= kRingCapacity, "telemetry ring sample count exceeds capacity");
    return sampleCount_ < kRingCapacity ? sampleCount_ : kRingCapacity;
}

u64 Telemetry::P99TickNs() const {
    const std::size_t n = LiveSampleCount();
    if (n == 0) return 0;

    // Fixed-size stack scratch: sorting the ring must not allocate either, and a const query
    // that quietly heap-allocates would be a trap for anyone calling it from the overlay.
    u64 scratch[kRingCapacity];
    for (std::size_t i = 0; i < n; ++i) scratch[i] = ring_[i].totalNs;
    std::sort(scratch, scratch + n);

    // Nearest-rank percentile in pure integer arithmetic: rank = ceil(0.99 * n), index is
    // rank - 1. Integer only, so the answer is identical on every machine - the same
    // discipline as the sim itself, even though this number never reaches the sim.
    // 1 <= rank <= n for every n >= 1, so the index below is always in range.
    const std::size_t rank = (99u * n + 99u) / 100u;   // ceil(99n/100), >= 1 for n >= 1
    const std::size_t idx  = (rank == 0 ? 0 : rank - 1);
    return scratch[idx < n ? idx : n - 1];
}

u64 Telemetry::MeanSystemNs(SimSystem system) const {
    const std::size_t n = LiveSampleCount();
    if (n == 0) return 0;
    const std::size_t i = SystemIndex(system);
    u64 sum = 0;
    for (std::size_t s = 0; s < n; ++s) sum += ring_[s].systemNs[i];
    return sum / static_cast<u64>(n);
}

u32 Telemetry::BudgetOverrunCount(SimSystem system) const {
    return overruns_[SystemIndex(system)];
}

void Telemetry::SetBudgetNs(SimSystem system, u64 nanoseconds) {
    budgetNs_[SystemIndex(system)] = nanoseconds;
}

u64 Telemetry::BudgetNs(SimSystem system) const {
    return budgetNs_[SystemIndex(system)];
}

// ---------------------------------------------------------------- CSV
// Columns: tick, total_ns, one column per system in SimSystem order, p99_ns.
// Header and row are both driven by kSimSystemCount, so adding a system updates both.
void Telemetry::WriteCsvHeader(std::string& out) const {
    out += "tick,total_ns";
    for (std::size_t i = 0; i < kSimSystemCount; ++i) {
        out += ',';
        out += kSystemNames[i];
        out += "_ns";
    }
    out += ",p99_ns\n";
}

void Telemetry::AppendCsvRow(std::string& out) const {
    if (sampleCount_ == 0) return;   // nothing measured yet; emit no row rather than a lie

    const std::size_t   last = (head_ + kRingCapacity - 1) % kRingCapacity;
    const TickSample&   s    = ring_[last];

    // Widest possible row: 10 fields of 20 digits plus separators. 256 is comfortable.
    char buf[256];
    const int written = std::snprintf(
        buf, sizeof(buf),
        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        static_cast<unsigned long long>(s.tick),
        static_cast<unsigned long long>(s.totalNs),
        static_cast<unsigned long long>(s.systemNs[0]),
        static_cast<unsigned long long>(s.systemNs[1]),
        static_cast<unsigned long long>(s.systemNs[2]),
        static_cast<unsigned long long>(s.systemNs[3]),
        static_cast<unsigned long long>(s.systemNs[4]),
        static_cast<unsigned long long>(s.systemNs[5]),
        static_cast<unsigned long long>(s.systemNs[6]),
        static_cast<unsigned long long>(P99TickNs()));

    // 7 system columns are spelled out above; if SimSystem grows, this must too.
    static_assert(kSimSystemCount == 7, "AppendCsvRow enumerates exactly 7 system columns");

    LC_ASSERT_MSG(written > 0 && written < static_cast<int>(sizeof(buf)),
                  "telemetry CSV row overflowed its buffer");
    out.append(buf, static_cast<std::size_t>(written));
}

} // namespace lc
