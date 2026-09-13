#include "VoyagerCitizen.h"
#include "VoyagerData.h"
#include "VoyagerGameMode.h"
#include "VoyagerSettlement.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"

namespace
{
    enum ECitizenActivity : uint8 { Commute,Work,Eat,Rest,Patrol,Social,Investigate,Shelter,Talk };
    constexpr int32 StreetNodes=81;
    constexpr int32 NodesPerBuilding=4; // Street, ramp lip, inside doorway, room center.
    int32 BuildingNode(int32 Building,int32 Offset=3) { return StreetNodes+Building*NodesPerBuilding+Offset; }
    bool IsStreetNode(int32 Node) { return Node<StreetNodes||(Node-StreetNodes)%NodesPerBuilding==0; }

    USceneComponent* CitizenJoint(AActor* Owner,USceneComponent* Parent,FVector At,TArray<TObjectPtr<UActorComponent>>& Parts)
    {
        auto Joint=NewObject<USceneComponent>(Owner);Joint->SetupAttachment(Parent);Joint->SetRelativeLocation(At);
        Joint->RegisterComponent();Parts.Add(Joint);return Joint;
    }

    UMaterialInstanceDynamic* CitizenMaterial(AActor* Owner,FLinearColor Tint,bool bSkin=false)
    {
        auto Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_VoyagerCitizen.M_VoyagerCitizen"));
        if(!Base)Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_Surface.M_Surface"));
        if(!Base)Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
        auto Result=UMaterialInstanceDynamic::Create(Base,Owner);
        if(Result)
        {
            Result->SetVectorParameterValue(TEXT("Tint"),Tint);Result->SetVectorParameterValue(TEXT("Color"),Tint);
            Result->SetScalarParameterValue(TEXT("IsSkin"),bSkin?1.f:0.f);Result->SetScalarParameterValue(TEXT("Roughness"),bSkin?.65f:.84f);
            Result->SetScalarParameterValue(TEXT("DetailScale"),1.f);
        }
        return Result;
    }
}

AVoyagerCitizen::AVoyagerCitizen()
{
    PrimaryActorTick.bCanEverTick=true;bReplicates=true;SetReplicateMovement(false);
    SetNetUpdateFrequency(10.f);SetMinNetUpdateFrequency(4.f);SetNetCullDistanceSquared(FMath::Square(85000.f));
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("CitizenRoot")));
    VisualRoot=CreateDefaultSubobject<USceneComponent>(TEXT("CitizenVisuals"));VisualRoot->SetupAttachment(GetRootComponent());
    VisualRoot->SetAbsolute(true,true,false);
    InteractionCapsule=CreateDefaultSubobject<UCapsuleComponent>(TEXT("CitizenInteraction"));
    InteractionCapsule->SetupAttachment(GetRootComponent());InteractionCapsule->InitCapsuleSize(28.f,90.f);
    InteractionCapsule->SetRelativeLocation(FVector(0,0,90));InteractionCapsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    InteractionCapsule->SetCollisionObjectType(ECC_Pawn);InteractionCapsule->SetCollisionResponseToAllChannels(ECR_Ignore);
    InteractionCapsule->SetCollisionResponseToChannel(ECC_Visibility,ECR_Block);InteractionCapsule->SetGenerateOverlapEvents(false);
    Tags.Add(TEXT("VoyagerCitizen"));
}

void AVoyagerCitizen::InitializeCitizen(int32 System,int32 Planet,int32 Site,int32 Ordinal,int32 StartNode)
{
    if(!HasAuthority())return;
    Identity.System=System;Identity.Planet=Planet;Identity.Site=Site;Identity.Ordinal=Ordinal;Identity.Role=Ordinal%6;
    Identity.Seed=int32(Voyager::Hash(uint32(Voyager::PlanetSeed(System,Planet))^uint32(Site*7717+Ordinal*104729+5371))&0x7fffffff);
    Random.Initialize(Identity.Seed);Identity.Scale=Random.FRandRange(.94f,1.06f);
    Identity.Home=BuildingForRole(4,Ordinal);const int32 WorkplaceRoles[]={0,3,2,1,5,2};
    Identity.Workplace=BuildingForRole(WorkplaceRoles[Identity.Role],Ordinal/6);
    Identity.bInitialized=true;BuildNavigation();CurrentNode=FMath::Clamp(StartNode,0,Navigation.Num()-1);
    PathPosition=Navigation[CurrentNode].Position;SetActorLocation(PathPosition);
    Pose.Location=PathPosition;Pose.Rotation=GetActorRotation();Pose.ServerTime=float(SynchronizedTime());
    ThinkRemaining=Random.FRandRange(.2f,2.f);IdleRemaining=Ordinal<6?Random.FRandRange(3.f,9.f):0.f;
    ArrivalActivity=Pose.Activity=Ordinal%6==4?Patrol:Social;
    if(CurrentNode>=StreetNodes&&(CurrentNode-StreetNodes)%NodesPerBuilding>=2)
    {
        Pose.Building=(CurrentNode-StreetNodes)/NodesPerBuilding;
        ArrivalActivity=Pose.Activity=Work;IdleRemaining=Random.FRandRange(18.f,30.f);
    }
    if(HasActorBegunPlay())BuildVisuals();ForceNetUpdate();
}

void AVoyagerCitizen::BeginPlay()
{
    Super::BeginPlay();VisualRoot->SetWorldLocationAndRotation(GetActorLocation(),GetActorQuat());
    if(Identity.bInitialized)BuildVisuals();
}

void AVoyagerCitizen::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(AVoyagerCitizen,Identity);DOREPLIFETIME(AVoyagerCitizen,Pose);
}

void AVoyagerCitizen::OnRep_Identity() { if(HasActorBegunPlay()&&Identity.bInitialized)BuildVisuals(); }
void AVoyagerCitizen::OnRep_Pose()
{
    if(!HasAuthority())
    {
        SetActorLocationAndRotation(Pose.Location,Pose.Rotation,false);
        if(!bVisualsBuilt)VisualRoot->SetWorldLocationAndRotation(Pose.Location,Pose.Rotation);
    }
}

