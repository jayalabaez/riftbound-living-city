#include "livingcity/sim/Sim.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace lc {

// ============================================================================ Simulation

Simulation::Simulation(const SimConfig& config)
    : config_(config) {
    rng_.Reseed(config_.worldSeed);

    placeholderX_ = Fixed::Zero();
    placeholderY_ = Fixed::Zero();
    placeholderZ_ = Fixed::Zero();

    // The government is the central bank: the only account that can create or destroy
    // money. Every other movement, anywhere in the sim, is a transfer (I3).
    centralBank_ = ledger_.OpenAccount();
    ledger_.SetCentralBank(centralBank_);

    // The world. Everything below is a pure function of the seed, so two runs with the same
    // seed produce the same streets, the same ground, the same people and the same money.
    if (config_.cityBlocksX > 0 && config_.cityBlocksY > 0) {
        city_.Generate(config_.worldSeed, config_.cityBlocksX, config_.cityBlocksY);

        // Flat under the whole city plus its ring road; rolling beyond.
        const i32 flatHalf = (city_.ExtentX() > city_.ExtentY() ? city_.ExtentX() : city_.ExtentY())
                             + City::kRoadWidth;
        const i32 halfExtent = config_.terrainHalfExtentM > flatHalf + 32
                                   ? config_.terrainHalfExtentM : flatHalf + 32;
        terrain_.Generate(config_.worldSeed, halfExtent, flatHalf);

        if (config_.citizenCount > 0) {
            citizens_.Generate(config_.worldSeed, city_, config_.citizenCount);
            economy_.Generate(config_.worldSeed, city_, citizens_, ledger_, EconomyParams{});
        }
    }

    LC_ASSERT_MSG(ledger_.ConservationHolds(), "money conservation violated at construction");

    // Open the world mid-morning so the streets already have people in them.
    clock_.SetTick(static_cast<TickIndex>(config_.startHour) * kTicksPerSimHour +
                   static_cast<TickIndex>(config_.startMinute) * kTicksPerSimMinute);

    inputLog_.SetWorldSeed(config_.worldSeed);

    // Reserve once so the tick path never allocates (I5).
    tickCommands_.reserve(CommandQueue::Capacity());

    // Budgets from CLAUDE.md section 4, in nanoseconds.
    telemetry_.SetBudgetNs(SimSystem::Agents,    3500000ull);
    telemetry_.SetBudgetNs(SimSystem::Transport, 2000000ull);
    telemetry_.SetBudgetNs(SimSystem::Economy,   1500000ull);
    telemetry_.SetBudgetNs(SimSystem::Law,        800000ull);
    telemetry_.SetBudgetNs(SimSystem::World,     1200000ull);
    telemetry_.SetBudgetNs(SimSystem::Events,     500000ull);

    PublishSnapshot();
}

void Simulation::BeginReplay(const InputLog& log) {
    replaySource_ = log;
    replaying_    = true;

    // A tool-edited replay can contain more commands at one tick than the live ring holds.
    // Setup may allocate; replay Step must not. The whole log is a safe upper bound even
    // when records were spliced out of tick order.
    tickCommands_.reserve(replaySource_.Commands().size());

    // A replay that starts from a different seed is not a replay.
    LC_ASSERT_MSG(replaySource_.WorldSeed() == config_.worldSeed,
                  "replay log seed does not match simulation seed");
}

void Simulation::ApplyCommand(const Command& cmd) {
    switch (cmd.type) {
        case CommandType::SetPaused:
            paused_ = (cmd.arg0 != 0u);
            break;

        case CommandType::StepOnce:
            pendingSteps_ += 1;
            break;

        case CommandType::MovePlaceholder:
            placeholderX_ = placeholderX_ + Fixed::FromRaw(cmd.arg1);
            placeholderY_ = placeholderY_ + Fixed::FromRaw(cmd.arg2);
            break;

        case CommandType::ModifyTerrain:
            // The player digs through the same queue as every other input, so a dig is in
            // the replay log and a replayed world has the same holes in it.
            terrain_.Modify(static_cast<i32>(cmd.arg1), static_cast<i32>(cmd.arg2),
                            UnpackTerrainRadiusCm(cmd.arg0), UnpackTerrainDeltaCm(cmd.arg0));
            break;

        case CommandType::SetTimeScale:
        case CommandType::Interact:
        case CommandType::Quit:
        case CommandType::None:
        default:
            // Phase 0: accepted and recorded, but with no simulation effect yet.
            break;
    }
}

