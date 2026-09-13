// Tests for livingcity/world/Terrain.h — the modifiable heightfield.
//
// Invariant tests, not getter tests. What each group actually defends:
//
//   * I1  — a seed pins a field, and the known-answer hash at the bottom freezes it: any
//           change to the noise, the blend, the draw order or the hash walk moves every
//           saved world, so it has to fail here loudly rather than pass quietly.
//   * R5  — the modified-cell walk is exactly the set of cells that differ from the seed,
//           so a save stores divergence and nothing else.
//   * R3  — the brush is integer-exact and symmetric; the same brush twice is twice the
//           brush; there is no float anywhere to make (x, y) and (y, x) come out different.
//   * I5  — Modify allocates nothing.
//   * The render-thread contract: a chunk copy is never torn, adjacent copies share an edge
//           exactly, and a chunk's version moves exactly when its COPY moves — including
//           the row and column it borrows from its neighbours.
//
// Heights are in centimetres. The field in most tests is 256 m each way (256 cells, 16x16
// chunks) with a 128 m flat square, which is small enough to scan whole in a test and large
// enough that the flat square, the blend ring and the open countryside all exist.
#include "TestFramework.h"

#include "livingcity/core/Rng.h"
#include "livingcity/world/Terrain.h"

#include <atomic>
#include <cstdint>
#include <thread>

using namespace lc;

namespace {

constexpr u64 kSeed        = 1337ull;
constexpr i32 kHalfExtentM = 256;
constexpr i32 kFlatHalfM   = 128;

// The copy buffer the header specifies: one extra row and column.
constexpr i32 kCopyStride = Terrain::kChunkCells + 1;
constexpr i32 kCopyCount  = kCopyStride * kCopyStride;

u64 HashOf(const Terrain& t) {
    Hasher h;
    t.HashInto(h);
    return h.Value();
}

// The uncached oracle reads every authoritative height. It never calls HashInto or the
// private cache helper: two terrains with the same stale cache must not make this pass.
u64 HashFromLiveHeights(const Terrain& t) {
    Hasher world;
    world.I32(t.CellsX());
    world.I32(t.CellsY());
    world.I32(t.OriginXCm());
    world.I32(t.OriginYCm());
    for (i32 chunkY = 0; chunkY < t.ChunksY(); ++chunkY) {
        for (i32 chunkX = 0; chunkX < t.ChunksX(); ++chunkX) {
            Hasher chunk;
            for (i32 y = 0; y < Terrain::kChunkCells; ++y) {
                for (i32 x = 0; x < Terrain::kChunkCells; ++x) {
                    chunk.I32(t.HeightCm(chunkX * Terrain::kChunkCells + x,
                                         chunkY * Terrain::kChunkCells + y));
                }
            }
            world.U64(chunk.Value());
        }
    }
    return world.Value();
}

i64 Abs64(i64 v) { return (v < 0) ? -v : v; }

// World-space centre of a cell, re-derived from the header constants ALONE rather than read
// back from the implementation: origin is the corner of cell (0,0), a cell is kCellSizeCm
// wide, and a height is the height at the cell's centre.
i32 CentreX(const Terrain& t, i32 cx) { return t.OriginXCm() + cx * Terrain::kCellSizeCm + Terrain::kCellSizeCm / 2; }
i32 CentreY(const Terrain& t, i32 cy) { return t.OriginYCm() + cy * Terrain::kCellSizeCm + Terrain::kCellSizeCm / 2; }

// A cell is "in the flat square" when its CENTRE is within the flat half-extent on both axes.
bool CellIsFlat(const Terrain& t, i32 cx, i32 cy, i32 flatHalfM) {
    const i64 flatCm = static_cast<i64>(flatHalfM) * 100;
    return Abs64(CentreX(t, cx)) <= flatCm && Abs64(CentreY(t, cy)) <= flatCm;
}

// Every height, by cell, into a caller-owned buffer of CellsX*CellsY. Used to diff a field
// around a brush without going through the implementation's own delta walk.
void SnapshotHeights(const Terrain& t, std::vector<i32>& out) {
    out.resize(static_cast<std::size_t>(t.CellsX()) * static_cast<std::size_t>(t.CellsY()));
    for (i32 cy = 0; cy < t.CellsY(); ++cy) {
        for (i32 cx = 0; cx < t.CellsX(); ++cx) {
            out[static_cast<std::size_t>(cy) * static_cast<std::size_t>(t.CellsX()) + static_cast<std::size_t>(cx)] =
                t.HeightCm(cx, cy);
        }
    }
}

i32 At(const std::vector<i32>& heights, const Terrain& t, i32 cx, i32 cy) {
    return heights[static_cast<std::size_t>(cy) * static_cast<std::size_t>(t.CellsX()) + static_cast<std::size_t>(cx)];
}

// Which chunk a cell belongs to, derived from the header's chunk size alone.
u32 ChunkOfCell(const Terrain& t, i32 cx, i32 cy) {
    return static_cast<u32>(cy / Terrain::kChunkCells) * static_cast<u32>(t.ChunksX()) +
           static_cast<u32>(cx / Terrain::kChunkCells);
}

// Every chunk's copy, back to back, so two whole-field copies can be diffed chunk by chunk.
void CopyAllChunks(const Terrain& t, std::vector<i32>& out) {
    out.resize(static_cast<std::size_t>(t.ChunkCount()) * static_cast<std::size_t>(kCopyCount));
    for (u32 c = 0; c < t.ChunkCount(); ++c) {
        t.CopyChunkHeights(c, out.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(kCopyCount));
    }
}

bool ChunkCopyDiffers(const std::vector<i32>& a, const std::vector<i32>& b, u32 chunk) {
    const std::size_t base = static_cast<std::size_t>(chunk) * static_cast<std::size_t>(kCopyCount);
    for (std::size_t k = 0; k < static_cast<std::size_t>(kCopyCount); ++k) {
        if (a[base + k] != b[base + k]) return true;
    }
    return false;
}

// floor(sqrt(n)) for the small distances a test brush produces. Integer, like the sim.
i64 ISqrtSmall(i64 n) {
    i64 d = 0;
    while ((d + 1) * (d + 1) <= n) ++d;
    return d;
}

} // namespace

// ---------------------------------------------------------------------------------------
// I1 — the seed is the field.
// ---------------------------------------------------------------------------------------

LC_TEST(terrain_same_seed_generates_an_identical_field) {
    Terrain a;
    Terrain b;
    a.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    b.Generate(kSeed, kHalfExtentM, kFlatHalfM);

    LC_CHECK_EQ(HashOf(a), HashOf(b));

    // The hash is a summary; confirm cell for cell so a collision could never be what
    // makes this pass.
    i32 firstDifferent = -1;
    for (i32 cy = 0; cy < a.CellsY() && firstDifferent < 0; ++cy) {
        for (i32 cx = 0; cx < a.CellsX(); ++cx) {
            if (a.HeightCm(cx, cy) != b.HeightCm(cx, cy)) {
                firstDifferent = cy * a.CellsX() + cx;
                break;
            }
        }
    }
    LC_CHECK_EQ(firstDifferent, -1);

    // Regenerating on a live object that has been dug into leaves no residue: the world is
    // a function of the seed, not of what the object used to hold.
    LC_CHECK(a.Modify(0, 0, 1000, 300));
    LC_CHECK(a.ModifiedCellCount() > 0u);
    a.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    LC_CHECK_EQ(HashOf(a), HashOf(b));
    LC_CHECK_EQ(a.ModifiedCellCount(), 0u);
    LC_CHECK_EQ(a.Version(), 0ull);
}

