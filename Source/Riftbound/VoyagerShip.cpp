#include "VoyagerShip.h"
#include "VoyagerCharacter.h"
#include "VoyagerGameMode.h"
#include "VoyagerWorld.h"
#include "VoyagerSettlement.h"
#include "VoyagerCitizen.h"
#include "VoyagerData.h"
#include "RiftCharacter.h"
#include "RiftVisual.h"
#include "Camera/CameraComponent.h"
#include "Components/SphereComponent.h"
#include "Components/InputComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/DamageType.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Sound/SoundBase.h"
#include "Net/UnrealNetwork.h"
#include "Misc/CommandLine.h"

namespace
{
    void SetBeam(UStaticMeshComponent* Beam,const FVector& Start,const FVector& End,float Width)
    {
        if(!Beam)return;
        const FVector Delta=End-Start;
        Beam->SetWorldLocation((Start+End)*.5f);
        Beam->SetWorldRotation(FRotationMatrix::MakeFromZ(Delta.GetSafeNormal()).Rotator());
        Beam->SetWorldScale3D(FVector(Width,Width,FMath::Max(1.f,Delta.Size())/100.f));
        Beam->SetVisibility(true);
    }
    void SpawnBurst(AActor* Owner,const FVector& At,float Size,const FLinearColor& Color)
    {
        if(!Owner||Owner->GetNetMode()==NM_DedicatedServer)return;
        auto FX=Owner->GetWorld()->SpawnActorDeferred<ARiftBurst>(ARiftBurst::StaticClass(),FTransform(FRotator::ZeroRotator,At));
        if(FX){FX->Size=Size;FX->Color=Color;UGameplayStatics::FinishSpawningActor(FX,FX->GetActorTransform());}
    }

    // All broad-phase geometry stays in double precision. The shared terrain
    // field is then used to resolve the actual surface instead of a planet shell.
    FVector ConstrainFlightSegment(int32 System,const FVector& Start,const FVector& Desired,bool& bContact)
    {
        bContact=false;
        const FVector Delta=Desired-Start;
        const double LengthSquared=Delta.SizeSquared();
        double FirstContact=1.0;
        int32 ContactPlanet=INDEX_NONE;
        for(int32 Planet=0;Planet<Voyager::PlanetCount;++Planet)
        {
            const FVector Center=Voyager::PlanetCenter(System,Planet);
            const double Radius=Voyager::PlanetRadius(System,Planet);
            const FVector Relative=Start-Center;
            const double ClosestT=LengthSquared>1e-8?FMath::Clamp(-FVector::DotProduct(Relative,Delta)/LengthSquared,0.0,1.0):0.0;
            if((Relative+Delta*ClosestT).SizeSquared()>FMath::Square(Radius+240000.0))continue;
            double EndT=FirstContact;
            // This inner sphere is below the mathematical minimum of the terrain
            // field. Its analytic intersection catches even an entire-planet jump.
            const double InnerRadius=Radius-225000.0;
            const double B=FVector::DotProduct(Relative,Delta);
            const double Discriminant=B*B-LengthSquared*(Relative.SizeSquared()-InnerRadius*InnerRadius);
            if(LengthSquared>1e-8&&Discriminant>=0.0)
            {
                const double Entry=(-B-FMath::Sqrt(Discriminant))/LengthSquared;
                if(Entry>=0.0&&Entry<EndT)EndT=FMath::Min(1.0,Entry+.000001);
            }
            const double StartAltitude=Voyager::SurfaceAltitude(System,Planet,Start);
            if(StartAltitude<159.0)
            {
                bContact=true;
                return Voyager::SurfacePoint(System,Planet,Voyager::SurfaceNormal(System,Planet,Start),160.0);
            }
            double ClearT=0.0;
            constexpr int32 Samples=12;
            for(int32 Sample=1;Sample<=Samples;++Sample)
            {
                const double T=EndT*double(Sample)/Samples;
                if(Voyager::SurfaceAltitude(System,Planet,Start+Delta*T)>160.0){ClearT=T;continue;}
                double HitT=T;
                for(int32 Refine=0;Refine<16;++Refine)
                {
                    const double Mid=(ClearT+HitT)*.5;
                    if(Voyager::SurfaceAltitude(System,Planet,Start+Delta*Mid)>160.0)ClearT=Mid;else HitT=Mid;
                }
                if(HitT<=FirstContact){FirstContact=HitT;ContactPlanet=Planet;}
                break;
            }
        }
        if(ContactPlanet!=INDEX_NONE)
        {
            bContact=true;
            const FVector At=Start+Delta*FirstContact;
            return Voyager::SurfacePoint(System,ContactPlanet,Voyager::SurfaceNormal(System,ContactPlanet,At),160.0);
        }
        return Desired;
    }

    FVector ConstrainSettlementSegment(UWorld* World,const AActor* Ship,const FVector& Start,
        const FVector& Desired,bool& bContact,FVector& ContactNormal)
    {
        bContact=false;ContactNormal=FVector::ZeroVector;
        const FVector Delta=Desired-Start;
        const double Distance=Delta.Size();
        if(!World||Distance<.001)return Desired;
        FCollisionQueryParams Query(SCENE_QUERY_STAT(VoyagerCityFlight),false,Ship);
        bool bHasCity=false;
        // The terrain already has an analytic swept constraint. Excluding other
        // actors also prevents an unrelated crystal/terrain hit from hiding the
        // later city hit in Unreal's first-blocking-result sweep.
        for(TActorIterator<AActor> It(World);It;++It)
        {
            if(const auto City=Cast<AVoyagerSettlement>(*It))bHasCity|=City->ActiveCityCount()>0;
            else Query.AddIgnoredActor(*It);
        }
        if(!bHasCity)return Desired;
        FCollisionObjectQueryParams Objects;Objects.AddObjectTypesToQuery(ECC_WorldStatic);
        FHitResult Hit;
        if(!World->SweepSingleByObjectType(Hit,Start,Desired,FQuat::Identity,Objects,
            FCollisionShape::MakeSphere(110.f),Query)||!Cast<AVoyagerSettlement>(Hit.GetActor()))return Desired;
        const FVector Normal=Hit.Normal.GetSafeNormal();
        // A ship inside newly streamed geometry can still move outward; do not
        // teleport it to a recovery location or pin it by an initial overlap.
        if(Hit.bStartPenetrating&&FVector::DotProduct(Delta,Normal)>=-.001)return Desired;
        bContact=true;ContactNormal=Normal;
        if(Hit.bStartPenetrating)return Start;
        return Start+Delta*FMath::Clamp(double(Hit.Time)-2.0/Distance,0.0,1.0);
    }

