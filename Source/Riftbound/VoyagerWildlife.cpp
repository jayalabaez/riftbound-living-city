#include "VoyagerWildlife.h"
#include "VoyagerData.h"
#include "VoyagerGameMode.h"
#include "VoyagerSettlement.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"

namespace
{
    USceneComponent* Joint(AActor* Owner,USceneComponent* Parent,const FVector& Position,TArray<TObjectPtr<UActorComponent>>& Parts)
    {
        auto Component=NewObject<USceneComponent>(Owner);
        Component->SetupAttachment(Parent);Component->SetRelativeLocation(Position);Component->RegisterComponent();Parts.Add(Component);return Component;
    }

    UMaterialInstanceDynamic* AnimalMaterial(AActor* Owner,const FLinearColor& Color,bool bSkin,bool bGlow=false)
    {
        UMaterialInterface* Base=LoadObject<UMaterialInterface>(nullptr,bGlow?TEXT("/Game/Materials/M_Glow.M_Glow"):
            (bSkin?TEXT("/Game/Materials/M_VoyagerAnimal.M_VoyagerAnimal"):TEXT("/Game/Materials/M_Surface.M_Surface")));
        if(!Base)Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_Surface.M_Surface"));
        if(!Base)Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
        auto Material=UMaterialInstanceDynamic::Create(Base,Owner);
        if(Material)
        {
            Material->SetVectorParameterValue(TEXT("Tint"),Color);
            Material->SetVectorParameterValue(TEXT("Color"),Color);
            Material->SetVectorParameterValue(TEXT("StripeTint"),Color*.42f);
            Material->SetScalarParameterValue(TEXT("Roughness"),.78f);
            Material->SetScalarParameterValue(TEXT("Glow"),bGlow?1.2f:0.f);
        }
        return Material;
    }
}

AVoyagerAnimal::AVoyagerAnimal()
{
    PrimaryActorTick.bCanEverTick=true;bReplicates=true;bAlwaysRelevant=false;
    SetReplicateMovement(false);SetNetUpdateFrequency(12.f);SetMinNetUpdateFrequency(8.f);
    SetNetCullDistanceSquared(FMath::Square(85000.f));
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("AnimalRoot")));
    VisualRoot=CreateDefaultSubobject<USceneComponent>(TEXT("AnimalVisuals"));VisualRoot->SetupAttachment(GetRootComponent());
    // The render hierarchy interpolates in world space while the authoritative
    // actor follows its precise double-coordinate path at20Hz.
    VisualRoot->SetAbsolute(true,true,false);
    SetActorEnableCollision(false);Tags.Add(TEXT("VoyagerWildlife"));
}

void AVoyagerAnimal::InitializeAnimal(int32 System,int32 Planet,int32 Species,int32 Seed)
{
    if(!HasAuthority())return;
    Identity.System=System;Identity.Planet=FMath::Clamp(Planet,0,Voyager::PlanetCount-1);
    Identity.Species=FMath::Clamp(Species,0,4);Identity.Seed=Seed;
    FRandomStream Appearance(Seed);Identity.Scale=Appearance.FRandRange(.82f,1.15f);Identity.bInitialized=true;
    Random.Initialize(Seed);Home=GetActorLocation();Destination=Home;ThinkRemaining=Random.FRandRange(2.f,7.f);
    Pose.Location=Home;Pose.Rotation=GetActorRotation();Pose.Behavior=0;Pose.ServerTime=float(SynchronizedTime());
    if(HasActorBegunPlay())BuildVisuals();ForceNetUpdate();
}

void AVoyagerAnimal::BeginPlay()
{
    Super::BeginPlay();VisualRoot->SetWorldLocationAndRotation(GetActorLocation(),GetActorQuat());
    if(Identity.bInitialized)BuildVisuals();
}

void AVoyagerAnimal::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(AVoyagerAnimal,Identity);DOREPLIFETIME(AVoyagerAnimal,Pose);
}

void AVoyagerAnimal::OnRep_Identity()
{
    if(HasActorBegunPlay()&&Identity.bInitialized)BuildVisuals();
}

