#include "LivingCityWorldView.h"

#include "LivingCitySubsystem.h"
#include "LivingCityTerrainView.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogLivingCityView, Log, All);

namespace
{
    // ------------------------------------------------------------------ the unit boundary
    // The simulation is metres, Unreal is centimetres. This constant is the ONLY place that
    // conversion is expressed in this module; everything below goes through these helpers.
    constexpr double kCmPerMetre = 100.0;

    FORCEINLINE double MetresToCm(int32 Metres)
    {
        return static_cast<double>(Metres) * kCmPerMetre;
    }

    // ------------------------------------------------------------------ presentation tuning
    // Road slabs sit a few centimetres proud of the ground, so the two nearly coplanar
    // surfaces cannot z-fight. The two axes are given DIFFERENT heights for the same reason:
    // north-south and east-west corridors overlap at every intersection, and two coplanar
    // slabs there would shimmer. A 2 cm step is invisible from eye height, and the slabs have
    // no collision, so nothing can trip over it. The city stands on the terrain's flat
    // region, which the generator holds at height 0, so the slabs are laid at Z = 0 and not
    // draped: a road is a slab, not a ribbon, until the road-network pass replaces it.
    constexpr double kRoadThicknessCm     = 20.0;
    constexpr double kRoadTopNorthSouthCm = 4.0;
    constexpr double kRoadTopEastWestCm   = 6.0;

    /** Citizens stand on top of the road surface rather than sinking into it. */
    constexpr double kCitizenGroundZCm = 8.0;

    /** A person: 45 cm across, 1.8 m tall. */
    constexpr double kCitizenDiameterCm = 45.0;
    constexpr double kCitizenHeightCm   = 180.0;

    /**
     * Scale of an instance that must not be seen. A citizen lives in both citizen components
     * but is visible in exactly one; the other copy is collapsed to under two millimetres at
     * the same spot rather than moved away, so the component bounds stay tight and no matrix
     * is ever singular.
     */
    constexpr double kHiddenScale = 0.001;

    /**
     * Sentinel for "this instance has never been given a colour".
     *
     * lc::Activity has six values, so 0xFF can never collide with a real one and every
     * instance is guaranteed to take the write on the first pass after a rebuild.
     */
    constexpr uint8 kUnknownActivity = 0xFFu;

    /** Whether an activity byte from the snapshot means the citizen is out on an errand. */
    FORCEINLINE bool IsShopperActivity(uint8 Activity)
    {
        return Activity == static_cast<uint8>(lc::Activity::TravelShop) ||
               Activity == static_cast<uint8>(lc::Activity::AtShop);
    }

    /**
     * Places a box mesh so that it exactly fills SizeCm, with its BASE on FootprintCentreCm.Z
     * and its footprint centred on FootprintCentreCm.XY.
     *
     * The mesh's own bounds are read rather than assumed. /Engine/BasicShapes meshes happen to
     * be 100 cm primitives centred on the origin today, but a renderer that bakes that in
     * breaks silently the day someone swaps the mesh - and "the city is 100x too tall" is a
     * much worse bug to diagnose than a wrong number here.
     */
    FTransform MakeBoxInstance(const FBox& LocalBox, const FVector& SizeCm, const FVector& FootprintCentreCm)
    {
        constexpr double kEpsilon = 1.0e-4;

        const FVector LocalSize   = LocalBox.GetSize();
        const FVector LocalCentre = LocalBox.GetCenter();

        const FVector Scale(
            LocalSize.X > kEpsilon ? SizeCm.X / LocalSize.X : 1.0,
            LocalSize.Y > kEpsilon ? SizeCm.Y / LocalSize.Y : 1.0,
            LocalSize.Z > kEpsilon ? SizeCm.Z / LocalSize.Z : 1.0);

        const FVector Translation(
            FootprintCentreCm.X - Scale.X * LocalCentre.X,
            FootprintCentreCm.Y - Scale.Y * LocalCentre.Y,
            FootprintCentreCm.Z - Scale.Z * LocalBox.Min.Z);

        return FTransform(FQuat::Identity, Translation, Scale);
    }

