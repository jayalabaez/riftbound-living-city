// The city, drawn.
//
// One actor renders the whole of LIVING CITY: the street grid, every building, and every
// citizen the simulation publishes. It is pure presentation. Rule R2 is absolute here -
// this actor READS simulation state and DRAWS it. It never decides anything, never moves a
// citizen, never picks where a building goes. If a line in the .cpp starts to look like a
// gameplay decision, it belongs in LivingCitySim instead.
//
// UNITS. The simulation works in METRES, as integers, measured from the city centre. Unreal
// works in CENTIMETRES, as floats. The multiply by 100 happens in exactly one place - the
// small conversion helpers at the top of the .cpp - and nowhere else.
//
// PLACEMENT. Every instance is added in COMPONENT space, so the whole city is drawn relative
// to wherever this actor sits. The game mode spawns it at the identity transform, which is
// what makes sim metres line up with world centimetres and with the terrain the
// ALivingCityTerrainView draws in world space. Spawn it somewhere else and the city moves
// while the ground does not - so don't.
//
// GROUND. Buildings and citizens take their Z from ALivingCityTerrainView::HeightAtCm(), the
// terrain view's own cached copy of the sim heightfield. The world view never reads terrain
// memory itself; if there is no terrain view in the world, everything sits at Z = 0.
//
// ---------------------------------------------------------------------------------------
// WHAT THIS NEEDS FROM ULivingCitySubsystem
// ---------------------------------------------------------------------------------------
// This file was written against exactly two subsystem entry points, both of which exist on
// ULivingCitySubsystem today:
//
//     const lc::Simulation* ULivingCitySubsystem::GetSimulation() const;
//     FLivingCitySimStatus  ULivingCitySubsystem::GetStatus() const;   // for .Alpha
//
// GetSimulation() gives the city layout, which is generated once at world seed time and is
// immutable thereafter, so it can be read straight off the sim object rather than pushed
// through a snapshot every tick. It is CONST: we look, we never touch.
//
// GetStatus().Alpha gives the interpolation fraction between the two most recent published
// snapshots, exactly as ALivingCityPlaceholder uses it. That call currently copies a whole
// snapshot AND heap-allocates an FString for the world hash, every frame, which is the one
// known per-frame allocation left on this path; the fix is a lightweight
// `float ULivingCitySubsystem::GetInterpolationAlpha() const` accessor - see the note in
// UpdateCitizens(). Nothing else here needs adding to the subsystem.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

// Pulls in lc::Simulation, lc::City and lc::Snapshot through the one sanctioned door.
#include "LivingCitySimBridge.h"

#include "LivingCityWorldView.generated.h"

class ALivingCityTerrainView;
class UInstancedStaticMeshComponent;
class ULivingCitySubsystem;

/**
 * Renders the generated city and its population entirely from simulation state.
 *
 * Static geometry (roads, buildings) is instanced ONCE when the city first becomes
 * available. Citizens are updated every frame from the snapshot pair, interpolated so the
 * 20 Hz simulation reads as smooth motion at 60 FPS.
 */
UCLASS()
class LIVINGCITYGAME_API ALivingCityWorldView : public AActor
{
    GENERATED_BODY()

public:
    ALivingCityWorldView();

    virtual void Tick(float DeltaSeconds) override;

protected:
    virtual void BeginPlay() override;

private:
    // ---- one-time city build ---------------------------------------------------------
    /** Returns true once the static city geometry has been instanced. */
    bool TryBuildCity();
    void BuildRoads(const lc::City& City);
    void BuildBuildings(const lc::City& City);

    /** Dresses the buildings in Riftbound's architecture material, or a flat tint if it will not load. */
    void TintBuildings();

    /** Finds the terrain view, if the game mode spawned one. Called once, from BeginPlay. */
    void FindTerrainView();

    /** Ground height under a world XY from the terrain view's cache, or 0 without one. */
    double GroundHeightAt(double XCm, double YCm) const;

    /**
     * Road centrelines, read out of the city rather than assumed.
     *
     * The renderer must not hard-code the generator's grid convention - that is the
     * generator's business, and it will change. City::NearestRoadX/Y is the public answer to
     * "where is the nearest road", so sweeping it across the city extent recovers the exact
     * set of centrelines whatever layout the generator picked.
     */
    static void CollectRoadCentrelines(const lc::City& City, TArray<int32>& OutX, TArray<int32>& OutY);

    // ---- per-frame citizen update ----------------------------------------------------
    void UpdateCitizens(const ULivingCitySubsystem& Subsystem);

    /** Applies per-instance colour data for citizens whose activity changed since last frame. */
    void RefreshCitizenActivityData(const lc::Snapshot& Latest, int32 Count);

    // ---- components ------------------------------------------------------------------
    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<USceneComponent> SceneRoot;

    /** Dark slabs laid along every road corridor, so the streets read as streets. */
    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<UInstancedStaticMeshComponent> RoadsISM;

    /** One box per Building, scaled to its real footprint and height. */
    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<UInstancedStaticMeshComponent> BuildingsISM;

    /**
     * One upright cylinder per snapshot citizen, moved every frame.
     *
     * A PLAIN ISM, NOT A HISM, AND THAT IS DELIBERATE - see the long comment in the
     * constructor before changing it back.
     */
    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<UInstancedStaticMeshComponent> CitizensISM;

    /**
     * The same population again in a second tint, one instance per citizen, index-aligned
     * with CitizensISM. A citizen is visible in exactly one of the two: whichever matches
     * their activity. See the comment in UpdateCitizens() for why a second component is the
     * only engine-only way to give shoppers their own colour.
     */
    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<UInstancedStaticMeshComponent> ShoppersISM;

    /** The ground. Found once in BeginPlay; null means everything sits on Z = 0. */
    UPROPERTY()
    TObjectPtr<ALivingCityTerrainView> TerrainView;

    // ---- citizen scratch, allocated once ---------------------------------------------
    // Sized to lc::kMaxSnapshotCitizens on the first update and never grown again, so the
    // per-frame path does no allocation. Rule I5 is a simulation invariant, but the view has
    // a 2.5 ms budget of its own (CLAUDE.md section 4) and churning 1,500 transforms through
    // the allocator every frame would eat it.
    TArray<FTransform> CitizenTransforms;

    /** Shopper-side transforms; only needed in full when the instance lists are (re)built. */
    TArray<FTransform> ShopperTransforms;

    /** Last activity value pushed to each instance, so unchanged citizens cost nothing. */
    TArray<uint8> CitizenActivityCache;

    /** The snapshot pair we are interpolating between; kept so a failed acquire can reuse it. */
    lc::Snapshot PrevSnapshot;
    lc::Snapshot CurrSnapshot;
    bool         bHasSnapshotPair = false;

    /** Cached instance fit for the citizen mesh: scale, plus the pivot correction. */
    FVector CitizenScale       = FVector::OneVector;
    FVector CitizenPivotOffset = FVector::ZeroVector;

    /** True once roads and buildings have been instanced. */
    bool bCityBuilt = false;

    /** Frames spent waiting for the terrain view to finish its first build. */
    int32 FramesWaitedForTerrain = 0;

    // Two distinct "we cannot draw a city" conditions, each latched separately so that one
    // never silences the diagnostic for the other. A single shared flag meant whichever
    // fired first hid the one that mattered.
    bool bWarnedNoSimulation = false;
    bool bWarnedEmptyCity    = false;
    bool bWarnedTerrainWait  = false;
};
