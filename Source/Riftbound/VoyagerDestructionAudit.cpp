#include "VoyagerDestruction.h"
#include "VoyagerSettlement.h"
#include "VoyagerCharacter.h"
#include "VoyagerGameMode.h"
#include "VoyagerCityLife.h"
#include "VoyagerData.h"
#include "Components/StaticMeshComponent.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
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
#include "UObject/UnrealType.h"
#include "UnrealClient.h"

namespace VoyagerDestructionAudit
{
struct FRun
{
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AVoyagerSettlement> City;
    TWeakObjectPtr<AVoyagerDemolitionCharge> Charge;
    TStrongObjectPtr<UVoyagerSave> Saved;
    TStrongObjectPtr<UVoyagerSave> SupportSaved;
    TMap<TWeakObjectPtr<AVoyagerDebris>,FVector> DebrisPositions;
    TMap<TWeakObjectPtr<AVoyagerDebris>,FVector> PoolPositions;
    TMap<TWeakObjectPtr<AVoyagerDebris>,uint16> ObservedPieceSerials;
    FVoyagerBuildingInfo Building,Other,SupportBuilding;
    FHitResult GlassHit,WallHit;
    double Started=0,StageStarted=0,NextPoll=0;
    int32 Stage=0,Checks=0,Before=0,OtherBefore=0,AfterGlass=0,AfterPartial=0,AfterFull=0,System=1;
    int32 SupportBefore=0,SupportPartial=0,RecycledPoseChecks=0;
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
bool CheckDebrisMaterials(UWorld* World,bool NeedConcrete)
{
    int32 Glass=0,Concrete=0;
    float MaximumGlassMass=0,MinimumConcreteMass=MAX_flt;
    for(TActorIterator<AVoyagerDebris> It(World);It;++It)
    {
        auto* Body=It->Body.Get();if(!Body||!Body->IsSimulatingPhysics())continue;
        const auto* Material=Body->BodyInstance.GetSimplePhysicalMaterial();
        if(!Material||!FMath::IsFinite(Body->GetMass())||Body->GetMass()<=0)return false;
        if(FMath::IsNearlyEqual(Material->Density,2.5f,.01f))
        {
            if(!FMath::IsNearlyEqual(Material->Friction,.35f,.01f)||!FMath::IsNearlyEqual(Material->Restitution,.18f,.01f)||
                Body->GetMass()>1.6f||!FMath::IsNearlyEqual(Body->GetLinearDamping(),.6f,.01f))return false;
            ++Glass;MaximumGlassMass=FMath::Max(MaximumGlassMass,Body->GetMass());
        }
        else if(FMath::IsNearlyEqual(Material->Density,2.3f,.01f))
        {
            if(!FMath::IsNearlyEqual(Material->Friction,.8f,.01f)||!FMath::IsNearlyEqual(Material->Restitution,.08f,.01f)||
                Body->GetMass()<8.f||!FMath::IsNearlyEqual(Body->GetLinearDamping(),.28f,.01f))return false;
            ++Concrete;MinimumConcreteMass=FMath::Min(MinimumConcreteMass,Body->GetMass());
        }
        else return false;
        if(Body->GetCollisionResponseToChannel(ECC_Pawn)!=ECR_Ignore||Body->GetCollisionResponseToChannel(ECC_Visibility)!=ECR_Ignore)return false;
    }
    return Glass>0&&(!NeedConcrete||(Concrete>0&&MinimumConcreteMass>MaximumGlassMass));
}
bool ObserveReplicatedRecycling(UWorld* World)
{
    // Observe real replicated fields immediately after actor ticks, not the
    // quarter-second scenario poll. Calling OnRep manually would hide a broken
    // wire path, and waiting until interpolation finishes would hide a bad snap.
    const auto* SerialProperty=FindFProperty<FUInt16Property>(AVoyagerDebris::StaticClass(),TEXT("PieceSerial"));
    const auto* PositionProperty=FindFProperty<FStructProperty>(AVoyagerDebris::StaticClass(),TEXT("Position"));
    const auto* RotationProperty=FindFProperty<FStructProperty>(AVoyagerDebris::StaticClass(),TEXT("Rotation"));
    if(!SerialProperty||!PositionProperty||!RotationProperty)return false;
    for(TActorIterator<AVoyagerDebris> It(World);It;++It)
    {
        const uint16 Serial=SerialProperty->GetPropertyValue_InContainer(*It);
        const auto* Prior=Run->ObservedPieceSerials.Find(*It);
        if(Prior&&*Prior>0&&*Prior!=Serial)
        {
            const auto* Position=PositionProperty->ContainerPtrToValuePtr<FVector>(*It);
            const auto* Rotation=RotationProperty->ContainerPtrToValuePtr<FQuat>(*It);
            if(!Position||!Rotation||FVector::DistSquared(It->GetActorLocation(),*Position)>1.0||
                It->GetActorQuat().AngularDistance(*Rotation)>.001)
            {UE_LOG(LogTemp,Error,TEXT("VOYAGER DESTRUCTION AUDIT RECYCLE SNAP error_cm=%.3f old_serial=%u new_serial=%u"),
                Position?FVector::Dist(It->GetActorLocation(),*Position):-1.,uint32(*Prior),uint32(Serial));return false;}
            ++Run->RecycledPoseChecks;
        }
        Run->ObservedPieceSerials.Add(*It,Serial);
    }
    return true;
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
    if(World->GetNetMode()==NM_Client&&!ObserveReplicatedRecycling(World))
    {Fail(TEXT("REMOTE_RECYCLED_FRAGMENT_DID_NOT_SNAP"));return;}
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
            Run->SupportBefore=City->VisiblePieceCount(0,0,10);
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
            int32 Fragments=0;for(TActorIterator<AVoyagerDebris> It(World);It;++It)
            {
                if(!It->Body||It->Body->IsSimulatingPhysics()||It->Body->GetCollisionEnabled()!=ECollisionEnabled::NoCollision)
                {Fail(TEXT("REMOTE_DEBRIS_SIMULATES_OR_COLLIDES"));return;}
                ++Fragments;
            }
            if(Fragments<1){Fail(TEXT("REMOTE_DEBRIS_MISSING"));return;}
            Pass(TEXT("REMOTE_FULL_COLLAPSE"));Pass(TEXT("REMOTE_DESTRUCTION_AUTHORITY_GUARD"));Pass(TEXT("REMOTE_DEBRIS_PRESENT"));
            Pass(TEXT("REMOTE_DEBRIS_IS_PRESENTATION_ONLY"));
            Next(Now,4);
        }
        else if(Run->Stage==4)
        {
            const auto* Local=Damage->FindDamage(Run->System,0,0,10);
            if(!Local||Local->Integrity!=840||Local->CollapsedFromFloor!=1||Run->RecycledPoseChecks<12)return;
            if(!AVoyagerSettlement::GetBuildingInfo(Run->System,0,0,10,Run->SupportBuilding))
            {Fail(TEXT("REMOTE_SUPPORT_BUILDING_INFO"));return;}
            if(Local->SupportDamage.Num()!=72||Local->SupportDamage[4]!=18000||Local->SupportDamage[5]!=18000||Local->BrokenGlassFloors!=1u)
            {Fail(TEXT("REMOTE_SUPPORT_DELTA_CONTENTS"));return;}
            Run->SupportPartial=City->VisiblePieceCount(0,0,10);FHitResult Hit;
            if(Run->SupportPartial>=Run->SupportBefore||TraceWall(World,Run->SupportBuilding,Run->SupportBuilding.FloorHeight+60,Hit))return;
            Pass(TEXT("REMOTE_RECYCLED_FRAGMENTS_SNAP_TO_REPLICATED_POSES"));
            Pass(TEXT("REMOTE_SUPPORT_DELTAS_AND_PARTIAL_COLLAPSE"));Next(Now,5);
        }
        else if(Run->Stage==5)
        {
            const auto* Local=Damage->FindDamage(Run->System,0,0,10);
            if(!Local||Local->Integrity!=480||Local->CollapsedFromFloor!=0)return;
            if(Local->SupportDamage.Num()!=72||Local->SupportDamage[0]!=18000||Local->SupportDamage[1]!=18000)
            {Fail(TEXT("REMOTE_GROUND_SUPPORT_DELTAS"));return;}
            FHitResult Hit;if(City->VisiblePieceCount(0,0,10)>=Run->SupportPartial||TraceWall(World,Run->SupportBuilding,60,Hit))return;
            if(Damage->DamageBuilding(Run->System,0,0,10,100,Run->SupportBuilding.Floor,PC)||Local->Integrity!=480)
            {Fail(TEXT("REMOTE_LOCAL_SUPPORT_MUTATION"));return;}
            Pass(TEXT("REMOTE_SUPPORT_LOSS_REMOVES_GROUND_STOREY"));Pass(TEXT("REMOTE_LOCAL_SUPPORT_AUTHORITY_GUARD"));
            Run->Finished=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER DESTRUCTION AUDIT CLIENT COMPLETE PASS recycled_poses=%d"),Run->RecycledPoseChecks);FPlatformMisc::RequestExit(false);
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
        if(Run->DebrisPositions.IsEmpty()){Fail(TEXT("NATIVE_CHAOS_BODY"));return;}Pass(TEXT("NATIVE_CHAOS_BODIES_SIMULATE"));
        if(!CheckDebrisMaterials(World,false)){Fail(TEXT("GLASS_MASS_AND_CONTACT"));return;}
        Pass(TEXT("GLASS_NATIVE_MASS_FRICTION_AND_DAMPING"));Next(Now,3);return;
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
        if(!CheckDebrisMaterials(World,true)){Fail(TEXT("CONCRETE_MASS_AND_CONTACT"));return;}
        Pass(TEXT("MATERIAL_AWARE_CHAOS_RESPONSE_AND_MASS"));
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
    {
        if(Elapsed<1.5)return;
        Run->AfterFull=City->VisiblePieceCount(0,0,0);
        if(!Entry||Entry->Integrity!=0||AVoyagerDestruction::SurvivingFloors(Entry,Run->Building.FloorCount)!=0||Run->AfterFull<=0||Run->AfterFull>=Run->AfterPartial)
        {Fail(TEXT("FINAL_COLLAPSE"));return;}
        Pass(TEXT("FULL_COLLAPSE_PERSISTS_FOUNDATION"));Capture(TEXT("Rubble"));
        Next(Now,81);return;
    }
    case 81:
    {
        // Screenshot requests resolve at end of frame. Keep the visual record of
        // real collapse separate from the synthetic airborne pool stress fixture.
        if(Elapsed<.4)return;
        Damage->EmitDebris(Run->Building.Floor+Run->Building.Up*(Run->Building.Height+1200),Run->Building.Up,Run->System,0,200);
        if(Damage->DebrisCount()!=96){Fail(TEXT("DEBRIS_BOUND"));return;}
        Run->PoolPositions.Reset();for(TActorIterator<AVoyagerDebris> It(World);It;++It)Run->PoolPositions.Add(*It,It->GetActorLocation());
        Next(Now,80);return;
    }
    case 80:
    {
        // Hold the full pool across several server updates so a peer can observe
        // its original serials and then the actual replicated reuse event.
        if(Elapsed<1.2)return;
        for(TActorIterator<AVoyagerDebris> It(World);It;++It)
        {
            auto* Old=Run->PoolPositions.Find(*It);if(!Old){Fail(TEXT("DEBRIS_POOL_CHANGED_DURING_HOLD"));return;}
            *Old=It->GetActorLocation();
        }
        Damage->EmitDebris(Run->Building.Floor,Run->Building.Up,Run->System,0,12);
        if(Damage->DebrisCount()!=96){Fail(TEXT("DEBRIS_BOUND_EXCEEDED"));return;}Pass(TEXT("DEBRIS_HARD_LIMIT_96"));
        int32 Reused=0,Existing=0;
        for(TActorIterator<AVoyagerDebris> It(World);It;++It)
        {
            const auto* Old=Run->PoolPositions.Find(*It);if(!Old){Fail(TEXT("DEBRIS_POOL_ALLOCATES_AT_CAP"));return;}
            ++Existing;if(FVector::DistSquared(*Old,It->GetActorLocation())>1000000.)++Reused;
        }
        if(Existing!=96||Reused!=12){Fail(TEXT("DEBRIS_POOL_DOES_NOT_RECYCLE"));return;}
        Pass(TEXT("CHAOS_POOL_REUSES_BODIES_WITHOUT_ACTOR_GROWTH"));
        PC->ServerSave();Next(Now,9);return;
    }
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
        Next(Now,14);return;
    }
    case 14:
    {
        if(!AVoyagerSettlement::GetBuildingInfo(Run->System,0,0,10,Run->SupportBuilding)||
            Run->SupportBuilding.FloorCount<3||Damage->FindDamage(Run->System,0,0,10))
        {Fail(TEXT("LOCAL_SUPPORT_FIXTURE"));return;}
        Run->SupportBefore=City->VisiblePieceCount(0,0,10);
        if(Run->SupportBefore<20){Fail(TEXT("LOCAL_SUPPORT_GEOMETRY"));return;}
        const auto& B=Run->SupportBuilding;
        const FVector Base=B.Floor+B.Up*(B.FloorHeight+60)-B.Right*(B.Depth*.3);
        if(!Damage->DamageBuilding(Run->System,0,0,10,34,B.Floor+B.Up*215,nullptr,true)||
            !Damage->DamageBuilding(Run->System,0,0,10,180,Base-B.Forward*(B.Width*.3),nullptr))
        {Fail(TEXT("FIRST_LOCAL_SUPPORT"));return;}
        const auto* First=Damage->FindDamage(Run->System,0,0,10);
        if(!First||First->Integrity!=1020||AVoyagerDestruction::SurvivingFloors(First,B.FloorCount)!=B.FloorCount)
        {Fail(TEXT("SINGLE_SUPPORT_REDUNDANCY"));return;}
        Pass(TEXT("ONE_FAILED_SUPPORT_RETAINS_STOREYS"));
        if(!Damage->DamageBuilding(Run->System,0,0,10,180,Base+B.Forward*(B.Width*.3),nullptr))
        {Fail(TEXT("SECOND_LOCAL_SUPPORT"));return;}
        Next(Now,15);return;
    }
    case 15:
    {
        if(Elapsed<1.2)return;
        const auto& B=Run->SupportBuilding;const auto* Local=Damage->FindDamage(Run->System,0,0,10);
        if(!Local||Local->Integrity!=840||Local->SupportDamage.Num()!=72||Local->CollapsedFromFloor!=1||
            Local->SupportDamage[4]!=18000||Local->SupportDamage[5]!=18000||
            AVoyagerDestruction::SurvivingFloors(Local,B.FloorCount)!=1||City->VisiblePieceCount(0,0,10)>=Run->SupportBefore)
        {Fail(TEXT("REGIONAL_SUPPORT_COLLAPSE"));return;}
        FHitResult Hit;
        if(TraceWall(World,B,B.FloorHeight+60,Hit)||!TraceWall(World,B,60,Hit))
        {Fail(TEXT("SUPPORT_COLLAPSE_COLLISION"));return;}
        Pass(TEXT("LOCAL_SUPPORT_LOSS_COLLAPSES_ONLY_UPPER_STOREYS"));
        Pass(TEXT("SUPPORT_COLLAPSE_UPDATES_REAL_COLLISION"));
        PC->ServerSave();Next(Now,16);return;
    }
    case 16:
    {
        if(Elapsed<1.2)return;
        Run->SupportSaved.Reset(Cast<UVoyagerSave>(UGameplayStatics::LoadGameFromSlot(TEXT("Voyager-Automation-Destruction"),0)));
        const auto* Saved=Run->SupportSaved.IsValid()?Run->SupportSaved->BuildingDamage.FindByPredicate([&](const auto& D){return D.Matches(Run->System,0,0,10);}):nullptr;
        if(!Saved||Saved->Integrity!=840||Saved->BrokenGlassFloors!=1u||Saved->SupportDamage.Num()!=72||Saved->CollapsedFromFloor!=1)
        {Fail(TEXT("SUPPORT_DISK_SAVE"));return;}
        Pass(TEXT("SUPPORT_DELTAS_SAVED_TO_DISK"));
        auto Invalid=NewObject<UVoyagerSave>();Invalid->BuildingDamage=Run->SupportSaved->BuildingDamage;
        for(auto& D:Invalid->BuildingDamage)if(D.Matches(Run->System,0,0,10))D.SupportDamage.SetNum(71);
        Damage->RestoreFrom(Invalid);
        if(Damage->FindDamage(Run->System,0,0,10)||!Damage->FindDamage(Run->System,0,0,0))
        {Fail(TEXT("INVALID_SUPPORT_SAVE_ACCEPTED"));return;}
        Pass(TEXT("MALFORMED_SUPPORT_DELTA_REJECTED"));
        auto Legacy=NewObject<UVoyagerSave>();FVoyagerBuildingDamage Old;
        Old.System=Run->System;Old.Building=10;Old.Integrity=600;Old.BrokenGlassFloors=4u;Legacy->BuildingDamage.Add(Old);
        Damage->RestoreFrom(Legacy);const auto* OldRestored=Damage->FindDamage(Run->System,0,0,10);
        if(!OldRestored||!OldRestored->SupportDamage.IsEmpty()||OldRestored->Integrity!=600||OldRestored->BrokenGlassFloors!=4u||
            AVoyagerDestruction::SurvivingFloors(OldRestored,Run->SupportBuilding.FloorCount)!=FMath::Max(1,Run->SupportBuilding.FloorCount*2/3))
        {Fail(TEXT("LEGACY_SUPPORT_MIGRATION"));return;}
        Pass(TEXT("LEGACY_INTEGRITY_AND_GLASS_SAVE_PRESERVED"));
        Damage->RestoreFrom(Run->SupportSaved.Get());Next(Now,17);return;
    }
    case 17:
    {
        if(Elapsed<1.2)return;
        const auto* Restored=Damage->FindDamage(Run->System,0,0,10);FHitResult Hit;
        if(!Restored||Restored->Integrity!=840||Restored->BrokenGlassFloors!=1u||Restored->SupportDamage.Num()!=72||
            Restored->SupportDamage[4]!=18000||Restored->SupportDamage[5]!=18000||Restored->CollapsedFromFloor!=1||
            AVoyagerDestruction::SurvivingFloors(Restored,Run->SupportBuilding.FloorCount)!=1||
            TraceWall(World,Run->SupportBuilding,Run->SupportBuilding.FloorHeight+60,Hit))
        {Fail(TEXT("SUPPORT_DISK_RESTORE"));return;}
        Pass(TEXT("SUPPORT_DISK_RESTORE_RETAINS_COLLAPSE_AND_GLASS"));
        const auto& B=Run->SupportBuilding;
        const FVector Base=B.Floor+B.Up*60-B.Right*(B.Depth*.3);
        if(!Damage->DamageBuilding(Run->System,0,0,10,180,Base-B.Forward*(B.Width*.3),nullptr)||
            !Damage->DamageBuilding(Run->System,0,0,10,180,Base+B.Forward*(B.Width*.3),nullptr))
        {Fail(TEXT("GROUND_SUPPORT_IMPACTS"));return;}
        const auto* Ground=Damage->FindDamage(Run->System,0,0,10);
        if(!Ground||Ground->Integrity!=480||AVoyagerDestruction::SurvivingFloors(Ground,B.FloorCount)!=0)
        {Fail(TEXT("POSITIVE_INTEGRITY_TOTAL_COLLAPSE"));return;}
        Pass(TEXT("GROUND_SUPPORT_LOSS_COLLAPSES_WITH_POSITIVE_INTEGRITY"));
        Pawn->GetCharacterMovement()->StopMovementImmediately();Pawn->GetCharacterMovement()->SetGravityDirection(-B.Up);
        Pawn->SetActorLocationAndRotation(B.InteriorPoint+B.Up*110,Voyager::TangentRotation(B.Up,B.Forward),false,nullptr,ETeleportType::TeleportPhysics);
        Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
        Next(Now,18);return;
    }
    case 18:
    {
        if(Elapsed<1.2)return;
        auto* Life=AVoyagerCityLife::Find(World);if(!Life){Fail(TEXT("COLLAPSED_SERVICE_HOST"));return;}
        const auto Before=Life->BuildView(PC);
        if(!Before.bAvailable||!Before.bAtCity||!Before.bOnFoot||Before.CurrentBuilding!=10)
        {if(Elapsed>5.0)Fail(TEXT("COLLAPSED_SERVICE_LOCATION"));return;}
        auto* PS=PC->GetPlayerState<AVoyagerPlayerState>();if(!PS){Fail(TEXT("COLLAPSED_SERVICE_PLAYER"));return;}
        const int32 Minerals=PS->Minerals;PC->Notice.Reset();
        Life->HandleAction(PC,uint8(EVoyagerCityAction::BuyGood),0);
        const auto After=Life->BuildView(PC);
        if(!PC->Notice.Contains(TEXT("This building has collapsed"))||PS->Minerals!=Minerals||After.CreditsMinor!=Before.CreditsMinor)
        {Fail(TEXT("COLLAPSED_BUILDING_SERVICE_ACCEPTED"));return;}
        Pass(TEXT("POSITIVE_INTEGRITY_RUINS_REFUSE_CITY_SERVICES"));
        PlaceCamera(Pawn);Damage->RestoreFrom(Run->SupportSaved.Get());Next(Now,19);return;
    }
    case 19:
    {
        if(Elapsed<1.2)return;
        const auto& B=Run->SupportBuilding;
        const auto* Before=Damage->FindDamage(Run->System,0,0,10);
        if(!Before||Before->Integrity!=840||AVoyagerDestruction::SurvivingFloors(Before,B.FloorCount)!=1)
        {Fail(TEXT("ABOVE_RUIN_BLAST_FIXTURE"));return;}
        Damage->Explode(B.Floor+B.Up*(B.FloorHeight+500),nullptr);
        const auto* Blasted=Damage->FindDamage(Run->System,0,0,10);
        if(!Blasted||Blasted->Integrity!=90||Damage->DebrisCount()>96)
        {Fail(TEXT("ABOVE_RUIN_BLAST_MISSES_REMAINING_STRUCTURE"));return;}
        Pass(TEXT("BLAST_ABOVE_RUIN_DAMAGES_REMAINING_SUPPORT"));
        Run->Finished=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER DESTRUCTION AUDIT COMPLETE PASS checks=%d seconds=%.2f"),Run->Checks,Now-Run->Started);FPlatformMisc::RequestExit(false);return;
    }
    }
}
struct FHook{FHook(){FWorldDelegates::OnWorldPostActorTick.AddStatic(&Tick);}} Hook;
}