    bool CruiseSegmentBlocked(int32 System,const FVector& Start,const FVector& End,int32& BlockingPlanet,FVector& Closest)
    {
        const FVector Delta=End-Start;
        const double LengthSquared=Delta.SizeSquared();
        if(LengthSquared<1.0)return false;
        double Earliest=2.0;
        BlockingPlanet=INDEX_NONE;
        for(int32 Planet=0;Planet<Voyager::PlanetCount;++Planet)
        {
            const FVector Center=Voyager::PlanetCenter(System,Planet);
            const double T=FMath::Clamp(FVector::DotProduct(Center-Start,Delta)/LengthSquared,0.0,1.0);
            const FVector Candidate=Start+Delta*T;
            const double SafeRadius=Voyager::PlanetRadius(System,Planet)+Voyager::AtmosphereHeight*1.17;
            if((Candidate-Center).SizeSquared()<FMath::Square(SafeRadius)&&T<Earliest)
            {Earliest=T;BlockingPlanet=Planet;Closest=Candidate;}
        }
        return BlockingPlanet!=INDEX_NONE;
    }

    bool BuildCruiseRoute(int32 System,const FVector& From,int32 Destination,TArray<FVector>& Route)
    {
        Route.Empty();
        const int32 Home=Voyager::NearestPlanet(System,From);
        const FVector Up=Voyager::SurfaceNormal(System,Home,From);
        FVector RouteStart=From;
        // Leave the nearest world radially before planning interplanetary legs.
        // This initial segment always increases clearance and needs no teleport.
        if(Voyager::SurfaceAltitude(System,Home,From)<Voyager::AtmosphereHeight*1.55)
        {
            RouteStart=Voyager::SurfacePoint(System,Home,Up,Voyager::AtmosphereHeight*1.65);
            Route.Add(RouteStart);
        }
        const int32 FirstPlannedLeg=Route.Num();
        Route.Add(Voyager::SurfacePoint(System,Destination,FVector::UpVector,Voyager::AtmosphereHeight*1.5));
        int32 Leg=FirstPlannedLeg;
        while(Leg<Route.Num())
        {
            const FVector A=Leg>0?Route[Leg-1]:From;
            const FVector B=Route[Leg];
            int32 BlockingPlanet=INDEX_NONE;FVector Closest;
            if(!CruiseSegmentBlocked(System,A,B,BlockingPlanet,Closest)){++Leg;continue;}
            if(Route.Num()>=48)return false;
            const FVector Center=Voyager::PlanetCenter(System,BlockingPlanet);
            const FVector Direction=(B-A).GetSafeNormal();
            FVector Outward=(Closest-Center).GetSafeNormal();
            if(Outward.IsNearlyZero())Outward=FVector::VectorPlaneProject(FVector::UpVector,Direction).GetSafeNormal();
            if(Outward.IsNearlyZero())Outward=FVector::VectorPlaneProject(FVector::RightVector,Direction).GetSafeNormal();
            const double SafeRadius=Voyager::PlanetRadius(System,BlockingPlanet)+Voyager::AtmosphereHeight*1.17;
            Route.Insert(Center+Outward*(SafeRadius*1.08+500000.0),Leg);
            // Recheck both new segments. Multiple insertions form a clear arc
            // around an obstructing planet instead of passing through its body.
        }
        return true;
    }
}

AVoyagerShip::AVoyagerShip()
{
    PrimaryActorTick.bCanEverTick=true;
    bReplicates=true;bAlwaysRelevant=true;
    // Replicate the explicit double-precision state to every peer. Default pawn
    // movement quantization is unsuitable for these planetary coordinates.
    SetReplicateMovement(false);SetNetUpdateFrequency(20);SetMinNetUpdateFrequency(15);
    Collision=CreateDefaultSubobject<USphereComponent>(TEXT("ShipCollision"));
    SetRootComponent(Collision);Collision->InitSphereRadius(140);
    Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Collision->SetCollisionObjectType(ECC_Pawn);
    Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
    Collision->SetCollisionResponseToChannel(ECC_Visibility,ECR_Block);
    Collision->SetCollisionResponseToChannel(ECC_Pawn,ECR_Block);
    Collision->SetGenerateOverlapEvents(false);
    VisualRoot=CreateDefaultSubobject<USceneComponent>(TEXT("FlightVisuals"));VisualRoot->SetupAttachment(Collision);
    SpringArm=CreateDefaultSubobject<USpringArmComponent>(TEXT("ChaseArm"));SpringArm->SetupAttachment(Collision);
    SpringArm->TargetArmLength=950;SpringArm->SocketOffset=FVector(0,0,190);
    SpringArm->bDoCollisionTest=false;SpringArm->bEnableCameraLag=false;
    SpringArm->bUsePawnControlRotation=false;
    Camera=CreateDefaultSubobject<UCameraComponent>(TEXT("FlightCamera"));Camera->SetupAttachment(SpringArm,USpringArmComponent::SocketName);
    Camera->FieldOfView=88;Camera->bUsePawnControlRotation=false;
    Tags.Add(TEXT("VoyagerShip"));
}

void AVoyagerShip::BeginPlay()
{
    Super::BeginPlay();BuildVisuals();LastInputPacketTime=GetWorld()->GetTimeSeconds();
    if(HasAuthority())PublishFlightState();
}

