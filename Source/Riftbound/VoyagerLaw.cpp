#include "VoyagerLaw.h"
#include "VoyagerGameMode.h"
#include "VoyagerCharacter.h"
#include "VoyagerShip.h"
#include "VoyagerData.h"
#include "RiftVisual.h"
#include "RiftCharacter.h"
#include "Components/SphereComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "DrawDebugHelpers.h"

int32 AVoyagerLaw::StarsForHeat(float H) { return H>=330?5:H>=210?4:H>=120?3:H>=60?2:H>0?1:0; }
AVoyagerLaw::AVoyagerLaw() { PrimaryActorTick.bCanEverTick=true;PrimaryActorTick.TickInterval=.5f; }
AVoyagerLaw* AVoyagerLaw::Find(const UWorld* World)
{ for(TActorIterator<AVoyagerLaw> It(World);It;++It)return *It;return nullptr; }
void AVoyagerLaw::ReportCrime(AController* C,const FVector& Position,int32 Severity,const FString& Description)
{
    if(!HasAuthority()||!C||Severity<=0)return;
    auto PS=C->GetPlayerState<AVoyagerPlayerState>();auto State=GetWorld()->GetGameState<AVoyagerState>();
    if(!PS||!State||State->bTransitioning)return;
    const float Now=GetWorld()->GetTimeSeconds();
    PS->CrimeHeat=FMath::Min(400.f,PS->CrimeHeat+Severity);PS->WantedStars=StarsForHeat(PS->CrimeHeat);
    PS->LastKnownPosition=C->GetPawn()?C->GetPawn()->GetActorLocation():Position;
    PS->LastLawContact=Now;PS->WantedSearchSeconds=0;PS->bLawSearching=false;
    if(PS->NextLawDispatch<=0)PS->NextLawDispatch=Now+4.f;
    PS->ForceNetUpdate();
    if(auto PC=Cast<AVoyagerController>(C))PC->Notify(FString::Printf(TEXT("CRIME REPORTED: %s / %d wanted star%s. Patrol response inbound."),*Description,PS->WantedStars,PS->WantedStars==1?TEXT(""):TEXT("s")));
}
void AVoyagerLaw::Contact(AController* C,const FVector& Position)
{
    if(!HasAuthority()||!C)return;auto PS=C->GetPlayerState<AVoyagerPlayerState>();if(!PS||PS->WantedStars<=0)return;
    PS->LastKnownPosition=Position;PS->LastLawContact=GetWorld()->GetTimeSeconds();
}
int32 AVoyagerLaw::PatrolCount(AController* C) const
{ int32 Count=0;for(TActorIterator<AVoyagerPatrolShip> It(GetWorld());It;++It)if(It->Hull>0&&It->TargetController()==C)++Count;return Count; }
void AVoyagerLaw::Resolve(AController* C,bool bNotify)
{
    if(!HasAuthority()||!C)return;auto PS=C->GetPlayerState<AVoyagerPlayerState>();if(!PS)return;
    const bool WasWanted=PS->WantedStars>0;PS->CrimeHeat=0;PS->WantedStars=0;PS->WantedSearchSeconds=0;PS->NextLawDispatch=0;PS->bLawSearching=false;PS->ForceNetUpdate();
    for(TActorIterator<AVoyagerPatrolShip> It(GetWorld());It;++It)if(It->TargetController()==C)It->Destroy();
    if(WasWanted&&bNotify)if(auto PC=Cast<AVoyagerController>(C))PC->Notify(TEXT("PURSUIT ENDED. Local patrols have stood down."));
}
void AVoyagerLaw::Tick(float D)
{
    Super::Tick(D);if(!HasAuthority())return;auto State=GetWorld()->GetGameState<AVoyagerState>();if(!State)return;
    if(System!=State->SystemSeed)
    {
        for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)Resolve(It->Get(),System!=INDEX_NONE);
        System=State->SystemSeed;
    }
    if(State->bTransitioning)return;
    const float Now=GetWorld()->GetTimeSeconds();
    for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        auto C=It->Get();auto PS=C?C->GetPlayerState<AVoyagerPlayerState>():nullptr;
        if(!PS||PS->WantedStars<=0||!C->GetPawn())continue;
        PS->bLawSearching=Now-PS->LastLawContact>3.f;
        // A lost suspect is searched for at the last observed position. Higher
        // response levels maintain the search longer; fresh contact resets it.
        const float Duration=25.f+PS->WantedStars*10.f;
        PS->WantedSearchSeconds=PS->bLawSearching?FMath::Max(0.f,Duration-(Now-PS->LastLawContact)):0.f;
        if(Now-PS->LastLawContact>Duration){Resolve(C);continue;}
        PS->ForceNetUpdate();
        const int32 Desired=FMath::Min(4,PS->WantedStars);
        const int32 Count=PatrolCount(C);
        if(Count<Desired&&Now>=PS->NextLawDispatch)
        {
            const FVector Known=PS->LastKnownPosition;const int32 Planet=Voyager::NearestPlanet(System,Known);
            const FVector Up=Voyager::SurfaceNormal(System,Planet,Known);
            const FRotator Frame=Voyager::TangentRotation(Up);
            const FVector Side=Frame.Vector()*FMath::Cos(Now+Count*2.f)+FRotationMatrix(Frame).GetScaledAxis(EAxis::Y)*FMath::Sin(Now+Count*2.f);
            FVector Spawn=Known+Side*100000.f+Up*50000.f;
            const double Alt=Voyager::SurfaceAltitude(System,Planet,Spawn);
            if(Alt<42000.f)Spawn+=Voyager::SurfaceNormal(System,Planet,Spawn)*(42000.f-Alt);
            FActorSpawnParameters Params;Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            if(auto Patrol=GetWorld()->SpawnActor<AVoyagerPatrolShip>(Spawn,(Known-Spawn).Rotation(),Params))Patrol->Assign(C,Count);
            PS->NextLawDispatch=Now+12.f;
        }
    }
}
AVoyagerPatrolShip::AVoyagerPatrolShip()
{
    PrimaryActorTick.bCanEverTick=true;bReplicates=true;bAlwaysRelevant=true;SetReplicateMovement(false);SetNetUpdateFrequency(15.f);
    Collision=CreateDefaultSubobject<USphereComponent>(TEXT("PatrolCollision"));SetRootComponent(Collision);Collision->InitSphereRadius(340.f);
    Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);Collision->SetCollisionObjectType(ECC_Pawn);Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
    Collision->SetCollisionResponseToChannel(ECC_Visibility,ECR_Block);Collision->SetGenerateOverlapEvents(false);
    VisualRoot=CreateDefaultSubobject<USceneComponent>(TEXT("PatrolVisuals"));VisualRoot->SetupAttachment(Collision);
    VisualRoot->SetAbsolute(true,true,false);
}
void AVoyagerPatrolShip::Assign(AController* C,int32 Slot){Offender=C;Suspect=C?C->GetPlayerState<AVoyagerPlayerState>():nullptr;FormationSlot=Slot;NextShot=5.f+Slot;}
void AVoyagerPatrolShip::BeginPlay()
{
    Super::BeginPlay();Pose.Location=GetActorLocation();Pose.Rotation=GetActorRotation();
    VisualRoot->SetWorldLocationAndRotation(Pose.Location,Pose.Rotation);
    if(GetNetMode()==NM_DedicatedServer)return;
    ShipVisuals=VoyagerShipVisuals::Build(this,VisualRoot,VoyagerShipVisuals::ELivery::Patrol);
    SearchLight=NewObject<USpotLightComponent>(this);SearchLight->SetupAttachment(VisualRoot);
    SearchLight->SetRelativeLocation(FVector(150,0,-45));SearchLight->SetRelativeRotation(FRotator(-70,0,0));
    SearchLight->SetLightColor(FLinearColor(.65f,.79f,1.f));SearchLight->SetIntensity(120000.f);
    SearchLight->SetAttenuationRadius(90000.f);SearchLight->SetInnerConeAngle(10.f);SearchLight->SetOuterConeAngle(18.f);
    SearchLight->SetCastShadows(false);SearchLight->RegisterComponent();AddInstanceComponent(SearchLight);
}
void AVoyagerPatrolShip::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(AVoyagerPatrolShip,Suspect);DOREPLIFETIME(AVoyagerPatrolShip,Pose);DOREPLIFETIME(AVoyagerPatrolShip,Hull);DOREPLIFETIME(AVoyagerPatrolShip,bSearching);}
void AVoyagerPatrolShip::OnRep_Pose(){if(!HasAuthority())SetActorLocationAndRotation(Pose.Location,Pose.Rotation,false);}
bool AVoyagerPatrolShip::CanSee(APawn* Target) const
{
    if(!Target)return false;const double Range=Cast<AVoyagerShip>(Target)?180000.:85000.;
    if(FVector::DistSquared(GetActorLocation(),Target->GetActorLocation())>FMath::Square(Range))return false;
    FHitResult Hit;FCollisionQueryParams Q(SCENE_QUERY_STAT(PatrolSensor),false,this);Q.AddIgnoredActor(Target);
    for(int32 Pane=0;Pane<16;++Pane)
    {
        if(!GetWorld()->LineTraceSingleByChannel(Hit,GetActorLocation(),Target->GetPawnViewLocation(),ECC_Visibility,Q))return true;
        auto Component=Hit.GetComponent();
        if(!Component||!Component->ComponentHasTag(TEXT("VoyagerGlass")))return false;
        Q.AddIgnoredComponent(Component);
    }
    return false;
}
void AVoyagerPatrolShip::Tick(float D)
{
    Super::Tick(D);Age+=D;if(Hull<=0)return;VoyagerShipVisuals::Update(ShipVisuals,D,Age,.8f,false,true);
    if(GetNetMode()!=NM_DedicatedServer)
    {
        VisualRoot->SetWorldLocation(FMath::VInterpTo(VisualRoot->GetComponentLocation(),Pose.Location,D,18.f));
        VisualRoot->SetWorldRotation(FQuat::Slerp(VisualRoot->GetComponentQuat(),Pose.Rotation.Quaternion(),1.f-FMath::Exp(-D*18.f)));
        if(SearchLight)SearchLight->SetVisibility(bSearching);
    }
    if(!HasAuthority()||Hull<=0)return;
    AController* C=Offender.Get();auto PS=C?C->GetPlayerState<AVoyagerPlayerState>():nullptr;
    auto State=GetWorld()->GetGameState<AVoyagerState>();if(!PS||!State||PS->WantedStars<=0){Destroy();return;}if(State->bTransitioning)return;
    APawn* Target=C->GetPawn();const bool Visible=CanSee(Target);bSearching=!Visible;
    if(Visible)if(auto Law=AVoyagerLaw::Find(GetWorld()))Law->Contact(C,Target->GetActorLocation());
    const FVector Known=PS->LastKnownPosition;const int32 Planet=Voyager::NearestPlanet(State->SystemSeed,Known);
    const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,Planet,Known);const FRotator Frame=Voyager::TangentRotation(Up);
    const float Angle=Age*.14f+FormationSlot*1.7f;
    const bool InSpace=Voyager::SurfaceAltitude(State->SystemSeed,Planet,Known)>Voyager::AtmosphereHeight;
    const double Radius=Visible?15000.:30000.+FMath::Min(Age*150.,25000.);
    FVector Desired=Known+(Frame.Vector()*FMath::Cos(Angle)+FRotationMatrix(Frame).GetScaledAxis(EAxis::Y)*FMath::Sin(Angle))*Radius+Up*(InSpace?6000.:38000.);
    if(Visible&&Cast<AVoyagerShip>(Target))Desired+=Target->GetVelocity()*.35f;
    const FVector Delta=Desired-GetActorLocation();const double Speed=Visible?FMath::Clamp(Target->GetVelocity().Size()+18000.,18000.,240000.):22000.;
    FVector Next=GetActorLocation()+Delta.GetSafeNormal()*FMath::Min(Delta.Size(),Speed*FMath::Min(D,.1f));
    const int32 NextPlanet=Voyager::NearestPlanet(State->SystemSeed,Next);const double Alt=Voyager::SurfaceAltitude(State->SystemSeed,NextPlanet,Next);
    if(Alt<33000.)Next+=Voyager::SurfaceNormal(State->SystemSeed,NextPlanet,Next)*(33000.-Alt);
    FHitResult Obstacle;FCollisionQueryParams MovementQ(SCENE_QUERY_STAT(PatrolAvoidance),false,this);if(Target)MovementQ.AddIgnoredActor(Target);
    if(GetWorld()->SweepSingleByChannel(Obstacle,GetActorLocation(),Next,FQuat::Identity,ECC_Visibility,FCollisionShape::MakeSphere(400.f),MovementQ))Next=GetActorLocation()+Up*Speed*FMath::Min(D,.1f);
    SetActorLocation(Next);SetActorRotation(FMath::RInterpTo(GetActorRotation(),Voyager::TangentRotation(Up,(Known-Next).GetSafeNormal()),D,2.f));
    Pose.Location=Next;Pose.Rotation=GetActorRotation();
    if(WarningRemaining>0)
    {
        WarningRemaining-=D;
        if(WarningRemaining<=0&&Target)
        {
            const FVector Start=GetActorLocation();FHitResult Hit;FCollisionQueryParams Q(SCENE_QUERY_STAT(PatrolFire),false,this);
            const FVector End=Start+(ShotEnd-Start).GetSafeNormal()*((ShotEnd-Start).Size()+600.f);
            const bool HitSomething=GetWorld()->LineTraceSingleByChannel(Hit,Start,End,ECC_Visibility,Q);
            BeamEffect(Start,HitSomething?Hit.ImpactPoint:End,false);
            if(HitSomething&&Hit.GetActor()==Target)UGameplayStatics::ApplyPointDamage(Target,Cast<AVoyagerShip>(Target)?14.f:12.f,(End-Start).GetSafeNormal(),Hit,nullptr,this,nullptr);
        }
    }
    NextShot-=D;
    if(Visible&&NextShot<=0&&PS->WantedStars>=1)
    {
        NextShot=FMath::Max(2.f,5.f-PS->WantedStars*.5f);WarningRemaining=.9f;
        ShotEnd=Target->GetActorLocation()+Target->GetVelocity()*.2f;BeamEffect(GetActorLocation(),ShotEnd,true);
    }
}
void AVoyagerPatrolShip::BeamEffect_Implementation(FVector Start,FVector End,bool bWarning)
{if(GetNetMode()!=NM_DedicatedServer)DrawDebugLine(GetWorld(),Start,End,bWarning?FColor(80,130,255):FColor(255,65,30),false,bWarning?.85f:.2f,0,bWarning?2.f:7.f);}
float AVoyagerPatrolShip::TakeDamage(float Damage,const FDamageEvent& Event,AController* DamageInstigator,AActor* Causer)
{
    if(!HasAuthority()||Hull<=0||!FMath::IsFinite(Damage)||Damage<=0)return 0;
    const float Applied=FMath::Min(Hull,Damage);Hull-=Applied;ForceNetUpdate();
    if(auto Law=AVoyagerLaw::Find(GetWorld()))Law->ReportCrime(DamageInstigator,GetActorLocation(),Hull<=0?110:35,Hull<=0?TEXT("patrol destroyed"):TEXT("attack on patrol"));
    if(Hull<=0){SetActorEnableCollision(false);DestructionEffect();SetLifeSpan(.2f);}
    return Applied;
}
void AVoyagerPatrolShip::DestructionEffect_Implementation()
{
    VisualRoot->SetVisibility(false,true);
    if(GetNetMode()==NM_DedicatedServer)return;
    auto Burst=GetWorld()->SpawnActorDeferred<ARiftBurst>(ARiftBurst::StaticClass(),FTransform(FRotator::ZeroRotator,GetActorLocation()));
    if(Burst){Burst->Size=9.f;Burst->Color=FLinearColor(1.f,.24f,.025f);UGameplayStatics::FinishSpawningActor(Burst,Burst->GetActorTransform());}
}
