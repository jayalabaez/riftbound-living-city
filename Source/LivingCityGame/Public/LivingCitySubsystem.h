// Owns the simulation thread and mediates the engine <-> sim boundary.
//
// This subsystem is the entire surface through which Unreal talks to LIVING CITY.
// Actors never reach into the sim themselves; they ask this for interpolated state.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"
#include "LivingCitySimBridge.h"
#include "LivingCitySubsystem.generated.h"

/** Read-only view of the sim, safe to hand to Blueprints and HUD code. */
USTRUCT(BlueprintType)
struct FLivingCitySimStatus
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int64 Tick = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    FString WorldHashHex;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    bool bPaused = false;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 AgentCount = 0;

    /** Ticks the sim thread has actually executed since start. */
    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int64 TicksExecuted = 0;

    /** Interpolation alpha between the last two published snapshots, 0..1. */
    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    float Alpha = 0.0f;

    /** Sim time of day, straight from the simulation calendar. */
    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 Hour = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 Minute = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 Day = 0;

    /** How the population is spending this moment. */
    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 AtHome = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 Commuting = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 AtWork = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 BuildingCount = 0;

    /** Sim ticks per real second, as a multiple of the 20 Hz base rate. */
    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 SpeedMultiplier = 1;

    /** Citizens on a shopping errand, walking or at the counter. */
    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 Shopping = 0;

    // ---- economy, in minor units (cents) ----------------------------------------------
    /** Sum of every account. Must equal what the central bank issued - invariant I3, live. */
    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int64 TotalMoneyMinor = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int64 GovernmentMinor = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int64 AvgShopPriceMinor = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 HungryCount = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 StarvationEvents = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 TotalShopStock = 0;

    /** Bumps once per terrain change the sim applied. Proves the dig command round-trips. */
    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int64 TerrainVersion = 0;

    // ---- the followed citizen ---------------------------------------------------------
    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 FollowedIndex = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int64 FollowedMoneyMinor = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 FollowedHunger = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    int32 FollowedFatigue = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    FString FollowedActivity;

    UPROPERTY(BlueprintReadOnly, Category = "Living City")
    FVector FollowedLocation = FVector::ZeroVector;
};

UCLASS()
class LIVINGCITYGAME_API ULivingCitySubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    // UWorldSubsystem
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void OnWorldBeginPlay(UWorld& InWorld) override;
    virtual void Deinitialize() override;

    // FTickableGameObject
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickable() const override { return SimHost.IsValid(); }

    /**
     * Interpolated position of the Phase 0 placeholder entity, in Unreal world space.
     * Returns false until the sim has published at least two snapshots.
     */
    bool GetInterpolatedPlaceholder(FVector& OutLocation) const;

    UFUNCTION(BlueprintCallable, Category = "Living City")
    FLivingCitySimStatus GetStatus() const;

    /** Engine -> sim. The only way the game influences the simulation (rule R2). */
    UFUNCTION(BlueprintCallable, Category = "Living City")
    void SetPaused(bool bInPaused);

    UFUNCTION(BlueprintCallable, Category = "Living City")
    void StepOnce();

    /**
     * Fast-forward. Changes how often the sim thread ticks, never what a tick does, so a
     * world run at 300x is bit-identical to one run in real time - it just gets there sooner.
     * Deliberately NOT a Command: the replay log records what happened, not how fast someone
     * watched it happen.
     */
    UFUNCTION(BlueprintCallable, Category = "Living City")
    void SetSpeedMultiplier(int32 Multiplier);

    /**
     * Dig (negative delta) or pile (positive) terrain with a round brush, in centimetres.
     * This is a Command: it enters the replay log, and the terrain only changes when the sim
     * applies it on its next tick. The view then redraws. That round trip is the point.
     */
    UFUNCTION(BlueprintCallable, Category = "Living City")
    void ModifyTerrain(float XCm, float YCm, float RadiusCm, float DeltaCm);

    /** Nudge the placeholder — proves the command path end to end. Offsets are in metres. */
    UFUNCTION(BlueprintCallable, Category = "Living City")
    void NudgePlaceholder(float MetresX, float MetresY);

    /** Milliseconds of wall time the last sim tick took, for the profiler overlay. */
    float GetLastTickMilliseconds() const;

    /**
     * Read-only access to the simulation, for views that need bulk state the snapshot does
     * not carry - the city layout in particular, which is generated once and never changes.
     *
     * CONST FOR A REASON. The engine may look at simulation state; it may never touch it.
     * Anything that wants to CHANGE the world sends a Command (rule R2). Returns null before
     * the sim thread exists.
     */
    const lc::Simulation* GetSimulation() const;

    /** Most recently published snapshot, for views that need the citizen arrays. */
    bool GetLatestSnapshot(lc::Snapshot& Out) const;

private:
    void PushCommand(lc::CommandType Type, uint32 Arg0, int64 Arg1, int64 Arg2);

    TUniquePtr<lc::SimThreadHost> SimHost;

    /** Seconds accumulated since the most recent snapshot publish, for interpolation. */
    float  TimeSinceLastPublish = 0.0f;
    uint64 LastSeenPublishCount = 0;
};