double AVoyagerCitizen::SynchronizedTime() const
{
    if(const auto State=GetWorld()->GetGameState<AVoyagerState>())return State->GetServerWorldTimeSeconds();
    return GetWorld()->GetTimeSeconds();
}

FString AVoyagerCitizen::DisplayName() const
{
    static const TCHAR* First[]={TEXT("Mara"),TEXT("Elias"),TEXT("Nia"),TEXT("Tomas"),TEXT("Sora"),TEXT("Idris"),TEXT("Lena"),TEXT("Arun"),TEXT("Ada"),TEXT("Noah"),TEXT("Aya"),TEXT("Leon"),TEXT("Imani"),TEXT("Kai"),TEXT("Esme"),TEXT("Ravi")};
    static const TCHAR* Last[]={TEXT("Voss"),TEXT("Okafor"),TEXT("Chen"),TEXT("Navarro"),TEXT("Singh"),TEXT("Vale"),TEXT("Moreau"),TEXT("Sato"),TEXT("Reyes"),TEXT("Hale"),TEXT("Patel"),TEXT("Costa"),TEXT("Kim"),TEXT("Mensah"),TEXT("Park"),TEXT("Aziz")};
    return FString::Printf(TEXT("%s %s"),First[uint32(Identity.Seed)%16],Last[Voyager::Hash(uint32(Identity.Seed))%16]);
}

FString AVoyagerCitizen::RoleName() const
{
    static const TCHAR* Roles[]={TEXT("Resident"),TEXT("Engineer"),TEXT("Trader"),TEXT("Medic"),TEXT("City guard"),TEXT("Courier")};
    return Roles[FMath::Clamp(Identity.Role,0,5)];
}

FString AVoyagerCitizen::ActivityName() const
{
    static const TCHAR* Activities[]={TEXT("Walking to next stop"),TEXT("On shift"),TEXT("Taking a meal break"),TEXT("At home"),TEXT("Patrolling the district"),TEXT("Taking a break"),TEXT("Investigating a disturbance"),TEXT("Seeking shelter"),TEXT("Speaking with you")};
    return Activities[FMath::Clamp(int32(Pose.Activity),0,8)];
}

int32 AVoyagerCitizen::BuildingForRole(int32 DesiredRole,int32 Variation) const
{
    TArray<int32> Matches;
    for(int32 I=0;I<AVoyagerSettlement::BuildingCount();++I)
    {
        FVoyagerBuildingInfo Info;
        if(AVoyagerSettlement::GetBuildingInfo(Identity.System,Identity.Planet,Identity.Site,I,Info)&&Info.Role==DesiredRole)Matches.Add(I);
    }
    return Matches.IsEmpty()?0:Matches[FMath::Abs(Variation)%Matches.Num()];
}

void AVoyagerCitizen::BuildNavigation()
{
    Navigation.SetNum(StreetNodes+AVoyagerSettlement::BuildingCount()*NodesPerBuilding);
    auto Connect=[this](int32 A,int32 B){Navigation[A].Links.AddUnique(B);Navigation[B].Links.AddUnique(A);};
    for(int32 Y=0;Y<9;++Y)for(int32 X=0;X<9;++X)
    {
        const int32 N=Y*9+X;Navigation[N].Position=AVoyagerSettlement::StreetPoint(Identity.System,Identity.Planet,Identity.Site,X,Y,20.0);
        if(X>0)Connect(N,N-1);if(Y>0)Connect(N,N-9);
    }
    int32 Building=0;
    for(int32 Y=0;Y<8;++Y)for(int32 X=0;X<8;++X)
    {
        if((X==3||X==4)&&(Y==3||Y==4))continue;
        FVoyagerBuildingInfo Info;
        if(AVoyagerSettlement::GetBuildingInfo(Identity.System,Identity.Planet,Identity.Site,Building,Info))
        {
            const int32 N=BuildingNode(Building,0);
            Navigation[N].Position=Info.DoorOutside;Navigation[N+1].Position=Info.DoorThreshold;
            Navigation[N+2].Position=Info.DoorInside;
            Navigation[N+3].Position=Info.InteriorPoint+Info.Forward*double((Identity.Ordinal%5-2)*55)+Info.Right*double(((Identity.Ordinal/5)%5-2)*55);
            Connect(N,Y*9+X);Connect(N,Y*9+X+1);Connect(N,N+1);Connect(N+1,N+2);Connect(N+2,N+3);
        }
        ++Building;
    }
}

int32 AVoyagerCitizen::NearestStreetNode(const FVector& Position) const
{
    int32 Best=0;double Distance=TNumericLimits<double>::Max();
    for(int32 I=0;I<StreetNodes;++I)
    {
        const double D=FVector::DistSquared(Position,Navigation[I].Position);
        if(D<Distance){Distance=D;Best=I;}
    }
    return Best;
}

void AVoyagerCitizen::RouteTo(int32 DestinationNode,uint8 FinalActivity)
{
    if(!Navigation.IsValidIndex(DestinationNode))return;
    // A new decision may happen midway through an edge. Return to either end of
    // that same edge before searching again; never shortcut through a building.
    int32 Start=CurrentNode;
    if(Route.IsValidIndex(RouteCursor))
    {
        const int32 Next=Route[RouteCursor];
        if(FVector::DistSquared(PathPosition,Navigation[Next].Position)<FVector::DistSquared(PathPosition,Navigation[Start].Position))Start=Next;
    }
    TArray<int32> Parent;Parent.Init(INDEX_NONE,Navigation.Num());TArray<int32> Queue;Queue.Add(Start);Parent[Start]=Start;
    for(int32 Cursor=0;Cursor<Queue.Num()&&Parent[DestinationNode]==INDEX_NONE;++Cursor)
    {
        const int32 Node=Queue[Cursor];
        for(int32 Link:Navigation[Node].Links)if(Parent[Link]==INDEX_NONE){Parent[Link]=Node;Queue.Add(Link);}
    }
    if(Parent[DestinationNode]==INDEX_NONE)return;
    TArray<int32> Reverse;for(int32 N=DestinationNode;N!=Start;N=Parent[N])Reverse.Add(N);
    Route.Reset();RouteCursor=0;
    if(FVector::DistSquared(PathPosition,Navigation[Start].Position)>FMath::Square(6.f))Route.Add(Start);
    else CurrentNode=Start;
    for(int32 I=Reverse.Num()-1;I>=0;--I)Route.Add(Reverse[I]);
    ArrivalActivity=FinalActivity;Pose.Activity=Pose.bAlarmed?(Identity.Role==4?Investigate:Shelter):(Identity.Role==4?Patrol:Commute);
    IdleRemaining=0.f;
    if(Route.IsEmpty()){Pose.Activity=ArrivalActivity;IdleRemaining=Random.FRandRange(12.f,28.f);}
}

