// Opt-in integration audit. All mutations go through the same public subsystem
// commands as the player. No test actor, content asset or normal-play hook is needed.
#include "LivingCitySubsystem.h"
#include "LivingCityPhase0.h"
#include "LivingCityPlayerCharacter.h"
#include "LivingCityTerrainView.h"
#include "LivingCityWorldView.h"

#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

namespace LivingCityRuntimeAudit
{
    struct FRun
    {
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<ALivingCityTerrainView> Terrain;
        TWeakObjectPtr<ALivingCityPlayerCharacter> Pawn;
        lc::Snapshot InitialSnapshot;
        FVector WalkStart = FVector::ZeroVector;
        FVector EditPoint = FVector::ZeroVector;
        double Started = 0.0;
        double StageStarted = 0.0;
        double FrameSeconds = 0.0;
        double MaxFrameSeconds = 0.0;
        int64 InitialTick = 0;
        int64 PauseTick = 0;
        int64 SpeedTick = 0;
        int64 TerrainVersion = 0;
        int32 Stage = 0;
        int32 Checks = 0;
        int32 Frames = 0;
        float InitialHeight = 0.0f;
        double RealtimeRate = 0.0;
        bool bFinished = false;
    };

    static TUniquePtr<FRun> Run;

    static void Pass(const TCHAR* Name, const FString& Details = FString())
    {
        ++Run->Checks;
        UE_LOG(LogTemp, Display, TEXT("LIVINGCITY AUDIT PASS %s %s"), Name, *Details);
    }

    static void Fail(const TCHAR* Reason)
    {
        Run->bFinished = true;
        UE_LOG(LogTemp, Error, TEXT("LIVINGCITY AUDIT FAIL stage=%d reason=%s"), Run->Stage, Reason);
        FPlatformMisc::RequestExit(false);
    }

    static void Advance(double Now)
    {
        ++Run->Stage;
        Run->StageStarted = Now;
    }

    static void Capture(const TCHAR* Name)
    {
        if (!FParse::Param(FCommandLine::Get(), TEXT("LivingCityCapture"))) return;
        FString Directory;
        if (!FParse::Value(FCommandLine::Get(), TEXT("LivingCityAuditOutput="), Directory))
            Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots/LivingCityAudit"));
        IFileManager::Get().MakeDirectory(*Directory, true);
        FScreenshotRequest::RequestScreenshot(Directory / (FString(Name) + TEXT(".png")), false, false);
        UE_LOG(LogTemp, Display, TEXT("LIVINGCITY AUDIT CAPTURE %s"), Name);
    }

    static bool FindMeshHeight(ALivingCityTerrainView* Terrain, const FVector& Point, double& Height)
    {
        TInlineComponentArray<UProceduralMeshComponent*> Meshes(Terrain);
        for (UProceduralMeshComponent* Mesh : Meshes)
        {
            if (!Mesh || !Mesh->IsVisible()) continue;
            const FProcMeshSection* Section = Mesh->GetProcMeshSection(0);
            if (!Section) continue;
            for (const FProcMeshVertex& Vertex : Section->ProcVertexBuffer)
            {
                const FVector Position = Mesh->GetComponentTransform().TransformPosition(Vertex.Position);
                if (FMath::Abs(Position.X - Point.X) < 0.1 && FMath::Abs(Position.Y - Point.Y) < 0.1)
                {
                    Height = Position.Z;
                    return true;
                }
            }
        }
        return false;
    }

