#include "LivingCitySubsystem.h"
#include "LivingCityPhase0.h"

#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogLivingCity, Log, All);

namespace
{
    /** The sim runs at 20 Hz, so consecutive snapshots are 50 ms apart. */
    constexpr float kSimTickSeconds = 0.05f;

    /** Forwards sim-core logging into UE_LOG. Installed on the game thread at startup. */
    void LivingCityLogSink(lc::LogLevel Level, const char* Message)
    {
        switch (Level)
        {
        case lc::LogLevel::Error: UE_LOG(LogLivingCity, Error,   TEXT("%hs"), Message); break;
        case lc::LogLevel::Warn:  UE_LOG(LogLivingCity, Warning, TEXT("%hs"), Message); break;
        default:                  UE_LOG(LogLivingCity, Log,     TEXT("%hs"), Message); break;
        }
    }
}

bool ULivingCitySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    if (!Super::ShouldCreateSubsystem(Outer))
    {
        return false;
    }

    // Only real game worlds run a simulation. Editor preview and inactive worlds do not,
    // or every asset thumbnail would spin up a sim thread.
    if (const UWorld* World = Cast<UWorld>(Outer))
    {
        return World->IsGameWorld();
    }
    return false;
}

void ULivingCitySubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
    Super::OnWorldBeginPlay(InWorld);

    // World subsystems also exist in Voyager and Survival. Wait until the actual
    // GameMode is assigned; only Living City owns a city simulation thread.
    // UE invokes this before GameMode::StartPlay, so BuildScene can read the city.
    if (!InWorld.GetAuthGameMode<ALivingCityGameMode>() || SimHost.IsValid())
    {
        return;
    }

    lc::SetLogSink(&LivingCityLogSink);

    lc::SimConfig Config;
    Config.worldSeed          = 1337;
    Config.checkpointInterval = 1000;
    Config.recordInputLog     = false;   // recording is a headless/CI concern

    // Open the world during the morning commute so the streets are busy on arrival.
    Config.startHour          = 7;
    Config.startMinute        = 30;

    SimHost = MakeUnique<lc::SimThreadHost>(Config);
    SimHost->Start();

    UE_LOG(LogLivingCity, Log,
           TEXT("LIVING CITY simulation started on its own thread (seed %llu, 20 Hz)."),
           static_cast<unsigned long long>(Config.worldSeed));
}

void ULivingCitySubsystem::Deinitialize()
{
    if (SimHost.IsValid())
    {
        SimHost->Stop();
        UE_LOG(LogLivingCity, Log, TEXT("LIVING CITY simulation stopped after %llu ticks."),
               static_cast<unsigned long long>(SimHost->TicksExecuted()));
        SimHost.Reset();
        lc::SetLogSink(nullptr);
    }
    Super::Deinitialize();
}

void ULivingCitySubsystem::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    if (!SimHost.IsValid())
    {
        return;
    }

    // Track how far we are between the last two published snapshots so rendering can
    // interpolate. The renderer runs at whatever rate it likes; the sim does not care.
    const uint64 Published = SimHost->Sim().Snapshots().PublishedCount();
    if (Published != LastSeenPublishCount)
    {
        LastSeenPublishCount = Published;
        TimeSinceLastPublish = 0.0f;
    }
    else
    {
        TimeSinceLastPublish += DeltaTime;
    }
}

TStatId ULivingCitySubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(ULivingCitySubsystem, STATGROUP_Tickables);
}

bool ULivingCitySubsystem::GetInterpolatedPlaceholder(FVector& OutLocation) const
{
    if (!SimHost.IsValid())
    {
        return false;
    }

    lc::Snapshot Prev, Curr;
    if (!SimHost->Sim().Snapshots().AcquireLatestPair(Prev, Curr))
    {
        return false;
    }

    const float Alpha = FMath::Clamp(TimeSinceLastPublish / kSimTickSeconds, 0.0f, 1.0f);

    const FVector From = LivingCitySnapshotToWorld(Prev);
    const FVector To   = LivingCitySnapshotToWorld(Curr);

    OutLocation = FMath::Lerp(From, To, Alpha);
    return true;
}