void AVoyagerShip::BuildVisuals()
{
    const FLinearColor Ivory(.72f,.81f,.8f),Blue(.035f,.21f,.37f),Dark(.017f,.035f,.059f),Cyan(.025f,.83f,1.f),Gold(.95f,.4f,.07f);
    auto Part=[&](const TCHAR* Name,const TCHAR* Shape,FVector At,FVector Scale,FLinearColor Color,bool Glow=false)
    {return RiftVisual::Mesh(this,VisualRoot,FName(Name),Shape,At,Scale,Color,Glow);};
    Part(TEXT("ArmoredSpine"),TEXT("Cube"),FVector(-20,0,0),FVector(3.55f,1.1f,.6f),Blue);
    Part(TEXT("LowerKeel"),TEXT("Cube"),FVector(-25,0,-34),FVector(2.8f,.65f,.22f),Dark);
    Part(TEXT("ForwardNose"),TEXT("Cone"),FVector(172,0,0),FVector(1.05f,.55f,1.48f),Ivory)->SetRelativeRotation(FRotator(-90,0,0));
    Part(TEXT("CockpitFrame"),TEXT("Sphere"),FVector(43,0,43),FVector(1.9f,1.02f,.69f),Ivory);
    Part(TEXT("CockpitGlass"),TEXT("Sphere"),FVector(58,0,54),FVector(1.59f,.88f,.53f),FLinearColor(.025f,.22f,.27f));
    Part(TEXT("CanopySpine"),TEXT("Cube"),FVector(42,0,80),FVector(1.42f,.065f,.08f),Blue);
    Part(TEXT("TailReactor"),TEXT("Cube"),FVector(-149,0,27),FVector(.85f,1.01f,.28f),Ivory);
    Part(TEXT("CenterEngine"),TEXT("Cylinder"),FVector(-205,0,0),FVector(.69f,.69f,.7f),Dark)->SetRelativeRotation(FRotator(90,0,0));
    Part(TEXT("NoseStripe"),TEXT("Cube"),FVector(134,0,29),FVector(1.65f,.18f,.045f),Gold);
    for(int32 Side=-1;Side<=1;Side+=2)
    {
        auto Wing=Part(*FString::Printf(TEXT("SweptWing%d"),Side),TEXT("Cube"),FVector(-64,Side*180,0),FVector(1.50f,3.35f,.16f),Ivory);
        Wing->SetRelativeRotation(FRotator(0,Side*24.f,Side*3.f));
        auto Inlay=Part(*FString::Printf(TEXT("WingBlue%d"),Side),TEXT("Cube"),FVector(-52,Side*166,11),FVector(.75f,2.69f,.07f),Blue);
        Inlay->SetRelativeRotation(FRotator(0,Side*24.f,Side*3.f));
        Part(*FString::Printf(TEXT("WingTip%d"),Side),TEXT("Cube"),FVector(-118,Side*326,10),FVector(1.04f,.22f,.33f),Blue);
        Part(*FString::Printf(TEXT("Navigation%d"),Side),TEXT("Sphere"),FVector(-70,Side*339,18),FVector(.12f),Side<0?Cyan:Gold,true);
        Part(*FString::Printf(TEXT("EngineHousing%d"),Side),TEXT("Cylinder"),FVector(-137,Side*120,-12),FVector(.63f,.63f,1.62f),Dark)->SetRelativeRotation(FRotator(90,0,0));
        Part(*FString::Printf(TEXT("EngineSleeve%d"),Side),TEXT("Cylinder"),FVector(-112,Side*120,-12),FVector(.68f,.68f,.84f),Blue)->SetRelativeRotation(FRotator(90,0,0));
        auto Glow=Part(*FString::Printf(TEXT("EngineGlow%d"),Side),TEXT("Sphere"),FVector(-228,Side*120,-12),FVector(.32f,.44f,.44f),Cyan,true);Engines.Add(Glow);
        auto Exhaust=Part(*FString::Printf(TEXT("Exhaust%d"),Side),TEXT("Cone"),FVector(-279,Side*120,-12),FVector(.39f,.39f,1.05f),Cyan,true);
        Exhaust->SetRelativeRotation(FRotator(90,0,0));Engines.Add(Exhaust);
        Part(*FString::Printf(TEXT("LaserCannon%d"),Side),TEXT("Cylinder"),FVector(66,Side*192,-9),FVector(.105f,.105f,1.34f),Dark)->SetRelativeRotation(FRotator(90,0,0));
        Part(*FString::Printf(TEXT("CannonGlow%d"),Side),TEXT("Sphere"),FVector(138,Side*192,-9),FVector(.105f),Cyan,true);
        auto Tail=Part(*FString::Printf(TEXT("TailFin%d"),Side),TEXT("Cube"),FVector(-159,Side*70,66),FVector(.75f,.09f,.95f),Blue);
        Tail->SetRelativeRotation(FRotator(-25,0,Side*22.f));
        auto Beam=Part(*FString::Printf(TEXT("ShipLaser%d"),Side),TEXT("Cylinder"),FVector::ZeroVector,FVector(.035f),Cyan,true);
        Beam->SetVisibility(false);LaserBeams.Add(Beam);
    }
    Impact=Part(TEXT("LaserImpact"),TEXT("Sphere"),FVector::ZeroVector,FVector(.35f),Cyan,true);Impact->SetVisibility(false);
}

void AVoyagerShip::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AVoyagerShip,Hull);DOREPLIFETIME(AVoyagerShip,Shield);DOREPLIFETIME(AVoyagerShip,TargetPlanet);
    DOREPLIFETIME_CONDITION(AVoyagerShip,Speed,COND_SkipOwner);
    DOREPLIFETIME_CONDITION(AVoyagerShip,bLanded,COND_SkipOwner);
    DOREPLIFETIME_CONDITION(AVoyagerShip,bCruising,COND_SkipOwner);
    DOREPLIFETIME(AVoyagerShip,FlightState);
}

void AVoyagerShip::SetupPlayerInputComponent(UInputComponent* Input)
{
    Super::SetupPlayerInputComponent(Input);
    Input->BindAxis(TEXT("MoveForward"),this,&AVoyagerShip::Forward);
    Input->BindAxis(TEXT("MoveRight"),this,&AVoyagerShip::Right);
    Input->BindAxis(TEXT("FlightUp"),this,&AVoyagerShip::Lift);
    Input->BindAxis(TEXT("Turn"),this,&AVoyagerShip::Turn);
    Input->BindAxis(TEXT("LookUp"),this,&AVoyagerShip::Look);
    Input->BindAction(TEXT("Boost"),IE_Pressed,this,&AVoyagerShip::Boost);
    Input->BindAction(TEXT("Boost"),IE_Released,this,&AVoyagerShip::StopBoost);
    Input->BindAction(TEXT("Fire"),IE_Pressed,this,&AVoyagerShip::StartFire);
    Input->BindAction(TEXT("Fire"),IE_Released,this,&AVoyagerShip::StopFire);
    Input->BindAction(TEXT("Interact"),IE_Pressed,this,&AVoyagerShip::Interact);
    Input->BindAction(TEXT("Target"),IE_Pressed,this,&AVoyagerShip::SelectTarget);
    Input->BindAction(TEXT("Warp"),IE_Pressed,this,&AVoyagerShip::Warp);
    Input->BindAction(TEXT("NextSystem"),IE_Pressed,this,&AVoyagerShip::NextSystem);
}

bool AVoyagerShip::CanAct() const
{
    if(Hull<=0||!GetController())return false;
    if(auto State=GetWorld()->GetGameState<AVoyagerState>())if(State->bTransitioning)return false;
    if(auto PC=Cast<AVoyagerController>(GetController()))if(PC->bMenuVisible)return false;
    return true;
}

double AVoyagerShip::SurfaceAltitude() const
{
    const auto State=GetWorld()->GetGameState<AVoyagerState>();
    return State?Voyager::SurfaceAltitude(State->SystemSeed,NearestPlanetIndex(),GetActorLocation()):0.0;
}

int32 AVoyagerShip::NearestPlanetIndex() const
{
    const auto State=GetWorld()->GetGameState<AVoyagerState>();
    return State?Voyager::NearestPlanet(State->SystemSeed,GetActorLocation()):0;
}

float AVoyagerShip::AtmosphericDensity() const
{
    return Voyager::AtmosphereDensity(SurfaceAltitude());
}

void AVoyagerShip::SetParked(bool bParked)
{
    FRotator Facing=GetActorRotation();
    if(bParked)if(const auto State=GetWorld()->GetGameState<AVoyagerState>())
        Facing=Voyager::TangentRotation(Voyager::SurfaceNormal(State->SystemSeed,NearestPlanetIndex(),GetActorLocation()),GetActorForwardVector());
    ResetFlight(GetActorLocation(),Facing,bParked);
}

void AVoyagerShip::ResetFlight(FVector Location,FRotator Rotation,bool bParked)
{
    SetActorLocationAndRotation(Location,Rotation,false,nullptr,ETeleportType::TeleportPhysics);
    bLanded=bParked;Speed=0;FlightVelocity=FVector::ZeroVector;bFiring=false;MouseYaw=0;MousePitch=0;bRecovering=false;
    bCruising=false;CruiseWaypoints.Empty();CruiseLeg=0;
    PendingMoves.Empty();LastProcessedSequence=0;LocalSequence=0;SimulationCredit=.2f;
    SimulationAccumulator=0;PendingYaw=0;PendingPitch=0;
    if(HasAuthority()){++FlightEpoch;PublishFlightState();ForceNetUpdate();}
}