    /**
     * Gives a component a flat colour off the engine's basic-shape material.
     *
     * A per-COMPONENT colour, so everything in one component is one shade. The per-instance
     * palette and activity values are still written into custom data floats below; the
     * moment a material that samples PerInstanceCustomData exists, point these components at
     * it and the city colours itself with no code change here. Rule R6 keeps us from checking
     * in a .uasset for it.
     */
    void TintComponent(UMeshComponent* Component, UObject* Outer, const FLinearColor& Colour)
    {
        if (!Component)
        {
            return;
        }

        UMaterialInterface* Base = LoadObject<UMaterialInterface>(
            nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
        if (!Base)
        {
            return;
        }

        UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Base, Outer);
        if (!Instance)
        {
            return;
        }

        Instance->SetVectorParameterValue(TEXT("Color"), Colour);
        Component->SetMaterial(0, Instance);
    }
}

// ============================================================ construction

ALivingCityWorldView::ALivingCityWorldView()
{
    PrimaryActorTick.bCanEverTick = true;

    // Tick after the world subsystem has refreshed its interpolation alpha for this frame,
    // exactly as ALivingCityPlaceholder does - and after the terrain view (TG_PrePhysics) has
    // refreshed the heights the citizens stand on.
    PrimaryActorTick.TickGroup = TG_PostPhysics;

    SceneRoot     = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    RootComponent = SceneRoot;

    // Static, so the finder runs once during CDO construction rather than on every spawn.
    static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
        TEXT("/Engine/BasicShapes/Cube.Cube"));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(
        TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));

    // Every component stays Movable. The project runs with r.AllowStaticLighting=False, so
    // Static mobility buys nothing here and only invites runtime warnings about touching
    // static primitives.

    // ---- roads ---------------------------------------------------------------------
    RoadsISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Roads"));
    RoadsISM->SetupAttachment(SceneRoot);
    if (CubeMesh.Succeeded())
    {
        RoadsISM->SetStaticMesh(CubeMesh.Object);
    }
    // The terrain is what the player actually walks on; these are paint. Collision here
    // would put a 4 cm kerb around every block for no benefit.
    RoadsISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    RoadsISM->SetCastShadow(false);
    RoadsISM->SetCanEverAffectNavigation(false);

    // ---- buildings -----------------------------------------------------------------
    BuildingsISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Buildings"));
    BuildingsISM->SetupAttachment(SceneRoot);
    if (CubeMesh.Succeeded())
    {
        BuildingsISM->SetStaticMesh(CubeMesh.Object);
    }
    // The profile carries its own ECollisionEnabled, so this one call is the whole story -
    // a SetCollisionEnabled before it would be silently overwritten by the profile anyway.
    BuildingsISM->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
    BuildingsISM->SetCastShadow(true);
    BuildingsISM->SetCanEverAffectNavigation(false);

    // ---- citizens ------------------------------------------------------------------
    //
    // THIS IS A PLAIN ISM ON PURPOSE. DO NOT "UPGRADE" IT TO A HISM.
    //
    // A HierarchicalISM is the wrong container for instances that move every frame, and in
    // UE 5.8 it is actively pathological. The chain, read out of the engine source:
    //
    //   UHierarchicalInstancedStaticMeshComponent::BatchUpdateInstancesTransforms is not
    //   batched at all - BatchUpdateInstancesTransformsInternal
    //   (HierarchicalInstancedStaticMesh.cpp) is a bare loop over UpdateInstanceTransform.
    //   The HISM override of UpdateInstanceTransform takes the "not an in-place update"
    //   branch for every instance whose LOCATION changed, and that branch does two things
    //   per instance: it appends to UnbuiltInstanceBoundsList, and it calls
    //   BuildTreeIfOutdated(Async=true).
    //
    //   So a 1,500-strong population that is actually walking around costs 1,500 array
    //   appends and 1,500 tree-dirty calls per frame; the first of those kicks off
    //   BuildTreeAsync, and each of the other 1,499 sets bConcurrentChanges, which throws
    //   the in-flight cluster-tree build away and starts another. The result is a worker
    //   thread rebuilding a 1,500-leaf tree continuously, forever, and an unbuilt-bounds
    //   array growing by 1,500 entries a frame between completed builds.
    //
    // A plain UInstancedStaticMeshComponent has no cluster tree to invalidate. Since 5.4 it
    // gets per-instance culling from GPU Scene regardless, and its
    // BatchUpdateInstancesTransformsInternal is a single loop that writes the matrix and
    // calls PrimitiveInstanceDataManager.TransformChanged - which streams just the changed
    // instances. The only thing given up is cluster-tree LOD dithering across the crowd,
    // which is worth nothing for 1,500 untextured cylinders.
    CitizensISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Citizens"));
    CitizensISM->SetupAttachment(SceneRoot);
    if (CylinderMesh.Succeeded())
    {
        CitizensISM->SetStaticMesh(CylinderMesh.Object);
    }
    CitizensISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    // 1,500 movable shadow casters is not a frame budget we have (CLAUDE.md section 4 gives
    // the whole view sync 2.5 ms). Buildings cast the shadows that matter.
    CitizensISM->SetCastShadow(false);

    // Every citizen moves every frame; a navigation dirty per instance per frame would cost
    // more than the drawing does.
    CitizensISM->SetCanEverAffectNavigation(false);

    // ---- shoppers ------------------------------------------------------------------
    // Everything above applies. Same mesh, same rules, different tint; see UpdateCitizens().
    ShoppersISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Shoppers"));
    ShoppersISM->SetupAttachment(SceneRoot);
    if (CylinderMesh.Succeeded())
    {
        ShoppersISM->SetStaticMesh(CylinderMesh.Object);
    }
    ShoppersISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    ShoppersISM->SetCastShadow(false);
    ShoppersISM->SetCanEverAffectNavigation(false);
}

