#include "LivingCityTerrainView.h"

#include "LivingCitySubsystem.h"

#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"

// The bridge is the sanctioned door to sim types, and Simulation::TerrainField() returns a
// Terrain held by value, so Terrain.h reaches this file through Sim.h in practice. It is
// included again here, under the same warning guards the bridge uses, so this file cannot
// break if the bridge ever moves to a forward declaration. A no-op when already included.
THIRD_PARTY_INCLUDES_START
#include "livingcity/world/Terrain.h"
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogLivingCityTerrain, Log, All);

namespace
{
    constexpr int32 kChunkCells      = lc::Terrain::kChunkCells;          // 16 cells a side
    constexpr int32 kVertsPerSide    = kChunkCells + 1;                     // 17: shared edge
    constexpr int32 kVertsPerChunk   = kVertsPerSide * kVertsPerSide;       // 289
    constexpr int32 kIndicesPerChunk = kChunkCells * kChunkCells * 6;       // 1536
    constexpr double kCellSizeCm     = static_cast<double>(lc::Terrain::kCellSizeCm);

    /**
     * Chunk meshes rebuilt per frame, at most. A brush stroke dirties the chunks it touched
     * plus their neighbours (see MarkDirtyWithNeighbours), so a stroke on a chunk corner can
     * dirty sixteen. Each rebuild is a 289-vertex upload plus a synchronous 512-triangle
     * collision cook; eight of those sit inside the 2 ms "chunk mesh apply" line of the frame
     * budget (CLAUDE.md section 4), and anything left over simply lands next frame.
     */
    constexpr int32 kMaxRemeshesPerFrame = 8;

    TAutoConsoleVariable<int32> CVarVoyagerGround(
        TEXT("lc.Terrain.VoyagerMaterial"),
        1,
        TEXT("1: dress the terrain in Riftbound's M_VoyagerTerrain (read-only reuse of an existing asset). ")
        TEXT("0: the engine basic-shape material in an earth tone. ")
        TEXT("Switchable at runtime, so the Voyager material can be dropped the moment it looks wrong on a flat world."),
        ECVF_Default);

    FORCEINLINE int32 VertexIndex(int32 LocalX, int32 LocalY)
    {
        return LocalY * kVertsPerSide + LocalX;
    }
}

// ============================================================ construction

ALivingCityTerrainView::ALivingCityTerrainView()
{
    PrimaryActorTick.bCanEverTick = true;

    // Before physics, and before the world view (TG_PostPhysics) reads HeightAtCm() for the
    // citizens: the cache must be current before anything is placed on it this frame.
    PrimaryActorTick.TickGroup = TG_PrePhysics;

    SceneRoot     = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    RootComponent = SceneRoot;
}

void ALivingCityTerrainView::BeginPlay()
{
    Super::BeginPlay();

    LoadGroundMaterials();
    TryBuild();
}

// ============================================================ materials

