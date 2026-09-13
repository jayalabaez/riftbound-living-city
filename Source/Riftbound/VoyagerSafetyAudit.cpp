#include "VoyagerLanding.h"
#include "VoyagerShip.h"
#include "VoyagerCharacter.h"
#include "VoyagerGameMode.h"
#include "VoyagerData.h"
#include "RiftVisual.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"

namespace VoyagerSafetyAudit {
struct FRun {TWeakObjectPtr<UWorld> World;TWeakObjectPtr<AVoyagerShip> Ship;FVector At;FRotator Facing;int32 Stage=0,Checks=0;double Started=0,Time=0;bool Done=false;};
TUniquePtr<FRun> Run;
void Pass(const TCHAR* Name){++Run->Checks;UE_LOG(LogTemp,Display,TEXT("VOYAGER SAFETY PASS %s"),Name);}
void Fail(const TCHAR* Name){Run->Done=true;UE_LOG(LogTemp,Error,TEXT("VOYAGER SAFETY FAIL stage=%d %s"),Run->Stage,Name);FPlatformMisc::RequestExit(false);}
AActor* Block(UWorld* W,const FVector& At,const FRotator& Facing,const FVector& Half) {
    auto Actor=W->SpawnActor<AActor>();auto Root=NewObject<USceneComponent>(Actor);Actor->SetRootComponent(Root);Root->RegisterComponent();Actor->SetActorLocationAndRotation(At,Facing);
    auto Mesh=RiftVisual::Mesh(Actor,Root,TEXT("SafetyFixture"),TEXT("Cube"),FVector::ZeroVector,Half/50,FLinearColor(.2f,.2f,.2f));
    Mesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);Mesh->SetCollisionResponseToAllChannels(ECR_Block);return Actor;
}
void Tick(UWorld* W,ELevelTick,float) {
    if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerSafetyAudit"))||!W||!W->IsGameWorld()||W->GetNetMode()!=NM_Standalone)return;
    if(!Run){if(W->GetTimeSeconds()<9)return;Run=MakeUnique<FRun>();Run->World=W;Run->Started=Run->Time=FPlatformTime::Seconds();}
    if(Run->World.Get()!=W||Run->Done)return;
    const double Now=FPlatformTime::Seconds();if(Now-Run->Started>60){Fail(TEXT("TIMEOUT"));return;}
    auto PC=Cast<AVoyagerController>(W->GetFirstPlayerController());auto GM=W->GetAuthGameMode<AVoyagerGameMode>();auto State=W->GetGameState<AVoyagerState>();if(!PC||!GM||!State)return;
    if(Run->Stage==0) {
        auto Ship=GM->ShipFor(PC);if(!Ship)return;Run->Ship=Ship;
        if(auto Old=PC->GetPawn()){PC->Possess(Ship);if(Old!=Ship)Old->Destroy();}
        const FVector Pole=Voyager::SurfacePoint(State->SystemSeed,State->PlanetIndex,FVector::UpVector,400);
        bool Clear=false;FVector Landing=FVector::ZeroVector;FRotator Facing=FRotator::ZeroRotator;FString Reason;
        for(int32 I=0;I<12&&!Clear;++I){Ship->ResetFlight(Pole+FVector(-2500-I*750,3500,0),FRotator::ZeroRotator,false);Clear=VoyagerLanding::FindLanding(Ship,Landing,Facing,Reason);}
        if(!Clear){Fail(TEXT("NO_CLEAR_STARTING_SITE"));return;}
        Run->At=Ship->GetActorLocation();Run->Facing=Facing;Pass(TEXT("GENERATED_SEED_CLEAR_LANDING"));
        auto Wall=Block(W,Landing+FVector(0,0,150),Facing,FVector(100,100,100));
        Ship->ServerInteract();
        if(PC->GetPawn()!=Ship||Ship->bLanded||FVector::Dist(Ship->GetActorLocation(),Run->At)>1){Fail(TEXT("BLOCKED_HULL_TELEPORTED"));return;}
        Wall->Destroy();Pass(TEXT("BLOCKED_HULL_REFUSES_WITHOUT_MOVEMENT"));
        Ship->ResetFlight(Landing,Facing,true);
        const FVector Left=Landing-Facing.RotateVector(FVector(0,520,0));
        Wall=Block(W,Left, Facing,FVector(90,90,220));FVector Exit;
        if(!VoyagerLanding::FindExit(Ship,Landing,Facing,Exit)||FVector::Dist(Exit,Left)<200){Fail(TEXT("BLOCKED_SIDE_NOT_AVOIDED"));return;}
        Wall->Destroy();Pass(TEXT("BLOCKED_LEFT_EXIT_USES_CLEAR_ALTERNATIVE"));
        TArray<AActor*> Walls;
        for(int32 Side:{-1,1}) {
            Walls.Add(Block(W,Landing+Facing.RotateVector(FVector(Side*450,0,60)),Facing,FVector(40,650,300)));
            Walls.Add(Block(W,Landing+Facing.RotateVector(FVector(0,Side*450,60)),Facing,FVector(650,40,300)));
        }
        Ship->ServerInteract();
        if(PC->GetPawn()!=Ship||!Ship->bLanded){Fail(TEXT("ENCLOSED_EXIT_FORCED_SPAWN"));return;}
        for(auto A:Walls)A->Destroy();Pass(TEXT("ENCLOSED_SHIP_RETAINS_POSSESSION"));
        Ship->ResetFlight(Run->At,Facing,false);Ship->SetFlightTestInput(1,0,false);Run->Stage=1;Run->Time=Now;return;
    }
    auto Ship=Run->Ship.Get();if(!Ship){Fail(TEXT("SHIP_LOST"));return;}
    if(Run->Stage==1) {
        if(Now-Run->Time<.5)return;
        if(Ship->GetVelocity().Size()<=500){Fail(TEXT("SPEED_FIXTURE_NOT_MOVING"));return;}
        const FVector Before=Ship->GetActorLocation();Ship->ServerInteract();
        if(PC->GetPawn()!=Ship||Ship->bLanded||FVector::Dist(Before,Ship->GetActorLocation())>1){Fail(TEXT("FAST_LANDING_ACCEPTED"));return;}
        Pass(TEXT("REAL_FLIGHT_SPEED_REJECTS_LANDING"));Ship->ClearFlightTestInput();Ship->ResetFlight(Run->At,Run->Facing,false);
        Ship->ServerInteract();if(!Cast<AVoyagerCharacter>(PC->GetPawn())||!Ship->bLanded){Fail(TEXT("SAFE_LANDING_EXIT_FAILED"));return;}
        Pass(TEXT("SAFE_LANDING_AND_POSSESSION_TRANSFER"));Run->Stage=2;Run->Time=Now;return;
    }
    if(Run->Stage==2) {
        if(Now-Run->Time<3)return;auto Pawn=Cast<AVoyagerCharacter>(PC->GetPawn());
        if(!Pawn||!Pawn->GetCharacterMovement()->IsMovingOnGround()||Pawn->Health<=0){Fail(TEXT("EXIT_NOT_GROUNDED"));return;}
        Pass(TEXT("EXIT_GROUNDED_AND_ALIVE"));
        auto PS=PC->GetPlayerState<AVoyagerPlayerState>();PS->Minerals=147;GM->SaveExpedition(true);
        auto Save=Cast<UVoyagerSave>(UGameplayStatics::LoadGameFromSlot(TEXT("Voyager-Automation-Safety"),0));
        if(!Save||Save->Minerals!=147||Save->SystemSeed!=State->SystemSeed||Save->JailSeconds!=0){Fail(TEXT("PRODUCTION_SAVE_ROUND_TRIP"));return;}
        Pass(TEXT("PRODUCTION_SAVE_ROUND_TRIP"));
        GM->BoardShip(Pawn);if(PC->GetPawn()!=Ship){Fail(TEXT("REBOARD_FAILED"));return;}
        Pass(TEXT("REBOARD_AFTER_SAFE_EXIT"));Run->Done=true;
        UE_LOG(LogTemp,Display,TEXT("VOYAGER SAFETY COMPLETE seed=%d checks=%d"),State->SystemSeed,Run->Checks);FPlatformMisc::RequestExit(false);
    }
}
struct FRegistration {FDelegateHandle Handle;FRegistration(){Handle=FWorldDelegates::OnWorldPostActorTick.AddStatic(&Tick);}~FRegistration(){FWorldDelegates::OnWorldPostActorTick.Remove(Handle);}} Registration;
}