LC_TEST(terrain_different_seeds_generate_different_fields) {
    Terrain a;
    Terrain b;
    Terrain c;
    a.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    b.Generate(kSeed + 1u, kHalfExtentM, kFlatHalfM);
    c.Generate(kSeed, kHalfExtentM + 16, kFlatHalfM);   // same seed, different extent

    LC_CHECK_NE(HashOf(a), HashOf(b));
    LC_CHECK_NE(HashOf(a), HashOf(c));

    // Seed 0 is a legal seed and must behave like any other.
    Terrain z;
    z.Generate(0ull, kHalfExtentM, kFlatHalfM);
    LC_CHECK_NE(HashOf(z), HashOf(a));
}

LC_TEST(terrain_field_is_chunk_aligned_centred_and_covers_the_requested_extent) {
    // The renderer builds one mesh per chunk and places it from OriginXCm/OriginYCm. If the
    // field ever ended on a partial chunk, or was not centred, every mesh would be offset
    // from every building by half a field. Both are checked for several extents, including
    // ones that are NOT already a multiple of the chunk size.
    const i32 extents[] = {1, 15, 16, 17, 100, 256, 1000};
    for (const i32 halfM : extents) {
        Terrain t;
        t.Generate(kSeed, halfM, halfM / 2);

        LC_CHECK(t.CellsX() > 0);
        LC_CHECK_EQ(t.CellsX(), t.CellsY());
        LC_CHECK_EQ(t.CellsX() % Terrain::kChunkCells, 0);
        LC_CHECK_EQ(t.ChunkCount(), static_cast<u32>(t.ChunksX()) * static_cast<u32>(t.ChunksY()));
        LC_CHECK_EQ(t.ChunksX() * Terrain::kChunkCells, t.CellsX());

        // Covers the request: the field reaches at least halfM metres from the origin.
        const i64 reachCm = static_cast<i64>(t.CellsX()) * Terrain::kCellSizeCm / 2;
        LC_CHECK(reachCm >= static_cast<i64>(halfM) * 100);

        // Centred: the origin corner is exactly half the field to the negative side.
        LC_CHECK_EQ(t.OriginXCm(), -static_cast<i32>(reachCm));
        LC_CHECK_EQ(t.OriginYCm(), -static_cast<i32>(reachCm));

        // And ChunkIndex agrees with the row-major layout the copy uses.
        LC_CHECK_EQ(t.ChunkIndex(0, 0), 0u);
        LC_CHECK_EQ(t.ChunkIndex(t.ChunksX() - 1, t.ChunksY() - 1), t.ChunkCount() - 1u);
    }
}

// ---------------------------------------------------------------------------------------
// The flat square and the countryside.
// ---------------------------------------------------------------------------------------

LC_TEST(terrain_flat_square_really_is_flat_and_the_countryside_is_not) {
    Terrain t;
    t.Generate(kSeed, kHalfExtentM, kFlatHalfM);

    i32 firstBumpInFlat  = -1;
    i32 firstOutOfBounds = -1;
    i32 positiveOutside  = 0;
    i32 negativeOutside  = 0;
    i32 flatCells        = 0;

    for (i32 cy = 0; cy < t.CellsY(); ++cy) {
        for (i32 cx = 0; cx < t.CellsX(); ++cx) {
            const i32 h = t.HeightCm(cx, cy);
            if (h < Terrain::kMinHeightCm || h > Terrain::kMaxHeightCm) {
                if (firstOutOfBounds < 0) firstOutOfBounds = cy * t.CellsX() + cx;
            }
            if (CellIsFlat(t, cx, cy, kFlatHalfM)) {
                ++flatCells;
                if (h != 0 && firstBumpInFlat < 0) firstBumpInFlat = cy * t.CellsX() + cx;
            } else {
                if (h > 0) ++positiveOutside;
                if (h < 0) ++negativeOutside;
            }
        }
    }

    // Exactly zero, not "nearly": the city's buildings are placed on z = 0.
    LC_CHECK_EQ(firstBumpInFlat, -1);
    LC_CHECK_EQ(firstOutOfBounds, -1);
    // The flat square is the 128 m square: 128 cells a side, so this is not vacuous.
    LC_CHECK_EQ(flatCells, 128 * 128);
    // And it is countryside beyond, with hills AND hollows — a check that would still pass
    // on an all-zero field proves nothing.
    LC_CHECK(positiveOutside > 0);
    LC_CHECK(negativeOutside > 0);

    // World positions well inside the flat square read zero through the position lookup too.
    // (Half a cell in from the edge, because a position on the edge maps to a cell whose
    // centre may already be past it.)
    const i32 edge = kFlatHalfM * 100 - Terrain::kCellSizeCm;
    i32 firstBumpByPosition = 0;
    for (i32 y = -edge; y <= edge && firstBumpByPosition == 0; y += 700) {
        for (i32 x = -edge; x <= edge; x += 700) {
            if (t.HeightAtCm(x, y) != 0) { firstBumpByPosition = 1; break; }
        }
    }
    LC_CHECK_EQ(firstBumpByPosition, 0);
}

LC_TEST(terrain_has_no_cliff_anywhere_including_the_city_edge) {
    // The blend ramp exists so the city does not sit on a plateau. The property that
    // guarantees it is a bound on the step between any two adjacent cells, everywhere —
    // inside the noise, across the ramp, and at the flat boundary. Half a cell (a 1:2 slope)
    // is generous against what the lattice can produce, and any cliff at all would break it.
    Terrain t;
    t.Generate(kSeed, kHalfExtentM, kFlatHalfM);

    i32 worstStep  = 0;
    i32 worstIndex = -1;
    for (i32 cy = 0; cy < t.CellsY(); ++cy) {
        for (i32 cx = 0; cx < t.CellsX(); ++cx) {
            const i32 h = t.HeightCm(cx, cy);
            if (cx + 1 < t.CellsX()) {
                const i32 step = static_cast<i32>(Abs64(static_cast<i64>(t.HeightCm(cx + 1, cy)) - h));
                if (step > worstStep) { worstStep = step; worstIndex = cy * t.CellsX() + cx; }
            }
            if (cy + 1 < t.CellsY()) {
                const i32 step = static_cast<i32>(Abs64(static_cast<i64>(t.HeightCm(cx, cy + 1)) - h));
                if (step > worstStep) { worstStep = step; worstIndex = cy * t.CellsX() + cx; }
            }
        }
    }
    LC_CHECK(worstStep <= Terrain::kCellSizeCm / 2);
    // Reported alongside so a failure says WHERE, not merely that a cliff exists somewhere.
    if (worstStep > Terrain::kCellSizeCm / 2) LC_CHECK_EQ(worstIndex, -1);
}

LC_TEST(terrain_growing_the_flat_square_does_not_reroll_the_countryside) {
    // Enlarging the flat square (the city grew) must not change a single hill beyond the
    // new blend ring. If it did, every saved world would shift the moment the city expanded,
    // and the delta save (R5) would be diffing against a different seed's countryside.
    Terrain small;
    Terrain large;
    small.Generate(kSeed, kHalfExtentM, 64);
    large.Generate(kSeed, kHalfExtentM, kFlatHalfM);

    // Beyond the larger flat square plus its blend ring, both fields must agree exactly.
    // The ring is at most a handful of cells; 16 cells (32 m) is safely past it.
    const i64 farCm = static_cast<i64>(kFlatHalfM) * 100 + 16 * Terrain::kCellSizeCm;

    i32 firstDifferent = -1;
    i32 compared       = 0;
    for (i32 cy = 0; cy < small.CellsY() && firstDifferent < 0; ++cy) {
        for (i32 cx = 0; cx < small.CellsX(); ++cx) {
            const bool far = Abs64(CentreX(small, cx)) > farCm || Abs64(CentreY(small, cy)) > farCm;
            if (!far) continue;
            ++compared;
            if (small.HeightCm(cx, cy) != large.HeightCm(cx, cy)) {
                firstDifferent = cy * small.CellsX() + cx;
                break;
            }
        }
    }
    LC_CHECK(compared > 0);
    LC_CHECK_EQ(firstDifferent, -1);
    // And the fields ARE different overall, or the test compared two identical things.
    LC_CHECK_NE(HashOf(small), HashOf(large));
}