void ALivingCityTerrainView::LoadGroundMaterials()
{
    // Riftbound's ground material, borrowed read-only. It was written for a sphere: it takes a
    // planet centre and radius and derives altitude and slope from them. A flat world hands
    // it a planet so large that its surface is flat to within a few centimetres across the
    // whole city - centre 2,000 km straight down. Float precision at that distance is about
    // 16 cm, which is invisible in a colour ramp.
    if (UMaterialInterface* Voyager = LoadObject<UMaterialInterface>(
            nullptr, TEXT("/Game/Materials/M_VoyagerTerrain.M_VoyagerTerrain")))
    {
        VoyagerGroundMaterial = UMaterialInstanceDynamic::Create(Voyager, this);
        if (VoyagerGroundMaterial)
        {
            constexpr float kPlanetRadiusCm = 2.0e8f;
            VoyagerGroundMaterial->SetVectorParameterValue(TEXT("Tint"),         FLinearColor(0.30f, 0.34f, 0.19f));
            VoyagerGroundMaterial->SetVectorParameterValue(TEXT("LandTint"),     FLinearColor(0.46f, 0.50f, 0.27f));
            VoyagerGroundMaterial->SetVectorParameterValue(TEXT("SnowTint"),     FLinearColor(0.90f, 0.92f, 0.95f));
            VoyagerGroundMaterial->SetScalarParameterValue(TEXT("SnowCoverage"), 0.0f);
            VoyagerGroundMaterial->SetVectorParameterValue(TEXT("PlanetCenter"), FLinearColor(0.0f, 0.0f, -kPlanetRadiusCm, 1.0f));
            VoyagerGroundMaterial->SetScalarParameterValue(TEXT("PlanetRadius"), kPlanetRadiusCm);
            VoyagerGroundMaterial->SetVectorParameterValue(TEXT("SeedOffset"),   FLinearColor(0.37f, 0.61f, 0.23f));
        }
    }

    if (!VoyagerGroundMaterial)
    {
        UE_LOG(LogLivingCityTerrain, Warning,
               TEXT("M_VoyagerTerrain did not load; the ground will use the engine basic-shape material."));
    }

    // The fallback is always prepared, so the cvar can switch to it without a load hitch.
    if (UMaterialInterface* Basic = LoadObject<UMaterialInterface>(
            nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
    {
        FlatGroundMaterial = UMaterialInstanceDynamic::Create(Basic, this);
        if (FlatGroundMaterial)
        {
            FlatGroundMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.34f, 0.38f, 0.22f));
        }
    }
}

void ALivingCityTerrainView::ApplyGroundMaterial()
{
    const bool  bWantVoyager = CVarVoyagerGround.GetValueOnGameThread() != 0 && VoyagerGroundMaterial != nullptr;
    const int32 Wanted       = bWantVoyager ? 1 : 0;
    if (Wanted == AppliedGroundMaterial)
    {
        return;
    }
    AppliedGroundMaterial = Wanted;

    UMaterialInterface* Material = bWantVoyager ? VoyagerGroundMaterial.Get() : FlatGroundMaterial.Get();
    if (!Material)
    {
        // Neither loaded: the chunks keep the component default, which at least draws.
        return;
    }

    for (const TObjectPtr<UProceduralMeshComponent>& Mesh : ChunkMeshes)
    {
        if (Mesh)
        {
            Mesh->SetMaterial(0, Material);
        }
    }
}

// ============================================================ build

