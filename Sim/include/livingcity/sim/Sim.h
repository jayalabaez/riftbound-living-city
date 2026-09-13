// LIVING CITY — the simulation orchestrator.
//
// This is the thing that owns all simulation state and advances it one fixed tick at a
// time. Everything here is deterministic: given the same seed and the same sequence of
// commands, Step() produces bit-identical state, forever (invariant I1).
//
// Phase 0 deliberately has no game in it. The tick drives the clock, the RNG streams, the
// event bus, a deterministic placeholder entity, and the ledger — enough that determinism
// is actually testable, and nothing more. Voxels, agents and economy arrive in later phases.
#pragma once

#include "livingcity/core/Core.h"
#include "livingcity/core/Clock.h"
#include "livingcity/core/Events.h"
#include "livingcity/core/Fixed.h"
#include "livingcity/core/Money.h"
#include "livingcity/core/Rng.h"
#include "livingcity/world/City.h"
#include "livingcity/world/Terrain.h"
#include "livingcity/agents/Citizens.h"
#include "livingcity/economy/Economy.h"
#include "livingcity/sim/Commands.h"
#include "livingcity/sim/Replay.h"
#include "livingcity/sim/Snapshot.h"
#include "livingcity/sim/Telemetry.h"

#include <vector>

namespace lc {

// ---------------------------------------------------------------- Phase 0 events
// A real event type so the bus is genuinely exercised by the determinism test rather
// than sitting unused until Phase 3.
//
// `pad` is explicit and always zeroed rather than left to the compiler: queued events are
// hashed byte-for-byte into the world hash, and indeterminate padding bytes would make the
// hash vary between builds. EventBus enforces this with a static_assert.
struct TickBoundaryEvent {
    LC_EVENT_TYPE(lc::sim::TickBoundary);

    TickIndex tick;
    u32       kind;   // 0 = second, 1 = sim-minute, 2 = day
    u32       pad;
};

// ---------------------------------------------------------------- configuration
struct SimConfig {
    u64  worldSeed          = 1337;
    u32  checkpointInterval = 1000;   // ticks between world-hash checkpoints
    bool recordInputLog     = false;
    u32  placeholderRange   = 64;     // box the placeholder entity wanders inside

    // The city. Zero blocks means "no city" - the headless determinism and throughput runs
    // use that to measure the tick loop itself without a population attached.
    // 6x6 rather than 8x8, with more people in it. Density is what makes a street read as a
    // street: the same population spread over a larger grid just looks deserted.
    i32  cityBlocksX        = 6;
    i32  cityBlocksY        = 6;
    u32  citizenCount       = 1500;

    // What time the world starts. NOT midnight, on purpose: shifts begin between 06:00 and
    // 10:00, so a world that opens at 00:00 has every citizen indoors and asleep, and at
    // 20 Hz the player would wait six real hours for the first one to leave the house. The
    // city has to be alive the moment you arrive in it.
    i32  startHour          = 7;
    i32  startMinute        = 0;

    // Terrain covers this far from the origin in every direction. It is flat under the city
    // and rolls gently beyond it, and the player can dig anywhere.
    i32  terrainHalfExtentM = 320;

    // The one citizen the HUD follows in detail. Any index; 0 is as good as another. This
    // is the "follow one all day" promise from Phase 2, made visible early.
    u32  followedCitizen    = 0;
};

// ---------------------------------------------------------------- the simulation
class LC_API Simulation {
public:
    explicit Simulation(const SimConfig& config);

    // Advance exactly one fixed tick. This is the only way time moves.
    void Step();

    // ------------------------------------------------------------ state access
    TickIndex CurrentTick() const { return clock_.CurrentTick(); }
    const SimClock& Clock() const { return clock_; }
    u64  WorldSeed() const { return config_.worldSeed; }
    bool IsPaused() const { return paused_; }

    // Canonical hash of all simulation state. The backbone of I1 and I7.
    // Deliberately excludes telemetry — timings are observation, never state.
    u64 ComputeWorldHash() const;

    // ------------------------------------------------------------ boundaries
    // Engine -> sim: input only. The producer side pushes commands here.
    CommandQueue& Commands() { return commandQueue_; }

    // Sim -> engine: state only. The renderer reads published snapshots here.
    const SnapshotRing& Snapshots() const { return snapshots_; }

