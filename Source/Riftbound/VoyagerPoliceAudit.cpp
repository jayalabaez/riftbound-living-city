#include "VoyagerPolice.h"
#include "VoyagerLaw.h"
#include "VoyagerCharacter.h"
#include "VoyagerGameMode.h"
#include "VoyagerCityLife.h"
#include "VoyagerSettlement.h"
#include "VoyagerCitizen.h"
#include "VoyagerAuditSignal.h"
#include "VoyagerData.h"
#include "RiftVisual.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/DamageEvents.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraActor.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "UnrealClient.h"
namespace VoyagerPoliceAudit
{
struct FRun
{
    TWeakObjectPtr<UWorld> World;TWeakObjectPtr<AVoyagerController> PC;
    TWeakObjectPtr<AVoyagerPoliceUnit> Drone,Car,Officer;TWeakObjectPtr<AActor> Cover;
    FVector Street,Up,Forward,FirstDrone,PeerPosition;double Started=0,Time=0;
    float Before=100;int32 Stage=0,Checks=0,Planet=0,Site=0;bool Done=false,PeerSaw=false;
};
TUniquePtr<FRun> Run;
void Pass(const TCHAR* Name){++Run->Checks;UE_LOG(LogTemp,Display,TEXT("VOYAGER POLICE AUDIT PASS %s"),Name);}
void Fail(const TCHAR* Name){Run->Done=true;UE_LOG(LogTemp,Error,TEXT("VOYAGER POLICE AUDIT FAIL stage=%d %s"),Run->Stage,Name);FPlatformMisc::RequestExit(false);}
void Next(double Now){++Run->Stage;Run->Time=Now;}
void Capture(const TCHAR* Name)
{
    if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerPoliceCapture")))return;
    FString Folder;FParse::Value(FCommandLine::Get(),TEXT("VoyagerPoliceOutput="),Folder);
    IFileManager::Get().MakeDirectory(*Folder,true);FScreenshotRequest::RequestScreenshot(Folder/(FString(Name)+TEXT(".png")),true,false);
}
void Tick(UWorld* W,ELevelTick,float)
{
    if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerPoliceAudit"))||!W||!W->IsGameWorld())return;
    if(!Run)
    {
        if(W->GetTimeSeconds()<8)return;auto PC=Cast<AVoyagerController>(W->GetFirstPlayerController());
        if(!PC||!PC->IsLocalController()||!Cast<AVoyagerCharacter>(PC->GetPawn()))return;
        Run=MakeUnique<FRun>();Run->World=W;Run->PC=PC;Run->Started=Run->Time=FPlatformTime::Seconds();
    }
    if(Run->World.Get()!=W||Run->Done)return;
    const double Now=FPlatformTime::Seconds(),Elapsed=Now-Run->Time;
    if(Now-Run->Started>130){Fail(TEXT("TIMEOUT"));return;}
    auto PC=Run->PC.Get();auto Pawn=Cast<AVoyagerCharacter>(PC->GetPawn());auto PS=PC->GetPlayerState<AVoyagerPlayerState>();
    auto State=W->GetGameState<AVoyagerState>();auto Law=AVoyagerLaw::Find(W);auto Life=AVoyagerCityLife::Find(W);
    if(!Pawn||!PS||!State||!Law||!Life)return;
    if(W->GetNetMode()==NM_Client)
    {
        if(PS->WantedStars||Law->IsJailed(PC)){Fail(TEXT("INNOCENT_PEER_TARGETED"));return;}
        auto Signal=AVoyagerAuditSignal::Find(W,PC,3);if(!Signal)return;
        auto Drone=Cast<AVoyagerPoliceUnit>(Signal->Subject);
        if(Drone&&Drone->IsDrone()&&Drone->GetHealth()>0&&Signal->Acknowledged<1)
        {
            if(!Run->PeerSaw){Run->PeerSaw=true;Run->PeerPosition=Drone->GetActorLocation();return;}
            if(FVector::Dist(Drone->GetActorLocation(),Run->PeerPosition)<20)return;
            FDamageEvent Event;if(Drone->TakeDamage(100,Event,PC,Pawn)!=0){Fail(TEXT("REMOTE_DRONE_AUTHORITY"));return;}
            Pass(TEXT("REMOTE_DRONE_AND_MOTION"));Pass(TEXT("REMOTE_AUTHORITY_GUARD"));Signal->ServerAcknowledge(1);
        }
        if(Drone&&Drone->GetHealth()<=0&&Signal->Acknowledged==1){Pass(TEXT("REMOTE_DRONE_DEATH"));Signal->ServerAcknowledge(2);}
        if(Signal->Acknowledged==2)
        {
            bool Wanted=false;for(auto P:State->PlayerArray)if(auto Other=Cast<AVoyagerPlayerState>(P))Wanted|=Other->WantedStars>0;
            if(!Wanted){Pass(TEXT("INNOCENT_PEER_FREE"));Signal->ServerAcknowledge(3);Run->Done=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER POLICE AUDIT CLIENT COMPLETE PASS"));}
        }
        return;
    }
    auto Signal=AVoyagerAuditSignal::EnsurePeer(W,3);
    const bool Network=W->GetNetMode()==NM_ListenServer;
    switch(Run->Stage)
    {
    case 0:
    {
        AVoyagerCitizen* Citizen=nullptr;for(TActorIterator<AVoyagerCitizen> It(W);It;++It)if(It->IsAlive()){Citizen=*It;break;}if(!Citizen)return;
        Run->Planet=Citizen->PlanetIndex();Run->Site=Citizen->SiteIndex();
        Run->Street=AVoyagerSettlement::StreetPoint(State->SystemSeed,Run->Planet,Run->Site,2,2,12);
        Run->Up=Voyager::SurfaceNormal(State->SystemSeed,Run->Planet,Run->Street);Run->Forward=Voyager::TangentRotation(Run->Up).Vector();
        Pawn->GetCharacterMovement()->StopMovementImmediately();Pawn->GetCharacterMovement()->SetGravityDirection(-Run->Up);
        Pawn->SetActorLocationAndRotation(Run->Street+Run->Up*105,Voyager::TangentRotation(Run->Up,Run->Forward),false,nullptr,ETeleportType::TeleportPhysics);
        PC->SetControlRotation(Voyager::TangentRotation(Run->Up,Run->Forward));
        auto Legacy=NewObject<UVoyagerSave>();Legacy->JailSeconds=60;Legacy->JailSystem=State->SystemSeed;Legacy->JailPlanet=Run->Planet;Legacy->JailSite=Run->Site;
        TArray<uint8> Bytes;UGameplayStatics::SaveGameToMemory(Legacy,Bytes);auto Loaded=Cast<UVoyagerSave>(UGameplayStatics::LoadGameFromMemory(Bytes));
        const FVector Before=Pawn->GetActorLocation();Law->RestoreCustody(PC,Loaded);
        if(Law->IsJailed(PC)||Law->Surrender(PC)||Law->TryArrest(PC,true)||FVector::Dist(Before,Pawn->GetActorLocation())>1){Fail(TEXT("LEGACY_SENTENCE_RESTORED"));return;}
        Law->CaptureCustody(PC,Loaded);if(Loaded->JailSeconds!=0){Fail(TEXT("NEW_SAVE_HAS_CUSTODY"));return;}Pass(TEXT("JAIL_REMOVED_AND_LEGACY_SAVE_CLEARED"));
        FActorSpawnParameters Params;Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto Officer=W->SpawnActor<AVoyagerPoliceUnit>(Run->Street+Run->Forward*1800,Voyager::TangentRotation(Run->Up,-Run->Forward),Params);
        Officer->Assign(PC,false,0);Run->Officer=Officer;
        Law->ReportCrime(PC,Pawn->GetActorLocation(),65,TEXT("security response audit"),Officer);
        if(PS->WantedStars!=2){Fail(TEXT("WITNESSED_CRIME"));return;}Pass(TEXT("WITNESSED_CRIME_WANTED"));Run->Before=Pawn->Health;Next(Now);return;
    }
    case 1:
        if(Elapsed<2)return;if(Pawn->Health<Run->Before||Run->Officer->ShotsFired()!=0){Fail(TEXT("WARNING_GRACE"));return;}Pass(TEXT("WARNING_GRACE"));Next(Now);return;
    case 2:
    {
        if(!Run->Officer.IsValid()){Fail(TEXT("OFFICER_MISSING"));return;}
        if(Run->Officer->HitsLanded()<1||Pawn->Health>=Run->Before)return;
        Pass(TEXT("OFFICER_REAL_WEAPON_DAMAGE"));Run->Officer->Destroy();Run->Before=Pawn->Health;
        Next(Now);return;
    }
    case 3:
    {
        // Observe a drone produced by the real star-level dispatcher.
        for(TActorIterator<AVoyagerPoliceUnit> It(W);It;++It)if(It->TargetController()==PC&&It->IsDrone()){Run->Drone=*It;break;}
        if(!Run->Drone.IsValid())return;
        if(Network&&!Signal)return;
        if(Signal){Signal->Subject=Run->Drone.Get();Signal->ForceNetUpdate();}
        Pass(TEXT("WANTED_LEVEL_DISPATCHES_DRONE"));Run->FirstDrone=Run->Drone->GetActorLocation();Run->Before=Pawn->Health;Next(Now);return;
    }
    case 4:
    {
        if(!Run->Drone.IsValid()){Fail(TEXT("DRONE_MISSING"));return;}
        if(Run->Drone->HitsLanded()<1||Pawn->Health>=Run->Before)return;
        if(FVector::Dist(Run->FirstDrone,Run->Drone->GetActorLocation())<100){Fail(TEXT("DRONE_STATIONARY"));return;}
        Pass(TEXT("DRONE_FLIES_AND_DAMAGES_CRIMINAL"));
        for(TActorIterator<AVoyagerPoliceUnit> It(W);It;++It)if(It->TargetController()==PC&&It->IsVehicle()){Run->Car=*It;break;}
        if(!Run->Car.IsValid())return;Pass(TEXT("HOVER_CAR_DISPATCHED"));
        for(auto Name:{TEXT("SM_VesperCruiser"),TEXT("SM_SentinelDrone")})
        {
            auto Mesh=LoadObject<UStaticMesh>(nullptr,*FString::Printf(TEXT("/Game/Security/%s.%s"),Name,Name));
            if(!Mesh||Mesh->GetNumLODs()!=3){Fail(TEXT("SECURITY_MODEL_LODS"));return;}
        }
        Pass(TEXT("SHAPED_MODELS_THREE_LODS"));
        // Actual units remain active; compose a close side view for asset inspection.
        const FVector Right=FVector::CrossProduct(Run->Up,Run->Forward);
        Run->Car->SetActorLocationAndRotation(Run->Street+Run->Forward*850+Right*100+Run->Up*110,Voyager::TangentRotation(Run->Up,-Right));
        Run->Drone->SetActorLocation(Run->Street+Run->Forward*1000-Right*350+Run->Up*410);
        if(FParse::Param(FCommandLine::Get(),TEXT("VoyagerPoliceCapture")))
        {
            const FVector Focus=Run->Car->GetActorLocation()+Run->Up*100;
            const FVector View=Focus-Run->Forward*850-Right*850+Run->Up*310;
            auto Camera=W->SpawnActor<ACameraActor>(View,FRotationMatrix::MakeFromXZ(Focus-View,Run->Up).Rotator());
            PC->SetViewTarget(Camera);
        }
        Next(Now);return;
    }
    case 5:
        if(Elapsed<.5)return;Capture(TEXT("DroneAndHoverCar"));Next(Now);return;
    case 6:
    {
        if(Elapsed<.3)return;if(Network&&(!Signal||Signal->Acknowledged<1))return;
        auto Drone=Run->Drone.Get();const FVector Eye=Pawn->GetPawnViewLocation(),Mid=(Eye+Drone->GetActorLocation())*.5;
        auto Cover=W->SpawnActor<AActor>();Cover->SetRootComponent(NewObject<USceneComponent>(Cover));Cover->GetRootComponent()->RegisterComponent();
        Cover->SetActorLocationAndRotation(Mid,FRotationMatrix::MakeFromX(Drone->GetActorLocation()-Eye).Rotator());
        auto Wall=RiftVisual::Mesh(Cover,Cover->GetRootComponent(),TEXT("AuditCover"),TEXT("Cube"),FVector::ZeroVector,FVector(.6,30,30),FLinearColor(.2f,.2f,.2f));
        Wall->SetCollisionEnabled(ECollisionEnabled::QueryOnly);Wall->SetCollisionResponseToAllChannels(ECR_Block);Run->Cover=Cover;
        if(Drone->CanSee(Pawn)){Fail(TEXT("DRONE_SEES_THROUGH_WALL"));return;}Pass(TEXT("SOLID_COVER_BLOCKS_DRONE"));
        Cover->Destroy();if(!Drone->CanSee(Pawn)){Fail(TEXT("DRONE_CANNOT_REACQUIRE"));return;}Pass(TEXT("DRONE_REACQUIRES_SIGHT"));
        FDamageEvent Event;Drone->TakeDamage(1000,Event,PC,Pawn);
        if(Drone->GetHealth()>0){Fail(TEXT("DRONE_INDESTRUCTIBLE"));return;}Pass(TEXT("DRONE_CAN_BE_SHOT_DOWN"));Next(Now);return;
    }
    case 7:
    {
        if(Elapsed<.5||Network&&(!Signal||Signal->Acknowledged<2))return;
        Law->Resolve(PC,false);if(Law->DroneCount(PC)||Law->GroundUnitCount(PC,true)||PS->WantedStars){Fail(TEXT("RESPONSE_NOT_CLEARED"));return;}
        Pass(TEXT("PURSUIT_END_CLEARS_DRONES"));
        // Recover through the actual lethal hit path; no arrest, cell or timer.
        const int32 Minerals=PS->Minerals;FDamageEvent Event;Pawn->TakeDamage(1000,Event,nullptr,Pawn);
        if(Pawn->Health<=0||Law->IsJailed(PC)||PS->Minerals!=Minerals){Fail(TEXT("DEATH_RECOVERY_OR_CARGO"));return;}
        Pass(TEXT("LETHAL_DAMAGE_RECOVERS_WITHOUT_JAIL"));
        Next(Now);return;
    }
    case 8:
        if(Elapsed<2||Network&&(!Signal||Signal->Acknowledged<3))return;
        Run->Done=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER POLICE AUDIT COMPLETE PASS checks=%d seconds=%.2f"),Run->Checks,Now-Run->Started);FPlatformMisc::RequestExit(false);return;
    }
}
struct FRegistration
{
    FDelegateHandle Handle;FRegistration(){Handle=FWorldDelegates::OnWorldPostActorTick.AddStatic(&Tick);}
    ~FRegistration(){FWorldDelegates::OnWorldPostActorTick.Remove(Handle);}
} Registration;
}
