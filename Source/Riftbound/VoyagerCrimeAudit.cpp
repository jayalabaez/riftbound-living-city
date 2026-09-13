// Opt-in tests exercise the public firing, damage, sensor and recovery paths.
#include "VoyagerLaw.h"
#include "VoyagerCharacter.h"
#include "VoyagerCitizen.h"
#include "VoyagerGameMode.h"
#include "VoyagerSettlement.h"
#include "VoyagerShip.h"
#include "VoyagerData.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "Engine/DamageEvents.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
namespace VoyagerCrimeAudit
{
    struct FRun
    {
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<AVoyagerController> PC;
        TWeakObjectPtr<AVoyagerCitizen> Citizen;
        TWeakObjectPtr<AVoyagerPatrolShip> Patrol;
        FVector DeadPosition=FVector::ZeroVector,LastKnown=FVector::ZeroVector;
        double Started=0,StageTime=0,LastShot=0;
        int32 Stage=0,Checks=0,DeadOrdinal=0,DeadPlanet=0,DeadSite=0;
        bool Finished=false,PeerWanted=false,PeerDead=false,PeerPatrol=false;
    };
    TUniquePtr<FRun> Run;
    void Pass(const TCHAR* Name){++Run->Checks;UE_LOG(LogTemp,Display,TEXT("VOYAGER CRIME AUDIT PASS %s"),Name);}
    void Fail(const TCHAR* Name){Run->Finished=true;UE_LOG(LogTemp,Error,TEXT("VOYAGER CRIME AUDIT FAIL stage=%d %s"),Run->Stage,Name);FPlatformMisc::RequestExit(false);}
    void Advance(double Now){++Run->Stage;Run->StageTime=Now;}
    void Capture(const TCHAR* Name)
    {
        if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerCrimeCapture")))return;
        FString Folder;if(!FParse::Value(FCommandLine::Get(),TEXT("VoyagerCrimeOutput="),Folder))Folder=FPaths::ProjectSavedDir()/TEXT("VoyagerCrimeAudit");
        IFileManager::Get().MakeDirectory(*Folder,true);FScreenshotRequest::RequestScreenshot(Folder/(FString(Name)+TEXT(".png")),true,false);
    }
    void Place(AVoyagerCharacter* Pawn,FVector Location,FVector Forward)
    {
        auto State=Pawn->GetWorld()->GetGameState<AVoyagerState>();const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,Voyager::NearestPlanet(State->SystemSeed,Location),Location);
        Pawn->GetCharacterMovement()->StopMovementImmediately();Pawn->GetCharacterMovement()->SetGravityDirection(-Up);
        Pawn->SetActorLocationAndRotation(Location,Voyager::TangentRotation(Up,Forward),false,nullptr,ETeleportType::TeleportPhysics);
        Run->PC->SetControlRotation(Forward.Rotation());Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
    }
    void Tick(UWorld* World,ELevelTick Type,float D)
    {
        if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerCrimeAudit"))||!World||!World->IsGameWorld())return;
        if(!Run)
        {
            if(World->GetTimeSeconds()<8.f)return;
            auto PC=Cast<AVoyagerController>(World->GetFirstPlayerController());if(!PC||!PC->IsLocalController()||!Cast<AVoyagerCharacter>(PC->GetPawn()))return;
            Run=MakeUnique<FRun>();Run->World=World;Run->PC=PC;Run->Started=Run->StageTime=FPlatformTime::Seconds();
        }
        if(Run->World.Get()!=World||Run->Finished)return;
        const double Now=FPlatformTime::Seconds(),Elapsed=Now-Run->StageTime;
        if(Now-Run->Started>180){Fail(TEXT("TIMEOUT"));return;}
        auto PC=Run->PC.Get();auto Pawn=PC?Cast<AVoyagerCharacter>(PC->GetPawn()):nullptr;
        auto PS=PC?PC->GetPlayerState<AVoyagerPlayerState>():nullptr;auto State=World->GetGameState<AVoyagerState>();
        if(!PC||!Pawn||!PS||!State)return;
        if(World->GetNetMode()==NM_Client)
        {
            for(auto Player:State->PlayerArray)if(auto Other=Cast<AVoyagerPlayerState>(Player))if(Other!=PS&&Other->WantedStars>=3)Run->PeerWanted=true;
            for(TActorIterator<AVoyagerCitizen> It(World);It;++It)if(!It->IsAlive()&&It->DeathTime>0)Run->PeerDead=true;
            for(TActorIterator<AVoyagerPatrolShip> It(World);It;++It)if(It->Hull>0)Run->PeerPatrol=true;
            if(PS->WantedStars!=0){Fail(TEXT("INNOCENT_PEER_WANTED"));return;}
            if(Run->PeerWanted&&Run->PeerDead&&Run->PeerPatrol)
            {
                Pass(TEXT("REMOTE_WANTED_REPLICATED"));Pass(TEXT("REMOTE_CITIZEN_DEATH_REPLICATED"));Pass(TEXT("REMOTE_PATROL_REPLICATED"));Pass(TEXT("INNOCENT_PEER_UNWANTED"));
                Run->Finished=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER CRIME AUDIT CLIENT COMPLETE PASS"));FPlatformMisc::RequestExit(false);
            }
            return;
        }
        auto Law=AVoyagerLaw::Find(World);if(!Law)return;
        switch(Run->Stage)
        {
        case 0:
        {
            for(TActorIterator<AVoyagerCitizen> It(World);It;++It)if(It->IsAlive()&&It->OrdinalIndex()==0){Run->Citizen=*It;break;}
            auto Citizen=Run->Citizen.Get();if(!Citizen)return;
            const FVector Up=Citizen->GetActorUpVector();const FVector At=Citizen->GetActorLocation();
            Place(Pawn,At+Citizen->GetActorForwardVector()*300+Up*110,-Citizen->GetActorForwardVector());
            Citizen->TalkTo(Pawn,0);
            Pawn->ServerToggleWeapon();
            Pass(TEXT("SIDEARM_EQUIPPED"));Advance(Now);return;
        }
        case 1:
        {
            if(Elapsed<2)return;auto Citizen=Run->Citizen.Get();if(!Citizen){Fail(TEXT("MISSING_RESIDENT"));return;}
            if(FParse::Param(FCommandLine::Get(),TEXT("VoyagerCrimeCapture")))
            {
                auto Mesh=Citizen->FindComponentByClass<USkeletalMeshComponent>();
                if(!Mesh||!Mesh->GetSkeletalMeshAsset()){Fail(TEXT("CHARACTER_SKELETAL_ASSET_MISSING"));return;}
            }
            Pass(TEXT("CITIZEN_PRESENT"));Capture(TEXT("CitizenBefore"));Advance(Now);return;
        }
        case 2:
        {
            if(Elapsed<.4)return;auto Citizen=Run->Citizen.Get();if(!Citizen){Fail(TEXT("MISSING_RESIDENT"));return;}
            const FVector Direction=(Citizen->GetActorLocation()+Citizen->GetActorUpVector()*105-Pawn->GetPawnViewLocation()).GetSafeNormal();
            PC->SetControlRotation(Direction.Rotation());Pawn->ServerMine(Direction);
            if(Citizen->Health>=100){Fail(TEXT("SIDEARM_DID_NOT_HIT_RESIDENT"));return;}
            if(PS->WantedStars!=1){Fail(TEXT("ASSAULT_NOT_ONE_STAR"));return;}
            Pass(TEXT("SIDEARM_REAL_HIT"));Pass(TEXT("ASSAULT_WANTED"));Advance(Now);return;
        }
        case 3:
        {
            if(Now-Run->LastShot<.3)return;auto Citizen=Run->Citizen.Get();if(!Citizen){Fail(TEXT("MISSING_RESIDENT"));return;}
            Run->LastShot=Now;const FVector Direction=(Citizen->GetActorLocation()+Citizen->GetActorUpVector()*105-Pawn->GetPawnViewLocation()).GetSafeNormal();
            PC->SetControlRotation(Direction.Rotation());Pawn->ServerMine(Direction);
            if(Citizen->IsAlive())return;
            if(PS->WantedStars<3||Citizen->DeathTime<=0){Fail(TEXT("HOMICIDE_NOT_ESCALATED"));return;}
            Run->DeadPosition=Citizen->GetActorLocation();Run->DeadOrdinal=Citizen->OrdinalIndex();Run->DeadPlanet=Citizen->PlanetIndex();Run->DeadSite=Citizen->SiteIndex();
            Pass(TEXT("CITIZEN_KILLABLE"));Pass(TEXT("HOMICIDE_ESCALATION"));Advance(Now);return;
        }
        case 4:
        {
            if(Elapsed<2)return;auto Citizen=Run->Citizen.Get();
            if(!Citizen||!Citizen->GetVelocity().IsNearlyZero()||FVector::Dist(Citizen->GetActorLocation(),Run->DeadPosition)>1||!Citizen->TalkTo(Pawn,0).IsEmpty())
            {Fail(TEXT("DEAD_CITIZEN_STILL_ACTIVE"));return;}
            Pass(TEXT("DEAD_CITIZEN_INACTIVE"));Capture(TEXT("CrimeScene"));
            Run->Stage=40;Run->StageTime=Now;return;
        }
        case 40:
        {
            if(Elapsed<.3)return;
            // A clear plaza fixture permits observing actual patrol sensors/fire.
            const FVector Up=AVoyagerSettlement::SiteDirection(State->SystemSeed,Run->DeadPlanet,Run->DeadSite);
            const FRotator Frame=Voyager::TangentRotation(Up);
            Place(Pawn,Voyager::SurfacePoint(State->SystemSeed,Run->DeadPlanet,Up,115)+Frame.Vector()*1500,Frame.Vector());
            Run->Stage=5;Run->StageTime=Now;return;
        }
        case 5:
        {
            for(TActorIterator<AVoyagerPatrolShip> It(World);It;++It)if(It->TargetController()==PC&&It->CanSee(Pawn)){Run->Patrol=*It;break;}
            if(!Run->Patrol.IsValid())return;
            if(Law->PatrolCount(PC)>4){Fail(TEXT("PATROL_BUDGET_EXCEEDED"));return;}
            Pass(TEXT("PATROL_DISPATCH_AND_SENSOR"));Advance(Now);return;
        }
        case 6:
        {
            if(Pawn->Health>=100)return;
            Pass(TEXT("PATROL_WEAPON_DAMAGE"));Capture(TEXT("PatrolResponse"));
            Run->Stage=60;Run->StageTime=Now;return;
        }
        case 60:
        {
            if(Elapsed<.3)return;
            FVoyagerBuildingInfo Info;if(!AVoyagerSettlement::GetBuildingInfo(State->SystemSeed,Run->DeadPlanet,Run->DeadSite,8,Info)){Fail(TEXT("MISSING_SHELTER"));return;}
            Place(Pawn,Info.InteriorPoint+Info.Up*105,Info.Forward);Run->LastKnown=PS->LastKnownPosition;
            Run->Stage=7;Run->StageTime=Now;return;
        }
        case 7:
        {
            if(Elapsed<5)return;
            bool AnySees=false;for(TActorIterator<AVoyagerPatrolShip> It(World);It;++It)if(It->TargetController()==PC&&It->CanSee(Pawn))AnySees=true;
            if(AnySees||!PS->bLawSearching){Fail(TEXT("SHELTER_DID_NOT_BREAK_SIGHT"));return;}
            if(FVector::Dist(PS->LastKnownPosition,Pawn->GetActorLocation())<300){Fail(TEXT("SEARCH_TRACKS_HIDDEN_PLAYER"));return;}
            Pass(TEXT("BUILDING_BREAKS_LINE_OF_SIGHT"));Pass(TEXT("SEARCH_LAST_KNOWN_POSITION"));Capture(TEXT("Searching"));Advance(Now);return;
        }
        case 8:
        {
            if(PS->WantedStars>0)return;if(Law->PatrolCount(PC)!=0){Fail(TEXT("PATROLS_NOT_STOOD_DOWN"));return;}
            Pass(TEXT("ESCAPED_SEARCH_COOLDOWN"));
            // Verify streamed city identities do not resurrect when a corpse is retired.
            if(auto Citizen=Run->Citizen.Get())Citizen->Destroy();Advance(Now);return;
        }
        case 9:
        {
            if(Elapsed<3)return;
            for(TActorIterator<AVoyagerCitizen> It(World);It;++It)
                if(It->PlanetIndex()==Run->DeadPlanet&&It->SiteIndex()==Run->DeadSite&&It->OrdinalIndex()==Run->DeadOrdinal&&It->IsAlive()){Fail(TEXT("DEAD_IDENTITY_RESPAWNED"));return;}
            Pass(TEXT("DEATH_SURVIVES_CORPSE_RETIREMENT"));
            Law->ReportCrime(PC,Pawn->GetActorLocation(),400,TEXT("audit escalation fixture"));
            if(PS->WantedStars!=5){Fail(TEXT("MAX_STARS"));return;}Pass(TEXT("FIVE_STAR_LIMIT"));
            const int32 Cargo=PS->Minerals;FDamageEvent Damage;Pawn->TakeDamage(200,Damage,nullptr,nullptr);
            if(Pawn->Health!=100||PS->WantedStars!=0||PS->Minerals!=Cargo){Fail(TEXT("PLAYER_RECOVERY"));return;}
            Pass(TEXT("PLAYER_RECOVERY_CARGO_PRESERVED"));Advance(Now);return;
        }
        case 10:
        {
            if(Elapsed<2)return;
            auto Patrol=World->SpawnActor<AVoyagerPatrolShip>(Pawn->GetActorLocation()+Pawn->GetActorUpVector()*80000,FRotator::ZeroRotator);
            if(!Patrol){Fail(TEXT("PATROL_DAMAGE_FIXTURE"));return;}Patrol->Assign(PC,0);
            FDamageEvent Damage;Patrol->TakeDamage(75,Damage,PC,Pawn);Patrol->TakeDamage(75,Damage,PC,Pawn);
            if(Patrol->Hull!=0||PS->WantedStars<3){Fail(TEXT("PATROL_DAMAGE_OR_ESCALATION"));return;}
            Pass(TEXT("PATROL_DESTROYED_AND_ESCALATED"));Law->Resolve(PC,false);
            auto GM=World->GetAuthGameMode<AVoyagerGameMode>();auto Ship=GM?GM->ShipFor(PC):nullptr;
            if(Ship)
            {
                // Explicit overhead camera fixture inspects the imported airframe.
                const FVector View=Ship->GetActorLocation()+Ship->GetActorForwardVector()*850-Ship->GetActorRightVector()*750+Ship->GetActorUpVector()*450;
                Place(Pawn,View,(Ship->GetActorLocation()-View).GetSafeNormal());
                Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
                PC->NoticeTime=0;PC->Notice.Empty();
            }
            Advance(Now);return;
        }
        case 11:
            if(Elapsed<3)return;Capture(TEXT("KestrelShip"));Advance(Now);return;
        case 12:
            if(Elapsed<1)return;Run->Finished=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER CRIME AUDIT COMPLETE PASS checks=%d"),Run->Checks);FPlatformMisc::RequestExit(false);return;
        }
    }
    struct FRegistration
    {
        FDelegateHandle Handle;
        FRegistration(){Handle=FWorldDelegates::OnWorldPostActorTick.AddStatic(&Tick);}
        ~FRegistration(){FWorldDelegates::OnWorldPostActorTick.Remove(Handle);}
    } Registration;
}