void AVoyagerShip::PublishFlightState()
{
    FlightState.Location=GetActorLocation();FlightState.Rotation=GetActorRotation();
    FlightState.Speed=Speed;FlightState.bLanded=bLanded;FlightState.Acknowledged=LastProcessedSequence;FlightState.Epoch=FlightEpoch;
    FlightState.Velocity=FlightVelocity;FlightState.bCruising=bCruising;FlightState.CruiseWaypoints=CruiseWaypoints;FlightState.CruiseLeg=CruiseLeg;
}

void AVoyagerShip::OnRep_FlightState()
{
    if(HasAuthority())return;
    const bool bNewEpoch=FlightEpoch!=FlightState.Epoch;
    if(bNewEpoch){FlightEpoch=FlightState.Epoch;PendingMoves.Empty();LocalSequence=0;MouseYaw=0;MousePitch=0;SimulationAccumulator=0;PendingYaw=0;PendingPitch=0;}
    PendingMoves.RemoveAll([this](const FVoyagerFlightMove& Move){return Move.Epoch!=FlightEpoch||Move.Sequence<=FlightState.Acknowledged;});
    SetActorLocationAndRotation(FlightState.Location,FlightState.Rotation,false,nullptr,ETeleportType::TeleportPhysics);
    Speed=FlightState.Speed;bLanded=FlightState.bLanded;FlightVelocity=FlightState.Velocity;
    bCruising=FlightState.bCruising;CruiseWaypoints=FlightState.CruiseWaypoints;CruiseLeg=FlightState.CruiseLeg;
    if(IsLocallyControlled())for(const FVoyagerFlightMove& Move:PendingMoves)SimulateMove(Move,false);
}

void AVoyagerShip::SimulateMove(const FVoyagerFlightMove& Move,bool bPermitTravel)
{
    const auto State=GetWorld()->GetGameState<AVoyagerState>();
    if(!State||State->bTransitioning||Move.bPaused||Hull<=0){FlightVelocity=FVector::ZeroVector;return;}
    const float D=FMath::Clamp(Move.Seconds,.000001f,.06f);
    SimulatedFlightSeconds+=double(D);
    const bool bManual=FMath::Abs(Move.Throttle)>.1f||FMath::Abs(Move.Strafe)>.1f||FMath::Abs(Move.Lift)>.1f||FMath::Abs(Move.Yaw)>.015f||FMath::Abs(Move.Pitch)>.015f;
    // Old lift/throttle packets can arrive just after the reliable cruise request.
    // Only inputs sampled after observing cruise may cancel it.
    if(bCruising&&bManual&&Move.bCruiseObserved){bCruising=false;CruiseWaypoints.Empty();CruiseLeg=0;}
    if(bCruising){SimulateCruise(D,bPermitTravel);return;}
    const FVector Start=GetActorLocation();
    const int32 Planet=Voyager::NearestPlanet(State->SystemSeed,Start);
    const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,Planet,Start);
    const double Altitude=FMath::Max(0.0,Voyager::SurfaceAltitude(State->SystemSeed,Planet,Start));
    if(bLanded)
    {
        Speed=0;FlightVelocity=FVector::ZeroVector;
        if(Move.Lift<=.1f)return;
        bLanded=false;
    }
    // Gravity is radial, including on the far side of a world. Yaw follows its
    // local horizon; pitch rotates around the ship's right vector with no Euler
    // pole clamp, allowing a continuous turn through vertical.
    FQuat Orientation=GetActorQuat();
    const double Yaw=FMath::DegreesToRadians(double(FMath::Clamp(Move.Yaw,-150.f*D,150.f*D)));
    const double Pitch=FMath::DegreesToRadians(double(FMath::Clamp(Move.Pitch,-100.f*D,100.f*D)));
    Orientation=FQuat(Up,Yaw)*Orientation;
    Orientation=FQuat(Orientation.GetRightVector(),-Pitch)*Orientation;
    const FVector Forward=Orientation.GetForwardVector();
    const double HorizonStrength=FMath::Exp(-Altitude/(Voyager::AtmosphereHeight*2.5));
    if(FMath::Abs(FVector::DotProduct(Forward,Up))<.995)
    {
        const FQuat Level=FRotationMatrix::MakeFromXZ(Forward,Up).ToQuat();
        Orientation=FQuat::Slerp(Orientation,Level,1.0-FMath::Exp(-D*2.5*HorizonStrength)).GetNormalized();
    }
    SetActorRotation(Orientation);
    const float Blend=float(1.0-FMath::Exp(-Altitude/500000.0));
    const float Maximum=Move.bBoost?FMath::Lerp(20000.f,2000000.f,Blend):FMath::Lerp(6000.f,800000.f,Blend);
    const float Acceleration=Maximum*.65f+10000.f;
    if(Move.Throttle>.02f)Speed+=Move.Throttle*Acceleration*D;
    else if(Move.Throttle<-.02f)Speed+=Move.Throttle*Acceleration*1.4f*D;
    else Speed=FMath::Max(0.f,Speed-(35.f+Voyager::AtmosphereDensity(Altitude)*350.f)*D);
    Speed=FMath::Clamp(Speed,0.f,Maximum);
    const float LiftMaximum=Move.bBoost?FMath::Lerp(4000.f,800000.f,Blend):FMath::Lerp(1800.f,120000.f,Blend);
    const float StrafeMaximum=FMath::Lerp(2500.f,90000.f,Blend);
    FVector Velocity=Orientation.GetForwardVector()*Speed+Orientation.GetRightVector()*Move.Strafe*StrafeMaximum+Up*Move.Lift*LiftMaximum;
    // Automatic reentry braking is an altitude envelope, not a collision trigger
    // or scene switch. It limits the radial descent to50m/s near the surface.
    const double MaximumDescent=FMath::Sqrt(5000.0*5000.0+2.0*80000.0*FMath::Max(0.0,Altitude-160.0));
    const double RadialSpeed=FVector::DotProduct(Velocity,Up);
    if(RadialSpeed<-MaximumDescent)Velocity+=Up*(-MaximumDescent-RadialSpeed);
    bool bContact=false;
    FVector Position=ConstrainFlightSegment(State->SystemSeed,Start,Start+Velocity*D,bContact);
    bool bCityContact=false;FVector CityNormal;
    if(Altitude<25000.0||Voyager::SurfaceAltitude(State->SystemSeed,Planet,Position)<25000.0)
        Position=ConstrainSettlementSegment(GetWorld(),this,Start,Position,bCityContact,CityNormal);
    FlightVelocity=(Position-Start)/D;
    SetActorLocation(Position,false);
    if(bCityContact)
    {
        Speed=0;
        const double IntoWall=FVector::DotProduct(FlightVelocity,CityNormal);
        if(IntoWall<0)FlightVelocity-=CityNormal*IntoWall;
    }
    if(bContact)
    {
        Speed=FMath::Min(Speed,5000.f);
        const FVector GroundUp=Voyager::SurfaceNormal(State->SystemSeed,Planet,Position);
        if(FVector::DotProduct(GetActorForwardVector(),GroundUp)<-.05)
            SetActorRotation(Voyager::TangentRotation(GroundUp,GetActorForwardVector()));
    }
}