void AVoyagerAnimal::OnRep_Pose()
{
    if(!HasAuthority())
    {
        SetActorLocationAndRotation(Pose.Location,Pose.Rotation,false);
        if(!bVisualsBuilt)VisualRoot->SetWorldLocationAndRotation(Pose.Location,Pose.Rotation);
    }
}

FString AVoyagerAnimal::SpeciesName() const
{
    static const TCHAR* Names[]={TEXT("Crestback grazer"),TEXT("Dune sailrunner"),TEXT("Tundra tuskbeast"),TEXT("Ashwing strider"),TEXT("Lanternback browser")};
    return Names[FMath::Clamp(Identity.Species,0,4)];
}

FString AVoyagerAnimal::BehaviorName() const
{
    return Pose.Behavior==2?TEXT("Fleeing"):(Pose.Behavior==1?TEXT("Wandering"):TEXT("Grazing"));
}

double AVoyagerAnimal::SynchronizedTime() const
{
    if(const auto State=GetWorld()->GetGameState<AVoyagerState>())return State->GetServerWorldTimeSeconds();
    return GetWorld()->GetTimeSeconds();
}

void AVoyagerAnimal::ClearVisuals()
{
    for(int32 Index=VisualParts.Num()-1;Index>=0;--Index)if(IsValid(VisualParts[Index]))VisualParts[Index]->DestroyComponent();
    VisualParts.Empty();Materials.Empty();LegPivots.Empty();KneePivots.Empty();WingPivots.Empty();
    BodyRoot=nullptr;NeckPivot=nullptr;TailPivot=nullptr;bVisualsBuilt=false;
}