void AVoyagerCitizen::ChooseActivity()
{
    const float Hour=AVoyagerCitizenManager::CityHour(GetWorld());
    if(Hour>=22.f||Hour<6.f)
    {
        if(Identity.Role==4&&Identity.Ordinal%2==0)RouteTo(Random.RandRange(0,80),Patrol);
        else RouteTo(BuildingNode(Identity.Home),Rest);
        return;
    }
    if((Hour>=12.f&&Hour<14.f)||(Hour>=18.f&&Hour<20.f))
    {RouteTo(BuildingNode(BuildingForRole(0,Identity.Ordinal/3)),Eat);return;}
    if(Identity.Role==4){RouteTo(Random.RandRange(0,80),Patrol);return;}
    if(Identity.Role==5)
    {RouteTo(BuildingNode(BuildingForRole(Random.RandRange(0,3),Random.RandRange(0,9))),Work);return;}
    if(Random.FRand()<.30f)
    {
        // The public square and the streets around it are always reachable on
        // the same graph used for work commutes and building entrances.
        RouteTo(Random.RandRange(2,6)+Random.RandRange(2,6)*9,Social);return;
    }
    if(Identity.Role==0)
    {RouteTo(BuildingNode(BuildingForRole(Random.FRand()<.5f?2:0,Identity.Ordinal)),Random.FRand()<.5f?Social:Eat);return;}
    RouteTo(BuildingNode(Identity.Workplace),Work);
}

FString AVoyagerCitizen::TalkTo(APawn* Player,int32 Topic)
{
    if(!HasAuthority()||!IsValid(Player)||Player->GetWorld()!=GetWorld()||!Player->IsPlayerControlled()||!Identity.bInitialized)return FString();
    const auto State=GetWorld()->GetGameState<AVoyagerState>();
    if(!State||State->SystemSeed!=Identity.System||State->bTransitioning)return FString();
    const float Time=float(SynchronizedTime());
    if(Topic<0||Topic>2||Time-LastTalkTime<.55f||FVector::DistSquared(Player->GetActorLocation(),GetActorLocation())>FMath::Square(400.f))return FString();
    const FVector Up=Voyager::SurfaceNormal(Identity.System,Identity.Planet,GetActorLocation());
    FCollisionQueryParams Query(SCENE_QUERY_STAT(CitizenConversation),false,Player);Query.AddIgnoredActor(this);
    FHitResult Hit;
    if(GetWorld()->LineTraceSingleByChannel(Hit,Player->GetPawnViewLocation(),GetActorLocation()+Up*155.f,ECC_Visibility,Query))return FString();
    LastTalkTime=Time;ConversationPartner=Player;ConversationUntil=Time+20.f;Pose.Activity=Talk;Pose.Velocity=FVector::ZeroVector;ForceNetUpdate();
    if(Pose.bAlarmed)
        return Identity.Role==4?TEXT("I heard a disturbance. Keep your weapon lowered while I check the street. Everyone else should move inside."):TEXT("That noise put everyone on edge. I'm heading indoors until the street is quiet. Please keep your weapon lowered.");
    if(Topic==0)
    {
        static const TCHAR* Greetings[]={TEXT("Welcome. I live here, just off the square. The cafes and market are open; walk through any marked entrance."),TEXT("Hello, traveler. I keep this district's equipment running. I'm between the workshop and my next maintenance stop."),TEXT("A new face! I work at the market. Come inside and have a look around; the street doors are open."),TEXT("Good to see you on your feet. I'm one of the medics here. The clinic is open if you need a quiet place to rest."),TEXT("Welcome to the district. You're free to explore the buildings. Please keep mining tools and ship fire away from the streets."),TEXT("Hello! I'm making deliveries around the district. These ramps lead straight into the buildings. Mind the people at the doors.")};
        return Greetings[FMath::Clamp(Identity.Role,0,5)];
    }
    if(Topic==1)
    {
        FVoyagerBuildingInfo Home,Workplace;
        AVoyagerSettlement::GetBuildingInfo(Identity.System,Identity.Planet,Identity.Site,Identity.Home,Home);
        AVoyagerSettlement::GetBuildingInfo(Identity.System,Identity.Planet,Identity.Site,Identity.Workplace,Workplace);
        if(Identity.Role==0)return FString::Printf(TEXT("My home is %s. I visit the market in the day, eat at a cafe, and head home after ten."),*Home.Name);
        if(Identity.Role==4)return TEXT("I patrol the streets and check loud disturbances. We stagger the night watch so there's still someone outside when the district sleeps.");
        if(Identity.Role==5)return TEXT("I carry deliveries between the market, clinic, cafes and workshops. I take the streets and ramps; there's no quick route through somebody's walls.");
        return FString::Printf(TEXT("My usual shift is at %s. We break for food around noon and six, then return home after ten."),*Workplace.Name);
    }
    FVoyagerBuildingInfo Destination;
    AVoyagerSettlement::GetBuildingInfo(Identity.System,Identity.Planet,Identity.Site,BuildingForRole(Identity.Role==3?1:0,0),Destination);
    const double Distance=FVector::Dist(Player->GetActorLocation(),Destination.DoorOutside)/100.0;
    return FString::Printf(TEXT("%s is about %.0f meters from here. Follow the street to its sign, then take the ramp through the open door. All ground floors are accessible."),*Destination.Name,Distance);
}

