// A body for the camera.
//
// This is a PLAYER-CONTROLLED PAWN, not a simulated citizen. Phase 8 is where the player
// becomes a real citizen with needs, money, a job and a criminal record, all running through
// the same code paths as everybody else (rule R4). Until then this is a camera with legs:
// it walks on the terrain, it looks around, and it does not exist as far as the simulation
// is concerned.
//
// Rule R2 still binds it. There is no simulation state here and no sim state is written from
// here. Player movement is an ENGINE concern for now precisely because the sim does not own
// the player yet; the moment it does, this class stops deciding where the player is and
// starts reading it from a snapshot like everything else.
//
// The one thing this pawn does to the world is the terrain tool, and it does it the only
// legal way: ULivingCitySubsystem::ModifyTerrain, which becomes a Command on the sim's queue.
// The sim changes the heightfield on its own thread, the snapshot's terrainVersion moves, and
// ALivingCityTerrainView redraws the chunks that changed. This class never touches a mesh.
// That round trip is the whole point: a shovel stroke is in the replay log next to the key
// presses (invariant I1).
//
// Input is bound to raw EKeys through BindKey / BindAxisKey, the same way
// ALivingCityPlayerController does it. There are no input assets in this project and rule R6
// keeps binary data assets out, so there is no Input Mapping Context to point at.
//
//   W A S D        walk, relative to where the camera is looking
//   mouse          look
//   Space          jump
//   Shift          sprint
//   V or F         toggle first / third person
//   left mouse     dig, held: lowers the ground under the crosshair, ten times a second
//   right mouse    pile, held: raises it
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"

#include "LivingCityPlayerCharacter.generated.h"

class UCameraComponent;
class USpringArmComponent;
class UStaticMeshComponent;

UCLASS()
class LIVINGCITYGAME_API ALivingCityPlayerCharacter : public ACharacter
{
    GENERATED_BODY()

public:
    ALivingCityPlayerCharacter();

    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

    /** First person puts the camera at eye height with the spring arm collapsed. */
    UFUNCTION(BlueprintCallable, Category = "Living City")
    void SetFirstPerson(bool bInFirstPerson);

    UFUNCTION(BlueprintPure, Category = "Living City")
    bool IsFirstPerson() const { return bFirstPerson; }

protected:
    virtual void BeginPlay() override;

private:
    // ---- input handlers ---------------------------------------------------------------
    // Digital keys cannot be bound as axes, so WASD is press/release into a small set of
    // flags that Tick turns into movement. That keeps the movement direction resolved once
    // per frame against the current camera yaw rather than once per key event.
    void OnForwardPressed();
    void OnForwardReleased();
    void OnBackPressed();
    void OnBackReleased();
    void OnLeftPressed();
    void OnLeftReleased();
    void OnRightPressed();
    void OnRightReleased();

    // Sprint is bound to BOTH shift keys, so it cannot be a single flag: hold left shift,
    // tap and release right shift, and one shared flag would stop the sprint while left
    // shift is still down. Each key owns its own flag and sprint is the OR of the two.
    void OnSprintLeftPressed();
    void OnSprintLeftReleased();
    void OnSprintRightPressed();
    void OnSprintRightReleased();
    void OnToggleView();

    // The terrain tool is held, not tapped, so the buttons are flags too and Tick meters the
    // applications - see ApplyTerrainTool.
    void OnDigPressed();
    void OnDigReleased();
    void OnPilePressed();
    void OnPileReleased();

    void OnMouseTurn(float Value);
    void OnMouseLook(float Value);

    /** Recomputes MaxWalkSpeed from the sprint keys currently held. */
    void ApplyMovementSpeed();

    /** Pushes the current view mode onto the camera rig and the movement component. */
    void ApplyViewMode();

    /**
     * While a tool button is held: at most ToolRateHz times a second, trace from the camera
     * along its view direction and, on a hit, ask the subsystem to modify the terrain there.
     * Never touches a mesh; the sim owns the ground.
     */
    void ApplyTerrainTool(float DeltaSeconds);

    // ---- components -------------------------------------------------------------------
    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<USpringArmComponent> SpringArm;

    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<UCameraComponent> Camera;

    /** Something to see in third person. Engine primitives only - rule R6. */
    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<UStaticMeshComponent> BodyMesh;

    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<UStaticMeshComponent> HeadMesh;

    // ---- tuning -----------------------------------------------------------------------
    /** Centimetres per second at a walk. A person, not a jeep. */
    UPROPERTY(EditAnywhere, Category = "Living City|Movement")
    float WalkSpeed = 400.0f;

    UPROPERTY(EditAnywhere, Category = "Living City|Movement")
    float SprintMultiplier = 1.8f;

    /**
     * Multiplier on the raw mouse axis. 1.0 matches ADefaultPawn exactly: the project's
     * DefaultInput.ini already applies a 0.07 sensitivity to MouseX/MouseY, and BindAxisKey
     * receives that same massaged value.
     */
    UPROPERTY(EditAnywhere, Category = "Living City|Camera")
    float MouseSensitivity = 1.0f;

    UPROPERTY(EditAnywhere, Category = "Living City|Camera")
    float ThirdPersonArmLength = 350.0f;

    /** Camera height above the capsule centre. The capsule is 88 cm half-height. */
    UPROPERTY(EditAnywhere, Category = "Living City|Camera")
    float EyeHeight = 70.0f;

    /** Where the third-person camera sits relative to the boom: right shoulder, slightly up. */
    UPROPERTY(EditAnywhere, Category = "Living City|Camera")
    FVector ThirdPersonSocketOffset = FVector(0.0, 48.0, 24.0);

    /** How far from the camera the tool reaches, in centimetres. */
    UPROPERTY(EditAnywhere, Category = "Living City|Terrain Tool")
    float ToolReachCm = 2000.0f;

    /** Brush radius handed to the sim, in centimetres. */
    UPROPERTY(EditAnywhere, Category = "Living City|Terrain Tool")
    float ToolRadiusCm = 400.0f;

    /** Height moved per application at the brush centre, in centimetres. Dig is minus this, pile is plus. */
    UPROPERTY(EditAnywhere, Category = "Living City|Terrain Tool")
    float ToolDeltaCm = 25.0f;

    /** Applications per second while a button is held. */
    UPROPERTY(EditAnywhere, Category = "Living City|Terrain Tool")
    float ToolRateHz = 10.0f;

    // ---- state ------------------------------------------------------------------------
    bool bFirstPerson  = false;
    bool bSprintLeft   = false;
    bool bSprintRight  = false;

    bool bMoveForward = false;
    bool bMoveBack    = false;
    bool bMoveLeft    = false;
    bool bMoveRight   = false;

    bool bDigHeld  = false;
    bool bPileHeld = false;

    /** Seconds until the tool may fire again. Zeroed on press so the first application is immediate. */
    float ToolCooldownSeconds = 0.0f;
};
