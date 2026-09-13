#include "VoyagerLaw.h"
#include "VoyagerGameMode.h"
#include "VoyagerCharacter.h"
#include "VoyagerShip.h"
#include "VoyagerCitizen.h"
#include "VoyagerCityLife.h"
#include "VoyagerPolice.h"
#include "VoyagerSettlement.h"
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
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

int32 AVoyagerLaw::StarsForHeat(float H) { return H>=330?5:H>=210?4:H>=120?3:H>=60?2:H>0?1:0; }
AVoyagerLaw::AVoyagerLaw() { PrimaryActorTick.bCanEverTick=true;PrimaryActorTick.TickInterval=.2f;bReplicates=true;bAlwaysRelevant=true; }
void AVoyagerLaw::BeginPlay()
{
    Super::BeginPlay();FString Text;TSharedPtr<FJsonObject> Data;
    if(FFileHelper::LoadFileToString(Text,*(FPaths::ProjectContentDir()/TEXT("CityData/police.json")))&&FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Data)&&Data.IsValid())
    {
        auto Read=[&](const TCHAR* Key,float& Value,float Low,float High){double N;if(Data->TryGetNumberField(Key,N)&&FMath::IsFinite(N))Value=FMath::Clamp(float(N),Low,High);};
        Read(TEXT("officer_damage"),OfficerDamage,1,25);Read(TEXT("officer_shot_interval_seconds"),OfficerShotInterval,.5f,5);
        Read(TEXT("initial_warning_seconds"),InitialWarning,2,12);Read(TEXT("drone_damage"),DroneShotDamage,1,20);Read(TEXT("drone_shot_interval_seconds"),DroneInterval,.7f,5);
    }
}
void AVoyagerLaw::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{Super::GetLifetimeReplicatedProps(OutLifetimeProps);}
AVoyagerLaw* AVoyagerLaw::Find(const UWorld* World)
{ for(TActorIterator<AVoyagerLaw> It(World);It;++It)return *It;return nullptr; }
void AVoyagerLaw::ReportCrime(AController* C,const FVector& Position,int32 Severity,const FString& Description,AActor* Witness)
{
    if(!HasAuthority()||!C||Severity<=0)return;
    auto PS=C->GetPlayerState<AVoyagerPlayerState>();auto State=GetWorld()->GetGameState<AVoyagerState>();
    if(!PS||!State||State->bTransitioning||IsJailed(C))return;
    APawn* Suspect=C->GetPawn();if(!Suspect)return;
    auto Observes=[&](AActor* Observer)
    {
        if(!Observer)return false;
        const FVector Eye=Observer->GetActorLocation()+Voyager::SurfaceNormal(State->SystemSeed,Voyager::NearestPlanet(State->SystemSeed,Position),Position)*155.f;
        if(FVector::DistSquared(Eye,Suspect->GetPawnViewLocation())>FMath::Square(125000.))return false;
        FHitResult Hit;FCollisionQueryParams Query(SCENE_QUERY_STAT(CityCrimeWitness),false,Observer);Query.AddIgnoredActor(Suspect);
        for(int32 Pane=0;Pane<16;++Pane)
        {
            if(!GetWorld()->LineTraceSingleByChannel(Hit,Eye,Suspect->GetPawnViewLocation(),ECC_Visibility,Query))return true;
            auto Component=Hit.GetComponent();if(!Component||!Component->ComponentHasTag(TEXT("VoyagerGlass")))return false;
            Query.AddIgnoredComponent(Component);
        }
        return false;
    };
    // An injured citizen or patrol can supply its own observed report. Otherwise a
    // live witness must see the suspect; opaque geometry blocks identification.
    bool bObserved=Observes(Witness);
    if(!bObserved)for(TActorIterator<AVoyagerCitizen> It(GetWorld());It;++It)
        if(It->IsAlive()&&FVector::DistSquared(It->GetActorLocation(),Position)<FMath::Square(25000.)&&Observes(*It)){bObserved=true;break;}
    if(!bObserved)for(TActorIterator<AVoyagerPatrolShip> It(GetWorld());It;++It)
        if(It->Hull>0&&It->CanSee(Suspect)){bObserved=true;break;}
    if(!bObserved)for(TActorIterator<AVoyagerPoliceUnit> It(GetWorld());It;++It)
        if(It->GetHealth()>0&&It->CanSee(Suspect)){bObserved=true;break;}
    if(!bObserved)return;
    if(auto Life=AVoyagerCityLife::Find(GetWorld()))Life->RecordCrime(C,Severity,900);
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
    for(TActorIterator<AVoyagerPoliceUnit> It(GetWorld());It;++It)if(It->TargetController()==C)It->Destroy();
    GroundDispatch.Remove(C);DroneDispatch.Remove(C);
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
    const float Now=GetWorld()->GetTimeSeconds();
    if(State->bTransitioning)return;
    for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        auto C=It->Get();auto PS=C?C->GetPlayerState<AVoyagerPlayerState>():nullptr;
        if(!PS||PS->WantedStars<=0||!C->GetPawn()||IsJailed(C))continue;
        DispatchGround(C,PS->WantedStars,Now);DispatchDrones(C,PS->WantedStars,Now);
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
    const bool GroundTarget=Cast<AVoyagerCharacter>(Target)!=nullptr&&!InSpace;
    const double Radius=GroundTarget?(Visible?8000.:18000.):Visible?15000.:30000.+FMath::Min(Age*150.,25000.);
    FVector Desired=Known+(Frame.Vector()*FMath::Cos(Angle)+FRotationMatrix(Frame).GetScaledAxis(EAxis::Y)*FMath::Sin(Angle))*Radius+Up*(InSpace?6000.:GroundTarget?6500.+FormationSlot*1800.:18000.);
    if(Visible&&Cast<AVoyagerShip>(Target))Desired+=Target->GetVelocity()*.35f;
    const FVector Delta=Desired-GetActorLocation();const double Speed=Visible?FMath::Clamp(Target->GetVelocity().Size()+18000.,18000.,240000.):22000.;
    FVector Next=GetActorLocation()+Delta.GetSafeNormal()*FMath::Min(Delta.Size(),Speed*FMath::Min(D,.1f));
    const int32 NextPlanet=Voyager::NearestPlanet(State->SystemSeed,Next);const double Alt=Voyager::SurfaceAltitude(State->SystemSeed,NextPlanet,Next);
    const double MinimumAltitude=GroundTarget?4500.:14000.;
    if(Alt<MinimumAltitude)Next+=Voyager::SurfaceNormal(State->SystemSeed,NextPlanet,Next)*(MinimumAltitude-Alt);
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
            if(HitSomething&&Hit.GetActor()==Target)
            {
                UGameplayStatics::ApplyPointDamage(Target,Cast<AVoyagerShip>(Target)?18.f:9.f,(End-Start).GetSafeNormal(),Hit,nullptr,this,nullptr);
            }
        }
    }
    NextShot-=D;
    if(Visible&&NextShot<=0&&PS->WantedStars>=2)
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
    if(auto Law=AVoyagerLaw::Find(GetWorld()))Law->ReportCrime(DamageInstigator,GetActorLocation(),Hull<=0?110:35,Hull<=0?TEXT("patrol destroyed"):TEXT("attack on patrol"),this);
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