// ---------------------------------------------------------------------------------------
// The brush.
// ---------------------------------------------------------------------------------------

LC_TEST(terrain_brush_is_exact_at_the_centre_zero_at_the_rim_and_symmetric) {
    Terrain t;
    t.Generate(kSeed, kHalfExtentM, kFlatHalfM);

    // Brush centred exactly on a cell centre in the middle of the flat square, so every
    // starting height is 0 and the result IS the falloff.
    const i32 cx = t.CellsX() / 2;
    const i32 cy = t.CellsY() / 2;
    const i32 x  = CentreX(t, cx);
    const i32 y  = CentreY(t, cy);
    LC_CHECK(CellIsFlat(t, cx, cy, kFlatHalfM));

    const i32 radius = 4 * Terrain::kCellSizeCm;   // 800 cm: the rim lands on a cell centre
    LC_CHECK(t.Modify(x, y, radius, 100));

    // Centre: exactly the delta. Rim (distance == radius): exactly nothing. Beyond: nothing.
    LC_CHECK_EQ(t.HeightCm(cx, cy), 100);
    LC_CHECK_EQ(t.HeightCm(cx + 4, cy), 0);
    LC_CHECK_EQ(t.HeightCm(cx - 4, cy), 0);
    LC_CHECK_EQ(t.HeightCm(cx, cy + 4), 0);
    LC_CHECK_EQ(t.HeightCm(cx, cy - 4), 0);
    LC_CHECK_EQ(t.HeightCm(cx + 5, cy), 0);
    LC_CHECK_EQ(t.HeightCm(cx + 4, cy + 4), 0);

    // The falloff is linear in distance: weight = (radius - d) * 1024 / radius, so at 1, 2
    // and 3 cells out along an axis it is 768, 512 and 256 thousandth-and-a-bits, and
    // delta * weight / 1024 is 75, 50 and 25. Derived by hand from the header's stated rule,
    // not from running the code.
    LC_CHECK_EQ(t.HeightCm(cx + 1, cy), 75);
    LC_CHECK_EQ(t.HeightCm(cx + 2, cy), 50);
    LC_CHECK_EQ(t.HeightCm(cx + 3, cy), 25);

    // Symmetric in x and y, and under reflection: eight-fold. No float means no reason for
    // (dx, dy) and (dy, dx) to ever disagree, and this is what pins it.
    i32 firstAsymmetric = -1;
    for (i32 k = 1; k <= 4 && firstAsymmetric < 0; ++k) {
        for (i32 m = 0; m <= k; ++m) {
            const i32 ref = t.HeightCm(cx + k, cy + m);
            if (t.HeightCm(cx - k, cy + m) != ref || t.HeightCm(cx + k, cy - m) != ref ||
                t.HeightCm(cx - k, cy - m) != ref || t.HeightCm(cx + m, cy + k) != ref ||
                t.HeightCm(cx - m, cy + k) != ref || t.HeightCm(cx + m, cy - k) != ref ||
                t.HeightCm(cx - m, cy - k) != ref) {
                firstAsymmetric = k * 10 + m;
                break;
            }
        }
    }
    LC_CHECK_EQ(firstAsymmetric, -1);

    // A second identical brush adds the same amounts again: the brush is additive, not a
    // "set to" — piling twice gives twice the pile.
    std::vector<i32> afterOne;
    SnapshotHeights(t, afterOne);
    LC_CHECK(t.Modify(x, y, radius, 100));
    LC_CHECK_EQ(t.HeightCm(cx, cy), 200);
    i32 firstNotDoubled = -1;
    for (i32 dy = -5; dy <= 5 && firstNotDoubled < 0; ++dy) {
        for (i32 dx = -5; dx <= 5; ++dx) {
            if (t.HeightCm(cx + dx, cy + dy) != 2 * At(afterOne, t, cx + dx, cy + dy)) {
                firstNotDoubled = (dy + 5) * 11 + (dx + 5);
                break;
            }
        }
    }
    LC_CHECK_EQ(firstNotDoubled, -1);

    // Digging the same brush twice with the opposite sign restores level ground exactly.
    LC_CHECK(t.Modify(x, y, radius, -100));
    LC_CHECK(t.Modify(x, y, radius, -100));
    LC_CHECK_EQ(t.HeightCm(cx, cy), 0);
    LC_CHECK_EQ(t.ModifiedCellCount(), 0u);
}

LC_TEST(terrain_brush_clamps_at_the_height_bounds_and_reports_when_nothing_moves) {
    Terrain t;
    t.Generate(kSeed, kHalfExtentM, kFlatHalfM);

    const i32 cx = t.CellsX() / 2;
    const i32 cy = t.CellsY() / 2;
    const i32 x  = CentreX(t, cx);
    const i32 y  = CentreY(t, cy);

    // One enormous pile: everything under the brush pins at the ceiling. The delta is large
    // enough that even a rim cell with a falloff weight of 1/1024 clears the whole range in
    // one pass, so "saturated" below means every cell inside the rim, not most of them.
    constexpr i32 kHuge = 1000000000;
    LC_CHECK(t.Modify(x, y, 600, kHuge));
    LC_CHECK_EQ(t.HeightCm(cx, cy), Terrain::kMaxHeightCm);
    LC_CHECK_EQ(t.HeightCm(cx + 2, cy), Terrain::kMaxHeightCm);
    const u64 afterPile = t.Version();

    // Piling more onto a saturated brush changes nothing, says so, and does not bump.
    LC_CHECK_FALSE(t.Modify(x, y, 600, kHuge));
    LC_CHECK_FALSE(t.Modify(x, y, 400, 50));
    LC_CHECK_EQ(t.Version(), afterPile);

    // Digging it out overshoots to the floor.
    LC_CHECK(t.Modify(x, y, 600, -kHuge));
    LC_CHECK_EQ(t.HeightCm(cx, cy), Terrain::kMinHeightCm);
    LC_CHECK_FALSE(t.Modify(x, y, 600, -kHuge));
    LC_CHECK_FALSE(t.Modify(x, y, 400, -1));
    LC_CHECK_EQ(t.Version(), afterPile + 1u);

    // The cells the brush never reached are untouched by any of it.
    LC_CHECK_EQ(t.HeightCm(cx + 8, cy), 0);
    LC_CHECK_EQ(t.HeightCm(cx, cy - 8), 0);

    // Degenerate brushes: no radius, no delta, or nowhere near the field. All refused.
    const u64 before = t.Version();
    LC_CHECK_FALSE(t.Modify(x, y, 0, 100));
    LC_CHECK_FALSE(t.Modify(x, y, -300, 100));
    LC_CHECK_FALSE(t.Modify(x, y, 300, 0));
    LC_CHECK_FALSE(t.Modify(t.OriginXCm() - 100000, 0, 500, 100));
    LC_CHECK_FALSE(t.Modify(0, -t.OriginYCm() + 100000, 500, 100));
    LC_CHECK_EQ(t.Version(), before);

    // A brush that overhangs the field's edge touches the cells that ARE on the field and
    // nothing else — it neither refuses nor reads off the end.
    const i32 cornerX = CentreX(t, 0);
    const i32 cornerY = CentreY(t, 0);
    const i32 wasCorner = t.HeightCm(0, 0);
    LC_CHECK(t.Modify(cornerX - 300, cornerY - 300, 700, 100));
    LC_CHECK_NE(t.HeightCm(0, 0), wasCorner);

    // And a brush whose delta is too small to move any cell after falloff is "nothing".
    // delta 1 at 1 cell out of a 4-cell radius is 1 * 768 / 1024 == 0; only the centre moves.
    const i32 mx = t.CellsX() / 4;
    const i32 my = t.CellsY() / 4;
    const i32 wasNext = t.HeightCm(mx + 1, my);
    LC_CHECK(t.Modify(CentreX(t, mx), CentreY(t, my), 800, 1));
    LC_CHECK_EQ(t.HeightCm(mx + 1, my), wasNext);
}