void AVoyagerAnimal::BuildVisuals()
{
    if(!Identity.bInitialized||GetNetMode()==NM_DedicatedServer)return;
    ClearVisuals();
    static const FLinearColor Hides[]={FLinearColor(.32f,.39f,.18f),FLinearColor(.62f,.38f,.14f),FLinearColor(.65f,.71f,.7f),FLinearColor(.20f,.115f,.075f),FLinearColor(.28f,.22f,.37f)};
    static const FLinearColor Accents[]={FLinearColor(.67f,.58f,.34f),FLinearColor(.30f,.18f,.09f),FLinearColor(.28f,.36f,.38f),FLinearColor(.48f,.25f,.13f),FLinearColor(.43f,.63f,.54f)};
    FRandomStream Appearance(Identity.Seed);
    const FLinearColor Hide=Hides[Identity.Species]*Appearance.FRandRange(.86f,1.12f);
    Materials.Add(AnimalMaterial(this,Hide,true));
    Materials.Add(AnimalMaterial(this,Accents[Identity.Species],false));
    Materials.Add(AnimalMaterial(this,FLinearColor(.045f,.038f,.027f),false));
    Materials.Add(AnimalMaterial(this,FLinearColor(.69f,.66f,.49f),false));
    Materials.Add(AnimalMaterial(this,FLinearColor(.18f,.58f,.43f),false,true));
    auto Part=[this](USceneComponent* Parent,const TCHAR* Shape,FVector Position,FVector Scale,int32 Material=0,FRotator Rotation=FRotator::ZeroRotator)
    {
        auto Component=NewObject<UStaticMeshComponent>(this);
        Component->SetupAttachment(Parent);
        Component->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,*FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"),Shape,Shape)));
        Component->SetRelativeLocation(Position);Component->SetRelativeScale3D(Scale);Component->SetRelativeRotation(Rotation);
        Component->SetMaterial(0,Materials[FMath::Clamp(Material,0,Materials.Num()-1)]);
        Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);Component->SetGenerateOverlapEvents(false);
        Component->SetCastShadow(Material!=4);Component->SetCullDistance(90000.f);Component->RegisterComponent();VisualParts.Add(Component);return Component;
    };
    VisualRoot->SetWorldScale3D(FVector(Identity.Scale));
    VisualRoot->SetWorldLocationAndRotation(GetActorLocation(),GetActorQuat());
    BodyRoot=Joint(this,VisualRoot,FVector::ZeroVector,VisualParts);
    const bool bBiped=Identity.Species==1||Identity.Species==3;
    const bool bHeavy=Identity.Species==2;
    const bool bSixLegs=Identity.Species==4;
    const float HipHeight=bBiped?132.f:(bHeavy?114.f:125.f);
    Part(BodyRoot,TEXT("Sphere"),FVector(-12,0,HipHeight+22),bHeavy?FVector(2.6f,1.5f,1.6f):FVector(2.25f,.95f,1.15f));
    Part(BodyRoot,TEXT("Sphere"),FVector(60,0,HipHeight+29),bHeavy?FVector(1.55f,1.48f,1.55f):FVector(1.15f,1.05f,1.23f));
    Part(BodyRoot,TEXT("Sphere"),FVector(-20,0,HipHeight+2),FVector(1.62f,.76f,.73f),1);
    if(bHeavy)Part(BodyRoot,TEXT("Sphere"),FVector(-45,0,198),FVector(1.6f,1.1f,.8f),1);
    if(bSixLegs)
    {
        Part(BodyRoot,TEXT("Sphere"),FVector(-64,0,175),FVector(1.3f,1.2f,.85f),1);
        Part(BodyRoot,TEXT("Sphere"),FVector(23,0,183),FVector(1.2f,1.12f,.8f),1);
        for(int32 Index=0;Index<4;++Index)Part(BodyRoot,TEXT("Sphere"),FVector(-76+Index*39,0,207),FVector(.14f,.19f,.13f),4);
    }
    NeckPivot=Joint(this,BodyRoot,FVector(bHeavy?88:70,0,bHeavy?140:HipHeight+18),VisualParts);
    const FVector HeadAt=bHeavy?FVector(32,0,18):(bBiped?FVector(38,0,112):FVector(35,0,75));
    if(bHeavy)Part(NeckPivot,TEXT("Sphere"),FVector(8,0,12),FVector(1.1f,1.2f,1.12f));
    else Part(NeckPivot,TEXT("Sphere"),FVector(18,0,bBiped?55:36),bBiped?FVector(.56f,.55f,1.62f):FVector(.64f,.63f,1.18f),0,FRotator(-17,0,0));
    auto Head=Joint(this,NeckPivot,HeadAt,VisualParts);
    Part(Head,TEXT("Sphere"),FVector::ZeroVector,bHeavy?FVector(1.36f,.96f,.99f):FVector(.97f,.64f,.67f));
    Part(Head,TEXT("Sphere"),FVector(bHeavy?44:42,0,-15),bBiped?FVector(.93f,.43f,.30f):FVector(.75f,.53f,.39f),1);
    Part(Head,TEXT("Sphere"),FVector(bHeavy?72:71,0,-13),FVector(.20f,.36f,.23f),2);
    for(int32 Side=-1;Side<=1;Side+=2)
    {
        Part(Head,TEXT("Sphere"),FVector(19,Side*(bHeavy?42:29),11),FVector(.13f,.075f,.14f),2);
        Part(Head,TEXT("Sphere"),FVector(22,Side*(bHeavy?45:31),15),FVector(.035f,.026f,.035f),3);
        if(!bSixLegs)Part(Head,TEXT("Sphere"),FVector(-17,Side*30,32),FVector(.21f,.13f,.65f),1,FRotator(-12,0,Side*42));
        if(Identity.Species==0)
        {
            Part(Head,TEXT("Cone"),FVector(-14,Side*22,59),FVector(.14f,.13f,.90f),3,FRotator(-17,0,Side*21));
            Part(Head,TEXT("Cone"),FVector(-1,Side*32,69),FVector(.10f,.09f,.52f),3,FRotator(-43,0,Side*39));
        }
        if(bHeavy)
        {
            Part(Head,TEXT("Sphere"),FVector(24,Side*40,-31),FVector(.19f,.22f,.82f),3,FRotator(-27,0,Side*12));
            Part(Head,TEXT("Cone"),FVector(47,Side*43,-42),FVector(.17f,.17f,.63f),3,FRotator(-82,0,Side*10));
        }
        if(bSixLegs)
        {
            Part(Head,TEXT("Cone"),FVector(10,Side*26,46),FVector(.09f,.09f,.69f),1,FRotator(-22,0,Side*23));
            Part(Head,TEXT("Sphere"),FVector(22,Side*38,74),FVector(.16f,.16f,.22f),4);
        }
    }
    const int32 PairCount=bSixLegs?3:(bBiped?1:2);
    for(int32 Pair=0;Pair<PairCount;++Pair)for(int32 Side=-1;Side<=1;Side+=2)
    {
        const float X=bBiped?-33.f:(bSixLegs?(-74.f+Pair*75.f):(-70.f+Pair*140.f));
        const float Width=bHeavy?53.f:(bSixLegs?48.f:36.f);
        auto Hip=Joint(this,BodyRoot,FVector(X,Side*Width,HipHeight),VisualParts);LegPivots.Add(Hip);
        const float UpperLength=HipHeight*.50f;
        Part(Hip,TEXT("Sphere"),FVector(0,0,-UpperLength*.45f),FVector(bHeavy?.40f:.26f,bHeavy?.44f:.29f,UpperLength/85.f));
        auto Knee=Joint(this,Hip,FVector(0,0,-UpperLength),VisualParts);KneePivots.Add(Knee);
        Part(Knee,TEXT("Sphere"),FVector(3,0,-UpperLength*.42f),FVector(bHeavy?.30f:.17f,bHeavy?.32f:.19f,UpperLength/90.f),bBiped?1:0);
        Part(Knee,TEXT("Sphere"),FVector(12,0,-UpperLength+9),bBiped?FVector(.68f,.27f,.18f):FVector(.47f,.31f,.20f),2);
    }
    if(Identity.Species==1)
    {
        for(int32 Side=-1;Side<=1;Side+=2)Part(BodyRoot,TEXT("Sphere"),FVector(67,Side*43,139),FVector(.40f,.15f,.65f),1,FRotator(-48,0,Side*16));
        for(int32 Index=0;Index<4;++Index)
            Part(BodyRoot,TEXT("Sphere"),FVector(-77+Index*40,0,215+FMath::Sin(Index*.8f)*17),FVector(.67f,.10f,.77f),1,FRotator(-15,0,0));
    }
    if(Identity.Species==3)
    {
        for(int32 Side=-1;Side<=1;Side+=2)
        {
            auto Wing=Joint(this,BodyRoot,FVector(12,Side*46,171),VisualParts);WingPivots.Add(Wing);
            Part(Wing,TEXT("Sphere"),FVector(-8,Side*65,0),FVector(1.45f,1.5f,.15f),1);
            for(int32 Feather=0;Feather<3;++Feather)
                Part(Wing,TEXT("Sphere"),FVector(-38+Feather*31,Side*(124+Feather*6),-4),FVector(.42f,1.10f,.10f),Feather%2?0:1,FRotator(0,Side*(12+Feather*8),0));
        }
        Part(Head,TEXT("Sphere"),FVector(-15,0,41),FVector(.77f,.13f,.41f),1,FRotator(-15,0,0));
    }
    TailPivot=Joint(this,BodyRoot,FVector(-111,0,HipHeight+27),VisualParts);
    Part(TailPivot,TEXT("Sphere"),FVector(bBiped?-62:-35,0,0),bBiped?FVector(1.45f,.28f,.28f):FVector(.83f,.22f,.25f));
    Part(TailPivot,TEXT("Sphere"),FVector(bBiped?-119:-72,0,-5),bBiped?FVector(.96f,.17f,.18f):FVector(.36f,.28f,.33f),1,FRotator(-12,0,0));
    bVisualsBuilt=true;
}

