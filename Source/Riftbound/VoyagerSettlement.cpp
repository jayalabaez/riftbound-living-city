#include "VoyagerSettlement.h"
#include "VoyagerData.h"
#include "VoyagerGameMode.h"
#include "RiftVisual.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    constexpr int32 MaxCities = 6;
    constexpr double CoreEnter = 3000000.0;
    constexpr double CoreExit = 3600000.0;
    constexpr double DetailEnter = 180000.0;
    constexpr double DetailExit = 240000.0;
    constexpr double CollisionEnter = 110000.0;
    constexpr double CollisionExit = 150000.0;
    constexpr double CityHalfWidth = 20500.0;
    constexpr double PlotSpacing = 4500.0;
    constexpr double RoadWidth = 900.0;
    constexpr double RoomHeight = 480.0;
    constexpr double DoorWidth = 420.0;
    constexpr double DoorHeight = 340.0;

    FString BuildingName(int32 Index)
    {
        static const TCHAR* Names[] = {TEXT("HORIZON CAFE"),TEXT("FRONTIER CLINIC"),TEXT("ORBITAL MARKET"),
            TEXT("SHIPWRIGHT WORKSHOP"),TEXT("EXPLORER RESIDENCES"),TEXT("CIVIC SECURITY")};
        return FString::Printf(TEXT("%s  %02d"),Names[Index % 6],Index + 1);
    }

    struct FCityPalette
    {
        FLinearColor Wall, Secondary, Base, Road, Glow, Roof;
    };
    FCityPalette Palette(int32 Biome)
    {
        static const FCityPalette Values[] = {
            {FLinearColor(.57f,.64f,.56f),FLinearColor(.22f,.40f,.31f),FLinearColor(.18f,.24f,.21f),FLinearColor(.055f,.078f,.065f),FLinearColor(.21f,.95f,.65f),FLinearColor(.13f,.31f,.17f)},
            {FLinearColor(.68f,.49f,.29f),FLinearColor(.32f,.24f,.19f),FLinearColor(.28f,.19f,.12f),FLinearColor(.095f,.075f,.055f),FLinearColor(.25f,.78f,1.f),FLinearColor(.35f,.29f,.22f)},
            {FLinearColor(.56f,.66f,.74f),FLinearColor(.24f,.37f,.48f),FLinearColor(.18f,.25f,.32f),FLinearColor(.05f,.075f,.10f),FLinearColor(.17f,.83f,1.f),FLinearColor(.31f,.43f,.52f)},
            {FLinearColor(.37f,.36f,.35f),FLinearColor(.19f,.23f,.26f),FLinearColor(.16f,.14f,.13f),FLinearColor(.055f,.044f,.04f),FLinearColor(1.f,.39f,.12f),FLinearColor(.23f,.24f,.26f)},
            {FLinearColor(.56f,.49f,.65f),FLinearColor(.29f,.21f,.39f),FLinearColor(.19f,.16f,.24f),FLinearColor(.062f,.045f,.085f),FLinearColor(.83f,.32f,1.f),FLinearColor(.37f,.26f,.46f)}
        };
        return Values[FMath::Clamp(Biome,0,4)];
    }

    struct FCityFrame
    {
        int32 System = 1, Planet = 0;
        double Radius = 1;
        FVector Up, Forward, Right, Origin;
        FVector Direction(double X, double Y) const
        {
            return (Up + (Forward * X + Right * Y) / Radius).GetSafeNormal();
        }
        FVector Ground(double X, double Y, double Height = 0) const
        {
            return Voyager::SurfacePoint(System, Planet, Direction(X,Y), Height);
        }
        FQuat Rotation(double X, double Y) const
        {
            return Voyager::TangentRotation(Direction(X,Y), Forward).Quaternion();
        }
    };
    FCityFrame CityFrame(int32 System, int32 Planet, int32 Site)
    {
        FCityFrame Frame;
        Frame.System = System; Frame.Planet = Planet;
        Frame.Radius = Voyager::PlanetRadius(System,Planet);
        Frame.Up = AVoyagerSettlement::SiteDirection(System,Planet,Site);
        const FQuat Rotation = Voyager::TangentRotation(Frame.Up).Quaternion();
        Frame.Forward = Rotation.GetAxisX(); Frame.Right = Rotation.GetAxisY();
        Frame.Origin = Voyager::SurfacePoint(System,Planet,Frame.Up);
        return Frame;
    }

    struct FBuilding
    {
        double X, Y, Width, Depth, Height;
        int32 Style, Column, Row;
    };
    TArray<FBuilding> BuildingPlan(int32 System, int32 Planet, int32 Site)
    {
        TArray<FBuilding> Result;
        FRandomStream Random{int32(Voyager::Hash(uint32(Voyager::PlanetSeed(System,Planet)) ^ uint32(Site + 1) * 73856093u))};
        for (int32 Y = 0; Y < 8; ++Y)
            for (int32 X = 0; X < 8; ++X)
            {
                // A ninety-metre central civic square and landing plaza remain open.
                if ((X == 3 || X == 4) && (Y == 3 || Y == 4)) continue;
                const double PX = (X - 3.5) * PlotSpacing, PY = (Y - 3.5) * PlotSpacing;
                const double Center = 1.0 - FMath::Clamp(FMath::Sqrt(PX*PX + PY*PY) / 23000.0,0.0,1.0);
                double Height = Random.FRandRange(600.f,2450.f) + Center * Center * 6700.0;
                if ((X == 2 && Y == 3) || (X == 5 && Y == 4)) Height = 7200;
                Height = FMath::Max(Height,1050.0);
                Result.Add({PX,PY,Random.FRandRange(2100.f,3200.f),Random.FRandRange(2100.f,3150.f),Height,
                    Random.RandRange(0,3),X,Y});
            }
        return Result;
    }
    struct FBuildingBase
    {
        FVector Ground, Up;
        FQuat Rotation;
        double Top = 32, Bottom = -450;
        FVector Floor() const { return Ground + Up * Top; }
    };
    FBuildingBase Foundation(const FCityFrame& Frame, const FBuilding& B)
    {
        FBuildingBase Base;
        Base.Up = Frame.Direction(B.X,B.Y);
        Base.Ground = Frame.Ground(B.X,B.Y);
        Base.Rotation = Frame.Rotation(B.X,B.Y);
        double Low = 0, High = 0;
        for (int32 Corner = 0; Corner < 4; ++Corner)
        {
            const FVector Ground = Frame.Ground(B.X + (Corner & 1 ? 1 : -1) * (B.Width + 450) * .5,
                B.Y + (Corner & 2 ? 1 : -1) * (B.Depth + 450) * .5);
            const double Elevation = FVector::DotProduct(Ground - Base.Ground, Base.Up);
            Low = FMath::Min(Low,Elevation); High = FMath::Max(High,Elevation);
        }
        Base.Bottom = Low - 450;
        Base.Top = High + 32;
        return Base;
    }

    void Dispose(AActor* Owner, TArray<TObjectPtr<UActorComponent>>& Components)
    {
        for (UActorComponent* Component : Components)
        {
            if (!IsValid(Component)) continue;
            Owner->RemoveInstanceComponent(Component);
            Component->DestroyComponent();
            Component->SetFlags(RF_Transient);
            const FName Retired = MakeUniqueObjectName(GetTransientPackage(), Component->GetClass(), TEXT("RetiredSettlement"));
            Component->Rename(*Retired.ToString(),GetTransientPackage(),
                REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty | REN_SkipComponentRegWork);
        }
        Components.Empty();
    }
    UInstancedStaticMeshComponent* Instances(AActor* Owner, FVoyagerSettlementRecord& Record,
        bool bDetail, const FString& Name, const TCHAR* Shape, FVector Origin,
        FLinearColor Tint, bool bGlow = false, bool bArchitecture = false, bool bCollision = false,
        int32 SurfaceType = INDEX_NONE)
    {
        auto* Mesh = NewObject<UInstancedStaticMeshComponent>(Owner,FName(Name));
        Mesh->SetNetAddressable();
        Owner->AddInstanceComponent(Mesh);
        Mesh->SetupAttachment(Owner->GetRootComponent());
        Mesh->SetRelativeLocation(Origin);
        Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,
            *FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"),Shape,Shape)));
        UMaterialInstanceDynamic* Material = nullptr;
        if (SurfaceType != INDEX_NONE)
            if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_VoyagerInterior.M_VoyagerInterior")))
            {
                Material = UMaterialInstanceDynamic::Create(Base,Owner);
                Material->SetVectorParameterValue(TEXT("Tint"),Tint);
                Material->SetScalarParameterValue(TEXT("SurfaceType"),float(SurfaceType));
                Material->SetScalarParameterValue(TEXT("BumpStrength"),.10f);
            }
        if (!Material && bArchitecture)
            if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_VoyagerArchitecture.M_VoyagerArchitecture")))
            {
                Material = UMaterialInstanceDynamic::Create(Base,Owner);
                Material->SetVectorParameterValue(TEXT("Tint"),Tint);
                Material->SetScalarParameterValue(TEXT("WindowGlow"),.42f);
            }
        if (!Material) Material = RiftVisual::Material(Owner,Tint,bGlow);
        Mesh->SetMaterial(0,Material);
        Mesh->SetCollisionProfileName(TEXT("BlockAll"));
        Mesh->SetCollisionObjectType(ECC_WorldStatic);
        Mesh->SetCollisionEnabled(bCollision && Record.bCollisionActive ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
        Mesh->SetGenerateOverlapEvents(false);
        Mesh->SetCanEverAffectNavigation(false);
        Mesh->SetCastShadow(!bGlow);
        Mesh->SetMobility(EComponentMobility::Static);
        Mesh->RegisterComponent();
        (bDetail ? Record.Details : Record.Core).Add(Mesh);
        if (bCollision) Record.Colliders.Add(Mesh);
        return Mesh;
    }
    void Add(UInstancedStaticMeshComponent* Mesh, FVector WorldPosition, FVector DimensionsCm,
        FQuat Rotation = FQuat::Identity)
    {
        Mesh->AddInstance(FTransform(Rotation,WorldPosition - Mesh->GetComponentLocation(),DimensionsCm / 100.0));
    }
    void GroundStrip(UInstancedStaticMeshComponent* Mesh, const FCityFrame& Frame,
        double X, double Y, double Length, double Width, double Height, bool bAlongY)
    {
        const FQuat Rotation = Frame.Rotation(X,Y) * FRotator(0,bAlongY ? 90.f : 0.f,0).Quaternion();
        Add(Mesh,Frame.Ground(X,Y,Height),FVector(Length,Width,16),Rotation);
    }
    void Label(AActor* Owner, FVoyagerSettlementRecord& Record, const FString& Name,
        const FString& Message, FVector Position, FQuat Rotation, float Size, FColor Color)
    {
        auto* Text = NewObject<UTextRenderComponent>(Owner,FName(Name));
        Owner->AddInstanceComponent(Text);
        Text->SetupAttachment(Owner->GetRootComponent());
        Text->SetRelativeLocationAndRotation(Position,Rotation);
        Text->SetText(FText::FromString(Message));
        Text->SetHorizontalAlignment(EHorizTextAligment::EHTA_Center);
        Text->SetVerticalAlignment(EVerticalTextAligment::EVRTA_TextCenter);
        Text->SetWorldSize(Size);
        Text->SetTextRenderColor(Color);
        Text->SetCastShadow(false);
        Text->SetCullDistance(14000.f);
        Text->SetMobility(EComponentMobility::Static);
        Text->RegisterComponent();
        Record.Details.Add(Text);
    }
}

