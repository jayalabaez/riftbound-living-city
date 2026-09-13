// LIVING CITY — modifiable terrain. Implements include/livingcity/world/Terrain.h.
//
// R1: zero Unreal types.  R3: every number here is an integer; the only randomness is the
// Terrain stream of a LOCAL RngStreams(worldSeed), drawn in a fixed row-major order, so the
// generated field is a pure function of (seed, halfExtentM, flatHalfExtentM).  R5: the
// generated heights are kept beside the live ones, so "what did the player change" is a
// comparison, not a journal that could drift from the truth.
//
// ---------------------------------------------------------------------------------------
// THREADING — stated once, so every function below can be read against it.
//
// The sim thread owns this object. The render thread is allowed exactly one read,
// CopyChunkHeights, and the two version queries that tell it whether a copy is worth making.
// Those four, plus Modify, take the mutex held in Impl. Modify holds it across the WHOLE
// brush, so a copy can never observe half a brush. Generate and Clear also take it while they
// swap the field in or out, so a copy made across a regenerate sees the old field or the new
// one and never a resize in progress.
//
// HeightCm, HeightAtCm, WorldToCell, the modified-cell walk and HashInto do NOT take the
// lock. They are sim-thread reads of sim-thread-owned state, and locking them would put a
// mutex acquisition inside every citizen's ground query on the 20 Hz path for no benefit.
//
// The mutex is NEVER recursive: nothing that holds it calls anything else that takes it.
// ---------------------------------------------------------------------------------------
//
// NAMING: UBT compiles every sim source as one translation unit (LivingCitySim.Build.cs), so
// the anonymous-namespace helpers and constants here carry a Terrain prefix. A bare `Abs` or
// `kBlendCells` would collide with a sibling file's helper and fail the Unreal build while
// passing the standalone one.
#include "livingcity/world/Terrain.h"

#include "livingcity/core/Rng.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace lc {

// ---------------------------------------------------------------- Impl
// Only the lock lives here. Keeping it behind a pointer is what keeps <mutex> out of
// Terrain.h and therefore out of every header that includes Terrain.h (R11).
struct Terrain::Impl {
    std::mutex mutex;
};