LC_TEST(terrain_brush_overhanging_any_edge_moves_exactly_the_cells_inside_the_disc) {
    // The heights are one flat array of rows. A brush clipped one cell too generously at the
    // +X edge would not read off the end of anything visible: it would write into the FIRST
    // cell of the next row, a whole field away from the brush, and the corner-of-the-field
    // check elsewhere (which overhangs the low side only) would never see it. So: brushes
    // overhanging every edge and every corner, and for each one the full field is diffed
    // against the brush's own rule — a cell moved if and only if its centre is inside the
    // disc and the falloff leaves it a non-zero delta.
    Terrain t;
    t.Generate(kSeed, kHalfExtentM, kFlatHalfM);

    const i32 minX = t.OriginXCm();
    const i32 maxX = -t.OriginXCm() - 1;   // last centimetre on the field
    const i32 minY = t.OriginYCm();
    const i32 maxY = -t.OriginYCm() - 1;
    const i32 radius = 700;                // 3.5 cells: overhangs by two or three cells
    const i32 delta  = 60;                 // small enough that nothing reaches a clamp

    struct Spot { i32 x; i32 y; };
    const Spot spots[] = {
        {maxX + 300, CentreY(t, 100)},   // +X edge, mid-field row
        {minX - 300, CentreY(t, 37)},    // -X edge
        {CentreX(t, 200), maxY + 300},   // +Y edge
        {CentreX(t, 9),   minY - 300},   // -Y edge
        {maxX + 300, maxY + 300},        // the four corners
        {minX - 300, maxY + 300},
        {maxX + 300, minY - 300},
        {minX - 300, minY - 300},
    };

    // Captured before ANY brush, because a spill from the +X-edge brush would otherwise be
    // sitting in `before` by the time the loop ends and the explicit check would compare the
    // spill with itself.
    const i32 spillCellWas = t.HeightCm(0, 101);

    std::vector<i32> before;
    i32 spotIndex = 0;
    for (const Spot& s : spots) {
        SnapshotHeights(t, before);
        LC_CHECK(t.Modify(s.x, s.y, radius, delta));

        i32 firstWrong    = -1;
        i32 changedCount  = 0;
        for (i32 cy = 0; cy < t.CellsY() && firstWrong < 0; ++cy) {
            for (i32 cx = 0; cx < t.CellsX(); ++cx) {
                const i64 dx = static_cast<i64>(CentreX(t, cx)) - s.x;
                const i64 dy = static_cast<i64>(CentreY(t, cy)) - s.y;
                const i64 d2 = dx * dx + dy * dy;

                // The brush's own rule, from the header: linear falloff, full at the centre,
                // zero at the rim, integer throughout.
                i64 expectDelta = 0;
                if (d2 <= static_cast<i64>(radius) * radius) {
                    const i64 d      = ISqrtSmall(d2);
                    const i64 weight = (radius - d) * 1024 / radius;
                    expectDelta      = delta * weight / 1024;
                }

                const i64 actualDelta = static_cast<i64>(t.HeightCm(cx, cy)) - At(before, t, cx, cy);
                if (actualDelta != 0) ++changedCount;
                if (actualDelta != expectDelta) {
                    firstWrong = cy * t.CellsX() + cx;
                    break;
                }
            }
        }
        // Reported with the spot, so a failure says which edge is wrong.
        LC_CHECK_EQ(firstWrong, -1);
        if (firstWrong >= 0) LC_CHECK_EQ(spotIndex, -1);
        LC_CHECK(changedCount > 0);
        ++spotIndex;
    }

    // The +X-edge brush sat on row 100. Had its clip run one cell long, the spill would be
    // cell (0, 101), which no brush in the list reaches. Said explicitly, on top of the full
    // diff above, for the failure message.
    LC_CHECK_EQ(t.HeightCm(0, 101), spillCellWas);
}

// ---------------------------------------------------------------------------------------
// Versions: the renderer's "what do I rebuild" signal.
// ---------------------------------------------------------------------------------------

LC_TEST(terrain_version_counts_effective_brushes_and_only_chunks_whose_copy_moved_bump) {
    Terrain t;
    t.Generate(kSeed, kHalfExtentM, kFlatHalfM);

    LC_CHECK_EQ(t.Version(), 0ull);
    i32 firstNonZero = -1;
    for (u32 c = 0; c < t.ChunkCount(); ++c) {
        if (t.ChunkVersion(c) != 0u && firstNonZero < 0) firstNonZero = static_cast<i32>(c);
    }
    LC_CHECK_EQ(firstNonZero, -1);

    // A sequence of brushes, some effective and some not. Version() must equal the number
    // that returned true — one per effective brush, never per cell.
    //
    // The per-chunk contract is stated in terms of what the renderer actually consumes: the
    // chunk's version moves by exactly one when CopyChunkHeights(chunk) would come back
    // different, and not at all otherwise. Whole-field copies before and after each brush
    // are diffed chunk by chunk, so this is the renderer's view of "did my mesh move", not
    // a re-derivation of which cells belong to which chunk.
    std::vector<i32> copiesBefore;
    std::vector<i32> copiesAfter;
    std::vector<u32> chunkBefore(t.ChunkCount());
    u64 effective = 0;

    struct Brush { i32 x; i32 y; i32 r; i32 d; };
    const Brush brushes[] = {
        // Cell 47 is one cell short of the chunk line at 48, so a 5-cell brush (cells 42..52)
        // straddles chunk 2 and chunk 3 on both axes: four chunks under one brush.
        {CentreX(t, 47),  CentreY(t, 47),  1000, 150},
        {CentreX(t, 47),  CentreY(t, 47),  1000, 150},          // again: still effective
        {CentreX(t, 200), CentreY(t, 60),  300,  -80},
        {CentreX(t, 200), CentreY(t, 60),  300,  0},      // no delta: ineffective
        {CentreX(t, 128), CentreY(t, 128), 2000, 1000000000},   // saturates a big area
        {CentreX(t, 128), CentreY(t, 128), 2000, 1000000000},   // saturated: ineffective
        {CentreX(t, 8),   CentreY(t, 250), 500,  30},
        // A half-cell brush moves exactly one cell. Cell 64 is the FIRST column of chunk 4,
        // which chunk 3's copy borrows as its 17th column: chunk 3 must bump although not
        // one of its own cells moved. Then the same on a chunk corner, where four copies
        // hold the cell.
        {CentreX(t, 64),  CentreY(t, 100), 100,  40},
        {CentreX(t, 96),  CentreY(t, 96),  100,  40},
    };

    for (const Brush& b : brushes) {
        CopyAllChunks(t, copiesBefore);
        for (u32 c = 0; c < t.ChunkCount(); ++c) chunkBefore[c] = t.ChunkVersion(c);

        const bool changed = t.Modify(b.x, b.y, b.r, b.d);
        if (changed) ++effective;
        LC_CHECK_EQ(t.Version(), effective);

        CopyAllChunks(t, copiesAfter);

        // A chunk whose copy moved bumped by EXACTLY one; a chunk whose copy did not move
        // did not bump at all — not even a chunk the brush's bounding box covered.
        i32 firstWrong = -1;
        i32 bumped     = 0;
        for (u32 c = 0; c < t.ChunkCount(); ++c) {
            const bool moved  = ChunkCopyDiffers(copiesBefore, copiesAfter, c);
            const u32  expect = chunkBefore[c] + (moved ? 1u : 0u);
            if (t.ChunkVersion(c) != expect && firstWrong < 0) firstWrong = static_cast<i32>(c);
            if (moved) ++bumped;
        }
        LC_CHECK_EQ(firstWrong, -1);
        LC_CHECK_EQ(bumped > 0, changed);
    }

    LC_CHECK_EQ(effective, 7ull);
    // The first brush straddles chunk boundaries: it must have touched more than one chunk,
    // or the "only touched chunks" half of this test never exercised a multi-chunk brush.
    Terrain u;
    u.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    LC_CHECK(u.Modify(brushes[0].x, brushes[0].y, brushes[0].r, brushes[0].d));
    i32 touched = 0;
    for (u32 c = 0; c < u.ChunkCount(); ++c) {
        if (u.ChunkVersion(c) != 0u) ++touched;
    }
    LC_CHECK(touched >= 4);
}