void AVoyagerShip::StartCruise(int32 Planet)
{
    if(!HasAuthority()||!CanAct()||Planet<0||Planet>=Voyager::PlanetCount)return;
    if(bCruising){CancelCruise();return;}
    auto PC=Cast<AVoyagerController>(GetController());
    if(bLanded||SurfaceAltitude()<Voyager::AtmosphereHeight)
    {
        if(PC)PC->Notify(TEXT("Climb above 60 km to engage interplanetary cruise. SPACE + SHIFT ascends."));return;
    }
    const auto State=GetWorld()->GetGameState<AVoyagerState>();if(!State)return;
    if(!BuildCruiseRoute(State->SystemSeed,GetActorLocation(),Planet,CruiseWaypoints))
    {if(PC)PC->Notify(TEXT("Cruise route obstructed. Fly farther from this world and try again."));return;}
    TargetPlanet=Planet;bCruising=true;CruiseLeg=0;PublishFlightState();ForceNetUpdate();
    if(PC)PC->Notify(TEXT("Cruise engaged. Flying to a 90 km orbit. Steer or press J to cancel."));
    UE_LOG(LogTemp,Display,TEXT("VOYAGER CRUISE START planet=%d waypoints=%d epoch=%d"),Planet,CruiseWaypoints.Num(),FlightEpoch);
}

void AVoyagerShip::CancelCruise()
{
    bCruising=false;CruiseWaypoints.Empty();CruiseLeg=0;
    // The atmospheric speed envelope handles the remaining momentum smoothly on
    // the next manual step; no coordinate or scene reset occurs here.
    if(HasAuthority()){PublishFlightState();ForceNetUpdate();}
}

void AVoyagerShip::SimulateCruise(float D,bool bAuthorityEffects)
{
    const auto State=GetWorld()->GetGameState<AVoyagerState>();
    if(!State||!CruiseWaypoints.IsValidIndex(CruiseLeg)){CancelCruise();return;}
    const FVector Start=GetActorLocation();
    const FVector Delta=CruiseWaypoints[CruiseLeg]-Start;
    const double Distance=Delta.Size();
    const FVector Direction=Delta.GetSafeNormal();
    constexpr double Acceleration=2500000.0; //25km/s per second, up to250km/s.
    const double ArrivalSpeed=CruiseLeg==CruiseWaypoints.Num()-1?0.0:3000000.0;
    const double DesiredSpeed=FMath::Min(25000000.0,FMath::Sqrt(ArrivalSpeed*ArrivalSpeed+2.0*Acceleration*Distance));
    Speed=float(FMath::FInterpConstantTo(double(Speed),DesiredSpeed,double(D),Acceleration));
    const double Travel=FMath::Min(Distance,double(Speed)*D);
    bool bContact=false;
    const FVector Position=ConstrainFlightSegment(State->SystemSeed,Start,Start+Direction*Travel,bContact);
    FlightVelocity=(Position-Start)/D;SetActorLocation(Position,false);
    if(!Direction.IsNearlyZero())
    {
        const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,NearestPlanetIndex(),Position);
        FVector CameraUp=FVector::VectorPlaneProject(Up,Direction).GetSafeNormal();
        if(CameraUp.IsNearlyZero())CameraUp=GetActorUpVector();
        const FQuat Aim=FRotationMatrix::MakeFromXZ(Direction,CameraUp).ToQuat();
        SetActorRotation(FQuat::Slerp(GetActorQuat(),Aim,1.0-FMath::Exp(-D*3.0)).GetNormalized());
    }
    if(bContact){CancelCruise();Speed=0;return;}
    if(Distance-Travel<1.0)
    {
        ++CruiseLeg;
        if(CruiseLeg>=CruiseWaypoints.Num())
        {
            bCruising=false;CruiseWaypoints.Empty();CruiseLeg=0;Speed=0;FlightVelocity=FVector::ZeroVector;
            // Level with the destination's radial horizon; CTRL then descends
            // directly into its atmosphere without an entry action or loading cut.
            const int32 Planet=NearestPlanetIndex();
            const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,Planet,GetActorLocation());
            SetActorRotation(Voyager::TangentRotation(Up,GetActorForwardVector()));
            if(bAuthorityEffects)
            {
                if(auto PC=Cast<AVoyagerController>(GetController()))PC->Notify(TEXT("Cruise complete: 90 km orbit. Hold CTRL to descend toward the surface."));
                UE_LOG(LogTemp,Display,TEXT("VOYAGER CRUISE COMPLETE planet=%d altitude=%.2f epoch=%d"),Planet,SurfaceAltitude(),FlightEpoch);
            }
        }
    }
}

void AVoyagerShip::ServerFlightInputs_Implementation(const TArray<FVoyagerFlightMove>& Moves)
{
    if(!GetController()||Moves.Num()>90)return;
    const double Now=GetWorld()->GetTimeSeconds();
    SimulationCredit=FMath::Min(.45f,SimulationCredit+float(Now-LastInputPacketTime));LastInputPacketTime=Now;
    for(FVoyagerFlightMove Move:Moves)
    {
        if(Move.Epoch!=FlightEpoch||Move.Sequence<=LastProcessedSequence)continue;
        if(!FMath::IsFinite(Move.Seconds)||!FMath::IsFinite(Move.Throttle)||!FMath::IsFinite(Move.Strafe)||!FMath::IsFinite(Move.Lift)||!FMath::IsFinite(Move.Yaw)||!FMath::IsFinite(Move.Pitch))continue;
        Move.Seconds=FMath::Clamp(Move.Seconds,.000001f,.06f);
        if(Move.Seconds>SimulationCredit+.01f)break;
        SimulationCredit=FMath::Max(0.f,SimulationCredit-Move.Seconds);
        Move.Throttle=FMath::Clamp(Move.Throttle,-1.f,1.f);Move.Strafe=FMath::Clamp(Move.Strafe,-1.f,1.f);Move.Lift=FMath::Clamp(Move.Lift,-1.f,1.f);
        LastProcessedSequence=Move.Sequence;SimulateMove(Move,true);
        if(FlightEpoch!=Move.Epoch)break;
    }
    PublishFlightState();ForceNetUpdate();
}