void AVoyagerCitizen::EndConversation(APawn* Player)
{
    if(!HasAuthority()||!Player||ConversationPartner.Get()!=Player)return;
    ConversationUntil=0.f;ConversationPartner.Reset();
    Pose.Activity=Route.IsValidIndex(RouteCursor)?(Pose.bAlarmed?(Identity.Role==4?Investigate:Shelter):Commute):ArrivalActivity;
    ForceNetUpdate();
}

void AVoyagerCitizen::NotifyDisturbance(const FVector& Position)
{
    if(!HasAuthority()||!Identity.bInitialized)return;
    const float Time=float(SynchronizedTime());
    if(Time-LastAlarmTime<1.5f||FVector::DistSquared(GetActorLocation(),Position)>FMath::Square(14000.f))return;
    LastAlarmTime=Time;AlarmPosition=Position;AlarmUntil=Time+Random.FRandRange(12.f,19.f);Pose.bAlarmed=true;
    ConversationUntil=0.f;ConversationPartner.Reset();
    if(Identity.Role==4)RouteTo(NearestStreetNode(Position),Investigate);
    else
    {
        int32 Refuge=Identity.Home;double Best=TNumericLimits<double>::Max();
        for(int32 I=0;I<AVoyagerSettlement::BuildingCount();++I)
        {
            const double Distance=FVector::DistSquared(GetActorLocation(),Navigation[BuildingNode(I,0)].Position);
            if(Distance<Best){Best=Distance;Refuge=I;}
        }
        RouteTo(BuildingNode(Refuge),Shelter);
    }
    ForceNetUpdate();
}

void AVoyagerCitizen::Simulate(float D)
{
    if(!Identity.bInitialized)return;
    const auto State=GetWorld()->GetGameState<AVoyagerState>();
    if(!State||State->SystemSeed!=Identity.System||State->bTransitioning)return;
    const float Time=float(SynchronizedTime());const FVector Position=PathPosition;
    const FVector Up=Voyager::SurfaceNormal(Identity.System,Identity.Planet,Position);
    if(Pose.bAlarmed&&Time>=AlarmUntil)
    {
        Pose.bAlarmed=false;
        // Finish the doorway or road edge before choosing a normal activity.
        ArrivalActivity=Identity.Role==4?Patrol:Rest;
        if(!Route.IsValidIndex(RouteCursor)){IdleRemaining=2.f;Pose.Activity=ArrivalActivity;}
    }
    APawn* Partner=ConversationPartner.Get();
    if(Partner&&Time<ConversationUntil&&FVector::DistSquared(Position,Partner->GetActorLocation())<FMath::Square(550.f))
    {
        Pose.Activity=Talk;Pose.Velocity=FVector::ZeroVector;
        const FVector Facing=FVector::VectorPlaneProject(Partner->GetActorLocation()-Position,Up).GetSafeNormal();
        if(!Facing.IsNearlyZero())Pose.Rotation=Voyager::TangentRotation(Up,Facing);
        Pose.ServerTime=Time;SetActorRotation(Pose.Rotation);return;
    }
    if(Partner||ConversationUntil>0.f)
    {
        ConversationPartner.Reset();ConversationUntil=0.f;
        Pose.Activity=Route.IsValidIndex(RouteCursor)?(Pose.bAlarmed?(Identity.Role==4?Investigate:Shelter):Commute):ArrivalActivity;
    }
    ThinkRemaining-=D;
    if(ThinkRemaining<=0.f)
    {
        ThinkRemaining=Random.FRandRange(.65f,1.05f);
        if(!Route.IsValidIndex(RouteCursor)&&IdleRemaining<=0.f&&!Pose.bAlarmed)ChooseActivity();
        if(!Pose.bAlarmed&&Time-LastGreetingTime>35.f)
        {
            for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
            {
                APawn* Player=It->Get()?It->Get()->GetPawn():nullptr;if(!Player)continue;
                const double Dist=FVector::DistSquared(Position,Player->GetActorLocation());
                if(Dist>FMath::Square(140.f)&&Dist<FMath::Square(420.f))
                {
                    FCollisionQueryParams Query(SCENE_QUERY_STAT(CitizenGreeting),false,this);Query.AddIgnoredActor(Player);FHitResult Hit;
                    if(!GetWorld()->LineTraceSingleByChannel(Hit,Position+Up*155.f,Player->GetPawnViewLocation(),ECC_Visibility,Query))
                    {
                        LastGreetingTime=Time;ConversationPartner=Player;ConversationUntil=Time+2.5f;Pose.Activity=Talk;break;
                    }
                }
            }
        }
    }
    if(!Route.IsValidIndex(RouteCursor))
    {
        IdleRemaining-=D;Pose.Velocity=FVector::ZeroVector;Pose.ServerTime=Time;
        if(Pose.bAlarmed&&Identity.Role==4)
        {
            const FVector Facing=FVector::VectorPlaneProject(AlarmPosition-Position,Up).GetSafeNormal();
            if(!Facing.IsNearlyZero())Pose.Rotation=Voyager::TangentRotation(Up,Facing);
        }
        SetActorRotation(Pose.Rotation);return;
    }
    const int32 NextNode=Route[RouteCursor];const FVector Destination=Navigation[NextNode].Position;
    const FVector Delta=Destination-Position;const double Distance=Delta.Size();
    float Speed=Pose.bAlarmed?(Identity.Role==4?320.f:390.f):(Identity.Role==5?170.f:135.f);
    if(!IsStreetNode(CurrentNode)||!IsStreetNode(NextNode))Speed=FMath::Min(Speed,190.f);
    const FVector Direction=Delta.GetSafeNormal();
    // Personal space is resolved along the existing path, so yielding cannot
    // steer an inhabitant sideways through a wall or off an entrance ramp.
    for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        APawn* Player=It->Get()?It->Get()->GetPawn():nullptr;if(!Player)continue;
        const FVector ToPlayer=FVector::VectorPlaneProject(Player->GetActorLocation()-Position,Up);
        if(ToPlayer.SizeSquared()<FMath::Square(120.f)&&FVector::DotProduct(Direction,ToPlayer)>10.f)Speed=0.f;
    }
    for(TActorIterator<AVoyagerCitizen> It(GetWorld());It;++It)
    {
        const auto Other=*It;
        if(Other==this||Other->Identity.Planet!=Identity.Planet||Other->Identity.Site!=Identity.Site)continue;
        const FVector ToOther=FVector::VectorPlaneProject(Other->GetActorLocation()-GetActorLocation(),Up);
        if(ToOther.SizeSquared()>FMath::Square(105.f)||FVector::DotProduct(ToOther,Direction)<15.f)continue;
        const double Alignment=FVector::DotProduct(Direction,Other->GetActorForwardVector());
        if(Alignment>.35||Identity.Ordinal>Other->Identity.Ordinal)Speed=0.f;
    }
    const float Step=FMath::Min(float(Distance),Speed*D);FVector NewPosition=Position+Direction*Step;
    if(IsStreetNode(CurrentNode)&&IsStreetNode(NextNode))
        NewPosition=Voyager::SurfacePoint(Identity.System,Identity.Planet,Voyager::SurfaceNormal(Identity.System,Identity.Planet,NewPosition),20.f);
    if(Distance<=FMath::Max(4.f,Speed*D))
    {
        NewPosition=Destination;CurrentNode=NextNode;++RouteCursor;
        if(CurrentNode>=StreetNodes&&(CurrentNode-StreetNodes)%NodesPerBuilding>=2)Pose.Building=(CurrentNode-StreetNodes)/NodesPerBuilding;
        else if(IsStreetNode(CurrentNode))Pose.Building=INDEX_NONE;
        if(!Route.IsValidIndex(RouteCursor))
        {
            Pose.Activity=ArrivalActivity;
            IdleRemaining=ArrivalActivity==Rest?Random.FRandRange(35.f,65.f):Random.FRandRange(12.f,28.f);
            if(Identity.Role==5||ArrivalActivity==Patrol)IdleRemaining=Random.FRandRange(2.f,5.f);
        }
    }
    const bool bOnStreet=IsStreetNode(CurrentNode)&&IsStreetNode(NextNode);
    // Opposing pedestrians use opposite sides of a street. The authoritative
    // path itself stays on the road graph; this small offset fades out at ramps.
    const FVector DesiredPassing=bOnStreet&&Step>0.f?FVector::CrossProduct(Up,Direction).GetSafeNormal()*80.f:FVector::ZeroVector;
    PassingOffset=FMath::VInterpTo(PassingOffset,DesiredPassing,D,5.f);
    const FVector BodyPosition=NewPosition+PassingOffset;
    Pose.Velocity=(BodyPosition-Pose.Location)/FMath::Max(D,.001f);Pose.GaitDistance+=float((NewPosition-Position).Size());
    if(Step>1.f)
    {
        const FQuat Target=Voyager::TangentRotation(Voyager::SurfaceNormal(Identity.System,Identity.Planet,NewPosition),Direction).Quaternion();
        Pose.Rotation=FQuat::Slerp(GetActorQuat(),Target,1.f-FMath::Exp(-D*9.f)).GetNormalized().Rotator();
    }
    PathPosition=NewPosition;Pose.Location=BodyPosition;Pose.ServerTime=Time;SetActorLocationAndRotation(BodyPosition,Pose.Rotation,false);
}

