// LIVING CITY - per-system tick timing for the headless profiler and the in-engine overlay.
//
// ---------------------------------------------------------------------------------------
// TIMINGS ARE OBSERVATION ONLY. THEY NEVER INFLUENCE A SIM DECISION. (R3)
//
// This is the one place in the sim where measured wall-clock is legitimate, because nothing
// here feeds back into the world. The moment any sim code branches on a value read out of
// Telemetry - "we are over budget, so skip the rest of the agents", "p99 is high, so drop an
// LOD tier" - determinism is gone: the same seed and the same input log would then produce
// different worlds on a fast machine and a slow one, and invariant I1 becomes untestable.
// LOD decisions must key off tick index, distance, and counts, never off elapsed time.
//
// Two structural consequences, both deliberate:
//
//   1. Telemetry has NO HashInto and is NOT part of world state. It must never appear in a
//      world hash or a save file. If it did, I1 and I7 would fail on any machine that runs
//      at a different speed - which is every machine.
//
//   2. Telemetry does not call std::chrono, or time(), or QueryPerformanceCounter. The
//      CALLER measures and passes nanoseconds in. That keeps this translation unit free of
//      wall-clock entirely, makes the CI purity grep simple, and makes every test in
//      test_replay.cpp exactly reproducible: the tests feed in fixed numbers.
// ---------------------------------------------------------------------------------------
//
// THREADING: none. Telemetry has no locks and no atomics, and every method - the queries
// included - touches the same ring. It belongs to the SIM THREAD. Reading it from the game
// thread while the sim is ticking is a data race, not a stale number: P99TickNs and
// MeanSystemNs walk the ring while EndTick is writing into it. SimThreadHost::Sim() hands
// out a mutable Simulation&, so this is reachable - snapshot what the overlay needs on the
// sim thread and publish it through the snapshot ring, or read it only while quiesced.
//
// R1: zero Unreal types. No allocation in BeginTick/RecordSystem/EndTick - the ring is
// sized once, at construction, and then only indexed (invariant I5).
#pragma once

#include "livingcity/core/Core.h"

#include <cstddef>
#include <string>
#include <vector>

namespace lc {

// The systems named in the CLAUDE.md section 4 tick budget, plus Other for everything that
// does not have a line of its own yet. Count is the sentinel, never a real system.
enum class SimSystem : u8 {
    Agents = 0,
    Transport,
    Economy,
    Law,
    World,
    Events,
    Other,
    Count,
};

inline constexpr std::size_t kSimSystemCount = static_cast<std::size_t>(SimSystem::Count);

// Lowercase, stable, machine-readable. Used for CSV column names and log lines, so changing
// one of these changes the profiler's output schema - treat it as a format change.
LC_API const char* SimSystemName(SimSystem system);

// CLAUDE.md section 4, "Sim tick - 10 ms design budget at 20 Hz", converted to nanoseconds.
// Other carries the 0.5 ms headroom line. These are design budgets, not deadlines the sim
// enforces at runtime - overrunning increments a counter and nothing else (see R9: a system
// that overruns gets an LOD tier, decided by a human, not more milliseconds decided by a
// branch on a timer).
LC_API u64 DefaultBudgetNs(SimSystem system);

class LC_API Telemetry {
public:
    // Ring of recent completed ticks. 256 ticks at 20 Hz is ~12.8 s of history, which is
    // long enough for a meaningful p99 and small enough (~18 KB) to keep resident always.
    static constexpr std::size_t kRingCapacity = 256;

    Telemetry();

    // --- recording. All three are allocation-free and must stay that way.
    void BeginTick(TickIndex tick);
    void RecordSystem(SimSystem system, u64 nanoseconds);   // additive within a tick
    void EndTick();

    // --- queries
    u64  LastTickTotalNs() const;                  // 0 before the first completed tick
    u64  P99TickNs() const;                        // nearest-rank p99 over the ring
    u64  MeanSystemNs(SimSystem system) const;     // integer mean over the ring
    // Ticks whose accumulated time for `system` was STRICTLY GREATER than its budget.
    // Cumulative since the last Reset, saturating at 2^32-1 rather than wrapping.
    u32  BudgetOverrunCount(SimSystem system) const;

    // A budget of 0 means UNBUDGETED: overruns for that system stop being counted entirely.
    // It does not mean "must take no time" - there is no way to express that, and silently
    // counting every tick as an overrun would be worse than saying so here.
    void SetBudgetNs(SimSystem system, u64 nanoseconds);
    u64  BudgetNs(SimSystem system) const;

    std::size_t SampleCount() const { return sampleCount_; }  // completed ticks in the ring
    u64         TotalTicksRecorded() const { return totalTicks_; }
    TickIndex   LastTick() const { return lastTick_; }

    // Clears measurements and overrun counters. KEEPS the configured budgets: those are
    // configuration, not data. Does not release the ring.
    void Reset();

    // --- CSV for the headless profiler. Appends to `out` (which may allocate) - this is a
    // reporting path, not a tick path. Header and row are generated from the same column
    // list, so they cannot drift apart.
    void WriteCsvHeader(std::string& out) const;
    // Appends one row describing the LAST COMPLETED tick, terminated by '\n'. Appends
    // nothing at all if no tick has completed yet.
    void AppendCsvRow(std::string& out) const;

private:
    // Valid slots in the ring, clamped to kRingCapacity. Every read of ring_ goes through
    // this so a corrupted count can never index past the buffer.
    std::size_t LiveSampleCount() const;

    struct TickSample {
        TickIndex tick    = 0;
        u64       totalNs = 0;
        u64       systemNs[kSimSystemCount] = {};
    };

    std::vector<TickSample> ring_;          // sized kRingCapacity at construction, never resized
    std::size_t             head_        = 0;   // next slot to write
    std::size_t             sampleCount_ = 0;   // valid slots, saturating at kRingCapacity
    u64                     totalTicks_  = 0;

    TickSample current_    = {};
    bool       inTick_     = false;
    TickIndex  lastTick_   = 0;

    u64 budgetNs_[kSimSystemCount]  = {};
    u32 overruns_[kSimSystemCount]  = {};
};

} // namespace lc