void AVoyagerShip::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);Age+=DeltaSeconds;
    HitFeedback=FMath::Max(0.f,HitFeedback-DeltaSeconds);DamageFeedback=FMath::Max(0.f,DamageFeedback-DeltaSeconds);
    if(IsLocallyControlled())
    {
        // A fixed simulation cadence makes flight independent of rendering FPS.
        // In particular, uncapped/headless clients must not inflate tiny frame
        // deltas or overflow an input packet with thousands of moves per second.
        constexpr float Step=1.f/120.f;
        SimulationAccumulator=FMath::Min(.25f,SimulationAccumulator+DeltaSeconds);
        PendingYaw+=MouseYaw*1.15f;PendingPitch-=MousePitch*1.05f;
        const int32 Steps=FMath::FloorToInt(SimulationAccumulator/Step);
        const float StepYaw=Steps>0?PendingYaw/Steps:0.f;
        const float StepPitch=Steps>0?PendingPitch/Steps:0.f;
        if(Steps>0){PendingYaw=0;PendingPitch=0;}
        for(int32 Index=0;Index<Steps;++Index)
        {
            SimulationAccumulator=FMath::Max(0.f,SimulationAccumulator-Step);
            FVoyagerFlightMove Move;Move.Sequence=++LocalSequence;Move.Epoch=FlightEpoch;Move.Seconds=Step;
            Move.Throttle=bTestInput?TestThrottle:ThrottleInput;Move.Strafe=StrafeInput;Move.Lift=bTestInput?TestLift:LiftInput;
            Move.Yaw=FMath::Clamp(StepYaw,-150.f*Step,150.f*Step);
            Move.Pitch=FMath::Clamp(StepPitch,-100.f*Step,100.f*Step);
            Move.bBoost=bTestInput?bTestBoost:bBoostInput;Move.bPaused=!CanAct();
            Move.bCruiseObserved=bCruising;
            if(HasAuthority()){LastProcessedSequence=Move.Sequence;SimulateMove(Move,true);}
            else {PendingMoves.Add(Move);SimulateMove(Move,false);}
        }
        if(!HasAuthority())
        {
            InputSendTime+=DeltaSeconds;
            // Bound memory during disconnect; travel/acknowledgement clears the queue normally.
            if(PendingMoves.Num()>90)PendingMoves.RemoveAt(0,PendingMoves.Num()-90);
            if(InputSendTime>=.05f){InputSendTime=0;ServerFlightInputs(PendingMoves);}
        }
        if(bFiring&&CanAct()&&Age>=NextShot){NextShot=Age+.16f;ServerShoot();}
    }
    MouseYaw=0;MousePitch=0;
    NetDiagnosticTime+=DeltaSeconds;
    if(NetDiagnosticTime>=5.f&&GetController()&&FParse::Param(FCommandLine::Get(),TEXT("VoyagerNetTest")))
    {
        NetDiagnosticTime=0;
        UE_LOG(LogTemp,Display,TEXT("VOYAGER FLIGHT net=%d authority=%d local=%d epoch=%d ack=%u sequence=%u pending=%d landed=%d z=%.2f"),
            int32(GetNetMode()),HasAuthority()?1:0,IsLocallyControlled()?1:0,FlightEpoch,
            HasAuthority()?LastProcessedSequence:FlightState.Acknowledged,LocalSequence,PendingMoves.Num(),bLanded?1:0,GetActorLocation().Z);
    }
    if(HasAuthority())
    {
        StateSendTime+=DeltaSeconds;
        if(StateSendTime>=.05f){StateSendTime=0;PublishFlightState();}
        if(Hull>0&&GetWorld()->GetTimeSeconds()-LastDamage>6.f)Shield=FMath::Min(100.f,Shield+DeltaSeconds*9.f);
    }
    const float DesiredBank=IsLocallyControlled()&&!bLanded?FMath::Clamp(-StrafeInput*20.f-ThrottleInput*2.f,-27.f,27.f):0.f;
    VisualBank=FMath::FInterpTo(VisualBank,DesiredBank,DeltaSeconds,4.f);
    VisualRoot->SetRelativeRotation(FRotator(0,0,VisualBank));
    VisualRoot->SetRelativeLocation(FVector(0,0,bLanded?FMath::Sin(Age*1.8f)*3.f:0.f));
    const float Thrust=bLanded?.15f:FMath::Clamp(.3f+float(FlightVelocity.Size())/250000.f,.3f,2.2f);
    for(int32 Index=0;Index<Engines.Num();++Index)
    {
        auto E=Engines[Index];if(!E)continue;
        const float Flicker=1.f+FMath::Sin(Age*29.f+Index)*.055f;
        if(Index%2)E->SetRelativeScale3D(FVector(.39f,.39f,(.32f+Thrust)*Flicker));
        else E->SetRelativeScale3D(FVector(.32f*Flicker,.44f,.44f));
    }
    Camera->SetFieldOfView(FMath::FInterpTo(Camera->FieldOfView,88.f+FMath::Clamp(float(FlightVelocity.Size())/2000000.f,0.f,1.f)*13.f,DeltaSeconds,3.f));
    if(EffectsRemaining>0)
    {
        EffectsRemaining-=DeltaSeconds;
        if(EffectsRemaining<=0){for(auto Beam:LaserBeams)if(Beam)Beam->SetVisibility(false);if(Impact)Impact->SetVisibility(false);}
    }
}

void AVoyagerShip::Forward(float Value){ThrottleInput=FMath::Clamp(Value,-1.f,1.f);}
void AVoyagerShip::Right(float Value){StrafeInput=FMath::Clamp(Value,-1.f,1.f);}
void AVoyagerShip::Lift(float Value){LiftInput=FMath::Clamp(Value,-1.f,1.f);}
void AVoyagerShip::Turn(float Value){MouseYaw+=Value;}
void AVoyagerShip::Look(float Value){MousePitch+=Value;}
void AVoyagerShip::Boost(){bBoostInput=true;}
void AVoyagerShip::StopBoost(){bBoostInput=false;}
void AVoyagerShip::StartFire(){if(CanAct())bFiring=true;}
void AVoyagerShip::StopFire(){bFiring=false;}
void AVoyagerShip::Interact(){if(CanAct())ServerInteract();}
void AVoyagerShip::SelectTarget(){if(CanAct())ServerSelectTarget();}
void AVoyagerShip::Warp(){if(CanAct())ServerWarp();}
void AVoyagerShip::NextSystem(){if(CanAct())ServerNextSystem();}
void AVoyagerShip::SetFlightTestInput(float Throttle,float LiftValue,bool bBoost){bTestInput=true;TestThrottle=FMath::Clamp(Throttle,-1.f,1.f);TestLift=FMath::Clamp(LiftValue,-1.f,1.f);bTestBoost=bBoost;}
void AVoyagerShip::ClearFlightTestInput(){bTestInput=false;TestThrottle=0;TestLift=0;bTestBoost=false;}

void AVoyagerShip::ServerInteract_Implementation()
{
    if(!CanAct())return;
    auto State=GetWorld()->GetGameState<AVoyagerState>();auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>();if(!State||!GM)return;
    if(bLanded||SurfaceAltitude()<1000.0)
    {
        const int32 Planet=NearestPlanetIndex();const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,Planet,GetActorLocation());
        const FVector Position=Voyager::SurfacePoint(State->SystemSeed,Planet,Up,160.0);
        CancelCruise();bLanded=true;Speed=0;FlightVelocity=FVector::ZeroVector;
        SetActorLocationAndRotation(Position,Voyager::TangentRotation(Up,GetActorForwardVector()),false);
        PublishFlightState();ForceNetUpdate();GM->LeaveShip(this);
    }
    else if(auto PC=Cast<AVoyagerController>(GetController()))PC->Notify(TEXT("Fly down to the terrain to land. CTRL descends; E exits below 10 m."));
}