// ============================================================ startup

void ALivingCityWorldView::BeginPlay()
{
    Super::BeginPlay();

    // Black-top streets, high-visibility people, shoppers in a second colour so an errand
    // reads as an errand from across the street. Buildings get Riftbound's architecture
    // material if it is there, and a flat sandstone if not.
    TintComponent(RoadsISM, this, FLinearColor(0.030f, 0.030f, 0.035f));
    TintComponent(CitizensISM, this, FLinearColor(0.950f, 0.320f, 0.100f));
    TintComponent(ShoppersISM, this, FLinearColor(0.120f, 0.820f, 0.920f));
    TintBuildings();

    // Work out, once, how to fit the citizen mesh to a 1.8 m person.
    if (CitizensISM && CitizensISM->GetStaticMesh())
    {
        const FTransform Fit = MakeBoxInstance(
            CitizensISM->GetStaticMesh()->GetBoundingBox(),
            FVector(kCitizenDiameterCm, kCitizenDiameterCm, kCitizenHeightCm),
            FVector::ZeroVector);

        CitizenScale       = Fit.GetScale3D();
        CitizenPivotOffset = Fit.GetTranslation();
    }

    // Reserved up front so the per-frame path never touches the allocator.
    CitizenTransforms.Reserve(static_cast<int32>(lc::kMaxSnapshotCitizens));
    ShopperTransforms.Reserve(static_cast<int32>(lc::kMaxSnapshotCitizens));
    CitizenActivityCache.Reserve(static_cast<int32>(lc::kMaxSnapshotCitizens));

    FindTerrainView();
    TryBuildCity();
}

void ALivingCityWorldView::TintBuildings()
{
    if (!BuildingsISM)
    {
        return;
    }

    // Riftbound's architecture material, borrowed read-only: a world-space window pattern
    // with a glow term, driven by a Tint parameter. Same custom-data floats as before are
    // still written per instance, so a future material can read them without touching this.
    if (UMaterialInterface* Architecture = LoadObject<UMaterialInterface>(
            nullptr, TEXT("/Game/Materials/M_VoyagerArchitecture.M_VoyagerArchitecture")))
    {
        if (UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Architecture, this))
        {
            Instance->SetVectorParameterValue(TEXT("Tint"), FLinearColor(0.520f, 0.450f, 0.360f));
            Instance->SetScalarParameterValue(TEXT("WindowGlow"), 0.42f);
            BuildingsISM->SetMaterial(0, Instance);
            return;
        }
    }

    UE_LOG(LogLivingCityView, Warning,
           TEXT("M_VoyagerArchitecture did not load; buildings will use a flat sandstone tint."));
    TintComponent(BuildingsISM, this, FLinearColor(0.520f, 0.450f, 0.360f));
}

