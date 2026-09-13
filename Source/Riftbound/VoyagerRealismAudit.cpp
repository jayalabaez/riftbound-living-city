// Opt-in integration audit. Normal play registers a delegate but never allocates
// a run, changes a pawn, submits model requests, or captures the user's screen.
#include "VoyagerCharacter.h"
#include "VoyagerCitizen.h"
#include "VoyagerData.h"
#include "VoyagerGameMode.h"
#include "VoyagerLocalAI.h"
#include "VoyagerSettlement.h"
#include "VoyagerVegetation.h"
#include "VoyagerWorld.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeCounter.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Misc/CommandLine.h"
#include "Misc/Crc.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "ProceduralMeshComponent.h"
#include "UnrealClient.h"

namespace VoyagerRealismAudit
{
    struct FDialogueObserver : FOutputDevice
    {
        FThreadSafeCounter Applied, Cached, Fallback;
        void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
        {
            if (FCString::Strstr(Text, TEXT("VOYAGER LOCAL AI APPLIED "))) Applied.Increment();
            if (FCString::Strstr(Text, TEXT("VOYAGER LOCAL AI CACHE_APPLIED "))) Cached.Increment();
            if (FCString::Strstr(Text, TEXT("VOYAGER LOCAL AI AUTHORED_FALLBACK "))) Fallback.Increment();
        }
    };
    struct FRun
    {
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<AVoyagerController> Controller;
        TWeakObjectPtr<AVoyagerCharacter> Explorer;
        TWeakObjectPtr<AVoyagerVegetation> Nature;
        TWeakObjectPtr<AVoyagerWorld> Terrain;
        TWeakObjectPtr<AVoyagerCitizen> Citizen;
        TUniquePtr<FDialogueObserver> DialogueLog;
        FVector ForestUp = FVector::UpVector, ForestPoint = FVector::ZeroVector;
        FVector WalkStart = FVector::ZeroVector, WalkDirection = FVector::ForwardVector;
        FString AuthoredLine, ModelLine;
        double Started = 0, StageStarted = 0;
        int32 Stage = 0, Checks = 0, Garden = 0, Remote = 0;
        int32 BaselineInstances = 0, AIAppliedBefore = 0, AICachedBefore = 0, AIFallbackBefore = 0, PriorAIValue = 1;
        uint32 BaselineSignature = 0;
        bool bInitialBodycam = true, bFinished = false, bAI = false, bLightingCompare = false;
        ~FRun() { if (GLog && DialogueLog) GLog->RemoveOutputDevice(DialogueLog.Get()); }
    };
    TUniquePtr<FRun> Run;