void AVoyagerAnimal::SimulateBehavior(float D)
{
    if(!Identity.bInitialized)return;
    const auto State=GetWorld()->GetGameState<AVoyagerState>();if(!State||State->SystemSeed!=Identity.System||State->bTransitioning)return;
    const FVector Position=GetActorLocation();const FVector Up=Voyager::SurfaceNormal(Identity.System,Identity.Planet,Position);
    APawn* Threat=nullptr;double ClosestThreat=Pose.Behavior==2?FMath::Square(1750.0):FMath::Square(800.0);
    for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        const auto Controller=It->Get();APawn* Pawn=Controller?Controller->GetPawn():nullptr;if(!Pawn)continue;
        const double Distance=FVector::DistSquared(Position,Pawn->GetActorLocation());
        if(Distance<ClosestThreat){ClosestThreat=Distance;Threat=Pawn;}
    }
    FVector Direction=FVector::ZeroVector;float DesiredSpeed=0.f;
    ThinkRemaining-=D;
    if(Threat)
    {
        Pose.Behavior=2;ThinkRemaining=Random.FRandRange(2.f,4.f);
        Direction=FVector::VectorPlaneProject(Position-Threat->GetActorLocation(),Up).GetSafeNormal();
        if(Direction.IsNearlyZero())Direction=GetActorForwardVector();
        DesiredSpeed=Identity.Species==2?570.f:(Identity.Species==4?650.f:850.f);
        Destination=Position+Direction*2500;
    }
    else
    {
        if(Pose.Behavior==2){Pose.Behavior=1;ThinkRemaining=3.f;}
        if(ThinkRemaining<=0)
        {
            Pose.Behavior=Random.FRand()<.53f?0:1;ThinkRemaining=Random.FRandRange(4.f,12.f);
            const FVector Forward=Voyager::TangentRotation(Up).Vector();
            const FVector Right=FVector::CrossProduct(Up,Forward);
            const float Angle=Random.FRandRange(0.f,2.f*PI);
            Destination=Position+(Forward*FMath::Cos(Angle)+Right*FMath::Sin(Angle))*Random.FRandRange(500.f,2300.f);
            if(FVector::DistSquared(Position,Home)>FMath::Square(6000.f)){Destination=Home;Pose.Behavior=1;}
        }
        if(Pose.Behavior==1)
        {
            Direction=FVector::VectorPlaneProject(Destination-Position,Up).GetSafeNormal();
            DesiredSpeed=Identity.Species==2?95.f:(Identity.Species==1?180.f:135.f);
            if(FVector::VectorPlaneProject(Destination-Position,Up).SizeSquared()<FMath::Square(100.f))
            {Pose.Behavior=0;ThinkRemaining=Random.FRandRange(3.f,8.f);DesiredSpeed=0.f;}
        }
    }
    CurrentSpeed=FMath::FInterpTo(CurrentSpeed,DesiredSpeed,D,Pose.Behavior==2?4.f:2.f);
    if(Direction.IsNearlyZero())Direction=GetActorForwardVector();
    const FQuat Facing=Voyager::TangentRotation(Up,Direction).Quaternion();
    FQuat Rotation=FQuat::Slerp(GetActorQuat(),Facing,1.f-FMath::Exp(-D*(Pose.Behavior==2?4.f:1.6f))).GetNormalized();
    FVector Candidate=Position+Rotation.GetForwardVector()*CurrentSpeed*D;
    if(AVoyagerSettlement::IsWithinSite(Identity.System,Identity.Planet,Voyager::SurfaceNormal(Identity.System,Identity.Planet,Candidate),350.0))
    {
        // Turn along the settlement boundary before a creature's body can enter
        // a plot. This also provides an escape route when a player startles it.
        bool bClearPath=false;
        static const float Turns[]={75.f,-75.f,130.f,-130.f,180.f};
        for(float Turn:Turns)
        {
            const FVector Avoid=FQuat(Up,FMath::DegreesToRadians(Turn)).RotateVector(Rotation.GetForwardVector());
            const FVector LookAhead=Position+Avoid*FMath::Max(CurrentSpeed*D,250.f);
            if(AVoyagerSettlement::IsWithinSite(Identity.System,Identity.Planet,Voyager::SurfaceNormal(Identity.System,Identity.Planet,LookAhead),350.0))continue;
            Rotation=Voyager::TangentRotation(Up,Avoid).Quaternion();
            Candidate=Position+Avoid*CurrentSpeed*D;Destination=Position+Avoid*1400.f;ThinkRemaining=4.f;bClearPath=true;break;
        }
        if(!bClearPath){Candidate=Position;CurrentSpeed=0;ThinkRemaining=0;}
    }
    const FVector Ground=Voyager::SurfacePoint(Identity.System,Identity.Planet,Voyager::SurfaceNormal(Identity.System,Identity.Planet,Candidate),3.f);
    const double Rise=FMath::Abs(FVector::DotProduct(Ground-Position,Up));
    FVector Next=Ground;
    if(CurrentSpeed>1.f&&Rise>FMath::Max(30.0,double(CurrentSpeed)*D*.9))
    {
        Next=Position;CurrentSpeed=0;ThinkRemaining=0; //Choose another path at cliffs.
    }
    SetActorLocationAndRotation(Next,Rotation,false);
    Pose.Location=Next;Pose.Rotation=Rotation.Rotator();Pose.Velocity=(Next-Position)/FMath::Max(D,.001f);
    Pose.GaitDistance=FMath::Fmod(Pose.GaitDistance+float(FVector::VectorPlaneProject(Next-Position,Up).Size()),26000.f);
    Pose.ServerTime=float(SynchronizedTime());
}