void Simulation::DrainCommands() {
    tickCommands_.clear();

    if (replaying_) {
        // Replay: commands come from the log, in exactly their recorded order.
        replaySource_.CommandsForTick(clock_.CurrentTick(), tickCommands_);
    } else {
        // Snapshot the pending batch before popping. A producer can refill freed slots
        // while we consume; chasing those writes could grow the buffer or starve the tick.
        // Later arrivals stay ordered in the ring and are consumed on the next tick.
        const u32 pending = commandQueue_.Count();
        Command cmd;
        for (u32 i = 0; i < pending; ++i) {
            const bool popped = commandQueue_.Pop(cmd);
            LC_ASSERT_MSG(popped, "command batch changed under its sole consumer");
            tickCommands_.push_back(cmd);
        }
    }

    for (const Command& cmd : tickCommands_) {
        if (config_.recordInputLog && !replaying_) {
            inputLog_.RecordCommand(clock_.CurrentTick(), cmd);
        }
        ApplyCommand(cmd);
    }
}

void Simulation::StepPlaceholder() {
    // Deterministic wander in fixed point. Exercises the RNG stream and Fixed maths so the
    // determinism test has real state to diverge on, not just a counter.
    Rng& r = rng_.Stream(StreamId::World);

    const Fixed step  = Fixed::FromRatio(1, 4);
    const Fixed limit = Fixed::FromInt(static_cast<i32>(config_.placeholderRange));

    placeholderX_ = placeholderX_ + step * Fixed::FromInt(r.RangeI(-1, 2));
    placeholderY_ = placeholderY_ + step * Fixed::FromInt(r.RangeI(-1, 2));
    placeholderZ_ = placeholderZ_ + step * Fixed::FromInt(r.RangeI(-1, 2));

    placeholderX_ = Fixed::Clamp(placeholderX_, -limit, limit);
    placeholderY_ = Fixed::Clamp(placeholderY_, -limit, limit);
    placeholderZ_ = Fixed::Clamp(placeholderZ_, -limit, limit);
}

void Simulation::Step() {
    const TickIndex tick = clock_.CurrentTick();
    telemetry_.BeginTick(tick);

    DrainCommands();

    // Paused still drains commands (so unpausing works) but does not advance the world.
    const bool advancing = !paused_ || pendingSteps_ > 0;
    if (!advancing) {
        telemetry_.EndTick();
        PublishSnapshot();
        return;
    }
    if (paused_ && pendingSteps_ > 0) pendingSteps_ -= 1;

    // ---- world -------------------------------------------------------------
    StepPlaceholder();

    // ---- citizens ----------------------------------------------------------
    if (citizens_.Count() > 0u) {
        citizens_.Tick(clock_, city_);
    }

    // ---- economy -----------------------------------------------------------
    // After the citizens have moved, so a purchase happens on the tick someone arrives.
    if (economy_.CitizenCount() > 0u) {
        economy_.Tick(clock_, city_, citizens_, ledger_);
    }

    // ---- periodic work, on the D-008 tick hierarchy -------------------------
    if (clock_.IsSecondBoundary()) {
        TickBoundaryEvent ev{tick, 0u, 0u};
        events_.Publish(ev);
    }
    if (clock_.IsMinuteBoundary()) {
        TickBoundaryEvent ev{tick, 1u, 0u};
        events_.Publish(ev);
    }
    if (clock_.IsDayBoundary()) {
        TickBoundaryEvent ev{tick, 2u, 0u};
        events_.Publish(ev);
    }

    // ---- events ------------------------------------------------------------
    events_.Drain();

    // ---- advance time ------------------------------------------------------
    clock_.Advance();

    // ---- checkpoint --------------------------------------------------------
    if (config_.checkpointInterval > 0 &&
        (clock_.CurrentTick() % config_.checkpointInterval) == 0) {
        LC_ASSERT_MSG(ledger_.ConservationHolds(), "money conservation violated (I3)");
        if (config_.recordInputLog) {
            inputLog_.RecordCheckpoint(clock_.CurrentTick(), ComputeWorldHash());
        }
    }

    telemetry_.EndTick();
    PublishSnapshot();
}

