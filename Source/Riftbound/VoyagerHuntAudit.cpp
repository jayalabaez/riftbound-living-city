#include "VoyagerWildlife.h"
#include "VoyagerAuditSignal.h"
#include "VoyagerCitizen.h"
#include "VoyagerCharacter.h"
#include "VoyagerGameMode.h"
#include "VoyagerData.h"
#include "VoyagerSettlement.h"
#include "Animation/AnimSequence.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Engine/DamageEvents.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

namespace VoyagerHuntAudit
{
struct FRun
{
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AVoyagerAnimal> Subject;
    TWeakObjectPtr<AVoyagerCitizen> Citizen;
    TWeakObjectPtr<AVoyagerAuditSignal> Signal;
    TArray<int32> InitialItems;
    double Started=0,StageStarted=0,NextPoll=0;
    int32 Stage=0,Shots=0,Checks=0;
    bool Finished=false;
};
TUniquePtr<FRun> Run;
void Pass(const TCHAR* Name){++Run->Checks;UE_LOG(LogTemp,Display,TEXT("VOYAGER HUNT AUDIT PASS %s"),Name);}
void Fail(const TCHAR* Name){Run->Finished=true;UE_LOG(LogTemp,Error,TEXT("VOYAGER HUNT AUDIT FAIL stage=%d %s"),Run->Stage,Name);FPlatformMisc::RequestExit(false);}
void Next(double Now,int32 Stage){Run->Stage=Stage;Run->StageStarted=Now;}
void Capture(const TCHAR* Name)
{
    if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerHuntCapture")))return;
    FString Folder;if(!FParse::Value(FCommandLine::Get(),TEXT("VoyagerHuntOutput="),Folder))Folder=FPaths::ProjectSavedDir()/TEXT("VoyagerHuntAudit");
    IFileManager::Get().MakeDirectory(*Folder,true);FScreenshotRequest::RequestScreenshot(Folder/(FString(Name)+TEXT(".png")),true,false);
}
void Place(AVoyagerCharacter* Pawn,FVector Location,FVector LookAt)
{
    auto State=Pawn->GetWorld()->GetGameState<AVoyagerState>();const int32 Planet=Voyager::NearestPlanet(State->SystemSeed,Location);
    const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,Planet,Location);
    const FRotator Facing=Voyager::TangentRotation(Up,LookAt-Location);
    Pawn->GetCharacterMovement()->StopMovementImmediately();Pawn->GetCharacterMovement()->SetGravityDirection(-Up);
    Pawn->SetActorLocationAndRotation(Location,Facing,false,nullptr,ETeleportType::TeleportPhysics);
    Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
    const FVector View=Pawn->GetPawnViewLocation();Pawn->GetController()->SetControlRotation((LookAt-View).Rotation());
}
bool InventoryMatches(AVoyagerPlayerState* PS,int32 Meat=0,int32 Hide=0,int32 Bone=0)
{
    return PS->ItemCount(EVoyagerItem::RawMeat)==Run->InitialItems[int32(EVoyagerItem::RawMeat)]+Meat&&
        PS->ItemCount(EVoyagerItem::Hide)==Run->InitialItems[int32(EVoyagerItem::Hide)]+Hide&&
        PS->ItemCount(EVoyagerItem::Bone)==Run->InitialItems[int32(EVoyagerItem::Bone)]+Bone;
}
void Tick(UWorld* World,ELevelTick TickType,float Delta)
{
    if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerHuntAudit"))||!World||!World->IsGameWorld())return;
    auto PC=Cast<AVoyagerController>(World->GetFirstPlayerController());
    auto Pawn=PC?Cast<AVoyagerCharacter>(PC->GetPawn()):nullptr;auto PS=PC?PC->GetPlayerState<AVoyagerPlayerState>():nullptr;
    auto State=World->GetGameState<AVoyagerState>();
    if(!Pawn||!PS||!State||!PC->IsLocalController()||World->GetTimeSeconds()<8.f)return;
    if(!Run){Run=MakeUnique<FRun>();Run->World=World;Run->Started=Run->StageStarted=FPlatformTime::Seconds();Run->InitialItems=PS->Items;}
    if(Run->Finished||Run->World.Get()!=World)return;
    const double Now=FPlatformTime::Seconds(),Elapsed=Now-Run->StageStarted;
    if(Now-Run->Started>130.f){Fail(TEXT("TIMEOUT"));return;}
    if(Now<Run->NextPoll)return;Run->NextPoll=Now+.35;
    if(World->GetNetMode()==NM_ListenServer&&!Run->Signal.IsValid())Run->Signal=AVoyagerAuditSignal::EnsurePeer(World,1);
    if(World->GetNetMode()==NM_Client)
    {
        if(!Run->Signal.IsValid())Run->Signal=AVoyagerAuditSignal::Find(World,PC,1);
        if(Run->Signal.IsValid())Run->Subject=Cast<AVoyagerAnimal>(Run->Signal->Subject);
    }
    auto Animal=Run->Subject.Get();
    if(World->GetNetMode()==NM_Client)
    {
        if(!Animal)return;
        if(Run->Stage==0&&Animal->IsAlive())
        {
            const float Health=Animal->Health;FDamageEvent Event;
            if(Animal->TakeDamage(200.f,Event,PC,Pawn)!=0.f||Animal->Health!=Health||Animal->Harvest(PC)){Fail(TEXT("REMOTE_MUTATION"));return;}
            Pass(TEXT("REMOTE_AUTHORITY_GUARD"));Run->Signal->ServerAcknowledge(1);Next(Now,1);
        }
        if(Run->Stage==1&&Animal->IsDead()){Pass(TEXT("REMOTE_DEATH_REPLICATED"));Run->Signal->ServerAcknowledge(2);Next(Now,2);}
        if(Run->Stage==2&&Animal->bHarvested)
        {
            if(!InventoryMatches(PS)){Fail(TEXT("REMOTE_LOOT_LEAK"));return;}
            Pass(TEXT("REMOTE_HARVEST_REPLICATED"));Pass(TEXT("REMOTE_LOOT_ISOLATED"));
            Run->Signal->ServerAcknowledge(3);Next(Now,3);return;
        }
        if(Run->Stage==3&&Run->Signal->Acknowledged>=3)
        {Run->Finished=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER HUNT AUDIT CLIENT COMPLETE PASS"));FPlatformMisc::RequestExit(false);}
        return;
    }
    switch(Run->Stage)
    {
    case 0:
    {
        for(const TCHAR* Variant:{TEXT("Male"),TEXT("Female"),TEXT("Guard")})
        {
            const FString Folder=FString::Printf(TEXT("/Game/Characters/Import/%s/Citizen%s/SkeletalMeshes/"),Variant,Variant);
            const FString MeshName=FString::Printf(TEXT("Citizen%s"),Variant);
            auto Mesh=LoadObject<USkeletalMesh>(nullptr,*(Folder+MeshName+TEXT(".")+MeshName));
            const FString Aim=FString::Printf(TEXT("Citizen%sA_Citizen%s_Aim"),Variant,Variant);
            auto Clip=LoadObject<UAnimSequence>(nullptr,*(Folder+Aim+TEXT(".")+Aim));
            if(!Mesh||!Clip||Mesh->GetSkeleton()!=Clip->GetSkeleton()||Clip->GetPlayLength()<1.f){Fail(TEXT("HUMANOID_ASSET_LOAD"));return;}
        }
        Pass(TEXT("THREE_HUMANOIDS_AND_AIM_ASSETS"));
        for(TActorIterator<AVoyagerCitizen> It(World);It;++It)if(It->IsAlive()&&!It->IsSecurityGuard()){Run->Citizen=*It;break;}
        if(!Run->Citizen.IsValid())return;
        auto Citizen=Run->Citizen.Get();
        auto Mesh=Citizen->FindComponentByClass<USkeletalMeshComponent>();
        if(!Mesh||FVector::DotProduct(Citizen->GetActorForwardVector(),Mesh->GetRightVector())<.95){Fail(TEXT("HUMANOID_FORWARD_AXIS"));return;}
        Pass(TEXT("HUMANOID_FORWARD_AXIS"));
        const FVector At=Citizen->GetActorLocation(),Up=Citizen->GetActorUpVector();
        Place(Pawn,At+Citizen->GetActorForwardVector()*190.f+Up*100.f,At+Up*165.f);
        Citizen->TalkTo(Pawn,0);Next(Now,1);return;
    }
    case 1:
        if(Elapsed<1.5)return;
        if(auto Citizen=Run->Citizen.Get())
        {
            const FVector Up=Citizen->GetActorUpVector();PC->SetControlRotation((Citizen->GetActorLocation()+Up*164.f-Pawn->GetPawnViewLocation()).Rotation());
        }
        Capture(TEXT("CitizenEyes"));Next(Now,2);return;
    case 2:
    {
        const FVector Up=AVoyagerSettlement::SiteDirection(State->SystemSeed,0,0);
        const FVector Forward=Voyager::TangentRotation(Up).Vector();
        const FVector Direction=Voyager::SurfaceNormal(State->SystemSeed,0,Voyager::SurfacePoint(State->SystemSeed,0,Up)+Forward*55000.f);
        const FVector Ground=Voyager::SurfacePoint(State->SystemSeed,0,Direction,3.f);
        const FTransform Transform(Voyager::TangentRotation(Direction,Forward),Ground);
        FActorSpawnParameters Params;Params.Name=TEXT("VoyagerHuntFixture");Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Animal=World->SpawnActor<AVoyagerAnimal>(AVoyagerAnimal::StaticClass(),Transform,Params);
        if(!Animal){Fail(TEXT("WILDLIFE_FIXTURE"));return;}
        Animal->InitializeAnimal(State->SystemSeed,0,0,777313);Run->Subject=Animal;
        Place(Pawn,Ground-Forward*1400.f+Direction*115.f,Ground+Direction*130.f);
        if(!Pawn->bWeaponMode)Pawn->ServerToggleWeapon();Next(Now,3);return;
    }
    case 3:
    {
        if(!Animal){Fail(TEXT("SUBJECT_MISSING"));return;}
        if(World->GetNetMode()==NM_ListenServer)
        {
            if(!Run->Signal.IsValid())return;
            if(Run->Signal->Subject!=Animal){Run->Signal->Subject=Animal;Run->Signal->ForceNetUpdate();}
            if(Run->Signal->Acknowledged<1)return;
            Pass(TEXT("PEER_OBSERVED_LIVE_SUBJECT"));
        }
        else if(Elapsed<4)return;
        FDamageEvent Event;
        const float Health=Animal->Health;
        if(Animal->TakeDamage(-50.f,Event,PC,Pawn)!=0||Animal->Health!=Health){Fail(TEXT("INVALID_DAMAGE"));return;}
        const FVector Prior=Pawn->GetActorLocation(),At=Animal->GetActorLocation(),Up=Animal->GetActorUpVector();
        Place(Pawn,At+Animal->GetActorForwardVector()*320.f+Up*115.f,At+Up*130.f);
        if(Animal->Harvest(PC)||!InventoryMatches(PS)){Fail(TEXT("INVALID_ALIVE_ACTION"));return;}
        Place(Pawn,Prior,At+Up*130.f);
        Pass(TEXT("INVALID_DAMAGE_AND_LIVE_HARVEST_REJECTED"));
        const FVector Start=Pawn->GetActorLocation()+Pawn->GetActorUpVector()*64.f;
        Pawn->ServerMine((Animal->GetActorLocation()+Animal->GetActorUpVector()*130.f-Start).GetSafeNormal());
        Next(Now,4);return;
    }
    case 4:
    {
        if(!Animal||Animal->Health>=100.f||!Animal->IsAlive()||!Animal->IsFleeing()){Fail(TEXT("NORMAL_GUN_DAMAGE"));return;}
        Pass(TEXT("NORMAL_GUN_DAMAGE_CAUSES_FLEE"));Next(Now,5);return;
    }
    case 5:
    {
        if(!Animal){Fail(TEXT("SUBJECT_MISSING"));return;}
        if(Animal->IsAlive())
        {
            if(++Run->Shots>5){Fail(TEXT("GUN_KILL"));return;}
            const FVector Start=Pawn->GetActorLocation()+Pawn->GetActorUpVector()*64.f;
            Pawn->ServerMine((Animal->GetActorLocation()+Animal->GetActorUpVector()*130.f-Start).GetSafeNormal());return;
        }
        if(!Animal->CanHarvest()||!Animal->GetVelocity().IsNearlyZero()){Fail(TEXT("DEATH_STATE"));return;}
        Pass(TEXT("WILDLIFE_DEATH_STOPS_AND_LEAVES_CARCASS"));
        if(Animal->Harvest(PC)||!InventoryMatches(PS)){Fail(TEXT("REMOTE_HARVEST"));return;}
        Pass(TEXT("OUT_OF_RANGE_HARVEST_REJECTED"));
        const FVector At=Animal->GetActorLocation(),Up=Animal->GetActorUpVector(),Forward=Animal->GetActorForwardVector();
        Place(Pawn,At+Forward*320.f+Up*115.f,At+Up*60.f);Next(Now,6);return;
    }
    case 6:
    {
        if(Elapsed<1.5)return;
        if(!Animal){Fail(TEXT("CARCASS_MISSING"));return;}
        Capture(TEXT("Carcass"));
        const FVector End=Animal->GetActorLocation()+Animal->GetActorUpVector()*55.f;
        auto Barrier=World->SpawnActor<AActor>();auto Box=NewObject<UBoxComponent>(Barrier);
        Barrier->SetRootComponent(Box);Box->SetBoxExtent(FVector(30,200,200));
        Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);Box->SetCollisionResponseToAllChannels(ECR_Ignore);Box->SetCollisionResponseToChannel(ECC_Visibility,ECR_Block);
        Box->RegisterComponent();Barrier->SetActorLocationAndRotation((Pawn->GetPawnViewLocation()+End)*.5,(End-Pawn->GetPawnViewLocation()).Rotation());
        const bool Rejected=!Animal->Harvest(PC)&&InventoryMatches(PS);Barrier->Destroy();
        if(!Rejected){Fail(TEXT("WALL_HARVEST"));return;}Pass(TEXT("OBSTRUCTED_HARVEST_REJECTED"));Next(Now,7);return;
    }
    case 7:
    {
        if(!Animal){Fail(TEXT("CARCASS_MISSING"));return;}
        if(World->GetNetMode()==NM_ListenServer&&(!Run->Signal.IsValid()||Run->Signal->Acknowledged<2))return;
        const int32 Meat=PS->ItemCount(EVoyagerItem::RawMeat);PS->Items[int32(EVoyagerItem::RawMeat)]=999;
        const bool Rejected=!Animal->Harvest(PC)&&!Animal->bHarvested&&PS->ItemCount(EVoyagerItem::RawMeat)==999;
        PS->Items[int32(EVoyagerItem::RawMeat)]=Meat;
        if(!Rejected||!InventoryMatches(PS)){Fail(TEXT("CAPACITY_ATOMICITY"));return;}
        Pass(TEXT("FULL_INVENTORY_PRESERVES_CARCASS"));
        Pawn->ServerInteract();Next(Now,8);return;
    }
    case 8:
        if(Elapsed<1)return;
        if(!Animal||!Animal->bHarvested||Animal->CanHarvest()||!InventoryMatches(PS,3,1,1)){Fail(TEXT("E_HARVEST_LOOT"));return;}
        Pass(TEXT("E_HARVEST_GRANTS_EXACT_LOOT"));
        if(Animal->Harvest(PC)||!InventoryMatches(PS,3,1,1)){Fail(TEXT("DUPLICATE_LOOT"));return;}
        Pass(TEXT("REPEATED_HARVEST_REJECTED"));Next(Now,9);return;
    case 9:
        if(Elapsed<14)return;
        if(World->GetNetMode()==NM_ListenServer&&(!Run->Signal.IsValid()||Run->Signal->Acknowledged<3))return;
        if(Run->Subject.IsValid()){Fail(TEXT("CARCASS_CLEANUP"));return;}
        Pass(TEXT("HARVESTED_CARCASS_EXPIRES"));PC->ServerSave();Next(Now,10);return;
    case 10:
    {
        auto Save=Cast<UVoyagerSave>(UGameplayStatics::LoadGameFromSlot(TEXT("Voyager-Automation-Hunt"),0));
        if(!Save||Save->Items!=PS->Items){Fail(TEXT("INVENTORY_DISK_ROUND_TRIP"));return;}
        Pass(TEXT("HUNT_INVENTORY_SAVED"));Run->Finished=true;
        UE_LOG(LogTemp,Display,TEXT("VOYAGER HUNT AUDIT COMPLETE PASS checks=%d seconds=%.2f"),Run->Checks,Now-Run->Started);FPlatformMisc::RequestExit(false);return;
    }
    }
}
struct FHook{FHook(){FWorldDelegates::OnWorldPostActorTick.AddStatic(&Tick);}} Hook;
}