void AVoyagerShip::ServerSelectTarget_Implementation()
{
    if(!CanAct())return;TargetPlanet=(TargetPlanet+1)%Voyager::PlanetCount;ForceNetUpdate();
}
void AVoyagerShip::ServerWarp_Implementation(){if(CanAct())if(auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->WarpToPlanet(GetController(),TargetPlanet);}
void AVoyagerShip::ServerNextSystem_Implementation(){if(CanAct())if(auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->NextSystem(GetController());}

void AVoyagerShip::ServerShoot_Implementation()
{
    if(!CanAct()||bLanded)return;
    const float Now=GetWorld()->GetTimeSeconds();if(Now-LastServerShot<.145f)return;LastServerShot=Now;
    const FVector Start=GetActorLocation()+GetActorForwardVector()*250.f;
    const FVector Sight=Camera->GetComponentLocation();const FVector Direction=GetActorForwardVector();
    FVector End=Sight+Direction*100000.f;
    FHitResult SightHit;FCollisionQueryParams Query(SCENE_QUERY_STAT(VoyagerLaser),false,this);
    if(GetWorld()->LineTraceSingleByChannel(SightHit,Sight,End,ECC_Visibility,Query))End=SightHit.ImpactPoint;
    // A modest targeting cone keeps fast-moving fighters hittable with mouse flight.
    AVoyagerPirate* Assisted=nullptr;float BestAlignment=.9982f;
    for(TActorIterator<AVoyagerPirate> It(GetWorld());It;++It)
    {
        const FVector To=It->GetActorLocation()-Sight;const float Distance=To.Size();if(Distance>100000.f||Distance<400.f||It->Hull<=0)continue;
        const float Alignment=FVector::DotProduct(To/Distance,Direction);
        if(Alignment>BestAlignment){BestAlignment=Alignment;Assisted=*It;}
    }
    if(Assisted)End=Assisted->GetActorLocation();
    FHitResult Hit;bool bHit=GetWorld()->LineTraceSingleByChannel(Hit,Start,End+(End-Start).GetSafeNormal()*40.f,ECC_Visibility,Query);
    if(bHit)
    {
        End=Hit.ImpactPoint;float Damage=20.f;if(auto PS=GetPlayerState<AVoyagerPlayerState>())Damage+=PS->Upgrades*5.f;
        const float Applied=UGameplayStatics::ApplyPointDamage(Hit.GetActor(),Damage,(End-Start).GetSafeNormal(),Hit,GetController(),this,nullptr);
        if(Applied>0)ConfirmHit();
    }
    for(TActorIterator<AVoyagerCitizenManager> It(GetWorld());It;++It)
    {
        It->ReportDisturbance(Start);
        if(bHit)It->ReportDisturbance(End);
    }
    LaserEffect(End,bHit);
}

void AVoyagerShip::LaserEffect_Implementation(FVector End,bool bHit)
{
    if(GetNetMode()==NM_DedicatedServer)return;
    for(int32 Index=0;Index<LaserBeams.Num();++Index)
    {
        const FVector Start=GetActorTransform().TransformPosition(FVector(145,Index?192:-192,-9));
        SetBeam(LaserBeams[Index],Start,End,.035f);
    }
    if(Impact){Impact->SetWorldLocation(End);Impact->SetVisibility(bHit);}EffectsRemaining=.08f;
    if(auto Sound=LoadObject<USoundBase>(nullptr,TEXT("/Game/Audio/S_Gun.S_Gun")))UGameplayStatics::PlaySoundAtLocation(this,Sound,GetActorLocation(),.25f,1.6f);
    if(bHit)SpawnBurst(this,End,.9f,FLinearColor(.025f,.8f,1.f));
}

void AVoyagerShip::ConfirmHit_Implementation(){HitFeedback=.2f;}

float AVoyagerShip::TakeDamage(float Damage,const FDamageEvent& Event,AController* DamageInstigator,AActor* Causer)
{
    if(!HasAuthority()||Hull<=0||bRecovering||Damage<=0||!FMath::IsFinite(Damage))return 0;
    if(auto State=GetWorld()->GetGameState<AVoyagerState>())if(State->bTransitioning)return 0;
    LastDamage=GetWorld()->GetTimeSeconds();const float Absorbed=FMath::Min(Shield,Damage);Shield-=Absorbed;
    Hull=FMath::Max(0.f,Hull-Damage+Absorbed);DamageEffect(Hull<=0);ForceNetUpdate();
    if(Hull<=0)
    {
        bRecovering=true;
        if(auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->RecoverShip(this);
    }
    return Damage;
}

void AVoyagerShip::DamageEffect_Implementation(bool bDestroyed)
{
    DamageFeedback=bDestroyed?1.4f:.6f;
    SpawnBurst(this,GetActorLocation(),bDestroyed?7.f:2.f,bDestroyed?FLinearColor(1,.2f,.015f):FLinearColor(.04f,.55f,1.f));
    if(bDestroyed)if(auto Sound=LoadObject<USoundBase>(nullptr,TEXT("/Game/Audio/S_Explosion.S_Explosion")))UGameplayStatics::PlaySoundAtLocation(this,Sound,GetActorLocation(),.65f);
}

AVoyagerPirate::AVoyagerPirate()
{
    PrimaryActorTick.bCanEverTick=true;bReplicates=true;bAlwaysRelevant=true;SetReplicateMovement(false);SetNetUpdateFrequency(20);
    Collision=CreateDefaultSubobject<USphereComponent>(TEXT("PirateCollision"));SetRootComponent(Collision);Collision->InitSphereRadius(245.f);
    Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);Collision->SetCollisionObjectType(ECC_Pawn);
    Collision->SetCollisionResponseToAllChannels(ECR_Ignore);Collision->SetCollisionResponseToChannel(ECC_Visibility,ECR_Block);Collision->SetGenerateOverlapEvents(false);
    VisualRoot=CreateDefaultSubobject<USceneComponent>(TEXT("RaiderBody"));VisualRoot->SetupAttachment(Collision);Tags.Add(TEXT("VoyagerPirate"));
}

void AVoyagerPirate::BeginPlay()
{
    Super::BeginPlay();OrbitPhase=FMath::FRandRange(0.f,6.283f);FireTime=FMath::FRandRange(3.5f,6.f);
    if(HasAuthority()){Pose.Location=GetActorLocation();Pose.Rotation=GetActorRotation();}
    const FLinearColor Dark(.055f,.022f,.028f),Red(.48f,.055f,.025f),Glow(1.f,.065f,.008f);
    auto Part=[&](const TCHAR* Name,const TCHAR* Shape,FVector At,FVector Scale,FLinearColor Color,bool bGlow=false)
    {return RiftVisual::Mesh(this,VisualRoot,FName(Name),Shape,At,Scale,Color,bGlow);};
    Part(TEXT("RaiderCore"),TEXT("Cube"),FVector::ZeroVector,FVector(2.8f,1.3f,.65f),Dark);
    Part(TEXT("RaiderNose"),TEXT("Cone"),FVector(150,0,0),FVector(1.2f,.58f,1.8f),Red)->SetRelativeRotation(FRotator(-90,0,0));
    Part(TEXT("RaiderCockpit"),TEXT("Sphere"),FVector(48,0,43),FVector(.85f,.83f,.3f),Glow,true);
    for(int32 Side=-1;Side<=1;Side+=2)
    {
        Part(*FString::Printf(TEXT("RaiderWing%d"),Side),TEXT("Cube"),FVector(-3,Side*169,0),FVector(.8f,2.7f,.24f),Red)->SetRelativeRotation(FRotator(0,Side*-32.f,Side*18.f));
        Part(*FString::Printf(TEXT("RaiderBlade%d"),Side),TEXT("Cone"),FVector(105,Side*270,18),FVector(.53f,.38f,2.6f),Dark)->SetRelativeRotation(FRotator(-90,0,0));
        Part(*FString::Printf(TEXT("RaiderGun%d"),Side),TEXT("Sphere"),FVector(208,Side*270,18),FVector(.19f),Glow,true);
        Part(*FString::Printf(TEXT("RaiderEngine%d"),Side),TEXT("Sphere"),FVector(-170,Side*75,0),FVector(.55f,.4f,.4f),Glow,true);
    }
    Engine=Part(TEXT("RaiderExhaust"),TEXT("Cone"),FVector(-237,0,0),FVector(.62f,.62f,2.1f),Glow,true);Engine->SetRelativeRotation(FRotator(90,0,0));
    Beam=Part(TEXT("RaiderBeam"),TEXT("Cylinder"),FVector::ZeroVector,FVector(.035f),Glow,true);Beam->SetVisibility(false);
}

void AVoyagerPirate::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(AVoyagerPirate,Hull);DOREPLIFETIME(AVoyagerPirate,Pose);}

void AVoyagerPirate::OnRep_Pose()
{
    if(!HasAuthority())SetActorLocationAndRotation(Pose.Location,Pose.Rotation,false);
}

AVoyagerShip* AVoyagerPirate::FindTarget() const
{
    AVoyagerShip* Best=nullptr;float Distance=MAX_flt;
    for(TActorIterator<AVoyagerShip> It(GetWorld());It;++It)
    {
        if(!It->GetController()||It->Hull<=0||It->bLanded||It->bCruising||It->SurfaceAltitude()<Voyager::AtmosphereHeight)continue;
        const float Candidate=FVector::DistSquared(It->GetActorLocation(),GetActorLocation());
        if(Candidate<Distance){Distance=Candidate;Best=*It;}
    }
    return Best;
}

void AVoyagerPirate::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);Age+=DeltaSeconds;
    if(Engine)Engine->SetRelativeScale3D(FVector(.62f,.62f,2.f+FMath::Sin(Age*22.f)*.15f));
    if(EffectRemaining>0){EffectRemaining-=DeltaSeconds;if(EffectRemaining<=0&&Beam)Beam->SetVisibility(false);}
    if(!HasAuthority()||Hull<=0)return;
    const auto State=GetWorld()->GetGameState<AVoyagerState>();if(!State||State->bTransitioning)return;
    AVoyagerShip* Target=FindTarget();if(!Target)return;
    const float Orbit=Age*.17f+OrbitPhase;
    const FVector Position=Target->GetActorLocation();
    const FVector Desired=Position+Target->GetActorForwardVector()*(5700.f+FMath::Sin(Orbit*.7f)*2300.f)
        +Target->GetActorRightVector()*FMath::Sin(Orbit)*4700.f+Target->GetActorUpVector()*(700.f+FMath::Cos(Orbit*1.2f)*2100.f);
    const FVector Displacement=Desired-GetActorLocation();const float Distance=Displacement.Size();
    const FVector Velocity=Displacement.GetSafeNormal()*FMath::Min(double(Distance)*1.05,Target->GetVelocity().Size()+6500.0);
    bool bContact=false;
    SetActorLocation(ConstrainFlightSegment(State->SystemSeed,GetActorLocation(),GetActorLocation()+Velocity*FMath::Min(DeltaSeconds,.06f),bContact),false);
    const FRotator Aim=(Position-GetActorLocation()).Rotation();SetActorRotation(FMath::RInterpTo(GetActorRotation(),Aim,DeltaSeconds,1.9f));
    Pose.Location=GetActorLocation();Pose.Rotation=GetActorRotation();
    VisualRoot->SetRelativeRotation(FRotator(0,0,FMath::Sin(Orbit)*19.f));
    if(WarningTime>0)
    {
        WarningTime-=DeltaSeconds;
        if(WarningTime<=0&&AttackTarget.IsValid())
        {
            BeamEffect(AimPoint,false);
            // The telegraphed shot stays on its original line; maneuvering evades it.
            const FVector Start=GetActorLocation();const FVector Line=(AimPoint-Start).GetSafeNormal();
            const FVector ToTarget=AttackTarget->GetActorLocation()-Start;
            const FVector Closest=Start+Line*FMath::Clamp(FVector::DotProduct(ToTarget,Line),0.f,(AimPoint-Start).Size()+400.f);
            if(FVector::DistSquared(Closest,AttackTarget->GetActorLocation())<FMath::Square(480.f))
                UGameplayStatics::ApplyDamage(AttackTarget.Get(),12.f,nullptr,this,nullptr);
        }
    }
    FireTime-=DeltaSeconds;
    if(FireTime<=0&&FVector::DistSquared(Position,GetActorLocation())<FMath::Square(24000.f))
    {
        FireTime=FMath::FRandRange(3.3f,5.f);WarningTime=.7f;AttackTarget=Target;
        AimPoint=Position+Target->GetVelocity()*.22f;
        BeamEffect(AimPoint,true);
    }
}