void ALivingCityWorldView::FindTerrainView()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    for (TActorIterator<ALivingCityTerrainView> It(World); It; ++It)
    {
        TerrainView = *It;
        return;
    }

    UE_LOG(LogLivingCityView, Log,
           TEXT("No terrain view in the world; buildings and citizens will sit on Z = 0."));
}

double ALivingCityWorldView::GroundHeightAt(double XCm, double YCm) const
{
    if (TerrainView && TerrainView->IsReady())
    {
        return static_cast<double>(TerrainView->HeightAtCm(static_cast<float>(XCm), static_cast<float>(YCm)));
    }
    return 0.0;
}

bool ALivingCityWorldView::TryBuildCity()
{
    if (bCityBuilt)
    {
        return true;
    }

    const UWorld* World = GetWorld();
    if (!World)
    {
        return false;
    }

    const ULivingCitySubsystem* Subsystem = World->GetSubsystem<ULivingCitySubsystem>();
    const lc::Simulation* Sim = Subsystem ? Subsystem->GetSimulation() : nullptr;
    if (!Sim)
    {
        if (!bWarnedNoSimulation)
        {
            bWarnedNoSimulation = true;
            UE_LOG(LogLivingCityView, Warning,
                   TEXT("No simulation available yet; the city will be built as soon as there is one."));
        }
        return false;
    }

    const lc::City& City = Sim->CityLayout();
    if (City.Count() == 0)
    {
        if (!bWarnedEmptyCity)
        {
            bWarnedEmptyCity = true;
            UE_LOG(LogLivingCityView, Warning,
                   TEXT("The simulation generated no buildings; there is nothing to draw."));
        }
        return false;
    }

    // Buildings take their base from the terrain, so the terrain has to exist first. In the
    // normal spawn order it already does; this is for the abnormal one. A bounded wait, so
    // a terrain view that can never build (no terrain in the sim) still leaves us with a
    // city rather than nothing.
    constexpr int32 kMaxFramesToWaitForTerrain = 300;
    if (TerrainView && !TerrainView->IsReady())
    {
        if (++FramesWaitedForTerrain <= kMaxFramesToWaitForTerrain)
        {
            return false;
        }
        if (!bWarnedTerrainWait)
        {
            bWarnedTerrainWait = true;
            UE_LOG(LogLivingCityView, Warning,
                   TEXT("Terrain view never became ready; building the city on Z = 0."));
        }
    }

    BuildRoads(City);
    BuildBuildings(City);
    bCityBuilt = true;

    UE_LOG(LogLivingCityView, Log,
           TEXT("City view built: %d buildings across %d x %d blocks, %d m by %d m."),
           static_cast<int32>(City.Count()), City.BlocksX(), City.BlocksY(),
           City.ExtentX() * 2, City.ExtentY() * 2);
    return true;
}

void ALivingCityWorldView::CollectRoadCentrelines(const lc::City& City, TArray<int32>& OutX, TArray<int32>& OutY)
{
    OutX.Reset();
    OutY.Reset();

    const int32 ExtentX = City.ExtentX();
    const int32 ExtentY = City.ExtentY();
    if (ExtentX <= 0 || ExtentY <= 0)
    {
        return;
    }

    // One sweep per axis at metre resolution. This runs once, at startup, and it means the
    // view never has to know how the generator lays its grid out.
    //
    // The `Centre != Last` filter is what keeps that affordable. NearestRoad* is a step
    // function, so consecutive samples repeat the same centreline for a whole stride;
    // without the filter every one of them pays a linear AddUnique scan, which at the
    // generator's 4096-block ceiling is ~295,000 samples times ~4,000 comparisons - a
    // multi-second stall inside BeginPlay. Skipping the repeats leaves one AddUnique per
    // distinct corridor. AddUnique is still what enforces uniqueness, so the filter is a
    // pure cost saving and cannot introduce a duplicate slab even if the step function
    // were not monotonic.
    int32 LastX = 0;
    for (int32 X = -ExtentX; X <= ExtentX; ++X)
    {
        const int32 Centre = City.NearestRoadX(X);
        if (X != -ExtentX && Centre == LastX)
        {
            continue;
        }
        LastX = Centre;

        if (Centre >= -ExtentX && Centre <= ExtentX)
        {
            OutX.AddUnique(Centre);
        }
    }

    int32 LastY = 0;
    for (int32 Y = -ExtentY; Y <= ExtentY; ++Y)
    {
        const int32 Centre = City.NearestRoadY(Y);
        if (Y != -ExtentY && Centre == LastY)
        {
            continue;
        }
        LastY = Centre;

        if (Centre >= -ExtentY && Centre <= ExtentY)
        {
            OutY.AddUnique(Centre);
        }
    }

    OutX.Sort();
    OutY.Sort();
}