u64 Simulation::ComputeWorldHash() const {
    Hasher h;
    h.Str("LIVINGCITY/PHASE0");
    h.U64(config_.worldSeed);
    clock_.HashInto(h);
    rng_.HashInto(h);
    placeholderX_.HashInto(h);
    placeholderY_.HashInto(h);
    placeholderZ_.HashInto(h);
    ledger_.HashInto(h);
    city_.HashInto(h);
    terrain_.HashInto(h);
    citizens_.HashInto(h);
    economy_.HashInto(h);
    events_.HashInto(h);
    h.Bool(paused_);
    h.I32(pendingSteps_);
    // Telemetry is deliberately absent: timings are observation, never state (R3).
    return h.Value();
}

void Simulation::PublishSnapshot() {
    Snapshot& s = snapshots_.BeginWrite();
    s.tick         = clock_.CurrentTick();

    // Once per sim-second is plenty for a readout. Hashing the whole world every tick was
    // costing more than the whole rest of the tick once the terrain arrived.
    if (clock_.IsSecondBoundary() || lastPublishedHash_ == 0u) {
        lastPublishedHash_ = ComputeWorldHash();
    }
    s.worldHash    = lastPublishedHash_;
    s.placeholderX = placeholderX_.Raw();
    s.placeholderY = placeholderY_.Raw();
    s.placeholderZ = placeholderZ_.Raw();
    s.agentCount   = citizens_.Count();
    s.paused       = paused_ ? 1u : 0u;

    const CalendarTime cal = clock_.ToCalendar();
    s.year   = cal.year;
    s.day    = cal.dayOfYear;
    s.hour   = cal.hour;
    s.minute = cal.minute;

    s.atHome     = citizens_.CountIn(Activity::AtHome);
    s.travelWork = citizens_.CountIn(Activity::TravelWork);
    s.atWork     = citizens_.CountIn(Activity::AtWork);
    s.travelHome = citizens_.CountIn(Activity::TravelHome);
    s.travelShop = citizens_.CountIn(Activity::TravelShop);
    s.atShop     = citizens_.CountIn(Activity::AtShop);

    s.terrainVersion    = terrain_.Version();
    s.totalMoneyMinor   = ledger_.SumOfBalances().Raw();
    s.governmentMinor   = ledger_.HasCentralBank() ? ledger_.BalanceOf(ledger_.CentralBank()).Raw() : 0;
    s.avgShopPriceMinor = economy_.AverageShopPrice().Raw();
    s.hungryCount       = economy_.HungryCount();
    s.starvationEvents  = economy_.StarvationEvents();
    s.totalShopStock    = economy_.TotalShopStock();

    // The followed citizen: one real person, in full, so the player can watch a day happen
    // to somebody rather than to a statistic.
    if (citizens_.Count() > 0u && economy_.CitizenCount() > 0u) {
        const u32 f = config_.followedCitizen < citizens_.Count() ? config_.followedCitizen : 0u;
        s.followedIndex      = f;
        s.followedX          = citizens_.PosX(f);
        s.followedY          = citizens_.PosY(f);
        s.followedActivity   = static_cast<u8>(citizens_.State(f));
        s.followedMoneyMinor = economy_.CitizenBalance(f, ledger_).Raw();
        s.followedHunger     = economy_.Hunger(f);
        s.followedFatigue    = economy_.Fatigue(f);
    }

    // Only what the view can draw crosses the boundary. If the population ever exceeds the
    // snapshot's capacity the surplus is simply not rendered - it is still simulated, still
    // earning and spending, just not on screen. That is the Tier 1/Tier 2 split arriving
    // early, and it is deliberate: the sim is never limited by what the renderer can show.
    const u32 visible = citizens_.Count() < kMaxSnapshotCitizens ? citizens_.Count()
                                                                : kMaxSnapshotCitizens;
    s.citizenCount = visible;
    for (u32 i = 0; i < visible; ++i) {
        s.citizenX[i]     = citizens_.PosX(i);
        s.citizenY[i]     = citizens_.PosY(i);
        s.citizenState[i] = static_cast<u8>(citizens_.State(i));
    }

    snapshots_.Publish();
}