void AVoyagerCitizen::ClearVisuals()
{
    for(int32 I=Parts.Num()-1;I>=0;--I)if(IsValid(Parts[I]))Parts[I]->DestroyComponent();
    Parts.Empty();Materials.Empty();DetailParts.Empty();Hips.Empty();Knees.Empty();Shoulders.Empty();Elbows.Empty();
    BodyRoot=nullptr;HeadPivot=nullptr;bVisualsBuilt=false;
}

void AVoyagerCitizen::BuildVisuals()
{
    if(!Identity.bInitialized||GetNetMode()==NM_DedicatedServer)return;
    ClearVisuals();FRandomStream Appearance(Identity.Seed);
    static const FLinearColor Uniforms[]={FLinearColor(.21f,.29f,.31f),FLinearColor(.44f,.28f,.14f),FLinearColor(.26f,.18f,.32f),FLinearColor(.66f,.73f,.69f),FLinearColor(.075f,.12f,.18f),FLinearColor(.30f,.39f,.19f)};
    static const FLinearColor Skin[]={FLinearColor(.50f,.30f,.19f),FLinearColor(.25f,.12f,.065f),FLinearColor(.69f,.44f,.30f),FLinearColor(.37f,.21f,.12f),FLinearColor(.77f,.56f,.42f)};
    const FLinearColor Cloth=Uniforms[Identity.Role]*Appearance.FRandRange(.82f,1.14f);
    Materials.Add(CitizenMaterial(this,Cloth));
    Materials.Add(CitizenMaterial(this,FLinearColor(.10f,.115f,.125f)));
    Materials.Add(CitizenMaterial(this,Skin[Appearance.RandRange(0,4)],true));
    Materials.Add(CitizenMaterial(this,FLinearColor(.035f,.027f,.022f)));
    Materials.Add(CitizenMaterial(this,Identity.Role==3?FLinearColor(.56f,.095f,.055f):FLinearColor(.67f,.61f,.40f)));
    Materials.Add(CitizenMaterial(this,FLinearColor(.075f,.16f,.19f)));
    auto Part=[this](USceneComponent* Parent,const TCHAR* Shape,FVector At,FVector Dimensions,int32 Material=0,bool bDetail=false,FRotator Rotation=FRotator::ZeroRotator)
    {
        auto Component=NewObject<UStaticMeshComponent>(this);Component->SetupAttachment(Parent);
        Component->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,*FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"),Shape,Shape)));
        Component->SetRelativeLocation(At);Component->SetRelativeScale3D(Dimensions/100.0);Component->SetRelativeRotation(Rotation);
        Component->SetMaterial(0,Materials[Material]);Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Component->SetGenerateOverlapEvents(false);Component->SetCastShadow(!bDetail);Component->SetCullDistance(bDetail?7000.f:48000.f);
        Component->RegisterComponent();Parts.Add(Component);if(bDetail)DetailParts.Add(Component);return Component;
    };
    VisualRoot->SetWorldScale3D(FVector(Identity.Scale));VisualRoot->SetWorldLocationAndRotation(GetActorLocation(),GetActorQuat());
    BodyRoot=CitizenJoint(this,VisualRoot,FVector::ZeroVector,Parts);
    // Human proportions: feet on the path, 88 cm hips, 145 cm shoulders,
    // approximately 178 cm total height. Uniform and skin remain non-emissive.
    Part(BodyRoot,TEXT("Sphere"),FVector(-1,0,118),FVector(29,43,58));
    Part(BodyRoot,TEXT("Sphere"),FVector(-1,0,94),FVector(27,32,28),1);
    Part(BodyRoot,TEXT("Cube"),FVector(0,0,102),FVector(27,35,5),3,true);
    Part(BodyRoot,TEXT("Cube"),FVector(14,0,102),FVector(2,6,4),4,true);
    Part(BodyRoot,TEXT("Cube"),FVector(13,-11,133),FVector(2,8,6),4,true);
    Part(BodyRoot,TEXT("Sphere"),FVector(0,0,148),FVector(13,14,17),2);
    HeadPivot=CitizenJoint(this,BodyRoot,FVector(0,0,164),Parts);
    Part(HeadPivot,TEXT("Sphere"),FVector::ZeroVector,FVector(22,20,27),2);
    Part(HeadPivot,TEXT("Sphere"),FVector(-3,0,9),FVector(21,21,13),3,true);
    Part(HeadPivot,TEXT("Sphere"),FVector(10,0,-1),FVector(6,4,8),2,true);
    for(int32 Side=-1;Side<=1;Side+=2)
    {
        Part(HeadPivot,TEXT("Sphere"),FVector(9,Side*5,3),FVector(3,4,2),3,true);
        Part(HeadPivot,TEXT("Sphere"),FVector(-1,Side*10,-1),FVector(5,4,8),2,true);
        auto Hip=CitizenJoint(this,BodyRoot,FVector(0,Side*10,89),Parts);Hips.Add(Hip);
        Part(Hip,TEXT("Sphere"),FVector(0,0,-22),FVector(18,18,46),1);
        auto Knee=CitizenJoint(this,Hip,FVector(0,0,-43),Parts);Knees.Add(Knee);
        Part(Knee,TEXT("Sphere"),FVector(0,0,-18),FVector(13,14,39),1);
        Part(Knee,TEXT("Sphere"),FVector(5,0,-39),FVector(29,15,13),3);
        auto Shoulder=CitizenJoint(this,BodyRoot,FVector(0,Side*22,139),Parts);Shoulders.Add(Shoulder);
        Part(Shoulder,TEXT("Sphere"),FVector(0,Side*2,-15),FVector(14,15,36));
        auto Elbow=CitizenJoint(this,Shoulder,FVector(0,Side*2,-31),Parts);Elbows.Add(Elbow);
        Part(Elbow,TEXT("Sphere"),FVector(1,0,-12),FVector(11,12,29));
        Part(Elbow,TEXT("Sphere"),FVector(2,0,-29),FVector(10,10,16),2,true);
    }
    if(Identity.Role==1||Identity.Role==5)
    {
        Part(BodyRoot,TEXT("Cube"),FVector(-20,0,126),FVector(14,28,39),1,true);
        Part(BodyRoot,TEXT("Cube"),FVector(13,13,122),FVector(3,4,39),1,true);
        if(Identity.Role==1)Part(HeadPivot,TEXT("Sphere"),FVector(-1,0,12),FVector(25,25,14),4,true);
    }
    if(Identity.Role==4)
    {
        Part(BodyRoot,TEXT("Cube"),FVector(11,0,127),FVector(9,32,35),1,true);
        Part(HeadPivot,TEXT("Sphere"),FVector(-2,0,10),FVector(25,25,20),1,true);
        Part(HeadPivot,TEXT("Sphere"),FVector(9,0,5),FVector(9,22,11),5,true);
    }
    bVisualsBuilt=true;bDetailsVisible=true;InteractionCapsule->SetRelativeScale3D(FVector(Identity.Scale));
}