    static void Tick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
    {
        // Cache only after a real game world ticks, when command-line initialization is
        // complete. Normal play returns here without allocating or changing the world.
        static const bool bAudit = FParse::Param(FCommandLine::Get(), TEXT("LivingCityAudit"));
        static const bool bIsolation = FParse::Param(FCommandLine::Get(), TEXT("LivingCityIsolationAudit"));
        if ((!bAudit && !bIsolation) || !World || !World->IsGameWorld() ||
            World->GetNetMode() != NM_Standalone || !World->HasBegunPlay()) return;
        (void)TickType;
        const double Now = FPlatformTime::Seconds();
        if (!Run)
        {
            Run = MakeUnique<FRun>();
            Run->World = World;
            Run->Started = Run->StageStarted = Now;
            UE_LOG(LogTemp, Display, TEXT("LIVINGCITY AUDIT START isolation=%d"), bIsolation ? 1 : 0);
        }
        if (Run->bFinished || Run->World.Get() != World) return;
        if (Now - Run->Started > 100.0) { Fail(TEXT("GLOBAL_TIMEOUT")); return; }

        ULivingCitySubsystem* Sim = World->GetSubsystem<ULivingCitySubsystem>();
        const double Elapsed = Now - Run->StageStarted;
        if (bIsolation)
        {
            if (Now - Run->Started < 3.0) return;
            if (Cast<ALivingCityGameMode>(World->GetAuthGameMode())) { Fail(TEXT("ISOLATION_WRONG_GAME_MODE")); return; }
            if (Sim && Sim->GetSimulation()) { Fail(TEXT("SIMULATION_RUNNING_IN_OTHER_GAME_MODE")); return; }
            Pass(TEXT("MODE_ISOLATION"), TEXT("simulation_thread_absent=true"));
            Run->bFinished = true;
            UE_LOG(LogTemp, Display, TEXT("LIVINGCITY AUDIT COMPLETE PASS checks=%d"), Run->Checks);
            FPlatformMisc::RequestExit(false);
            return;
        }
        if (!Cast<ALivingCityGameMode>(World->GetAuthGameMode())) { Fail(TEXT("WRONG_GAME_MODE")); return; }
        if (!Sim || !Sim->GetSimulation())
        {
            if (Elapsed > 10.0) Fail(TEXT("SIMULATION_NOT_STARTED"));
            return;
        }
        const FLivingCitySimStatus Status = Sim->GetStatus();
        // GetStatus returns defaults if its lock-free snapshot acquire overlaps a
        // writer. Retry on the next frame; a transient acquire miss is not a reset.
        if (Status.Tick == 0 || Status.AgentCount == 0) return;
        if (Run->Stage > 0 && Run->Stage < 10)
        {
            ++Run->Frames;
            Run->FrameSeconds += DeltaSeconds;
            Run->MaxFrameSeconds = FMath::Max(Run->MaxFrameSeconds, static_cast<double>(DeltaSeconds));
        }
        if (Run->Stage == 0)
        {
            if (!Run->Terrain.IsValid())
                for (TActorIterator<ALivingCityTerrainView> It(World); It; ++It) { Run->Terrain = *It; break; }
            APlayerController* Controller = World->GetFirstPlayerController();
            Run->Pawn = Controller ? Cast<ALivingCityPlayerCharacter>(Controller->GetPawn()) : nullptr;
            if (!Run->Terrain.IsValid() || !Run->Terrain->IsReady() || !Run->Pawn.IsValid() || Status.Tick < 10)
            {
                if (Elapsed > 25.0) Fail(TEXT("CITY_TERRAIN_OR_PAWN_NOT_READY"));
                return;
            }
            if (!Run->Pawn->GetCharacterMovement()->IsMovingOnGround())
            {
                if (Elapsed > 25.0) Fail(TEXT("PLAYER_NOT_GROUNDED_AT_SPAWN"));
                return;
            }
            if (Status.AgentCount != 1500 || Status.BuildingCount < 100)
            { Fail(TEXT("CITY_POPULATION_OR_BUILDINGS_MISSING")); return; }
            int32 CitizenInstances = 0;
            int32 BuildingInstances = 0;
            for (TActorIterator<ALivingCityWorldView> It(World); It; ++It)
            {
                TInlineComponentArray<UInstancedStaticMeshComponent*> Meshes(*It);
                for (UInstancedStaticMeshComponent* Mesh : Meshes)
                {
                    if (Mesh->GetFName() == FName(TEXT("Citizens"))) CitizenInstances += Mesh->GetInstanceCount();
                    if (Mesh->GetFName() == FName(TEXT("Buildings"))) BuildingInstances += Mesh->GetInstanceCount();
                }
            }
            if (CitizenInstances != Status.AgentCount || BuildingInstances != Status.BuildingCount)
            {
                if (Elapsed > 25.0) Fail(TEXT("CITY_NOT_REPRESENTED_IN_RENDER_COMPONENTS"));
                return;
            }
            if (!Sim->GetLatestSnapshot(Run->InitialSnapshot)) return;
            Pass(TEXT("LIVE_SIMULATION"), TEXT("simulation_non_null=true"));
            Pass(TEXT("POPULATION_AND_RENDER"), FString::Printf(TEXT("citizens=%d buildings=%d citizen_instances=%d building_instances=%d"),
                Status.AgentCount, Status.BuildingCount, CitizenInstances, BuildingInstances));
            Pass(TEXT("PLAYER_GROUNDED"), FString::Printf(TEXT("z_cm=%.1f"), Run->Pawn->GetActorLocation().Z));
            Sim->SetSpeedMultiplier(1);
            Run->InitialTick = Status.Tick;
            Controller->SetControlRotation(FRotator(-8.0, 20.0, 0.0));
            Advance(Now);
            return;
        }
        ALivingCityPlayerCharacter* Pawn = Run->Pawn.Get();
        ALivingCityTerrainView* Terrain = Run->Terrain.Get();
        if (!Pawn || !Terrain) { Fail(TEXT("TEST_ACTOR_LOST")); return; }
        if (Elapsed > 12.0 && Run->Stage < 10) { Fail(TEXT("STAGE_TIMEOUT")); return; }

        switch (Run->Stage)
        {
        case 1:
            if (Elapsed < 2.0) return;
            {
                lc::Snapshot Latest;
                if (!Sim->GetLatestSnapshot(Latest)) return;
                int32 Moved = 0;
                const uint32 Count = FMath::Min(Latest.citizenCount, Run->InitialSnapshot.citizenCount);
                for (uint32 Index = 0; Index < Count; ++Index)
                    if (Latest.citizenX[Index] != Run->InitialSnapshot.citizenX[Index] ||
                        Latest.citizenY[Index] != Run->InitialSnapshot.citizenY[Index]) ++Moved;
                if (Status.Tick - Run->InitialTick < 20 || Moved < 1) { Fail(TEXT("SIMULATION_OR_CITIZENS_NOT_ADVANCING")); return; }
                Run->RealtimeRate = (Status.Tick - Run->InitialTick) / Elapsed;
                Pass(TEXT("REALTIME_AND_CITIZEN_MOTION"), FString::Printf(TEXT("ticks=%lld moved_citizens=%d ticks_per_second=%.2f"),
                    Status.Tick - Run->InitialTick, Moved, Run->RealtimeRate));
                Capture(TEXT("LivingCityStreet"));
                Run->WalkStart = Pawn->GetActorLocation();
                Advance(Now);
            }
            return;
        case 2:
            Pawn->AddMovementInput(FVector::ForwardVector, 1.0f);
            if (Elapsed < 2.0) return;
            {
                const double Walked = FVector::Dist2D(Run->WalkStart, Pawn->GetActorLocation());
                Pawn->ConsumeMovementInputVector();
                Pawn->GetCharacterMovement()->StopMovementImmediately();
                if (Walked < 200.0 || Walked > 1600.0 || !Pawn->GetCharacterMovement()->IsMovingOnGround())
                { Fail(TEXT("PLAYER_WALK_BLOCKED_OR_UNGROUNDED")); return; }
                Pass(TEXT("PLAYER_WALK"), FString::Printf(TEXT("distance_cm=%.1f grounded=true"), Walked));
                Sim->SetPaused(true);
                Advance(Now);
            }
            return;
        case 3:
            if (!Status.bPaused) return;
            Run->PauseTick = Status.Tick;
            Advance(Now);
            return;
        case 4:
            if (Elapsed < 0.6) return;
            if (!Status.bPaused || Status.Tick != Run->PauseTick) { Fail(TEXT("PAUSE_DID_NOT_HOLD_WORLD_TICK")); return; }
            Pass(TEXT("PAUSE_HOLDS_TICK"), FString::Printf(TEXT("tick=%lld"), Status.Tick));
            Sim->StepOnce();
            Advance(Now);
            return;
        case 5:
            if (Status.Tick == Run->PauseTick) return;
            if (!Status.bPaused || Status.Tick != Run->PauseTick + 1) { Fail(TEXT("STEP_DID_NOT_ADVANCE_EXACTLY_ONCE")); return; }
            Advance(Now);
            return;
        case 6:
            if (Elapsed < 0.6) return;
            if (!Status.bPaused || Status.Tick != Run->PauseTick + 1) { Fail(TEXT("STEP_CONTINUED_ADVANCING_WHILE_PAUSED")); return; }
            Pass(TEXT("SINGLE_STEP"), FString::Printf(TEXT("before_tick=%lld after_tick=%lld paused=true"), Run->PauseTick, Status.Tick));
            Sim->SetSpeedMultiplier(10);
            Sim->SetPaused(false);
            Advance(Now);
            return;
        case 7:
            if (Status.bPaused || Status.SpeedMultiplier != 10) return;
            Run->SpeedTick = Status.Tick;
            Advance(Now);
            return;
        case 8:
            if (Elapsed < 1.2) return;
            {
                const double Rate = (Status.Tick - Run->SpeedTick) / Elapsed;
                if (Status.bPaused || Status.SpeedMultiplier != 10 || Rate < Run->RealtimeRate * 3.0)
                { Fail(TEXT("SPEED_CONTROL_DID_NOT_ACCELERATE_SIMULATION")); return; }
                Pass(TEXT("SPEED_CONTROL"), FString::Printf(TEXT("requested_multiplier=10 measured_ticks_per_second=%.2f realtime_ticks_per_second=%.2f"), Rate, Run->RealtimeRate));
                Sim->SetSpeedMultiplier(1);
                // Immutable city dimensions are safe to read. The edit is outside the
                // building grid, on an exact 2 m cell centre for an unambiguous vertex.
                const lc::City& City = Sim->GetSimulation()->CityLayout();
                Run->EditPoint = FVector((City.ExtentX() + 30) * 100 + 100,
                                         (City.ExtentY() + 30) * 100 + 100, 0.0);
                Run->InitialHeight = Terrain->HeightAtCm(static_cast<float>(Run->EditPoint.X), static_cast<float>(Run->EditPoint.Y));
                Run->TerrainVersion = Status.TerrainVersion;
                Sim->ModifyTerrain(static_cast<float>(Run->EditPoint.X), static_cast<float>(Run->EditPoint.Y), 600.0f, -160.0f);
                Advance(Now);
            }
            return;
        case 9:
            {
                if (Status.TerrainVersion <= Run->TerrainVersion) return;
                const double CacheHeight = Terrain->HeightAtCm(static_cast<float>(Run->EditPoint.X), static_cast<float>(Run->EditPoint.Y));
                double MeshHeight = 0.0;
                if (CacheHeight > Run->InitialHeight - 100.0 || !FindMeshHeight(Terrain, Run->EditPoint, MeshHeight) ||
                    FMath::Abs(MeshHeight - CacheHeight) > 1.0) return;
                FHitResult Hit;
                FCollisionQueryParams Query(SCENE_QUERY_STAT(LivingCityTerrainAudit), true, Pawn);
                if (!World->LineTraceSingleByChannel(Hit, Run->EditPoint + FVector(0.0, 0.0, 10000.0),
                    Run->EditPoint - FVector(0.0, 0.0, 10000.0), ECC_Visibility, Query) ||
                    Hit.GetActor() != Terrain || FMath::Abs(Hit.ImpactPoint.Z - MeshHeight) > 3.0) return;
                Pass(TEXT("TERRAIN_COMMAND"), FString::Printf(TEXT("before_version=%lld after_version=%lld delta_cm=%.1f"),
                    Run->TerrainVersion, Status.TerrainVersion, CacheHeight - Run->InitialHeight));
                Pass(TEXT("TERRAIN_RENDER_AND_COLLISION"), FString::Printf(TEXT("cached_height_cm=%.1f mesh_height_cm=%.1f collision_height_cm=%.1f"),
                    CacheHeight, MeshHeight, Hit.ImpactPoint.Z));
                const FVector CameraPoint = Run->EditPoint - FVector(1150.0, 0.0, 0.0);
                Pawn->SetActorLocation(CameraPoint + FVector(0.0, 0.0, Terrain->HeightAtCm(static_cast<float>(CameraPoint.X), static_cast<float>(CameraPoint.Y)) + 150.0),
                    false, nullptr, ETeleportType::TeleportPhysics);
                Pawn->GetCharacterMovement()->StopMovementImmediately();
                Pawn->SetFirstPerson(true);
                if (APlayerController* Controller = Cast<APlayerController>(Pawn->GetController()))
                    Controller->SetControlRotation(FRotator(-13.0, 0.0, 0.0));
                Advance(Now);
            }
            return;
        case 10:
            if (Elapsed < 2.0) return;
            Capture(TEXT("LivingCityTerrainEdit"));
            Advance(Now);
            return;
        case 11:
            if (Elapsed < 2.0) return;
            Run->bFinished = true;
            UE_LOG(LogTemp, Display, TEXT("LIVINGCITY AUDIT PERFORMANCE frames=%d mean_frame_ms=%.3f worst_frame_ms=%.3f last_sim_tick_ms=%.3f"),
                Run->Frames, Run->Frames > 0 ? Run->FrameSeconds * 1000.0 / Run->Frames : 0.0,
                Run->MaxFrameSeconds * 1000.0, Sim->GetLastTickMilliseconds());
            UE_LOG(LogTemp, Display, TEXT("LIVINGCITY AUDIT COMPLETE PASS checks=%d"), Run->Checks);
            FPlatformMisc::RequestExit(false);
            return;
        default:
            Fail(TEXT("INVALID_STAGE"));
            return;
        }
    }

    struct FRegistration
    {
        FDelegateHandle Handle;
        FRegistration() { Handle = FWorldDelegates::OnWorldPostActorTick.AddStatic(&Tick); }
        ~FRegistration() { FWorldDelegates::OnWorldPostActorTick.Remove(Handle); }
    };
    static FRegistration Registration;
}