LC_TEST(terrain_a_cell_on_a_chunk_line_bumps_every_chunk_whose_copy_holds_it) {
    // The renderer recopies a chunk when its version moves and builds the chunk's shared edge
    // from that copy. A cell on the first row or column of a chunk therefore sits in more
    // than one mesh, and every one of those meshes has to be told. Spelled out by chunk
    // coordinate so a failure names the neighbour that was forgotten.
    Terrain t;
    t.Generate(kSeed, kHalfExtentM, kFlatHalfM);

    // One cell, (80, 100): first column of chunk (5, 6), well inside the flat square so its
    // neighbours start at exactly 0 (cell 63, by contrast, is the first cell of the blend
    // ring). Copies holding it: (5, 6) and (4, 6).
    LC_CHECK(CellIsFlat(t, 79, 100, kFlatHalfM));
    LC_CHECK(t.Modify(CentreX(t, 80), CentreY(t, 100), 100, 40));
    LC_CHECK_EQ(t.HeightCm(80, 100), 40);
    LC_CHECK_EQ(t.HeightCm(79, 100), 0);
    LC_CHECK_EQ(t.HeightCm(81, 100), 0);
    LC_CHECK_EQ(t.ChunkVersion(t.ChunkIndex(5, 6)), 1u);
    LC_CHECK_EQ(t.ChunkVersion(t.ChunkIndex(4, 6)), 1u);
    LC_CHECK_EQ(t.ChunkVersion(t.ChunkIndex(5, 5)), 0u);
    LC_CHECK_EQ(t.ChunkVersion(t.ChunkIndex(4, 5)), 0u);
    LC_CHECK_EQ(t.ChunkVersion(t.ChunkIndex(6, 6)), 0u);
    i32 bumped = 0;
    for (u32 c = 0; c < t.ChunkCount(); ++c) {
        if (t.ChunkVersion(c) != 0u) ++bumped;
    }
    LC_CHECK_EQ(bumped, 2);

    // And chunk (4, 6)'s copy really does carry the new height in its borrowed column, so
    // the bump was for something the renderer would have drawn wrong otherwise.
    i32 copy[kCopyCount];
    t.CopyChunkHeights(t.ChunkIndex(4, 6), copy);
    LC_CHECK_EQ(copy[(100 - 6 * Terrain::kChunkCells) * kCopyStride + Terrain::kChunkCells], 40);

    // One cell on a chunk corner, (96, 96): first row AND column of chunk (6, 6). Four copies.
    Terrain u;
    u.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    LC_CHECK(u.Modify(CentreX(u, 96), CentreY(u, 96), 100, 40));
    LC_CHECK_EQ(u.ChunkVersion(u.ChunkIndex(6, 6)), 1u);
    LC_CHECK_EQ(u.ChunkVersion(u.ChunkIndex(5, 6)), 1u);
    LC_CHECK_EQ(u.ChunkVersion(u.ChunkIndex(6, 5)), 1u);
    LC_CHECK_EQ(u.ChunkVersion(u.ChunkIndex(5, 5)), 1u);
    i32 cornerBumped = 0;
    for (u32 c = 0; c < u.ChunkCount(); ++c) {
        if (u.ChunkVersion(c) != 0u) ++cornerBumped;
    }
    LC_CHECK_EQ(cornerBumped, 4);

    // Cell (0, 0) is on the field's own corner: there is no chunk to its left or above, and
    // nothing may try to bump one.
    Terrain v;
    v.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    LC_CHECK(v.Modify(CentreX(v, 0), CentreY(v, 0), 100, 40));
    i32 edgeBumped = 0;
    for (u32 c = 0; c < v.ChunkCount(); ++c) {
        if (v.ChunkVersion(c) != 0u) ++edgeBumped;
    }
    LC_CHECK_EQ(edgeBumped, 1);
    LC_CHECK_EQ(v.ChunkVersion(v.ChunkIndex(0, 0)), 1u);
}

LC_TEST(terrain_hash_covers_heights_but_not_brush_history) {
    // Two fields with identical heights reached by different histories are the same world.
    // The versions are renderer bookkeeping and a save/load restarts them; if they were in
    // the hash, I7 (hash(load(save(w))) == hash(w)) could never hold.
    Terrain a;
    Terrain b;
    a.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    b.Generate(kSeed, kHalfExtentM, kFlatHalfM);

    LC_CHECK(a.Modify(500, 500, 900, 120));
    LC_CHECK(a.Modify(500, 500, 900, -120));
    LC_CHECK_EQ(a.Version(), 2ull);
    LC_CHECK_EQ(b.Version(), 0ull);
    LC_CHECK_EQ(HashOf(a), HashOf(b));

    // And a real change IS in the hash.
    LC_CHECK(a.Modify(500, 500, 900, 7));
    LC_CHECK_NE(HashOf(a), HashOf(b));
}

// ---------------------------------------------------------------------------------------
// Chunk copies: the render thread's read.
// ---------------------------------------------------------------------------------------

LC_TEST(terrain_adjacent_chunk_copies_share_their_common_edge_exactly) {
    Terrain t;
    t.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    // Rough it up so the shared edges are not trivially zero.
    LC_CHECK(t.Modify(CentreX(t, 32), CentreY(t, 32), 3000, 400));
    LC_CHECK(t.Modify(CentreX(t, 33), CentreY(t, 17), 1500, -250));

    i32 left[kCopyCount];
    i32 right[kCopyCount];
    i32 below[kCopyCount];

    // Chunk (1,1) against (2,1) to its right and (1,2) below it.
    t.CopyChunkHeights(t.ChunkIndex(1, 1), left);
    t.CopyChunkHeights(t.ChunkIndex(2, 1), right);
    t.CopyChunkHeights(t.ChunkIndex(1, 2), below);

    i32 firstSeamX = -1;
    i32 firstSeamY = -1;
    for (i32 k = 0; k < kCopyStride; ++k) {
        // left's last column is right's first column, row by row.
        if (left[k * kCopyStride + Terrain::kChunkCells] != right[k * kCopyStride + 0] && firstSeamX < 0) {
            firstSeamX = k;
        }
        // left's last row is below's first row, column by column.
        if (left[Terrain::kChunkCells * kCopyStride + k] != below[0 * kCopyStride + k] && firstSeamY < 0) {
            firstSeamY = k;
        }
    }
    LC_CHECK_EQ(firstSeamX, -1);
    LC_CHECK_EQ(firstSeamY, -1);

    // And each copy is exactly the cells it claims to be, so the edge agreement above is
    // agreement about the RIGHT cells and not two copies of the same wrong thing.
    i32 firstMismatch = -1;
    for (i32 j = 0; j < kCopyStride && firstMismatch < 0; ++j) {
        for (i32 i = 0; i < kCopyStride; ++i) {
            const i32 cx = 1 * Terrain::kChunkCells + i;
            const i32 cy = 1 * Terrain::kChunkCells + j;
            if (left[j * kCopyStride + i] != t.HeightCm(cx, cy)) { firstMismatch = j * kCopyStride + i; break; }
        }
    }
    LC_CHECK_EQ(firstMismatch, -1);

    // At the field's far edge there is no neighbour: the extra row and column repeat the
    // last row and column rather than reading past the end.
    i32 corner[kCopyCount];
    t.CopyChunkHeights(t.ChunkIndex(t.ChunksX() - 1, t.ChunksY() - 1), corner);
    const i32 lastX = t.CellsX() - 1;
    const i32 lastY = t.CellsY() - 1;
    i32 firstEdgeWrong = -1;
    for (i32 k = 0; k < kCopyStride && firstEdgeWrong < 0; ++k) {
        const i32 cx = (t.ChunksX() - 1) * Terrain::kChunkCells + ((k < Terrain::kChunkCells) ? k : Terrain::kChunkCells - 1);
        const i32 cy = (t.ChunksY() - 1) * Terrain::kChunkCells + ((k < Terrain::kChunkCells) ? k : Terrain::kChunkCells - 1);
        if (corner[k * kCopyStride + Terrain::kChunkCells] != t.HeightCm(lastX, cy)) firstEdgeWrong = k;
        if (corner[Terrain::kChunkCells * kCopyStride + k] != t.HeightCm(cx, lastY)) firstEdgeWrong = k;
    }
    LC_CHECK_EQ(firstEdgeWrong, -1);
}

