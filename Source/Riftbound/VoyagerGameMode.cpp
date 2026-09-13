#include "VoyagerGameMode.h"
#include "VoyagerLaw.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "VoyagerCharacter.h"
#include "VoyagerShip.h"
#include "VoyagerWorld.h"
#include "VoyagerSettlement.h"
#include "VoyagerHUD.h"
#include "VoyagerData.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "Misc/CommandLine.h"

namespace
{
    bool IsVoyagerTest(){return FParse::Param(FCommandLine::Get(),TEXT("VoyagerCrimeAudit"))||FParse::Param(FCommandLine::Get(),TEXT("VoyagerBuildingAudit"))|| FParse::Param(FCommandLine::Get(),TEXT("VoyagerRealismAudit"))||FParse::Param(FCommandLine::Get(),TEXT("VoyagerAITest"))||FParse::Param(FCommandLine::Get(),TEXT("LivingCityIsolationAudit"))||FParse::Param(FCommandLine::Get(),TEXT("VoyagerCityNetAudit"))||FParse::Param(FCommandLine::Get(),TEXT("VoyagerCityAudit"))||FParse::Param(FCommandLine::Get(),TEXT("VoyagerLifeAudit"))||FParse::Param(FCommandLine::Get(),TEXT("VoyagerTest"))||FParse::Param(FCommandLine::Get(),TEXT("VoyagerNetTest"))||FParse::Param(FCommandLine::Get(),TEXT("VoyagerSurfaceAudit"));}
    int32 ArrivalPlanet(int32 System,int32 SavedPlanet)
    {
        if(FParse::Param(FCommandLine::Get(),TEXT("VoyagerNatureVisit")))
            for(int32 Planet=0;Planet<5;++Planet)if(Voyager::Biome(System,Planet)==0)return Planet;
        return SavedPlanet;
    }
    FString SaveSlot(){return FParse::Param(FCommandLine::Get(),TEXT("VoyagerSurfaceAudit"))?TEXT("Voyager-Automation-Surface"):(FParse::Param(FCommandLine::Get(),TEXT("VoyagerNetTest"))?TEXT("Voyager-Automation-Network"):(IsVoyagerTest()?TEXT("Voyager-Automation"):TEXT("Voyager-Expedition")));}
    void Tell(AController* C,const FString& Message){if(auto PC=Cast<AVoyagerController>(C))PC->Notify(Message);}
    FVector NorthSite(int32 System,int32 Planet,double X,double Y,double Height)
    {return Voyager::SurfacePoint(System,Planet,FVector(X,Y,Voyager::PlanetRadius(System,Planet)).GetSafeNormal(),Height);}
}
AVoyagerState::AVoyagerState(){bReplicates=true;SetNetUpdateFrequency(10);}
void AVoyagerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AVoyagerState,SystemSeed);DOREPLIFETIME(AVoyagerState,PlanetIndex);DOREPLIFETIME(AVoyagerState,Mode);DOREPLIFETIME(AVoyagerState,Revision);
    DOREPLIFETIME(AVoyagerState,bTransitioning);DOREPLIFETIME(AVoyagerState,TransitionTime);DOREPLIFETIME(AVoyagerState,TransitionLabel);
}
void AVoyagerPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(AVoyagerPlayerState,Minerals);DOREPLIFETIME(AVoyagerPlayerState,Discoveries);DOREPLIFETIME(AVoyagerPlayerState,PirateKills);DOREPLIFETIME(AVoyagerPlayerState,Upgrades);DOREPLIFETIME(AVoyagerPlayerState,WantedStars);DOREPLIFETIME(AVoyagerPlayerState,bLawSearching);DOREPLIFETIME(AVoyagerPlayerState,WantedSearchSeconds);
}
void AVoyagerPlayerState::AddMinerals(int32 Amount)
{
    if(!HasAuthority()||Amount<=0)return;Minerals=FMath::Min(Minerals+Amount,100000000);ForceNetUpdate();
    if(auto PC=Cast<AVoyagerController>(GetOwner()))PC->Notify(FString::Printf(TEXT("+%d EXOTIC MINERALS  /  %d in cargo"),Amount,Minerals));
    if(auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->SaveExpedition();
}
bool AVoyagerPlayerState::DiscoverPlanet(int32 System,int32 Planet)
{
    if(!HasAuthority())return false;const int64 Key=int64(System)*5+Planet;
    if(Visited.Contains(Key))return false;Visited.Add(Key);Discoveries=Visited.Num();AddMinerals(30);ForceNetUpdate();return true;
}

AVoyagerGameMode::AVoyagerGameMode()
{
    PrimaryActorTick.bCanEverTick=true;DefaultPawnClass=AVoyagerCharacter::StaticClass();PlayerControllerClass=AVoyagerController::StaticClass();
    PlayerStateClass=AVoyagerPlayerState::StaticClass();GameStateClass=AVoyagerState::StaticClass();HUDClass=AVoyagerHUD::StaticClass();
}
void AVoyagerGameMode::InitGame(const FString& MapName,const FString& Options,FString& ErrorMessage)
{
    Super::InitGame(MapName,Options,ErrorMessage);
    if(!IsVoyagerTest()&&!FParse::Param(FCommandLine::Get(),TEXT("VoyagerFresh")))LoadedSave=Cast<UVoyagerSave>(UGameplayStatics::LoadGameFromSlot(SaveSlot(),0));
}
void AVoyagerGameMode::BeginPlay()
{
    Super::BeginPlay();auto State=GetGameState<AVoyagerState>();
    GetWorld()->GetWorldSettings()->bEnableWorldBoundsChecks=false;
    if(LoadedSave&&State){State->SystemSeed=FMath::Max(1,LoadedSave->SystemSeed);State->PlanetIndex=FMath::Clamp(LoadedSave->PlanetIndex,0,4);}
    if(State)State->PlanetIndex=ArrivalPlanet(State->SystemSeed,State->PlanetIndex);
    WorldBuilder=GetWorld()->SpawnActor<AVoyagerWorld>(FVector::ZeroVector,FRotator::ZeroRotator);
    GetWorld()->SpawnActor<AVoyagerLaw>();
    UE_LOG(LogTemp,Display,TEXT("VOYAGER READY system=%d planet=%d mode=%d"),State->SystemSeed,State->PlanetIndex,State->Mode);
}
void AVoyagerGameMode::PreLogin(const FString& Options,const FString& Address,const FUniqueNetIdRepl& UniqueId,FString& ErrorMessage)
{
    Super::PreLogin(Options,Address,UniqueId,ErrorMessage);if(ErrorMessage.IsEmpty()&&GetNumPlayers()>=4)ErrorMessage=TEXT("This expedition already has four explorers.");
}
AActor* AVoyagerGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
    const int32 System=LoadedSave?FMath::Max(1,LoadedSave->SystemSeed):1;
    const int32 Planet=ArrivalPlanet(System,LoadedSave?FMath::Clamp(LoadedSave->PlanetIndex,0,4):0);
    if(Starts.IsEmpty())for(int32 I=0;I<4;++I)
    {
        FActorSpawnParameters Params;Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Starts.Add(GetWorld()->SpawnActor<APlayerStart>(NorthSite(System,Planet,-420,-450+I*260,115),FRotator(0,30,0),Params));
    }
    return Starts[StartIndex++%Starts.Num()];
}
void AVoyagerGameMode::PostLogin(APlayerController* NewPlayer)
{
    Super::PostLogin(NewPlayer);
    if(auto PS=NewPlayer->GetPlayerState<AVoyagerPlayerState>())
    {
        if(LoadedSave&&!bLoadedHost){PS->Minerals=LoadedSave->Minerals;PS->PirateKills=LoadedSave->PirateKills;PS->Upgrades=LoadedSave->Upgrades;PS->Visited=LoadedSave->Visited;PS->Discoveries=PS->Visited.Num();bLoadedHost=true;}
    }
    FTimerHandle Handle;TWeakObjectPtr<APlayerController> PlayerRef=NewPlayer;
    GetWorldTimerManager().SetTimer(Handle,FTimerDelegate::CreateWeakLambda(this,[this,PlayerRef]()
    {
        if(!PlayerRef.IsValid())return;auto PC=PlayerRef.Get();auto Ship=ShipFor(PC);auto State=GetGameState<AVoyagerState>();
        if(State&&State->Mode==1)
        {
            if(APawn* Old=PC->GetPawn())if(Old!=Ship){PC->UnPossess();Old->Destroy();}
            PC->Possess(Ship);const FVector Position=NorthSite(State->SystemSeed,State->PlanetIndex,0,Ships.Num()*650,Voyager::AtmosphereHeight*1.5);
            Ship->ResetFlight(Position,FRotator::ZeroRotator,false);
        }
        else if(State&&PC->GetPawn())
        {
            if(FParse::Param(FCommandLine::Get(),TEXT("VoyagerCityVisit")))
            {
                FVoyagerBuildingInfo Arrival;
                if(AVoyagerSettlement::GetBuildingInfo(State->SystemSeed,State->PlanetIndex,0,8,Arrival))
                {
                    const FRotator Facing=Voyager::TangentRotation(Arrival.Up,Arrival.Right);
                    PC->GetPawn()->SetActorLocationAndRotation(Arrival.DoorOutside-Arrival.Right*750+Arrival.Up*115,Facing);
                    PC->SetControlRotation(Facing);PC->ClientSetRotation(Facing,true);
                    const FVector PlazaUp=AVoyagerSettlement::SiteDirection(State->SystemSeed,State->PlanetIndex);
                    Ship->ResetFlight(Voyager::SurfacePoint(State->SystemSeed,State->PlanetIndex,PlazaUp,160),Voyager::TangentRotation(PlazaUp),true);
                    Tell(PC,TEXT("Welcome to the city. Walk through a signed entrance. Look at a resident and press E to talk. Your ship is on the plaza."));
                    return;
                }
            }
            const FVector NearShip=Ship->GetActorLocation()+FVector(-820,-450,0);
            const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,State->PlanetIndex,NearShip);
            const FRotator Facing=Voyager::TangentRotation(Up,FRotator(0,30,0).Vector());
            PC->GetPawn()->SetActorLocationAndRotation(Voyager::SurfacePoint(State->SystemSeed,State->PlanetIndex,Up,115),Facing);
            PC->SetControlRotation(Facing);
        }
        Tell(PC,TEXT("Welcome, Voyager. F scans this planet. E boards your ship. Your expedition autosaves."));
    }),.6f,false);
}
AVoyagerShip* AVoyagerGameMode::ShipFor(AController* Pilot)
{
    if(!Pilot)return nullptr;if(auto Found=Ships.Find(Pilot))if(IsValid(*Found))return *Found;
    FActorSpawnParameters Params;Params.Owner=Pilot;Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto State=GetGameState<AVoyagerState>();const int32 System=State?State->SystemSeed:1;const int32 Planet=State?State->PlanetIndex:0;
    AVoyagerShip* Ship=GetWorld()->SpawnActor<AVoyagerShip>(NorthSite(System,Planet,400,Ships.Num()*750,160),FRotator(0,0,0),Params);
    if(Ship){Ships.Add(Pilot,Ship);Ship->SetParked(true);}return Ship;
}
void AVoyagerGameMode::Logout(AController* Exiting)
{
    SaveExpedition();if(auto Found=Ships.Find(Exiting))if(IsValid(*Found))(*Found)->Destroy();Ships.Remove(Exiting);Super::Logout(Exiting);
}
void AVoyagerGameMode::BoardShip(AVoyagerCharacter* Explorer)
{
    if(!IsValid(Explorer)||!Explorer->GetController())return;auto State=GetGameState<AVoyagerState>();if(!State||State->bTransitioning)return;
    auto Pilot=Explorer->GetController();auto Ship=ShipFor(Pilot);if(!Ship)return;
    if(FVector::Dist(Explorer->GetActorLocation(),Ship->GetActorLocation())>1100){Tell(Pilot,TEXT("Move closer to your ship to board. F highlights the landing area."));return;}
    Pilot->Possess(Ship);Explorer->Destroy();Tell(Pilot,TEXT("SPACE lift off  /  W thrust  /  mouse steer  /  SHIFT boost. Climb to orbit."));
    UE_LOG(LogTemp,Display,TEXT("VOYAGER BOARDED ship=%s"),*Ship->GetName());
}
void AVoyagerGameMode::LeaveShip(AVoyagerShip* Ship)
{
    if(!Ship||!Ship->GetController())return;auto State=GetGameState<AVoyagerState>();if(!State||State->bTransitioning)return;
    if(!Ship->bLanded)return;auto Pilot=Ship->GetController();FVector Position=Ship->GetActorLocation()-Ship->GetActorRightVector()*520;
    const int32 Planet=Voyager::NearestPlanet(State->SystemSeed,Position);
    const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,Planet,Position);
    Position=Voyager::SurfacePoint(State->SystemSeed,Planet,Up,110);
    FActorSpawnParameters Params;Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
    auto Explorer=GetWorld()->SpawnActor<AVoyagerCharacter>(Position,Voyager::TangentRotation(Up,Ship->GetActorForwardVector()),Params);
    if(Explorer){Pilot->Possess(Explorer);Tell(Pilot,TEXT("F scan the planet  /  left click mine crystals  /  E return to your ship."));UE_LOG(LogTemp,Display,TEXT("VOYAGER DISEMBARKED"));}
}
void AVoyagerGameMode::BeginTravel(int32 Mode,int32 System,int32 Planet,const FString& Label,float Duration,bool WarpOnly)
{
    auto State=GetGameState<AVoyagerState>();if(!State||State->bTransitioning)return;
    PendingMode=Mode;PendingSystem=System;PendingPlanet=FMath::Clamp(Planet,0,4);bPendingWarp=WarpOnly;
    State->bTransitioning=true;State->TransitionTime=Duration;State->TransitionLabel=Label;State->ForceNetUpdate();
    ClearPirates();UE_LOG(LogTemp,Display,TEXT("VOYAGER TRAVEL %s -> system=%d planet=%d mode=%d"),*Label,System,Planet,Mode);
}
void AVoyagerGameMode::EnterSpace(AController* Pilot)
{
    // Atmosphere crossings are continuous flight. This legacy entry point never
    // resets the ship, changes possession, rebuilds scenery, or begins travel.
    Tell(Pilot,TEXT("Fly upward to leave the atmosphere. No entry gate or loading screen."));
}
void AVoyagerGameMode::LandOnPlanet(AController* Pilot,int32 Planet)
{
    Tell(Pilot,TEXT("Fly into the atmosphere directly. CTRL descends; E deploys landing gear below 10 m."));
}
void AVoyagerGameMode::WarpToPlanet(AController* Pilot,int32 Planet)
{
    auto State=GetGameState<AVoyagerState>();auto Ship=Cast<AVoyagerShip>(Pilot?Pilot->GetPawn():nullptr);
    if(!State||State->bTransitioning||!Ship||Planet<0||Planet>=5)return;
    if(Ship->SurfaceAltitude()<Voyager::AtmosphereHeight){Tell(Pilot,TEXT("Climb above 60 km to engage interplanetary cruise. Hold SPACE + SHIFT to ascend."));return;}
    Ship->StartCruise(Planet);
}
void AVoyagerGameMode::NextSystem(AController* Pilot)
{
    auto State=GetGameState<AVoyagerState>();auto Ship=Cast<AVoyagerShip>(Pilot?Pilot->GetPawn():nullptr);
    if(!State||!Ship)return;
    if(Ship->SurfaceAltitude()<Voyager::AtmosphereHeight){Tell(Pilot,TEXT("Reach space above 60 km before jumping to another star."));return;}
    const int32 Next=State->SystemSeed==MAX_int32?1:State->SystemSeed+1;
    BeginTravel(1,Next,0,FString::Printf(TEXT("HYPERSPACE / %s"),*Voyager::SystemName(Next)),3.f);
}
void AVoyagerGameMode::FinishTravel()
{
    auto State=GetGameState<AVoyagerState>();if(!State)return;
    State->Mode=PendingMode;State->SystemSeed=PendingSystem;State->PlanetIndex=PendingPlanet;++State->Revision;
    if(WorldBuilder)WorldBuilder->RebuildNow();
    int32 I=0;
    for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        auto Pilot=It->Get();if(!Pilot)continue;auto Ship=ShipFor(Pilot);if(!Ship)continue;
        if(APawn* Old=Pilot->GetPawn())if(Old!=Ship){Pilot->UnPossess();Old->Destroy();}
        Pilot->Possess(Ship);Ship->TargetPlanet=PendingPlanet;
        FVector Position;FRotator Facing;
        if(PendingMode==1)
        {
            Position=NorthSite(PendingSystem,PendingPlanet,0,I*800,Voyager::AtmosphereHeight*1.5);
            Facing=FRotator::ZeroRotator;
        }
        else {Position=NorthSite(PendingSystem,PendingPlanet,400,I*750,160);Facing=FRotator::ZeroRotator;}
        Ship->ResetFlight(Position,Facing,PendingMode==0);Ship->Hull=100;Ship->Shield=100;
        Tell(Pilot,PendingMode==0?TEXT("Touchdown. Press E to step outside and F to record your discovery."):TEXT("System reached. CTRL + SHIFT descends / TAB target / J cruise / H next star."));++I;
    }
    State->bTransitioning=false;State->TransitionTime=0;State->ForceNetUpdate();
    if(PendingMode==1&&!bPendingWarp)SpawnPirates();PirateTimer=90;
    SaveExpedition();UE_LOG(LogTemp,Display,TEXT("VOYAGER ARRIVED system=%d planet=%d mode=%d"),State->SystemSeed,State->PlanetIndex,State->Mode);
}
void AVoyagerGameMode::Tick(float D)
{
    Super::Tick(D);auto State=GetGameState<AVoyagerState>();if(!State)return;
    if(State->bTransitioning){State->TransitionTime=FMath::Max(0.f,State->TransitionTime-D);if(State->TransitionTime<=0)FinishTravel();return;}
    for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        auto PC=It->Get();if(!PC||!PC->IsLocalController()||!PC->GetPawn())continue;
        const FVector Position=PC->GetPawn()->GetActorLocation();State->PlanetIndex=Voyager::NearestPlanet(State->SystemSeed,Position);
        State->Mode=Voyager::SurfaceAltitude(State->SystemSeed,State->PlanetIndex,Position)>=Voyager::AtmosphereHeight?1:0;break;
    }
    PirateTimer-=D;if(PirateTimer<=0){SpawnPirates();PirateTimer=20;}
}
void AVoyagerGameMode::ClearPirates(){for(TActorIterator<AVoyagerPirate> It(GetWorld());It;++It)It->Destroy();}
void AVoyagerGameMode::SpawnPirates()
{
    int32 Count=0;for(TActorIterator<AVoyagerPirate> It(GetWorld());It;++It)++Count;if(Count>=3)return;
    for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        auto Ship=Cast<AVoyagerShip>(It->Get()->GetPawn());if(!Ship||Ship->SurfaceAltitude()<Voyager::AtmosphereHeight||Ship->bCruising)continue;
        for(int32 I=0;I<3-Count;++I)
        {
            const FVector Offset=Ship->GetActorForwardVector()*(5800+I*2100)+Ship->GetActorRightVector()*(I-1)*1700+FVector(0,0,600);
            GetWorld()->SpawnActor<AVoyagerPirate>(Ship->GetActorLocation()+Offset,(-Offset).Rotation());
        }
        Tell(It->Get(),TEXT("HOSTILE SIGNALS  /  red corsairs inbound. Left click fires your ship's lasers."));break;
    }
}
void AVoyagerGameMode::RecoverShip(AVoyagerShip* Ship)
{
    if(!Ship)return;auto State=GetGameState<AVoyagerState>();if(!State)return;
    if(auto Law=AVoyagerLaw::Find(GetWorld()))Law->Resolve(Ship->GetController(),false);
    const int32 Planet=Voyager::NearestPlanet(State->SystemSeed,Ship->GetActorLocation());
    if(Ship->SurfaceAltitude()>=Voyager::AtmosphereHeight)
    {
        const FVector P=NorthSite(State->SystemSeed,Planet,0,0,Voyager::AtmosphereHeight*1.5);
        Ship->ResetFlight(P,FRotator::ZeroRotator,false);
    }
    else Ship->ResetFlight(NorthSite(State->SystemSeed,Planet,400,0,160),FRotator::ZeroRotator,true);
    Ship->Hull=100;Ship->Shield=100;ClearPirates();PirateTimer=100;Tell(Ship->GetController(),TEXT("Emergency recovery complete. Hull repaired; cargo preserved."));
}
void AVoyagerGameMode::UpgradeShip(AController* Pilot)
{
    if(!Pilot)return;auto PS=Pilot->GetPlayerState<AVoyagerPlayerState>();if(!PS)return;
    if(PS->Upgrades>=5){Tell(Pilot,TEXT("Your ship is fully upgraded."));return;}
    if(PS->Minerals<75){Tell(Pilot,TEXT("Upgrade requires 75 minerals. Scan planets and mine crystals to earn more."));return;}
    auto Ship=ShipFor(Pilot);if(!Ship)return;
    if(Pilot->GetPawn()!=Ship&&(!Pilot->GetPawn()||FVector::Dist(Pilot->GetPawn()->GetActorLocation(),Ship->GetActorLocation())>1300)){Tell(Pilot,TEXT("Return to your ship to install an upgrade."));return;}
    PS->Minerals-=75;++PS->Upgrades;Ship->Hull=100;Ship->Shield=100;PS->ForceNetUpdate();Tell(Pilot,TEXT("Laser array upgraded. Hull and shields restored."));SaveExpedition();
}
void AVoyagerGameMode::SaveExpedition()
{
    auto State=GetGameState<AVoyagerState>();if(!State)return;auto Save=Cast<UVoyagerSave>(UGameplayStatics::CreateSaveGameObject(UVoyagerSave::StaticClass()));if(!Save)return;
    Save->SystemSeed=State->SystemSeed;Save->PlanetIndex=State->PlanetIndex;
    for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        auto PC=It->Get();if(!PC||!PC->IsLocalController())continue;
        if(auto PS=PC->GetPlayerState<AVoyagerPlayerState>()){Save->Minerals=PS->Minerals;Save->PirateKills=PS->PirateKills;Save->Upgrades=PS->Upgrades;Save->Visited=PS->Visited;break;}
    }
    const bool Saved=UGameplayStatics::SaveGameToSlot(Save,SaveSlot(),0);
    UE_LOG(LogTemp,Display,TEXT("VOYAGER SAVE %s system=%d planet=%d minerals=%d discoveries=%d"),Saved?TEXT("PASS"):TEXT("FAIL"),Save->SystemSeed,Save->PlanetIndex,Save->Minerals,Save->Visited.Num());
}

void AVoyagerGameMode::RecoverExplorer(AVoyagerCharacter* Explorer)
{
    if(!IsValid(Explorer)||!Explorer->GetController())return;
    auto C=Explorer->GetController();auto State=GetGameState<AVoyagerState>();if(!State)return;
    if(auto Law=AVoyagerLaw::Find(GetWorld()))Law->Resolve(C,false);
    const int32 Planet=Voyager::NearestPlanet(State->SystemSeed,Explorer->GetActorLocation());
    const FVector Up=AVoyagerSettlement::SiteDirection(State->SystemSeed,Planet);
    Explorer->GetCharacterMovement()->StopMovementImmediately();
    const FRotator Frame=Voyager::TangentRotation(Up);
    const FVector Rescue=Voyager::SurfacePoint(State->SystemSeed,Planet,Up,130.f)+Frame.Vector()*900+FRotationMatrix(Frame).GetScaledAxis(EAxis::Y)*450;
    Explorer->SetActorLocationAndRotation(Rescue,Frame,false,nullptr,ETeleportType::TeleportPhysics);
    Explorer->Health=100.f;Explorer->ForceNetUpdate();
    Tell(C,TEXT("RESCUED / Emergency medics restored your suit. Local pursuit ended; cargo preserved."));
}