AVoyagerSettlement::AVoyagerSettlement()
{
    bReplicates = true;
    bAlwaysRelevant = true;
    SetReplicateMovement(false);
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickInterval = .25f;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("SettlementRoot")));
    GetRootComponent()->SetMobility(EComponentMobility::Static);
}
void AVoyagerSettlement::BeginPlay() { Super::BeginPlay(); Refresh(); }
void AVoyagerSettlement::Tick(float DeltaSeconds) { Super::Tick(DeltaSeconds); Refresh(); UpdateInteriorLights(); }
void AVoyagerSettlement::EndPlay(const EEndPlayReason::Type Reason) { ClearAll(); Super::EndPlay(Reason); }

FVector AVoyagerSettlement::SiteDirection(int32 System, int32 Planet, int32 Site)
{
    if (Site <= 0) return FVector(30000,12000,Voyager::PlanetRadius(System,Planet)).GetSafeNormal();
    FRandomStream Random{int32(Voyager::Hash(uint32(Voyager::PlanetSeed(System,Planet)) + uint32(Site) * 104729u))};
    FVector Direction = Random.VRand();
    if (Direction.Z > .75) Direction.Z = -.35;
    return Direction.GetSafeNormal();
}
FString AVoyagerSettlement::SiteName(int32 System, int32 Planet, int32 Site)
{
    static const TCHAR* Names[] = {TEXT("Verdant Exchange"),TEXT("Sundial Port"),TEXT("Boreal Reach"),TEXT("Cinder Foundry"),TEXT("Amethyst Haven")};
    const FString Base = Names[Voyager::Biome(System,Planet)];
    return Site == 0 ? Base : FString::Printf(TEXT("%s %02d"),*Base,Site + 1);
}
bool AVoyagerSettlement::IsWithinSite(int32 System, int32 Planet, FVector UnitDirection, double PaddingCm)
{
    const double Limit = CityHalfWidth + FMath::Max(0.0,PaddingCm);
    const double Radius = Voyager::PlanetRadius(System,Planet);
    UnitDirection = UnitDirection.GetSafeNormal();
    for (int32 Site = 0; Site < SitesPerPlanet; ++Site)
    {
        const FCityFrame Frame = CityFrame(System,Planet,Site);
        const double Facing = FVector::DotProduct(UnitDirection,Frame.Up);
        if (Facing < .999) continue;
        const FVector Delta = (UnitDirection / Facing - Frame.Up) * Radius;
        if (FMath::Abs(FVector::DotProduct(Delta,Frame.Forward)) <= Limit &&
            FMath::Abs(FVector::DotProduct(Delta,Frame.Right)) <= Limit) return true;
    }
    return false;
}
bool AVoyagerSettlement::GetBuildingInfo(int32 System, int32 Planet, int32 Site, int32 Index, FVoyagerBuildingInfo& Out)
{
    if (Planet < 0 || Planet >= Voyager::PlanetCount || Site < 0 || Site >= SitesPerPlanet || Index < 0 || Index >= BuildingCount()) return false;
    const FCityFrame Frame = CityFrame(System,Planet,Site);
    const TArray<FBuilding> Plan = BuildingPlan(System,Planet,Site);
    const FBuilding& B = Plan[Index];
    const FBuildingBase Base = Foundation(Frame,B);
    Out.Index = Index; Out.Role = Index % 6; Out.Name = BuildingName(Index);
    Out.Floor = Base.Floor(); Out.Up = Base.Up; Out.Rotation = Base.Rotation;
    Out.Forward = Base.Rotation.GetAxisX(); Out.Right = Base.Rotation.GetAxisY();
    Out.Width = B.Width; Out.Depth = B.Depth;
    Out.DoorOutside = Frame.Ground(B.X,(B.Row - 4) * PlotSpacing,20);
    Out.DoorThreshold = Out.Floor - Out.Right * (B.Depth * .5 + 225) + Out.Up * 2;
    Out.DoorInside = Out.Floor - Out.Right * (B.Depth * .5 - 180) + Out.Up * 2;
    Out.InteriorPoint = Out.Floor + Out.Up * 2;
    return true;
}
FVector AVoyagerSettlement::StreetPoint(int32 System, int32 Planet, int32 Site, int32 GridX, int32 GridY, double Clearance)
{
    return CityFrame(System,Planet,Site).Ground((FMath::Clamp(GridX,0,8) - 4) * PlotSpacing,
        (FMath::Clamp(GridY,0,8) - 4) * PlotSpacing,20 + Clearance);
}
int32 AVoyagerSettlement::FindBuildingAt(int32 System, int32 Planet, int32 Site, FVector Position)
{
    const FCityFrame Frame = CityFrame(System,Planet,Site);
    const FVector Delta = Position - Frame.Origin;
    const int32 Column = FMath::FloorToInt(FVector::DotProduct(Delta,Frame.Forward) / PlotSpacing + 4);
    const int32 Row = FMath::FloorToInt(FVector::DotProduct(Delta,Frame.Right) / PlotSpacing + 4);
    if (Column < 0 || Column >= 8 || Row < 0 || Row >= 8 || ((Column == 3 || Column == 4) && (Row == 3 || Row == 4))) return INDEX_NONE;
    const TArray<FBuilding> Plan = BuildingPlan(System,Planet,Site);
    for (int32 Index = 0; Index < Plan.Num(); ++Index)
    {
        const FBuilding& B = Plan[Index];
        if (B.Column != Column || B.Row != Row) continue;
        const FBuildingBase Base = Foundation(Frame,B);
        const FVector Local = Base.Rotation.UnrotateVector(Position - Base.Floor());
        return FMath::Abs(Local.X) < B.Width * .5 - 20 && FMath::Abs(Local.Y) < B.Depth * .5 - 20 &&
            Local.Z > -20 && Local.Z < RoomHeight ? Index : INDEX_NONE;
    }
    return INDEX_NONE;
}
int32 AVoyagerSettlement::ActiveBuildingCount() const
{
    int32 Count = 0;
    for (const auto& Pair : Cities) Count += Pair.Value.Buildings;
    return Count;
}
int32 AVoyagerSettlement::ActiveDetailCount() const
{
    int32 Count = 0;
    for (const auto& Pair : Cities)
        for (UActorComponent* Component : Pair.Value.Details)
            if (auto* Mesh = Cast<UInstancedStaticMeshComponent>(Component)) Count += Mesh->GetInstanceCount();
    return Count;
}
void AVoyagerSettlement::ClearAll()
{
    TArray<int32> Keys;
    Cities.GetKeys(Keys);
    for (int32 Key : Keys) RemoveCity(Key);
}
void AVoyagerSettlement::RemoveCity(int32 Key)
{
    if (FVoyagerSettlementRecord* City = Cities.Find(Key))
    {
        RemoveDetails(*City);
        Dispose(this,City->Core);
        City->Colliders.Empty();
    }
    Cities.Remove(Key);
}
void AVoyagerSettlement::RemoveDetails(FVoyagerSettlementRecord& City)
{
    City.Colliders.RemoveAll([&City](const TObjectPtr<UPrimitiveComponent>& Component)
    {
        return !IsValid(Component) || City.Details.Contains(Component.Get());
    });
    City.InteriorLights.Empty();
    City.StreetLights.Empty();
    City.StreetLightPositions.Empty();
    Dispose(this,City.Details);
}
void AVoyagerSettlement::UpdateInteriorLights()
{
    // Shared pools allow at most twelve active lights across every city: eight
    // interior lights and four street lamps following the local players.
    struct FLightCandidate { int32 Key, Building; double Distance; };
    TArray<FLightCandidate> Candidates;
    TArray<FLightCandidate> StreetCandidates;
    TArray<FVector> Views;
    for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
        if (const APlayerController* PC = It->Get(); PC && PC->IsLocalController() && PC->GetPawn()) Views.Add(PC->GetPawn()->GetActorLocation());
    for (auto& Pair : Cities)
    {
        for (UPointLightComponent* Light : Pair.Value.InteriorLights) if (IsValid(Light)) Light->SetVisibility(false);
        for (UPointLightComponent* Light : Pair.Value.StreetLights) if (IsValid(Light)) Light->SetVisibility(false);
        if (!Pair.Value.StreetLights.IsEmpty())
            for (int32 Index = 0; Index < Pair.Value.StreetLightPositions.Num(); ++Index)
            {
                double Distance = TNumericLimits<double>::Max();
                for (FVector View : Views) Distance = FMath::Min(Distance,FVector::DistSquared(View,Pair.Value.StreetLightPositions[Index]));
                if (Distance < FMath::Square(7500.0)) StreetCandidates.Add({Pair.Key,Index,Distance});
            }
        if (Pair.Value.InteriorLights.IsEmpty()) continue;
        for (int32 Index = 0; Index < Pair.Value.BuildingInfo.Num(); ++Index)
        {
            double Distance = TNumericLimits<double>::Max();
            for (FVector View : Views) Distance = FMath::Min(Distance,FVector::DistSquared(View,Pair.Value.BuildingInfo[Index].InteriorPoint));
            if (Distance < FMath::Square(10000.0)) Candidates.Add({Pair.Key,Index,Distance});
        }
    }
    Candidates.Sort([](const FLightCandidate& A,const FLightCandidate& B) { return A.Distance < B.Distance; });
    TMap<int32,int32> Assigned;
    for (int32 Index = 0; Index < FMath::Min(8,Candidates.Num()); ++Index)
    {
        const FLightCandidate& Candidate = Candidates[Index];
        FVoyagerSettlementRecord& City = Cities.FindChecked(Candidate.Key);
        int32& LightIndex = Assigned.FindOrAdd(Candidate.Key);
        if (!City.InteriorLights.IsValidIndex(LightIndex)) continue;
        UPointLightComponent* Light = City.InteriorLights[LightIndex++];
        const FVoyagerBuildingInfo& Building = City.BuildingInfo[Candidate.Building];
        Light->SetWorldLocation(Building.Floor + Building.Up * 350);
        Light->SetLightColor(Building.Role == 1 ? FLinearColor(.78f,.88f,1.f) : FLinearColor(1.f,.78f,.53f));
        Light->SetVisibility(true);
    }
    StreetCandidates.Sort([](const FLightCandidate& A,const FLightCandidate& B) { return A.Distance < B.Distance; });
    Assigned.Empty();
    for (int32 Index = 0; Index < FMath::Min(4,StreetCandidates.Num()); ++Index)
    {
        const FLightCandidate& Candidate = StreetCandidates[Index];
        FVoyagerSettlementRecord& City = Cities.FindChecked(Candidate.Key);
        int32& LightIndex = Assigned.FindOrAdd(Candidate.Key);
        if (!City.StreetLights.IsValidIndex(LightIndex)) continue;
        UPointLightComponent* Light = City.StreetLights[LightIndex++];
        Light->SetWorldLocation(City.StreetLightPositions[Candidate.Building]);
        Light->SetVisibility(true);
    }
}
void AVoyagerSettlement::Refresh()
{
    const AVoyagerState* State = GetWorld()->GetGameState<AVoyagerState>();
    if (!State) return;
    if (BuiltSystem != State->SystemSeed) { ClearAll(); BuiltSystem = State->SystemSeed; }
    TArray<FVector> Views;
    for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
    {
        const APlayerController* PC = It->Get();
        if (PC && (HasAuthority() || PC->IsLocalController()))
            if (const APawn* Pawn = PC->GetPawn()) Views.Add(Pawn->GetActorLocation());
    }
    if (Views.IsEmpty()) return;
    struct FCandidate { int32 Key; double Distance; };
    TArray<FCandidate> Wanted;
    for (int32 Planet = 0; Planet < Voyager::PlanetCount; ++Planet)
        for (int32 Site = 0; Site < SitesPerPlanet; ++Site)
        {
            const int32 Key = Planet * SitesPerPlanet + Site;
            const FVector Position = Voyager::SurfacePoint(BuiltSystem,Planet,SiteDirection(BuiltSystem,Planet,Site));
            double Distance = TNumericLimits<double>::Max();
            for (FVector View : Views) Distance = FMath::Min(Distance,FVector::Dist(View,Position));
            const double Range = GetNetMode() == NM_DedicatedServer ? CollisionExit : Cities.Contains(Key) ? CoreExit : CoreEnter;
            if (Distance <= Range) Wanted.Add({Key,Distance});
        }
    Wanted.Sort([](const FCandidate& A,const FCandidate& B) { return A.Distance == B.Distance ? A.Key < B.Key : A.Distance < B.Distance; });
    if (Wanted.Num() > MaxCities) Wanted.SetNum(MaxCities);
    TSet<int32> Keep;
    for (const FCandidate& Candidate : Wanted) Keep.Add(Candidate.Key);
    TArray<int32> Existing;
    Cities.GetKeys(Existing);
    for (int32 Key : Existing) if (!Keep.Contains(Key)) RemoveCity(Key);
    bool bBuiltCore = false, bBuiltDetails = false;
    for (const FCandidate& Candidate : Wanted)
    {
        if (!Cities.Contains(Candidate.Key))
        {
            if (bBuiltCore) continue;
            BuildCity(Candidate.Key); bBuiltCore = true;
        }
        FVoyagerSettlementRecord& City = Cities.FindChecked(Candidate.Key);
        const bool bCollision = Candidate.Distance < (City.bCollisionActive ? CollisionExit : CollisionEnter);
        if (bCollision != City.bCollisionActive)
        {
            City.bCollisionActive = bCollision;
            for (UPrimitiveComponent* Collider : City.Colliders)
                if (IsValid(Collider)) Collider->SetCollisionEnabled(bCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
        }
        if (!City.Details.IsEmpty() && Candidate.Distance > DetailExit) RemoveDetails(City);
        if (City.Details.IsEmpty() && Candidate.Distance < DetailEnter && !bBuiltDetails)
        {
            BuildDetails(Candidate.Key); bBuiltDetails = true;
        }
    }
}

void AVoyagerSettlement::BuildCity(int32 Key)
{
    if (Cities.Contains(Key) || Cities.Num() >= MaxCities) return;
    FVoyagerSettlementRecord& City = Cities.Add(Key);
    const int32 Planet = Key / SitesPerPlanet, Site = Key % SitesPerPlanet;
    const FCityFrame Frame = CityFrame(BuiltSystem,Planet,Site);
    const FCityPalette Colors = Palette(Voyager::Biome(BuiltSystem,Planet));
    const FString Name = FString::Printf(TEXT("Settlement_%d_%d_"),BuiltSystem,Key);
    auto* Hull = Instances(this,City,false,Name + TEXT("Towers"),TEXT("Cube"),Frame.Origin,Colors.Wall,false,true,true);
    auto* Upper = Instances(this,City,false,Name + TEXT("TieredTowers"),TEXT("Cube"),Frame.Origin,Colors.Secondary,false,true,true);
    auto* Base = Instances(this,City,false,Name + TEXT("Foundations"),TEXT("Cube"),Frame.Origin,Colors.Base,false,false,true);
    auto* Roofs = Instances(this,City,false,Name + TEXT("RoofCaps"),TEXT("Cube"),Frame.Origin,Colors.Roof,false,false,true);
    auto* Roads = Instances(this,City,false,Name + TEXT("Roads"),TEXT("Cube"),Frame.Origin,Colors.Road,false,false,true);
    auto* Plazas = Instances(this,City,false,Name + TEXT("Plazas"),TEXT("Cylinder"),Frame.Origin,Colors.Base,false,false,true);
    auto* Silhouette = Instances(this,City,false,Name + TEXT("SpireTops"),TEXT("Cylinder"),Frame.Origin,Colors.Secondary,false,false);
    auto* Beacons = Instances(this,City,false,Name + TEXT("RoofBeacons"),TEXT("Sphere"),Frame.Origin,Colors.Glow,true);
    auto* InteriorWalls = Instances(this,City,false,Name + TEXT("RoomWalls"),TEXT("Cube"),Frame.Origin,FLinearColor(.70f,.68f,.60f),false,false,true,0);
    auto* Floors = Instances(this,City,false,Name + TEXT("RoomFloors"),TEXT("Cube"),Frame.Origin,FLinearColor(.31f,.32f,.30f),false,false,true,4);
    auto* WindowFrames = Instances(this,City,false,Name + TEXT("WindowFrames"),TEXT("Cube"),Frame.Origin,FLinearColor(.085f,.11f,.12f),false,false,true,2);
    const TArray<FBuilding> Plan = BuildingPlan(BuiltSystem,Planet,Site);
    City.Buildings = Plan.Num();
    for (int32 Index = 0; Index < Plan.Num(); ++Index)
    {
        const FBuilding& B = Plan[Index];
        const FBuildingBase F = Foundation(Frame,B);
        FVoyagerBuildingInfo Info;
        GetBuildingInfo(BuiltSystem,Planet,Site,Index,Info);
        City.BuildingInfo.Add(Info);
        Add(Base,F.Ground + F.Up * (F.Top + F.Bottom) * .5,FVector(B.Width + 450,B.Depth + 450,F.Top - F.Bottom),F.Rotation);
        const FVector Floor = F.Floor();
        const FVector X = F.Rotation.GetAxisX(), Y = F.Rotation.GetAxisY();
        auto Place = [&](UInstancedStaticMeshComponent* Mesh, double LX, double LY, double LZ, FVector Size)
        { Add(Mesh,Floor + X * LX + Y * LY + F.Up * LZ,Size,F.Rotation); };
        const double Lower = B.Height * .72, UpperHeight = B.Height - Lower;
        // The visible orbital mass and the walk-in room share one building. No
        // hidden solid tower intersects the player beneath the first ceiling.
        Add(B.Style & 1 ? Upper : Hull,Floor + F.Up * (RoomHeight + (Lower - RoomHeight) * .5),
            FVector(B.Width,B.Depth,Lower - RoomHeight),F.Rotation);
        Place(Floors,0,0,-3,FVector(B.Width + 448,B.Depth + 448,6));
        Place(InteriorWalls,0,0,RoomHeight - 10,FVector(B.Width,B.Depth,20));
        const double Wing = (B.Width - DoorWidth) * .5;
        for (int32 Side : {-1,1})
        {
            Place(InteriorWalls,Side * (DoorWidth * .5 + Wing * .5),-B.Depth * .5,RoomHeight * .5,FVector(Wing,36,RoomHeight));
            Place(WindowFrames,Side * (DoorWidth * .5 + 12),-B.Depth * .5 - 4,DoorHeight * .5,FVector(24,56,DoorHeight));
            // Open daylight windows have real apertures, sills and four mullions.
            Place(InteriorWalls,Side * B.Width * .5,0,60,FVector(36,B.Depth,120));
            Place(InteriorWalls,Side * B.Width * .5,0,395,FVector(36,B.Depth,170));
            Place(WindowFrames,Side * B.Width * .5,0,120,FVector(52,B.Depth,16));
            Place(WindowFrames,Side * B.Width * .5,0,310,FVector(52,B.Depth,16));
            for (int32 Post = 0; Post < 4; ++Post)
                Place(WindowFrames,Side * B.Width * .5,(Post - 1.5) * B.Depth / 3,215,FVector(45,65,190));
        }
        Place(InteriorWalls,0,-B.Depth * .5,(RoomHeight + DoorHeight) * .5,FVector(DoorWidth,36,RoomHeight - DoorHeight));
        Place(WindowFrames,0,-B.Depth * .5 - 4,DoorHeight,FVector(DoorWidth + 48,56,24));
        Place(InteriorWalls,0,B.Depth * .5,60,FVector(B.Width,36,120));
        Place(InteriorWalls,0,B.Depth * .5,395,FVector(B.Width,36,170));
        Place(WindowFrames,0,B.Depth * .5,120,FVector(B.Width,52,16));
        Place(WindowFrames,0,B.Depth * .5,310,FVector(B.Width,52,16));
        for (int32 Post = 0; Post < 4; ++Post)
            Place(WindowFrames,(Post - 1.5) * B.Width / 3,B.Depth * .5,215,FVector(65,45,190));
        const FVector RampDirection = Info.DoorThreshold - Info.DoorOutside;
        const FQuat RampRotation = FRotationMatrix::MakeFromXZ(RampDirection.GetSafeNormal(),F.Up).ToQuat();
        Add(Floors,(Info.DoorThreshold + Info.DoorOutside) * .5 - RampRotation.GetAxisZ() * 10,
            FVector(RampDirection.Size() + 12,DoorWidth + 130,20),RampRotation);
        if (Index % 10 == 4 && Index % 6 != 2)
        {
            // A rear gallery uses ordinary twenty-centimetre steps and stays out
            // of the door-to-service corridor. Its balcony is part of this room.
            const double GalleryY = B.Depth * .5 - 210;
            Place(Floors,0,GalleryY,233,FVector(B.Width - 90,420,14));
            const double StairX = B.Width * .5 - 150;
            const double StartY = B.Depth * .5 - 900;
            for (int32 Step = 0; Step < 12; ++Step)
            {
                const double Height = (Step + 1) * 20;
                Place(Floors,StairX,StartY + (Step + .5) * 40,Height * .5,FVector(200,40,Height));
            }
            Place(WindowFrames,-100,B.Depth * .5 - 409,340,FVector(B.Width - 400,18,18));
            for (int32 Post = 0; Post < 6; ++Post)
                Place(WindowFrames,-B.Width * .5 + 100 + Post * (B.Width - 500) / 5,B.Depth * .5 - 409,290,FVector(16,16,100));
        }
        Add(B.Style & 1 ? Hull : Upper,Floor + F.Up * (Lower + UpperHeight * .5),
            FVector(B.Width * .73,B.Depth * .78,UpperHeight),F.Rotation);
        Add(Roofs,Floor + F.Up * (Lower + 36),FVector(B.Width + 110,B.Depth + 110,72),F.Rotation);
        Add(Roofs,Floor + F.Up * (B.Height + 30),FVector(B.Width * .78,B.Depth * .83,60),F.Rotation);
        if (B.Height > 4000)
        {
            const double AntennaHeight = B.Style == 0 ? 1100 : 650;
            Add(Silhouette,Floor + F.Up * (B.Height + AntennaHeight * .5),FVector(95,95,AntennaHeight),F.Rotation);
            Add(Beacons,Floor + F.Up * (B.Height + AntennaHeight),FVector(105,105,105),F.Rotation);
        }
    }
    // Roads follow sampled radial ground; no giant flat foundation hides the planet.
    for (int32 Road = 0; Road <= 8; ++Road)
    {
        const double Axis = (Road - 4) * PlotSpacing;
        for (int32 Segment = 0; Segment < 24; ++Segment)
            GroundStrip(Roads,Frame,Axis,-17250 + Segment * 1500,1510,RoadWidth,10,true);
        for (int32 Gap = 0; Gap < 8; ++Gap)
            GroundStrip(Roads,Frame,-15750 + Gap * PlotSpacing,Axis,PlotSpacing - RoadWidth,RoadWidth,10,false);
    }
    Add(Plazas,Frame.Ground(0,0,1),FVector(7200,7200,22),Frame.Rotation(0,0));
    Add(Plazas,Frame.Ground(0,0,15),FVector(5300,5300,20),Frame.Rotation(0,0));
    // A few bridges form a readable urban silhouette without filling the civic square.
    for (int32 Row : {1,6})
        for (int32 Pair : {1,5})
        {
            const double X = (Pair - 3.0) * PlotSpacing, Y = (Row - 3.5) * PlotSpacing;
            Add(Base,Frame.Ground(X,Y,850),FVector(PlotSpacing,620,90),Frame.Rotation(X,Y));
            Add(Roofs,Frame.Ground(X,Y,1140),FVector(PlotSpacing,740,70),Frame.Rotation(X,Y));
        }
    UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY BUILT site=%s planet=%d buildings=%d center=%s"),
        *SiteName(BuiltSystem,Planet,Site),Planet,City.Buildings,*Frame.Origin.ToString());
}