namespace {

// ---------------------------------------------------------------- constants
// Largest half-extent Generate accepts. 32768 m is 32768 cells a side: the cell count
// squared is 2^30, which fits the u32 index space with room, and cells * kCellSizeCm is
// 6.5 million, well inside an i32 world coordinate. Nothing in the phase plan comes near it.
constexpr i32 kTerrainMaxHalfExtentM = 32768;

// The noise lattice: one random height every kTerrainLatticeCells cells, bilinearly
// interpolated between. 8 cells = 16 m, so the rolling surface has features a citizen can
// see over, not a tile-sized speckle.
constexpr i32 kTerrainLatticeCells = 8;
constexpr i32 kTerrainNoiseMinCm   = -150;   // inclusive
constexpr i32 kTerrainNoiseMaxCm   = 250;    // inclusive

// Width of the ramp from the flat square out to full noise. Without it the city would stand
// on a plateau with a cliff around its edge.
constexpr i32 kTerrainBlendCells = 8;

// Brush falloff is computed in 1/1024ths. Full weight at the centre, zero at the rim.
constexpr i64 kTerrainFalloffScale = 1024;

// A chunk copy is (kChunkCells + 1)^2 values: one extra row and column shared with the
// neighbour so adjacent meshes meet on identical vertices.
constexpr i32 kTerrainCopyStride = Terrain::kChunkCells + 1;

static_assert(Terrain::kChunkCells % kTerrainLatticeCells == 0,
              "the noise lattice must tile a chunk, or a chunk-aligned field could end mid-cell");
static_assert(kTerrainNoiseMinCm >= Terrain::kMinHeightCm &&
              kTerrainNoiseMaxCm <= Terrain::kMaxHeightCm,
              "generated heights must already sit inside the clamp range");
static_assert(kTerrainNoiseMinCm < kTerrainNoiseMaxCm, "noise range is empty");
static_assert(Terrain::kCellSizeCm % 2 == 0, "a cell must have an integer centre");
static_assert(Terrain::kMinHeightCm < 0 && Terrain::kMaxHeightCm > 0,
              "level ground (height 0) must be inside the clamp range");

// ---------------------------------------------------------------- integer helpers
// Floor division for b > 0. C++ `/` truncates toward zero, which is the wrong answer for a
// world coordinate left of the origin: -1 / 200 is 0, but the cell containing x = -1 is -1.
i64 TerrainFloorDiv(i64 a, i64 b) {
    i64 q = a / b;
    if ((a % b) < 0) --q;
    return q;
}

i64 TerrainCeilDiv(i64 a, i64 b) { return -TerrainFloorDiv(-a, b); }

i64 TerrainAbs64(i64 v) { return (v < 0) ? -v : v; }
i64 TerrainMin64(i64 a, i64 b) { return (a < b) ? a : b; }
i64 TerrainMax64(i64 a, i64 b) { return (a > b) ? a : b; }

// floor(sqrt(n)), digit by digit in base 4. No float anywhere near it (R3): std::sqrt on a
// double would give a bit-exact answer for these magnitudes on this compiler, and that is
// exactly the kind of "happens to work" that a different compiler or a /fp switch breaks.
u64 TerrainISqrt(u64 n) {
    u64 root = 0;
    u64 bit  = 1ull << 62;   // the highest power of 4 that fits a u64
    while (bit > n) bit >>= 2;
    while (bit != 0ull) {
        if (n >= root + bit) {
            n   -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

i32 TerrainClampHeight(i64 h) {
    if (h < Terrain::kMinHeightCm) return Terrain::kMinHeightCm;
    if (h > Terrain::kMaxHeightCm) return Terrain::kMaxHeightCm;
    return static_cast<i32>(h);
}

i32 TerrainClampIndex(i64 v, i32 count) {
    if (v < 0) return 0;
    if (v >= count) return count - 1;
    return static_cast<i32>(v);
}

// World-space centre of cell c along one axis. Heights are heights at cell centres, so this
// is the one place the "+ half a cell" lives.
i64 TerrainCellCentreCm(i32 originCm, i64 c) {
    return static_cast<i64>(originCm) + c * Terrain::kCellSizeCm + Terrain::kCellSizeCm / 2;
}

} // namespace

// ---------------------------------------------------------------- lifetime

Terrain::Terrain() : impl_(new Impl) {}

Terrain::~Terrain() { delete impl_; }

// ---------------------------------------------------------------- generation

// LOCK: taken only for the final swap-in. Everything before it works on locals.
void Terrain::Generate(u64 worldSeed, i32 halfExtentM, i32 flatHalfExtentM) {
    LC_ASSERT_MSG(halfExtentM > 0 && halfExtentM <= kTerrainMaxHalfExtentM,
                  "Terrain::Generate: half-extent must be in (0, 32768] metres");
    LC_ASSERT_MSG(flatHalfExtentM >= 0, "Terrain::Generate: flat half-extent must not be negative");

    // ---- field size -----------------------------------------------------------------
    // Cells to cover the requested span, rounded UP to whole chunks so every chunk is full
    // and the field never ends with a partial chunk the renderer would have to special-case.
    // The rounding grows the field, never shrinks it: the requested extent is always covered.
    const i64 spanCm = 2ll * static_cast<i64>(halfExtentM) * 100ll;
    i32       cells  = static_cast<i32>(spanCm / kCellSizeCm);
    cells = ((cells + kChunkCells - 1) / kChunkCells) * kChunkCells;
    LC_ASSERT_MSG(cells >= kChunkCells && cells % kChunkCells == 0, "Terrain::Generate: bad cell count");

    // Centred: the corner of cell (0,0) sits half the field to the negative side of the
    // origin, on both axes, so world (0,0) is the middle of the field.
    const i32 originCm = -(cells * kCellSizeCm) / 2;

    // ---- noise lattice --------------------------------------------------------------
    // One random height every kTerrainLatticeCells cells, plus one more point per axis so
    // the last cell has a right/bottom neighbour to interpolate toward.
    //
    // Drawn for EVERY lattice point, flat region included, in strict row-major order. The
    // draw sequence therefore depends on the seed and the half-extent alone: enlarging the
    // flat square as the city grows must not re-roll the countryside beyond it, and it does
    // not, because the flat square never touches the draw count.
    //
    // R3: a LOCAL stream set, so generating terrain never advances a generator the caller is
    // also drawing from. Terrain owns the heightfield per the StreamId table in Rng.h.
    const i32 latticeN = cells / kTerrainLatticeCells + 1;
    std::vector<i32> lattice(static_cast<std::size_t>(latticeN) * static_cast<std::size_t>(latticeN));
    {
        RngStreams streams(worldSeed);
        Rng&       rng = streams.Stream(StreamId::Terrain);
        for (i32 ly = 0; ly < latticeN; ++ly) {
            for (i32 lx = 0; lx < latticeN; ++lx) {
                lattice[static_cast<std::size_t>(ly) * static_cast<std::size_t>(latticeN) +
                        static_cast<std::size_t>(lx)] = rng.RangeI(kTerrainNoiseMinCm, kTerrainNoiseMaxCm + 1);
            }
        }
    }
    const auto latticeAt = [&](i32 lx, i32 ly) -> i64 {
        return static_cast<i64>(lattice[static_cast<std::size_t>(ly) * static_cast<std::size_t>(latticeN) +
                                        static_cast<std::size_t>(lx)]);
    };

    // ---- heights --------------------------------------------------------------------
    std::vector<i32> heights(static_cast<std::size_t>(cells) * static_cast<std::size_t>(cells));

    const i64 flatHalfCm = static_cast<i64>(flatHalfExtentM) * 100ll;
    const i64 blendCm    = static_cast<i64>(kTerrainBlendCells) * kCellSizeCm;
    constexpr i64 kL     = kTerrainLatticeCells;

    for (i32 cy = 0; cy < cells; ++cy) {
        const i64 centreY = TerrainCellCentreCm(originCm, cy);
        for (i32 cx = 0; cx < cells; ++cx) {
            const i64 centreX = TerrainCellCentreCm(originCm, cx);

            // Chebyshev distance past the flat square's edge. Inside or on it: level ground,
            // exactly 0, so the city stands on a plane and not on "almost zero".
            const i64 beyond = TerrainMax64(TerrainAbs64(centreX), TerrainAbs64(centreY)) - flatHalfCm;
            i32       h      = 0;

            if (beyond > 0) {
                // Bilinear interpolation between the four surrounding lattice points, in
                // 1/(kL*kL)ths. Integer division truncates toward zero; that is a fixed,
                // reproducible rule, which is all R3 asks of a rounding choice.
                const i32 lx = cx / kTerrainLatticeCells;
                const i32 ly = cy / kTerrainLatticeCells;
                const i64 fx = cx % kTerrainLatticeCells;
                const i64 fy = cy % kTerrainLatticeCells;

                const i64 top   = latticeAt(lx, ly)     * (kL - fx) + latticeAt(lx + 1, ly)     * fx;
                const i64 bot   = latticeAt(lx, ly + 1) * (kL - fx) + latticeAt(lx + 1, ly + 1) * fx;
                const i64 noise = (top * (kL - fy) + bot * fy) / (kL * kL);

                // Ramp from 0 at the flat edge to full noise kTerrainBlendCells out.
                const i64 ramp = TerrainMin64(beyond, blendCm);
                h = static_cast<i32>(noise * ramp / blendCm);
            }

            heights[static_cast<std::size_t>(cy) * static_cast<std::size_t>(cells) +
                    static_cast<std::size_t>(cx)] = h;
        }
    }

    // ---- publish ---------------------------------------------------------------------
    // Under the lock: a render-thread copy in flight sees the whole old field or the whole
    // new one. Everything is assigned, so a Generate on a live object leaves no residue
    // from the previous field (versions restart at zero: it is a new world, not an edit).
    const u32 chunkCount = static_cast<u32>(cells / kChunkCells) * static_cast<u32>(cells / kChunkCells);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        heights_.swap(heights);
        generated_ = heights_;
        chunkVersion_.assign(static_cast<std::size_t>(chunkCount), 0u);
        cellsX_    = cells;
        cellsY_    = cells;
        originXCm_ = originCm;
        originYCm_ = originCm;
        version_   = 0;
        RebuildChunkHashes();
    }
}

// ---------------------------------------------------------------- geometry

i32 Terrain::ChunksX() const { return cellsX_ / kChunkCells; }
i32 Terrain::ChunksY() const { return cellsY_ / kChunkCells; }

u32 Terrain::ChunkCount() const {
    return static_cast<u32>(ChunksX()) * static_cast<u32>(ChunksY());
}

// LOCK: none. Sim-thread read.
i32 Terrain::HeightCm(i32 cx, i32 cy) const {
    if (cellsX_ <= 0 || cellsY_ <= 0) return 0;   // no field yet: level ground everywhere
    const i32 x = TerrainClampIndex(cx, cellsX_);
    const i32 y = TerrainClampIndex(cy, cellsY_);
    return heights_[static_cast<std::size_t>(y) * static_cast<std::size_t>(cellsX_) +
                    static_cast<std::size_t>(x)];
}

// LOCK: none. Sim-thread read.
i32 Terrain::HeightAtCm(i32 xCm, i32 yCm) const {
    i32 cx = 0;
    i32 cy = 0;
    WorldToCell(xCm, yCm, cx, cy);   // clamps on the way out, so the result is always a cell
    return HeightCm(cx, cy);
}

// LOCK: none. Sim-thread read.
//
// The outputs are ALWAYS written, clamped to the field, and the return value says whether the
// position was actually inside it. A caller that only wants "the nearest cell" ignores the
// bool; one that must know whether the point is on the map reads it.
bool Terrain::WorldToCell(i32 xCm, i32 yCm, i32& outCx, i32& outCy) const {
    if (cellsX_ <= 0 || cellsY_ <= 0) {
        outCx = 0;
        outCy = 0;
        return false;
    }
    // The cell containing the point is the one whose centre is nearest: a cell spans
    // [corner, corner + kCellSizeCm) and its centre is in the middle of that span.
    const i64 cx = TerrainFloorDiv(static_cast<i64>(xCm) - originXCm_, kCellSizeCm);
    const i64 cy = TerrainFloorDiv(static_cast<i64>(yCm) - originYCm_, kCellSizeCm);

    const bool inside = cx >= 0 && cx < cellsX_ && cy >= 0 && cy < cellsY_;
    outCx = TerrainClampIndex(cx, cellsX_);
    outCy = TerrainClampIndex(cy, cellsY_);
    return inside;
}

// ---------------------------------------------------------------- modification

// LOCK: held across the whole brush, from the first write to the last version bump.
//
// I5: allocates nothing. The brush is walked in place; the per-chunk "did this chunk's copy
// change" flag is a local, and the field's vectors are never resized.
//
// VERSION CONTRACT: ChunkVersion(c) moves exactly once per call, and exactly when the
// (kChunkCells + 1)^2 block CopyChunkHeights(c) returns would come back different. That block
// borrows one row and one column from the chunks at +X and +Y, so a cell in the FIRST row or
// column of a chunk lives in up to four copies: its owner's, the copy of the chunk to its
// left, the one above, and the one diagonally up-left. All of those must bump, or a renderer
// keyed on versions rebuilds the neighbour's edge from a stale copy and opens a seam along
// every chunk line the brush merely touched.
bool Terrain::Modify(i32 xCm, i32 yCm, i32 radiusCm, i32 deltaCm) {
    if (radiusCm <= 0 || deltaCm == 0) return false;
    if (cellsX_ <= 0 || cellsY_ <= 0) return false;

    // ---- the bounding rectangle of cells whose centre CAN lie inside the brush ---------
    // centre(c) = origin + c*size + size/2 is within [p - r, p + r] exactly when
    //     c in [ceil((p - r - origin - size/2) / size), floor((p + r - origin - size/2) / size)]
    // i64 throughout: p and r are arbitrary i32s and p - r alone can leave the type.
    const i64 r    = static_cast<i64>(radiusCm);
    const i64 half = kCellSizeCm / 2;

    const i64 loX = TerrainCeilDiv (static_cast<i64>(xCm) - r - originXCm_ - half, kCellSizeCm);
    const i64 hiX = TerrainFloorDiv(static_cast<i64>(xCm) + r - originXCm_ - half, kCellSizeCm);
    const i64 loY = TerrainCeilDiv (static_cast<i64>(yCm) - r - originYCm_ - half, kCellSizeCm);
    const i64 hiY = TerrainFloorDiv(static_cast<i64>(yCm) + r - originYCm_ - half, kCellSizeCm);

    // Clip to the field. A brush entirely off the map changes nothing and says so.
    const i64 minCx = TerrainMax64(loX, 0);
    const i64 maxCx = TerrainMin64(hiX, static_cast<i64>(cellsX_) - 1);
    const i64 minCy = TerrainMax64(loY, 0);
    const i64 maxCy = TerrainMin64(hiY, static_cast<i64>(cellsY_) - 1);
    if (minCx > maxCx || minCy > maxCy) return false;

    const u64 r2    = static_cast<u64>(r) * static_cast<u64>(r);
    const i64 delta = static_cast<i64>(deltaCm);

    // The brush, as one pure function of a cell's offset from its centre: how far it moves
    // that cell before clamping, or 0 outside the disc. Every cell — owned or borrowed — is
    // judged by this one formula, so the look-ahead below can never disagree with the write
    // that follows.
    const auto brushDelta = [&](i64 dx, i64 dy) -> i64 {
        const u64 d2 = static_cast<u64>(dx * dx) + static_cast<u64>(dy * dy);
        if (d2 > r2) return 0;   // the rectangle's corners lie outside the disc

        // d <= r by construction (d2 <= r2), so weight is in [0, 1024] and the rim, at
        // d == r, gets exactly zero. The centre cell gets deltaCm exactly.
        const i64 d      = static_cast<i64>(TerrainISqrt(d2));
        const i64 weight = (r - d) * kTerrainFalloffScale / r;
        return delta * weight / kTerrainFalloffScale;
    };
    const auto cellAt = [&](i64 cx, i64 cy) -> i32& {
        return heights_[static_cast<std::size_t>(cy) * static_cast<std::size_t>(cellsX_) +
                        static_cast<std::size_t>(cx)];
    };
    // Read-only: would the brush move this cell? Used on cells another chunk owns and has
    // not written yet, so the answer is exactly what that later write will do.
    const auto wouldMove = [&](i64 cx, i64 cy) -> bool {
        const i64 dc = brushDelta(TerrainCellCentreCm(originXCm_, cx) - xCm,
                                  TerrainCellCentreCm(originYCm_, cy) - yCm);
        if (dc == 0) return false;
        const i32 cell = cellAt(cx, cy);
        return TerrainClampHeight(static_cast<i64>(cell) + dc) != cell;
    };

    // ---- walk the rectangle chunk by chunk ---------------------------------------------
    // Chunk-major, in increasing X then increasing Y. That order is load-bearing: when chunk
    // (chX, chY) is walked, the column it borrows from (chX + 1, chY), the row it borrows
    // from (chX, chY + 1) and the corner it borrows from (chX + 1, chY + 1) have not been
    // written yet, so looking at them read-only tells this chunk whether its copy is about
    // to change. One bool per chunk instead of a scratch set (I5).
    //
    // The walk starts one chunk EARLIER than the rectangle's own first chunk whenever the
    // rectangle begins on a chunk's first column (or row): that column is the previous
    // chunk's borrowed one, and the previous chunk has to be visited to notice — a brush
    // that moves only cell 64 changes chunk 3's copy without owning a single cell of chunk
    // 3. Such a leading chunk has an empty own-cell range and only its look-ahead runs.
    const i32 chunkMinX = static_cast<i32>(minCx > 0 ? (minCx - 1) / kChunkCells : 0);
    const i32 chunkMaxX = static_cast<i32>(maxCx / kChunkCells);
    const i32 chunkMinY = static_cast<i32>(minCy > 0 ? (minCy - 1) / kChunkCells : 0);
    const i32 chunkMaxY = static_cast<i32>(maxCy / kChunkCells);

    bool anyChanged = false;

    std::lock_guard<std::mutex> lock(impl_->mutex);

    for (i32 chY = chunkMinY; chY <= chunkMaxY; ++chY) {
        const i64 cy0 = TerrainMax64(minCy, static_cast<i64>(chY) * kChunkCells);
        const i64 cy1 = TerrainMin64(maxCy, static_cast<i64>(chY) * kChunkCells + kChunkCells - 1);
        // The row this chunk's copy borrows. It can only move if the brush rectangle reaches
        // it, which is exactly when there is a chunk row below this one inside the walk.
        const i64  cyBorrowed    = static_cast<i64>(chY) * kChunkCells + kChunkCells;
        const bool rowIsBorrowed = chY < chunkMaxY;

        for (i32 chX = chunkMinX; chX <= chunkMaxX; ++chX) {
            const i64  cx0 = TerrainMax64(minCx, static_cast<i64>(chX) * kChunkCells);
            const i64  cx1 = TerrainMin64(maxCx, static_cast<i64>(chX) * kChunkCells + kChunkCells - 1);
            const i64  cxBorrowed    = static_cast<i64>(chX) * kChunkCells + kChunkCells;
            const bool colIsBorrowed = chX < chunkMaxX;
            bool       copyChanged   = false;

            // Own cells: apply the brush. (Empty for a leading chunk, see above.)
            for (i64 cy = cy0; cy <= cy1; ++cy) {
                const i64 dy = TerrainCellCentreCm(originYCm_, cy) - yCm;
                for (i64 cx = cx0; cx <= cx1; ++cx) {
                    const i64 dc = brushDelta(TerrainCellCentreCm(originXCm_, cx) - xCm, dy);
                    if (dc == 0) continue;

                    i32&      cell = cellAt(cx, cy);
                    const i32 next = TerrainClampHeight(static_cast<i64>(cell) + dc);
                    if (next != cell) {
                        cell        = next;
                        copyChanged = true;
                        anyChanged  = true;
                    }
                }
            }

            // Borrowed column, row and corner: look, do not touch. Their owners write them
            // later in this same walk. Skipped as soon as the copy is known to change.
            if (!copyChanged && colIsBorrowed) {
                for (i64 cy = cy0; cy <= cy1 && !copyChanged; ++cy) {
                    copyChanged = wouldMove(cxBorrowed, cy);
                }
            }
            if (!copyChanged && rowIsBorrowed) {
                for (i64 cx = cx0; cx <= cx1 && !copyChanged; ++cx) {
                    copyChanged = wouldMove(cx, cyBorrowed);
                }
            }
            if (!copyChanged && colIsBorrowed && rowIsBorrowed) {
                copyChanged = wouldMove(cxBorrowed, cyBorrowed);
            }

            const std::size_t chunk = static_cast<std::size_t>(chY) * static_cast<std::size_t>(ChunksX()) +
                                      static_cast<std::size_t>(chX);
            if (copyChanged) {
                ++chunkVersion_[chunk];
            }
            // The hash cache covers only OWN cells. A borrowed edge can dirty the renderer's
            // copy without changing this digest. Recomputing each visited chunk is bounded
            // to 256 cells and leaves the cache independent of the renderer's dirty flag.
            chunkHash_[chunk] = HashChunk(static_cast<u32>(chunk));
        }
    }

    if (anyChanged) ++version_;
    return anyChanged;
}

// ---------------------------------------------------------------- change tracking

// LOCK: taken. Safe from the render thread.
u64 Terrain::Version() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return version_;
}

// LOCK: taken. Safe from the render thread.
u32 Terrain::ChunkVersion(u32 chunk) const {
    LC_ASSERT_MSG(chunk < ChunkCount(), "Terrain::ChunkVersion: chunk index out of range");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return chunkVersion_[static_cast<std::size_t>(chunk)];
}

// LOCK: none. Pure geometry on the field's dimensions.
u32 Terrain::ChunkIndex(i32 chunkX, i32 chunkY) const {
    LC_ASSERT_MSG(chunkX >= 0 && chunkX < ChunksX() && chunkY >= 0 && chunkY < ChunksY(),
                  "Terrain::ChunkIndex: chunk coordinate out of range");
    return static_cast<u32>(chunkY) * static_cast<u32>(ChunksX()) + static_cast<u32>(chunkX);
}

// LOCK: taken, for the whole copy. This is the render thread's read.
//
// (kChunkCells + 1)^2 values, row-major, row = y. Row and column kChunkCells come from the
// neighbouring chunk, or repeat the field's last row/column at its edge, so two adjacent
// chunk meshes are built from identical edge values and can never show a seam.
void Terrain::CopyChunkHeights(u32 chunk, i32* out) const {
    LC_ASSERT_MSG(out != nullptr, "Terrain::CopyChunkHeights: null output");
    LC_ASSERT_MSG(chunk < ChunkCount(), "Terrain::CopyChunkHeights: chunk index out of range");

    const i32 chunksX = ChunksX();
    const i32 baseX   = static_cast<i32>(chunk % static_cast<u32>(chunksX)) * kChunkCells;
    const i32 baseY   = static_cast<i32>(chunk / static_cast<u32>(chunksX)) * kChunkCells;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (i32 j = 0; j < kTerrainCopyStride; ++j) {
        const i32  cy  = TerrainClampIndex(static_cast<i64>(baseY) + j, cellsY_);
        const i32* row = heights_.data() + static_cast<std::size_t>(cy) * static_cast<std::size_t>(cellsX_);
        i32*       dst = out + static_cast<std::size_t>(j) * static_cast<std::size_t>(kTerrainCopyStride);
        for (i32 i = 0; i < kTerrainCopyStride; ++i) {
            const i32 cx = TerrainClampIndex(static_cast<i64>(baseX) + i, cellsX_);
            dst[i] = row[static_cast<std::size_t>(cx)];
        }
    }
}

// ---------------------------------------------------------------- deltas (R5)
// The modified set is DEFINED as "cells that differ from the seed's heights", not "cells a
// brush has ever touched". A dig that is later filled back to exactly its generated height
// drops out of the save, which is the right answer: the save stores divergence, and there
// is none. Both walks are O(cells); they run at save time, never on the tick.

// LOCK: none. Sim-thread read.
u32 Terrain::ModifiedCellCount() const {
    u32 count = 0;
    for (std::size_t i = 0; i < heights_.size(); ++i) {
        if (heights_[i] != generated_[i]) ++count;
    }
    return count;
}

// LOCK: none. Sim-thread read. n is an index into the ascending list of modified cells.
void Terrain::ModifiedCell(u32 n, i32& outCx, i32& outCy, i32& outHeightCm) const {
    u32 seen = 0;
    for (std::size_t i = 0; i < heights_.size(); ++i) {
        if (heights_[i] == generated_[i]) continue;
        if (seen == n) {
            outCx       = static_cast<i32>(i % static_cast<std::size_t>(cellsX_));
            outCy       = static_cast<i32>(i / static_cast<std::size_t>(cellsX_));
            outHeightCm = heights_[i];
            return;
        }
        ++seen;
    }
    // Reached only when n >= ModifiedCellCount(). A caller that walks past the end has a
    // bug in its save loop; fail there, not with garbage in a save file (I4).
    LC_FATAL("Terrain::ModifiedCell: index is past the modified-cell count");
}

// ---------------------------------------------------------------- hashing (I1, I7)
// LOCK: none. Sim-thread read.
//
// Dimensions, origin, then the cached digest of each chunk's own live heights, in chunk
// index order. NOT hashed, deliberately:
//   * generated_ — a pure function of the seed, recomputable, and therefore not state;
//   * version_ and chunkVersion_ — renderer bookkeeping. Two worlds with identical heights
//     reached by different brush histories ARE the same world, and must hash the same, or a
//     save/load round trip (which restarts the versions) would change the world hash.
void Terrain::HashInto(Hasher& h) const {
    h.I32(cellsX_);
    h.I32(cellsY_);
    h.I32(originXCm_);
    h.I32(originYCm_);
    // Chunk digests in index order, not the cells themselves - see chunkHash_ in the header.
    for (std::size_t c = 0; c < chunkHash_.size(); ++c) h.U64(chunkHash_[c]);
}

u64 Terrain::HashChunk(u32 chunk) const {
    const i32 chunksX = ChunksX();
    const i32 chX = static_cast<i32>(chunk) % chunksX;
    const i32 chY = static_cast<i32>(chunk) / chunksX;
    const i32 x0  = chX * kChunkCells;
    const i32 y0  = chY * kChunkCells;
    Hasher h;
    for (i32 cy = y0; cy < y0 + kChunkCells; ++cy) {
        const std::size_t row = static_cast<std::size_t>(cy) * static_cast<std::size_t>(cellsX_);
        for (i32 cx = x0; cx < x0 + kChunkCells; ++cx) {
            h.I32(heights_[row + static_cast<std::size_t>(cx)]);
        }
    }
    return h.Value();
}

void Terrain::RebuildChunkHashes() {
    const u32 n = ChunkCount();
    chunkHash_.assign(static_cast<std::size_t>(n), 0ull);
    for (u32 c = 0; c < n; ++c) chunkHash_[c] = HashChunk(c);
}

// LOCK: taken while the field is torn down, for the same reason Generate takes it.
void Terrain::Clear() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    heights_.clear();
    generated_.clear();
    chunkVersion_.clear();
    chunkHash_.clear();
    cellsX_    = 0;
    cellsY_    = 0;
    originXCm_ = 0;
    originYCm_ = 0;
    version_   = 0;
}

} // namespace lc
