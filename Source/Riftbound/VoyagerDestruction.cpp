#include "VoyagerDestruction.h"
#include "VoyagerGameMode.h"
#include "VoyagerSettlement.h"
#include "VoyagerData.h"
#include "VoyagerLaw.h"
#include "VoyagerCitizen.h"
#include "VoyagerWildlife.h"
#include "VoyagerCharacter.h"
#include "VoyagerShip.h"
#include "VoyagerPolice.h"
#include "Sound/SoundBase.h"
#include "RiftVisual.h"
#include "RiftCharacter.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"

namespace
{
    constexpr int32 MaxSavedDamageRecords=65536;
    constexpr uint32 ValidGlassFloorBits=(1u<<18)-1u;
    bool ValidDamage(const FVoyagerBuildingDamage& Entry)
    {
        return Entry.System>0&&Entry.Planet>=0&&Entry.Planet<Voyager::PlanetCount&&
            Entry.Site>=0&&Entry.Site<AVoyagerSettlement::SitesPerPlanet&&Entry.Building>=0&&Entry.Building<AVoyagerSettlement::BuildingCount()&&
            FMath::IsFinite(Entry.Integrity)&&Entry.Integrity>=0.f&&Entry.Integrity<=1200.f&&
            (Entry.BrokenGlassFloors&~ValidGlassFloorBits)==0;
    }
    uint64 DamageKey(const FVoyagerBuildingDamage& Entry)
    {
        return (uint64(uint32(Entry.System))<<10)|uint64(Entry.Planet*180+Entry.Site*60+Entry.Building);
    }
}

AVoyagerDebris::AVoyagerDebris()
{
    bReplicates=true;SetReplicateMovement(false);SetNetUpdateFrequency(12);PrimaryActorTick.bCanEverTick=true;
    Body=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ChaosFragment"));SetRootComponent(Body);
    Body->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
    Body->SetMobility(EComponentMobility::Movable);Body->SetCollisionProfileName(TEXT("BlockAllDynamic"));
    Body->SetCollisionResponseToChannel(ECC_Pawn,ECR_Ignore);Body->SetEnableGravity(false);
    Body->SetLinearDamping(.5f);Body->SetAngularDamping(.8f);Body->BodyInstance.bUseCCD=true;
    SetNetCullDistanceSquared(FMath::Square(90000.f));
}
void AVoyagerDebris::BeginPlay()
{
    Super::BeginPlay();OnRep_Appearance();
    // Clients interpolate authoritative rubble poses; running local collision or
    // local Chaos would create a different obstacle course for each player.
    if(!HasAuthority())Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}
void AVoyagerDebris::OnRep_Position()
{
    if(!bReceivedPosition&&!Position.ContainsNaN())SetActorLocationAndRotation(Position,Rotation);
    bReceivedPosition=true;
}
void AVoyagerDebris::OnRep_Appearance()
{
    Body->SetWorldScale3D(Scale);
    Body->SetMaterial(0,RiftVisual::Material(this,Tint));
}
void AVoyagerDebris::InitializePiece(const FTransform& Transform,FLinearColor Color,FVector Impulse,int32 S,int32 P)
{
    if(!HasAuthority()||Transform.ContainsNaN()||Impulse.ContainsNaN()||S<1||P<0||P>=Voyager::PlanetCount){Destroy();return;}
    System=S;Planet=P;Tint=Color;Scale=Transform.GetScale3D().GetAbs().BoundToBox(FVector(.02),FVector(8));SetActorTransform(Transform);Position=GetActorLocation();Rotation=GetActorQuat();
    OnRep_Appearance();Body->SetSimulatePhysics(true);Body->SetMassOverrideInKg(NAME_None,FMath::Clamp(float(Scale.X*Scale.Y*Scale.Z)*120.f,4.f,240.f),true);
    Body->AddImpulse(Impulse,NAME_None,true);Body->SetPhysicsAngularVelocityInDegrees(FVector(35,65,20));SetLifeSpan(45.f);ForceNetUpdate();
}
void AVoyagerDebris::Tick(float D)
{
    Super::Tick(D);Age+=D;
    if(HasAuthority())
    {
        if(auto State=GetWorld()->GetGameState<AVoyagerState>();!State||State->SystemSeed!=System){Destroy();return;}
        if(Body->IsSimulatingPhysics())
        {
            Body->AddForce(-Voyager::SurfaceNormal(System,Planet,GetActorLocation())*980.f,NAME_None,true);
            if(Body->GetPhysicsLinearVelocity().SizeSquared()>FMath::Square(5000.))Body->SetPhysicsLinearVelocity(Body->GetPhysicsLinearVelocity().GetClampedToMaxSize(5000));
            if(Voyager::SurfaceAltitude(System,Planet,GetActorLocation())<-250){Destroy();return;}
            if(Age>10&&Body->GetPhysicsLinearVelocity().SizeSquared()<FMath::Square(15.0)&&Body->GetPhysicsAngularVelocityInDegrees().SizeSquared()<FMath::Square(10.0))
                Body->SetSimulatePhysics(false);
        }
        Position=GetActorLocation();Rotation=GetActorQuat();
    }
    else
    {if(!bReceivedPosition||Position.ContainsNaN())return;SetActorLocation(FVector::DistSquared(GetActorLocation(),Position)>1.e10?Position:FMath::VInterpTo(GetActorLocation(),Position,D,18));SetActorRotation(FQuat::Slerp(GetActorQuat(),Rotation,FMath::Clamp(D*18.f,0.f,1.f)));}
}
void AVoyagerDebris::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(AVoyagerDebris,Position);DOREPLIFETIME(AVoyagerDebris,Rotation);DOREPLIFETIME(AVoyagerDebris,Scale);DOREPLIFETIME(AVoyagerDebris,Tint);}

