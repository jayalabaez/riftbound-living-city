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
#include "VoyagerItemVisuals.h"
#include "Sound/SoundBase.h"
#include "RiftVisual.h"
#include "RiftCharacter.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "livingcity/destruct/BuildingStructure.h"

namespace
{
    constexpr int32 MaxSavedDamageRecords=65536;
    struct FDebrisSurface
    {
        float Density=2300.f,Friction=.8f,Restitution=.08f,LinearDamping=.28f,AngularDamping=.7f,Impulse=18000.f;
    };
    struct FDestructionSettings
    {
        lc::StructureLimits Structure;
        FDebrisSurface Concrete;
        FDebrisSurface Glass{2500.f,.35f,.18f,.6f,1.2f,380.f};
        int32 MaxDebris=96;
        float Lifetime=45.f;
    };
    const FDestructionSettings& Settings()
    {
        static const FDestructionSettings Value=[]
        {
            FDestructionSettings S;FString Text;TSharedPtr<FJsonObject> Json;
            if(FFileHelper::LoadFileToString(Text,*(FPaths::ProjectContentDir()/TEXT("CityData/destruction.json")))&&
                FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Json)&&Json.IsValid())
            {
                auto Read=[&](const FString& Key,float& V,float Low,float High){double N;if(Json->TryGetNumberField(Key,N)&&FMath::IsFinite(N))V=float(FMath::Clamp(N,double(Low),double(High)));};
                float Strength=float(S.Structure.supportStrength),Supports=float(S.Structure.requiredSupports),Budget=float(S.MaxDebris);
                Read(TEXT("support_strength_centipoints"),Strength,100.f,60000.f);S.Structure.supportStrength=FMath::RoundToInt(Strength);
                Read(TEXT("required_regional_supports"),Supports,1.f,4.f);S.Structure.requiredSupports=FMath::RoundToInt(Supports);
                Read(TEXT("maximum_debris_bodies"),Budget,12.f,96.f);S.MaxDebris=FMath::RoundToInt(Budget);
                Read(TEXT("debris_lifetime_seconds"),S.Lifetime,8.f,45.f);
                auto Surface=[&](const FString& Prefix,FDebrisSurface& M)
                {
                    Read(Prefix+TEXT("_density_kg_m3"),M.Density,500.f,3500.f);
                    Read(Prefix+TEXT("_friction"),M.Friction,.05f,1.f);Read(Prefix+TEXT("_restitution"),M.Restitution,0.f,.35f);
                    Read(Prefix+TEXT("_linear_damping"),M.LinearDamping,.05f,3.f);Read(Prefix+TEXT("_angular_damping"),M.AngularDamping,.1f,3.f);
                    Read(Prefix+TEXT("_impulse_kg_cm_s"),M.Impulse,50.f,50000.f);
                };
                Surface(TEXT("concrete"),S.Concrete);Surface(TEXT("glass"),S.Glass);
            }
            else UE_LOG(LogTemp,Warning,TEXT("VOYAGER DESTRUCTION using bounded defaults: destruction.json unavailable"));
            return S;
        }();return Value;
    }
    bool ReadStructure(const FVoyagerBuildingDamage& Entry,lc::BuildingStructure& State)
    {
        if(!FMath::IsFinite(Entry.Integrity)||Entry.Integrity<0.f||Entry.Integrity>1200.f)return false;
        return lc::RestoreStructure(State,FMath::RoundToInt(double(Entry.Integrity)*100.0),Entry.BrokenGlassFloors,
            std::span<const lc::u16>(Entry.SupportDamage.GetData(),Entry.SupportDamage.Num()),Entry.CollapsedFromFloor);
    }
    void WriteStructure(const lc::BuildingStructure& State,FVoyagerBuildingDamage& Entry)
    {
        Entry.Integrity=float(State.integrity)*.01f;Entry.BrokenGlassFloors=State.brokenGlassFloors;Entry.CollapsedFromFloor=State.collapsedFromFloor;
        const bool HasSupportDamage=std::any_of(State.supportDamage.begin(),State.supportDamage.end(),[](lc::u16 D){return D!=0;});
        Entry.SupportDamage.Reset();
        if(HasSupportDamage)Entry.SupportDamage.Append(State.supportDamage.data(),lc::StructureSupportCount);
    }
    bool ValidDamage(const FVoyagerBuildingDamage& Entry)
    {
        lc::BuildingStructure Structure;
        return Entry.System>0&&Entry.Planet>=0&&Entry.Planet<Voyager::PlanetCount&&
            Entry.Site>=0&&Entry.Site<AVoyagerSettlement::SitesPerPlanet&&Entry.Building>=0&&Entry.Building<AVoyagerSettlement::BuildingCount()&&
            ReadStructure(Entry,Structure);
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
    Body->SetCollisionResponseToChannel(ECC_Pawn,ECR_Ignore);Body->SetCollisionResponseToChannel(ECC_WorldDynamic,ECR_Ignore);
    Body->SetCollisionResponseToChannel(ECC_Visibility,ECR_Ignore);Body->SetCollisionResponseToChannel(ECC_Camera,ECR_Ignore);
    Body->SetEnableGravity(false);Body->SetCanEverAffectNavigation(false);
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
void AVoyagerDebris::OnRep_Recycled()
{
    // A reused body represents a new fragment. Never interpolate its old rubble
    // position through the city to the next blast location.
    bReceivedPosition=false;OnRep_Position();
}
void AVoyagerDebris::OnRep_Appearance()
{
    Body->SetWorldScale3D(Scale);
    if(bGlassPiece)
    {
        if(auto* Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_VoyagerGlass.M_VoyagerGlass")))Body->SetMaterial(0,Material);
        else Body->SetMaterial(0,RiftVisual::Material(this,Tint));
    }
    else
    {
        auto* Material=RiftVisual::Material(this,Tint);Material->SetScalarParameterValue(TEXT("Roughness"),.88f);Body->SetMaterial(0,Material);
    }
    Body->SetCastShadow(!bGlassPiece);
}
void AVoyagerDebris::InitializePiece(const FTransform& Transform,FLinearColor Color,FVector Impulse,int32 S,int32 P,bool Glass)
{
    if(!HasAuthority()||Transform.ContainsNaN()||Impulse.ContainsNaN()||S<1||P<0||P>=Voyager::PlanetCount){Destroy();return;}
    // Recycling resets the body rather than allocating beyond the shared pool cap.
    Body->SetSimulatePhysics(false);Age=0;System=S;Planet=P;Tint=Color;bGlassPiece=Glass;++PieceSerial;
    Scale=Transform.GetScale3D().GetAbs().BoundToBox(FVector(.004),FVector(8));
    SetActorTransform(FTransform(Transform.GetRotation(),Transform.GetLocation(),Scale),false,nullptr,ETeleportType::TeleportPhysics);
    Position=GetActorLocation();Rotation=GetActorQuat();OnRep_Appearance();
    const auto& Surface=Glass?Settings().Glass:Settings().Concrete;
    if(!ContactMaterial)ContactMaterial=NewObject<UPhysicalMaterial>(this);
    ContactMaterial->Friction=Surface.Friction;ContactMaterial->StaticFriction=Surface.Friction;
    ContactMaterial->Restitution=Surface.Restitution;ContactMaterial->Density=Surface.Density*.001f;
    ContactMaterial->bOverrideRestitutionCombineMode=true;ContactMaterial->RestitutionCombineMode=EFrictionCombineMode::Min;
    Body->SetPhysMaterialOverride(ContactMaterial);Body->SetLinearDamping(Surface.LinearDamping);Body->SetAngularDamping(Surface.AngularDamping);
    Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);Body->SetSimulatePhysics(true);
    const float MassKg=FMath::Clamp(float(Scale.X*Scale.Y*Scale.Z)*Surface.Density,.05f,1500.f);
    Body->SetMassOverrideInKg(NAME_None,MassKg,true);Body->SetPhysicsLinearVelocity(FVector::ZeroVector);
    Body->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
    // Impulse is momentum (kg cm/s), so equally pushed large chunks move slower.
    Body->AddImpulse(Impulse.GetClampedToMaxSize(MassKg*2200.f),NAME_None,false);
    Body->SetPhysicsAngularVelocityInDegrees(Transform.GetRotation().RotateVector(FVector(35,65,20))*(Glass?2.f:.5f));
    SetLifeSpan(Settings().Lifetime);ForceNetUpdate();
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
{Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(AVoyagerDebris,Position);DOREPLIFETIME(AVoyagerDebris,Rotation);DOREPLIFETIME(AVoyagerDebris,Scale);DOREPLIFETIME(AVoyagerDebris,Tint);DOREPLIFETIME(AVoyagerDebris,bGlassPiece);DOREPLIFETIME(AVoyagerDebris,PieceSerial);}

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
{bReplicates=true;SetReplicateMovement(true);PrimaryActorTick.bCanEverTick=true;Body=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Charge"));SetRootComponent(Body);Body->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);Body->SetVisibility(false);}
void AVoyagerDemolitionCharge::BeginPlay()
{
    Super::BeginPlay();
    // The proxy root stays unscaled: item geometry uses actual centimetres.
    if(!VoyagerItemVisuals::Build(this,Body,EVoyagerItem::DemolitionCharge)&&GetNetMode()!=NM_DedicatedServer)
    {Body->SetVisibility(true);Body->SetRelativeScale3D(FVector(.27,.18,.1));Body->SetMaterial(0,RiftVisual::Material(this,FLinearColor(.07f,.085f,.075f)));}
}
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
{if(!Damage)return Floors;lc::BuildingStructure State;return ReadStructure(*Damage,State)?lc::StandingFloors(State,Floors):Floors;}
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
    const FVector Local=Info.Rotation.UnrotateVector(Impact-Info.Floor);
    const int32 Support=(Local.X>=0?1:0)+(Local.Y>=0?2:0);
    lc::BuildingStructure Structure;
    if(!ReadStructure(*Entry,Structure)||!lc::ApplyStructuralImpact(Structure,Info.FloorCount,Floor,Support,
        FMath::Max(1,FMath::RoundToInt(double(FMath::Min(Amount,1200.f))*100.0)),bGlass,Settings().Structure))return false;
    WriteStructure(Structure,*Entry);
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
    Count=FMath::Clamp(Count,0,Settings().MaxDebris);Up=Up.GetSafeNormal(UE_SMALL_NUMBER,FVector::UpVector);
    Debris.RemoveAll([](const auto& Piece){return !IsValid(Piece);});
    FRandomStream Random{DamageRevision*193+Debris.Num()*7};
    for(int32 I=0;I<Count;++I)
    {
        AVoyagerDebris* Piece=nullptr;
        if(Debris.Num()<Settings().MaxDebris)
        {
            FActorSpawnParameters Params;Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            Piece=GetWorld()->SpawnActor<AVoyagerDebris>(Center,FRotator::ZeroRotator,Params);if(!Piece)continue;Debris.Add(Piece);
        }
        else {NextRecycledPiece%=Debris.Num();Piece=Debris[NextRecycledPiece++];}
        const FVector Side=Random.VRand();const float Size=Glass?Random.FRandRange(.12f,.34f):Random.FRandRange(.22f,.72f);
        const FVector Scale=Glass?FVector(Size,Random.FRandRange(.004f,.008f),Size*.65f):FVector(Size,Size*.8f,Size*.45f);
        const FVector Momentum=(Side+Up*.7f).GetSafeNormal()*(Glass?Settings().Glass.Impulse:Settings().Concrete.Impulse)*Random.FRandRange(.65f,1.25f);
        Piece->InitializePiece(FTransform(Voyager::TangentRotation(Up).Quaternion(),Center+Side*130,Scale),Glass?FLinearColor(.15f,.43f,.5f):FLinearColor(.35f,.34f,.31f),Momentum,S,P,Glass);
    }
}
void AVoyagerDestruction::Explode(FVector Center,AController* DamageInstigator)
{
    if(!HasAuthority()||Center.ContainsNaN())return;auto State=GetWorld()->GetGameState<AVoyagerState>();if(!State)return;
    const int32 P=Voyager::NearestPlanet(State->SystemSeed,Center);const float Radius=2200;
    for(int32 Site=0;Site<AVoyagerSettlement::SitesPerPlanet;++Site)for(int32 Building=0;Building<AVoyagerSettlement::BuildingCount();++Building)
    {
        FVoyagerBuildingInfo Info;if(!AVoyagerSettlement::GetBuildingInfo(State->SystemSeed,P,Site,Building,Info))continue;
        const int32 Floors=SurvivingFloors(FindDamage(State->SystemSeed,P,Site,Building),Info.FloorCount);
        if(Floors<=0)continue;
        const double Height=Floors*Info.FloorHeight;
        const FVector Local=Info.Rotation.UnrotateVector(Center-Info.Floor);
        FVector Nearest(FMath::Clamp(Local.X,-Info.Width*.5,Info.Width*.5),FMath::Clamp(Local.Y,-Info.Depth*.5,Info.Depth*.5),FMath::Clamp(Local.Z,0.,Height));
        if(FVector::DistSquared(Local,Nearest)>=FMath::Square(Radius))continue;
        // Damage the nearest surviving storey, including blasts above collapsed
        // upper floors. Keep a roof contact inside the top storey's half-open range.
        Nearest.Z=FMath::Min(Nearest.Z,Height-1.0);
        const FVector StructuralImpact=Info.Floor+Info.Rotation.RotateVector(Nearest);
        DamageBuilding(State->SystemSeed,P,Site,Building,750,StructuralImpact,DamageInstigator);
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
