#include "LivingCityPhase0.h"

#include "LivingCitySubsystem.h"
#include "LivingCityWorldView.h"
#include "LivingCityPlayerCharacter.h"
#include "LivingCityAtmosphere.h"
#include "LivingCityTerrainView.h"
#include "GameFramework/PlayerStart.h"

#include "Components/StaticMeshComponent.h"
#include "GameFramework/DefaultPawn.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMeshActor.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/ConstructorHelpers.h"

// ============================================================ ALivingCityPlaceholder

ALivingCityPlaceholder::ALivingCityPlaceholder()
{
    PrimaryActorTick.bCanEverTick = true;

    // Tick after most gameplay so the subsystem has already updated its interpolation
    // alpha for this frame.
    PrimaryActorTick.TickGroup = TG_PostPhysics;

    Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
    RootComponent = Mesh;
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    // Engine primitive: no asset of our own required, which keeps Phase 0 free of any
    // binary content (rule R6).
    static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
        TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (CubeMesh.Succeeded())
    {
        Mesh->SetStaticMesh(CubeMesh.Object);
    }
}

void ALivingCityPlaceholder::BeginPlay()
{
    Super::BeginPlay();
    SetActorScale3D(FVector(0.5f));
}

void ALivingCityPlaceholder::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    const UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    ULivingCitySubsystem* Sim = World->GetSubsystem<ULivingCitySubsystem>();
    if (!Sim)
    {
        return;
    }

    FVector SimLocation;
    if (Sim->GetInterpolatedPlaceholder(SimLocation))
    {
        // Lift it clear of the ground plane so it is visible in an empty level.
        SetActorLocation(SimLocation + FVector(0.0f, 0.0f, 100.0f));
        bHasValidState = true;
    }
}

// ============================================================ ALivingCityHUD

void ALivingCityHUD::DrawLine(const FString& Text, float& Y, const FLinearColor& Colour)
{
    if (!Canvas)
    {
        return;
    }
    FCanvasTextItem Item(FVector2D(24.0f, Y), FText::FromString(Text),
                         GEngine->GetMediumFont(), Colour);
    Item.EnableShadow(FLinearColor::Black);
    Canvas->DrawItem(Item);
    Y += 18.0f;
}