    void Pass(const TCHAR* Check, const FString& Detail = FString())
    {
        ++Run->Checks;
        UE_LOG(LogTemp, Display, TEXT("VOYAGER REALISM AUDIT PASS %s %s"), Check, *Detail);
    }
    void Fail(const TCHAR* Reason)
    {
        Run->bFinished = true;
        if (auto* Controller = Run->Controller.Get()) UVoyagerLocalAI::CancelConversation(Controller);
        UE_LOG(LogTemp, Error, TEXT("VOYAGER REALISM AUDIT FAIL stage=%d reason=%s"), Run->Stage, Reason);
        FPlatformMisc::RequestExit(false);
    }
    void Advance(double Now) { ++Run->Stage; Run->StageStarted = Now; }
    void Capture(const TCHAR* Name)
    {
        if (!FParse::Param(FCommandLine::Get(), TEXT("VoyagerRealismCapture"))) return;
        FString Directory;
        if (!FParse::Value(FCommandLine::Get(), TEXT("VoyagerRealismOutput="), Directory))
            Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots/VoyagerRealismAudit"));
        IFileManager::Get().MakeDirectory(*Directory, true);
        FScreenshotRequest::RequestScreenshot(Directory / (FString(Name) + TEXT(".png")), false, false);
        UE_LOG(LogTemp, Display, TEXT("VOYAGER REALISM AUDIT CAPTURE %s"), Name);
    }
    void Place(const FVector& Position, const FVector& Up, const FVector& Forward, bool bWalking = true)
    {
        auto* Explorer = Run->Explorer.Get();
        auto* Controller = Run->Controller.Get();
        const FRotator Facing = Voyager::TangentRotation(Up, Forward);
        Explorer->GetCharacterMovement()->StopMovementImmediately();
        Explorer->SetActorLocationAndRotation(Position, Facing, false, nullptr, ETeleportType::TeleportPhysics);
        Explorer->GetCharacterMovement()->SetGravityDirection(-Up);
        Explorer->GetCharacterMovement()->SetMovementMode(bWalking ? MOVE_Falling : MOVE_Flying);
        Controller->SetControlRotation(Facing);
        if (auto* Nature = Run->Nature.Get()) Nature->RefreshNow();
    }
    bool ApplyLightingVariant(int32 Variant)
    {
        auto* Terrain = Run->Terrain.Get();
        UDirectionalLightComponent* Sun = nullptr;
        TInlineComponentArray<UDirectionalLightComponent*> Lights(Terrain);
        for (auto* Light : Lights) if (Light->IsUsedAsAtmosphereSunLight()) Sun = Light;
        if (!Sun) return false;
        switch (Variant)
        {
        case 1:
        {
            TInlineComponentArray<USkyLightComponent*> Skylights(Terrain);
            if (Skylights.Num() != 1) return false;
            const FVector Before = Skylights[0]->GetComponentLocation();
            const FVector Eye = Run->Explorer->Camera->GetComponentLocation();
            Skylights[0]->SetWorldLocation(Eye);
            Skylights[0]->RecaptureSky();
            UE_LOG(LogTemp, Display, TEXT("VOYAGER LIGHTING COMPARE skylight_before=%s skylight_after=%s"), *Before.ToString(), *Eye.ToString());
            break;
        }
        case 2: Sun->bCastCloudShadows = false; Sun->MarkRenderStateDirty(); break;
        case 3:
        {
            TInlineComponentArray<UVolumetricCloudComponent*> Clouds(Terrain);
            if (Clouds.IsEmpty()) return false;
            for (auto* Cloud : Clouds) Cloud->SetVisibility(false);
            break;
        }
        case 4: Sun->SetCastShadows(false); break;
        case 5:
        {
            Sun->SetIntensity(100.f);
            TInlineComponentArray<UPostProcessComponent*> PostProcesses(Terrain);
            if (PostProcesses.IsEmpty()) return false;
            for (auto* Post : PostProcesses)
            {
                Post->Settings.bOverride_AutoExposureBias = true;
                Post->Settings.AutoExposureBias = 1.f;
            }
            break;
        }
        case 6: Sun->bPerPixelAtmosphereTransmittance = false; Sun->MarkRenderStateDirty(); break;
        default: return false;
        }
        UE_LOG(LogTemp, Display, TEXT("VOYAGER LIGHTING COMPARE cumulative_variant=%d sun_intensity=%.3f cast_shadows=%d cloud_shadows=%d"),
            Variant, Sun->Intensity, Sun->CastShadows, Sun->bCastCloudShadows);
        return true;
    }
    uint32 NatureSignature(AVoyagerVegetation* Nature)
    {
        TArray<FString> Samples;
        TInlineComponentArray<UHierarchicalInstancedStaticMeshComponent*> Components(Nature);
        for (auto* Component : Components)
        {
            if (!Component->GetStaticMesh()) continue;
            for (int32 I = 0; I < FMath::Min(16, Component->GetInstanceCount()); ++I)
            {
                FTransform Transform;
                if (!Component->GetInstanceTransform(I, Transform, true)) continue;
                const FVector Position = Transform.GetLocation();
                Samples.Add(FString::Printf(TEXT("%s:%.2f:%.2f:%.2f:%.3f"), *Component->GetStaticMesh()->GetName(),
                    Position.X, Position.Y, Position.Z, Transform.GetScale3D().Z));
            }
        }
        Samples.Sort();
        return FCrc::StrCrc32(*FString::Join(Samples, TEXT("|")));
    }
    bool CheckAssets()
    {
        const TCHAR* Names[] = {TEXT("SM_Broadleaf"), TEXT("SM_Conifer"), TEXT("SM_DryShrub"), TEXT("SM_Grass"), TEXT("SM_Fern"), TEXT("SM_Boulder")};
        int32 Materials = 0;
        for (const TCHAR* Name : Names)
        {
            auto* Mesh = LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/Nature/Meshes/%s.%s"), Name, Name));
            if (!Mesh || Mesh->GetNumLODs() != 3 || Mesh->GetStaticMaterials().IsEmpty()) return false;
            for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
            {
                if (!Slot.MaterialInterface || !Slot.MaterialInterface->GetPathName().StartsWith(TEXT("/Game/Nature/")) ||
                    !Slot.MaterialInterface->GetName().StartsWith(TEXT("M_Nature"))) return false;
                ++Materials;
            }
        }
        Pass(TEXT("IMPORTED_ASSETS"), FString::Printf(TEXT("meshes=6 lods_each=3 nature_material_slots=%d"), Materials));
        return true;
    }
    bool CheckTerrainAndLight(AVoyagerWorld* Terrain)
    {
        int32 CheckedPatches = 0;
        TInlineComponentArray<UProceduralMeshComponent*> Patches(Terrain);
        for (auto* Patch : Patches)
        {
            if (!Patch->GetName().StartsWith(TEXT("SpherePatch_"))) continue;
            UMaterialInterface* Material = Patch->GetMaterial(0);
            float Radius = 0, NormalStrength = 0, Snow = 0;
            FLinearColor Center, Tint;
            if (!Material || !Material->GetMaterial()->GetPathName().Contains(TEXT("M_NatureTerrain")) ||
                !Material->GetScalarParameterValue(FMaterialParameterInfo(TEXT("PlanetRadius")), Radius) || Radius < 1000000.f ||
                !Material->GetScalarParameterValue(FMaterialParameterInfo(TEXT("NormalStrength")), NormalStrength) || NormalStrength <= 0.f ||
                !Material->GetScalarParameterValue(FMaterialParameterInfo(TEXT("SnowCoverage")), Snow) ||
                !Material->GetVectorParameterValue(FMaterialParameterInfo(TEXT("PlanetCenter")), Center) ||
                !Material->GetVectorParameterValue(FMaterialParameterInfo(TEXT("LandTint")), Tint)) return false;
            const auto& PrimitiveData = Patch->GetCustomPrimitiveData().Data;
            if (PrimitiveData.Num() < 3) return false;
            for (int32 I = 0; I < 3; ++I)
                if (!FMath::IsFinite(PrimitiveData[I]) || PrimitiveData[I] < 0.f || PrimitiveData[I] >= 1.f) return false;
            ++CheckedPatches;
        }
        if (CheckedPatches < 6) return false;
        Pass(TEXT("NATURAL_TERRAIN_MATERIAL"), FString::Printf(TEXT("triplanar_origin_patches=%d normal_strength_parameter=true"), CheckedPatches));
        int32 Suns = 0;
        TInlineComponentArray<UDirectionalLightComponent*> Lights(Terrain);
        for (auto* Light : Lights)
        {
            if (!Light->IsUsedAsAtmosphereSunLight()) continue;
            if (Light->Mobility != EComponentMobility::Movable || !Light->CastShadows || Light->GetAtmosphereSunLightIndex() != 0) return false;
            ++Suns;
        }
        if (Suns != 1) return false;
        Pass(TEXT("PRIMARY_ATMOSPHERE_SUN"), TEXT("count=1 movable=true shadows=true"));
        return true;
    }
    bool CheckObstacles(UWorld* World, AVoyagerVegetation* Nature, AVoyagerCharacter* Explorer)
    {
        TInlineComponentArray<UCapsuleComponent*> Capsules(Nature);
        if (Capsules.IsEmpty() || Capsules.Num() > 64) return false;
        for (auto* Capsule : Capsules)
            if (Capsule->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block || !Capsule->IsQueryCollisionEnabled() ||
                Capsule->CanCharacterStepUpOn != ECB_No) return false;
        auto* Sample = Capsules[0];
        const FVector Across = Sample->GetForwardVector() * (Sample->GetScaledCapsuleRadius() + 180.f);
        FHitResult Hit;
        FCollisionQueryParams Query(SCENE_QUERY_STAT(VoyagerNatureObstacleAudit), false, Explorer);
        const bool bBlocked = World->SweepSingleByChannel(Hit, Sample->GetComponentLocation() - Across,
            Sample->GetComponentLocation() + Across, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(20.f), Query);
        if (!bBlocked || Hit.GetComponent() != Sample) return false;
        Pass(TEXT("NATURAL_OBSTACLE_COLLISION"), FString::Printf(TEXT("capsules=%d pawn_sweep_blocked=true"), Capsules.Num()));
        return true;
    }