void AVoyagerAnimal::Animate(float D)
{
    if(!bVisualsBuilt||!BodyRoot)return;
    const double Time=SynchronizedTime();const float PhaseOffset=float(uint32(Identity.Seed)%1000)*.013f;
    const float Speed=float(Pose.Velocity.Size());
    const float Extrapolation=FMath::Clamp(float(Time)-Pose.ServerTime,0.f,.15f);
    const float Phase=(Pose.GaitDistance+Speed*Extrapolation)*(2.f*PI/260.f)+PhaseOffset;
    const float Moving=FMath::Clamp(Speed/150.f,0.f,1.f);
    const float Amplitude=Pose.Behavior==2?31.f:20.f;
    FVector RenderLocation=Pose.Location+Pose.Velocity*Extrapolation;
    if(FVector::DistSquared(VisualRoot->GetComponentLocation(),RenderLocation)>FMath::Square(5000.f))VisualRoot->SetWorldLocation(RenderLocation);
    else VisualRoot->SetWorldLocation(FMath::VInterpTo(VisualRoot->GetComponentLocation(),RenderLocation,D,12.f));
    VisualRoot->SetWorldRotation(FQuat::Slerp(VisualRoot->GetComponentQuat(),Pose.Rotation.Quaternion(),1.f-FMath::Exp(-D*12.f)).GetNormalized());
    const float Breath=FMath::Sin(float(Time)*1.2f+PhaseOffset);
    BodyRoot->SetRelativeLocation(FVector(0,0,Breath*.8f+FMath::Abs(FMath::Sin(Phase))*Moving*4.2f));
    BodyRoot->SetRelativeRotation(FRotator(FMath::Sin(Phase)*Moving*1.6f,0,FMath::Cos(Phase*.5f)*Moving*.8f));
    for(int32 Index=0;Index<LegPivots.Num();++Index)
    {
        const int32 Pair=Index/2;const bool Opposite=((Index%2)+(Pair%2))%2!=0;
        const float Wave=FMath::Sin(Phase+(Opposite?PI:0.f));
        LegPivots[Index]->SetRelativeRotation(FRotator(Wave*Amplitude*Moving,0,0));
        if(KneePivots.IsValidIndex(Index))KneePivots[Index]->SetRelativeRotation(FRotator(FMath::Max(0.f,-Wave)*33.f*Moving,0,0));
    }
    const float Browse=FMath::Clamp((FMath::Sin(float(Time)*.40f+PhaseOffset)+.4f)*1.2f,0.f,1.f);
    const float NeckTarget=Pose.Behavior==0?-92.f*Browse:(Pose.Behavior==2?14.f:0.f);
    AnimatedNeck=FMath::FInterpTo(AnimatedNeck,Identity.Species==2?NeckTarget*.34f:NeckTarget,D,2.4f);
    if(NeckPivot)NeckPivot->SetRelativeRotation(FRotator(AnimatedNeck+FMath::Sin(Phase)*Moving*3.f,FMath::Sin(float(Time)*.7f+PhaseOffset)*5.f,0));
    if(TailPivot)TailPivot->SetRelativeRotation(FRotator(FMath::Sin(float(Time)*1.7f+PhaseOffset)*8.f,FMath::Sin(float(Time)*(Pose.Behavior==2?6.f:2.5f)+PhaseOffset)*13.f,0));
    for(int32 Index=0;Index<WingPivots.Num();++Index)
    {
        const float Side=Index==0?-1.f:1.f;
        const float Flap=Pose.Behavior==2?23.f+FMath::Sin(float(Time)*9.f+PhaseOffset)*27.f:64.f+Breath*3.f;
        WingPivots[Index]->SetRelativeRotation(FRotator(0,0,Side*Flap));
    }
}