void ALivingCityHUD::DrawHUD()
{
    Super::DrawHUD();

    const UWorld* World = GetWorld();
    if (!World || !Canvas)
    {
        return;
    }

    ULivingCitySubsystem* Sim = World->GetSubsystem<ULivingCitySubsystem>();
    if (!Sim)
    {
        return;
    }

    const FLivingCitySimStatus Status = Sim->GetStatus();

    const float FrameMs = World->GetDeltaSeconds() * 1000.0f;
    SmoothedFrameMs = (SmoothedFrameMs <= 0.0f)
        ? FrameMs
        : FMath::Lerp(SmoothedFrameMs, FrameMs, 0.1f);

    const float TickMs = Sim->GetLastTickMilliseconds();

    // Budgets from CLAUDE.md section 4.
    constexpr float kFrameBudgetMs = 16.6f;
    constexpr float kTickBudgetMs  = 10.0f;

    const FLinearColor Good  = FLinearColor(0.55f, 0.85f, 0.55f);
    const FLinearColor Warn  = FLinearColor(0.95f, 0.80f, 0.35f);
    const FLinearColor Bad   = FLinearColor(0.95f, 0.40f, 0.40f);
    const FLinearColor Label = FLinearColor(0.75f, 0.80f, 0.90f);

    float Y = 24.0f;

    DrawLine(TEXT("LIVING CITY"), Y, FLinearColor::White);
    Y += 6.0f;

    DrawLine(FString::Printf(TEXT("day %d   %02d:%02d      speed %dx"),
                             Status.Day, Status.Hour, Status.Minute, Status.SpeedMultiplier),
             Y, FLinearColor(0.95f, 0.90f, 0.70f));
    Y += 6.0f;

    DrawLine(FString::Printf(TEXT("citizens   %d"), Status.AgentCount), Y, Label);
    DrawLine(FString::Printf(TEXT("  at home  %d"), Status.AtHome), Y, Label);
    DrawLine(FString::Printf(TEXT("  commuting %d"), Status.Commuting), Y,
             Status.Commuting > 0 ? FLinearColor(0.65f, 0.85f, 0.95f) : Label);
    DrawLine(FString::Printf(TEXT("  at work  %d"), Status.AtWork), Y, Label);
    DrawLine(FString::Printf(TEXT("  shopping %d"), Status.Shopping), Y,
             Status.Shopping > 0 ? FLinearColor(0.95f, 0.80f, 0.55f) : Label);
    DrawLine(FString::Printf(TEXT("buildings  %d"), Status.BuildingCount), Y, Label);
    Y += 6.0f;

    // Money. TotalMoney is every account summed; it must never move except when the central
    // bank mints. If it drifts, invariant I3 is broken and this line is where you see it.
    const auto Dollars = [](int64 Minor) { return static_cast<double>(Minor) / 100.0; };
    DrawLine(FString::Printf(TEXT("money in world  $%.2f"), Dollars(Status.TotalMoneyMinor)), Y, Label);
    DrawLine(FString::Printf(TEXT("government      $%.2f"), Dollars(Status.GovernmentMinor)), Y, Label);
    DrawLine(FString::Printf(TEXT("food price      $%.2f   stock %d"),
                             Dollars(Status.AvgShopPriceMinor), Status.TotalShopStock), Y, Label);
    DrawLine(FString::Printf(TEXT("hungry %d   starved %d"), Status.HungryCount, Status.StarvationEvents),
             Y, Status.StarvationEvents > 0 ? Bad : (Status.HungryCount > 0 ? Warn : Label));
    DrawLine(FString::Printf(TEXT("ground edits    %lld"), Status.TerrainVersion), Y,
             Status.TerrainVersion > 0 ? FLinearColor(0.85f, 0.70f, 0.45f) : Label);
    Y += 6.0f;

    // One real person, followed in full.
    DrawLine(FString::Printf(TEXT("citizen #%d   %s   $%.2f   hunger %d   fatigue %d"),
                             Status.FollowedIndex, *Status.FollowedActivity,
                             Dollars(Status.FollowedMoneyMinor), Status.FollowedHunger, Status.FollowedFatigue),
             Y, FLinearColor(0.85f, 0.95f, 0.75f));
    Y += 6.0f;

    DrawLine(FString::Printf(TEXT("sim tick   %lld"), Status.Tick), Y, Label);
    DrawLine(FString::Printf(TEXT("world hash %s"), *Status.WorldHashHex), Y, Label);
    DrawLine(FString::Printf(TEXT("state      %s"),
                             Status.bPaused ? TEXT("PAUSED") : TEXT("running")),
             Y, Status.bPaused ? Warn : Good);
    Y += 6.0f;

    DrawLine(FString::Printf(TEXT("frame  %5.2f ms / %.1f budget"), SmoothedFrameMs, kFrameBudgetMs),
             Y,
             SmoothedFrameMs > kFrameBudgetMs ? Bad
                 : (SmoothedFrameMs > kFrameBudgetMs * 0.8f ? Warn : Good));

    DrawLine(FString::Printf(TEXT("sim    %5.2f ms / %.1f budget"), TickMs, kTickBudgetMs),
             Y,
             TickMs > kTickBudgetMs ? Bad
                 : (TickMs > kTickBudgetMs * 0.8f ? Warn : Good));

    Y += 6.0f;
    DrawLine(TEXT("every citizen has a home, a job and a shift. they are walking to it."), Y,
             FLinearColor(0.5f, 0.5f, 0.6f));
    DrawLine(TEXT("WASD move   Shift sprint   Space jump   P pause   O step one tick"), Y,
             FLinearColor(0.55f, 0.60f, 0.72f));
    DrawLine(TEXT("1 realtime   2 x10   3 x60 (an hour a minute)   4 x300 (a day in 5 min)"), Y,
             FLinearColor(0.55f, 0.60f, 0.72f));
    DrawLine(TEXT("left mouse dig   right mouse pile   F first/third person"), Y,
             FLinearColor(0.55f, 0.60f, 0.72f));
}

// ============================================================ ALivingCityPlayerController

ALivingCityPlayerController::ALivingCityPlayerController()
{
    bShowMouseCursor = false;
}

ULivingCitySubsystem* ALivingCityPlayerController::Sim() const
{
    const UWorld* World = GetWorld();
    return World ? World->GetSubsystem<ULivingCitySubsystem>() : nullptr;
}

void ALivingCityPlayerController::SetupInputComponent()
{
    Super::SetupInputComponent();
    if (!InputComponent)
    {
        return;
    }

    // Bound to raw keys rather than input mappings on purpose: Phase 0 has no data assets,
    // and rule R6 keeps game data out of binary .uasset files.
    InputComponent->BindKey(EKeys::P, IE_Pressed, this, &ALivingCityPlayerController::TogglePause);
    InputComponent->BindKey(EKeys::O, IE_Pressed, this, &ALivingCityPlayerController::StepOnce);

    // Time control. A sim day is 24 hours; at 1x that is 24 real hours, which is no way to
    // watch a city live. 60x turns an hour into a minute; 300x runs a whole day in 5 minutes.
    InputComponent->BindKey(EKeys::One,   IE_Pressed, this, &ALivingCityPlayerController::SpeedRealtime);
    InputComponent->BindKey(EKeys::Two,   IE_Pressed, this, &ALivingCityPlayerController::SpeedFast);
    InputComponent->BindKey(EKeys::Three, IE_Pressed, this, &ALivingCityPlayerController::SpeedFaster);
    InputComponent->BindKey(EKeys::Four,  IE_Pressed, this, &ALivingCityPlayerController::SpeedFastest);

    InputComponent->BindKey(EKeys::I,     IE_Pressed, this, &ALivingCityPlayerController::NudgeNorth);
    InputComponent->BindKey(EKeys::K,     IE_Pressed, this, &ALivingCityPlayerController::NudgeSouth);
    InputComponent->BindKey(EKeys::L,     IE_Pressed, this, &ALivingCityPlayerController::NudgeEast);
    InputComponent->BindKey(EKeys::J,     IE_Pressed, this, &ALivingCityPlayerController::NudgeWest);
    InputComponent->BindKey(EKeys::Up,    IE_Pressed, this, &ALivingCityPlayerController::NudgeNorth);
    InputComponent->BindKey(EKeys::Down,  IE_Pressed, this, &ALivingCityPlayerController::NudgeSouth);
    InputComponent->BindKey(EKeys::Right, IE_Pressed, this, &ALivingCityPlayerController::NudgeEast);
    InputComponent->BindKey(EKeys::Left,  IE_Pressed, this, &ALivingCityPlayerController::NudgeWest);
}