FLivingCitySimStatus ULivingCitySubsystem::GetStatus() const
{
    FLivingCitySimStatus Status;
    if (!SimHost.IsValid())
    {
        return Status;
    }

    lc::Snapshot Latest;
    if (SimHost->Sim().Snapshots().AcquireLatest(Latest))
    {
        Status.Tick         = static_cast<int64>(Latest.tick);
        Status.WorldHashHex = FString::Printf(TEXT("%016llx"),
                                              static_cast<unsigned long long>(Latest.worldHash));
        Status.bPaused      = Latest.paused != 0;
        Status.AgentCount   = static_cast<int32>(Latest.agentCount);
        Status.Hour         = Latest.hour;
        Status.Minute       = Latest.minute;
        Status.Day          = Latest.day;
        Status.AtHome       = static_cast<int32>(Latest.atHome);
        Status.Commuting    = static_cast<int32>(Latest.travelWork + Latest.travelHome);
        Status.AtWork       = static_cast<int32>(Latest.atWork);
        Status.Shopping     = static_cast<int32>(Latest.travelShop + Latest.atShop);

        Status.TotalMoneyMinor   = Latest.totalMoneyMinor;
        Status.GovernmentMinor   = Latest.governmentMinor;
        Status.AvgShopPriceMinor = Latest.avgShopPriceMinor;
        Status.HungryCount       = static_cast<int32>(Latest.hungryCount);
        Status.StarvationEvents  = static_cast<int32>(Latest.starvationEvents);
        Status.TotalShopStock    = static_cast<int32>(Latest.totalShopStock);
        Status.TerrainVersion    = static_cast<int64>(Latest.terrainVersion);

        Status.FollowedIndex      = static_cast<int32>(Latest.followedIndex);
        Status.FollowedMoneyMinor = Latest.followedMoneyMinor;
        Status.FollowedHunger     = Latest.followedHunger;
        Status.FollowedFatigue    = Latest.followedFatigue;
        Status.FollowedActivity   = FString(lc::ActivityName(static_cast<lc::Activity>(Latest.followedActivity)));
        Status.FollowedLocation   = FVector(static_cast<float>(Latest.followedX) * 100.0f,
                                            static_cast<float>(Latest.followedY) * 100.0f, 0.0f);
    }

    Status.BuildingCount    = static_cast<int32>(SimHost->Sim().CityLayout().Count());
    Status.SpeedMultiplier  = static_cast<int32>(SimHost->SpeedMultiplier());

    Status.TicksExecuted = static_cast<int64>(SimHost->TicksExecuted());
    Status.Alpha         = FMath::Clamp(TimeSinceLastPublish / kSimTickSeconds, 0.0f, 1.0f);
    return Status;
}

const lc::Simulation* ULivingCitySubsystem::GetSimulation() const
{
    return SimHost.IsValid() ? &SimHost->Sim() : nullptr;
}

bool ULivingCitySubsystem::GetLatestSnapshot(lc::Snapshot& Out) const
{
    if (!SimHost.IsValid())
    {
        return false;
    }
    return SimHost->Sim().Snapshots().AcquireLatest(Out);
}

float ULivingCitySubsystem::GetLastTickMilliseconds() const
{
    if (!SimHost.IsValid())
    {
        return 0.0f;
    }
    return static_cast<float>(SimHost->Sim().Telem().LastTickTotalNs()) / 1000000.0f;
}

void ULivingCitySubsystem::PushCommand(lc::CommandType Type, uint32 Arg0, int64 Arg1, int64 Arg2)
{
    if (!SimHost.IsValid())
    {
        return;
    }

    lc::Command Cmd{};
    Cmd.type          = Type;
    Cmd.arg0          = Arg0;
    Cmd.arg1          = Arg1;
    Cmd.arg2          = Arg2;
    Cmd.issuedAtTick  = SimHost->Sim().CurrentTick();

    if (!SimHost->Sim().Commands().Push(Cmd))
    {
        UE_LOG(LogLivingCity, Warning,
               TEXT("Command queue full; input dropped. The sim thread may be stalled."));
    }
}

void ULivingCitySubsystem::SetPaused(bool bInPaused)
{
    PushCommand(lc::CommandType::SetPaused, bInPaused ? 1u : 0u, 0, 0);
}

void ULivingCitySubsystem::StepOnce()
{
    PushCommand(lc::CommandType::StepOnce, 0u, 0, 0);
}

void ULivingCitySubsystem::ModifyTerrain(float XCm, float YCm, float RadiusCm, float DeltaCm)
{
    // Floats stop here. Everything past this line is integer centimetres.
    const int32  X = FMath::RoundToInt32(XCm);
    const int32  Y = FMath::RoundToInt32(YCm);
    const uint32 Brush = lc::PackTerrainBrush(FMath::RoundToInt32(RadiusCm), FMath::RoundToInt32(DeltaCm));
    PushCommand(lc::CommandType::ModifyTerrain, Brush, X, Y);
}

void ULivingCitySubsystem::SetSpeedMultiplier(int32 Multiplier)
{
    if (SimHost.IsValid())
    {
        SimHost->SetSpeedMultiplier(static_cast<lc::u32>(FMath::Max(1, Multiplier)));
    }
}

void ULivingCitySubsystem::NudgePlaceholder(float MetresX, float MetresY)
{
    // Convert to Q32.32 raw on the way in. The sim never sees a float.
    const int64 RawX = static_cast<int64>(static_cast<double>(MetresX) * 4294967296.0);
    const int64 RawY = static_cast<int64>(static_cast<double>(MetresY) * 4294967296.0);
    PushCommand(lc::CommandType::MovePlaceholder, 0u, RawX, RawY);
}