void ALivingCityWorldView::BuildRoads(const lc::City& City)
{
    if (!RoadsISM || !RoadsISM->GetStaticMesh())
    {
        return;
    }

    TArray<int32> RoadX;
    TArray<int32> RoadY;
    CollectRoadCentrelines(City, RoadX, RoadY);

    if (RoadX.Num() == 0 && RoadY.Num() == 0)
    {
        UE_LOG(LogLivingCityView, Warning,
               TEXT("Found no road centrelines in the generated city; drawing buildings only."));
        return;
    }

    const int32  RoadWidthM  = lc::City::kRoadWidth;
    const double RoadWidthCm = static_cast<double>(RoadWidthM) * kCmPerMetre;

    // Each corridor is drawn full length so the intersections are covered; the extra road
    // width on the span stops the outermost junctions from being clipped.
    const double SpanXCm = (static_cast<double>(City.ExtentX()) * 2.0 + static_cast<double>(RoadWidthM)) * kCmPerMetre;
    const double SpanYCm = (static_cast<double>(City.ExtentY()) * 2.0 + static_cast<double>(RoadWidthM)) * kCmPerMetre;

    const FBox LocalBox = RoadsISM->GetStaticMesh()->GetBoundingBox();

    TArray<FTransform> Slabs;
    Slabs.Reserve(RoadX.Num() + RoadY.Num());

    // North-south corridors: narrow in X, full length in Y.
    for (const int32 X : RoadX)
    {
        Slabs.Add(MakeBoxInstance(
            LocalBox,
            FVector(RoadWidthCm, SpanYCm, kRoadThicknessCm),
            FVector(MetresToCm(X), 0.0, kRoadTopNorthSouthCm - kRoadThicknessCm)));
    }

    // East-west corridors: full length in X, narrow in Y.
    for (const int32 Y : RoadY)
    {
        Slabs.Add(MakeBoxInstance(
            LocalBox,
            FVector(SpanXCm, RoadWidthCm, kRoadThicknessCm),
            FVector(0.0, MetresToCm(Y), kRoadTopEastWestCm - kRoadThicknessCm)));
    }

    RoadsISM->PreAllocateInstancesMemory(Slabs.Num());
    RoadsISM->AddInstances(Slabs, /*bShouldReturnIndices*/ false,
                           /*bWorldSpace*/ false, /*bUpdateNavigation*/ false);
}

void ALivingCityWorldView::BuildBuildings(const lc::City& City)
{
    if (!BuildingsISM || !BuildingsISM->GetStaticMesh())
    {
        return;
    }

    const uint32 Count = City.Count();
    if (Count == 0)
    {
        return;
    }

    const FBox LocalBox = BuildingsISM->GetStaticMesh()->GetBoundingBox();

    TArray<FTransform> Boxes;
    Boxes.Reserve(static_cast<int32>(Count));

    for (uint32 Index = 0; Index < Count; ++Index)
    {
        const lc::Building& Lot = City.At(Index);

        // A degenerate footprint is the generator's business, not ours; clamp so a zero does
        // not become a zero-scale instance the renderer cannot draw at all.
        const double SizeX = FMath::Max(static_cast<double>(Lot.halfX) * 2.0, 1.0) * kCmPerMetre;
        const double SizeY = FMath::Max(static_cast<double>(Lot.halfY) * 2.0, 1.0) * kCmPerMetre;
        const double SizeZ = FMath::Max(static_cast<double>(Lot.height), 1.0) * kCmPerMetre;

        const double CentreXCm = MetresToCm(Lot.centreX);
        const double CentreYCm = MetresToCm(Lot.centreY);

        // Base on the ground under the centre of the footprint. The city stands on the
        // terrain's flat region, so today this is 0 everywhere; it is read rather than
        // assumed so the day the generator puts a block on a hill, the block sits on it.
        Boxes.Add(MakeBoxInstance(
            LocalBox,
            FVector(SizeX, SizeY, SizeZ),
            FVector(CentreXCm, CentreYCm, GroundHeightAt(CentreXCm, CentreYCm))));
    }

    // Custom data 0 = art-direction palette index, 1 = what the building is for. Nothing
    // reads these yet (see TintBuildings) but they cost one float each and mean the
    // material, when it exists, needs no code change here.
    BuildingsISM->SetNumCustomDataFloats(2);
    BuildingsISM->PreAllocateInstancesMemory(Boxes.Num());
    BuildingsISM->AddInstances(Boxes, /*bShouldReturnIndices*/ false,
                               /*bWorldSpace*/ false, /*bUpdateNavigation*/ false);

    constexpr float kPaletteScale = 1.0f / 7.0f;   // Building::palette is documented 0..7
    constexpr float kUseScale =
        1.0f / static_cast<float>(static_cast<int32>(lc::LotUse::Count) - 1);

    for (uint32 Index = 0; Index < Count; ++Index)
    {
        const lc::Building& Lot      = City.At(Index);
        const int32         Instance = static_cast<int32>(Index);

        BuildingsISM->SetCustomDataValue(Instance, 0,
                                         static_cast<float>(Lot.palette) * kPaletteScale, false);
        BuildingsISM->SetCustomDataValue(Instance, 1,
                                         static_cast<float>(static_cast<uint8>(Lot.use)) * kUseScale, false);
    }

    BuildingsISM->MarkRenderInstancesDirty();
}