AVoyagerBlastCloud::AVoyagerBlastCloud()
{
    PrimaryActorTick.bCanEverTick=true;bReplicates=false;
    Puffs=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("DustPuffs"));SetRootComponent(Puffs);
    Puffs->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Sphere.Sphere")));
    Puffs->SetMobility(EComponentMobility::Movable);Puffs->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Puffs->SetCastShadow(false);Puffs->SetCanEverAffectNavigation(false);
}
void AVoyagerBlastCloud::BeginPlay()
{
    Super::BeginPlay();SetLifeSpan(2.8f);
    if(auto* Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Cosmos/M_CosmosDust.M_CosmosDust")))
    {
        DustMaterial=UMaterialInstanceDynamic::Create(Base,this);Puffs->SetMaterial(0,DustMaterial);
        DustMaterial->SetScalarParameterValue(TEXT("DustOpacity"),.30f);
    }
    else {UE_LOG(LogTemp,Warning,TEXT("VOYAGER DESTRUCTION MISSING DUST MATERIAL"));Puffs->SetVisibility(false);}
    FRandomStream Random(3817);
    for(int32 Index=0;Index<12;++Index)
    {
        FVector Direction=Random.VRand();Direction.Z=FMath::Abs(Direction.Z)*.8+.15;Directions.Add(Direction);
        Puffs->AddInstance(FTransform(FQuat::Identity,FVector::ZeroVector,FVector(.1)));
    }
}
void AVoyagerBlastCloud::Tick(float D)
{
    Super::Tick(D);Age+=D;const float Spread=1.f-FMath::Exp(-Age*2.1f);
    const float Fade=1.f-FMath::SmoothStep(.9f,2.8f,Age);
    if(DustMaterial)DustMaterial->SetScalarParameterValue(TEXT("DustOpacity"),.30f*Fade);
    for(int32 Index=0;Index<Directions.Num();++Index)
    {
        const FVector Position=Directions[Index]*Radius*.40f*Spread+FVector(0,0,Age*75);
        const float Size=(.25f+Spread*Radius/280.f)*(.72f+.09f*(Index%4));
        Puffs->UpdateInstanceTransform(Index,FTransform(FQuat::Identity,Position,FVector(Size)),false,Index==Directions.Num()-1,true);
    }
}

AVoyagerDemolitionCharge::AVoyagerDemolitionCharge()
{bReplicates=true;SetReplicateMovement(true);PrimaryActorTick.bCanEverTick=true;Body=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Charge"));SetRootComponent(Body);Body->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);Body->SetRelativeScale3D(FVector(.28,.14,.32));}
void AVoyagerDemolitionCharge::BeginPlay(){Super::BeginPlay();Body->SetMaterial(0,RiftVisual::Material(this,FLinearColor(.07f,.085f,.075f)));RiftVisual::Mesh(this,Body,TEXT("ArmedIndicator"),TEXT("Sphere"),FVector(0,-55,0),FVector(.18),FLinearColor(1.f,.04f,.01f),true);}
void AVoyagerDemolitionCharge::Arm(AController* Controller){InstigatorController=Controller;}
void AVoyagerDemolitionCharge::Tick(float D){Super::Tick(D);if(HasAuthority()&&(Fuse-=D)<=0){if(auto System=AVoyagerDestruction::Find(GetWorld()))System->Explode(GetActorLocation(),InstigatorController.Get());Destroy();}}

AVoyagerDestruction::AVoyagerDestruction(){bReplicates=true;bAlwaysRelevant=true;SetNetUpdateFrequency(6);PrimaryActorTick.bCanEverTick=true;PrimaryActorTick.TickInterval=.2f;}
AVoyagerDestruction* AVoyagerDestruction::Find(const UWorld* World){if(World)for(TActorIterator<AVoyagerDestruction> It(World);It;++It)return *It;return nullptr;}
void AVoyagerDestruction::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const{Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(AVoyagerDestruction,Current);DOREPLIFETIME(AVoyagerDestruction,DamageRevision);}
void AVoyagerDestruction::Tick(float D)
{
    Super::Tick(D);if(!HasAuthority())return;auto State=GetWorld()->GetGameState<AVoyagerState>();if(!State)return;
    if(ActiveSystem!=State->SystemSeed)
    {
        Archived.RemoveAll([this](const auto& Entry){return Entry.System==ActiveSystem;});Archived.Append(Current);Current.Reset();ActiveSystem=State->SystemSeed;
        for(const auto& Entry:Archived)if(Entry.System==ActiveSystem)Current.Add(Entry);
        Archived.RemoveAll([this](const auto& Entry){return Entry.System==ActiveSystem;});
        ++DamageRevision;ForceNetUpdate();
    }
    Debris.RemoveAll([](const auto& Piece){return !IsValid(Piece);});
}
const FVoyagerBuildingDamage* AVoyagerDestruction::FindDamage(int32 S,int32 P,int32 C,int32 B) const
{for(const auto& Entry:Current)if(Entry.Matches(S,P,C,B))return &Entry;return nullptr;}
int32 AVoyagerDestruction::SurvivingFloors(const FVoyagerBuildingDamage* Damage,int32 Floors)
{if(!Damage)return Floors;if(Damage->Integrity<=0)return 0;if(Damage->Integrity<=300)return FMath::Max(1,Floors/3);if(Damage->Integrity<=650)return FMath::Max(1,Floors*2/3);return Floors;}
int32 AVoyagerDestruction::DebrisCount() const {int32 Count=0;for(const auto& Piece:Debris)if(IsValid(Piece))++Count;return Count;}
bool AVoyagerDestruction::DamageHit(const FHitResult& Hit,float Amount,AController* DamageInstigator)
{
    auto City=Cast<AVoyagerSettlement>(Hit.GetActor());if(!City)return false;
    int32 System=0,Planet=0,Site=0,Building=0;
    if(!City->ResolveBuildingHit(Hit,System,Planet,Site,Building))return false;
    return DamageBuilding(System,Planet,Site,Building,Amount,Hit.ImpactPoint,DamageInstigator,Hit.GetComponent()&&Hit.GetComponent()->ComponentHasTag(TEXT("VoyagerGlass")));
}
bool AVoyagerDestruction::DamageBuilding(int32 S,int32 P,int32 C,int32 B,float Amount,FVector Impact,AController* DamageInstigator,bool bGlass)
{
    if(!HasAuthority()||!FMath::IsFinite(Amount)||Amount<=0||S!=ActiveSystem||S<1||P<0||P>=Voyager::PlanetCount||
        C<0||C>=AVoyagerSettlement::SitesPerPlanet||B<0||B>=AVoyagerSettlement::BuildingCount()||Impact.ContainsNaN())return false;
    FVoyagerBuildingInfo Info;if(!AVoyagerSettlement::GetBuildingInfo(S,P,C,B,Info))return false;
    auto Entry=Current.FindByPredicate([&](const auto& Item){return Item.Matches(S,P,C,B);});
    if(!Entry)
    {
        if(Archived.Num()+Current.Num()>=MaxSavedDamageRecords)return false;
        FVoyagerBuildingDamage New;New.System=S;New.Planet=P;New.Site=C;New.Building=B;Entry=&Current.Add_GetRef(New);
    }
    const int32 Before=SurvivingFloors(Entry,Info.FloorCount);if(Before==0)return false;
    const int32 Floor=FMath::Clamp(FMath::FloorToInt(FVector::DotProduct(Impact-Info.Floor,Info.Up)/Info.FloorHeight),0,Info.FloorCount-1);
    if(bGlass){const uint32 Flag=1u<<Floor;if(Entry->BrokenGlassFloors&Flag)return false;Entry->BrokenGlassFloors|=Flag;}
    else Entry->Integrity=FMath::Max(0.f,Entry->Integrity-FMath::Min(Amount,1200.f));
    const int32 After=SurvivingFloors(Entry,Info.FloorCount);++DamageRevision;ForceNetUpdate();
    EmitDebris(Impact,Info.Up,S,P,bGlass?5:4,bGlass);
    if(After<Before)
    {
        for(int32 Level=After;Level<Before;++Level)EmitDebris(Info.Floor+Info.Up*(Level*Info.FloorHeight+200),Info.Up,S,P,6);
        BlastEffect(Info.Floor+Info.Up*(After*Info.FloorHeight+200),1600);
        UE_LOG(LogTemp,Display,TEXT("VOYAGER DESTRUCTION COLLAPSE system=%d planet=%d site=%d building=%d floors_before=%d floors_after=%d debris=%d"),S,P,C,B,Before,After,DebrisCount());
    }
    if(auto Law=AVoyagerLaw::Find(GetWorld()))Law->ReportCrime(DamageInstigator,Impact,25,TEXT("property destruction"));
    if(auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->SaveExpedition();return true;
}
void AVoyagerDestruction::EmitDebris(FVector Center,FVector Up,int32 S,int32 P,int32 Count,bool Glass)
{
    if(!HasAuthority()||Center.ContainsNaN()||Up.ContainsNaN()||S<1||P<0||P>=Voyager::PlanetCount)return;
    Count=FMath::Clamp(Count,0,96);Up=Up.GetSafeNormal(UE_SMALL_NUMBER,FVector::UpVector);
    FRandomStream Random{DamageRevision*193+Debris.Num()*7};
    for(int32 I=0;I<Count&&DebrisCount()<96;++I)
    {
        FActorSpawnParameters Params;Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto Piece=GetWorld()->SpawnActor<AVoyagerDebris>(Center,FRotator::ZeroRotator,Params);if(!Piece)continue;
        const FVector Side=Random.VRand();const float Size=Glass?Random.FRandRange(.12f,.34f):Random.FRandRange(.4f,1.3f);
        Piece->InitializePiece(FTransform(Voyager::TangentRotation(Up).Quaternion(),Center+Side*130,FVector(Size,Size*(Glass?.15f:1.f),Size*.4f)),Glass?FLinearColor(.15f,.43f,.5f):FLinearColor(.35f,.34f,.31f),(Side+Up*.7f)*Random.FRandRange(350.f,1100.f),S,P);Debris.Add(Piece);
    }
}
void AVoyagerDestruction::Explode(FVector Center,AController* DamageInstigator)
{
    if(!HasAuthority()||Center.ContainsNaN())return;auto State=GetWorld()->GetGameState<AVoyagerState>();if(!State)return;
    const int32 P=Voyager::NearestPlanet(State->SystemSeed,Center);const float Radius=2200;
    for(int32 Site=0;Site<3;++Site)for(int32 Building=0;Building<60;++Building)
    {
        FVoyagerBuildingInfo Info;if(!AVoyagerSettlement::GetBuildingInfo(State->SystemSeed,P,Site,Building,Info))continue;
        const FVector Local=Info.Rotation.UnrotateVector(Center-Info.Floor);
        const FVector Nearest(FMath::Clamp(Local.X,-Info.Width*.5,Info.Width*.5),FMath::Clamp(Local.Y,-Info.Depth*.5,Info.Depth*.5),FMath::Clamp(Local.Z,0.,Info.Height));
        if(FVector::DistSquared(Local,Nearest)<FMath::Square(Radius))DamageBuilding(State->SystemSeed,P,Site,Building,750,Center,DamageInstigator);
    }
    for(TActorIterator<AActor> It(GetWorld());It;++It)
    {
        if(!(Cast<AVoyagerCharacter>(*It)||Cast<AVoyagerCitizen>(*It)||Cast<AVoyagerAnimal>(*It)||Cast<AVoyagerPatrolShip>(*It)||Cast<AVoyagerShip>(*It)||Cast<AVoyagerPoliceUnit>(*It)))continue;
        const double Distance=FVector::Dist(Center,It->GetActorLocation());if(Distance<Radius)
            UGameplayStatics::ApplyDamage(*It,float(95*(1-Distance/Radius)),DamageInstigator,this,nullptr);
    }
    BlastEffect(Center,Radius);
}
void AVoyagerDestruction::BlastEffect_Implementation(FVector Center,float Radius)
{
    if(GetNetMode()==NM_DedicatedServer||Center.ContainsNaN()||!FMath::IsFinite(Radius)||Radius<=0)return;
    Radius=FMath::Min(Radius,3000.f);
    const auto* State=GetWorld()->GetGameState<AVoyagerState>();
    const FVector Up=State?Voyager::SurfaceNormal(State->SystemSeed,Voyager::NearestPlanet(State->SystemSeed,Center),Center):FVector::UpVector;
    const FTransform Frame(Voyager::TangentRotation(Up),Center);
    DustClouds.RemoveAll([](const auto& Cloud){return !IsValid(Cloud);});
    if(DustClouds.Num()>=6){DustClouds[0]->Destroy();DustClouds.RemoveAt(0);}
    if(auto* Cloud=GetWorld()->SpawnActorDeferred<AVoyagerBlastCloud>(AVoyagerBlastCloud::StaticClass(),Frame))
    {Cloud->Radius=Radius;UGameplayStatics::FinishSpawningActor(Cloud,Frame);DustClouds.Add(Cloud);}
    if(auto* Sparks=GetWorld()->SpawnActorDeferred<ARiftBurst>(ARiftBurst::StaticClass(),Frame))
    {Sparks->Color=FLinearColor(1.f,.26f,.025f);Sparks->Size=FMath::Clamp(Radius/250.f,1.f,10.f);UGameplayStatics::FinishSpawningActor(Sparks,Frame);}
    if(auto Sound=LoadObject<USoundBase>(nullptr,TEXT("/Game/Audio/S_Gun.S_Gun")))UGameplayStatics::PlaySoundAtLocation(this,Sound,Center,.65f,.35f);
}
void AVoyagerDestruction::SaveTo(UVoyagerSave* Save) const
{if(!Save)return;Save->BuildingDamage=Archived;Save->BuildingDamage.RemoveAll([this](const auto& Entry){return Entry.System==ActiveSystem;});Save->BuildingDamage.Append(Current);}
void AVoyagerDestruction::RestoreFrom(const UVoyagerSave* Save)
{
    if(!HasAuthority())return;
    // Clearing Current before rebinding is essential: Tick must not append the
    // pre-load world's damage back into the newly restored archive.
    Current.Reset();Archived.Reset();ActiveSystem=INDEX_NONE;
    for(AVoyagerDebris* Piece:Debris)if(IsValid(Piece))Piece->Destroy();Debris.Reset();
    TSet<uint64> Seen;int32 Rejected=0;
    if(Save)for(const auto& Entry:Save->BuildingDamage)
    {
        if(Archived.Num()>=MaxSavedDamageRecords||!ValidDamage(Entry)){++Rejected;continue;}
        const uint64 Key=DamageKey(Entry);if(Seen.Contains(Key)){++Rejected;continue;}
        Seen.Add(Key);Archived.Add(Entry);
    }
    if(Rejected>0)UE_LOG(LogTemp,Warning,TEXT("VOYAGER DESTRUCTION RESTORE rejected=%d invalid, duplicate or over-budget records"),Rejected);
    Tick(0);
}