bool ALivingCityTerrainView::TryBuild()
{
    if (bBuilt)
    {
        return true;
    }

    const UWorld* World = GetWorld();
    if (!World)
    {
        return false;
    }

    const ULivingCitySubsystem* Subsystem = World->GetSubsystem<ULivingCitySubsystem>();
    const lc::Simulation*       Sim       = Subsystem ? Subsystem->GetSimulation() : nullptr;
    if (!Sim)
    {
        if (!bWarnedNoSimulation)
        {
            bWarnedNoSimulation = true;
            UE_LOG(LogLivingCityTerrain, Warning,
                   TEXT("No simulation available yet; the terrain will be built as soon as there is one."));
        }
        return false;
    }

    const lc::Terrain& Terrain    = Sim->TerrainField();
    const int32        ChunkCount = static_cast<int32>(Terrain.ChunkCount());
    if (ChunkCount <= 0 || Terrain.CellsX() <= 0 || Terrain.CellsY() <= 0 ||
        Terrain.ChunksX() <= 0 || Terrain.ChunksY() <= 0)
    {
        if (!bWarnedNoTerrain)
        {
            bWarnedNoTerrain = true;
            UE_LOG(LogLivingCityTerrain, Warning,
                   TEXT("The simulation has no terrain; there is no ground to draw."));
        }
        return false;
    }

    const double StartSeconds = FPlatformTime::Seconds();

    CellsX    = Terrain.CellsX();
    CellsY    = Terrain.CellsY();
    ChunksX   = Terrain.ChunksX();
    ChunksY   = Terrain.ChunksY();
    OriginXCm = Terrain.OriginXCm();
    OriginYCm = Terrain.OriginYCm();

    // The chunk grid -> chunk index mapping is the terrain's to define, so it is read out of
    // Terrain::ChunkIndex once rather than assumed. Both directions are cached.
    ChunkIndexLookup.SetNumUninitialized(ChunksX * ChunksY);
    ChunkCoords.Init(FIntPoint::ZeroValue, ChunkCount);
    for (int32 ChunkY = 0; ChunkY < ChunksY; ++ChunkY)
    {
        for (int32 ChunkX = 0; ChunkX < ChunksX; ++ChunkX)
        {
            const int32 Index = static_cast<int32>(Terrain.ChunkIndex(ChunkX, ChunkY));
            checkf(Index >= 0 && Index < ChunkCount,
                   TEXT("Terrain::ChunkIndex(%d, %d) returned %d, outside %d chunks"),
                   ChunkX, ChunkY, Index, ChunkCount);
            ChunkIndexLookup[ChunkY * ChunksX + ChunkX] = Index;
            ChunkCoords[Index]                          = FIntPoint(ChunkX, ChunkY);
        }
    }

    Heights.SetNumZeroed(ChunkCount * kVertsPerChunk);
    ChunkVersions.SetNumZeroed(ChunkCount);
    DirtyChunks.SetNumZeroed(ChunkCount);
    PendingDirtyCount = 0;

    ScratchVertices.SetNumUninitialized(kVertsPerChunk);
    ScratchNormals.SetNumUninitialized(kVertsPerChunk);
    ScratchUVs.SetNumUninitialized(kVertsPerChunk);
    ScratchTangents.SetNum(kVertsPerChunk);   // has a constructor, so not Uninitialized

    // The index buffer is the same for every chunk. Winding follows the engine's own
    // GenerateBoxMesh: a front face is one whose (v1 - v0) x (v2 - v0) points AWAY from the
    // viewer in Unreal's left-handed frame, so for a +Z-facing quad
    //     A = (x, y)   B = (x+1, y)   C = (x+1, y+1)   D = (x, y+1)
    // the triangles are (A, D, B) and (D, C, B).
    ChunkTriangles.Reset(kIndicesPerChunk);
    for (int32 LocalY = 0; LocalY < kChunkCells; ++LocalY)
    {
        for (int32 LocalX = 0; LocalX < kChunkCells; ++LocalX)
        {
            const int32 A = VertexIndex(LocalX,     LocalY);
            const int32 B = VertexIndex(LocalX + 1, LocalY);
            const int32 C = VertexIndex(LocalX + 1, LocalY + 1);
            const int32 D = VertexIndex(LocalX,     LocalY + 1);

            ChunkTriangles.Add(A);
            ChunkTriangles.Add(D);
            ChunkTriangles.Add(B);

            ChunkTriangles.Add(D);
            ChunkTriangles.Add(C);
            ChunkTriangles.Add(B);
        }
    }

    // ---- one component per chunk --------------------------------------------------------
    // Each component sits at its chunk's world corner and its vertices are local, so the
    // per-chunk bounds stay tight for culling and a rebuild never touches the transform.
    ChunkMeshes.SetNum(ChunkCount);
    for (int32 ChunkY = 0; ChunkY < ChunksY; ++ChunkY)
    {
        for (int32 ChunkX = 0; ChunkX < ChunksX; ++ChunkX)
        {
            const int32 Index = ChunkIndexLookup[ChunkY * ChunksX + ChunkX];

            UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(
                this, FName(*FString::Printf(TEXT("TerrainChunk_%d_%d"), ChunkX, ChunkY)));

            Mesh->SetupAttachment(SceneRoot);
            Mesh->SetRelativeLocation(FVector(
                static_cast<double>(OriginXCm) + static_cast<double>(ChunkX * kChunkCells) * kCellSizeCm,
                static_cast<double>(OriginYCm) + static_cast<double>(ChunkY * kChunkCells) * kCellSizeCm,
                0.0));

            // Synchronous cooking on purpose. Collision has to exist the same frame the mesh
            // does - the player is standing on it - and a 512-triangle cook is far cheaper
            // than a character falling through a hill for two frames.
            Mesh->bUseAsyncCooking            = false;
            Mesh->bUseComplexAsSimpleCollision = true;
            Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
            Mesh->SetCastShadow(true);
            Mesh->SetCanEverAffectNavigation(false);
            Mesh->RegisterComponent();

            ChunkMeshes[Index] = Mesh;
        }
    }

    // ---- fill --------------------------------------------------------------------------
    // Copy every chunk first, then mesh every chunk: normals read across chunk seams, so all
    // of the cache has to be current before any of it is turned into vertices.
    for (int32 Chunk = 0; Chunk < ChunkCount; ++Chunk)
    {
        CopyChunkFromSim(Terrain, Chunk);
    }
    for (int32 Chunk = 0; Chunk < ChunkCount; ++Chunk)
    {
        MeshChunk(Chunk, /*bCreate*/ true);
    }

    bBuilt = true;
    ApplyGroundMaterial();

    UE_LOG(LogLivingCityTerrain, Log,
           TEXT("Terrain view built: %d chunks (%d x %d cells of %d cm) in %.1f ms."),
           ChunkCount, CellsX, CellsY, lc::Terrain::kCellSizeCm,
           (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
    return true;
}

// ============================================================ per-chunk work

void ALivingCityTerrainView::CopyChunkFromSim(const lc::Terrain& Terrain, int32 Chunk)
{
    // Version BEFORE heights. A brush that lands between the two reads then shows up as a
    // stale version on the next scan and the chunk is copied again - rather than a fresh
    // version quietly certifying stale heights, which would never be corrected.
    //
    // The copy's 17th row and column (the neighbour's first) are landed here but never read
    // back for a vertex: MeshChunk looks every height up by cell, so a neighbour that changed
    // on its own is read from ITS fresh copy, not from this chunk's stale edge. See the header.
    ChunkVersions[Chunk] = Terrain.ChunkVersion(static_cast<lc::u32>(Chunk));
    Terrain.CopyChunkHeights(static_cast<lc::u32>(Chunk), &Heights[Chunk * kVertsPerChunk]);
}

int32 ALivingCityTerrainView::CachedCellHeight(int32 CellX, int32 CellY) const
{
    const int32 ClampedX = FMath::Clamp(CellX, 0, CellsX - 1);
    const int32 ClampedY = FMath::Clamp(CellY, 0, CellsY - 1);

    // A partial last chunk (field not a multiple of kChunkCells) still lands inside the chunk
    // grid because each chunk's copy carries the extra row and column, so the local index may
    // legitimately reach kChunkCells.
    const int32 ChunkX = FMath::Min(ClampedX / kChunkCells, ChunksX - 1);
    const int32 ChunkY = FMath::Min(ClampedY / kChunkCells, ChunksY - 1);
    const int32 LocalX = FMath::Min(ClampedX - ChunkX * kChunkCells, kChunkCells);
    const int32 LocalY = FMath::Min(ClampedY - ChunkY * kChunkCells, kChunkCells);

    const int32 Chunk = ChunkIndexLookup[ChunkY * ChunksX + ChunkX];
    return Heights[Chunk * kVertsPerChunk + VertexIndex(LocalX, LocalY)];
}

void ALivingCityTerrainView::MeshChunk(int32 Chunk, bool bCreate)
{
    UProceduralMeshComponent* Mesh = ChunkMeshes.IsValidIndex(Chunk) ? ChunkMeshes[Chunk].Get() : nullptr;
    if (!Mesh)
    {
        return;
    }

    const FIntPoint Coord     = ChunkCoords[Chunk];
    const int32     CellBaseX = Coord.X * kChunkCells;
    const int32     CellBaseY = Coord.Y * kChunkCells;

    for (int32 LocalY = 0; LocalY < kVertsPerSide; ++LocalY)
    {
        for (int32 LocalX = 0; LocalX < kVertsPerSide; ++LocalX)
        {
            const int32 Vertex = VertexIndex(LocalX, LocalY);
            const int32 CellX  = CellBaseX + LocalX;
            const int32 CellY  = CellBaseY + LocalY;

            // Position by CELL, through the cache, never from this chunk's own copy. The
            // vertex at LocalX == kChunkCells is the neighbour's first column; reading it
            // from the neighbour's copy is what keeps the seam closed when only the
            // neighbour changed (see the header). At the field's edge the lookup clamps to
            // the last cell, which is exactly the value the sim's copy repeats there.
            ScratchVertices[Vertex] = FVector(
                (static_cast<double>(LocalX) + 0.5) * kCellSizeCm,
                (static_cast<double>(LocalY) + 0.5) * kCellSizeCm,
                static_cast<double>(CachedCellHeight(CellX, CellY)));

            // Normal from central differences over the whole cache, so shading is continuous
            // across chunk seams rather than kinked at every sixteenth cell.
            const double DzDx = static_cast<double>(CachedCellHeight(CellX + 1, CellY) - CachedCellHeight(CellX - 1, CellY))
                                / (2.0 * kCellSizeCm);
            const double DzDy = static_cast<double>(CachedCellHeight(CellX, CellY + 1) - CachedCellHeight(CellX, CellY - 1))
                                / (2.0 * kCellSizeCm);
            ScratchNormals[Vertex] = FVector(-DzDx, -DzDy, 1.0).GetSafeNormal();

            // The surface tangent along +X, which is perpendicular to that normal whatever
            // the slope. The component does NOT derive this: with no tangents it stores a
            // constant (1, 0, 0) per vertex (FProcMeshTangent's default), which is only
            // right on level ground - on a dug slope a normal-mapped ground material would
            // shade against a frame tilted by the slope angle. bFlipTangentY is false, so
            // the bitangent (normal x tangent) points +Y, the same convention as the
            // default, with UV.v increasing along +Y.
            ScratchTangents[Vertex] = FProcMeshTangent(FVector(1.0, 0.0, DzDx).GetSafeNormal(), false);

            // One tile per metre, continuous across the whole field.
            ScratchUVs[Vertex] = FVector2D(
                static_cast<double>(CellX) * kCellSizeCm / 100.0,
                static_cast<double>(CellY) * kCellSizeCm / 100.0);
        }
    }

    // An empty array allocates nothing. No vertex colours: the materials do not read them.
    const TArray<FLinearColor> NoColors;

    if (bCreate)
    {
        Mesh->CreateMeshSection_LinearColor(0, ScratchVertices, ChunkTriangles, ScratchNormals, ScratchUVs,
                                            NoColors, ScratchTangents, /*bCreateCollision*/ true);
    }
    else
    {
        // Same topology, new heights: the cheap path. It refreshes collision too - in Chaos
        // that is a deep copy of the chunk's 512-triangle mesh with the vertices swapped and
        // its BVH rebuilt (FBodyInstance::UpdateTriMeshVertices), not a full re-cook.
        Mesh->UpdateMeshSection_LinearColor(0, ScratchVertices, ScratchNormals, ScratchUVs,
                                            NoColors, ScratchTangents);
    }
}

void ALivingCityTerrainView::MarkDirtyWithNeighbours(const FIntPoint& ChunkCoord)
{
    // A chunk's mesh includes the first row and column of the chunks at +X and +Y (so the
    // chunks at -X, -Y and -X-Y draw THIS chunk's edge cells and must be rebuilt when it
    // moves), and every chunk's normals read one cell into every neighbour. Rather than
    // depend on exactly which chunk versions the sim bumps for a cell on a boundary, dirty
    // the whole 3x3 neighbourhood: at most nine 289-vertex rebuilds, and because MeshChunk
    // reads every height by cell from the cache, no seam can survive a stroke.
    for (int32 DeltaY = -1; DeltaY <= 1; ++DeltaY)
    {
        for (int32 DeltaX = -1; DeltaX <= 1; ++DeltaX)
        {
            const int32 ChunkX = ChunkCoord.X + DeltaX;
            const int32 ChunkY = ChunkCoord.Y + DeltaY;
            if (ChunkX < 0 || ChunkX >= ChunksX || ChunkY < 0 || ChunkY >= ChunksY)
            {
                continue;
            }

            const int32 Chunk = ChunkIndexLookup[ChunkY * ChunksX + ChunkX];
            if (DirtyChunks[Chunk] == 0)
            {
                DirtyChunks[Chunk] = 1;
                ++PendingDirtyCount;
            }
        }
    }
}

// ============================================================ per-frame

void ALivingCityTerrainView::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    if (!bBuilt && !TryBuild())
    {
        return;
    }

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

    ApplyGroundMaterial();
    PollForChanges(*Subsystem);
    RemeshDirtyChunks();
}

void ALivingCityTerrainView::PollForChanges(const ULivingCitySubsystem& Subsystem)
{
    // AcquireLatest leaves the member untouched when it cannot hand over a stable snapshot,
    // so a miss simply means "no news this frame".
    if (!Subsystem.GetLatestSnapshot(LatestSnapshot))
    {
        return;
    }
    if (LatestSnapshot.terrainVersion == SeenTerrainVersion)
    {
        return;
    }
    SeenTerrainVersion = LatestSnapshot.terrainVersion;

    const lc::Simulation* Sim = Subsystem.GetSimulation();
    if (!Sim)
    {
        return;
    }

    // The global version moved, so something changed. Per-chunk versions say what: only the
    // chunks that actually moved are copied and dirtied.
    const lc::Terrain& Terrain    = Sim->TerrainField();
    const int32        ChunkCount = ChunkVersions.Num();
    for (int32 Chunk = 0; Chunk < ChunkCount; ++Chunk)
    {
        if (Terrain.ChunkVersion(static_cast<lc::u32>(Chunk)) == ChunkVersions[Chunk])
        {
            continue;
        }
        CopyChunkFromSim(Terrain, Chunk);
        MarkDirtyWithNeighbours(ChunkCoords[Chunk]);
    }
}

void ALivingCityTerrainView::RemeshDirtyChunks()
{
    if (PendingDirtyCount <= 0)
    {
        return;
    }

    int32 Remeshed = 0;
    for (int32 Chunk = 0; Chunk < DirtyChunks.Num() && Remeshed < kMaxRemeshesPerFrame; ++Chunk)
    {
        if (DirtyChunks[Chunk] == 0)
        {
            continue;
        }
        DirtyChunks[Chunk] = 0;
        --PendingDirtyCount;
        MeshChunk(Chunk, /*bCreate*/ false);
        ++Remeshed;
    }
}

// ============================================================ queries

float ALivingCityTerrainView::HeightAtCm(float XCm, float YCm) const
{
    if (!bBuilt)
    {
        return 0.0f;
    }

    // Continuous cell coordinates with the cell CENTRE at integer values, since that is where
    // the heights are measured. Clamped to just outside the field before the integer cast, so
    // a query from across the map cannot overflow the cast.
    const double U = FMath::Clamp((static_cast<double>(XCm) - static_cast<double>(OriginXCm)) / kCellSizeCm - 0.5,
                                  -1.0, static_cast<double>(CellsX));
    const double V = FMath::Clamp((static_cast<double>(YCm) - static_cast<double>(OriginYCm)) / kCellSizeCm - 0.5,
                                  -1.0, static_cast<double>(CellsY));

    const double FloorU = FMath::Floor(U);
    const double FloorV = FMath::Floor(V);
    const int32  CellX  = static_cast<int32>(FloorU);
    const int32  CellY  = static_cast<int32>(FloorV);
    const double FracX  = U - FloorU;
    const double FracY  = V - FloorV;

    const double H00 = static_cast<double>(CachedCellHeight(CellX,     CellY));
    const double H10 = static_cast<double>(CachedCellHeight(CellX + 1, CellY));
    const double H01 = static_cast<double>(CachedCellHeight(CellX,     CellY + 1));
    const double H11 = static_cast<double>(CachedCellHeight(CellX + 1, CellY + 1));

    const double Bottom = H00 + (H10 - H00) * FracX;
    const double Top    = H01 + (H11 - H01) * FracX;
    return static_cast<float>(Bottom + (Top - Bottom) * FracY);
}
