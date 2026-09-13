// LIVING CITY — modifiable terrain.
//
// R1: zero Unreal types. R3: deterministic, integer-only. R5: the terrain is generated from
// the seed and only the cells the player changed will ever be saved.
//
// This is the first, deliberately simple slice of the Phase 1 world: a heightfield the player
// can dig into and pile up, not yet the 32^3 voxel chunks with caves and minerals. A
// heightfield gets "terrain modification" in front of the player now, on the same
// command/replay path everything else uses, and every rule it establishes - chunked dirty
// tracking, integer heights, the sim owning the authoritative data - carries straight over to
// voxels.
//
// UNITS: centimetres, as i32, because that is what the rest of the sim and the renderer both
// speak. A cell is 2 m square. Heights are the height at the cell's centre.
#pragma once

#include "livingcity/core/Core.h"

#include <vector>

namespace lc {

class LC_API Terrain {
public:
    static constexpr i32 kCellSizeCm = 200;    // 2 m per cell
    static constexpr i32 kChunkCells = 16;     // chunk = 16 x 16 cells = 32 m square

    // Height bounds, so a determined player cannot dig to the centre of the earth or build
    // a tower of Babel. Clamped, not asserted: hitting the limit is normal play.
    static constexpr i32 kMinHeightCm = -2000;
    static constexpr i32 kMaxHeightCm =  4000;

    Terrain();
    ~Terrain();
    Terrain(const Terrain&)            = delete;
    Terrain& operator=(const Terrain&) = delete;

    // Generates a heightfield covering a square of halfExtentM metres each way from the
    // origin. Flat (height 0) inside flatHalfExtentM so the city stands on level ground, with
    // gentle deterministic undulation beyond it. Integer noise only.
    void Generate(u64 worldSeed, i32 halfExtentM, i32 flatHalfExtentM);

    // ---- geometry -------------------------------------------------------------------
    i32 CellsX() const { return cellsX_; }
    i32 CellsY() const { return cellsY_; }
    i32 OriginXCm() const { return originXCm_; }   // world position of cell (0,0)'s corner
    i32 OriginYCm() const { return originYCm_; }
    i32 ChunksX() const;
    i32 ChunksY() const;
    u32 ChunkCount() const;

    // Height at a cell, clamped to the field's edge for out-of-range cells - so a caller
    // that steps one past the edge sees the edge, never garbage.
    i32 HeightCm(i32 cx, i32 cy) const;

    // Height under a world position, by nearest cell. No interpolation: integer-exact, and
    // the renderer does its own smoothing on its own side.
    i32 HeightAtCm(i32 xCm, i32 yCm) const;

    bool WorldToCell(i32 xCm, i32 yCm, i32& outCx, i32& outCy) const;

    // ---- modification ---------------------------------------------------------------
    // A round brush. Every cell within radiusCm of (xCm, yCm) moves by deltaCm scaled by an
    // integer falloff (full at the centre, zero at the rim), then is clamped to the height
    // bounds. Returns true if any cell actually changed. Bumps the version of every chunk it
    // touched. Deterministic: same inputs, same heights, every time, on every machine.
    //
    // This is the only way terrain changes. It is reached through CommandType::ModifyTerrain
    // and therefore through the replay log - a dig is as replayable as a keypress.
    bool Modify(i32 xCm, i32 yCm, i32 radiusCm, i32 deltaCm);

    // ---- change tracking, for the renderer ------------------------------------------
    // Global version increments on every Modify that changed something. Per-chunk versions
    // let the view rebuild only the meshes that moved.
    u64 Version() const;
    u32 ChunkVersion(u32 chunk) const;
    u32 ChunkIndex(i32 chunkX, i32 chunkY) const;

    // Copies a chunk's heights into `out`, which must hold (kChunkCells+1)^2 values - one
    // extra row and column so adjacent chunk meshes share their edge and never show a seam.
    //
    // THREAD SAFETY: this is the ONE read the render thread may perform on terrain state,
    // and it takes an internal lock that Modify also takes. Everything else on this class is
    // sim-thread only. Modifications are player-driven and rare, so the lock is never
    // contended in practice; it exists so a partially-applied brush can never be observed.
    void CopyChunkHeights(u32 chunk, i32* out) const;

    // Cells changed since generation, for R5 (save = seed + deltas). Ascending cell index,
    // each with its current height.
    u32  ModifiedCellCount() const;
    void ModifiedCell(u32 n, i32& outCx, i32& outCy, i32& outHeightCm) const;

    void HashInto(Hasher& h) const;
    void Clear();

private:
    struct Impl;
    Impl* impl_;   // holds the lock, so <mutex> stays out of every header that includes this

    std::vector<i32> heights_;        // cellsY_ rows of cellsX_
    std::vector<i32> generated_;      // the seed's heights, for delta detection (R5)
    std::vector<u32> chunkVersion_;

    // Per-chunk digest of the chunk's OWN cells, kept current by Modify. HashInto folds these
    // instead of walking every cell: the world hash is taken often, the terrain is 100k cells,
    // and hashing all of them each time cost more than the rest of the tick put together.
    // Still a pure function of the heights, so determinism (I1) and save equality (I7) hold.
    std::vector<u64> chunkHash_;

    u64  HashChunk(u32 chunk) const;    // digest of one chunk's 16x16 cells, row-major
    void RebuildChunkHashes();          // all chunks; Generate initializes the cache
    i32 cellsX_    = 0;
    i32 cellsY_    = 0;
    i32 originXCm_ = 0;
    i32 originYCm_ = 0;
    u64 version_   = 0;
};

} // namespace lc
