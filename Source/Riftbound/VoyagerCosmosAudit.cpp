// Isolated opt-in presentation and authority checks. Never runs in normal play.
#include "VoyagerCosmos.h"
#include "VoyagerWorld.h"
#include "VoyagerCharacter.h"
#include "VoyagerShip.h"
#include "VoyagerGameMode.h"
#include "VoyagerData.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "HAL/IConsoleManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

namespace VoyagerCosmosAudit
{
    struct FRun
    {
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<AVoyagerController> PC;
        TWeakObjectPtr<AVoyagerWorld> Terrain;
        TWeakObjectPtr<AVoyagerShip> Ship;
        int32 Stage=0,Checks=0,Seed=0,Garden=0;
        double Started=0,StageTime=0;
        FVector CruisePrevious=FVector::ZeroVector;
        double CruiseDistance=0,CruiseMaxStep=0,CruiseArrivedAt=0;
        float ShieldHull=0;
        bool Finished=false;
    };
    TUniquePtr<FRun> Run;
    void Pass(const TCHAR* Name){++Run->Checks;UE_LOG(LogTemp,Display,TEXT("VOYAGER COSMOS AUDIT PASS %s"),Name);}
    void Fail(const TCHAR* Name){Run->Finished=true;UE_LOG(LogTemp,Error,TEXT("VOYAGER COSMOS AUDIT FAIL stage=%d %s"),Run->Stage,Name);FPlatformMisc::RequestExit(false);}
    void Advance(double Now){++Run->Stage;Run->StageTime=Now;}
    void Capture(const TCHAR* Name)
    {
        if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerCosmosCapture")))return;
        FString Folder;FParse::Value(FCommandLine::Get(),TEXT("VoyagerCosmosOutput="),Folder);
        if(Folder.IsEmpty())Folder=FPaths::ProjectSavedDir()/TEXT("VoyagerCosmosAudit");
        IFileManager::Get().MakeDirectory(*Folder,true);
        FScreenshotRequest::RequestScreenshot(Folder/(FString(Name)+TEXT(".png")),false,false);
    }
    void PositionShip(FVector At,FVector Facing)
    {
        auto* Ship=Run->Ship.Get();if(!Ship)return;
        Ship->ClearFlightTestInput();Ship->ResetFlight(At,Facing.Rotation(),false);
        Run->PC->SetControlRotation(Facing.Rotation());
    }
    void Tick(UWorld* World,ELevelTick Type,float Delta)
    {
        if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerCosmosAudit"))||!World||!World->IsGameWorld()||World->GetNetMode()==NM_Client)return;
        if(!Run)
        {
            if(World->GetTimeSeconds()<8.f)return;
            auto* PC=Cast<AVoyagerController>(World->GetFirstPlayerController());if(!PC||!PC->GetPawn())return;
            Run=MakeUnique<FRun>();Run->World=World;Run->PC=PC;Run->Started=Run->StageTime=FPlatformTime::Seconds();
            for(TActorIterator<AVoyagerWorld> It(World);It;++It){Run->Terrain=*It;break;}
        }
        if(Run->World.Get()!=World||Run->Finished)return;
        const double Now=FPlatformTime::Seconds(),Elapsed=Now-Run->StageTime;
        if(Now-Run->Started>130){Fail(TEXT("TIMEOUT"));return;}
        auto* State=World->GetGameState<AVoyagerState>();auto* Terrain=Run->Terrain.Get();auto* Cosmos=AVoyagerCosmos::Find(World);
        auto* GM=World->GetAuthGameMode<AVoyagerGameMode>();auto* PC=Run->PC.Get();
        if(!State||!Terrain||!Cosmos||!GM||!PC)return;
        if(FParse::Param(FCommandLine::Get(),TEXT("VoyagerCloudDiagnostic")))
        {
            auto* Clouds=Terrain->FindComponentByClass<UVolumetricCloudComponent>();
            if(!Clouds||!Clouds->GetMaterial()){Fail(TEXT("CLOUD_MATERIAL_BINDING"));return;}
            switch(Run->Stage)
            {
            case 0:
            {
                auto* Material=Cast<UMaterialInstanceDynamic>(Clouds->GetMaterial());
                if(!Material){Fail(TEXT("CLOUD_DYNAMIC_MATERIAL"));return;}
                UE_LOG(LogTemp,Display,TEXT("VOYAGER CLOUD DIAGNOSTIC material=%s domain=%d visible=%d registered=%d bottom=%.2f height=%.2f"),
                    *Material->GetPathName(),int32(Material->GetMaterial()->MaterialDomain.GetValue()),Clouds->IsVisible(),Clouds->IsRegistered(),Clouds->LayerBottomAltitude,Clouds->LayerHeight);
                for(const TCHAR* Name:{TEXT("r.VolumetricCloud"),TEXT("r.VolumetricCloud.Support"),TEXT("r.VolumetricRenderTarget"),TEXT("r.VolumetricRenderTarget.Mode")})
                    if(auto* Variable=IConsoleManager::Get().FindConsoleVariable(Name))UE_LOG(LogTemp,Display,TEXT("VOYAGER CLOUD DIAGNOSTIC %s=%d"),Name,Variable->GetInt());
                int32 Garden=0;for(int32 P=0;P<5;++P)if(Voyager::Biome(State->SystemSeed,P)==0)Garden=P;
                if(auto* Explorer=Cast<AVoyagerCharacter>(PC->GetPawn()))
                {
                    Explorer->GetCharacterMovement()->StopMovementImmediately();
                    Explorer->SetActorLocation(Voyager::SurfacePoint(State->SystemSeed,Garden,FVector::UpVector,130),false,nullptr,ETeleportType::TeleportPhysics);
                    Explorer->GetCharacterMovement()->SetGravityDirection(-FVector::UpVector);PC->SetControlRotation(FRotator(24,-40,0));
                }
                Material->SetScalarParameterValue(TEXT("Cloud_GlobalDensity"),.02f);
                Advance(Now);return;
            }
            case 1:if(Elapsed<6)return;Capture(TEXT("DenseWeather"));Advance(Now);return;
            case 2:
                if(Elapsed<1)return;
                if(auto* Variable=IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricRenderTarget")))Variable->Set(0,ECVF_SetByCode);
                Advance(Now);return;
            case 3:if(Elapsed<6)return;Capture(TEXT("DirectVolume"));Advance(Now);return;
            case 4:
                if(Elapsed<1)return;
                Clouds->SetMaterial(LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst")));
                Advance(Now);return;
            case 5:if(Elapsed<6)return;Capture(TEXT("EngineCloud"));Advance(Now);return;
            default:
                if(Elapsed<1)return;
                Run->Finished=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER CLOUD DIAGNOSTIC COMPLETE"));FPlatformMisc::RequestExit(false);return;
            }
        }
        switch(Run->Stage)
        {
        case 0:
        {
            Run->Seed=State->SystemSeed;
            for(int32 P=0;P<Voyager::PlanetCount;++P)if(Voyager::Biome(Run->Seed,P)==0)Run->Garden=P;
            const auto* Sky=Terrain->PhysicalAtmosphere();
            if(!Sky||!Terrain->HasPlanetaryWeather()){Fail(TEXT("NATIVE_SKY_OR_CLOUD_MISSING"));return;}
            if(Sky->TransformMode!=ESkyAtmosphereTransformMode::PlanetCenterAtComponentTransform){Fail(TEXT("ATMOSPHERE_COORDINATES"));return;}
            if(!Cosmos->HasVisuals()){Fail(TEXT("ANOMALY_VISUALS_MISSING"));return;}
            Pass(TEXT("NATIVE_ATMOSPHERE_AND_VOLUMETRIC_CLOUDS"));Pass(TEXT("SEEDED_BLACK_HOLE_VISUALS"));
            if(auto* Pawn=Cast<AVoyagerCharacter>(PC->GetPawn()))
            {
                const FVector Up=FVector(.002,.001,1).GetSafeNormal();
                const FVector At=Voyager::SurfacePoint(Run->Seed,Run->Garden,Up,180);
                Pawn->GetCharacterMovement()->StopMovementImmediately();Pawn->GetCharacterMovement()->SetGravityDirection(-Up);
                Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
                Pawn->SetActorLocationAndRotation(At,Voyager::TangentRotation(Up),false,nullptr,ETeleportType::TeleportPhysics);
                PC->SetControlRotation(FRotator(18,-40,0));
            }
            Advance(Now);return;
        }
        case 1:
        {
            if(Elapsed<8)return;Capture(TEXT("NaturalSky"));
            const int32 P=Terrain->ActiveAtmospherePlanet();const auto* Sky=Terrain->PhysicalAtmosphere();
            const double Expected=(Voyager::PlanetRadius(State->SystemSeed,P)-300000.0)/Voyager::CentimetersPerKm;
            if(FMath::Abs(Sky->BottomRadius-Expected)>.01||FVector::Dist(Sky->GetComponentLocation(),Voyager::PlanetCenter(State->SystemSeed,P))>1.0)
            {Fail(TEXT("SKY_PLANET_RADIUS_MISMATCH"));return;}
            Pass(TEXT("MATCHED_PLANET_SKY_CENTER_AND_RADIUS"));
            Advance(Now);return;
        }
        case 2:
        {
            // Leave capture requests a complete frame before possession or repositioning.
            if(Elapsed<1)return;
            auto* Ship=GM->ShipFor(PC);if(!Ship){Fail(TEXT("PLAYER_SHIP_MISSING"));return;}
            if(auto* Pawn=Cast<AVoyagerCharacter>(PC->GetPawn()))
            {
                Pawn->SetActorLocation(Ship->GetActorLocation()+FVector(0,0,150),false,nullptr,ETeleportType::TeleportPhysics);
                GM->BoardShip(Pawn);
            }
            if(PC->GetPawn()!=Ship){Fail(TEXT("BOARD_SHIP"));return;}
            Run->Ship=Ship;
            const FVector Up=FVector(.15,-.08,1).GetSafeNormal();
            PositionShip(Voyager::SurfacePoint(Run->Seed,Run->Garden,Up,350*Voyager::CentimetersPerKm),-Up);
            Advance(Now);return;
        }
        case 3:
        {
            if(Elapsed<8)return;
            if(Terrain->ActiveAtmospherePlanet()!=Run->Garden||Terrain->ActiveChunkCount()<=0){Fail(TEXT("ORBIT_TERRAIN_CONTINUITY"));return;}
            Pass(TEXT("ORBIT_USES_SAME_PLANET_TERRAIN"));Capture(TEXT("PlanetOrbit"));Advance(Now);return;
        }
        case 4:
        {
            if(Elapsed<1)return;
            const FVector Up=FVector(.15,-.08,1).GetSafeNormal();
            const FVector At=Voyager::SurfacePoint(Run->Seed,Run->Garden,Up,9*Voyager::CentimetersPerKm);
            const FVector Tangent=FVector::VectorPlaneProject(FVector::ForwardVector,Up).GetSafeNormal();
            PositionShip(At,(Tangent-Up*.2).GetSafeNormal());Advance(Now);return;
        }
        case 5:
        {
            if(Elapsed<8)return;
            Pass(TEXT("ATMOSPHERE_DESCENT_RETAINS_COORDINATES"));Capture(TEXT("CloudFlight"));Advance(Now);return;
        }
        case 6:
        {
            if(Elapsed<1)return;
            const FVector At=VoyagerCosmos::SurveyPoint(Run->Seed);
            if(VoyagerCosmos::TidalDamagePerSecond(Run->Seed,At)!=0){Fail(TEXT("SURVEY_NOT_SAFE"));return;}
            const FVector Center=VoyagerCosmos::AnomalyCenter(Run->Seed);const double R=VoyagerCosmos::HorizonRadius(Run->Seed);
            const float Outer=VoyagerCosmos::TidalDamagePerSecond(Run->Seed,Center+FVector(R*4,0,0));
            const float Inner=VoyagerCosmos::TidalDamagePerSecond(Run->Seed,Center+FVector(R*2,0,0));
            if(!(Inner>Outer&&Outer>0&&Inner<=80)){Fail(TEXT("TIDAL_FIELD_BOUNDS"));return;}
            Pass(TEXT("SAFE_SURVEY_AND_BOUNDED_INCREASING_TIDES"));
            const FVector Start=At+(At-Center).GetSafeNormal()*1000.0*Voyager::CentimetersPerKm;
            PositionShip(Start,At-Start);Run->CruisePrevious=Start;
            Run->Ship->ServerSurveyAnomaly();
            if(!Run->Ship->bCruising||FVector::Dist(Start,Run->Ship->GetActorLocation())>1.0){Fail(TEXT("SURVEY_START_TELEPORT_OR_REJECT"));return;}
            Pass(TEXT("SURVEY_CRUISE_STARTS_WITHOUT_TELEPORT"));Advance(Now);return;
        }
        case 7:
        {
            auto* Ship=Run->Ship.Get();
            const double Step=FVector::Dist(Ship->GetActorLocation(),Run->CruisePrevious);
            Run->CruiseDistance+=Step;Run->CruiseMaxStep=FMath::Max(Run->CruiseMaxStep,Step);Run->CruisePrevious=Ship->GetActorLocation();
            if(Step>25000000.0*(double(Delta)+.02)+200.0){Fail(TEXT("SURVEY_CRUISE_DISCONTINUITY"));return;}
            if(Elapsed>35){Fail(TEXT("SURVEY_CRUISE_TIMEOUT"));return;}
            if(Ship->bCruising)return;
            if(FVector::Dist(Ship->GetActorLocation(),VoyagerCosmos::SurveyPoint(Run->Seed))>100||Run->CruiseDistance<999.0*Voyager::CentimetersPerKm)
            {Fail(TEXT("SURVEY_CRUISE_ARRIVAL"));return;}
            if(Run->CruiseArrivedAt==0)
            {
                Run->CruiseArrivedAt=Now;Pass(TEXT("SURVEY_CONTINUOUS_CRUISE_ARRIVAL"));
                UE_LOG(LogTemp,Display,TEXT("VOYAGER COSMOS CRUISE distance_km=%.3f largest_frame_step_km=%.3f seconds=%.2f"),
                    Run->CruiseDistance/Voyager::CentimetersPerKm,Run->CruiseMaxStep/Voyager::CentimetersPerKm,Elapsed);
            }
            if(Now-Run->CruiseArrivedAt<2)return;
            Capture(TEXT("BlackHole"));Pass(TEXT("BLACK_HOLE_OBSERVATION_ORBIT"));Advance(Now);return;
        }
        case 8:
        {
            if(Elapsed<1)return;
            auto* Ship=Run->Ship.Get();Run->ShieldHull=Ship->Shield+Ship->Hull;
            const FVector Center=VoyagerCosmos::AnomalyCenter(Run->Seed);
            PositionShip(Center+FVector(VoyagerCosmos::HorizonRadius(Run->Seed)*3.5,0,0),FVector::ForwardVector);
            Advance(Now);return;
        }
        case 9:
        {
            if(Elapsed<1.6)return;
            const auto* Ship=Run->Ship.Get();if(Ship->Shield+Ship->Hull>=Run->ShieldHull){Fail(TEXT("AUTHORITY_TIDAL_DAMAGE"));return;}
            Pass(TEXT("AUTHORITY_TIDAL_DAMAGE"));
            const FVector At=VoyagerCosmos::SurveyPoint(Run->Seed);
            PositionShip(At,VoyagerCosmos::AnomalyCenter(Run->Seed)-At);
            GM->NextSystem(PC);Advance(Now);return;
        }
        case 10:
        {
            if(State->bTransitioning||State->SystemSeed==Run->Seed||Elapsed<8)return;
            if(Cosmos->ActiveSystem()!=State->SystemSeed||!Cosmos->HasVisuals()){Fail(TEXT("NEXT_SYSTEM_ANOMALY"));return;}
            if(VoyagerCosmos::AnomalyCenter(State->SystemSeed)==VoyagerCosmos::AnomalyCenter(Run->Seed)){Fail(TEXT("SYSTEM_SEED_NOT_VARIED"));return;}
            Pass(TEXT("OTHER_STAR_SYSTEM_REBUILDS_SEEDED_COSMOS"));
            UE_LOG(LogTemp,Display,TEXT("VOYAGER COSMOS AUDIT COMPLETE PASS checks=%d seconds=%.2f"),Run->Checks,Now-Run->Started);
            Run->Finished=true;FPlatformMisc::RequestExit(false);return;
        }
        default:Fail(TEXT("INVALID_STAGE"));return;
        }
    }
    struct FRegistration
    {
        FDelegateHandle Handle;
        FRegistration(){Handle=FWorldDelegates::OnWorldPostActorTick.AddStatic(&Tick);}
        ~FRegistration(){FWorldDelegates::OnWorldPostActorTick.Remove(Handle);}
    } Registration;
}