void AVoyagerCitizen::Animate(float D)
{
    if(!bVisualsBuilt||!BodyRoot)return;
    const double Time=SynchronizedTime();const float Speed=float(Pose.Velocity.Size());
    const float Extrapolation=FMath::Clamp(float(Time)-Pose.ServerTime,0.f,.12f);
    const float Phase=(Pose.GaitDistance+Speed*Extrapolation)*(2.f*PI/145.f);
    const float Moving=FMath::Clamp(Speed/140.f,0.f,1.f);
    const float Stride=Pose.bAlarmed?34.f:24.f;
    const FVector RenderAt=Pose.Location+Pose.Velocity*Extrapolation;
    if(FVector::DistSquared(VisualRoot->GetComponentLocation(),RenderAt)>FMath::Square(2000.f))VisualRoot->SetWorldLocation(RenderAt);
    else VisualRoot->SetWorldLocation(FMath::VInterpTo(VisualRoot->GetComponentLocation(),RenderAt,D,15.f));
    VisualRoot->SetWorldRotation(FQuat::Slerp(VisualRoot->GetComponentQuat(),Pose.Rotation.Quaternion(),1.f-FMath::Exp(-D*13.f)).GetNormalized());
    BodyRoot->SetRelativeLocation(FVector(0,0,FMath::Abs(FMath::Sin(Phase))*Moving*2.1f+FMath::Sin(float(Time)*1.5f+Identity.Ordinal)*.35f));
    BodyRoot->SetRelativeRotation(FRotator(Pose.bAlarmed?-5.f:0.f,0,FMath::Sin(Phase)*Moving*1.2f));
    for(int32 I=0;I<2&&Hips.IsValidIndex(I);++I)
    {
        const float Wave=FMath::Sin(Phase+I*PI);
        Hips[I]->SetRelativeRotation(FRotator(Wave*Stride*Moving,0,0));
        Knees[I]->SetRelativeRotation(FRotator(FMath::Max(0.f,-Wave)*32.f*Moving,0,0));
        float Arm=-Wave*Stride*.7f*Moving;
        float ElbowAngle=Pose.bAlarmed?-45.f:-12.f;
        if(Pose.Activity==Talk&&I==1){Arm=19.f+FMath::Sin(float(Time)*2.2f)*7.f;ElbowAngle=-48.f;}
        Shoulders[I]->SetRelativeRotation(FRotator(Arm,0,I==0?-4.f:4.f));
        Elbows[I]->SetRelativeRotation(FRotator(ElbowAngle,0,0));
    }
    HeadPivot->SetRelativeRotation(FRotator(Pose.Activity==Talk?FMath::Sin(float(Time)*2.1f)*3.f:0.f,FMath::Sin(float(Time)*.53f+Identity.Ordinal)*6.f,0));
}

