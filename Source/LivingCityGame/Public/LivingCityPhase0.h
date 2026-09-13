// Phase 0 presentation scaffolding.
//
// Three small classes that together satisfy the Phase 0 "done when" condition:
// the Unreal client renders a placeholder whose position comes from simulation state,
// not from engine-side logic.
//
// Everything here is deliberately thin. Per rule R2, these classes read sim state and
// draw it; they never decide anything. When any of this starts wanting a branch on
// game logic, that logic belongs in LivingCitySim instead.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "LivingCityPhase0.generated.h"

class UStaticMeshComponent;
class ULivingCitySubsystem;

/**
 * A cube that moves because the simulation says so.
 *
 * Its transform is pulled from the interpolated snapshot pair every frame. It has no
 * movement logic of its own — if the sim thread pauses, this stops; if the sim is stepped
 * once, this moves exactly one tick's worth.
 */
UCLASS()
class LIVINGCITYGAME_API ALivingCityPlaceholder : public AActor
{
    GENERATED_BODY()

public:
    ALivingCityPlaceholder();

    virtual void Tick(float DeltaSeconds) override;

protected:
    virtual void BeginPlay() override;

private:
    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<UStaticMeshComponent> Mesh;

    /** True once the sim has published enough snapshots to interpolate between. */
    bool bHasValidState = false;
};

/**
 * The Phase 0 profiler overlay.
 *
 * Shows sim tick index, world hash, tick cost against the 10 ms budget, and frame cost
 * against the 16.6 ms budget — the numbers CLAUDE.md section 4 says we must measure
 * rather than assume.
 */
UCLASS()
class LIVINGCITYGAME_API ALivingCityHUD : public AHUD
{
    GENERATED_BODY()

public:
    virtual void DrawHUD() override;

private:
    void DrawLine(const FString& Text, float& Y, const FLinearColor& Colour);

    /** Smoothed frame time so the overlay is readable rather than flickering. */
    float SmoothedFrameMs = 0.0f;
};

/**
 * Turns key presses into simulation commands. Nothing here changes world state directly —
 * every key becomes a `Command` on the queue and takes effect on the next sim tick, which is
 * the whole point of Boundary 1 (engine -> sim is input only).
 *
 * P     toggle pause
 * O     step exactly one tick while paused
 * arrows / IJKL   nudge the placeholder
 */
UCLASS()
class LIVINGCITYGAME_API ALivingCityPlayerController : public APlayerController
{
    GENERATED_BODY()

public:
    ALivingCityPlayerController();

protected:
    virtual void SetupInputComponent() override;

private:
    void TogglePause();
    void StepOnce();
    void SpeedRealtime();
    void SpeedFast();
    void SpeedFaster();
    void SpeedFastest();
    void NudgeNorth();
    void NudgeSouth();
    void NudgeEast();
    void NudgeWest();

    ULivingCitySubsystem* Sim() const;

    bool bPausedLocally = false;
};

/**
 * Starts the world in a state where the placeholder is visible and the overlay is up.
 * The simulation itself is owned by ULivingCitySubsystem, not by this GameMode — the sim
 * outlives any particular mode and must never depend on one.
 */
UCLASS()
class LIVINGCITYGAME_API ALivingCityGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    ALivingCityGameMode();

protected:
    virtual void BeginPlay() override;

private:
    /** Spawns ground, sun and sky in code so no authored .umap is needed (rule R6). */
    static void BuildScene(UWorld* World);
};