void AVoyagerAnimal::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if(HasAuthority())
    {
        MovementAccumulator+=DeltaSeconds;
        if(MovementAccumulator>=.05f){const float D=FMath::Min(MovementAccumulator,.15f);MovementAccumulator=0;SimulateBehavior(D);}
    }
    if(GetNetMode()!=NM_DedicatedServer)Animate(DeltaSeconds);
}

AVoyagerWildlifeManager::AVoyagerWildlifeManager()
{
    PrimaryActorTick.bCanEverTick=true;PrimaryActorTick.TickInterval=.5f;
    bReplicates=true;bAlwaysRelevant=true;SetReplicateMovement(false);SetNetUpdateFrequency(2.f);
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("WildlifeManagerRoot")));
    PlanetPopulations.Init(0,Voyager::PlanetCount);
}

void AVoyagerWildlifeManager::BeginPlay()
{
    Super::BeginPlay();if(!HasAuthority())SetActorTickEnabled(false);
}

void AVoyagerWildlifeManager::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(AVoyagerWildlifeManager,Population);
    DOREPLIFETIME(AVoyagerWildlifeManager,PlanetPopulations);DOREPLIFETIME(AVoyagerWildlifeManager,SpeciesPresent);
}

int32 AVoyagerWildlifeManager::PopulationOnPlanet(int32 Planet) const
{
    return PlanetPopulations.IsValidIndex(Planet)?PlanetPopulations[Planet]:0;
}