void AVoyagerCitizen::UpdateSignificance()
{
    NearestViewerDistance=TNumericLimits<float>::Max();
    for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        auto Controller=It->Get();if(!Controller||(!HasAuthority()&&!Controller->IsLocalController()))continue;
        APawn* Player=Controller->GetPawn();if(Player)NearestViewerDistance=FMath::Min(NearestViewerDistance,float(FVector::Dist(GetActorLocation(),Player->GetActorLocation())));
    }
    SetActorTickInterval(NearestViewerDistance<12000.f?0.f:.0667f);
    const bool bShowDetails=NearestViewerDistance<6500.f;
    if(bShowDetails!=bDetailsVisible)
    {
        bDetailsVisible=bShowDetails;for(auto Detail:DetailParts)if(IsValid(Detail))Detail->SetVisibility(bShowDetails);
    }
}

void AVoyagerCitizen::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);SignificanceRemaining-=DeltaSeconds;
    if(SignificanceRemaining<=0.f){SignificanceRemaining=.5f+float(Identity.Ordinal%5)*.03f;UpdateSignificance();}
    if(HasAuthority())
    {
        MovementAccumulator+=DeltaSeconds;const float Interval=NearestViewerDistance<18000.f?.05f:.10f;
        if(MovementAccumulator>=Interval){const float D=FMath::Min(MovementAccumulator,.2f);MovementAccumulator=0;Simulate(D);}
    }
    if(GetNetMode()!=NM_DedicatedServer)Animate(DeltaSeconds);
}

AVoyagerCitizenManager::AVoyagerCitizenManager()
{
    PrimaryActorTick.bCanEverTick=true;PrimaryActorTick.TickInterval=.5f;bReplicates=true;bAlwaysRelevant=true;
    SetReplicateMovement(false);SetNetUpdateFrequency(2.f);SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("CitizenManagerRoot")));
    CityPopulations.Init(0,Voyager::PlanetCount*AVoyagerSettlement::SitesPerPlanet);
}

void AVoyagerCitizenManager::BeginPlay() { Super::BeginPlay();if(!HasAuthority())SetActorTickEnabled(false); }
void AVoyagerCitizenManager::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(AVoyagerCitizenManager,Population);DOREPLIFETIME(AVoyagerCitizenManager,CityPopulations);
}

float AVoyagerCitizenManager::CityHour(const UWorld* World)
{
    if(!World)return 8.f;
    const auto State=World->GetGameState<AVoyagerState>();
    const double Time=State?State->GetServerWorldTimeSeconds():World->GetTimeSeconds();
    return float(FMath::Fmod(8.0+Time/60.0,24.0));
}

FString AVoyagerCitizenManager::ClockText(const UWorld* World)
{
    const float Hour=CityHour(World);const int32 Hours=FMath::FloorToInt(Hour);const int32 Minutes=FMath::FloorToInt((Hour-Hours)*60.f);
    return FString::Printf(TEXT("%02d:%02d"),Hours,Minutes);
}

int32 AVoyagerCitizenManager::CitizensInCity(int32 Planet,int32 Site) const
{
    const int32 Index=Planet*AVoyagerSettlement::SitesPerPlanet+Site;return CityPopulations.IsValidIndex(Index)?CityPopulations[Index]:0;
}

void AVoyagerCitizenManager::ClearPopulation()
{
    for(auto Citizen:Citizens)if(IsValid(Citizen))Citizen->Destroy();Citizens.Empty();UpdateCounts();
}

void AVoyagerCitizenManager::UpdateCounts()
{
    Citizens.RemoveAll([](const TObjectPtr<AVoyagerCitizen>& Citizen){return !IsValid(Citizen);});
    CityPopulations.Init(0,Voyager::PlanetCount*AVoyagerSettlement::SitesPerPlanet);
    for(auto Citizen:Citizens)
    {
        const int32 Index=Citizen->PlanetIndex()*AVoyagerSettlement::SitesPerPlanet+Citizen->SiteIndex();
        if(CityPopulations.IsValidIndex(Index))++CityPopulations[Index];
    }
    Population=Citizens.Num();ForceNetUpdate();
}