void AVoyagerPirate::BeamEffect_Implementation(FVector End,bool bWarning)
{
    if(GetNetMode()==NM_DedicatedServer)return;
    SetBeam(Beam,GetActorLocation()+GetActorForwardVector()*170.f,End,bWarning?.008f:.07f);EffectRemaining=bWarning?.6f:.15f;
    if(!bWarning)if(auto Sound=LoadObject<USoundBase>(nullptr,TEXT("/Game/Audio/S_Gun.S_Gun")))UGameplayStatics::PlaySoundAtLocation(this,Sound,GetActorLocation(),.2f,.7f);
}

float AVoyagerPirate::TakeDamage(float Damage,const FDamageEvent& Event,AController* DamageInstigator,AActor* Causer)
{
    if(!HasAuthority()||Hull<=0||Damage<=0||!FMath::IsFinite(Damage))return 0;
    Hull=FMath::Max(0.f,Hull-Damage);ForceNetUpdate();
    if(Hull<=0)
    {
        if(DamageInstigator)if(auto PS=DamageInstigator->GetPlayerState<AVoyagerPlayerState>()){PS->AddMinerals(25);++PS->PirateKills;PS->ForceNetUpdate();}
        DeathEffect();SetActorEnableCollision(false);SetLifeSpan(.2f);
        if(auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->SaveExpedition();
    }
    return Damage;
}

void AVoyagerPirate::DeathEffect_Implementation()
{
    SpawnBurst(this,GetActorLocation(),9.f,FLinearColor(1.f,.19f,.018f));VisualRoot->SetVisibility(false,true);
    if(GetNetMode()!=NM_DedicatedServer)if(auto Sound=LoadObject<USoundBase>(nullptr,TEXT("/Game/Audio/S_Explosion.S_Explosion")))UGameplayStatics::PlaySoundAtLocation(this,Sound,GetActorLocation(),.65f,.8f);
}
