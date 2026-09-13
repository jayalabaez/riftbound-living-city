// Simulation boundary invariants: an accepted command burst is consumed in order without
// allocating inside Step, and a replay can reproduce batches larger than the live ring.
#include "TestFramework.h"

#include "livingcity/sim/Sim.h"

using namespace lc;

namespace {

Command PauseCommand() {
    Command c;
    c.type = CommandType::SetPaused;
    c.arg0 = 1u;
    return c;
}

Command MoveCommand() {
    Command c;
    c.type = CommandType::MovePlaceholder;
    c.arg1 = Fixed::FromInt(1).Raw();
    c.arg2 = Fixed::FromInt(-1).Raw();
    return c;
}

} // namespace

LC_TEST(sim_full_live_command_batch_allocates_nothing_and_loses_no_input) {
    SimConfig cfg;
    cfg.checkpointInterval = 0;
    Simulation sim(cfg);
    const TickIndex beforeTick = sim.CurrentTick();
    LC_CHECK(sim.Commands().Push(PauseCommand()));
    for (u32 i = 1; i < CommandQueue::Capacity(); ++i) {
        LC_CHECK(sim.Commands().Push(MoveCommand()));
    }
    const AllocStats before = GetAllocStats();
    sim.Step();
    const AllocStats after = GetAllocStats();

    if (AllocTrackingEnabled()) LC_CHECK_EQ(after.allocations, before.allocations);
    LC_CHECK(sim.Commands().Empty());
    LC_CHECK(sim.IsPaused());
    LC_CHECK_EQ(sim.CurrentTick(), beforeTick);
    const i32 moves = static_cast<i32>(CommandQueue::Capacity() - 1u);
    LC_CHECK_EQ(sim.PlaceholderX().Raw(), Fixed::FromInt(moves).Raw());
    LC_CHECK_EQ(sim.PlaceholderY().Raw(), Fixed::FromInt(-moves).Raw());
}

LC_TEST(sim_large_spliced_replay_batch_is_preallocated_and_keeps_recorded_order) {
    SimConfig cfg;
    cfg.checkpointInterval = 0;
    Simulation sim(cfg);
    InputLog log;
    log.SetWorldSeed(cfg.worldSeed);
    const TickIndex tick = sim.CurrentTick();
    log.RecordCommand(tick, PauseCommand());
    constexpr u32 moves = CommandQueue::kCapacity + 31u;
    for (u32 i = 0; i < moves; ++i) log.RecordCommand(tick, MoveCommand());
    // An out-of-order future record must neither hide this tick's commands nor execute now.
    log.RecordCommand(tick + 3u, MoveCommand());
    log.RecordCommand(tick, MoveCommand());
    sim.BeginReplay(log);
    const AllocStats before = GetAllocStats();
    sim.Step();
    const AllocStats after = GetAllocStats();

    if (AllocTrackingEnabled()) LC_CHECK_EQ(after.allocations, before.allocations);
    LC_CHECK(sim.IsPaused());
    LC_CHECK_EQ(sim.CurrentTick(), tick);
    LC_CHECK_EQ(sim.PlaceholderX().Raw(), Fixed::FromInt(static_cast<i32>(moves + 1u)).Raw());
    LC_CHECK_EQ(sim.PlaceholderY().Raw(), Fixed::FromInt(-static_cast<i32>(moves + 1u)).Raw());
}
