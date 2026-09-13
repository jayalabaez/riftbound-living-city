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
    SuspectController=Target;bVehicle=Vehicle;SlotIndex=Slot;Health=Vehicle?250.f:110.f;
    if(auto State=GetWorld()->GetGameState<AVoyagerState>())
    {
        Planet=Voyager::NearestPlanet(State->SystemSeed,GetActorLocation());double Best=TNumericLimits<double>::Max();
        for(int32 S=0;S<3;++S){const double Distance=FVector::DistSquared(GetActorLocation(),Voyager::SurfacePoint(State->SystemSeed,Planet,AVoyagerSettlement::SiteDirection(State->SystemSeed,Planet,S)));if(Distance<Best){Best=Distance;Site=S;}}
    }
    if(auto Law=AVoyagerLaw::Find(GetWorld()))NextShot=Law->WarningTime()+Slot*.35f;
    BuildVisuals();ForceNetUpdate();
    if(auto PC=Cast<AVoyagerController>(Target))PC->Notify(Vehicle?TEXT("SECURITY CRUISER INBOUND / Press G on foot to surrender."):TEXT("ARMED OFFICERS: Stop and press G to surrender. Take cover to break sight."));
}
void AVoyagerPoliceUnit::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(AVoyagerPoliceUnit,bVehicle);DOREPLIFETIME(AVoyagerPoliceUnit,Health);DOREPLIFETIME(AVoyagerPoliceUnit,bMoving);DOREPLIFETIME(AVoyagerPoliceUnit,Pose);}
void AVoyagerPoliceUnit::OnRep_Kind(){BuildVisuals();}
void AVoyagerPoliceUnit::OnRep_Pose(){if(!HasAuthority())SetActorLocationAndRotation(Pose.Location,Pose.Rotation,false);}
void AVoyagerPoliceUnit::BuildVisuals()
{
    Collision->SetSphereRadius(bVehicle?240.f:85.f);Collision->SetRelativeLocation(FVector(0,0,bVehicle?60:90));
    if(GetNetMode()==NM_DedicatedServer)return;
    TArray<USceneComponent*> Old;VisualRoot->GetChildrenComponents(true,Old);for(int32 I=Old.Num()-1;I>=0;--I)Old[I]->DestroyComponent();Body=nullptr;
    auto Part=[&](const TCHAR* Name,const TCHAR* Shape,FVector At,FVector Scale,FLinearColor Color,bool Glow=false){return RiftVisual::Mesh(this,VisualRoot,FName(Name),Shape,At,Scale,Color,Glow);};
    const FLinearColor Armor(.045f,.07f,.09f),Steel(.26f,.3f,.34f),Blue(.015f,.24f,1.f),Red(1.f,.015f,.01f);
    if(bVehicle)
    {
        Part(TEXT("ArmoredChassis"),TEXT("Cube"),FVector(0,0,40),FVector(6.5f,3.05f,.9f),Armor);
        Part(TEXT("Cabin"),TEXT("Cube"),FVector(-45,0,127),FVector(3.9f,2.75f,1.1f),Steel);
        auto Windshield=Part(TEXT("ArmoredWindshield"),TEXT("Cube"),FVector(151,0,141),FVector(.1f,2.35f,.65f),FLinearColor(.025f,.075f,.10f));Windshield->SetRelativeRotation(FRotator(0,0,-12));
        Part(TEXT("Hood"),TEXT("Cube"),FVector(228,0,88),FVector(1.8f,2.8f,.35f),Steel);
        Part(TEXT("FrontRam"),TEXT("Cube"),FVector(325,0,47),FVector(.25f,3.3f,.65f),Steel);
        Part(TEXT("LightBar"),TEXT("Cube"),FVector(-30,0,188),FVector(1.f,2.3f,.14f),Armor);
        for(int32 S:{-1,1})
        {
            Part(*FString::Printf(TEXT("Emergency%d"),S),TEXT("Cube"),FVector(-30,S*75,200),FVector(.8f,.6f,.13f),S<0?Red:Blue,true);
            Part(*FString::Printf(TEXT("Door%d"),S),TEXT("Cube"),FVector(-45,S*141,100),FVector(2.05f,.07f,.63f),FLinearColor(.72f,.76f,.79f));
            Part(*FString::Printf(TEXT("Window%d"),S),TEXT("Cube"),FVector(-45,S*141,151),FVector(2.05f,.075f,.35f),FLinearColor(.018f,.052f,.065f));
            Part(*FString::Printf(TEXT("Headlight%d"),S),TEXT("Cube"),FVector(326,S*108,78),FVector(.055f,.5f,.18f),FLinearColor(.72f,.83f,1.f),true);
            for(int32 End:{-1,1})
            {
                Part(*FString::Printf(TEXT("HoverPod%d%d"),S,End),TEXT("Cylinder"),FVector(End*216,S*168,8),FVector(.98f,.98f,.58f),Armor);
                Part(*FString::Printf(TEXT("Thruster%d%d"),S,End),TEXT("Cylinder"),FVector(End*216,S*168,-26),FVector(.72f,.72f,.06f),Blue,true);
            }
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
    const FVector Eye=Observer->GetActorLocation()+Observer->GetActorUpVector()*150.f;
    if(FVector::DistSquared(Eye,Target->GetPawnViewLocation())>FMath::Square(double(Range)))return false;
    FHitResult Hit;FCollisionQueryParams Query(SCENE_QUERY_STAT(GroundPoliceSensor),false,Observer);Query.AddIgnoredActor(Target);
    for(int32 Pane=0;Pane<16;++Pane)
    {
        if(!Observer->GetWorld()->LineTraceSingleByChannel(Hit,Eye,Target->GetPawnViewLocation(),ECC_Visibility,Query))return true;
        auto Component=Hit.GetComponent();if(!Component||!Component->ComponentHasTag(TEXT("VoyagerGlass")))return false;Query.AddIgnoredComponent(Component);
    }
    return false;
}
bool AVoyagerPoliceUnit::CanSee(APawn* Target) const { return HasClearSight(this,Target,bVehicle?35000.f:26000.f); }
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
void AVoyagerPoliceUnit::Tick(float D)
{
    Super::Tick(D);Age+=D;
    if(GetNetMode()!=NM_DedicatedServer)
    {
        VisualRoot->SetWorldLocation(FMath::VInterpTo(VisualRoot->GetComponentLocation(),Pose.Location,D,16.f));
        VisualRoot->SetWorldRotation(FQuat::Slerp(VisualRoot->GetComponentQuat(),Pose.Rotation.Quaternion(),1-FMath::Exp(-D*16.f)));
        if(Body)if(auto Instance=Body->GetSingleNodeInstance())
        {
            UAnimationAsset* Clip=Health<=0?DeathClip.Get():bMoving?RunClip.Get():AimClip.Get();
            if(Clip&&Instance->GetAnimationAsset()!=Clip)Body->PlayAnimation(Clip,Health>0);
            // Imported run cycle travels 210 cm in 0.72 s; pursuit moves at 420 cm/s.
            Instance->SetPlayRate(Health>0&&bMoving?1.44f:1.f);
        }
    }
    if(!HasAuthority()||Health<=0)return;
    auto C=SuspectController.Get();auto PS=C?C->GetPlayerState<AVoyagerPlayerState>():nullptr;auto Law=AVoyagerLaw::Find(GetWorld());
    auto State=GetWorld()->GetGameState<AVoyagerState>();
    if(!C||!PS||!Law||PS->WantedStars<=0||Law->IsJailed(C)||!State){Destroy();return;}
    if(State->bTransitioning)return;
    APawn* Target=C->GetPawn();if(!Target)return;
    const bool Visible=CanSee(Target);if(Visible){Law->Contact(C,Target->GetActorLocation());LostContact=0;}else LostContact+=D;
    const FVector Known=PS->LastKnownPosition;const double Distance=FVector::Dist(GetActorLocation(),Known);
    // Cruisers stop outside buildings; officers approach from cover and fan out.
    if(!Visible||Distance>(bVehicle?2200.:1400.+SlotIndex*350.))MoveToward(Known,D);
    else
    {
        bMoving=false;Velocity=FVector::ZeroVector;
        SetActorRotation(Voyager::TangentRotation(Voyager::SurfaceNormal(State->SystemSeed,Planet,GetActorLocation()),Known-GetActorLocation()));
    }
    Pose.Location=GetActorLocation();Pose.Rotation=GetActorRotation();
    if(bVehicle||!Visible||Cast<AVoyagerShip>(Target))return;
    auto Explorer=Cast<AVoyagerCharacter>(Target);if(!Explorer)return;
    if(Explorer->Health<=18.f&&Law->TryArrest(C))return;
    if(PS->WantedStars<2||Age<NextShot)return;
    NextShot=Age+FMath::Max(.85f,Law->GroundShotInterval()-PS->WantedStars*.12f);
    const FVector Up=GetActorUpVector(),Start=GetActorLocation()+Up*132+GetActorForwardVector()*70;
    const FVector End=Target->GetPawnViewLocation();FHitResult Hit;FCollisionQueryParams Query(SCENE_QUERY_STAT(OfficerFire),false,this);
    const bool HitSomething=GetWorld()->LineTraceSingleByChannel(Hit,Start,End,ECC_Visibility,Query);
    ++Fired;ShotEffect(Start,HitSomething?Hit.ImpactPoint:End,false);
    if(HitSomething&&Hit.GetActor()==Target)UGameplayStatics::ApplyPointDamage(Target,Law->GroundDamage(),(End-Start).GetSafeNormal(),Hit,nullptr,this,nullptr);
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
AVoyagerPoliceCell::AVoyagerPoliceCell(){bReplicates=true;bAlwaysRelevant=true;SetReplicateMovement(true);SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("HoldingCell")));}
void AVoyagerPoliceCell::BeginPlay()
{
    Super::BeginPlay();if(GetNetMode()==NM_DedicatedServer)return;
    const FLinearColor Steel(.10f,.13f,.15f);
    for(int32 Side=0;Side<4;++Side)for(int32 Bar=-4;Bar<=4;++Bar)
    {
        const bool AlongX=Side<2;const float Sign=Side%2==0?1.f:-1.f;
        RiftVisual::Mesh(this,GetRootComponent(),FName(*FString::Printf(TEXT("CellBar_%d_%d"),Side,Bar)),TEXT("Cylinder"),AlongX?FVector(Sign*170,Bar*40,145):FVector(Bar*40,Sign*170,145),FVector(.045f,.045f,2.9f),Steel);
    }
    RiftVisual::Mesh(this,GetRootComponent(),TEXT("CellUpperFrame"),TEXT("Cube"),FVector(0,0,292),FVector(3.6f,3.6f,.06f),Steel);
    RiftVisual::Mesh(this,GetRootComponent(),TEXT("CellBench"),TEXT("Cube"),FVector(-120,0,45),FVector(.6f,2.7f,.10f),Steel);
}