// ============================================================ per-frame

void ALivingCityWorldView::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    const UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    const ULivingCitySubsystem* Subsystem = World->GetSubsystem<ULivingCitySubsystem>();
    if (!Subsystem)
    {
        return;
    }

    if (!bCityBuilt && !TryBuildCity())
    {
        return;
    }

    UpdateCitizens(*Subsystem);
}

void ALivingCityWorldView::UpdateCitizens(const ULivingCitySubsystem& Subsystem)
{
    if (!CitizensISM || !CitizensISM->GetStaticMesh() || !ShoppersISM || !ShoppersISM->GetStaticMesh())
    {
        return;
    }

    const lc::Simulation* Sim = Subsystem.GetSimulation();
    if (!Sim)
    {
        return;
    }

    // SnapshotRing leaves both arguments untouched when it cannot hand over a stable pair, so
    // reading straight into the members is safe: on a miss we simply keep interpolating the
    // pair we already had, which is what the ring's own documentation recommends.
    if (Sim->Snapshots().AcquireLatestPair(PrevSnapshot, CurrSnapshot))
    {
        bHasSnapshotPair = true;
    }
    if (!bHasSnapshotPair)
    {
        return;
    }

    // The subsystem owns the clock that produces this; the view must never derive its own, or
    // the citizens and the placeholder would drift apart on a hitching frame.
    //
    // NOTE: GetStatus() copies a whole snapshot AND builds an FString of the world hash, so
    // this one line is a ~20 KB copy plus a heap allocation every frame. It is the last
    // per-frame allocation on this path. The fix is a `float GetInterpolationAlpha() const`
    // on ULivingCitySubsystem returning the value it already computes in Tick() - deliberately
    // not added here, because that file belongs to another agent. Swap this one call site
    // when it lands.
    const double Alpha = FMath::Clamp(static_cast<double>(Subsystem.GetStatus().Alpha), 0.0, 1.0);

    // Only citizens present in BOTH snapshots can be interpolated. A citizen index is stable
    // in the simulation's struct-of-arrays, so index i is the same person in each snapshot.
    const uint32 PairCount = FMath::Min(PrevSnapshot.citizenCount, CurrSnapshot.citizenCount);
    const int32  Count     = static_cast<int32>(
        FMath::Min(PairCount, static_cast<uint32>(lc::kMaxSnapshotCitizens)));

    if (CitizenTransforms.Num() != Count)
    {
        CitizenTransforms.SetNum(Count, EAllowShrinking::No);
    }

    // The population is fixed after generation, so the rebuild branch runs exactly once.
    // Rebuilding the instance lists is the expensive path and it must never become the
    // per-frame one. Both components are rebuilt together so their indices stay aligned.
    const bool bRebuild = CitizensISM->GetInstanceCount() != Count || ShoppersISM->GetInstanceCount() != Count;
    if (bRebuild && ShopperTransforms.Num() != Count)
    {
        ShopperTransforms.SetNum(Count, EAllowShrinking::No);
    }

    // Ground heights come from the terrain view's cache, hoisted out of the loop so the null
    // and readiness checks are paid once, not 1,500 times.
    const ALivingCityTerrainView* Ground = (TerrainView && TerrainView->IsReady()) ? TerrainView.Get() : nullptr;

    // ---- why two components -----------------------------------------------------------
    // The activity value is already written to per-instance custom data, but nothing in the
    // engine's stock materials reads it, and rule R6 keeps a material asset of our own out
    // of the repo. A per-instance TINT with engine-only assets therefore needs a second
    // component with a second flat colour. Every citizen has an instance in both; the copy
    // that does not match their activity is collapsed to kHiddenScale in place. The citizen
    // component is rewritten in one batch every frame as before. The shopper component is
    // touched only for citizens who ARE shopping (they move) and citizens who just STOPPED
    // (their shopper copy has to vanish) - a few dozen writes a frame, not 1,500.
    bool bShopperTouched = false;
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const double FromX = static_cast<double>(PrevSnapshot.citizenX[Index]);
        const double FromY = static_cast<double>(PrevSnapshot.citizenY[Index]);
        const double ToX   = static_cast<double>(CurrSnapshot.citizenX[Index]);
        const double ToY   = static_cast<double>(CurrSnapshot.citizenY[Index]);

        // Metres -> centimetres happens here and nowhere else on this path.
        const double X = (FromX + (ToX - FromX) * Alpha) * kCmPerMetre;
        const double Y = (FromY + (ToY - FromY) * Alpha) * kCmPerMetre;

        // A stable per-citizen offset around their exact simulated position.
        //
        // WHY THIS IS A RENDERING CONCERN AND NOT A SIMULATION ONE: the simulation is right
        // that fourteen people who live in the same building all stand at the same doorway.
        // Drawn literally, those fourteen capsules occupy one coordinate and read as a single
        // person - which is exactly what the first build looked like. The scatter is how a
        // crowd is DRAWN, not where it IS, so it belongs here. The sim position stays exact
        // and the invariant tests that pin arrival to a precise coordinate keep their meaning.
        //
        // Derived from the index so it never changes between frames - a citizen shuffling
        // around their doorway every tick would be far worse than the stack.
        const uint32 Scatter = static_cast<uint32>(Index) * 2654435761u;
        const double AngleRad = static_cast<double>(Scatter >> 16) * (6.2831853071795864 / 65536.0);
        const double Radius   = 60.0 + static_cast<double>((Scatter >> 8) & 0xFFu) * (240.0 / 255.0);
        const double OffX     = FMath::Cos(AngleRad) * Radius;
        const double OffY     = FMath::Sin(AngleRad) * Radius;

        const double DrawX   = X + OffX;
        const double DrawY   = Y + OffY;
        const double GroundZ = Ground
            ? static_cast<double>(Ground->HeightAtCm(static_cast<float>(DrawX), static_cast<float>(DrawY)))
            : 0.0;

        const FVector Location(DrawX + CitizenPivotOffset.X,
                               DrawY + CitizenPivotOffset.Y,
                               GroundZ + kCitizenGroundZCm + CitizenPivotOffset.Z);

        const uint8 State     = CurrSnapshot.citizenState[Index];
        const bool  bShopping = IsShopperActivity(State);

        // Indoors is not drawn. A citizen AtWork (2) or AtShop (5) is inside a building; the
        // simulation places them at the kerb outside it because that is the road-grid point
        // their walk ends on, not because they are standing there. Drawing all 1,500 of them
        // at ~35 workplace kerbs produced a solid wall of capsules at the centre intersection.
        // AtHome stays visible: 73 home kerbs spread the population thinly enough to read as
        // people around their houses, and it keeps residential blocks looking lived in.
        const bool  bIndoors  = (State == 2u) || (State == 5u);

        const FTransform Shown (FQuat::Identity, Location, CitizenScale);
        const FTransform Hidden(FQuat::Identity, Location, CitizenScale * kHiddenScale);

        CitizenTransforms[Index] = (bShopping || bIndoors) ? Hidden : Shown;

        if (bRebuild)
        {
            ShopperTransforms[Index] = (bShopping && !bIndoors) ? Shown : Hidden;
        }
        else if (bShopping)
        {
            ShoppersISM->UpdateInstanceTransform(Index, Shown, /*bWorldSpace*/ false,
                                                 /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
            bShopperTouched = true;
        }
        else if (CitizenActivityCache.IsValidIndex(Index) && IsShopperActivity(CitizenActivityCache[Index]))
        {
            // Was shopping last frame, is not now: the shopper copy has to disappear. The
            // cache still holds last frame's activity because it is refreshed at the end.
            ShoppersISM->UpdateInstanceTransform(Index, Hidden, /*bWorldSpace*/ false,
                                                 /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
            bShopperTouched = true;
        }
    }

    if (bRebuild)
    {
        CitizensISM->ClearInstances();
        ShoppersISM->ClearInstances();
        CitizenActivityCache.Reset();

        if (Count > 0)
        {
            CitizensISM->SetNumCustomDataFloats(1);
            CitizensISM->PreAllocateInstancesMemory(Count);
            CitizensISM->AddInstances(CitizenTransforms, /*bShouldReturnIndices*/ false,
                                      /*bWorldSpace*/ false, /*bUpdateNavigation*/ false);

            ShoppersISM->PreAllocateInstancesMemory(Count);
            ShoppersISM->AddInstances(ShopperTransforms, /*bShouldReturnIndices*/ false,
                                      /*bWorldSpace*/ false, /*bUpdateNavigation*/ false);

            // kUnknownActivity is not a valid Activity, so every instance gets its colour on
            // this pass.
            CitizenActivityCache.Init(kUnknownActivity, Count);
        }
    }
    else if (Count > 0)
    {
        // The steady-state path: rewrite every transform in one batched call with the render
        // state left alone, then raise the cheap instance-dirty flag once.
        //
        // NOT MarkRenderStateDirty(). In UE 5.4+ an instanced component streams changed
        // instances to the GPU scene through FInstanceDataManager; MarkRenderStateDirty()
        // would instead destroy and rebuild the entire scene proxy - every frame, for 1,500
        // instances. MarkRenderInstancesDirty() is the flag that matches what actually
        // changed, it is one bool plus an end-of-frame-update request, and it is idempotent.
        // BatchUpdateInstancesTransforms already raises it internally on the frame's first
        // changed instance (TransformChanged -> MarkComponentRenderInstancesDirty), so this
        // call is belt and braces for the case where instance tracking is disabled - not the
        // mechanism the update relies on.
        CitizensISM->BatchUpdateInstancesTransforms(0, CitizenTransforms,
                                                    /*bWorldSpace*/ false,
                                                    /*bMarkRenderStateDirty*/ false,
                                                    /*bTeleport*/ true);
        CitizensISM->MarkRenderInstancesDirty();

        // Only when a shopper instance was actually written this frame. The flag is cheap,
        // but it schedules an end-of-frame instance update for the component, and on a
        // frame where nobody is shopping that is a scheduled update with nothing in it.
        if (bShopperTouched)
        {
            ShoppersISM->MarkRenderInstancesDirty();
        }
    }

    RefreshCitizenActivityData(CurrSnapshot, Count);
}

void ALivingCityWorldView::RefreshCitizenActivityData(const lc::Snapshot& Latest, int32 Count)
{
    if (!CitizensISM || CitizenActivityCache.Num() < Count)
    {
        return;
    }

    // Six activities (AtHome .. AtShop) spread over 0..1, so TravelShop and AtShop land at
    // 0.8 and 1.0 - distinct from every commuting value the moment a material reads this.
    constexpr float kActivityScale =
        1.0f / static_cast<float>(static_cast<int32>(lc::Activity::Count) - 1);

    // A citizen's activity changes a handful of times a day, so almost every instance skips
    // out here. Writing all of them every frame would push a full custom-data upload for no
    // visible difference. SetCustomDataValue raises the instance-dirty flag itself, so the
    // false argument (do not recreate the render state) is the whole story.
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const uint8 Activity = Latest.citizenState[Index];
        if (Activity == CitizenActivityCache[Index])
        {
            continue;
        }

        CitizenActivityCache[Index] = Activity;
        CitizensISM->SetCustomDataValue(Index, 0,
                                        static_cast<float>(Activity) * kActivityScale, false);
    }
}
