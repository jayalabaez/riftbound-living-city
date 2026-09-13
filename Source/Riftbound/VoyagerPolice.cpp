#include "VoyagerPolice.h"
#include "VoyagerCharacter.h"
#include "VoyagerShip.h"
#include "VoyagerGameMode.h"
#include "VoyagerSettlement.h"
#include "VoyagerData.h"
#include "RiftVisual.h"
#include "Animation/AnimationAsset.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/TextRenderComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "DrawDebugHelpers.h"

AVoyagerPoliceUnit::AVoyagerPoliceUnit()
{
    PrimaryActorTick.bCanEverTick=true;bReplicates=true;bAlwaysRelevant=true;SetReplicateMovement(false);SetNetUpdateFrequency(15);
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("PoliceRoot")));
    Collision=CreateDefaultSubobject<USphereComponent>(TEXT("PoliceHitBody"));Collision->SetupAttachment(GetRootComponent());Collision->InitSphereRadius(85);Collision->SetRelativeLocation(FVector(0,0,90));
    Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);Collision->SetCollisionObjectType(ECC_Pawn);Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
    Collision->SetCollisionResponseToChannel(ECC_Visibility,ECR_Block);Collision->SetGenerateOverlapEvents(false);
    VisualRoot=CreateDefaultSubobject<USceneComponent>(TEXT("PolicePresentation"));VisualRoot->SetupAttachment(GetRootComponent());VisualRoot->SetAbsolute(true,true,false);
    Tags.Add(TEXT("VoyagerPolice"));
}
void AVoyagerPoliceUnit::BeginPlay()
{Super::BeginPlay();Pose.Location=GetActorLocation();Pose.Rotation=GetActorRotation();VisualRoot->SetWorldLocationAndRotation(Pose.Location,Pose.Rotation);BuildVisuals();}
void AVoyagerPoliceUnit::Assign(AController* Target,bool Vehicle,int32 Slot)
{
    SuspectController=Target;bVehicle=Vehicle;SlotIndex=Slot;Health=bDrone?80.f:Vehicle?250.f:110.f;
    if(auto State=GetWorld()->GetGameState<AVoyagerState>())
    {
        Planet=Voyager::NearestPlanet(State->SystemSeed,GetActorLocation());double Best=TNumericLimits<double>::Max();
        for(int32 S=0;S<3;++S){const double Distance=FVector::DistSquared(GetActorLocation(),Voyager::SurfacePoint(State->SystemSeed,Planet,AVoyagerSettlement::SiteDirection(State->SystemSeed,Planet,S)));if(Distance<Best){Best=Distance;Site=S;}}
    }
    if(auto Law=AVoyagerLaw::Find(GetWorld()))NextShot=Law->WarningTime()+Slot*.35f;
    BuildVisuals();ForceNetUpdate();
    if(auto PC=Cast<AVoyagerController>(Target))PC->Notify(bDrone?TEXT("SECURITY DRONE INBOUND / Find solid cover."):Vehicle?TEXT("SECURITY CRUISER INBOUND / Evade the pursuit."):TEXT("ARMED OFFICERS: Take cover and break line of sight."));
}
void AVoyagerPoliceUnit::AssignDrone(AController* Target,int32 Slot)
{bDrone=true;Assign(Target,false,Slot);}
void AVoyagerPoliceUnit::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(AVoyagerPoliceUnit,bVehicle);DOREPLIFETIME(AVoyagerPoliceUnit,bDrone);DOREPLIFETIME(AVoyagerPoliceUnit,Health);DOREPLIFETIME(AVoyagerPoliceUnit,bMoving);DOREPLIFETIME(AVoyagerPoliceUnit,Pose);}
void AVoyagerPoliceUnit::OnRep_Kind(){BuildVisuals();}
void AVoyagerPoliceUnit::OnRep_Pose(){if(!HasAuthority())SetActorLocationAndRotation(Pose.Location,Pose.Rotation,false);}
void AVoyagerPoliceUnit::BuildVisuals()
{
    Collision->SetSphereRadius(bDrone?70.f:bVehicle?220.f:85.f);Collision->SetRelativeLocation(FVector(0,0,bDrone?0:bVehicle?60:90));
    if(GetNetMode()==NM_DedicatedServer)return;
    TArray<USceneComponent*> Old;VisualRoot->GetChildrenComponents(true,Old);for(int32 I=Old.Num()-1;I>=0;--I)Old[I]->DestroyComponent();Body=nullptr;
    auto Part=[&](const TCHAR* Name,const TCHAR* Shape,FVector At,FVector Scale,FLinearColor Color,bool Glow=false){return RiftVisual::Mesh(this,VisualRoot,FName(Name),Shape,At,Scale,Color,Glow);};
    const FLinearColor Armor(.045f,.07f,.09f),Steel(.26f,.3f,.34f),Blue(.015f,.24f,1.f),Red(1.f,.015f,.01f);
    if(bVehicle||bDrone)
    {
        auto Mesh=LoadObject<UStaticMesh>(nullptr,bDrone?TEXT("/Game/Security/SM_SentinelDrone.SM_SentinelDrone"):TEXT("/Game/Security/SM_VesperCruiser.SM_VesperCruiser"));
        if(Mesh)
        {
            auto Model=NewObject<UStaticMeshComponent>(this);Model->SetupAttachment(VisualRoot);Model->SetStaticMesh(Mesh);
            Model->SetCollisionEnabled(ECollisionEnabled::NoCollision);Model->RegisterComponent();AddInstanceComponent(Model);
            Model->ComponentTags.Add(bDrone?TEXT("SentinelDroneModel"):TEXT("VesperCruiserModel"));
        }
        else UE_LOG(LogTemp,Error,TEXT("VOYAGER SECURITY MISSING MODEL drone=%d"),bDrone);
        if(!bDrone)
        {
            auto Label=NewObject<UTextRenderComponent>(this);Label->SetupAttachment(VisualRoot);Label->SetText(FText::FromString(TEXT("SECURITY")));
            Label->SetRelativeLocation(FVector(0,-107,57));Label->SetRelativeRotation(FRotator(0,-90,0));Label->SetWorldSize(18);Label->SetHorizontalAlignment(EHTA_Center);
            Label->SetTextRenderColor(FColor(220,230,239));Label->SetCollisionEnabled(ECollisionEnabled::NoCollision);Label->RegisterComponent();AddInstanceComponent(Label);
        }
    }
    else
    {
        auto Mesh=LoadObject<USkeletalMesh>(nullptr,TEXT("/Game/Characters/Import/Guard/CitizenGuard/SkeletalMeshes/CitizenGuard.CitizenGuard"));
        if(Mesh)
        {
            Body=NewObject<USkeletalMeshComponent>(this,TEXT("SecurityOfficer"));Body->SetupAttachment(VisualRoot);Body->SetSkeletalMeshAsset(Mesh);
            Body->SetRelativeRotation(FRotator(0,-90,0));Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);Body->SetAnimationMode(EAnimationMode::AnimationSingleNode);Body->RegisterComponent();AddInstanceComponent(Body);
            IdleClip=LoadObject<UAnimationAsset>(nullptr,TEXT("/Game/Characters/Import/Guard/CitizenGuard/SkeletalMeshes/CitizenGuardA_CitizenGuard_Idle.CitizenGuardA_CitizenGuard_Idle"));
            RunClip=LoadObject<UAnimationAsset>(nullptr,TEXT("/Game/Characters/Import/Guard/CitizenGuard/SkeletalMeshes/CitizenGuardA_CitizenGuard_Run.CitizenGuardA_CitizenGuard_Run"));
            AimClip=LoadObject<UAnimationAsset>(nullptr,TEXT("/Game/Characters/Import/Guard/CitizenGuard/SkeletalMeshes/CitizenGuardA_CitizenGuard_Aim.CitizenGuardA_CitizenGuard_Aim"));
            DeathClip=LoadObject<UAnimationAsset>(nullptr,TEXT("/Game/Characters/Import/Guard/CitizenGuard/SkeletalMeshes/CitizenGuardA_CitizenGuard_Death.CitizenGuardA_CitizenGuard_Death"));
            if(!AimClip)AimClip=IdleClip;
            if(IdleClip)Body->PlayAnimation(IdleClip,true);
        }
        Part(TEXT("CarbineReceiver"),TEXT("Cube"),FVector(29,18,126),FVector(.60f,.09f,.14f),Armor);
        Part(TEXT("CarbineBarrel"),TEXT("Cube"),FVector(65,18,129),FVector(.35f,.047f,.05f),Steel);
        Part(TEXT("CarbineMagazine"),TEXT("Cube"),FVector(32,18,112),FVector(.10f,.08f,.23f),Armor);
        Part(TEXT("BodyArmor"),TEXT("Cube"),FVector(2,0,119),FVector(.23f,.39f,.41f),Armor);
        Part(TEXT("Badge"),TEXT("Cube"),FVector(15,-9,132),FVector(.018f,.07f,.085f),FLinearColor(.65f,.69f,.34f));
    }
}
bool AVoyagerPoliceUnit::HasClearSight(const AActor* Observer,const APawn* Target,float Range)
{
    if(!Observer||!Target)return false;
    const FVector Eye=Observer->GetActorLocation()+Observer->GetActorUpVector()*(Cast<AVoyagerPoliceUnit>(Observer)&&Cast<AVoyagerPoliceUnit>(Observer)->IsDrone()?0.f:150.f);
    if(FVector::DistSquared(Eye,Target->GetPawnViewLocation())>FMath::Square(double(Range)))return false;
    FHitResult Hit;FCollisionQueryParams Query(SCENE_QUERY_STAT(GroundPoliceSensor),false,Observer);Query.AddIgnoredActor(Target);
    for(int32 Pane=0;Pane<16;++Pane)
    {
        if(!Observer->GetWorld()->LineTraceSingleByChannel(Hit,Eye,Target->GetPawnViewLocation(),ECC_Visibility,Query))return true;
        auto Component=Hit.GetComponent();if(!Component||!Component->ComponentHasTag(TEXT("VoyagerGlass")))return false;Query.AddIgnoredComponent(Component);
    }
    return false;
}
bool AVoyagerPoliceUnit::CanSee(APawn* Target) const { return HasClearSight(this,Target,bDrone?18000.f:bVehicle?35000.f:26000.f); }
void AVoyagerPoliceUnit::MoveToward(const FVector& Target,float D)
{
    auto State=GetWorld()->GetGameState<AVoyagerState>();if(!State)return;
    const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,Planet,GetActorLocation());
    FVector Aim=Target;
    if(Age>=NextRoute)
    {
        NextRoute=Age+2;StreetPath.Reset();PathCursor=0;
        const FVector SitePoint=Voyager::SurfacePoint(State->SystemSeed,Planet,AVoyagerSettlement::SiteDirection(State->SystemSeed,Planet,Site));
        if(FVector::DistSquared(SitePoint,Target)<FMath::Square(125000.))
        {
            auto Nearest=[&](FVector At){int32 Best=0;double Distance=TNumericLimits<double>::Max();for(int32 Y=0;Y<9;++Y)for(int32 X=0;X<9;++X){const double Dist=FVector::DistSquared(AVoyagerSettlement::StreetPoint(State->SystemSeed,Planet,Site,X,Y),At);if(Dist<Distance){Distance=Dist;Best=Y*9+X;}}return Best;};
            int32 Current=Nearest(GetActorLocation()),End=Nearest(Target);
            StreetPath.Add(AVoyagerSettlement::StreetPoint(State->SystemSeed,Planet,Site,Current%9,Current/9,bVehicle?110:10));
            while(Current%9!=End%9){Current+=Current%9<End%9?1:-1;StreetPath.Add(AVoyagerSettlement::StreetPoint(State->SystemSeed,Planet,Site,Current%9,Current/9,bVehicle?110:10));}
            while(Current/9!=End/9){Current+=Current/9<End/9?9:-9;StreetPath.Add(AVoyagerSettlement::StreetPoint(State->SystemSeed,Planet,Site,Current%9,Current/9,bVehicle?110:10));}
            const int32 Building=AVoyagerSettlement::FindBuildingAt(State->SystemSeed,Planet,Site,Target);FVoyagerBuildingInfo Info;
            if(Building>=0&&AVoyagerSettlement::GetBuildingInfo(State->SystemSeed,Planet,Site,Building,Info))
            {
                StreetPath.Add(Info.DoorOutside+Info.Up*(bVehicle?110:10));
                if(!bVehicle){StreetPath.Add(Info.DoorThreshold+Info.Up*10);StreetPath.Add(Info.DoorInside+Info.Up*10);StreetPath.Add(Target-Up*90);}
            }
            else StreetPath.Add(Target);
        }
    }
    if(StreetPath.IsValidIndex(PathCursor))
    {
        if(FVector::DistSquared(GetActorLocation(),StreetPath[PathCursor])<FMath::Square(bVehicle?350.:110.))++PathCursor;
        if(StreetPath.IsValidIndex(PathCursor))Aim=StreetPath[PathCursor];
    }
    const FVector Delta=FVector::VectorPlaneProject(Aim-GetActorLocation(),Up);
    const float Speed=bVehicle?1900.f:420.f;
    const FVector Step=Delta.GetSafeNormal()*FMath::Min(Delta.Size(),double(Speed*FMath::Min(D,.1f)));
    FVector Next=GetActorLocation()+Step;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(PoliceStreetMotion),false,this);
    for(TActorIterator<AVoyagerPoliceUnit> It(GetWorld());It;++It)Query.AddIgnoredActor(*It);
    if(auto C=SuspectController.Get())if(C->GetPawn())Query.AddIgnoredActor(C->GetPawn());
    const double Clearance=bVehicle?125.:90.;FHitResult Hit;
    if(GetWorld()->SweepSingleByChannel(Hit,GetActorLocation()+Up*Clearance,Next+Up*Clearance,GetActorQuat(),ECC_Visibility,FCollisionShape::MakeSphere(bVehicle?100.f:24.f),Query))Next=GetActorLocation();
    // Trace real floors before falling back to the radial terrain field; stairs and interiors remain grounded.
    FHitResult Ground;
    if(GetWorld()->LineTraceSingleByChannel(Ground,Next+Up*(bVehicle?200.:120.),Next-Up*1200,ECC_Visibility,Query)&&FVector::DotProduct(Ground.ImpactNormal,Up)>.5)
        Next=Ground.ImpactPoint+Up*(bVehicle?110.:8.);
    else Next=Voyager::SurfacePoint(State->SystemSeed,Planet,Voyager::SurfaceNormal(State->SystemSeed,Planet,Next),bVehicle?110:8);
    Velocity=(Next-GetActorLocation())/FMath::Max(D,.001f);bMoving=Velocity.SizeSquared()>100;
    SetActorLocation(Next);if(bMoving)SetActorRotation(FMath::RInterpTo(GetActorRotation(),Voyager::TangentRotation(Up,Step),D,6.f));
}
void AVoyagerPoliceUnit::MoveAerial(const FVector& Known,float D)
{
    auto State=GetWorld()->GetGameState<AVoyagerState>();if(!State)return;
    const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,Planet,GetActorLocation());
    const FRotator Frame=Voyager::TangentRotation(Up);const float Angle=Age*.24f+SlotIndex*2.1f;
    const FVector Ring=Frame.RotateVector(FVector(FMath::Cos(Angle),FMath::Sin(Angle),0));
    FVector Desired=Known+Ring*(1400+SlotIndex*250)+Up*(650+SlotIndex*230);
    FVector Delta=Desired-GetActorLocation();const float Dt=FMath::Min(D,.1f);
    Velocity=FMath::VInterpTo(Velocity,Delta.GetClampedToMaxSize(1900),Dt,2.4f);
    FVector Next=GetActorLocation()+Velocity*Dt;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(SecurityDroneAvoidance),false,this);
    for(TActorIterator<AVoyagerPoliceUnit> It(GetWorld());It;++It)Query.AddIgnoredActor(*It);
    if(auto C=SuspectController.Get())if(C->GetPawn())Query.AddIgnoredActor(C->GetPawn());
    FHitResult Hit;
    if(GetWorld()->SweepSingleByChannel(Hit,GetActorLocation(),Next,FQuat::Identity,ECC_Visibility,FCollisionShape::MakeSphere(85),Query))
    {
        // Slide along walls and climb around the obstruction; never teleport through it.
        const FVector Slide=FVector::VectorPlaneProject(Velocity,Hit.ImpactNormal)*Dt+Up*450*Dt;
        Next=GetActorLocation()+Slide;
        if(GetWorld()->SweepSingleByChannel(Hit,GetActorLocation(),Next,FQuat::Identity,ECC_Visibility,FCollisionShape::MakeSphere(85),Query))Next=GetActorLocation();
        Velocity=(Next-GetActorLocation())/FMath::Max(Dt,.001f);
    }
    const double Alt=Voyager::SurfaceAltitude(State->SystemSeed,Planet,Next);
    if(Alt<250)Next+=Up*FMath::Min(250-Alt,450.*Dt);
    SetActorLocation(Next);bMoving=Velocity.SizeSquared()>100;
    SetActorRotation(FMath::RInterpTo(GetActorRotation(),Voyager::TangentRotation(Up,Known-Next),Dt,5));
}
void AVoyagerPoliceUnit::Tick(float D)
{
    Super::Tick(D);Age+=D;
    if(GetNetMode()!=NM_DedicatedServer)
    {
        const FVector Hover= (bVehicle||bDrone)&&Health>0?Pose.Rotation.RotateVector(FVector(0,0,FMath::Sin(Age*(bDrone?2.3f:1.6f))*4.f)):FVector::ZeroVector;
        VisualRoot->SetWorldLocation(FMath::VInterpTo(VisualRoot->GetComponentLocation(),Pose.Location+Hover,D,16.f));
        VisualRoot->SetWorldRotation(FQuat::Slerp(VisualRoot->GetComponentQuat(),Pose.Rotation.Quaternion(),1-FMath::Exp(-D*16.f)));
        if(Body)if(auto Instance=Body->GetSingleNodeInstance())
        {
            UAnimationAsset* Clip=Health<=0?DeathClip.Get():bMoving?RunClip.Get():AimClip.Get();
            if(Clip&&Instance->GetAnimationAsset()!=Clip)Body->PlayAnimation(Clip,Health>0);
            // Imported run cycle travels 210 cm in 0.72 s; pursuit moves at 420 cm/s.
            Instance->SetPlayRate(Health>0&&bMoving?1.44f:1.f);
        }
    }
    if(!HasAuthority())return;
    if(Health<=0)
    {
        if(bDrone)
        {
            const float Dt=FMath::Min(D,.1f);const FVector Up=GetActorUpVector();Velocity-=Up*980*Dt;
            FHitResult Hit;FCollisionQueryParams Query(SCENE_QUERY_STAT(FallingDrone),false,this);
            const FVector Next=GetActorLocation()+Velocity*Dt;
            if(GetWorld()->SweepSingleByChannel(Hit,GetActorLocation(),Next,FQuat::Identity,ECC_Visibility,FCollisionShape::MakeSphere(60),Query))Velocity=FVector::ZeroVector;
            else SetActorLocation(Next);
            Pose.Location=GetActorLocation();
        }
        return;
    }
    auto C=SuspectController.Get();auto PS=C?C->GetPlayerState<AVoyagerPlayerState>():nullptr;auto Law=AVoyagerLaw::Find(GetWorld());
    auto State=GetWorld()->GetGameState<AVoyagerState>();
    if(!C||!PS||!Law||PS->WantedStars<=0||Law->IsJailed(C)||!State){Destroy();return;}
    if(State->bTransitioning)return;
    APawn* Target=C->GetPawn();if(!Target)return;
    const bool Visible=CanSee(Target);if(Visible){Law->Contact(C,Target->GetActorLocation());LostContact=0;}else LostContact+=D;
    const FVector Known=PS->LastKnownPosition;const double Distance=FVector::Dist(GetActorLocation(),Known);
    // Cruisers stop outside buildings; officers approach from cover and fan out.
    if(bDrone)MoveAerial(Known,D);
    else if(!Visible||Distance>(bVehicle?2200.:1400.+SlotIndex*350.))MoveToward(Known,D);
    else
    {
        bMoving=false;Velocity=FVector::ZeroVector;
        SetActorRotation(Voyager::TangentRotation(Voyager::SurfaceNormal(State->SystemSeed,Planet,GetActorLocation()),Known-GetActorLocation()));
    }
    Pose.Location=GetActorLocation();Pose.Rotation=GetActorRotation();
    if(bVehicle||!Visible||(!bDrone&&Cast<AVoyagerShip>(Target)))return;
    if(!Cast<AVoyagerCharacter>(Target)&&!Cast<AVoyagerShip>(Target))return;
    if(PS->WantedStars<2||Age<NextShot)return;
    NextShot=Age+FMath::Max(.85f,(bDrone?Law->DroneShotInterval():Law->GroundShotInterval())-PS->WantedStars*.12f);
    const FVector Up=GetActorUpVector(),Start=GetActorLocation()+Up*(bDrone?-22:132)+GetActorForwardVector()*(bDrone?85:70);
    const FVector End=Target->GetPawnViewLocation();FHitResult Hit;FCollisionQueryParams Query(SCENE_QUERY_STAT(OfficerFire),false,this);
    const bool HitSomething=GetWorld()->LineTraceSingleByChannel(Hit,Start,End,ECC_Visibility,Query);
    ++Fired;ShotEffect(Start,HitSomething?Hit.ImpactPoint:End,false);
    if(HitSomething&&Hit.GetActor()==Target)
        if(UGameplayStatics::ApplyPointDamage(Target,bDrone?Law->DroneDamage():Law->GroundDamage(),(End-Start).GetSafeNormal(),Hit,nullptr,this,nullptr)>0)++Hits;
}
void AVoyagerPoliceUnit::ShotEffect_Implementation(FVector From,FVector To,bool bWarning)
{if(GetNetMode()!=NM_DedicatedServer){DrawDebugLine(GetWorld(),From,To,bWarning?FColor(90,160,255):FColor(255,155,45),false,bWarning?.75f:.12f,0,bWarning?1.f:4.f);DrawDebugPoint(GetWorld(),From,9,FColor(255,220,140),false,.1f);}}
float AVoyagerPoliceUnit::TakeDamage(float Damage,const FDamageEvent& Event,AController* DamageInstigator,AActor* Causer)
{
    if(!HasAuthority()||Health<=0||!FMath::IsFinite(Damage)||Damage<=0)return 0;
    const float Applied=FMath::Min(Damage,Health);Health-=Applied;ForceNetUpdate();
    if(auto Law=AVoyagerLaw::Find(GetWorld()))Law->ReportCrime(DamageInstigator,GetActorLocation(),Health<=0?110:35,Health<=0?TEXT("security unit destroyed"):TEXT("attack on officer"),this);
    if(Health<=0){bMoving=false;SetActorEnableCollision(false);if(bVehicle)SetActorRotation(GetActorRotation()+FRotator(0,0,82));Pose.Rotation=GetActorRotation();SetLifeSpan(15.f);}
    return Applied;
}