void AVoyagerWildlifeManager::ClearPopulation()
{
    for(auto Animal:Animals)if(IsValid(Animal))Animal->Destroy();Animals.Empty();SpawnSequence=0;UpdateCounts();
}

void AVoyagerWildlifeManager::UpdateCounts()
{
    Animals.RemoveAll([](const TObjectPtr<AVoyagerAnimal>& Animal){return !IsValid(Animal);});
    PlanetPopulations.Init(0,Voyager::PlanetCount);TSet<int32> Species;
    for(auto Animal:Animals){if(PlanetPopulations.IsValidIndex(Animal->PlanetIndex()))++PlanetPopulations[Animal->PlanetIndex()];Species.Add(Animal->SpeciesIndex());}
    Population=Animals.Num();SpeciesPresent=Species.Num();ForceNetUpdate();
}

bool AVoyagerWildlifeManager::SpawnNear(APawn* Explorer,int32 Planet,int32 ExistingNearby)
{
    if(!Explorer||Animals.Num()>=MaximumPopulation)return false;
    const FVector Position=Explorer->GetActorLocation();
    const FVector Up=Voyager::SurfaceNormal(ActiveSystem,Planet,Position);
    const FVector Forward=Voyager::TangentRotation(Up,Explorer->GetActorForwardVector()).Vector();
    const FVector Right=FVector::CrossProduct(Up,Forward);
    const FVector Unit=Voyager::SurfaceNormal(ActiveSystem,Planet,Position);
    const uint32 CellHash=Voyager::Hash(uint32(FMath::FloorToInt(Unit.X*50000.0)))^Voyager::Hash(uint32(FMath::FloorToInt(Unit.Y*50000.0)))^Voyager::Hash(uint32(FMath::FloorToInt(Unit.Z*50000.0)));
    const uint32 Seed=Voyager::Hash(uint32(Voyager::PlanetSeed(ActiveSystem,Planet))^CellHash^(++SpawnSequence*7919u));
    FRandomStream Stream(int32(Seed&0x7fffffff));
    const float Angle=ExistingNearby<3?Stream.FRandRange(-.9f,.9f):Stream.FRandRange(-PI,PI);
    const float Distance=ExistingNearby<3?Stream.FRandRange(2300.f,4900.f):Stream.FRandRange(5000.f,17000.f);
    const FVector Candidate=Position+(Forward*FMath::Cos(Angle)+Right*FMath::Sin(Angle))*Distance;
    const FVector Direction=Voyager::SurfaceNormal(ActiveSystem,Planet,Candidate);
    if(AVoyagerSettlement::IsWithinSite(ActiveSystem,Planet,Direction,450.0))return false;
    const FVector Ground=Voyager::SurfacePoint(ActiveSystem,Planet,Direction,3.f);
    for(auto Animal:Animals)if(IsValid(Animal)&&FVector::DistSquared(Animal->GetActorLocation(),Ground)<FMath::Square(650.f))return false;
    // Reject steep spawn sites using the same radial terrain field as walking.
    const FVector Sample=Voyager::SurfacePoint(ActiveSystem,Planet,Voyager::SurfaceNormal(ActiveSystem,Planet,Ground+Forward*250.f),3.f);
    if(FMath::Abs(FVector::DotProduct(Sample-Ground,Up))>175.f)return false;
    const FVector Heading=FQuat(Up,Stream.FRandRange(-PI,PI)).RotateVector(Forward);
    const FTransform SpawnTransform(Voyager::TangentRotation(Up,Heading),Ground);
    auto Animal=GetWorld()->SpawnActorDeferred<AVoyagerAnimal>(AVoyagerAnimal::StaticClass(),SpawnTransform,nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if(!Animal)return false;
    Animal->InitializeAnimal(ActiveSystem,Planet,Voyager::Biome(ActiveSystem,Planet),int32(Seed&0x7fffffff));
    Animal->FinishSpawning(SpawnTransform);Animals.Add(Animal);return true;
}

void AVoyagerWildlifeManager::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);if(!HasAuthority())return;
    const auto State=GetWorld()->GetGameState<AVoyagerState>();if(!State)return;
    if(ActiveSystem!=State->SystemSeed){ClearPopulation();ActiveSystem=State->SystemSeed;}
    if(State->bTransitioning)return;
    TArray<APawn*> Explorers;TArray<int32> Planets;
    for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        const auto Controller=It->Get();APawn* Pawn=Controller?Controller->GetPawn():nullptr;if(!Pawn)continue;
        const int32 Planet=Voyager::NearestPlanet(ActiveSystem,Pawn->GetActorLocation());
        const double Altitude=Voyager::SurfaceAltitude(ActiveSystem,Planet,Pawn->GetActorLocation());
        if(Altitude<25000.0&&Altitude>-1000.0){Explorers.Add(Pawn);Planets.Add(Planet);}
    }
    for(int32 Index=Animals.Num()-1;Index>=0;--Index)
    {
        auto Animal=Animals[Index];bool bNearby=false;
        if(IsValid(Animal))for(APawn* Explorer:Explorers)
            if(FVector::DistSquared(Animal->GetActorLocation(),Explorer->GetActorLocation())<FMath::Square(65000.f)){bNearby=true;break;}
        if(!bNearby){if(IsValid(Animal))Animal->Destroy();Animals.RemoveAtSwap(Index);}
    }
    if(!Explorers.IsEmpty())
    {
        const int32 DesiredPerExplorer=FMath::Clamp(MaximumPopulation/Explorers.Num(),4,12);
        int32 Spawned=0;
        for(int32 Try=0;Try<Explorers.Num()*2&&Spawned<2&&Animals.Num()<MaximumPopulation;++Try)
        {
            const int32 View=ViewCursor++%Explorers.Num();int32 Nearby=0;
            const FVector Up=Voyager::SurfaceNormal(ActiveSystem,Planets[View],Explorers[View]->GetActorLocation());
            for(auto Animal:Animals)if(IsValid(Animal)&&Animal->PlanetIndex()==Planets[View]&&FVector::VectorPlaneProject(Animal->GetActorLocation()-Explorers[View]->GetActorLocation(),Up).SizeSquared()<FMath::Square(24000.f))++Nearby;
            if(Nearby<DesiredPerExplorer&&SpawnNear(Explorers[View],Planets[View],Nearby))++Spawned;
        }
    }
    UpdateCounts();
}

void AVoyagerWildlifeManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if(HasAuthority())ClearPopulation();Super::EndPlay(EndPlayReason);
}