// ---------------------------------------------------------------------------------------
// R5 — the modified set.
// ---------------------------------------------------------------------------------------

LC_TEST(terrain_modified_cells_are_exactly_the_cells_a_brush_changed) {
    Terrain t;
    t.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    LC_CHECK_EQ(t.ModifiedCellCount(), 0u);

    std::vector<i32> generated;
    SnapshotHeights(t, generated);

    // Two brushes, overlapping, one of them out in the noise where starting heights are not
    // zero, so "changed" means "differs from the seed" and not "is non-zero".
    LC_CHECK(t.Modify(CentreX(t, 200), CentreY(t, 200), 1400, 90));
    LC_CHECK(t.Modify(CentreX(t, 205), CentreY(t, 203), 900, -60));

    // Count from the heights, independently of the implementation's own walk.
    u32 expectedCount = 0;
    for (i32 cy = 0; cy < t.CellsY(); ++cy) {
        for (i32 cx = 0; cx < t.CellsX(); ++cx) {
            if (t.HeightCm(cx, cy) != At(generated, t, cx, cy)) ++expectedCount;
        }
    }
    LC_CHECK(expectedCount > 0u);
    LC_CHECK_EQ(t.ModifiedCellCount(), expectedCount);

    // Every reported cell: ascending index, really differs from the seed, and carries its
    // CURRENT height — the value a save has to store.
    i32 firstNotAscending = -1;
    i32 firstNotModified  = -1;
    i32 firstWrongHeight  = -1;
    i64 previousIndex     = -1;
    for (u32 n = 0; n < t.ModifiedCellCount(); ++n) {
        i32 cx = -1, cy = -1, h = 0;
        t.ModifiedCell(n, cx, cy, h);
        const i64 index = static_cast<i64>(cy) * t.CellsX() + cx;
        if (index <= previousIndex && firstNotAscending < 0) firstNotAscending = static_cast<i32>(n);
        previousIndex = index;
        if (t.HeightCm(cx, cy) == At(generated, t, cx, cy) && firstNotModified < 0) firstNotModified = static_cast<i32>(n);
        if (h != t.HeightCm(cx, cy) && firstWrongHeight < 0) firstWrongHeight = static_cast<i32>(n);
    }
    LC_CHECK_EQ(firstNotAscending, -1);
    LC_CHECK_EQ(firstNotModified, -1);
    LC_CHECK_EQ(firstWrongHeight, -1);

    // Undoing both brushes exactly empties the set: the save stores divergence, and a cell
    // put back where the seed had it has none, whatever its history.
    LC_CHECK(t.Modify(CentreX(t, 205), CentreY(t, 203), 900, 60));
    LC_CHECK(t.Modify(CentreX(t, 200), CentreY(t, 200), 1400, -90));
    LC_CHECK_EQ(t.ModifiedCellCount(), 0u);
    Terrain fresh;
    fresh.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    LC_CHECK_EQ(HashOf(t), HashOf(fresh));
}

// ---------------------------------------------------------------------------------------
// World positions.
// ---------------------------------------------------------------------------------------

LC_TEST(terrain_height_at_a_world_position_agrees_with_the_cell_lookup_and_clamps) {
    Terrain t;
    t.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    LC_CHECK(t.Modify(CentreX(t, 3), CentreY(t, 5), 1200, 333));   // rough up a corner too

    // A sweep over the field at an awkward stride, including negative coordinates and
    // positions that are not cell centres.
    i32 firstDisagree  = -1;
    i32 firstNotInside = -1;
    const i32 minCm = t.OriginXCm();
    const i32 maxCm = -t.OriginXCm() - 1;
    for (i32 y = minCm; y <= maxCm && firstDisagree < 0; y += 437) {
        for (i32 x = minCm; x <= maxCm; x += 391) {
            i32 cx = -1, cy = -1;
            const bool inside = t.WorldToCell(x, y, cx, cy);
            if (!inside && firstNotInside < 0) firstNotInside = x;
            if (t.HeightAtCm(x, y) != t.HeightCm(cx, cy)) { firstDisagree = x; break; }
            // And the cell really is the one containing the point.
            const i32 cornerX = t.OriginXCm() + cx * Terrain::kCellSizeCm;
            const i32 cornerY = t.OriginYCm() + cy * Terrain::kCellSizeCm;
            if (x < cornerX || x >= cornerX + Terrain::kCellSizeCm || y < cornerY || y >= cornerY + Terrain::kCellSizeCm) {
                firstDisagree = x;
                break;
            }
        }
    }
    LC_CHECK_EQ(firstDisagree, -1);
    LC_CHECK_EQ(firstNotInside, -1);

    // The field's own corners: the very first centimetre is cell 0, the very last is the
    // last cell, and one past either is outside but clamps to the edge.
    i32 cx = -1, cy = -1;
    LC_CHECK(t.WorldToCell(minCm, minCm, cx, cy));
    LC_CHECK_EQ(cx, 0);
    LC_CHECK_EQ(cy, 0);
    LC_CHECK(t.WorldToCell(maxCm, maxCm, cx, cy));
    LC_CHECK_EQ(cx, t.CellsX() - 1);
    LC_CHECK_EQ(cy, t.CellsY() - 1);

    LC_CHECK_FALSE(t.WorldToCell(minCm - 1, minCm - 1, cx, cy));
    LC_CHECK_EQ(cx, 0);
    LC_CHECK_EQ(cy, 0);
    LC_CHECK_FALSE(t.WorldToCell(maxCm + 1, maxCm + 1, cx, cy));
    LC_CHECK_EQ(cx, t.CellsX() - 1);
    LC_CHECK_EQ(cy, t.CellsY() - 1);

    // Far outside in every direction: the edge cell, never garbage, never an abort.
    LC_CHECK_EQ(t.HeightAtCm(-2000000000, 0), t.HeightCm(0, t.CellsY() / 2));
    LC_CHECK_EQ(t.HeightAtCm(2000000000, 0), t.HeightCm(t.CellsX() - 1, t.CellsY() / 2));
    LC_CHECK_EQ(t.HeightAtCm(0, -2000000000), t.HeightCm(t.CellsX() / 2, 0));
    LC_CHECK_EQ(t.HeightAtCm(0, 2000000000), t.HeightCm(t.CellsX() / 2, t.CellsY() - 1));
    LC_CHECK_EQ(t.HeightAtCm(-2000000000, -2000000000), t.HeightCm(0, 0));
    LC_CHECK_EQ(t.HeightCm(-7, -7), t.HeightCm(0, 0));
    LC_CHECK_EQ(t.HeightCm(t.CellsX() + 9, 4), t.HeightCm(t.CellsX() - 1, 4));

    // A field that was never generated is level ground everywhere and nowhere is inside it.
    Terrain empty;
    LC_CHECK_EQ(empty.HeightAtCm(12345, -6789), 0);
    LC_CHECK_EQ(empty.HeightCm(3, 3), 0);
    LC_CHECK_FALSE(empty.WorldToCell(0, 0, cx, cy));
    LC_CHECK_EQ(empty.ChunkCount(), 0u);
    LC_CHECK_EQ(empty.ModifiedCellCount(), 0u);
    LC_CHECK_FALSE(empty.Modify(0, 0, 500, 100));
}

