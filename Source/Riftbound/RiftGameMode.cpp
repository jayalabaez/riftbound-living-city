#include "RiftGameMode.h"
#include "RiftCharacter.h"
#include "RiftHUD.h"
#include "RiftEnemy.h"
#include "RiftWorld.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"

ARiftGameState::ARiftGameState()
{
    bReplicates = true;
    SetNetUpdateFrequency(5.f);
    Objective = TEXT("Explore the clearing. Shoot the hovering UFO to trigger the anomaly.");
}

void ARiftGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ARiftGameState, Phase);
    DOREPLIFETIME(ARiftGameState, Wave);
    DOREPLIFETIME(ARiftGameState, Remaining);
    DOREPLIFETIME(ARiftGameState, TeamKills);
    DOREPLIFETIME(ARiftGameState, Anomaly);
    DOREPLIFETIME(ARiftGameState, PhaseTime);
    DOREPLIFETIME(ARiftGameState, Objective);
}

ARiftGameMode::ARiftGameMode()
{
    PrimaryActorTick.bCanEverTick = true;
    DefaultPawnClass = ARiftCharacter::StaticClass();
    GameStateClass = ARiftGameState::StaticClass();
    PlayerControllerClass = ARiftPlayerController::StaticClass();
    HUDClass = ARiftHUD::StaticClass();
    bUseSeamlessTravel = false;
}

void ARiftGameMode::BeginPlay()
{
    Super::BeginPlay();
    bool bHasWorld = false;
    for (TActorIterator<ARiftWorld> It(GetWorld()); It; ++It) { bHasWorld = true; break; }
    if (!bHasWorld)
    {
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        GetWorld()->SpawnActor<ARiftWorld>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
    }
    UE_LOG(LogTemp, Display, TEXT("RIFTBOUND: dynamic world ready; shoot the hovering UFO to begin."));
}

void ARiftGameMode::PreLogin(const FString& Options, const FString& Address,
    const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
    Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
    if (ErrorMessage.IsEmpty() && GetNumPlayers() >= 4)
        ErrorMessage = TEXT("This Riftbound session already has four players.");
}

AActor* ARiftGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
    if (SpawnPoints.IsEmpty())
    {
        for (int32 Index = 0; Index < 4; ++Index)
        {
            FActorSpawnParameters Params;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            APlayerStart* Start = GetWorld()->SpawnActor<APlayerStart>(
                FVector(-1800.f + (Index / 2) * 160.f, (Index % 2) * 180.f, 130.f), FRotator::ZeroRotator, Params);
            if (Start) SpawnPoints.Add(Start);
        }
    }
    if (SpawnPoints.Num() > 0) return SpawnPoints[SpawnCursor++ % SpawnPoints.Num()];
    return Super::ChoosePlayerStart_Implementation(Player);
}

int32 ARiftGameMode::ActivePlayerCount() const
{
    int32 Count = 0;
    for (TActorIterator<ARiftCharacter> It(GetWorld()); It; ++It)
        if (It->GetController()) ++Count;
    return FMath::Clamp(Count, 1, 4);
}

void ARiftGameMode::TriggerAnomaly()
{
    ARiftGameState* State = GetGameState<ARiftGameState>();
    if (!State || State->Phase != 0) return;
    State->Phase = 1;
    State->Anomaly = 1;
    State->PhaseTime = 6.f;
    State->Objective = TEXT("TEMPORAL RUPTURE: find cover. Alien drones inbound.");
    State->ForceNetUpdate();
    RefillTeam();
    UE_LOG(LogTemp, Display, TEXT("RIFTBOUND: anomaly triggered; temporal rupture beginning."));
}

void ARiftGameMode::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    ARiftGameState* State = GetGameState<ARiftGameState>();
    if (!State) return;
    if (State->Phase == 1 || State->Phase == 3)
    {
        State->PhaseTime = FMath::Max(0.f, State->PhaseTime - DeltaSeconds);
        if (State->PhaseTime <= 0.f) StartWave();
    }
    else if (State->Phase == 2)
    {
        State->PhaseTime += DeltaSeconds;
        bool bAnyAlive = false;
        bool bAnyPlayer = false;
        for (TActorIterator<ARiftCharacter> It(GetWorld()); It; ++It)
        {
            if (!It->GetController()) continue;
            bAnyPlayer = true;
            if (It->Health > 0.f) bAnyAlive = true;
        }
        AllDownTime = bAnyPlayer && !bAnyAlive ? AllDownTime + DeltaSeconds : 0.f;
        if (AllDownTime > 2.5f)
        {
            State->Phase = 5;
            State->Objective = TEXT("SQUAD LOST. Press Enter to restart the expedition.");
            State->ForceNetUpdate();
            ClearThreats();
            UE_LOG(LogTemp, Display, TEXT("RIFTBOUND: squad defeated."));
            return;
        }
        // Reconcile externally destroyed actors so a disconnected or removed enemy cannot strand a wave.
        RecountTime += DeltaSeconds;
        if (RecountTime >= 2.f)
        {
            RecountTime = 0.f;
            int32 LivingEnemies = 0;
            for (TActorIterator<ARiftEnemy> It(GetWorld()); It; ++It) if (!It->bDead) ++LivingEnemies;
            State->Remaining = LivingEnemies;
            if (LivingEnemies == 0) CompleteWave();
        }
    }
}