bool AVoyagerCitizenManager::SpawnCitizen(int32 Planet,int32 Site)
{
    if(Citizens.Num()>=MaximumPopulation)return false;
    TSet<int32> Ordinals;
    for(auto Citizen:Citizens)if(IsValid(Citizen)&&Citizen->PlanetIndex()==Planet&&Citizen->SiteIndex()==Site)Ordinals.Add(Citizen->OrdinalIndex());
    int32 Ordinal=0;while(Ordinals.Contains(Ordinal))++Ordinal;if(Ordinal>=CitizensPerCity)return false;
    FRandomStream Seed(int32(Voyager::Hash(uint32(Voyager::PlanetSeed(ActiveSystem,Planet))^uint32(Site*7717+Ordinal*104729+5371))&0x7fffffff));
    int32 StartNode=Seed.RandRange(1,7)+Seed.RandRange(1,7)*9;
    FVector Position=AVoyagerSettlement::StreetPoint(ActiveSystem,Planet,Site,StartNode%9,StartNode/9,20.f);
    // Start the first regulars at actual storefronts instead of hiding everyone
    // deep inside a residence as the city first streams in.
    if(Ordinal<6)
    {
        FVoyagerBuildingInfo Info;
        const int32 Building=8+Ordinal;
        if(AVoyagerSettlement::GetBuildingInfo(ActiveSystem,Planet,Site,Building,Info))
        {StartNode=BuildingNode(Building,0);Position=Info.DoorOutside;}
    }
    else if(Ordinal%6==0)
    {
        FVoyagerBuildingInfo Info;
        const int32 Building=(Ordinal*7)%AVoyagerSettlement::BuildingCount();
        if(AVoyagerSettlement::GetBuildingInfo(ActiveSystem,Planet,Site,Building,Info))
        {StartNode=BuildingNode(Building,3);Position=Info.InteriorPoint;}
    }
    const FVector Up=Voyager::SurfaceNormal(ActiveSystem,Planet,Position);
    const FTransform Transform(Voyager::TangentRotation(Up),Position);
    auto Citizen=GetWorld()->SpawnActorDeferred<AVoyagerCitizen>(AVoyagerCitizen::StaticClass(),Transform,this,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if(!Citizen)return false;
    Citizen->InitializeCitizen(ActiveSystem,Planet,Site,Ordinal,StartNode);Citizen->FinishSpawning(Transform);Citizens.Add(Citizen);return true;
}

void AVoyagerCitizenManager::ReportDisturbance(const FVector& Position)
{
    if(!HasAuthority())return;const float Time=GetWorld()->GetTimeSeconds();
    if(Time-LastReportTime<.8f&&FVector::DistSquared(Position,LastReportPosition)<FMath::Square(1000.f))return;
    LastReportTime=Time;LastReportPosition=Position;
    for(auto Citizen:Citizens)if(IsValid(Citizen))Citizen->NotifyDisturbance(Position);
}

void AVoyagerCitizenManager::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);if(!HasAuthority())return;
    const auto State=GetWorld()->GetGameState<AVoyagerState>();if(!State)return;
    if(ActiveSystem!=State->SystemSeed){ClearPopulation();ActiveSystem=State->SystemSeed;}
    if(State->bTransitioning)return;
    TSet<int32> WantedCities;TSet<int32> RetainedCities;
    for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        APawn* Player=It->Get()?It->Get()->GetPawn():nullptr;if(!Player)continue;
        const FVector Position=Player->GetActorLocation();const int32 Planet=Voyager::NearestPlanet(ActiveSystem,Position);
        const double Altitude=Voyager::SurfaceAltitude(ActiveSystem,Planet,Position);
        if(Altitude>40000.0||Altitude<-1000.0)continue;
        for(int32 Site=0;Site<AVoyagerSettlement::SitesPerPlanet;++Site)
        {
            const FVector Center=Voyager::SurfacePoint(ActiveSystem,Planet,AVoyagerSettlement::SiteDirection(ActiveSystem,Planet,Site));
            const double Distance=FVector::DistSquared(Position,Center);const int32 Key=Planet*AVoyagerSettlement::SitesPerPlanet+Site;
            if(Distance<FMath::Square(95000.f))RetainedCities.Add(Key);
            if(Altitude<30000.f&&Distance<FMath::Square(70000.f))WantedCities.Add(Key);
        }
    }
    const int32 PerCityQuota=WantedCities.IsEmpty()?CitizensPerCity:FMath::Clamp(MaximumPopulation/WantedCities.Num(),1,CitizensPerCity);
    for(int32 I=Citizens.Num()-1;I>=0;--I)
    {
        auto Citizen=Citizens[I];
        const int32 Key=IsValid(Citizen)?Citizen->PlanetIndex()*AVoyagerSettlement::SitesPerPlanet+Citizen->SiteIndex():INDEX_NONE;
        const bool bOverQuota=IsValid(Citizen)&&Citizen->OrdinalIndex()>=PerCityQuota;
        const bool bReservedForActiveCities=!WantedCities.IsEmpty()&&!WantedCities.Contains(Key);
        if(!IsValid(Citizen)||!RetainedCities.Contains(Key)||bOverQuota||bReservedForActiveCities)
        {if(IsValid(Citizen))Citizen->Destroy();Citizens.RemoveAtSwap(I);}
    }
    TArray<int32> Keys=WantedCities.Array();Keys.Sort();
    for(int32 Attempt=0;Attempt<3&&!Keys.IsEmpty()&&Citizens.Num()<MaximumPopulation;++Attempt)
    {
        const int32 Key=Keys[CityCursor++%Keys.Num()];int32 Existing=0;
        for(auto Citizen:Citizens)if(Citizen->PlanetIndex()*AVoyagerSettlement::SitesPerPlanet+Citizen->SiteIndex()==Key)++Existing;
        if(Existing<PerCityQuota)SpawnCitizen(Key/AVoyagerSettlement::SitesPerPlanet,Key%AVoyagerSettlement::SitesPerPlanet);
    }
    UpdateCounts();
}

void AVoyagerCitizenManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if(HasAuthority())ClearPopulation();Super::EndPlay(EndPlayReason);
}