LC_TEST(terrain_clear_empties_the_field_and_a_cleared_field_hashes_like_a_fresh_one) {
    Terrain t;
    t.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    LC_CHECK(t.Modify(0, 0, 900, 100));
    LC_CHECK(t.ChunkCount() > 0u);

    t.Clear();

    LC_CHECK_EQ(t.CellsX(), 0);
    LC_CHECK_EQ(t.CellsY(), 0);
    LC_CHECK_EQ(t.ChunkCount(), 0u);
    LC_CHECK_EQ(t.Version(), 0ull);
    LC_CHECK_EQ(t.ModifiedCellCount(), 0u);
    LC_CHECK_EQ(t.OriginXCm(), 0);
    LC_CHECK_EQ(t.OriginYCm(), 0);

    Terrain fresh;
    LC_CHECK_EQ(HashOf(t), HashOf(fresh));

    // And the cleared object regenerates identically to a new one.
    t.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    fresh.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    LC_CHECK_EQ(HashOf(t), HashOf(fresh));
}

// ---------------------------------------------------------------------------------------
// Threads: the sim digs while the renderer copies.
// ---------------------------------------------------------------------------------------

LC_TEST(terrain_threaded_modify_and_chunk_copy_never_tear) {
    // One thread applies brushes at random spots, the other copies every chunk over and
    // over. A copy made through a half-applied brush would be the bug; the observable
    // symptom the test can pin without knowing the brush is that every value in every copy
    // is a legal height, and that once the writer is quiet the copies are exact.
    //
    // The worker threads never call LC_CHECK (the failure path allocates a std::string and
    // pushes into a shared vector). Findings come back through atomics, asserted after join.
    static constexpr u32 kBrushes = 500u;

    Terrain t;
    t.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    const u32 chunkCount = t.ChunkCount();
    const i64 originCm   = t.OriginXCm();
    const i64 spanCm     = static_cast<i64>(t.CellsX()) * Terrain::kCellSizeCm;

    std::atomic<bool> writerDone{false};
    std::atomic<u32>  outOfBounds{0};
    std::atomic<u32>  effective{0};
    std::atomic<u64>  copies{0};

    std::thread writer([&]() {
        // A local LCG: the sim's Rng is not needed here, and the test must not draw from a
        // stream a shipping system owns. Positions overhang the field edge on purpose.
        u64 lcg     = 0x9E3779B97F4A7C15ull;
        u32 applied = 0;
        for (u32 i = 0; i < kBrushes; ++i) {
            lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
            const i64 px = originCm - 1000 + static_cast<i64>((lcg >> 33) % static_cast<u64>(spanCm + 2000));
            lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
            const i64 py = originCm - 1000 + static_cast<i64>((lcg >> 33) % static_cast<u64>(spanCm + 2000));
            lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
            const i32 radius = 200 + static_cast<i32>((lcg >> 33) % 3800u);
            lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
            const i32 delta = static_cast<i32>((lcg >> 33) % 601u) - 300;
            if (t.Modify(static_cast<i32>(px), static_cast<i32>(py), radius, delta)) ++applied;
        }
        effective.store(applied, std::memory_order_relaxed);
        writerDone.store(true, std::memory_order_release);
    });

    std::thread reader([&]() {
        i32 out[kCopyCount];
        for (;;) {
            // Read the flag BEFORE the sweep, so the final sweep is guaranteed to run after
            // the writer's last brush.
            const bool done = writerDone.load(std::memory_order_acquire);
            for (u32 c = 0; c < chunkCount; ++c) {
                t.CopyChunkHeights(c, out);
                u32 bad = 0;
                for (i32 k = 0; k < kCopyCount; ++k) {
                    if (out[k] < Terrain::kMinHeightCm || out[k] > Terrain::kMaxHeightCm) ++bad;
                }
                if (bad != 0u) outOfBounds.fetch_add(bad, std::memory_order_relaxed);
                (void)t.ChunkVersion(c);   // the other render-thread read, under load
                (void)t.Version();
                copies.fetch_add(1u, std::memory_order_relaxed);
            }
            if (done) break;
        }
    });

    writer.join();
    reader.join();

    LC_CHECK_EQ(outOfBounds.load(std::memory_order_relaxed), 0u);
    LC_CHECK(copies.load(std::memory_order_relaxed) >= static_cast<u64>(chunkCount));
    LC_CHECK(effective.load(std::memory_order_relaxed) > 0u);

    // Quiescent now: the version is exactly the number of effective brushes, every chunk
    // copy is exact, and every height is in bounds by the sim thread's own reading too.
    LC_CHECK_EQ(t.Version(), static_cast<u64>(effective.load(std::memory_order_relaxed)));

    i32 out[kCopyCount];
    i32 firstInexact = -1;
    for (u32 c = 0; c < chunkCount && firstInexact < 0; ++c) {
        t.CopyChunkHeights(c, out);
        const i32 baseX = static_cast<i32>(c % static_cast<u32>(t.ChunksX())) * Terrain::kChunkCells;
        const i32 baseY = static_cast<i32>(c / static_cast<u32>(t.ChunksX())) * Terrain::kChunkCells;
        for (i32 j = 0; j < kCopyStride && firstInexact < 0; ++j) {
            for (i32 i = 0; i < kCopyStride; ++i) {
                if (out[j * kCopyStride + i] != t.HeightCm(baseX + i, baseY + j)) {
                    firstInexact = static_cast<i32>(c);
                    break;
                }
            }
        }
    }
    LC_CHECK_EQ(firstInexact, -1);

    // The same 500 brushes on a fresh field, single-threaded, give the same world: the lock
    // protects the copy, and it must not have changed what the brushes did.
    Terrain again;
    again.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    {
        u64 lcg = 0x9E3779B97F4A7C15ull;
        for (u32 i = 0; i < kBrushes; ++i) {
            lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
            const i64 px = originCm - 1000 + static_cast<i64>((lcg >> 33) % static_cast<u64>(spanCm + 2000));
            lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
            const i64 py = originCm - 1000 + static_cast<i64>((lcg >> 33) % static_cast<u64>(spanCm + 2000));
            lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
            const i32 radius = 200 + static_cast<i32>((lcg >> 33) % 3800u);
            lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
            const i32 delta = static_cast<i32>((lcg >> 33) % 601u) - 300;
            again.Modify(static_cast<i32>(px), static_cast<i32>(py), radius, delta);
        }
    }
    LC_CHECK_EQ(HashOf(again), HashOf(t));
}

// ---------------------------------------------------------------------------------------
// I5 — the brush allocates nothing.
// ---------------------------------------------------------------------------------------