void ALivingCityPlayerController::TogglePause()
{
    if (ULivingCitySubsystem* S = Sim())
    {
        // The local flag only tracks what we last asked for. The authoritative paused state
        // comes back through the snapshot, because the sim decides, not us.
        bPausedLocally = !bPausedLocally;
        S->SetPaused(bPausedLocally);
    }
}

void ALivingCityPlayerController::StepOnce()
{
    if (ULivingCitySubsystem* S = Sim()) { S->StepOnce(); }
}

void ALivingCityPlayerController::SpeedRealtime() { if (ULivingCitySubsystem* S = Sim()) { S->SetSpeedMultiplier(1); } }
void ALivingCityPlayerController::SpeedFast()     { if (ULivingCitySubsystem* S = Sim()) { S->SetSpeedMultiplier(10); } }
void ALivingCityPlayerController::SpeedFaster()   { if (ULivingCitySubsystem* S = Sim()) { S->SetSpeedMultiplier(60); } }
void ALivingCityPlayerController::SpeedFastest()  { if (ULivingCitySubsystem* S = Sim()) { S->SetSpeedMultiplier(300); } }

void ALivingCityPlayerController::NudgeNorth() { if (ULivingCitySubsystem* S = Sim()) { S->NudgePlaceholder( 1.0f,  0.0f); } }
void ALivingCityPlayerController::NudgeSouth() { if (ULivingCitySubsystem* S = Sim()) { S->NudgePlaceholder(-1.0f,  0.0f); } }
void ALivingCityPlayerController::NudgeEast()  { if (ULivingCitySubsystem* S = Sim()) { S->NudgePlaceholder( 0.0f,  1.0f); } }
void ALivingCityPlayerController::NudgeWest()  { if (ULivingCitySubsystem* S = Sim()) { S->NudgePlaceholder( 0.0f, -1.0f); } }

// ============================================================ ALivingCityGameMode

ALivingCityGameMode::ALivingCityGameMode()
{
    HUDClass              = ALivingCityHUD::StaticClass();
    PlayerControllerClass = ALivingCityPlayerController::StaticClass();
    DefaultPawnClass      = ALivingCityPlayerCharacter::StaticClass();
}

void ALivingCityGameMode::BeginPlay()
{
    Super::BeginPlay();

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    BuildScene(World);

    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    // The city and its people. Everything it draws comes from the simulation.
    World->SpawnActor<ALivingCityWorldView>(
        ALivingCityWorldView::StaticClass(), FTransform::Identity, Params);
}

// Builds the entire Phase 0 scene in code: ground, sun, sky. Nothing is authored.
//
// This is why LIVING CITY can run on the stock /Engine/Maps/Entry and needs no .umap of its
// own. A .umap is a binary asset - undiffable, unreviewable, and uneditable outside the
// editor - so rule R6 pushes hard against introducing one. Generating the scene here also
// removes a whole failure mode: no editor commandlet has to succeed before the game will run.
void ALivingCityGameMode::BuildScene(UWorld* World)
{
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    // Sky, sun, clouds, fog and exposure - Riftbound's atmosphere recipe on a flat world.
    World->SpawnActor<ALivingCityAtmosphere>(
        ALivingCityAtmosphere::StaticClass(), FTransform::Identity, Params);

    // The ground is the simulation's heightfield, drawn chunk by chunk with collision. There
    // is no flat plate any more: what the player stands on is what the player can dig.
    World->SpawnActor<ALivingCityTerrainView>(
        ALivingCityTerrainView::StaticClass(), FTransform::Identity, Params);

    // ---- where the player starts ------------------------------------------------------
    // On a road, not inside a building. /Engine/Maps/Entry has no PlayerStart of its own, so
    // without this the pawn spawns at the origin - which is the middle of the densest block.
    float StartX = 0.0f;
    float StartY = 0.0f;
    if (const ULivingCitySubsystem* Sim = World->GetSubsystem<ULivingCitySubsystem>())
    {
        if (const lc::Simulation* S = Sim->GetSimulation())
        {
            const lc::City& C = S->CityLayout();
            if (C.Count() > 0)
            {
                StartX = static_cast<float>(C.NearestRoadX(0)) * 100.0f;
                StartY = static_cast<float>(C.NearestRoadY(0)) * 100.0f;
            }
        }
    }
    World->SpawnActor<APlayerStart>(FVector(StartX, StartY, 300.0f),
                                    FRotator::ZeroRotator, Params);
}
