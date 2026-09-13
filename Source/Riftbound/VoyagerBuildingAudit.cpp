// Opt-in movement integration audit. Fixture teleports occur only between
// buildings; every tested doorway, stair flight and roof is reached by walking.
#include "VoyagerCharacter.h"
#include "VoyagerData.h"
#include "VoyagerGameMode.h"
#include "VoyagerSettlement.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

namespace VoyagerBuildingAudit
{
    struct FRun
    {
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<AVoyagerController> Controller;
        TWeakObjectPtr<AVoyagerCharacter> Explorer;
        FVoyagerBuildingInfo Building;
        TArray<FVector> Route;
        int32 Stage=0,Case=0,Level=0,Waypoint=0,Checks=0,Frames=0,Planet=0,Site=0,Index=0;
        double Started=0,StageStarted=0,WaypointStarted=0;
        bool bFinished=false;
    };
    TUniquePtr<FRun> Run;
    void Pass(const TCHAR* Check,const FString& Detail=FString())
    {++Run->Checks;UE_LOG(LogTemp,Display,TEXT("VOYAGER BUILDING AUDIT PASS %s %s"),Check,*Detail);}
    void Fail(const TCHAR* Why)
    {
        Run->bFinished=true;
        UE_LOG(LogTemp,Error,TEXT("VOYAGER BUILDING AUDIT FAIL %s stage=%d case=%d floor=%d waypoint=%d position=%s"),Why,Run->Stage,Run->Case,Run->Level,Run->Waypoint,
            Run->Explorer.IsValid()?*Run->Explorer->GetActorLocation().ToString():TEXT("invalid"));
        FPlatformMisc::RequestExit(false);
    }
    void Advance(int32 Stage,double Now){Run->Stage=Stage;Run->StageStarted=Now;Run->WaypointStarted=Now;}
    void Capture(const FString& Name)
    {
        if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerBuildingCapture")))return;
        FString Directory;
        if(!FParse::Value(FCommandLine::Get(),TEXT("VoyagerBuildingOutput="),Directory))
            Directory=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Screenshots/VoyagerBuildings"));
        IFileManager::Get().MakeDirectory(*Directory,true);
        FScreenshotRequest::RequestScreenshot(Directory/(Name+TEXT(".png")),false,false);
    }
    void Fixture(const FVector& Position)
    {
        auto* E=Run->Explorer.Get();const auto& B=Run->Building;
        E->SetBase(static_cast<FMovementBaseInterfaceData*>(nullptr));
        E->GetCharacterMovement()->StopMovementImmediately();
        const FRotator Facing=Voyager::TangentRotation(B.Up,B.Right);
        E->SetActorLocationAndRotation(Position,Facing,false,nullptr,ETeleportType::TeleportPhysics);
        E->GetCharacterMovement()->SetGravityDirection(-B.Up);
        E->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
        Run->Controller->SetControlRotation(Facing);
    }
    bool Walk(const FVector& Surface,double Now)
    {
        auto* E=Run->Explorer.Get();const auto& B=Run->Building;
        const FVector Offset=FVector::VectorPlaneProject(Surface-E->GetActorLocation(),B.Up);
        const double Height=FVector::DotProduct(E->GetActorLocation()-Surface,B.Up);
        const double Half=E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
        if(Offset.Size()>38)
        {
            E->AddMovementInput(Offset.GetSafeNormal(),FMath::Clamp(Offset.Size()/110.,.25,1.));
            Run->Controller->SetControlRotation(Voyager::TangentRotation(B.Up,Offset.GetSafeNormal()));
        }
        else if(E->GetCharacterMovement()->IsMovingOnGround()&&FMath::Abs(Height-Half)<40)
        {E->GetCharacterMovement()->StopMovementImmediately();return true;}
        if(Now-Run->WaypointStarted>16)Fail(TEXT("WALK_TARGET_BLOCKED"));
        return false;
    }
    bool CheckCityGeometry(int32 System,int32 Planet,int32 Site)
    {
        auto* World=Run->World.Get();FCollisionQueryParams Q(SCENE_QUERY_STAT(VoyagerBuildingAudit),false);
        Q.AddIgnoredActor(Run->Explorer.Get());int32 Floors=0;
        for(int32 Index=0;Index<AVoyagerSettlement::BuildingCount();++Index)
        {
            FVoyagerBuildingInfo B;if(!AVoyagerSettlement::GetBuildingInfo(System,Planet,Site,Index,B))return false;
            if(B.FloorCount<3||B.Height!=B.FloorCount*B.FloorHeight)return false;
            for(int32 Level=0;Level<=B.FloorCount;++Level)
            {
                FVector Landing;if(!AVoyagerSettlement::GetFloorLanding(System,Planet,Site,Index,Level,Landing))return false;
                FHitResult Hit;
                if(!World->LineTraceSingleByChannel(Hit,Landing+B.Up*45,Landing-B.Up*35,ECC_Visibility,Q)||
                    FVector::DotProduct(Hit.ImpactNormal,B.Up)<.85)return false;
                // An entire capsule fits above the actual landing, including
                // the roof. A decorative solid upper tower fails this check.
                if(World->OverlapBlockingTestByChannel(Landing+B.Up*92,B.Rotation,ECC_Pawn,
                    FCollisionShape::MakeCapsule(34.f,86.f),Q))return false;
                if(Level<B.FloorCount)
                {
                    TArray<FVector> Route;
                    if(!AVoyagerSettlement::GetStairRoute(System,Planet,Site,Index,Level,Route)||Route.Num()!=4)return false;
                    const FVector W=B.Floor+B.Forward*(B.Width*.5)+B.Right*155+B.Up*(Level*B.FloorHeight+215);
                    if(!World->LineTraceSingleByChannel(Hit,W-B.Forward*65,W+B.Forward*65,ECC_Visibility,Q))return false;
                }
                ++Floors;
            }
            FHitResult DoorHit;
            const FVector Door=B.Floor-B.Right*(B.Depth*.5)+B.Up*160;
            if(World->SweepSingleByChannel(DoorHit,Door-B.Right*120,Door+B.Right*220,B.Rotation,ECC_Pawn,
                FCollisionShape::MakeCapsule(34.f,86.f),Q))return false;
        }
        Pass(TEXT("ALL_BUILDING_GEOMETRY"),FString::Printf(TEXT("planet=%d site=%d buildings=60 floors_and_roofs=%d"),Planet,Site,Floors));
        return true;
    }
    void Tick(UWorld* World,ELevelTick TickType,float DeltaSeconds)
    {
        static const bool Enabled=FParse::Param(FCommandLine::Get(),TEXT("VoyagerBuildingAudit"));
        if(!Enabled||!World||!World->IsGameWorld()||!World->GetAuthGameMode<AVoyagerGameMode>())return;
        if(!Run)
        {
            auto* PC=Cast<AVoyagerController>(World->GetFirstPlayerController());
            auto* E=PC?Cast<AVoyagerCharacter>(PC->GetPawn()):nullptr;if(!E)return;
            Run=MakeUnique<FRun>();Run->World=World;Run->Controller=PC;Run->Explorer=E;
            Run->Started=Run->StageStarted=FPlatformTime::Seconds();
        }
        if(Run->bFinished||Run->World.Get()!=World)return;
        if(!Run->Explorer.IsValid()||!Run->Controller.IsValid()){Fail(TEXT("EXPLORER_LOST"));return;}
        const double Now=FPlatformTime::Seconds(),Elapsed=Now-Run->StageStarted;
        ++Run->Frames;
        if(Now-Run->Started>650){Fail(TEXT("TIMEOUT"));return;}
        auto* State=World->GetGameState<AVoyagerState>();if(!State)return;
        auto& B=Run->Building;
        switch(Run->Stage)
        {
        case 0:
            if(Elapsed<5)return;
            Run->Planet=Run->Case==3?1:0;Run->Site=Run->Case==3?1:0;
            Run->Index=Run->Case==0?0:Run->Case==1?4:1;
            if(Run->Case==2)
            {
                int32 Highest=0;
                for(int32 I=0;I<60;++I)
                {FVoyagerBuildingInfo Info;AVoyagerSettlement::GetBuildingInfo(State->SystemSeed,0,0,I,Info);if(Info.FloorCount>Highest){Highest=Info.FloorCount;Run->Index=I;}}
            }
            if(!AVoyagerSettlement::GetBuildingInfo(State->SystemSeed,Run->Planet,Run->Site,Run->Index,B)){Fail(TEXT("BUILDING_INFO"));return;}
            Fixture(B.DoorOutside+B.Up*150);Advance(1,Now);return;
        case 1:
            if(Elapsed<5)return;
            if((Run->Case==0||Run->Case==3)&&!CheckCityGeometry(State->SystemSeed,Run->Planet,Run->Site)){Fail(TEXT("CITY_GEOMETRY"));return;}
            Advance(2,Now);return;
        case 2:
            if(!Walk(B.DoorInside,Now))return;
            Pass(TEXT("DOOR_ENTRY"),FString::Printf(TEXT("case=%d role=%d"),Run->Case,B.Role));
            Capture(FString::Printf(TEXT("BuildingDoor%d"),Run->Case));
            Run->Level=0;Run->Waypoint=0;
            AVoyagerSettlement::GetStairRoute(State->SystemSeed,Run->Planet,Run->Site,Run->Index,0,Run->Route);
            // First cross the empty front corridor, then approach the stair mouth.
            Run->Route.Insert(B.Floor+B.Forward*(B.Width*.5-480)+B.Right*(-B.Depth*.5+245)+B.Up*2,0);
            Run->Route.Insert(B.Floor+B.Right*(-B.Depth*.5+245)+B.Up*2,0);
            Advance(3,Now);return;
        case 3:
            if(!Walk(Run->Route[Run->Waypoint],Now))return;
            if(Run->Case==0&&Run->Level==0&&Run->Waypoint==Run->Route.Num()-3)
            {
                ++Run->Waypoint;
                Run->Controller->SetControlRotation(FRotationMatrix::MakeFromXZ((-B.Right-B.Up*.32).GetSafeNormal(),B.Up).Rotator());
                Advance(9,Now);return;
            }
            if(++Run->Waypoint<Run->Route.Num()){Run->WaypointStarted=Now;return;}
            ++Run->Level;
            Pass(TEXT("STAIR_ASCENT"),FString::Printf(TEXT("case=%d floor=%d grounded=true"),Run->Case,Run->Level));
            if(Run->Level<B.FloorCount)
            {
                AVoyagerSettlement::GetStairRoute(State->SystemSeed,Run->Planet,Run->Site,Run->Index,Run->Level,Run->Route);
                Run->Waypoint=0;Run->WaypointStarted=Now;return;
            }
            Run->Controller->SetControlRotation(Voyager::TangentRotation(B.Up,-B.Forward+B.Right*.3));
            Capture(FString::Printf(TEXT("BuildingRoof%d"),Run->Case));
            Pass(TEXT("ROOF_REACHED"),FString::Printf(TEXT("case=%d floors=%d height_cm=%.0f"),Run->Case,B.FloorCount,B.Height));
            Advance(4,Now);return;
        case 4:
            if(Elapsed<1)return;
            Run->Level=B.FloorCount-1;
            AVoyagerSettlement::GetStairRoute(State->SystemSeed,Run->Planet,Run->Site,Run->Index,Run->Level,Run->Route);
            Run->Waypoint=Run->Route.Num()-1;Advance(5,Now);return;
        case 5:
            if(!Walk(Run->Route[Run->Waypoint],Now))return;
            if(--Run->Waypoint>=0){Run->WaypointStarted=Now;return;}
            Pass(TEXT("STAIR_DESCENT"),FString::Printf(TEXT("case=%d floor=%d grounded=true"),Run->Case,Run->Level));
            if(--Run->Level>=0)
            {
                AVoyagerSettlement::GetStairRoute(State->SystemSeed,Run->Planet,Run->Site,Run->Index,Run->Level,Run->Route);
                Run->Waypoint=Run->Route.Num()-1;Run->WaypointStarted=Now;return;
            }
            Run->Waypoint=0;Advance(6,Now);return;
        case 6:
            if(Run->Waypoint==0)
            {
                if(!Walk(B.Floor+B.Forward*(B.Width*.5-480)+B.Right*(-B.Depth*.5+245)+B.Up*2,Now))return;
                ++Run->Waypoint;Run->WaypointStarted=Now;return;
            }
            if(!Walk(B.Floor+B.Right*(-B.Depth*.5+245)+B.Up*2,Now))return;
            Advance(7,Now);return;
        case 7:
            if(!Walk(B.DoorOutside,Now))return;
            Pass(TEXT("DOOR_EXIT"),FString::Printf(TEXT("case=%d returned_from_roof=true"),Run->Case));
            if(++Run->Case<(FParse::Param(FCommandLine::Get(),TEXT("VoyagerBuildingVisualAudit"))?1:4)){Advance(0,Now);return;}
            Advance(8,Now);return;
        case 8:
            if(Elapsed<1)return;
            Run->bFinished=true;
            UE_LOG(LogTemp,Display,TEXT("VOYAGER BUILDING PERF frames=%d seconds=%.2f fps=%.2f includes_capture=true"),Run->Frames,Now-Run->Started,Run->Frames/(Now-Run->Started));
            UE_LOG(LogTemp,Display,TEXT("VOYAGER BUILDING AUDIT COMPLETE PASS checks=%d cases=%d physical_stairs=true"),Run->Checks,Run->Case);
            FPlatformMisc::RequestExit(false);return;
        case 9:
            if(Elapsed<.15)return;
            Capture(TEXT("BuildingStairs0"));Advance(10,Now);return;
        case 10:
            if(Elapsed<.2)return;
            Advance(3,Now);return;
        default:Fail(TEXT("INVALID_STAGE"));return;
        }
    }
    struct FRegistration
    {
        FDelegateHandle Handle;
        FRegistration(){Handle=FWorldDelegates::OnWorldPostActorTick.AddStatic(&Tick);}
        ~FRegistration(){FWorldDelegates::OnWorldPostActorTick.Remove(Handle);}
    };
    FRegistration Registration;
}
