#include "LivingCityPlayerCharacter.h"

#include "LivingCitySubsystem.h"

#include "Camera/CameraComponent.h"
#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
    /** Human proportions: 34 cm radius, 88 cm half-height, so 1.76 m tall. */
    constexpr float kCapsuleRadius     = 34.0f;
    constexpr float kCapsuleHalfHeight = 88.0f;

    /**
     * Sizes a mesh component so its bounds exactly fill SizeCm with its base at BaseZCm,
     * relative to its parent.
     *
     * The mesh's own bounds are measured rather than assumed. /Engine/BasicShapes has no
     * capsule, so the body is a cylinder and the head a sphere, and neither is guaranteed to
     * keep the dimensions it has today.
     */
    void FitMeshComponent(UStaticMeshComponent* Component, const FVector& SizeCm, double BaseZCm)
    {
        if (!Component || !Component->GetStaticMesh())
        {
            return;
        }

        constexpr double kEpsilon = 1.0e-4;

        const FBox     Local       = Component->GetStaticMesh()->GetBoundingBox();
        const FVector  LocalSize   = Local.GetSize();
        const FVector  LocalCentre = Local.GetCenter();

        const FVector Scale(
            LocalSize.X > kEpsilon ? SizeCm.X / LocalSize.X : 1.0,
            LocalSize.Y > kEpsilon ? SizeCm.Y / LocalSize.Y : 1.0,
            LocalSize.Z > kEpsilon ? SizeCm.Z / LocalSize.Z : 1.0);

        Component->SetRelativeScale3D(Scale);
        Component->SetRelativeLocation(FVector(
            -Scale.X * LocalCentre.X,
            -Scale.Y * LocalCentre.Y,
            BaseZCm - Scale.Z * Local.Min.Z));
    }

    /** Flat colour off the engine basic-shape material, so no .uasset of ours is needed (R6). */
    void TintMeshComponent(UStaticMeshComponent* Component, UObject* Outer, const FLinearColor& Colour)
    {
        if (!Component)
        {
            return;
        }

        UMaterialInterface* Base = LoadObject<UMaterialInterface>(
            nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
        if (!Base)
        {
            return;
        }

        UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Base, Outer);
        if (!Instance)
        {
            return;
        }

        Instance->SetVectorParameterValue(TEXT("Color"), Colour);
        Component->SetMaterial(0, Instance);
    }
}

// ============================================================ construction