void ARiftGameMode::StartWave()
{
    ARiftGameState* State = GetGameState<ARiftGameState>();
    if (!State) return;
    ++State->Wave;
    State->Phase = 2;
    State->Anomaly = State->Wave;
    State->PhaseTime = 0.f;
    State->Remaining = 0;
    AllDownTime = 0.f;
    RecountTime = 0.f;
    const int32 Count = 3 + State->Wave * 3 + (ActivePlayerCount() - 1) * 3;
    const int32 Archetype = FMath::Clamp(State->Wave - 1, 0, 2);
    State->Objective = Archetype == 0 ? TEXT("ALIEN INCURSION: destroy the scout drones.") :
        (Archetype == 1 ? TEXT("IRON EPOCH: defeat the soldiers. Shoot red mines from a distance.") :
        TEXT("PRIMAL BREACH: stop the beasts. They can smash your cover."));
    for (int32 Index = 0; Index < Count; ++Index)
    {
        float Angle = (Index + .4f) * 2.f * PI / Count;
        FVector Position;
        for (int32 Attempt = 0; Attempt < 16; ++Attempt)
        {
            const float Radius = 2600.f + FMath::FRandRange(0.f, 480.f);
            Position = FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, Archetype == 0 ? 215.f : 83.f);
            bool bSafe = true;
            for (TActorIterator<ARiftCharacter> It(GetWorld()); It; ++It)
                if (FVector::Dist2D(Position, It->GetActorLocation()) < 950.f) { bSafe = false; break; }
            if (bSafe) break;
            Angle += .47f;
        }
        const FTransform SpawnTransform((-Position).Rotation(), Position);
        ARiftEnemy* Enemy = GetWorld()->SpawnActorDeferred<ARiftEnemy>(ARiftEnemy::StaticClass(), SpawnTransform,
            this, nullptr, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
        if (Enemy)
        {
            Enemy->Archetype = Archetype;
            UGameplayStatics::FinishSpawningActor(Enemy, SpawnTransform);
            ++State->Remaining;
        }
    }
    State->ForceNetUpdate();
    UE_LOG(LogTemp, Display, TEXT("RIFTBOUND: wave %d started; %d threats; archetype %d."), State->Wave, State->Remaining, Archetype);
}

void ARiftGameMode::EnemyKilled(AController* Killer)
{
    ARiftGameState* State = GetGameState<ARiftGameState>();
    if (!State || State->Phase != 2) return;
    ++State->TeamKills;
    State->Remaining = FMath::Max(0, State->Remaining - 1);
    if (Killer)
    {
        if (ARiftCharacter* Character = Cast<ARiftCharacter>(Killer->GetPawn()))
        {
            Character->AddKill();
            Character->ForceNetUpdate();
        }
    }
    if (State->Remaining == 0) CompleteWave();
    State->ForceNetUpdate();
}

void ARiftGameMode::CompleteWave()
{
    ARiftGameState* State = GetGameState<ARiftGameState>();
    if (!State || State->Phase != 2) return;
    for (TActorIterator<ARiftMine> It(GetWorld()); It; ++It) It->Destroy();
    RefillTeam();
    if (State->Wave >= 3)
    {
        State->Phase = 4;
        State->Objective = TEXT("RIFT SEALED. Expedition complete! Press Enter for another run.");
        UE_LOG(LogTemp, Display, TEXT("RIFTBOUND: victory; %d team eliminations."), State->TeamKills);
    }
    else
    {
        State->Phase = 3;
        State->PhaseTime = 12.f;
        State->Objective = State->Wave == 1 ? TEXT("Supplies restored. Temporal soldiers arrive next. Find cover.") :
            TEXT("Supplies restored. Prehistoric beasts are approaching. Keep moving.");
        UE_LOG(LogTemp, Display, TEXT("RIFTBOUND: wave %d cleared; intermission."), State->Wave);
    }
    State->ForceNetUpdate();
}

void ARiftGameMode::RefillTeam()
{
    for (TActorIterator<ARiftCharacter> It(GetWorld()); It; ++It)
        if (It->Health > 0.f) It->Refill();
}

void ARiftGameMode::ClearThreats()
{
    for (TActorIterator<ARiftEnemy> It(GetWorld()); It; ++It) It->Destroy();
    for (TActorIterator<ARiftMine> It(GetWorld()); It; ++It) It->Destroy();
    if (ARiftGameState* State = GetGameState<ARiftGameState>()) State->Remaining = 0;
}

void ARiftGameMode::RestartRun()
{
    ARiftGameState* State = GetGameState<ARiftGameState>();
    if (!State || (State->Phase != 4 && State->Phase != 5)) return;
    ClearThreats();
    for (TActorIterator<ARiftUFO> It(GetWorld()); It; ++It) It->ResetAnomaly();
    for (TActorIterator<ARiftProp> It(GetWorld()); It; ++It) It->ResetCover();
    State->Phase = 0;
    State->Wave = 0;
    State->Remaining = 0;
    State->TeamKills = 0;
    State->Anomaly = 0;
    State->PhaseTime = 0.f;
    State->Objective = TEXT("Explore the clearing. Shoot the hovering UFO to trigger the anomaly.");
    State->ForceNetUpdate();
    AllDownTime = 0.f;
    int32 Index = 0;
    for (TActorIterator<ARiftCharacter> It(GetWorld()); It; ++It)
    {
        It->Refill();
        It->Kills = 0;
        It->SetActorLocation(FVector(-1800.f, Index++ * 180.f, 135.f), false, nullptr, ETeleportType::TeleportPhysics);
        It->SetActorRotation(FRotator::ZeroRotator);
        if (AController* Controller = It->GetController()) Controller->SetControlRotation(FRotator::ZeroRotator);
        It->ForceNetUpdate();
    }
    UE_LOG(LogTemp, Display, TEXT("RIFTBOUND: expedition reset; ready for a new anomaly."));
}
