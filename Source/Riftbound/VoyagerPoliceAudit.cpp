// Opt-in integration checks use isolated save slots and the production response/arrest paths.
#include "VoyagerPolice.h"
#include "VoyagerLaw.h"
#include "VoyagerCharacter.h"
#include "VoyagerCitizen.h"
#include "VoyagerGameMode.h"
#include "VoyagerSettlement.h"
#include "VoyagerData.h"
#include "VoyagerCityLife.h"
#include "RiftVisual.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/DamageEvents.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
namespace VoyagerPoliceAudit
{
    struct FRun
    {
        TWeakObjectPtr<UWorld> World;TWeakObjectPtr<AVoyagerController> Controller;
        TWeakObjectPtr<AVoyagerPoliceUnit> Officer,Cruiser;TWeakObjectPtr<AActor> Cover;
        FVector Street=FVector::ZeroVector,Up=FVector::UpVector,Forward=FVector::ForwardVector;
        double Started=0,StageTime=0;int32 Stage=0,StageFrames=0,Checks=0,Planet=0,Site=0,Minerals=0;
        float Health=0;bool Finished=false,PeerCustody=false,PeerGround=false,PeerWanted=false;
        TArray<uint8> CustodySave;
    };
    TUniquePtr<FRun> Run;
    void Pass(const TCHAR* Name){++Run->Checks;UE_LOG(LogTemp,Display,TEXT("VOYAGER POLICE AUDIT PASS %s"),Name);}
    void Fail(const TCHAR* Name){Run->Finished=true;UE_LOG(LogTemp,Error,TEXT("VOYAGER POLICE AUDIT FAIL stage=%d %s"),Run->Stage,Name);FPlatformMisc::RequestExit(false);}
    void Next(double Now){++Run->Stage;Run->StageTime=Now;Run->StageFrames=0;}
    bool IsCapturing(){return FParse::Param(FCommandLine::Get(),TEXT("VoyagerPoliceCapture"));}
    void Capture(const TCHAR* Name)
    {
        if(!IsCapturing())return;
        FString Folder;if(!FParse::Value(FCommandLine::Get(),TEXT("VoyagerPoliceOutput="),Folder))Folder=FPaths::ProjectSavedDir()/TEXT("VoyagerPoliceAudit");
        IFileManager::Get().MakeDirectory(*Folder,true);FScreenshotRequest::RequestScreenshot(Folder/(FString(Name)+TEXT(".png")),true,false);
    }
    bool CameraSettled(AVoyagerCharacter* Pawn)
    {
        auto Camera=Run->Controller->PlayerCameraManager;
        return Camera&&FVector::Dist(Camera->GetCameraLocation(),Pawn->GetPawnViewLocation())<40.;
    }
    bool InResponseFrame(AActor* Subject)
    {
        if(!Subject)return false;FVector2D Pixel;int32 Width=0,Height=0;
        Run->Controller->GetViewportSize(Width,Height);
        return Run->Controller->ProjectWorldLocationToScreen(Subject->GetActorLocation()+Run->Up*120,Pixel)
            &&Pixel.X>Width*.20&&Pixel.X<Width*.80&&Pixel.Y>Height*.28&&Pixel.Y<Height*.76;
    }
    void Place(AVoyagerCharacter* Pawn,FVector Position)
    {
        Pawn->GetCharacterMovement()->StopMovementImmediately();Pawn->GetCharacterMovement()->SetGravityDirection(-Run->Up);
        Pawn->SetActorLocationAndRotation(Position,Voyager::TangentRotation(Run->Up,Run->Forward),false,nullptr,ETeleportType::TeleportPhysics);
        Run->Controller->SetControlRotation(Voyager::TangentRotation(Run->Up,Run->Forward));Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
    }
    void Tick(UWorld* World,ELevelTick Type,float D)
    {
        if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerPoliceAudit"))||!World||!World->IsGameWorld())return;
        if(!Run)
        {
            if(World->GetTimeSeconds()<8)return;auto PC=Cast<AVoyagerController>(World->GetFirstPlayerController());
            if(!PC||!PC->IsLocalController()||!Cast<AVoyagerCharacter>(PC->GetPawn()))return;
            Run=MakeUnique<FRun>();Run->World=World;Run->Controller=PC;Run->Started=Run->StageTime=FPlatformTime::Seconds();
        }
        if(Run->World.Get()!=World||Run->Finished)return;
        ++Run->StageFrames;
        const double Now=FPlatformTime::Seconds(),Elapsed=Now-Run->StageTime;
        if(Now-Run->Started>145){Fail(TEXT("TIMEOUT"));return;}
        auto PC=Run->Controller.Get();auto Pawn=PC?Cast<AVoyagerCharacter>(PC->GetPawn()):nullptr;
        auto PS=PC?PC->GetPlayerState<AVoyagerPlayerState>():nullptr;auto State=World->GetGameState<AVoyagerState>();auto Law=AVoyagerLaw::Find(World);
        if(!Pawn||!PS||!State||!Law)return;
        if(World->GetNetMode()==NM_Client)
        {
            for(TActorIterator<AVoyagerPoliceUnit> It(World);It;++It)if(It->GetHealth()>0)Run->PeerGround=true;
            if(Law->IsJailed(PC)||PS->WantedStars){Fail(TEXT("INNOCENT_PEER_PUNISHED"));return;}
            for(auto Player:State->PlayerArray)if(auto Other=Cast<AVoyagerPlayerState>(Player))if(Other!=PS)
            {
                if(Other->WantedStars>0)Run->PeerWanted=true;
                if(Law->GetResidentCustody(Other).bJailed)Run->PeerCustody=true;
            }
            if(Now-Run->Started>38&&Run->PeerGround&&Run->PeerCustody&&Run->PeerWanted)
            {Pass(TEXT("REMOTE_GROUND_UNITS_REPLICATED"));Pass(TEXT("REMOTE_WANTED_REPLICATED"));Pass(TEXT("REMOTE_CUSTODY_REPLICATED"));Pass(TEXT("INNOCENT_PEER_FREE"));Run->Finished=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER POLICE AUDIT CLIENT COMPLETE PASS"));FPlatformMisc::RequestExit(false);}
            return;
        }
        switch(Run->Stage)
        {
        case 0:
        {
            AVoyagerCitizen* Citizen=nullptr;for(TActorIterator<AVoyagerCitizen> It(World);It;++It)if(It->IsAlive()){Citizen=*It;break;}if(!Citizen)return;
            Run->Planet=Citizen->PlanetIndex();Run->Site=Citizen->SiteIndex();
            // Keep the fixture on an ordinary road intersection. The central
            // plaza contains the player's parked ship and can occlude the witness.
            Run->Street=AVoyagerSettlement::StreetPoint(State->SystemSeed,Run->Planet,Run->Site,2,2,12);
            Run->Up=Voyager::SurfaceNormal(State->SystemSeed,Run->Planet,Run->Street);Run->Forward=Voyager::TangentRotation(Run->Up).Vector();
            Place(Pawn,Run->Street+Run->Up*105);Run->Minerals=PS->Minerals;
            if(Law->Surrender(PC)||Law->IsJailed(PC)){Fail(TEXT("INNOCENT_SURRENDER_ARRESTED"));return;}Pass(TEXT("INNOCENT_SURRENDER_REJECTED"));
            FActorSpawnParameters Params;Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            auto Officer=World->SpawnActor<AVoyagerPoliceUnit>(Run->Street+Run->Forward*1800,Voyager::TangentRotation(Run->Up,-Run->Forward),Params);
            if(!Officer){Fail(TEXT("OFFICER_SPAWN"));return;}Officer->Assign(PC,false,0);Run->Officer=Officer;
            Law->ReportCrime(PC,Pawn->GetActorLocation(),65,TEXT("armed assault audit fixture"),Officer);
            if(PS->WantedStars!=2)
            {
                FHitResult Block;FCollisionQueryParams Query(SCENE_QUERY_STAT(PoliceAuditWitness),false,Officer);Query.AddIgnoredActor(Pawn);
                World->LineTraceSingleByChannel(Block,Officer->GetActorLocation()+Run->Up*155,Pawn->GetPawnViewLocation(),ECC_Visibility,Query);
                UE_LOG(LogTemp,Error,TEXT("VOYAGER POLICE AUDIT witness stars=%d heat=%.1f see=%d obstacle=%s component=%s pawn=%s officer=%s"),PS->WantedStars,PS->CrimeHeat,Officer->CanSee(Pawn),*GetNameSafe(Block.GetActor()),*GetNameSafe(Block.GetComponent()),*Pawn->GetActorLocation().ToCompactString(),*Officer->GetActorLocation().ToCompactString());
                Fail(TEXT("WITNESSED_CRIME_WANTED"));return;
            }
            Pass(TEXT("WITNESSED_CRIME_WANTED"));
            Run->Health=Pawn->Health;Next(Now);return;
        }
        case 1:
            if(Elapsed<2)return;
            if(Pawn->Health<Run->Health||!Run->Officer.IsValid()||Run->Officer->ShotsFired()!=0){Fail(TEXT("WARNING_GRACE"));return;}
            Pass(TEXT("WARNING_GRACE"));Next(Now);return;
        case 2:
        {
            if(Elapsed>14){Fail(TEXT("OFFICER_REAL_WEAPON_DAMAGE"));return;}
            if(!Run->Officer.IsValid()||Run->Officer->ShotsFired()<1||Pawn->Health>=Run->Health)return;
            Pass(TEXT("OFFICER_REAL_WEAPON_DAMAGE"));
            auto Officer=Run->Officer.Get();Officer->SetActorLocation(Run->Street+Run->Forward*1800);Place(Pawn,Run->Street+Run->Up*105);
            auto Cover=World->SpawnActor<AActor>();Cover->SetRootComponent(NewObject<USceneComponent>(Cover));Cover->GetRootComponent()->RegisterComponent();
            Cover->SetActorLocationAndRotation(Run->Street+Run->Forward*900,Voyager::TangentRotation(Run->Up,Run->Forward));
            auto Wall=RiftVisual::Mesh(Cover,Cover->GetRootComponent(),TEXT("AuditOpaqueCover"),TEXT("Cube"),FVector(0,0,160),FVector(.7f,9.f,4.f),FLinearColor(.2f,.2f,.2f));
            Wall->SetCollisionEnabled(ECollisionEnabled::QueryOnly);Wall->SetCollisionResponseToAllChannels(ECR_Block);Run->Cover=Cover;
            if(Officer->CanSee(Pawn)){Fail(TEXT("OPAQUE_COVER_BLOCKS_SENSOR"));return;}Pass(TEXT("OPAQUE_COVER_BLOCKS_SENSOR"));
            Next(Now);return;
        }
        case 3:
        {
            if(Elapsed<1)return;if(Run->Cover.IsValid())Run->Cover->Destroy();
            if(!Run->Officer.IsValid()){Fail(TEXT("MISSING_OFFICER"));return;}
            if(!Run->Officer->CanSee(Pawn)){Fail(TEXT("UNCOVERED_SENSOR_REACQUIRES"));return;}Pass(TEXT("UNCOVERED_SENSOR_REACQUIRES"));
            Next(Now);return;
        }
        case 4:
        {
            if(Elapsed>20){Fail(TEXT("DISPATCH_RESPONSE"));return;}
            if(Law->GroundUnitCount(PC,true)<1||Law->GroundUnitCount(PC)<2)return;
            bool LowPatrol=false;for(TActorIterator<AVoyagerPatrolShip> It(World);It;++It)if(It->TargetController()==PC)
                if(Voyager::SurfaceAltitude(State->SystemSeed,Run->Planet,It->GetActorLocation())<16000.)LowPatrol=true;
            if(!LowPatrol)return;
            Pass(TEXT("ARMORED_CRUISER_DISPATCHED"));Pass(TEXT("REINFORCEMENT_OFFICERS_DISPATCHED"));Pass(TEXT("PATROL_DESCENDS_TO_SURFACE"));
            if(IsCapturing())
            {
                // Compose the already-tested responding actors on the same road.
                // They keep running production AI while their presentation and
                // the player camera settle; the LOS fixture has been destroyed.
                for(TActorIterator<AVoyagerPoliceUnit> It(World);It;++It)
                    if(It->TargetController()==PC&&It->IsVehicle()){Run->Cruiser=*It;break;}
                if(!Run->Officer.IsValid()||!Run->Cruiser.IsValid()){Fail(TEXT("RESPONSE_CAPTURE_ACTORS"));return;}
                const FVector Right=FVector::CrossProduct(Run->Up,Run->Forward).GetSafeNormal();
                Place(Pawn,Run->Street+Run->Up*105);
                Run->Officer->SetActorLocation(Run->Street+Run->Forward*650-Right*210+Run->Up*8);
                Run->Cruiser->SetActorLocation(Run->Street+Run->Forward*1250+Right*340+Run->Up*110);
            }
            Next(Now);return;
        }
        case 5:
            if(IsCapturing())
            {
                if(Elapsed<.5||Run->StageFrames<3)return;
                if(!CameraSettled(Pawn)||!InResponseFrame(Run->Officer.Get())||!InResponseFrame(Run->Cruiser.Get()))
                {if(Elapsed>3)Fail(TEXT("RESPONSE_CAMERA_READY"));return;}
                Pass(TEXT("RESPONSE_CAMERA_READY"));Capture(TEXT("ArmedResponse"));
            }
            Next(Now);return;
        case 6:
        {
            // Screenshot requests draw after this delegate. Leave the scene
            // intact for subsequent frames before arrest moves the viewpoint.
            if(IsCapturing()&&(Elapsed<.25||Run->StageFrames<2))return;
            // Surrender uses a production server-validated observing response unit.
            const float HealthBeforeSurrender=Pawn->Health;
            if(!Law->Surrender(PC)||!Law->IsJailed(PC)){Fail(TEXT("SURRENDER_ARREST"));return;}
            Pass(TEXT("SURRENDER_ARREST"));
            if(HealthBeforeSurrender>18.f&&!FMath::IsNearlyEqual(HealthBeforeSurrender,Pawn->Health)){Fail(TEXT("SURRENDER_PRESERVES_HEALTH"));return;}
            Pass(TEXT("SURRENDER_PRESERVES_HEALTH"));
            if(PS->WantedStars!=0||Law->GroundUnitCount(PC)!=0||Law->PatrolCount(PC)!=0){Fail(TEXT("ARREST_STANDS_DOWN_PURSUIT"));return;}Pass(TEXT("ARREST_STANDS_DOWN_PURSUIT"));
            if(PS->Minerals!=Run->Minerals){Fail(TEXT("ARREST_CARGO_PRESERVED"));return;}Pass(TEXT("ARREST_CARGO_PRESERVED"));
            Next(Now);return;
        }
        case 7:
        {
            if(Elapsed<1||Run->StageFrames<3)return;
            if(IsCapturing())
            {
                bool CellPresent=false;
                for(TActorIterator<AVoyagerPoliceCell> It(World);It;++It)
                    if(FVector::Dist(It->GetActorLocation(),Pawn->GetActorLocation())<220.)CellPresent=true;
                if(!CameraSettled(Pawn)||!CellPresent){if(Elapsed>4)Fail(TEXT("CUSTODY_CAMERA_READY"));return;}
                Pass(TEXT("CUSTODY_CAMERA_READY"));Capture(TEXT("CivicHoldingCell"));
            }
            Next(Now);return;
        }
        case 8:
        {
            if(IsCapturing()&&(Elapsed<.25||Run->StageFrames<2))return;
            const auto Custody=Law->GetCustody(PC);
            if(!Custody.bJailed||Custody.SecondsRemaining<10||Custody.SecondsRemaining>60){Fail(TEXT("BOUNDED_CUSTODY_TIMER"));return;}Pass(TEXT("BOUNDED_CUSTODY_TIMER"));
            auto Save=NewObject<UVoyagerSave>();Law->CaptureCustody(PC,Save);
            if(Save->JailSeconds<10||Save->JailSystem!=State->SystemSeed||!UGameplayStatics::SaveGameToMemory(Save,Run->CustodySave)){Fail(TEXT("CUSTODY_SAVE_ENCODING"));return;}
            Pass(TEXT("CUSTODY_SAVE_ENCODING"));
            auto GM=World->GetAuthGameMode<AVoyagerGameMode>();GM->BoardShip(Pawn);GM->NextSystem(PC);
            if(PC->GetPawn()!=Pawn||State->bTransitioning){Fail(TEXT("CUSTODY_BLOCKS_BOARDING_AND_WARP"));return;}Pass(TEXT("CUSTODY_BLOCKS_BOARDING_AND_WARP"));
            Pawn->SetActorLocation(Custody.CivicLocation+Run->Forward*9000,false,nullptr,ETeleportType::TeleportPhysics);
            Next(Now);return;
        }
        case 9:
        {
            if(Elapsed<1)return;const auto Custody=Law->GetCustody(PC);
            if(!Custody.bJailed||FVector::Dist(Pawn->GetActorLocation(),Custody.CivicLocation)>160){Fail(TEXT("CUSTODY_REJECTS_ESCAPE_POSITION"));return;}Pass(TEXT("CUSTODY_REJECTS_ESCAPE_POSITION"));Next(Now);return;
        }
        case 10:
            if(Law->IsJailed(PC))return;
            Pass(TEXT("TIMED_RELEASE"));
            if(PS->WantedStars||PS->Minerals!=Run->Minerals){Fail(TEXT("RELEASE_OWNER_STATE"));return;}Pass(TEXT("RELEASE_OWNER_STATE"));Next(Now);return;
        case 11:
            if(Elapsed<.5||Run->StageFrames<3)return;
            if(IsCapturing())
            {
                if(!CameraSettled(Pawn)){if(Elapsed>4)Fail(TEXT("RELEASE_CAMERA_READY"));return;}
                Pass(TEXT("RELEASE_CAMERA_READY"));Capture(TEXT("CivicRelease"));
            }
            Next(Now);return;
        case 12:
            if(Elapsed<4)return;
            {
                auto Saved=Cast<UVoyagerSave>(UGameplayStatics::LoadGameFromMemory(Run->CustodySave));
                if(!Saved||Saved->JailSeconds<10){Fail(TEXT("CUSTODY_SAVE_DECODING"));return;}
                Law->RestoreCustody(PC,Saved);
                if(!Law->IsJailed(PC)||Law->GetCustody(PC).SecondsRemaining<10){Fail(TEXT("CUSTODY_SAVE_RESTORES_SENTENCE"));return;}
                Pass(TEXT("CUSTODY_SAVE_RESTORES_SENTENCE"));Next(Now);return;
            }
        case 13:
            if(Law->IsJailed(PC))return;
            {
                FActorSpawnParameters Params;Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
                const FVector At=Pawn->GetActorLocation()-Run->Up*90+Run->Forward*650;
                auto Officer=World->SpawnActor<AVoyagerPoliceUnit>(At,Voyager::TangentRotation(Run->Up,-Run->Forward),Params);
                if(!Officer){Fail(TEXT("ARREST_DAMAGE_FIXTURE"));return;}Officer->Assign(PC,false,0);
                Law->ReportCrime(PC,Pawn->GetActorLocation(),65,TEXT("armed assault damage fixture"),Officer);
                const FDamageEvent Event;Pawn->TakeDamage(100.f,Event,nullptr,Officer);
                if(!Law->IsJailed(PC)||Pawn->Health<=0){Fail(TEXT("INCAPACITATION_ARRESTS_AND_RECOVERS"));return;}
                Pass(TEXT("INCAPACITATION_ARRESTS_AND_RECOVERS"));Next(Now);return;
            }
        case 14:
            if(Elapsed<1)return;
            Run->Finished=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER POLICE AUDIT COMPLETE PASS checks=%d elapsed=%.2f"),Run->Checks,Now-Run->Started);FPlatformMisc::RequestExit(false);return;
        }
    }
    struct FRegistration
    {
        FDelegateHandle Handle;FRegistration(){Handle=FWorldDelegates::OnWorldPostActorTick.AddStatic(&Tick);}
        ~FRegistration(){FWorldDelegates::OnWorldPostActorTick.Remove(Handle);}
    } Registration;
}