ALivingCityPlayerCharacter::ALivingCityPlayerCharacter()
{
    PrimaryActorTick.bCanEverTick = true;

    GetCapsuleComponent()->InitCapsuleSize(kCapsuleRadius, kCapsuleHalfHeight);

    // The controller aims the camera; the body decides its own facing from how it is moving.
    // ApplyViewMode() flips this round in first person.
    bUseControllerRotationPitch = false;
    bUseControllerRotationYaw   = false;
    bUseControllerRotationRoll  = false;

    if (UCharacterMovementComponent* Movement = GetCharacterMovement())
    {
        Movement->bOrientRotationToMovement = true;
        Movement->RotationRate              = FRotator(0.0, 540.0, 0.0);
        Movement->MaxWalkSpeed              = WalkSpeed;
        Movement->MaxAcceleration           = 1500.0f;
        Movement->BrakingDecelerationWalking = 2000.0f;
        Movement->JumpZVelocity             = 450.0f;
        Movement->AirControl                = 0.2f;
        // The ground is a 2 m heightfield with road slabs on it; a generous step-up keeps
        // kerbs, slab seams and freshly dug edges from snagging the capsule.
        Movement->MaxStepHeight             = 45.0f;
        Movement->SetWalkableFloorAngle(50.0f);
    }

    // ---- camera rig ------------------------------------------------------------------
    SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
    SpringArm->SetupAttachment(GetCapsuleComponent());
    SpringArm->SetRelativeLocation(FVector(0.0, 0.0, static_cast<double>(EyeHeight)));
    SpringArm->TargetArmLength        = ThirdPersonArmLength;
    SpringArm->bUsePawnControlRotation = true;
    SpringArm->bInheritPitch           = true;
    SpringArm->bInheritYaw             = true;
    SpringArm->bInheritRoll            = false;
    SpringArm->bDoCollisionTest        = true;
    SpringArm->ProbeSize               = 12.0f;
    SpringArm->bEnableCameraLag        = true;
    SpringArm->CameraLagSpeed          = 14.0f;
    SpringArm->SocketOffset            = ThirdPersonSocketOffset;

    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
    Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
    Camera->bUsePawnControlRotation = false;   // the boom already applies it
    Camera->FieldOfView             = 90.0f;

    // ---- something to look at in third person ----------------------------------------
    static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(
        TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(
        TEXT("/Engine/BasicShapes/Sphere.Sphere"));

    BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
    BodyMesh->SetupAttachment(GetCapsuleComponent());
    if (CylinderMesh.Succeeded())
    {
        BodyMesh->SetStaticMesh(CylinderMesh.Object);
    }
    BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    BodyMesh->SetCanEverAffectNavigation(false);

    HeadMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Head"));
    HeadMesh->SetupAttachment(GetCapsuleComponent());
    if (SphereMesh.Succeeded())
    {
        HeadMesh->SetStaticMesh(SphereMesh.Object);
    }
    HeadMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    HeadMesh->SetCanEverAffectNavigation(false);
}

void ALivingCityPlayerCharacter::BeginPlay()
{
    Super::BeginPlay();

    // Body from the soles of the feet up, head on top of it. The capsule spans
    // -kCapsuleHalfHeight .. +kCapsuleHalfHeight around the actor origin.
    constexpr double kFeetZ      = -static_cast<double>(kCapsuleHalfHeight);
    constexpr double kBodyHeight = 138.0;
    constexpr double kHeadSize   = 44.0;

    FitMeshComponent(BodyMesh, FVector(68.0, 68.0, kBodyHeight), kFeetZ);
    FitMeshComponent(HeadMesh, FVector(kHeadSize, kHeadSize, kHeadSize), kFeetZ + kBodyHeight - 4.0);

    TintMeshComponent(BodyMesh, this, FLinearColor(0.150f, 0.210f, 0.320f));
    TintMeshComponent(HeadMesh, this, FLinearColor(0.720f, 0.560f, 0.440f));

    if (SpringArm)
    {
        SpringArm->SetRelativeLocation(FVector(0.0, 0.0, static_cast<double>(EyeHeight)));
    }

    // WalkSpeed and SprintMultiplier are EditAnywhere, so the constructor's copy of them is
    // only the CDO's. Re-apply here from whatever this instance actually carries.
    ApplyMovementSpeed();

    // NOTE: do not add the actor tick as a prerequisite of the movement component's tick.
    // ACharacter already orders them the other way round, so the engine refuses the edge
    // ("would form a cycle") on EVERY frame and logs it - the prerequisite never took effect,
    // and removing it changes nothing except the log. If input latency is ever measured to
    // matter, the fix is to call AddMovementInput from the axis callbacks directly, which is
    // the standard pattern, rather than buffering flags for Tick.

    ApplyViewMode();
}

// ============================================================ input

void ALivingCityPlayerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);

    if (!PlayerInputComponent)
    {
        return;
    }

    // Raw keys, no input assets. Same approach as ALivingCityPlayerController - see R6.
    PlayerInputComponent->BindKey(EKeys::W, IE_Pressed,  this, &ALivingCityPlayerCharacter::OnForwardPressed);
    PlayerInputComponent->BindKey(EKeys::W, IE_Released, this, &ALivingCityPlayerCharacter::OnForwardReleased);
    PlayerInputComponent->BindKey(EKeys::S, IE_Pressed,  this, &ALivingCityPlayerCharacter::OnBackPressed);
    PlayerInputComponent->BindKey(EKeys::S, IE_Released, this, &ALivingCityPlayerCharacter::OnBackReleased);
    PlayerInputComponent->BindKey(EKeys::A, IE_Pressed,  this, &ALivingCityPlayerCharacter::OnLeftPressed);
    PlayerInputComponent->BindKey(EKeys::A, IE_Released, this, &ALivingCityPlayerCharacter::OnLeftReleased);
    PlayerInputComponent->BindKey(EKeys::D, IE_Pressed,  this, &ALivingCityPlayerCharacter::OnRightPressed);
    PlayerInputComponent->BindKey(EKeys::D, IE_Released, this, &ALivingCityPlayerCharacter::OnRightReleased);

    PlayerInputComponent->BindKey(EKeys::SpaceBar, IE_Pressed,  this, &ACharacter::Jump);
    PlayerInputComponent->BindKey(EKeys::SpaceBar, IE_Released, this, &ACharacter::StopJumping);

    PlayerInputComponent->BindKey(EKeys::LeftShift,  IE_Pressed,  this, &ALivingCityPlayerCharacter::OnSprintLeftPressed);
    PlayerInputComponent->BindKey(EKeys::LeftShift,  IE_Released, this, &ALivingCityPlayerCharacter::OnSprintLeftReleased);
    PlayerInputComponent->BindKey(EKeys::RightShift, IE_Pressed,  this, &ALivingCityPlayerCharacter::OnSprintRightPressed);
    PlayerInputComponent->BindKey(EKeys::RightShift, IE_Released, this, &ALivingCityPlayerCharacter::OnSprintRightReleased);

    // V is the original binding; F is the one every other game uses. Both stay.
    PlayerInputComponent->BindKey(EKeys::V, IE_Pressed, this, &ALivingCityPlayerCharacter::OnToggleView);
    PlayerInputComponent->BindKey(EKeys::F, IE_Pressed, this, &ALivingCityPlayerCharacter::OnToggleView);

    // The terrain tool. Held, not tapped: Tick meters the applications.
    PlayerInputComponent->BindKey(EKeys::LeftMouseButton,  IE_Pressed,  this, &ALivingCityPlayerCharacter::OnDigPressed);
    PlayerInputComponent->BindKey(EKeys::LeftMouseButton,  IE_Released, this, &ALivingCityPlayerCharacter::OnDigReleased);
    PlayerInputComponent->BindKey(EKeys::RightMouseButton, IE_Pressed,  this, &ALivingCityPlayerCharacter::OnPilePressed);
    PlayerInputComponent->BindKey(EKeys::RightMouseButton, IE_Released, this, &ALivingCityPlayerCharacter::OnPileReleased);

    PlayerInputComponent->BindAxisKey(EKeys::MouseX, this, &ALivingCityPlayerCharacter::OnMouseTurn);
    PlayerInputComponent->BindAxisKey(EKeys::MouseY, this, &ALivingCityPlayerCharacter::OnMouseLook);
}