// Legacy save/API compatibility only. Custody no longer exists in Voyager.
FVoyagerCustodyStatus AVoyagerLaw::GetCustody(AController*) const { return {}; }
FVoyagerCustodyStatus AVoyagerLaw::GetResidentCustody(const AVoyagerPlayerState*) const { return {}; }
bool AVoyagerLaw::IsJailed(AController*) const { return false; }
bool AVoyagerLaw::Surrender(AController*) { return false; }
bool AVoyagerLaw::TryArrest(AController*,bool) { return false; }
void AVoyagerLaw::CaptureCustody(AController*,UVoyagerSave* Save) const
{if(Save){Save->JailSeconds=0;Save->JailSystem=0;Save->JailPlanet=0;Save->JailSite=0;}}
void AVoyagerLaw::RestoreCustody(AController*,const UVoyagerSave*) {}

int32 AVoyagerLaw::DroneCount(AController* C) const
{int32 Count=0;for(TActorIterator<AVoyagerPoliceUnit> It(GetWorld());It;++It)if(It->TargetController()==C&&It->GetHealth()>0&&It->IsDrone())++Count;return Count;}
void AVoyagerLaw::DispatchDrones(AController* C,int32 Stars,float Now)
{
    auto State=GetWorld()->GetGameState<AVoyagerState>();auto Pawn=C?C->GetPawn():nullptr;
    if(!State||!Pawn||Stars<2)return;
    const int32 Planet=Voyager::NearestPlanet(State->SystemSeed,Pawn->GetActorLocation());
    if(Voyager::SurfaceAltitude(State->SystemSeed,Planet,Pawn->GetActorLocation())>50000)return;
    const int32 Count=DroneCount(C),Desired=FMath::Min(3,Stars-1);
    float& Next=DroneDispatch.FindOrAdd(C);if(Next==0){Next=Now+InitialWarning;return;}
    if(Count>=Desired||Now<Next)return;
    const FVector Known=C->GetPlayerState<AVoyagerPlayerState>()->LastKnownPosition;
    const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,Planet,Known);
    const FVector Side=Voyager::TangentRotation(Up).RotateVector(FVector(1,Count%2?1:-1,0)).GetSafeNormal();
    FVector Spawn=Known+Side*6000+Up*(1800+Count*400);
    FCollisionQueryParams Query(SCENE_QUERY_STAT(DroneSpawn),false,Pawn);FHitResult Hit;
    if(GetWorld()->LineTraceSingleByChannel(Hit,Known+Up*180,Spawn,ECC_Visibility,Query))Spawn=Hit.ImpactPoint-Side*250+Up*350;
    FActorSpawnParameters Params;Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    if(auto Drone=GetWorld()->SpawnActor<AVoyagerPoliceUnit>(Spawn,Voyager::TangentRotation(Up,-Side),Params))Drone->AssignDrone(C,Count);
    Next=Now+8.f;
}