// ============================================================================ SimThreadHost

struct SimThreadHost::Impl {
    std::thread            thread;
    std::atomic<bool>      running{false};
    std::atomic<bool>      stopRequested{false};
    std::atomic<u64>       ticksExecuted{0};
    std::atomic<u32>       speed{1};
};

SimThreadHost::SimThreadHost(const SimConfig& config)
    : sim_(config), impl_(new Impl()) {}

SimThreadHost::~SimThreadHost() {
    Stop();
    delete impl_;
}

bool SimThreadHost::Running() const { return impl_->running.load(std::memory_order_acquire); }

void SimThreadHost::SetSpeedMultiplier(u32 multiplier) {
    if (multiplier < 1u)   multiplier = 1u;
    if (multiplier > 512u) multiplier = 512u;
    impl_->speed.store(multiplier, std::memory_order_relaxed);
}

u32 SimThreadHost::SpeedMultiplier() const {
    return impl_->speed.load(std::memory_order_relaxed);
}
u64  SimThreadHost::TicksExecuted() const { return impl_->ticksExecuted.load(std::memory_order_relaxed); }

void SimThreadHost::Start() {
    if (impl_->running.load(std::memory_order_acquire)) return;
    impl_->stopRequested.store(false, std::memory_order_release);
    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread([this] { ThreadMain(); });
}

void SimThreadHost::Stop() {
    if (!impl_->running.load(std::memory_order_acquire)) return;
    impl_->stopRequested.store(true, std::memory_order_release);
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->running.store(false, std::memory_order_release);
}

void SimThreadHost::ThreadMain() {
    using Clock    = std::chrono::steady_clock;
    using Duration = std::chrono::nanoseconds;

    // 20 Hz fixed timestep. The wall clock decides WHEN to tick; it never influences
    // WHAT a tick does, which is what keeps determinism intact (see header comment).
    // The speed multiplier shortens the interval between ticks - it never changes the
    // timestep itself, so a fast-forwarded world is bit-identical to a real-time one.
    const long long basePeriodNs = 50000000ll;   // 50 ms

    Clock::time_point next = Clock::now();

    while (!impl_->stopRequested.load(std::memory_order_acquire)) {
        const u32      speed      = impl_->speed.load(std::memory_order_relaxed);
        const Duration tickPeriod(basePeriodNs / static_cast<long long>(speed));

        const Clock::time_point now = Clock::now();

        if (now < next) {
            // Only yield when there is real time to give back. At high multipliers the
            // period is shorter than a sleep can resolve, so spinning briefly is correct.
            if (next - now > std::chrono::milliseconds(2)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                std::this_thread::yield();
            }
            continue;
        }

        sim_.Step();
        impl_->ticksExecuted.fetch_add(1, std::memory_order_relaxed);

        next += tickPeriod;

        // If we have fallen more than half a second behind (a breakpoint, a stall),
        // drop the backlog rather than spiral-of-death catching up.
        if (Clock::now() - next > std::chrono::milliseconds(500)) {
            next = Clock::now();
        }
    }
}

} // namespace lc