void ALivingCityPlayerCharacter::OnForwardPressed()  { bMoveForward = true;  }
void ALivingCityPlayerCharacter::OnForwardReleased() { bMoveForward = false; }
void ALivingCityPlayerCharacter::OnBackPressed()     { bMoveBack    = true;  }
void ALivingCityPlayerCharacter::OnBackReleased()    { bMoveBack    = false; }
void ALivingCityPlayerCharacter::OnLeftPressed()     { bMoveLeft    = true;  }
void ALivingCityPlayerCharacter::OnLeftReleased()    { bMoveLeft    = false; }
void ALivingCityPlayerCharacter::OnRightPressed()    { bMoveRight   = true;  }
void ALivingCityPlayerCharacter::OnRightReleased()   { bMoveRight   = false; }

// Both shift keys sprint, and either one alone is enough. They therefore need one flag each:
// with a single shared flag, releasing right shift while left shift is still held would drop
// the player back to a walk - which reads as the sprint key randomly failing.
void ALivingCityPlayerCharacter::OnSprintLeftPressed()   { bSprintLeft  = true;  ApplyMovementSpeed(); }
void ALivingCityPlayerCharacter::OnSprintLeftReleased()  { bSprintLeft  = false; ApplyMovementSpeed(); }
void ALivingCityPlayerCharacter::OnSprintRightPressed()  { bSprintRight = true;  ApplyMovementSpeed(); }
void ALivingCityPlayerCharacter::OnSprintRightReleased() { bSprintRight = false; ApplyMovementSpeed(); }

// A press zeroes the cooldown so the first stroke lands on the very next frame rather than
// up to a tenth of a second later; the metering only applies while the button stays down.
void ALivingCityPlayerCharacter::OnDigPressed()   { bDigHeld  = true;  ToolCooldownSeconds = 0.0f; }
void ALivingCityPlayerCharacter::OnDigReleased()  { bDigHeld  = false; }
void ALivingCityPlayerCharacter::OnPilePressed()  { bPileHeld = true;  ToolCooldownSeconds = 0.0f; }
void ALivingCityPlayerCharacter::OnPileReleased() { bPileHeld = false; }

void ALivingCityPlayerCharacter::ApplyMovementSpeed()
{
    if (UCharacterMovementComponent* Movement = GetCharacterMovement())
    {
        const bool bSprinting = bSprintLeft || bSprintRight;
        Movement->MaxWalkSpeed = bSprinting ? WalkSpeed * SprintMultiplier : WalkSpeed;
    }
}

void ALivingCityPlayerCharacter::OnToggleView()
{
    SetFirstPerson(!bFirstPerson);
}

void ALivingCityPlayerCharacter::OnMouseTurn(float Value)
{
    if (Value != 0.0f)
    {
        AddControllerYawInput(Value * MouseSensitivity);
    }
}