int32 AVoyagerLaw::GroundUnitCount(AController* C,bool bVehicle) const
{int32 Count=0;for(TActorIterator<AVoyagerPoliceUnit> It(GetWorld());It;++It)if(It->TargetController()==C&&It->GetHealth()>0&&!It->IsDrone()&&It->IsVehicle()==bVehicle)++Count;return Count;}
void AVoyagerLaw::DispatchGround(AController* C,int32 Stars,float Now)
{
    auto Pawn=Cast<AVoyagerCharacter>(C->GetPawn());auto State=GetWorld()->GetGameState<AVoyagerState>();if(!Pawn||!State)return;
    const int32 Planet=Voyager::NearestPlanet(State->SystemSeed,Pawn->GetActorLocation());
    if(Voyager::SurfaceAltitude(State->SystemSeed,Planet,Pawn->GetActorLocation())>25000)return;
    float& DispatchAt=GroundDispatch.FindOrAdd(C);if(DispatchAt==0){DispatchAt=Now+InitialWarning;return;}if(Now<DispatchAt)return;
    const int32 WantedOfficers=FMath::Min(4,1+Stars/2),Officers=GroundUnitCount(C),Vehicles=GroundUnitCount(C,true);
    if(Officers>=WantedOfficers&&Vehicles>=1)return;
    int32 Site=0;double Best=TNumericLimits<double>::Max();
    for(int32 S=0;S<3;++S){const FVector At=Voyager::SurfacePoint(State->SystemSeed,Planet,AVoyagerSettlement::SiteDirection(State->SystemSeed,Planet,S));const double Distance=FVector::DistSquared(At,Pawn->GetActorLocation());if(Distance<Best){Best=Distance;Site=S;}}
    const bool Vehicle=Vehicles==0;const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,Planet,Pawn->GetActorLocation());
    FVector Spawn=Pawn->GetActorLocation()+Voyager::TangentRotation(Up).Vector()*(Vehicle?12500.:6500.);
    if(Best<FMath::Square(130000.))
    {
        double StreetDistance=TNumericLimits<double>::Max();
        for(int32 Y=0;Y<9;++Y)for(int32 X=0;X<9;++X)
        {
            const FVector Point=AVoyagerSettlement::StreetPoint(State->SystemSeed,Planet,Site,X,Y,Vehicle?110:10);
            const double Distance=FVector::DistSquared(Point,Pawn->GetActorLocation());
            if(Distance>FMath::Square(Vehicle?10000.:4500.)&&Distance<StreetDistance){StreetDistance=Distance;Spawn=Point;}
        }
    }
    else Spawn=Voyager::SurfacePoint(State->SystemSeed,Planet,Voyager::SurfaceNormal(State->SystemSeed,Planet,Spawn),Vehicle?110:10);
    FActorSpawnParameters Params;Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    if(auto Unit=GetWorld()->SpawnActor<AVoyagerPoliceUnit>(Spawn,Voyager::TangentRotation(Up),Params))Unit->Assign(C,Vehicle,Officers);
    DispatchAt=Now+(Vehicle?2.f:4.f);
}
bool AVoyagerLaw::IsGuardEngaged(const AVoyagerCitizen* Guard) const { return GetGuardTarget(Guard)!=nullptr; }
APawn* AVoyagerLaw::GetGuardTarget(const AVoyagerCitizen* Guard) const
{auto Target=GuardTargets.Find(const_cast<AVoyagerCitizen*>(Guard));return Target&&Target->IsValid()?Target->Get()->GetPawn():nullptr;}
void AVoyagerLaw::TickGuard(AVoyagerCitizen* Guard,float D)
{
    if(!HasAuthority()||!Guard)return;
    if(!Guard->IsAlive()||!Guard->IsSecurityGuard()){GuardTargets.Remove(Guard);GuardShots.Remove(Guard);return;}
    AController* Target=nullptr;double Closest=FMath::Square(20000.);
    for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        auto C=It->Get();auto PS=C?C->GetPlayerState<AVoyagerPlayerState>():nullptr;
        if(!PS||PS->WantedStars<=0||IsJailed(C)||!Cast<AVoyagerCharacter>(C->GetPawn()))continue;
        const double Distance=FVector::DistSquared(Guard->GetActorLocation(),C->GetPawn()->GetActorLocation());
        if(Distance<Closest&&AVoyagerPoliceUnit::HasClearSight(Guard,C->GetPawn(),20000)){Closest=Distance;Target=C;}
    }
    if(!Target){GuardTargets.Remove(Guard);GuardShots.Remove(Guard);return;}
    const float Now=GetWorld()->GetTimeSeconds();
    if(!GuardTargets.Contains(Guard)||GuardTargets[Guard].Get()!=Target)
    {
        GuardTargets.Add(Guard,Target);GuardShots.Add(Guard,Now+InitialWarning);
        if(auto PC=Cast<AVoyagerController>(Target))PC->Notify(TEXT("CIVIC SECURITY: Armed response inbound. Break line of sight and escape."));
    }
    Contact(Target,Target->GetPawn()->GetActorLocation());
    auto PS=Target->GetPlayerState<AVoyagerPlayerState>();auto Pawn=Cast<AVoyagerCharacter>(Target->GetPawn());
    if(PS->WantedStars<2||Now<GuardShots.FindOrAdd(Guard))return;
    GuardShots[Guard]=Now+OfficerShotInterval;
    const FVector Start=Guard->GetActorLocation()+Guard->GetActorUpVector()*140.f,End=Pawn->GetPawnViewLocation();
    FHitResult Hit;FCollisionQueryParams Query(SCENE_QUERY_STAT(CivicGuardFire),false,Guard);
    const bool HitSomething=GetWorld()->LineTraceSingleByChannel(Hit,Start,End,ECC_Visibility,Query);
    // Reuse a multicast weapon effect so remote players also see guards firing.
    for(TActorIterator<AVoyagerPoliceUnit> It(GetWorld());It;++It)if(It->TargetController()==Target){It->ShotEffect(Start,HitSomething?Hit.ImpactPoint:End,false);break;}
    if(GetNetMode()!=NM_DedicatedServer)DrawDebugLine(GetWorld(),Start,HitSomething?Hit.ImpactPoint:End,FColor(255,135,55),false,.13f,0,3.f);
    if(HitSomething&&Hit.GetActor()==Pawn)UGameplayStatics::ApplyPointDamage(Pawn,OfficerDamage,(End-Start).GetSafeNormal(),Hit,nullptr,Guard,nullptr);
}
