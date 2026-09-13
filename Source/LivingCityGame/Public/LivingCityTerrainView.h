// The ground, drawn from the simulation's heightfield.
//
// One UProceduralMeshComponent per terrain chunk, each with collision, so the player stands
// on what the simulation says is there. Rule R2 is absolute here: this actor READS the
// heightfield and draws it. It never changes a height. The player's shovel goes
//
//     ALivingCityPlayerCharacter -> ULivingCitySubsystem::ModifyTerrain -> Command
//         -> sim thread -> Terrain::Modify -> Snapshot::terrainVersion -> this actor redraws
//
// and that round trip is the architecture, not an inefficiency: it is what makes a shovel
// stroke exactly as replayable as a keypress (invariant I1).
//
// THE VIEW'S OWN COPY. Every height this actor uses - for vertices, for collision, and for the
// HeightAtCm() query the other views call - comes from a private copy filled only through
// Terrain::CopyChunkHeights, the one terrain read the render thread is allowed. Nothing here
// touches sim memory without that lock, and other views must go through HeightAtCm() rather
// than reaching for the terrain themselves.
//
// UNITS. The terrain already speaks centimetres (Terrain::kCellSizeCm), so unlike the city
// there is no metre conversion in this file. Heights are integers on the sim side and become
// floats only as they are written into vertices.
//
// CHUNK LAYOUT, CONFIRMED AGAINST Terrain.cpp. A chunk covers kChunkCells x kChunkCells cells
// and CopyChunkHeights hands back (kChunkCells+1)^2 values, row-major with X fastest - the
// extra row and column being the first row and column of the next chunk (or the field's last
// row and column repeated, at its edge). Heights are the height at a cell's CENTRE and
// OriginXCm/OriginYCm is the corner of cell (0,0), so the vertex for cell (cx, cy) sits at
// origin + (cx + 0.5, cy + 0.5) cells.
//
// THE SHARED EDGE IS NEVER READ FROM A CHUNK'S OWN COPY. The sim bumps only the versions of
// chunks whose cells moved, so a brush that changes chunk (kx+1, ky)'s first column without
// touching chunk (kx, ky) leaves the extra column in chunk (kx, ky)'s copy stale - and a mesh
// built from that copy would crack away from its neighbour by the height of the stroke. Every
// vertex height, edge vertices included, is therefore looked up by CELL through
// CachedCellHeight(), which always lands in the chunk that owns the cell: the one whose
// version moved and whose copy is fresh. Two neighbours are then built from identical numbers
// by construction, and a copy's extra row and column are never used for a position.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

// Pulls in lc::Simulation and lc::Snapshot through the one sanctioned door.
#include "LivingCitySimBridge.h"

// For FProcMeshTangent, held by value in the geometry scratch below. The component could stay
// forward-declared; the tangent struct cannot, so the plugin header comes in here.
#include "ProceduralMeshComponent.h"

#include "LivingCityTerrainView.generated.h"

class ULivingCitySubsystem;
class UMaterialInstanceDynamic;

namespace lc { class Terrain; }

UCLASS()
class LIVINGCITYGAME_API ALivingCityTerrainView : public AActor
{
    GENERATED_BODY()

public:
    ALivingCityTerrainView();

    virtual void Tick(float DeltaSeconds) override;

    /** True once every chunk has been meshed at least once. HeightAtCm() is meaningful only then. */
    bool IsReady() const { return bBuilt; }

    /**
     * Ground height under a world XY, bilinear over this view's own cached copy of the
     * heightfield. Clamped to the field's edge outside it, so a caller never gets garbage.
     * Returns 0 before the terrain has been built.
     */
    float HeightAtCm(float XCm, float YCm) const;

protected:
    virtual void BeginPlay() override;

private:
    // ---- build ----------------------------------------------------------------------------
    /** Returns true once every chunk has a mesh. Retried each frame until the sim has a terrain. */
    bool TryBuild();
    void LoadGroundMaterials();

    /** Points every chunk at whichever ground material the lc.Terrain.VoyagerMaterial cvar asks for. */
    void ApplyGroundMaterial();

    // ---- change tracking ------------------------------------------------------------------
    /** Reads the newest snapshot's terrainVersion and, if it moved, recopies the chunks that changed. */
    void PollForChanges(const ULivingCitySubsystem& Subsystem);

    /** Remeshes up to a fixed number of dirty chunks so a big stroke never spikes one frame. */
    void RemeshDirtyChunks();

    // ---- per-chunk work -------------------------------------------------------------------
    void CopyChunkFromSim(const lc::Terrain& Terrain, int32 Chunk);
    void MeshChunk(int32 Chunk, bool bCreate);
    void MarkDirtyWithNeighbours(const FIntPoint& ChunkCoord);

    /** A cell's height from the cached copy, clamped to the field so a neighbour off the edge reads the edge. */
    int32 CachedCellHeight(int32 CellX, int32 CellY) const;

    // ---- components -----------------------------------------------------------------------
    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<USceneComponent> SceneRoot;

    /** One mesh per chunk, indexed by the simulation's chunk index. */
    UPROPERTY()
    TArray<TObjectPtr<UProceduralMeshComponent>> ChunkMeshes;

    /** Riftbound's ground material, borrowed read-only. Null if it did not load. */
    UPROPERTY()
    TObjectPtr<UMaterialInstanceDynamic> VoyagerGroundMaterial;

    /** The engine basic-shape material in an earth colour: the fallback, and the cvar alternative. */
    UPROPERTY()
    TObjectPtr<UMaterialInstanceDynamic> FlatGroundMaterial;

    // ---- the view's copy of the heightfield --------------------------------------------
    /** (kChunkCells+1)^2 heights per chunk, chunk-major. Written ONLY by CopyChunkFromSim. */
    TArray<int32> Heights;

    /** Terrain::ChunkVersion as of the last copy, per chunk. */
    TArray<uint32> ChunkVersions;

    /** Simulation chunk index for (ChunkY * ChunksX + ChunkX), read from Terrain::ChunkIndex once. */
    TArray<int32> ChunkIndexLookup;

    /** The inverse: chunk grid coordinate for each simulation chunk index. */
    TArray<FIntPoint> ChunkCoords;

    /** Chunks whose mesh is behind the cache. Persists across frames so remeshing can be amortised. */
    TArray<uint8> DirtyChunks;
    int32         PendingDirtyCount = 0;

    /** Newest snapshot, kept as a member so the per-frame acquire does not zero 20 KB first. */
    lc::Snapshot LatestSnapshot;

    // ---- geometry scratch, sized once ---------------------------------------------------
    // One chunk's worth of vertices, reused for every chunk built or rebuilt. The triangle
    // list is identical for every chunk, so it is filled exactly once.
    TArray<FVector>          ScratchVertices;
    TArray<FVector>          ScratchNormals;
    TArray<FVector2D>        ScratchUVs;
    TArray<FProcMeshTangent> ScratchTangents;
    TArray<int32>            ChunkTriangles;

    // ---- field geometry, cached from the terrain at build ------------------------------
    int32 CellsX    = 0;
    int32 CellsY    = 0;
    int32 ChunksX   = 0;
    int32 ChunksY   = 0;
    int32 OriginXCm = 0;
    int32 OriginYCm = 0;

    /** Snapshot terrainVersion as of the last chunk-version scan. */
    uint64 SeenTerrainVersion = 0;

    /** Which ground material the chunks carry right now: 1 Voyager, 0 flat, -1 none yet. */
    int32 AppliedGroundMaterial = -1;

    bool bBuilt              = false;
    bool bWarnedNoSimulation = false;
    bool bWarnedNoTerrain    = false;
};