void ALivingCityPlayerCharacter::OnMouseLook(float Value)
{
    if (Value != 0.0f)
    {
        // ADefaultPawn maps MouseY through a -1 scale before feeding AddControllerPitchInput.
        // Reproduce the sign here or the vertical look comes out inverted.
        AddControllerPitchInput(-Value * MouseSensitivity);
    }
}

// ============================================================ view mode

void ALivingCityPlayerCharacter::SetFirstPerson(bool bInFirstPerson)
{
    bFirstPerson = bInFirstPerson;
    ApplyViewMode();
}

void ALivingCityPlayerCharacter::ApplyViewMode()
{
    if (SpringArm)
    {
        SpringArm->TargetArmLength  = bFirstPerson ? 0.0f : ThirdPersonArmLength;
        SpringArm->SocketOffset     = bFirstPerson ? FVector::ZeroVector : ThirdPersonSocketOffset;
        SpringArm->bDoCollisionTest = !bFirstPerson;
        SpringArm->bEnableCameraLag = !bFirstPerson;
    }

    // The camera is inside the body in first person, so the body has to go.
    if (BodyMesh)
    {
        BodyMesh->SetVisibility(!bFirstPerson, true);
    }
    if (HeadMesh)
    {
        HeadMesh->SetVisibility(!bFirstPerson, true);
    }

    // First person: the body faces wherever the camera looks, so strafing reads correctly.
    // Third person: the body turns toward the direction it is actually walking.
    bUseControllerRotationYaw = bFirstPerson;
    if (UCharacterMovementComponent* Movement = GetCharacterMovement())
    {
        Movement->bOrientRotationToMovement = !bFirstPerson;
    }
}

// ============================================================ terrain tool

void ALivingCityPlayerCharacter::ApplyTerrainTool(float DeltaSeconds)
{
    if (!bDigHeld && !bPileHeld)
    {
        return;
    }

    // Metered by a cooldown that is RESET, not accumulated, after each application: a slow
    // frame can never bank strokes and release them in a burst. At most one application per
    // frame, and at most ToolRateHz per second.
    ToolCooldownSeconds -= DeltaSeconds;
    if (ToolCooldownSeconds > 0.0f)
    {
        return;
    }
    ToolCooldownSeconds = (ToolRateHz > 0.0f) ? 1.0f / ToolRateHz : 0.0f;

    // Both buttons held cancel out to nothing, which is exactly what the sum says.
    const float Delta = (bPileHeld ? ToolDeltaCm : 0.0f) - (bDigHeld ? ToolDeltaCm : 0.0f);
    if (Delta == 0.0f || !Camera)
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    ULivingCitySubsystem* Subsystem = World->GetSubsystem<ULivingCitySubsystem>();
    if (!Subsystem)
    {
        return;
    }

    // Straight out of the lens along the view direction: in first person that is the
    // crosshair, in third person it is the point the camera looks at past the shoulder. The
    // pawn's own capsule is skipped so the boom camera cannot hit the player's back.
    const FVector Start = Camera->GetComponentLocation();
    const FVector End   = Start + Camera->GetForwardVector() * static_cast<double>(ToolReachCm);

    FCollisionQueryParams Params(FName(TEXT("LivingCityTerrainTool")), /*bTraceComplex*/ true, this);

    FHitResult Hit;
    if (!World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
    {
        return;
    }

    // The only thing this pawn ever does to the world, and it goes through the one door.
    Subsystem->ModifyTerrain(static_cast<float>(Hit.ImpactPoint.X),
                             static_cast<float>(Hit.ImpactPoint.Y),
                             ToolRadiusCm, Delta);
}

// ============================================================ per-frame

void ALivingCityPlayerCharacter::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    // The tool runs whether or not the player is moving.
    ApplyTerrainTool(DeltaSeconds);

    const float Forward = (bMoveForward ? 1.0f : 0.0f) - (bMoveBack ? 1.0f : 0.0f);
    const float Strafe  = (bMoveRight   ? 1.0f : 0.0f) - (bMoveLeft ? 1.0f : 0.0f);

    if (Forward == 0.0f && Strafe == 0.0f)
    {
        return;
    }

    // Movement is relative to where the camera is looking, flattened to the ground plane:
    // looking up must not slow you down, and looking straight down must not stop you.
    // The movement component normalises the combined vector, so diagonals are not faster.
    const FRotationMatrix Basis(FRotator(0.0, GetControlRotation().Yaw, 0.0));

    AddMovementInput(Basis.GetUnitAxis(EAxis::X), Forward);
    AddMovementInput(Basis.GetUnitAxis(EAxis::Y), Strafe);
}
