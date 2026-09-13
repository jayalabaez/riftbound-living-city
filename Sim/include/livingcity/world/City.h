// LIVING CITY — deterministic city layout.
//
// R1: zero Unreal types. R3: generated purely from a seed, identical every run.
//
// This is the Phase 1 skeleton of the generation pipeline in CLAUDE.md: a street grid, block
// subdivision, zoning and building footprints. Terrain, hydrology and caves come later; what
// exists here is enough to place buildings that citizens can actually live and work in.
//
// UNITS: metres, as i32, measured from the city centre at (0,0). The renderer converts to
// Unreal centimetres at the boundary. No floats anywhere.
#pragma once

#include "livingcity/core/Core.h"
#include "livingcity/core/Rng.h"

#include <vector>

namespace lc {

// What a building is for. Drives who lives there, who works there, and its colour.
enum class LotUse : u8 {
    Home = 0,
    Work = 1,
    Shop = 2,
    Civic = 3,
    Count = 4
};

LC_API const char* LotUseName(LotUse use);

// One building. Axis-aligned box on the ground plane.
//
// Field order is largest-first and the struct is padding-free on purpose: it is hashed
// byte-wise into the world hash, and padding bytes are indeterminate (see D-013).
struct Building {
    i32    centreX;     // metres from city centre
    i32    centreY;
    i32    halfX;       // footprint half-extent, metres
    i32    halfY;
    i32    height;      // metres
    u32    blockIndex;  // which city block it belongs to
    LotUse use;
    u8     palette;     // 0..7, index into the art-direction palette
    u8     pad0;
    u8     pad1;
};

static_assert(sizeof(Building) == 28, "Building must stay padding-free; it is hashed byte-wise");

// The street grid plus everything built on it.
//
// The grid is regular: blocks of kBlockSize metres separated by roads of kRoadWidth metres.
// A regular grid is a deliberate Phase 1 simplification - it makes agent pathing exact
// integer arithmetic instead of a graph search, which is what lets thousands of citizens
// commute inside the tick budget. Irregular roads arrive with the real road-network pass.
class LC_API City {
public:
    static constexpr i32 kBlockSize  = 60;   // metres, edge of a city block
    static constexpr i32 kRoadWidth  = 12;   // metres, road corridor between blocks
    static constexpr i32 kStride     = kBlockSize + kRoadWidth;

    // Lays out blocksX by blocksY blocks around the origin and fills them with buildings.
    // Deterministic: the same seed and dimensions always produce the same city.
    void Generate(u64 worldSeed, i32 blocksX, i32 blocksY);

    // ---- queries ----------------------------------------------------------------------
    u32             Count() const { return static_cast<u32>(buildings_.size()); }
    const Building& At(u32 index) const;
    const std::vector<Building>& All() const { return buildings_; }

    // Indices of every building with a given use, so homes and jobs can be assigned without
    // scanning. Always in ascending index order - never rebuilt in a different order.
    const std::vector<u32>& OfUse(LotUse use) const;

    i32 BlocksX() const { return blocksX_; }
    i32 BlocksY() const { return blocksY_; }

    // Half-extent of the whole city in metres, for camera framing and clamping.
    i32 ExtentX() const { return (blocksX_ * kStride) / 2; }
    i32 ExtentY() const { return (blocksY_ * kStride) / 2; }

    // ---- road grid --------------------------------------------------------------------
    // Nearest road centreline to a coordinate. Citizens walk along roads, so pathing is
    // "snap to the road grid, travel in X, travel in Y" - exact, cheap, and deterministic.
    i32 NearestRoadX(i32 x) const;
    i32 NearestRoadY(i32 y) const;

    // True if the point is inside a road corridor rather than on a block.
    bool IsOnRoad(i32 x, i32 y) const;

    void HashInto(Hasher& h) const;
    void Clear();

private:
    std::vector<Building> buildings_;
    std::vector<u32>      byUse_[static_cast<u32>(LotUse::Count)];
    i32                   blocksX_ = 0;
    i32                   blocksY_ = 0;
};

} // namespace lc