    void Tick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
    {
        static const bool bAudit = FParse::Param(FCommandLine::Get(), TEXT("VoyagerRealismAudit"));
        if (!bAudit || !World || !World->IsGameWorld() || !World->HasBegunPlay() || World->GetNetMode() != NM_Standalone) return;
        const double Now = FPlatformTime::Seconds();
        if (!Run)
        {
            Run = MakeUnique<FRun>(); Run->World = World; Run->Started = Run->StageStarted = Now;
            Run->bLightingCompare = FParse::Param(FCommandLine::Get(), TEXT("VoyagerLightingCompare"));
            Run->bAI = !Run->bLightingCompare && FParse::Param(FCommandLine::Get(), TEXT("VoyagerAITest"));
            if (Run->bAI && GLog) { Run->DialogueLog = MakeUnique<FDialogueObserver>(); GLog->AddOutputDevice(Run->DialogueLog.Get()); }
            UE_LOG(LogTemp, Display, TEXT("VOYAGER REALISM AUDIT START ai=%d lighting_compare=%d altitude_checks=teleport_streaming_fixtures"), Run->bAI, Run->bLightingCompare);
        }
        if (Run->bFinished || Run->World.Get() != World) return;
        if (Now - Run->Started > 135.0) { Fail(TEXT("GLOBAL_TIMEOUT")); return; }
        const double Elapsed = Now - Run->StageStarted;
        auto* State = World->GetGameState<AVoyagerState>();
        auto* Controller = Cast<AVoyagerController>(World->GetFirstPlayerController());
        auto* Explorer = Controller ? Cast<AVoyagerCharacter>(Controller->GetPawn()) : nullptr;
        if (!State || !Controller || !Explorer || State->bTransitioning)
        { if (Elapsed > 15.0) Fail(TEXT("VOYAGER_LOCAL_PLAYER_NOT_READY")); return; }
        Run->Controller = Controller; Run->Explorer = Explorer;

        switch (Run->Stage)
        {
        case 0:
        {
            if (Elapsed < 2.0) return;
            if (State->SystemSeed != 1) { Fail(TEXT("EXPECTED_ISOLATED_SEED_ONE")); return; }
            if (!CheckAssets()) { Fail(TEXT("MISSING_NATURE_ASSETS_LODS_OR_MATERIALS")); return; }
            for (TActorIterator<AVoyagerVegetation> It(World); It; ++It) { Run->Nature = *It; break; }
            for (TActorIterator<AVoyagerWorld> It(World); It; ++It) { Run->Terrain = *It; break; }
            if (!Run->Nature.IsValid() || !Run->Terrain.IsValid()) { Fail(TEXT("NATURE_WORLD_ACTOR_MISSING")); return; }
            for (int32 P = 0; P < Voyager::PlanetCount; ++P) if (Voyager::Biome(State->SystemSeed, P) == 0) Run->Garden = P;
            Run->Remote = (Run->Garden + 1) % Voyager::PlanetCount;
            Run->ForestUp = FVector(-14000, -10000, Voyager::PlanetRadius(State->SystemSeed, Run->Garden)).GetSafeNormal();
            Run->ForestPoint = Voyager::SurfacePoint(State->SystemSeed, Run->Garden, Run->ForestUp, 120);
            Place(Run->ForestPoint, Run->ForestUp, FVector::ForwardVector);
            Advance(Now); return;
        }
        case 1:
        {
            auto* Nature = Run->Nature.Get();
            if (Elapsed < (Run->bLightingCompare ? 6.0 : 12.0)) return;
            if (Nature->ActiveCellCount() < 8 || Nature->ActiveCellCount() > 160 || Nature->ActiveTreeCount() < 4 || Nature->ActiveGrassCount() < 30)
            { Fail(TEXT("GROUND_VEGETATION_NOT_STREAMED")); return; }
            Pass(TEXT("GROUND_VEGETATION"), FString::Printf(TEXT("cells=%d trees=%d grass=%d instances=%d"),
                Nature->ActiveCellCount(), Nature->ActiveTreeCount(), Nature->ActiveGrassCount(), Nature->ActiveInstanceCount()));
            if (!CheckObstacles(World, Nature, Explorer)) { Fail(TEXT("NATURAL_OBSTACLE_COLLISION")); return; }
            if (!CheckTerrainAndLight(Run->Terrain.Get())) { Fail(TEXT("TERRAIN_OR_LIGHT_CONTRACT")); return; }
            Run->bInitialBodycam = Explorer->IsBodycamEnabled();
            if (Run->bLightingCompare) { Run->Stage = 3; Run->StageStarted = Now; return; }
            if (Explorer->IsBodycamEnabled()) Explorer->ToggleBodycam();
            Run->WalkStart = Explorer->GetActorLocation();
            Run->WalkDirection = Voyager::TangentRotation(Run->ForestUp).Vector();
            Advance(Now); return;
        }
        case 2:
        {
            if (Elapsed < 1.6) { Explorer->AddMovementInput(Run->WalkDirection, 1.f); return; }
            const double Moved = FVector::VectorPlaneProject(Explorer->GetActorLocation() - Run->WalkStart, Run->ForestUp).Size();
            const double Height = Voyager::SurfaceAltitude(State->SystemSeed, Run->Garden, Explorer->GetActorLocation());
            if (Moved < 150.0 || Height < 40.0 || Height > 220.0 || !Explorer->GetCharacterMovement()->IsMovingOnGround())
            { Fail(TEXT("GROUNDED_WALK_INPUT")); return; }
            Pass(TEXT("GROUNDED_WALK"), FString::Printf(TEXT("moved_cm=%.1f height_cm=%.1f"), Moved, Height));
            if (Explorer->IsBodycamEnabled() || FMath::Abs(Explorer->Camera->FieldOfView - 90.f) > 1.f)
            { Fail(TEXT("BODYCAM_STEADY_TOGGLE")); return; }
            Explorer->ToggleBodycam();
            Place(Run->ForestPoint, Run->ForestUp, FVector::ForwardVector);
            Advance(Now); return;
        }
        case 3:
            if (Elapsed < (Run->bLightingCompare ? 2.0 : 12.0)) return;
            if (Run->bLightingCompare)
            {
                Capture(TEXT("Lighting00Baseline"));
                Run->Stage = 20; Run->StageStarted = Now; return;
            }
            if (!Explorer->IsBodycamEnabled() || FMath::Abs(Explorer->Camera->FieldOfView - 100.f) > 1.f)
            { Fail(TEXT("BODYCAM_FIELD_TOGGLE")); return; }
            Pass(TEXT("BODYCAM_TOGGLE"), TEXT("steady_fov=90 field_fov=100 possession_unchanged=true"));
            Run->BaselineInstances = Run->Nature->ActiveInstanceCount();
            Run->BaselineSignature = NatureSignature(Run->Nature.Get());
            Capture(TEXT("NatureGround"));
            Advance(Now); return;
        case 4:
            if (Elapsed < 1.5) return;
            Place(Voyager::SurfacePoint(State->SystemSeed, Run->Garden, Run->ForestUp, 15.0 * Voyager::CentimetersPerKm),
                Run->ForestUp, FVector::ForwardVector, false);
            Controller->SetControlRotation(FRotationMatrix::MakeFromX(Explorer->GetActorForwardVector() - Run->ForestUp * .3).Rotator());
            Advance(Now); return;
        case 5:
            if (Elapsed < 3.0) return;
            if (Run->Nature->ActiveCellCount() != 0 || Run->Nature->ActiveCollisionCount() != 0)
            { if (Elapsed > 8.0) Fail(TEXT("ALTITUDE_DID_NOT_UNLOAD_NATURE")); return; }
            Pass(TEXT("ALTITUDE_UNLOAD"), TEXT("fixture_altitude_km=15 cells=0 collisions=0"));
            Capture(TEXT("Nature15kmFixture"));
            Advance(Now); return;
        case 6:
            if (Elapsed < 1.5) return;
            Place(Voyager::SurfacePoint(State->SystemSeed, Run->Garden, Run->ForestUp, 65.0 * Voyager::CentimetersPerKm),
                Run->ForestUp, FVector::ForwardVector, false);
            Controller->SetControlRotation(FRotationMatrix::MakeFromX(Explorer->GetActorForwardVector() - Run->ForestUp * .6).Rotator());
            Advance(Now); return;
        case 7:
        {
            if (Elapsed < 3.0) return;
            TInlineComponentArray<USkyAtmosphereComponent*> Atmospheres(Run->Terrain.Get());
            if (Atmospheres.Num() != 1 || FVector::Dist(Atmospheres[0]->GetComponentLocation(), Voyager::PlanetCenter(State->SystemSeed, Run->Garden)) > 10.0)
            { Fail(TEXT("ATMOSPHERE_PLANET_CENTER")); return; }
            // These UE properties are absolute extinction/scattering coefficients
            // per kilometre, not multipliers. Treating 1.0 as neutral previously
            // produced a yellow opaque sky despite all geometry checks passing.
            const float GroundOffsetKm=float(Voyager::PlanetRadius(State->SystemSeed,Run->Garden)/Voyager::CentimetersPerKm)-Atmospheres[0]->BottomRadius;
            const float Rayleigh = Atmospheres[0]->RayleighScatteringScale*FMath::Exp(-GroundOffsetKm/Atmospheres[0]->RayleighExponentialDistribution);
            const float Mie = Atmospheres[0]->MieScatteringScale*FMath::Exp(-GroundOffsetKm/Atmospheres[0]->MieExponentialDistribution);
            const float Absorption = Atmospheres[0]->MieAbsorptionScale*FMath::Exp(-GroundOffsetKm/Atmospheres[0]->MieExponentialDistribution);
            if (!(Rayleigh > 0.f && Rayleigh < .1f && Mie >= 0.f && Mie < .03f && Absorption >= 0.f && Absorption < .02f))
            { Fail(TEXT("ATMOSPHERE_COEFFICIENT_UNITS_OR_DENSITY")); return; }
            Pass(TEXT("EXOSPHERE_FIXTURE"), FString::Printf(
                TEXT("fixture_altitude_km=65 atmosphere_center_matches=true rayleigh_per_km=%.6f mie_per_km=%.6f absorption_per_km=%.6f"),
                Rayleigh, Mie, Absorption));
            Capture(TEXT("Nature65kmFixture"));
            Advance(Now); return;
        }
        case 8:
            if (Elapsed < 1.5) return;
            Place(Voyager::SurfacePoint(State->SystemSeed, Run->Remote, FVector(.45, -.2, .87), 140),
                FVector(.45, -.2, .87).GetSafeNormal(), FVector::ForwardVector);
            Advance(Now); return;
        case 9:
        {
            if (Elapsed < 12.0) return;
            const FVector Up = Voyager::SurfaceNormal(State->SystemSeed, Run->Remote, Explorer->GetActorLocation());
            const double Height = Voyager::SurfaceAltitude(State->SystemSeed, Run->Remote, Explorer->GetActorLocation());
            const double Alignment = FVector::DotProduct(-Explorer->GetCharacterMovement()->GetGravityDirection(), Up);
            if (Voyager::NearestPlanet(State->SystemSeed, Explorer->GetActorLocation()) != Run->Remote ||
                Run->Nature->ActiveInstanceCount() < 5 || Alignment < .999 || Height < 35.0 || Height > 240.0 ||
                !Explorer->GetCharacterMovement()->IsMovingOnGround()) { Fail(TEXT("REMOTE_RADIAL_GROUND_AND_STREAMING")); return; }
            Pass(TEXT("REMOTE_RADIAL_STREAMING"), FString::Printf(TEXT("planet=%d biome=%d instances=%d gravity_alignment=%.6f height_cm=%.1f"),
                Run->Remote, Voyager::Biome(State->SystemSeed, Run->Remote), Run->Nature->ActiveInstanceCount(), Alignment, Height));
            Capture(TEXT("NatureRemoteGround"));
            Advance(Now); return;
        }
        case 10:
            if (Elapsed < 1.5) return;
            Place(Run->ForestPoint, Run->ForestUp, FVector::ForwardVector);
            Advance(Now); return;
        case 11:
            if (Elapsed < 12.0) return;
            if (Run->Nature->ActiveInstanceCount() != Run->BaselineInstances || NatureSignature(Run->Nature.Get()) != Run->BaselineSignature)
            { Fail(TEXT("DETERMINISTIC_NATURE_RELOAD")); return; }
            Pass(TEXT("DETERMINISTIC_RELOAD"), FString::Printf(TEXT("instances=%d signature=%u"), Run->BaselineInstances, Run->BaselineSignature));
            if (!Run->bAI) { Run->Stage = 16; Run->StageStarted = Now; return; }
            Place(AVoyagerSettlement::StreetPoint(State->SystemSeed, Run->Garden, 0, 3, 3, 115),
                AVoyagerSettlement::SiteDirection(State->SystemSeed, Run->Garden), FVector::ForwardVector);
            Advance(Now); return;
        case 12:
        {
            if (Elapsed < 5.0) return;
            AVoyagerCitizen* Subject = nullptr;
            for (TActorIterator<AVoyagerCitizen> It(World); It; ++It)
                if (It->PlanetIndex() == Run->Garden && It->SiteIndex() == 0 && !It->IsIndoors() && !It->IsAlarmed())
                { Subject = *It; if (It->RoleName() == TEXT("Engineer")) break; }
            if (!Subject) { if (Elapsed > 15.0) Fail(TEXT("AI_CITIZEN_NOT_AVAILABLE")); return; }
            const FVector Up = Subject->GetActorUpVector();
            Place(Subject->GetActorLocation() + Subject->GetActorForwardVector() * 240.0 + Up * 90.0, Up, -Subject->GetActorForwardVector());
            if (Explorer->FocusedCitizen() != Subject) { if (Elapsed > 15.0) Fail(TEXT("AI_CITIZEN_FOCUS")); return; }
            Run->Citizen = Subject;
            Run->AIAppliedBefore = Run->DialogueLog->Applied.GetValue();
            Run->AICachedBefore = Run->DialogueLog->Cached.GetValue();
            Run->AIFallbackBefore = Run->DialogueLog->Fallback.GetValue();
            Explorer->ServerInteract();
            if (Controller->ConversationTarget != Subject || Controller->ConversationSpeech.IsEmpty())
            { if (Elapsed > 15.0) Fail(TEXT("AI_REAL_INTERACTION_NO_AUTHORED_REPLY")); return; }
            Run->AuthoredLine = Controller->ConversationSpeech;
            Pass(TEXT("AI_IMMEDIATE_AUTHORED_REPLY"), FString::Printf(TEXT("citizen=%s chars=%d"), *Subject->DisplayName(), Run->AuthoredLine.Len()));
            Advance(Now); return;
        }
        case 13:
            if (Controller->ConversationTarget != Run->Citizen || Controller->ConversationTime <= 0.f)
            { Fail(TEXT("AI_CONVERSATION_LOST_WHILE_WAITING")); return; }
            if (Run->DialogueLog->Fallback.GetValue() > Run->AIFallbackBefore)
            { Fail(TEXT("AI_LOCAL_MODEL_UNAVAILABLE_OR_REJECTED_REPLY")); return; }
            if (Run->DialogueLog->Applied.GetValue() <= Run->AIAppliedBefore)
            { if (Elapsed > 18.0) Fail(TEXT("AI_ASYNC_REPLY_NOT_APPLIED")); return; }
            Run->ModelLine = Controller->ConversationSpeech;
            if (Run->ModelLine.IsEmpty() || !UVoyagerLocalAI::PreservesAuthoredFacts(Run->AuthoredLine, Run->ModelLine))
            { Fail(TEXT("AI_REPLY_GROUNDING")); return; }
            Pass(TEXT("AI_ASYNC_REPLY"), FString::Printf(TEXT("held_target=true chars=%d"), Run->ModelLine.Len()));
            Explorer->ServerTalk(0);
            Advance(Now); return;
        case 14:
            if (Elapsed < .8) return;
            if (Run->DialogueLog->Cached.GetValue() <= Run->AICachedBefore || Controller->ConversationTarget != Run->Citizen || Controller->ConversationSpeech != Run->ModelLine)
            { Fail(TEXT("AI_CACHE_REPLAY")); return; }
            Pass(TEXT("AI_CACHE_REPLAY"), TEXT("same_target=true exact_cached_line=true"));
            if (IConsoleVariable* Enabled = IConsoleManager::Get().FindConsoleVariable(TEXT("voyager.LocalAI")))
            { Run->PriorAIValue = Enabled->GetInt(); Enabled->Set(0, ECVF_SetByCode); }
            Explorer->ServerTalk(0);
            Advance(Now); return;
        case 15:
            if (Elapsed < .8) return;
            if (Controller->ConversationTarget != Run->Citizen || Controller->ConversationSpeech != Run->AuthoredLine)
            { Fail(TEXT("AI_DISABLED_AUTHORED_FALLBACK")); return; }
            Pass(TEXT("AI_AUTHORED_FALLBACK"), TEXT("same_target=true exact_authored_line=true model_disabled=true"));
            if (IConsoleVariable* Enabled = IConsoleManager::Get().FindConsoleVariable(TEXT("voyager.LocalAI"))) Enabled->Set(Run->PriorAIValue, ECVF_SetByCode);
            UVoyagerLocalAI::CancelConversation(Controller);
            Explorer->ServerEndTalk();
            Advance(Now); return;
        case 16:
            if (Elapsed < 2.0) return;
            if (Explorer->IsBodycamEnabled() != Run->bInitialBodycam) Explorer->ToggleBodycam();
            Run->bFinished = true;
            UE_LOG(LogTemp, Display, TEXT("VOYAGER REALISM AUDIT COMPLETE PASS checks=%d ai=%d"), Run->Checks, Run->bAI);
            FPlatformMisc::RequestExit(false);
            return;
        case 20:
            if (Elapsed < 3.0) return;
            if (!ApplyLightingVariant(1)) { Fail(TEXT("LIGHTING_COMPARE_COMPONENTS_MISSING")); return; }
            Advance(Now); return;
        case 21:
        case 22:
        case 23:
        case 24:
        case 25:
        case 26:
        {
            if (Elapsed < 3.0) return;
            const TCHAR* Names[] = {TEXT("Lighting01SkylightAtEye"), TEXT("Lighting02NoCloudShadows"),
                TEXT("Lighting03NoClouds"), TEXT("Lighting04NoSunShadows"), TEXT("Lighting05Sun100Exposure1"), TEXT("Lighting06NoPerPixelTransmittance")};
            Capture(Names[Run->Stage - 21]);
            // Screenshot readback occurs on the next rendered frame. Delay the
            // next mutation to preserve each independently named comparison.
            if (Run->Stage == 26)
            {
                Pass(TEXT("LIGHTING_COMPARE"), TEXT("cumulative_variants=6 captures=7 visual_diagnosis_only=true"));
                Run->Stage = 16; Run->StageStarted = Now; return;
            }
            Run->Stage += 10; Run->StageStarted = Now; return;
        }
        case 31:
        case 32:
        case 33:
        case 34:
        case 35:
            if (Elapsed < .2) return;
            if (!ApplyLightingVariant(Run->Stage - 29)) { Fail(TEXT("LIGHTING_COMPARE_COMPONENTS_MISSING")); return; }
            Run->Stage -= 9; Run->StageStarted = Now; return;
        default: Fail(TEXT("INVALID_STAGE")); return;
        }
    }

    struct FRegistration
    {
        FDelegateHandle Handle;
        FRegistration() { Handle = FWorldDelegates::OnWorldPostActorTick.AddStatic(&Tick); }
        ~FRegistration() { FWorldDelegates::OnWorldPostActorTick.Remove(Handle); }
    };
    FRegistration Registration;
}
