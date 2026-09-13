#include "VoyagerDestruction.h"
#include "VoyagerSettlement.h"
#include "VoyagerCharacter.h"
#include "VoyagerGameMode.h"
#include "VoyagerData.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#include "UnrealClient.h"

namespace VoyagerDestructionAudit
{
struct FRun
{
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AVoyagerSettlement> City;
    TWeakObjectPtr<AVoyagerDemolitionCharge> Charge;
    TStrongObjectPtr<UVoyagerSave> Saved;
    TMap<TWeakObjectPtr<AVoyagerDebris>,FVector> DebrisPositions;
    FVoyagerBuildingInfo Building,Other;
    FHitResult GlassHit,WallHit;
    double Started=0,StageStarted=0,NextPoll=0;
    int32 Stage=0,Checks=0,Before=0,OtherBefore=0,AfterGlass=0,AfterPartial=0,AfterFull=0,System=1;
    bool Finished=false;
};
TUniquePtr<FRun> Run;
void Pass(const TCHAR* Name){++Run->Checks;UE_LOG(LogTemp,Display,TEXT("VOYAGER DESTRUCTION AUDIT PASS %s"),Name);}
void Fail(const TCHAR* Name){Run->Finished=true;UE_LOG(LogTemp,Error,TEXT("VOYAGER DESTRUCTION AUDIT FAIL stage=%d %s"),Run->Stage,Name);FPlatformMisc::RequestExit(false);}
void Next(double Now,int32 Stage){Run->Stage=Stage;Run->StageStarted=Now;}
void Capture(const TCHAR* Name)
{
    if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerDestructionCapture")))return;
    FString Folder;if(!FParse::Value(FCommandLine::Get(),TEXT("VoyagerDestructionOutput="),Folder))Folder=FPaths::ProjectSavedDir()/TEXT("VoyagerDestructionAudit");
    IFileManager::Get().MakeDirectory(*Folder,true);FScreenshotRequest::RequestScreenshot(Folder/(FString(Name)+TEXT(".png")),true,false);
}
bool TraceWall(UWorld* World,const FVoyagerBuildingInfo& B,double Height,FHitResult& Hit)
{
    const FVector At=B.Floor+B.Right*(B.Depth*.17)+B.Up*Height;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(VoyagerDestructionTrace),false);
    // Query actual building collision. Dynamic fragments and pedestrians must
    // not conceal whether the original wall still exists.
    for(TActorIterator<AActor> It(World);It;++It)if(!Cast<AVoyagerSettlement>(*It))Query.AddIgnoredActor(*It);
    return World->LineTraceSingleByChannel(Hit,At+B.Forward*(B.Width*.5+500),At+B.Forward*(B.Width*.5-110),ECC_Visibility,Query);
}
bool Resolved(const FHitResult& Hit,int32 Building)
{
    int32 S=0,P=0,C=0,B=0;return Run->City.IsValid()&&Run->City->ResolveBuildingHit(Hit,S,P,C,B)&&S==Run->System&&P==0&&C==0&&B==Building;
}
void PlaceCamera(AVoyagerCharacter* Pawn)
{
    const auto& B=Run->Building;const FVector Candidate=B.Floor-B.Forward*(B.Width*.5+5200.f);
    const FVector Up=Voyager::SurfaceNormal(Run->System,0,Candidate);
    const FVector Position=Voyager::SurfacePoint(Run->System,0,Up,115.f);
    Pawn->GetCharacterMovement()->StopMovementImmediately();Pawn->GetCharacterMovement()->SetGravityDirection(-Up);
    Pawn->SetActorLocationAndRotation(Position,Voyager::TangentRotation(Up,B.Floor-Position),false,nullptr,ETeleportType::TeleportPhysics);
    Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
    Pawn->GetController()->SetControlRotation((B.Floor+B.Up*(B.Height*.43)-Pawn->GetPawnViewLocation()).Rotation());
}
void Tick(UWorld* World,ELevelTick Type,float Delta)
{
    if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerDestructionAudit"))||!World||!World->IsGameWorld())return;
    auto PC=Cast<AVoyagerController>(World->GetFirstPlayerController());auto Pawn=PC?Cast<AVoyagerCharacter>(PC->GetPawn()):nullptr;
    auto State=World->GetGameState<AVoyagerState>();auto Damage=AVoyagerDestruction::Find(World);
    if(!PC||!PC->IsLocalController()||!Pawn||!State||!Damage||World->GetTimeSeconds()<8.f)return;
    if(!Run){Run=MakeUnique<FRun>();Run->World=World;Run->Started=Run->StageStarted=FPlatformTime::Seconds();Run->System=State->SystemSeed;}
    if(Run->Finished||Run->World.Get()!=World)return;
    const double Now=FPlatformTime::Seconds(),Elapsed=Now-Run->StageStarted;
    if(Now-Run->Started>120){Fail(TEXT("TIMEOUT"));return;}
    if(Now<Run->NextPoll)return;Run->NextPoll=Now+.4;
    if(!Run->City.IsValid())for(TActorIterator<AVoyagerSettlement> It(World);It;++It)if(It->VisiblePieceCount(0,0,0)>0){Run->City=*It;break;}
    auto City=Run->City.Get();if(!City)return;
    const FVoyagerBuildingDamage* Entry=Damage->FindDamage(Run->System,0,0,0);
    if(World->GetNetMode()==NM_Client)
    {
        if(Run->Stage==0)
        {
            if(Entry)return;
            Run->Before=City->VisiblePieceCount(0,0,0);Run->OtherBefore=City->VisiblePieceCount(0,0,1);
            if(Run->Before<=0||Run->OtherBefore<=0)return;
            Pass(TEXT("REMOTE_INTACT_BUILDING"));Next(Now,1);
        }
        else if(Run->Stage==1&&Entry&&(Entry->BrokenGlassFloors&1u)&&City->VisiblePieceCount(0,0,0)<Run->Before)
        {Pass(TEXT("REMOTE_BROKEN_GLASS"));Next(Now,2);}
        else if(Run->Stage==2&&Entry&&Entry->Integrity<=650&&Entry->Integrity>0)
        {Run->AfterPartial=City->VisiblePieceCount(0,0,0);Pass(TEXT("REMOTE_PARTIAL_COLLAPSE"));Next(Now,3);}
        else if(Run->Stage==3&&Entry&&Entry->Integrity==0&&City->VisiblePieceCount(0,0,0)<Run->AfterPartial)
        {
            if(City->VisiblePieceCount(0,0,1)!=Run->OtherBefore){Fail(TEXT("REMOTE_UNRELATED_BUILDING"));return;}
            if(Damage->DamageBuilding(Run->System,0,0,1,200,Pawn->GetActorLocation(),PC)||Damage->FindDamage(Run->System,0,0,1))
            {Fail(TEXT("REMOTE_DESTRUCTION_MUTATION"));return;}
            int32 Fragments=0;for(TActorIterator<AVoyagerDebris> It(World);It;++It)++Fragments;
            if(Fragments<1){Fail(TEXT("REMOTE_DEBRIS_MISSING"));return;}
            Pass(TEXT("REMOTE_FULL_COLLAPSE"));Pass(TEXT("REMOTE_DESTRUCTION_AUTHORITY_GUARD"));Pass(TEXT("REMOTE_DEBRIS_PRESENT"));
            Run->Finished=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER DESTRUCTION AUDIT CLIENT COMPLETE PASS"));FPlatformMisc::RequestExit(false);
        }
        return;
    }
    switch(Run->Stage)
    {
    case 0:
        if(!AVoyagerSettlement::GetBuildingInfo(Run->System,0,0,0,Run->Building)||!AVoyagerSettlement::GetBuildingInfo(Run->System,0,0,1,Run->Other)||Run->Building.FloorCount<3)
        {Fail(TEXT("BUILDING_LAYOUT"));return;}
        if(Entry){Fail(TEXT("DIRTY_AUDIT_WORLD"));return;}
        Run->Before=City->VisiblePieceCount(0,0,0);Run->OtherBefore=City->VisiblePieceCount(0,0,1);
        if(Run->Before<20||Run->OtherBefore<20)return;
        PlaceCamera(Pawn);Pass(TEXT("INTACT_BUILDINGS_READY"));Next(Now,1);return;
    case 1:
        if(Elapsed<1.2)return;Capture(TEXT("Intact"));Next(Now,2);return;
    case 2:
    {
        if(Elapsed<(World->GetNetMode()==NM_ListenServer?12.0:1.2))return;
        if(Damage->DamageBuilding(Run->System,0,0,0,-1,Run->Building.Floor,nullptr)||Damage->DamageBuilding(Run->System,0,0,60,100,Run->Building.Floor,nullptr)||Damage->FindDamage(Run->System,0,0,0))
        {Fail(TEXT("INVALID_DAMAGE"));return;}Pass(TEXT("INVALID_BUILDING_DAMAGE_REJECTED"));
        if(!TraceWall(World,Run->Building,215,Run->GlassHit)||!Resolved(Run->GlassHit,0)||!Run->GlassHit.GetComponent()->ComponentHasTag(TEXT("VoyagerGlass")))
        {Fail(TEXT("REAL_GLASS_TRACE"));return;}Pass(TEXT("REAL_TRACE_RESOLVES_WINDOW"));
        if(!TraceWall(World,Run->Building,(Run->Building.FloorCount-1)*Run->Building.FloorHeight+60,Run->WallHit)||!Resolved(Run->WallHit,0)||Run->WallHit.GetComponent()->ComponentHasTag(TEXT("VoyagerGlass")))
        {Fail(TEXT("REAL_UPPER_WALL_TRACE"));return;}Pass(TEXT("REAL_TRACE_RESOLVES_UPPER_WALL"));
        if(!Damage->DamageHit(Run->GlassHit,34,nullptr)){Fail(TEXT("GLASS_DAMAGE_PATH"));return;}
        for(TActorIterator<AVoyagerDebris> It(World);It;++It)if(It->Body&&It->Body->IsSimulatingPhysics())Run->DebrisPositions.Add(*It,It->GetActorLocation());
        if(Run->DebrisPositions.IsEmpty()){Fail(TEXT("NATIVE_CHAOS_BODY"));return;}Pass(TEXT("NATIVE_CHAOS_BODIES_SIMULATE"));Next(Now,3);return;
    }
    case 3:
    {
        if(Elapsed<1.2)return;
        Run->AfterGlass=City->VisiblePieceCount(0,0,0);
        if(!Entry||Entry->Integrity!=1200||Entry->BrokenGlassFloors!=1u||Run->AfterGlass>=Run->Before){Fail(TEXT("GLASS_PRESENTATION"));return;}
        FHitResult Hit;if(TraceWall(World,Run->Building,215,Hit)){Fail(TEXT("GLASS_COLLISION_REMAINS"));return;}
        if(Damage->DamageHit(Run->GlassHit,34,nullptr)){Fail(TEXT("REPEATED_WINDOW_DAMAGE"));return;}
        Pass(TEXT("GLASS_BREAK_REMOVES_PIECES_AND_COLLISION"));Pass(TEXT("REPEATED_GLASS_BREAK_REJECTED"));
        bool Moved=false;for(const auto& Pair:Run->DebrisPositions)if(Pair.Key.IsValid()&&FVector::DistSquared(Pair.Key->GetActorLocation(),Pair.Value)>25.f){Moved=true;break;}
        if(!Moved){Fail(TEXT("CHAOS_DEBRIS_STATIC"));return;}Pass(TEXT("CHAOS_DEBRIS_ACTUALLY_MOVES"));
        Capture(TEXT("GlassBroken"));Next(Now,4);return;
    }
    case 4:
        if(Elapsed<1.2)return;
        if(!Damage->DamageHit(Run->WallHit,600,nullptr)){Fail(TEXT("STRUCTURE_DAMAGE_PATH"));return;}
        Next(Now,5);return;
    case 5:
    {
        if(Elapsed<1.2)return;
        Run->AfterPartial=City->VisiblePieceCount(0,0,0);
        if(!Entry||Entry->Integrity!=600||AVoyagerDestruction::SurvivingFloors(Entry,Run->Building.FloorCount)!=FMath::Max(1,Run->Building.FloorCount*2/3)||Run->AfterPartial>=Run->AfterGlass)
        {Fail(TEXT("PARTIAL_COLLAPSE"));return;}
        FHitResult Hit;if(TraceWall(World,Run->Building,(Run->Building.FloorCount-1)*Run->Building.FloorHeight+60,Hit)){Fail(TEXT("REMOVED_UPPER_WALL_COLLISION"));return;}
        Pass(TEXT("STRUCTURAL_THRESHOLD_REMOVES_UPPER_FLOORS"));Pass(TEXT("COLLAPSED_UPPER_STOREY_HAS_NO_COLLISION"));
        if(City->VisiblePieceCount(0,0,1)!=Run->OtherBefore||!TraceWall(World,Run->Other,60,Hit)||!Resolved(Hit,1))
        {Fail(TEXT("UNRELATED_BUILDING_CHANGED"));return;}Pass(TEXT("OTHER_BUILDING_REMAINS_INTACT"));
        Capture(TEXT("PartialCollapse"));Next(Now,6);return;
    }
    case 6:
        if(Elapsed<1.2)return;
        if(!Damage->DamageBuilding(Run->System,0,0,0,300,Run->Building.Floor+Run->Building.Up*350,nullptr)){Fail(TEXT("SECOND_THRESHOLD_DAMAGE"));return;}
        Entry=Damage->FindDamage(Run->System,0,0,0);
        if(!Entry||Entry->Integrity!=300||AVoyagerDestruction::SurvivingFloors(Entry,Run->Building.FloorCount)!=FMath::Max(1,Run->Building.FloorCount/3))
        {Fail(TEXT("SECOND_THRESHOLD"));return;}Pass(TEXT("SECOND_STRUCTURAL_THRESHOLD"));Next(Now,7);return;
    case 7:
        if(Elapsed<1.2)return;
        if(!Damage->DamageBuilding(Run->System,0,0,0,300,Run->Building.Floor+Run->Building.Up*350,nullptr)){Fail(TEXT("FINAL_COLLAPSE_DAMAGE"));return;}
        Next(Now,8);return;
    case 8:
        if(Elapsed<1.5)return;
        Run->AfterFull=City->VisiblePieceCount(0,0,0);
        if(!Entry||Entry->Integrity!=0||AVoyagerDestruction::SurvivingFloors(Entry,Run->Building.FloorCount)!=0||Run->AfterFull<=0||Run->AfterFull>=Run->AfterPartial)
        {Fail(TEXT("FINAL_COLLAPSE"));return;}
        Pass(TEXT("FULL_COLLAPSE_PERSISTS_FOUNDATION"));Capture(TEXT("Rubble"));
        Damage->EmitDebris(Run->Building.Floor+Run->Building.Up*(Run->Building.Height+1200),Run->Building.Up,Run->System,0,200);
        if(Damage->DebrisCount()!=96){Fail(TEXT("DEBRIS_BOUND"));return;}
        Damage->EmitDebris(Run->Building.Floor,Run->Building.Up,Run->System,0,12);
        if(Damage->DebrisCount()!=96){Fail(TEXT("DEBRIS_BOUND_EXCEEDED"));return;}Pass(TEXT("DEBRIS_HARD_LIMIT_96"));
        PC->ServerSave();Next(Now,9);return;
    case 9:
    {
        if(Elapsed<1.2)return;
        Run->Saved.Reset(Cast<UVoyagerSave>(UGameplayStatics::LoadGameFromSlot(TEXT("Voyager-Automation-Destruction"),0)));
        if(!Run->Saved.IsValid()){Fail(TEXT("DAMAGE_SAVE_MISSING"));return;}
        int32 Matching=0;for(const auto& D:Run->Saved->BuildingDamage)if(D.Matches(Run->System,0,0,0)&&D.Integrity==0&&D.BrokenGlassFloors==1u)++Matching;
        if(Matching!=1){Fail(TEXT("DAMAGE_SAVE_CONTENTS"));return;}Pass(TEXT("BUILDING_DAMAGE_WRITTEN_TO_DISK"));
        auto Empty=NewObject<UVoyagerSave>();Damage->RestoreFrom(Empty);Next(Now,10);return;
    }
    case 10:
    {
        if(Elapsed<1.2)return;FHitResult Hit;
        if(Damage->FindDamage(Run->System,0,0,0)||City->VisiblePieceCount(0,0,0)!=Run->Before||!TraceWall(World,Run->Building,(Run->Building.FloorCount-1)*Run->Building.FloorHeight+60,Hit)||!Resolved(Hit,0))
        {Fail(TEXT("RESTORE_CLEARS_OLD_STATE"));return;}Pass(TEXT("CLEAN_RESTORE_REBUILDS_INTACT_COLLISION"));
        Damage->RestoreFrom(Run->Saved.Get());Next(Now,11);return;
    }
    case 11:
    {
        if(Elapsed<1.2)return;FHitResult Hit;
        Entry=Damage->FindDamage(Run->System,0,0,0);
        auto RoundTrip=NewObject<UVoyagerSave>();Damage->SaveTo(RoundTrip);
        if(!Entry||Entry->Integrity!=0||Entry->BrokenGlassFloors!=1u||RoundTrip->BuildingDamage.Num()!=Run->Saved->BuildingDamage.Num()||City->VisiblePieceCount(0,0,0)!=Run->AfterFull||TraceWall(World,Run->Building,(Run->Building.FloorCount-1)*Run->Building.FloorHeight+60,Hit))
        {Fail(TEXT("DISK_DAMAGE_RESTORE"));return;}Pass(TEXT("DISK_RESTORE_REAPPLIES_DAMAGE_AND_COLLISION"));
        auto PS=PC->GetPlayerState<AVoyagerPlayerState>();
        if(!PS){Fail(TEXT("CHARGE_PLAYER_STATE"));return;}
        const int32 BeforeCharges=PS->ItemCount(EVoyagerItem::DemolitionCharge);
        if(!PS->AddItem(EVoyagerItem::DemolitionCharge,1)){Fail(TEXT("CHARGE_FIXTURE_INVENTORY"));return;}
        const auto& B=Run->Other;
        const FVector Candidate=B.Floor+B.Forward*(B.Width*.5+850)+B.Right*(B.Depth*.17);
        const FVector Up=Voyager::SurfaceNormal(Run->System,0,Candidate);
        const FVector At=Voyager::SurfacePoint(Run->System,0,Up,115);
        Pawn->GetCharacterMovement()->StopMovementImmediately();Pawn->GetCharacterMovement()->SetGravityDirection(-Up);
        Pawn->SetActorLocationAndRotation(At,Voyager::TangentRotation(Up,-B.Forward),false,nullptr,ETeleportType::TeleportPhysics);
        Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
        PC->SetControlRotation((B.Floor+B.Forward*(B.Width*.5)+B.Right*(B.Depth*.17)+B.Up*395-Pawn->GetPawnViewLocation()).Rotation());
        PC->ServerSurvivalAction(7);
        for(TActorIterator<AVoyagerDemolitionCharge> It(World);It;++It)if(It->GetOwner()==PC){Run->Charge=*It;break;}
        if(!Run->Charge.IsValid()||PS->ItemCount(EVoyagerItem::DemolitionCharge)!=BeforeCharges||Damage->FindDamage(Run->System,0,0,1))
        {Fail(TEXT("NORMAL_CHARGE_PLACEMENT"));return;}
        Pass(TEXT("BACKPACK_ACTION_PLACES_AND_CONSUMES_CHARGE"));
        PlaceCamera(Pawn);Next(Now,12);return;
    }
    case 12:
        if(Elapsed<1.0)return;
        if(!Run->Charge.IsValid()||Damage->FindDamage(Run->System,0,0,1)){Fail(TEXT("CHARGE_FUSE_TOO_SHORT"));return;}
        Pass(TEXT("ARMED_CHARGE_OBSERVES_FUSE"));Next(Now,13);return;
    case 13:
    {
        if(Elapsed<2.8)return;
        const auto Blasted=Damage->FindDamage(Run->System,0,0,1);
        if(Run->Charge.IsValid()||!Blasted||Blasted->Integrity!=450.f||Damage->DebrisCount()>96)
        {Fail(TEXT("TIMED_CHARGE_STRUCTURAL_DAMAGE"));return;}
        Pass(TEXT("TIMED_CHARGE_EXPLODES_AND_DAMAGES_REAL_BUILDING"));
        Run->Finished=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER DESTRUCTION AUDIT COMPLETE PASS checks=%d seconds=%.2f"),Run->Checks,Now-Run->Started);FPlatformMisc::RequestExit(false);return;
    }
    }
}
struct FHook{FHook(){FWorldDelegates::OnWorldPostActorTick.AddStatic(&Tick);}} Hook;
}