LC_TEST(terrain_modify_allocates_nothing) {
    if (!AllocTrackingEnabled()) {
        // Tracking is compiled out (e.g. under UBT). The loop still runs, so an assert or a
        // crash on these paths would still be caught here.
        LC_CHECK(true);
    }

    Terrain t;
    t.Generate(kSeed, kHalfExtentM, kFlatHalfM);   // the ONE place terrain may allocate

    i32 out[kCopyCount];
    u64 sink = 0;
    u32 moved = 0;

    const NoAllocScope guard("Terrain::Modify / CopyChunkHeights / Version steady state");
    // Deliberately no LC_CHECK inside this loop - the failure macros build a std::string.
    for (i32 i = 0; i < 400; ++i) {
        // Brushes of every kind: small, large, saturating, overhanging the edge, refused.
        const i32 x = t.OriginXCm() + (i * 7919) % (t.CellsX() * Terrain::kCellSizeCm + 4000) - 2000;
        const i32 y = t.OriginYCm() + (i * 104729) % (t.CellsY() * Terrain::kCellSizeCm + 4000) - 2000;
        const i32 r = 100 + (i * 31) % 3000;
        const i32 d = ((i % 3) == 0) ? 1000000 : (((i % 3) == 1) ? -45 : 12);
        if (t.Modify(x, y, r, d)) ++moved;

        t.CopyChunkHeights(static_cast<u32>(i) % t.ChunkCount(), out);
        sink += static_cast<u64>(out[i % kCopyCount] + 5000);
        sink += t.Version();
        sink += t.ChunkVersion(static_cast<u32>(i) % t.ChunkCount());
        sink += static_cast<u64>(t.HeightAtCm(x, y) + 5000);
    }
    Hasher h;
    t.HashInto(h);
    sink += h.Value();
    sink += t.ModifiedCellCount();

    LC_CHECK(moved > 0u);
    LC_CHECK(sink > 0ull);
    if (AllocTrackingEnabled()) {
        LC_CHECK_EQ(guard.AllocationsSinceStart(), 0ull);
        LC_CHECK(guard.Clean());
    }
}

// ---------------------------------------------------------------------------------------
// Known answer.
// ---------------------------------------------------------------------------------------

LC_TEST(terrain_cached_hash_matches_live_heights_after_edits_and_regeneration) {
    Terrain t;
    LC_CHECK_EQ(HashOf(t), HashFromLiveHeights(t));
    t.Generate(kSeed, 64, 32);
    LC_CHECK_EQ(HashOf(t), HashFromLiveHeights(t));

    // First/last cells, each side of a chunk seam, and a four-chunk corner. A radius below
    // half a cell edits only the owner; larger brushes exercise the borrowed-edge walk.
    const i32 cells[] = {0, 15, 16, 31, 32, 47, 48, 63};
    for (const i32 cy : cells) {
        for (const i32 cx : cells) {
            LC_CHECK(t.Modify(CentreX(t, cx), CentreY(t, cy), 50, -37));
            LC_CHECK_EQ(HashOf(t), HashFromLiveHeights(t));
            LC_CHECK(t.Modify(CentreX(t, cx), CentreY(t, cy), 650, 71));
            LC_CHECK_EQ(HashOf(t), HashFromLiveHeights(t));
        }
    }

    // Saturating and refused edits must not leave old digests behind or turn render
    // versions into hash state. These checks use the full field after every operation.
    LC_CHECK(t.Modify(0, 0, 30000, 1000000));
    LC_CHECK_EQ(HashOf(t), HashFromLiveHeights(t));
    const u64 saturated = HashOf(t);
    LC_CHECK_FALSE(t.Modify(0, 0, 30000, 1000000));
    LC_CHECK_FALSE(t.Modify(0, 0, 0, -10));
    LC_CHECK_FALSE(t.Modify(100000, 100000, 100, -10));
    LC_CHECK_EQ(HashOf(t), saturated);
    LC_CHECK_EQ(HashOf(t), HashFromLiveHeights(t));
    LC_CHECK(t.Modify(0, 0, 30000, -1000000));
    LC_CHECK_EQ(HashOf(t), HashFromLiveHeights(t));

    // Reusing the object with a different grid size must replace the entire cache.
    t.Generate(kSeed + 1u, 48, 16);
    LC_CHECK_EQ(HashOf(t), HashFromLiveHeights(t));
    t.Clear();
    LC_CHECK_EQ(HashOf(t), HashFromLiveHeights(t));
    t.Generate(kSeed, 64, 32);
    LC_CHECK_EQ(HashOf(t), HashFromLiveHeights(t));
}

LC_TEST(terrain_known_hash_for_seed_1337_at_256m_with_a_128m_flat_square) {
    // Frozen from this implementation by running it once. It is a regression anchor, not a
    // proof: its job is to make ANY change to the generator — a different lattice spacing,
    // a reordered draw, a changed blend, a new field in the hash walk — fail here rather than
    // silently invalidate every saved world and every recorded replay made against it.
    //
    // If you changed the generator on purpose, re-capture this number in the same commit and
    // say so in the message. Do not "fix" it by loosening the test.
    //
    // Re-captured once when HashInto stopped walking every cell and started folding the
    // per-chunk digests instead (D-027). The heights did not change; the walk order did.
    constexpr u64 kExpected = 0x5CA2D5123369CD99ull;

    Terrain t;
    t.Generate(kSeed, kHalfExtentM, kFlatHalfM);
    LC_CHECK_EQ(HashOf(t), kExpected);

    // Pinned alongside the hash so a failure says WHAT moved, not merely that something did.
    LC_CHECK_EQ(t.CellsX(), 256);
    LC_CHECK_EQ(t.CellsY(), 256);
    LC_CHECK_EQ(t.OriginXCm(), -25600);
    LC_CHECK_EQ(t.ChunkCount(), 256u);
    LC_CHECK_EQ(t.HeightCm(128, 128), 0);        // dead centre of the flat square

    // DERIVED, not frozen: the generator draws one lattice height every 8 cells, in
    // [-150, 250], row-major from the Terrain stream of RngStreams(seed), and a cell that
    // sits exactly on a lattice point far outside the blend ring is that draw with nothing
    // interpolated and nothing ramped. So three such cells can be checked against the
    // stream itself. If the hash above moves and these still hold, the draw order and the
    // lattice are intact and it is the blend or the hash walk that changed; if these move
    // too, it is the noise. (The spacing and range mirror Terrain.cpp's lattice constants.)
    constexpr i32 kLatticeCells = 8;
    const i32     latticeN      = t.CellsX() / kLatticeCells + 1;   // 33 per row
    std::vector<i32> draws(static_cast<std::size_t>(latticeN) * static_cast<std::size_t>(latticeN));
    {
        RngStreams streams(kSeed);
        Rng&       rng = streams.Stream(StreamId::Terrain);
        for (std::size_t i = 0; i < draws.size(); ++i) draws[i] = rng.RangeI(-150, 251);
    }
    // (0, 0): the field's corner, lattice point (0, 0), the very first draw.
    LC_CHECK_EQ(t.HeightCm(0, 0), draws[0]);
    // (200, 8): lattice point (25, 1) — the 26th draw of the SECOND row, which pins row-major.
    LC_CHECK_EQ(t.HeightCm(200, 8), draws[static_cast<std::size_t>(1 * latticeN + 25)]);
    // (48, 200): lattice point (6, 25), deep in the rows.
    LC_CHECK_EQ(t.HeightCm(48, 200), draws[static_cast<std::size_t>(25 * latticeN + 6)]);
    // All three are outside the blend ring, or the check above would be ramped: the ring
    // ends 8 cells (1600 cm) past the 12800 cm flat edge, at 14400 cm.
    LC_CHECK(Abs64(CentreX(t, 0)) >= 14400);
    LC_CHECK(Abs64(CentreY(t, 8)) >= 14400);
    LC_CHECK(Abs64(CentreY(t, 200)) >= 14400);
}