    Telemetry&       Telem() { return telemetry_; }
    const Telemetry& Telem() const { return telemetry_; }

    // ------------------------------------------------------------ replay (I1)
    InputLog&       Log() { return inputLog_; }
    const InputLog& Log() const { return inputLog_; }

    // Drive the sim from a recorded log instead of the live command queue.
    // Commands for each tick are injected in exactly their recorded order.
    void BeginReplay(const InputLog& log);
    bool IsReplaying() const { return replaying_; }

    // ------------------------------------------------------------ introspection
    Fixed PlaceholderX() const { return placeholderX_; }
    Fixed PlaceholderY() const { return placeholderY_; }
    Fixed PlaceholderZ() const { return placeholderZ_; }
    const Ledger& Accounts() const { return ledger_; }

    // The city layout is generated once and never changes, so the renderer reads it directly
    // rather than through a snapshot. It is const: the engine may look, never touch (R2).
    const City&     CityLayout() const { return city_; }
    const Citizens& Population() const { return citizens_; }
    const Economy&  EconomyState() const { return economy_; }
    const Terrain&  TerrainField() const { return terrain_; }

private:
    void ApplyCommand(const Command& cmd);
    void DrainCommands();
    void StepPlaceholder();
    void PublishSnapshot();

    SimConfig   config_;
    SimClock    clock_;
    RngStreams  rng_;

    // Fixed capacities, allocated once. Overflow is a loud failure, never a silent drop,
    // so these numbers are a design statement: Phase 0 must never queue 4096 events in a
    // single tick. If it ever does, that is a bug worth stopping for.
    EventBus    events_{4096u, 256u, 64u};

    Ledger      ledger_;
    Telemetry   telemetry_;

    City        city_;
    Terrain     terrain_;
    Citizens    citizens_;
    Economy     economy_;

    CommandQueue commandQueue_;
    SnapshotRing snapshots_;

    InputLog  inputLog_;
    InputLog  replaySource_;
    bool      replaying_ = false;

    // Phase 0 placeholder entity — deterministic wander, fixed-point only.
    Fixed placeholderX_;
    Fixed placeholderY_;
    Fixed placeholderZ_;

    AccountId centralBank_;

    bool paused_       = false;
    i32  pendingSteps_ = 0;

    // The world hash shown on the HUD. Recomputed once per sim-second, not per tick: a full
    // hash walks every citizen, every account and every terrain cell, and doing that twenty
    // times a second was the single largest cost in the tick. Checkpoints and the headless
    // harness call ComputeWorldHash() directly and are unaffected.
    u64  lastPublishedHash_ = 0;

    // Scratch reused every tick so the tick path allocates nothing (I5).
    std::vector<Command> tickCommands_;
};

// ---------------------------------------------------------------- threaded host
// Runs a Simulation at a fixed real-time cadence on its own thread, for the Unreal client.
//
// NOTE ON DETERMINISM: this class reads the wall clock to decide WHEN to call Step().
// That is legitimate — it schedules ticks, it never changes what a tick does. The tick
// content depends only on state and commands, so a slow frame produces the same world as
// a fast one, just later. Never let a wall-clock value cross into Step().
class LC_API SimThreadHost {
public:
    explicit SimThreadHost(const SimConfig& config);
    ~SimThreadHost();

    SimThreadHost(const SimThreadHost&) = delete;
    SimThreadHost& operator=(const SimThreadHost&) = delete;

    void Start();
    void Stop();
    bool Running() const;

    // How many sim ticks to run per real second, as a multiple of the 20 Hz base rate.
    //
    // THIS IS NOT A SIMULATION SETTING and deliberately does not go through the command
    // queue. It changes WHEN ticks happen, never what a tick does, so a world fast-forwarded
    // to noon is bit-identical to one that got there in real time. Routing it through the
    // command queue would put it in the replay log and make the recorded speed part of the
    // world, which would be wrong.
    void SetSpeedMultiplier(u32 multiplier);
    u32  SpeedMultiplier() const;

    Simulation&       Sim() { return sim_; }
    const Simulation& Sim() const { return sim_; }

    u64 TicksExecuted() const;

private:
    void ThreadMain();

    Simulation sim_;
    struct Impl;
    Impl* impl_;   // opaque so this header stays free of <thread> and <atomic>
};

} // namespace lc