void AVoyagerSettlement::BuildDetails(int32 Key)
{
    FVoyagerSettlementRecord* Found = Cities.Find(Key);
    if (!Found || !Found->Details.IsEmpty()) return;
    FVoyagerSettlementRecord& City = *Found;
    const int32 Planet = Key / SitesPerPlanet, Site = Key % SitesPerPlanet, Biome = Voyager::Biome(BuiltSystem,Planet);
    const FCityFrame Frame = CityFrame(BuiltSystem,Planet,Site);
    const FCityPalette Colors = Palette(Biome);
    const FString Name = FString::Printf(TEXT("SettlementDetail_%d_%d_"),BuiltSystem,Key);
    auto* WindowBands = Instances(this,City,true,Name + TEXT("WindowBands"),TEXT("Cube"),Frame.Origin,Colors.Glow * .52f,true);
    auto* Frames = Instances(this,City,true,Name + TEXT("FacadeFrames"),TEXT("Cube"),Frame.Origin,Colors.Base);
    auto* Equipment = Instances(this,City,true,Name + TEXT("Equipment"),TEXT("Cube"),Frame.Origin,Colors.Secondary,false,true);
    auto* Pipes = Instances(this,City,true,Name + TEXT("Pipes"),TEXT("Cylinder"),Frame.Origin,Colors.Base);
    auto* Lamps = Instances(this,City,true,Name + TEXT("StreetLamps"),TEXT("Sphere"),Frame.Origin,Colors.Glow,true);
    auto* Markings = Instances(this,City,true,Name + TEXT("StreetMarkings"),TEXT("Cube"),Frame.Origin,FLinearColor(.7f,.74f,.72f));
    auto* Signs = Instances(this,City,true,Name + TEXT("NavigationLights"),TEXT("Cube"),Frame.Origin,Colors.Glow,true);
    auto* RoofGarden = Instances(this,City,true,Name + TEXT("RoofGardens"),TEXT("Sphere"),Frame.Origin,
        Biome == 4 ? FLinearColor(.43f,.19f,.61f) : FLinearColor(.18f,.35f,.2f));
    auto* Wood = Instances(this,City,true,Name + TEXT("WoodFurniture"),TEXT("Cube"),Frame.Origin,FLinearColor(.28f,.14f,.067f),false,false,true,1);
    auto* Metal = Instances(this,City,true,Name + TEXT("MetalFurniture"),TEXT("Cube"),Frame.Origin,FLinearColor(.10f,.14f,.16f),false,false,true,2);
    auto* Fabric = Instances(this,City,true,Name + TEXT("Upholstery"),TEXT("Cube"),Frame.Origin,FLinearColor(.16f,.30f,.30f),false,false,true,3);
    auto* Ceramic = Instances(this,City,true,Name + TEXT("CeramicFurniture"),TEXT("Cube"),Frame.Origin,FLinearColor(.76f,.74f,.66f),false,false,true,4);
    auto* Round = Instances(this,City,true,Name + TEXT("RoundFurniture"),TEXT("Cylinder"),Frame.Origin,FLinearColor(.23f,.16f,.09f),false,false,true,1);
    auto* Goods = Instances(this,City,true,Name + TEXT("Goods"),TEXT("Cube"),Frame.Origin,FLinearColor(.57f,.30f,.11f),false,false,false,0);
    auto* Screen = Instances(this,City,true,Name + TEXT("DisplayScreens"),TEXT("Cube"),Frame.Origin,FLinearColor(.06f,.21f,.24f),true);
    auto* LightPanels = Instances(this,City,true,Name + TEXT("RoomLightPanels"),TEXT("Cube"),Frame.Origin,FLinearColor(.38f,.30f,.20f),true);
    const TArray<FBuilding> Plan = BuildingPlan(BuiltSystem,Planet,Site);
    for (int32 BuildingIndex = 0; BuildingIndex < Plan.Num(); ++BuildingIndex)
    {
        const FBuilding& B = Plan[BuildingIndex];
        const FBuildingBase F = Foundation(Frame,B);
        const FVector Floor = F.Floor(), X = F.Rotation.GetAxisX(), Y = F.Rotation.GetAxisY();
        const int32 Floors = FMath::Clamp(int32(B.Height / 430),1,18);
        for (int32 Level = 1; Level < Floors; ++Level)
        {
            const double Height = 210 + Level * 430;
            const bool bUpper = Height > B.Height * .72;
            const double Width = B.Width * (bUpper ? .73 : 1), Depth = B.Depth * (bUpper ? .78 : 1);
            const FVector Center = Floor + F.Up * Height;
            Add(WindowBands,Center + X * (Width * .5 + 3),FVector(9,Depth * .7,28),F.Rotation);
            Add(WindowBands,Center - X * (Width * .5 + 3),FVector(9,Depth * .7,28),F.Rotation);
            Add(WindowBands,Center + Y * (Depth * .5 + 3),FVector(Width * .7,9,28),F.Rotation);
            Add(WindowBands,Center - Y * (Depth * .5 + 3),FVector(Width * .7,9,28),F.Rotation);
        }
        for (int32 Corner = 0; Corner < 4; ++Corner)
        {
            const FVector Offset = X * (Corner & 1 ? 1 : -1) * (B.Width * .5 + 8) + Y * (Corner & 2 ? 1 : -1) * (B.Depth * .5 + 8);
            Add(Frames,Floor + Offset + F.Up * B.Height * .36,FVector(55,55,B.Height * .72),F.Rotation);
        }
        // Mechanical rooftop boxes, ducts, landing markers and narrow entrance canopies.
        Add(Equipment,Floor + F.Up * (B.Height + 170),FVector(B.Width * .31,B.Depth * .26,290),F.Rotation);
        Add(Pipes,Floor + F.Up * (B.Height + 280) + X * B.Width * .22,FVector(145,145,450),F.Rotation);
        Add(Frames,Floor - Y * (B.Depth * .5 + 240) + F.Up * 365,FVector(820,700,36),F.Rotation);
        Add(Equipment,Floor - Y * (B.Depth * .5 + 42) + F.Up * 425,FVector(1200,35,95),F.Rotation);
        Add(LightPanels,Floor - Y * (B.Depth * .5 + 170) + F.Up * 339,FVector(300,35,8),F.Rotation);
        if (GetNetMode() != NM_DedicatedServer)
        {
            const FQuat TextRotation = Voyager::TangentRotation(F.Up,-Y).Quaternion();
            Label(this,City,Name + FString::Printf(TEXT("Address%d"),BuildingIndex),BuildingName(BuildingIndex),
                Floor - Y * (B.Depth * .5 + 64) + F.Up * 425,TextRotation,48,FColor(234,226,206));
            static const TCHAR* Services[] = {TEXT("COFFEE / A PLACE TO REST"),TEXT("MEDICAL ASSISTANCE"),TEXT("SUPPLIES / LOCAL TRADE"),
                TEXT("MAINTENANCE / ENGINEERING"),TEXT("WELCOME HOME, EXPLORER"),TEXT("OPERATIONS / COMMUNITY SAFETY")};
            Label(this,City,Name + FString::Printf(TEXT("RoomSign%d"),BuildingIndex),Services[BuildingIndex % 6],
                Floor + Y * (B.Depth * .5 - 32) + F.Up * 390,TextRotation,42,FColor(55,68,65));
        }
        auto Place = [&](UInstancedStaticMeshComponent* Mesh, double LX, double LY, double LZ, FVector Size)
        { Add(Mesh,Floor + X * LX + Y * LY + F.Up * LZ,Size,F.Rotation); };
        auto Chair = [&](double LX,double LY,bool bFaceBack)
        {
            Place(Fabric,LX,LY,48,FVector(64,64,16));
            Place(Fabric,LX,LY + (bFaceBack ? -27 : 27),83,FVector(64,12,60));
            for (int32 A : {-1,1}) for (int32 C : {-1,1})
                Place(Metal,LX + A * 24,LY + C * 24,21,FVector(6,6,42));
        };
        auto Desk = [&](double LX,double LY,double Width)
        {
            Place(Wood,LX,LY,86,FVector(Width,115,10));
            for (int32 Side : {-1,1}) Place(Metal,LX + Side * (Width * .5 - 12),LY,40,FVector(16,92,80));
            Place(Metal,LX,LY + 20,105,FVector(10,10,40));
            Place(Screen,LX,LY + 28,130,FVector(82,8,50));
            Place(Metal,LX,LY - 25,94,FVector(62,26,5));
        };
        auto Bed = [&](double LX,double LY,bool bMedical)
        {
            Place(bMedical ? Ceramic : Wood,LX,LY,38,FVector(135,245,28));
            Place(Fabric,LX,LY,59,FVector(128,238,22));
            Place(Ceramic,LX,LY + 85,76,FVector(105,50,17));
            for (int32 A : {-1,1}) for (int32 C : {-1,1})
                Place(Metal,LX + A * 53,LY + C * 100,17,FVector(12,12,34));
            if (bMedical)
            {
                Place(Metal,LX + 110,LY + 80,85,FVector(8,8,170));
                Place(Screen,LX + 110,LY + 80,166,FVector(55,10,38));
            }
        };
        const double SideX = B.Width * .28, RearY = B.Depth * .28;
        switch (BuildingIndex % 6)
        {
        case 0: // Cafe: working counter, individual tables, seating, crockery.
            Place(Wood,-B.Width * .5 + 170,0,53,FVector(190,B.Depth * .65,106));
            Place(Ceramic,-B.Width * .5 + 165,0,112,FVector(210,B.Depth * .65 + 20,12));
            for (int32 Row = 0; Row < 3; ++Row)
            {
                Place(Metal,-B.Width * .5 + 160,(Row - 1) * 300,149,FVector(85,90,65));
                for (int32 Side : {-1,1})
                {
                    const double TableX = Side * SideX * .65, TableY = (Row - 1) * 350;
                    Place(Round,TableX,TableY,78,FVector(135,135,10));
                    Place(Metal,TableX,TableY,38,FVector(12,12,76));
                    Place(Round,TableX,TableY,7,FVector(60,60,10));
                    Chair(TableX,TableY - 105,false); Chair(TableX,TableY + 105,true);
                    Place(Goods,TableX + 25,TableY,90,FVector(12,12,15));
                }
            }
            break;
        case 1: // Clinic: beds, diagnostics and a staffed reception desk.
            for (int32 Side : {-1,1}) for (int32 Row : {-1,1}) Bed(Side * SideX,Row * RearY,true);
            Desk(-SideX,0,260); Chair(-SideX,-100,false);
            Place(Ceramic,SideX,0,110,FVector(130,65,220));
            Place(Signs,SideX,-35,130,FVector(18,6,65)); Place(Signs,SideX,-35,130,FVector(65,6,18));
            break;
        case 2: // Market: shelving leaves a generous central shopping aisle.
            for (int32 Side : {-1,1})
            {
                const double ShelfX = Side * (B.Width * .5 - 125);
                Place(Wood,ShelfX,0,135,FVector(140,30,270));
                for (int32 Level = 0; Level < 4; ++Level)
                {
                    Place(Wood,ShelfX,0,35 + Level * 62,FVector(145,B.Depth * .65,8));
                    for (int32 Item = 0; Item < 9; ++Item)
                        Place(Goods,ShelfX,(Item - 4) * B.Depth * .065,61 + Level * 62,FVector(65,90,43));
                }
                for (int32 End : {-1,1}) Place(Metal,ShelfX,End * B.Depth * .325,132,FVector(145,12,264));
            }
            Desk(-SideX,-RearY,300); Chair(-SideX,-RearY - 100,false);
            break;
        case 3: // Workshop: heavy benches, tools, portable machinery and crates.
            for (int32 Side : {-1,1})
            {
                Desk(Side * SideX,RearY,380);
                Place(Metal,Side * SideX,-RearY,70,FVector(260,160,140));
                Place(Round,Side * SideX,-RearY,170,FVector(115,115,70));
                for (int32 Crate = 0; Crate < 3; ++Crate)
                    Place(Wood,Side * (B.Width * .5 - 130),-RearY + Crate * 145,55,FVector(145,125,110));
                Place(Metal,Side * SideX,RearY + 75,195,FVector(380,18,170));
                for (int32 Tool = 0; Tool < 6; ++Tool)
                {
                    Place(Ceramic,Side * SideX + (Tool - 2.5) * 52,RearY + 61,185,FVector(9,9,75));
                    Place(Ceramic,Side * SideX + (Tool - 2.5) * 52,RearY + 61,222,FVector(36,9,12));
                }
            }
            break;
        case 4: // Habitation: furnished sleeping alcoves and a shared lounge.
            for (int32 Side : {-1,1})
            {
                Bed(Side * SideX,RearY,false);
                Place(Wood,Side * SideX + Side * 150,RearY + 35,49,FVector(100,100,98));
                Place(Ceramic,Side * SideX + Side * 150,RearY + 35,116,FVector(48,48,35));
                Place(Fabric,Side * SideX,-RearY,55,FVector(220,95,40));
                Place(Fabric,Side * SideX,-RearY + 45,98,FVector(220,20,90));
                for (int32 Arm : {-1,1}) Place(Fabric,Side * SideX + Arm * 105,-RearY,80,FVector(25,105,85));
                Place(Wood,Side * SideX,-RearY - 145,34,FVector(190,90,10));
                Place(Metal,Side * SideX,-RearY - 145,15,FVector(150,55,30));
            }
            Desk(-SideX,0,220);
            break;
        case 5: // Security: dispatch consoles, briefing bench and equipment lockers.
            for (int32 Side : {-1,1})
            {
                Desk(Side * SideX,0,310); Chair(Side * SideX,-110,false);
                for (int32 Locker = 0; Locker < 4; ++Locker)
                {
                    const double LX = Side * SideX + (Locker - 1.5) * 95;
                    Place(Metal,LX,RearY,135,FVector(88,85,270));
                    Place(Goods,LX + 24,RearY - 45,140,FVector(6,8,26));
                }
                Place(Fabric,Side * SideX,-RearY,52,FVector(300,85,16));
                Place(Metal,Side * SideX,-RearY,24,FVector(260,50,48));
            }
            break;
        }
        for (int32 Side : {-1,1})
        {
            Place(LightPanels,Side * B.Width * .24,0,RoomHeight - 23,FVector(210,B.Depth * .58,6));
            Place(Round,Side * (B.Width * .5 - 125),-B.Depth * .5 + 190,40,FVector(130,130,80));
            Add(RoofGarden,Floor + X * Side * (B.Width * .5 - 125) - Y * (B.Depth * .5 - 190) + F.Up * 140,
                FVector(140,120,180),F.Rotation);
        }
        if ((Biome == 0 || Biome == 4) && B.Style == 2)
            for (int32 I = 0; I < 3; ++I)
                Add(RoofGarden,Floor + F.Up * (B.Height + 130) + X * (I - 1) * 410,FVector(440,420,240),F.Rotation);
    }
    for (int32 XIndex = 0; XIndex <= 8; ++XIndex)
        for (int32 YIndex = 0; YIndex <= 8; ++YIndex)
        {
            const double X = (XIndex - 4) * PlotSpacing + 540, Y = (YIndex - 4) * PlotSpacing + 540;
            const FVector Up = Frame.Direction(X,Y);
            const FQuat Rotation = Frame.Rotation(X,Y);
            Add(Pipes,Frame.Ground(X,Y,350),FVector(28,28,700),Rotation);
            Add(Lamps,Frame.Ground(X,Y,725),FVector(100,100,65),Rotation);
            City.StreetLightPositions.Add(Frame.Ground(X,Y,725));
            if (XIndex == 4 || YIndex == 4)
                Add(Signs,Frame.Ground(X,Y,470),FVector(12,300,105),Rotation);
        }
    for (int32 I = 0; I < 24; ++I)
    {
        const double A = I * UE_TWO_PI / 24.0;
        const double X = FMath::Cos(A) * 2440, Y = FMath::Sin(A) * 2440;
        Add(Signs,Frame.Ground(X,Y,31),FVector(280,30,5),Frame.Rotation(X,Y) * FRotator(0,float(FMath::RadiansToDegrees(A) + 90),0).Quaternion());
    }
    // Large H and approach chevrons identify a usable central landing plaza from flight.
    Add(Markings,Frame.Ground(-700,0,33),FVector(150,2000,4),Frame.Rotation(-700,0));
    Add(Markings,Frame.Ground(700,0,33),FVector(150,2000,4),Frame.Rotation(700,0));
    Add(Markings,Frame.Ground(0,0,34),FVector(1400,150,4),Frame.Rotation(0,0));
    for (int32 I = 0; I < 9; ++I)
    {
        const double Y = -15750 + I * 1800;
        GroundStrip(Markings,Frame,0,Y,420,45,21,true);
    }
    for (int32 I = 0; I < (GetNetMode() == NM_DedicatedServer ? 0 : 8); ++I)
    {
        auto* Light = NewObject<UPointLightComponent>(this,FName(Name + FString::Printf(TEXT("InteriorLight%d"),I)));
        AddInstanceComponent(Light);
        Light->SetupAttachment(GetRootComponent());
        Light->SetMobility(EComponentMobility::Movable);
        Light->SetRelativeLocation(Frame.Ground(0,0,350));
        Light->SetLightColor(FLinearColor(1.f,.78f,.53f));
        Light->SetIntensity(32000.f);
        Light->SetAttenuationRadius(2100.f);
        Light->SetCastShadows(false);
        Light->SetVisibility(false);
        Light->RegisterComponent();
        City.Details.Add(Light);
        City.InteriorLights.Add(Light);
    }
    for (int32 I = 0; I < (GetNetMode() == NM_DedicatedServer ? 0 : 4); ++I)
    {
        auto* Light = NewObject<UPointLightComponent>(this,FName(Name + FString::Printf(TEXT("StreetLight%d"),I)));
        AddInstanceComponent(Light);
        Light->SetupAttachment(GetRootComponent());
        Light->SetMobility(EComponentMobility::Movable);
        Light->SetRelativeLocation(Frame.Ground(0,0,725));
        Light->SetLightColor(FLinearColor(1.f,.88f,.72f));
        Light->SetIntensity(60000.f);
        Light->SetAttenuationRadius(2800.f);
        Light->SetCastShadows(false);
        Light->SetVisibility(false);
        Light->RegisterComponent();
        City.Details.Add(Light);
        City.StreetLights.Add(Light);
    }
}
