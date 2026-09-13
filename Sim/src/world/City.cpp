// LIVING CITY — deterministic city layout. Implements include/livingcity/world/City.h.
//
// R1: zero Unreal types.  R3: every number here is an integer drawn from ONE named RNG
// stream. No float, no wall clock, no unordered iteration, no pointer identity.
//
// ---------------------------------------------------------------------------------------
// GEOMETRY — stated once, so the rest of the file can be read against it.
//
// The road grid is PERIODIC with period kStride, phased by a single origin offset that
// centres the city on (0,0):
//
//     roadBase(n)  = -(n * kStride) / 2                  n = blocksX_ or blocksY_
//     centreline_k = roadBase(n) + k * kStride           k over the integers
//     corridor_k   = [centreline_k - kRoadWidth/2, centreline_k + kRoadWidth/2)
//
// Therefore block index b owns the interior
//
//     [ roadBase(n) + b*kStride + kRoadWidth/2 , + kBlockSize )
//
// and every block is flanked by a road corridor on BOTH sides. A blocksX-wide city has
// blocksX+1 north-south roads; the two outermost are its perimeter roads.
//
// Generate's footprint placement, NearestRoadX/Y and IsOnRoad ALL derive from those three
// lines through the helpers below. They must never be re-derived independently: an
// off-by-one between the placement offset and the pathing offset does not crash, it walks
// every commuter in the city through a wall, for the rest of the run.
//
// The corridor is half-open at the top so corridor and block interior partition the stride
// exactly: metres {-6..5} of a stride are road, metres {6..65} are block interior. No metre
// is both, and no metre is neither.
// ---------------------------------------------------------------------------------------
#include "livingcity/world/City.h"

#include <cstdint>
#include <cstdio>

namespace lc {

namespace {

// ---------------------------------------------------------------- layout constants
// A block is subdivided into a 2x2 sub-grid, and at most one footprint lands in each
// sub-cell. That is what makes "no two buildings overlap" true by construction rather than
// by luck, and it is why the overlap test is a regression guard and not a coin toss.
constexpr i32 kSubCell     = City::kBlockSize / 2;   // 30 m
constexpr i32 kLotMargin   = 2;                      // metres of air around each footprint
constexpr i32 kMinHalf     = 8;                      // footprint half-extents, metres
constexpr i32 kMaxHalf     = 13;
constexpr i32 kMinPerBlock = 2;
constexpr i32 kMaxPerBlock = 4;                      // == the four sub-cells
constexpr i32 kCivicRise   = 10;                     // metres the civic tower clears the rest
constexpr i32 kMaxBlocksPerAxis = 4096;              // keeps every extent inside i32
constexpr i32 kRoadHalf    = City::kRoadWidth / 2;   // 6 m
constexpr u32 kUseCount    = static_cast<u32>(LotUse::Count);

// The placement window inside one sub-cell is [margin + half, subCell - margin - half].
// If the widest footprint did not fit, the jitter range below would go negative and the
// whole "footprints never touch a road" argument would collapse silently. Fail at compile
// time instead of discovering it as a citizen clipping through a wall.
static_assert(kSubCell - 2 * kLotMargin - 2 * kMaxHalf >= 0,
              "widest footprint plus its margins must fit inside a sub-cell");
static_assert(kLotMargin >= 1,
              "a zero margin would let a footprint edge sit on the road corridor boundary");
static_assert(2 * kSubCell == City::kBlockSize, "the sub-grid must tile the block exactly");
static_assert(kMaxPerBlock <= 4, "there are only four sub-cells to place into");
static_assert(kMinPerBlock >= 1 && kMinPerBlock <= kMaxPerBlock, "bad per-block range");
static_assert(City::kRoadWidth % 2 == 0, "a road corridor must split evenly about its centre");

// ---------------------------------------------------------------- the three grid lines
// i64 throughout: a caller may hand NearestRoad* any i32 at all, and `v - base` on a city
// laid out near the ends of the i32 range would otherwise wrap.
i64 RoadBase(i32 blocks) {
    return -((static_cast<i64>(blocks) * static_cast<i64>(City::kStride)) / 2);
}

// Centreline of the corridor nearest to v. Ties — exactly half a stride from two roads —
// resolve upward. Arbitrary, but FIXED, which is all determinism asks of a tie.
i64 NearestRoadLine(i32 v, i32 blocks) {
    const i64 stride = static_cast<i64>(City::kStride);
    const i64 base   = RoadBase(blocks);
    const i64 d      = (static_cast<i64>(v) - base) + stride / 2;
    i64       k      = d / stride;
    if (d % stride < 0) --k;   // floor division; C++ integer division truncates toward zero
    return base + k * stride;
}

// True when v lies in a road corridor along this one axis.
bool OnRoadAxis(i32 v, i32 blocks) {
    const i64 off = static_cast<i64>(v) - NearestRoadLine(v, blocks);
    return off >= -static_cast<i64>(kRoadHalf) && off < static_cast<i64>(kRoadHalf);
}

// A centreline is an i64 because it genuinely can fall outside the i32 range: the lattice is
// unbounded, so any v within half a stride of INT32_MAX has its nearest road PAST the end of
// the type (and likewise at INT32_MIN). Truncating that back to i32 wraps it to the far side
// of the world, and a wrapped centreline is not a lattice point at all: IsOnRoad denies it,
// snapping it again moves it, and a citizen told to walk there crosses the whole coordinate
// space. Both of the guarantees NearestRoad* makes would be false for real i32 inputs.
//
// Step onto the nearest REPRESENTABLE centreline instead. It is still an exact lattice point,
// so it is still on a road and still a fixed point of NearestRoad*; it is merely one stride
// short of the true answer, in the only place where the true answer cannot be said in an i32
// at all. One step always suffices — the true line is never more than half a stride out of
// range — and nothing inside a real city ever reaches this branch.
i32 LineToI32(i64 line) {
    constexpr i64 stride = static_cast<i64>(City::kStride);
    while (line > static_cast<i64>(INT32_MAX)) line -= stride;
    while (line < static_cast<i64>(INT32_MIN)) line += stride;
    return static_cast<i32>(line);
}

// ---------------------------------------------------------------- zoning
// Distance from the centre is what makes a city legible from the air: a dense tall core,
// a mixed middle, low housing at the edge.
enum class Ring : u8 { Inner = 0, Middle = 1, Outer = 2 };

i32 AbsI32(i32 v) { return (v < 0) ? -v : v; }

// Chebyshev distance from the city centre, in DOUBLED block units so that an even block
// count — whose centre falls on a road rather than on a block — needs no half-integer
// arithmetic and therefore no rounding decision to get wrong.
Ring RingOf(i32 bx, i32 by, i32 blocksX, i32 blocksY) {
    const i32 ddx   = AbsI32(2 * bx - (blocksX - 1));
    const i32 ddy   = AbsI32(2 * by - (blocksY - 1));
    const i32 dd    = (ddx > ddy) ? ddx : ddy;
    const i32 ddMax = ((blocksX > blocksY) ? blocksX : blocksY) - 1;

    if (dd * 3 <= ddMax)     return Ring::Inner;
    if (dd * 3 <= ddMax * 2) return Ring::Middle;
    return Ring::Outer;
}

// roll is uniform in [0, 100).
LotUse UseFor(Ring ring, u32 roll) {
    if (ring == Ring::Inner) {
        return (roll < 70u) ? LotUse::Work : ((roll < 95u) ? LotUse::Shop : LotUse::Home);
    }
    if (ring == Ring::Middle) {
        return (roll < 35u) ? LotUse::Work : ((roll < 65u) ? LotUse::Shop : LotUse::Home);
    }
    return (roll < 80u) ? LotUse::Home : ((roll < 95u) ? LotUse::Shop : LotUse::Work);
}

// The first footprint in every block takes the ring's anchor use rather than a die roll.
// That is not decoration: it is what stops a small city from rolling an all-Shop downtown
// and tripping the I4 guard at the bottom of Generate. It also gives every block one
// primary character, which is how real blocks read.
LotUse AnchorFor(Ring ring, i32 bx, i32 by) {
    if (ring == Ring::Inner) return LotUse::Work;
    if (ring == Ring::Outer) return LotUse::Home;
    return (((bx + by) & 1) == 0) ? LotUse::Work : LotUse::Home;
}

i32 HeightFor(Ring ring, Rng& rng) {
    if (ring == Ring::Inner)  return rng.RangeI(18, 46);   // 18..45 m
    if (ring == Ring::Middle) return rng.RangeI(10, 25);   // 10..24 m
    return rng.RangeI(6, 13);                              //  6..12 m
}

// 0..7. Two shades per use, split on height, so the skyline reads as a colour scheme rather
// than as noise: low homes and tall homes are visibly related, and a Work tower never wears
// a Home colour.
u8 PaletteFor(LotUse use, i32 height) {
    if (use == LotUse::Home) return (height >= 9)  ? static_cast<u8>(1) : static_cast<u8>(0);
    if (use == LotUse::Work) return (height >= 20) ? static_cast<u8>(3) : static_cast<u8>(2);
    if (use == LotUse::Shop) return (height >= 15) ? static_cast<u8>(5) : static_cast<u8>(4);
    return (height >= 40) ? static_cast<u8>(7) : static_cast<u8>(6);   // Civic
}

} // namespace

// ---------------------------------------------------------------- LotUseName

const char* LotUseName(LotUse use) {
    switch (use) {
        case LotUse::Home:  return "Home";
        case LotUse::Work:  return "Work";
        case LotUse::Shop:  return "Shop";
        case LotUse::Civic: return "Civic";
        case LotUse::Count: break;
        // Required, not decorative: without it MSVC treats the switch as exhaustive, calls
        // the guard below unreachable and raises C4702 — which Unreal compiles as an error,
        // forcing the deletion of the very check this function needs.
        default: break;
    }
    // LotUse is a u8 enum, so a corrupt save or a bad cast can hand this a value that is
    // none of the four. Returning "Unknown" would put nonsense in a tooltip and hide it;
    // I4 says fail loudly. LC_FATAL is [[noreturn]] — nothing may follow it.
    LC_FATAL("LotUseName: LotUse value is not one of the four uses");
}

// ---------------------------------------------------------------- generation

void City::Generate(u64 worldSeed, i32 blocksX, i32 blocksY) {
    LC_ASSERT_MSG(blocksX > 0 && blocksY > 0,
                  "City::Generate needs at least one block on each axis");
    LC_ASSERT_MSG(blocksX <= kMaxBlocksPerAxis && blocksY <= kMaxBlocksPerAxis,
                  "City::Generate: block count would push the city outside the i32 extent");

    Clear();
    blocksX_ = blocksX;
    blocksY_ = blocksY;

    // R3: draw ONLY from a named stream, and construct the set LOCALLY so that generating a
    // city never advances a stream the caller is also drawing from. Terrain owns top-level
    // layout, per the StreamId table in Rng.h.
    RngStreams streams(worldSeed);
    Rng&       rng = streams.Stream(StreamId::Terrain);

    // I5: one allocation up front for the worst case; none inside the loop.
    buildings_.reserve(static_cast<std::size_t>(blocksX) * static_cast<std::size_t>(blocksY) *
                       static_cast<std::size_t>(kMaxPerBlock));

    const i32 baseX = static_cast<i32>(RoadBase(blocksX));
    const i32 baseY = static_cast<i32>(RoadBase(blocksY));

    // Strict row-major order. The draw sequence is a function of the seed and the dimensions
    // and of nothing else, which is what makes the whole city a pure function of the seed.
    for (i32 by = 0; by < blocksY; ++by) {
        for (i32 bx = 0; bx < blocksX; ++bx) {
            const u32  blockIndex = static_cast<u32>(by) * static_cast<u32>(blocksX) +
                                    static_cast<u32>(bx);
            const Ring ring       = RingOf(bx, by, blocksX, blocksY);

            const i32 blockMinX = baseX + bx * kStride + kRoadHalf;
            const i32 blockMinY = baseY + by * kStride + kRoadHalf;

            const u32 spread = static_cast<u32>(kMaxPerBlock - kMinPerBlock + 1);
            const i32 count  = kMinPerBlock + static_cast<i32>(rng.Range(spread));

            // Which sub-cell the block starts filling from. Without it every under-filled
            // block would empty out toward the same corner and the whole city would list.
            const u32 rot = rng.Range(4u);

            for (i32 i = 0; i < count; ++i) {
                const u32 cell = (rot + static_cast<u32>(i)) & 3u;
                const i32 sx   = static_cast<i32>(cell & 1u);
                const i32 sy   = static_cast<i32>((cell >> 1) & 1u);

                const i32 subMinX = blockMinX + sx * kSubCell;
                const i32 subMinY = blockMinY + sy * kSubCell;

                const i32 halfX = rng.RangeI(kMinHalf, kMaxHalf + 1);
                const i32 halfY = rng.RangeI(kMinHalf, kMaxHalf + 1);

                // Room left inside the sub-cell once the margins and the footprint are paid
                // for. Never negative — the static_assert at the top of the file owns that —
                // so the cast to u32 is safe and Range() always sees a positive n.
                const i32 slackX = kSubCell - 2 * kLotMargin - 2 * halfX;
                const i32 slackY = kSubCell - 2 * kLotMargin - 2 * halfY;
                LC_ASSERT_MSG(slackX >= 0 && slackY >= 0, "footprint overflows its sub-cell");

                const i32 jitterX = static_cast<i32>(rng.Range(static_cast<u32>(slackX) + 1u));
                const i32 jitterY = static_cast<i32>(rng.Range(static_cast<u32>(slackY) + 1u));

                // Drawn unconditionally even where the anchor overrides it, so the number of
                // draws per footprint is constant. A draw that happens on only some branches
                // is the classic way a "harmless" tweak shifts every later decision in a run.
                const u32    roll   = rng.Range(100u);
                const LotUse use    = (i == 0) ? AnchorFor(ring, bx, by) : UseFor(ring, roll);
                const i32    height = HeightFor(ring, rng);

                Building b{};
                b.centreX    = subMinX + kLotMargin + halfX + jitterX;
                b.centreY    = subMinY + kLotMargin + halfY + jitterY;
                b.halfX      = halfX;
                b.halfY      = halfY;
                b.height     = height;
                b.blockIndex = blockIndex;
                b.use        = use;
                b.palette    = PaletteFor(use, height);
                b.pad0       = 0;   // explicit: these bytes are hashed, so they are set
                b.pad1       = 0;
                buildings_.push_back(b);
            }
        }
    }

    LC_ASSERT_MSG(!buildings_.empty(), "City::Generate produced no buildings at all");

    // ---- exactly one civic building: the most central one, and the tallest thing here ----
    // Chosen by squared distance to the origin in i64. At the 4096-block limit a coordinate
    // reaches ~150,000 m, and the square of that overflows i32 long before the sum does.
    {
        std::size_t best    = 0;
        i64         bestD   = 0;
        i32         tallest = 0;
        for (std::size_t i = 0; i < buildings_.size(); ++i) {
            const i64 dx = static_cast<i64>(buildings_[i].centreX);
            const i64 dy = static_cast<i64>(buildings_[i].centreY);
            const i64 d  = dx * dx + dy * dy;
            if (i == 0 || d < bestD) {   // ties keep the lower index: order-stable
                bestD = d;
                best  = i;
            }
            if (buildings_[i].height > tallest) tallest = buildings_[i].height;
        }

        Building& civic = buildings_[best];
        civic.use     = LotUse::Civic;
        civic.height  = tallest + kCivicRise;   // strictly the tallest, by construction
        civic.palette = PaletteFor(LotUse::Civic, civic.height);
    }

    // ---- byUse_, built AFTER the civic rewrite so it can never disagree with it ----------
    // Counted first, then reserved exactly, then filled in ascending building order — the
    // order OfUse() promises and the order every consumer iterates in (R3).
    {
        u32 counts[kUseCount] = {0u, 0u, 0u, 0u};
        for (const Building& b : buildings_) {
            const u32 u = static_cast<u32>(b.use);
            LC_ASSERT_MSG(u < kUseCount, "building carries a use outside LotUse");
            ++counts[u];
        }
        for (u32 u = 0; u < kUseCount; ++u) byUse_[u].reserve(counts[u]);
        for (u32 i = 0; i < Count(); ++i) {
            byUse_[static_cast<u32>(buildings_[i].use)].push_back(i);
        }
    }

    // ---- I4 -----------------------------------------------------------------------------
    // A city with nowhere to live, or nowhere to work, cannot house a single citizen. That
    // is a generator failure, and the one thing it must never do is reach the player as
    // "some of them just stand there". Name the counts so the failure is diagnosable from
    // the abort message alone.
    const u32 homes = static_cast<u32>(byUse_[static_cast<u32>(LotUse::Home)].size());
    const u32 jobs  = static_cast<u32>(byUse_[static_cast<u32>(LotUse::Work)].size());
    if (homes == 0u || jobs == 0u) {
        char msg[192];
        std::snprintf(msg, sizeof(msg),
                      "City::Generate: %u Home and %u Work buildings out of %u across %dx%d "
                      "blocks - a city nobody can live or work in (I4)",
                      homes, jobs, Count(), blocksX, blocksY);
        LC_FATAL(msg);
    }
}

// ---------------------------------------------------------------- queries

const Building& City::At(u32 index) const {
    LC_ASSERT_MSG(static_cast<std::size_t>(index) < buildings_.size(),
                  "City::At: building index out of range");
    return buildings_[index];
}

const std::vector<u32>& City::OfUse(LotUse use) const {
    const u32 u = static_cast<u32>(use);
    LC_ASSERT_MSG(u < kUseCount, "City::OfUse: use out of range");
    return byUse_[(u < kUseCount) ? u : 0u];
}

// ---------------------------------------------------------------- road grid

i32 City::NearestRoadX(i32 x) const { return LineToI32(NearestRoadLine(x, blocksX_)); }
i32 City::NearestRoadY(i32 y) const { return LineToI32(NearestRoadLine(y, blocksY_)); }

bool City::IsOnRoad(i32 x, i32 y) const {
    // EITHER axis. The corridors form a lattice: a point beside one road is on that road,
    // and a junction is on both. A conjunction here would leave only the junctions as road
    // and every citizen would path through the middle of a block.
    return OnRoadAxis(x, blocksX_) || OnRoadAxis(y, blocksY_);
}

// ---------------------------------------------------------------- hashing (I1, I7)

void City::HashInto(Hasher& h) const {
    h.I32(blocksX_);
    h.I32(blocksY_);
    h.U32(Count());
    // Index order, and every field including the two explicit pad bytes. Building is
    // padding-free, so this is byte-for-byte the struct's own image and cannot drift from
    // a byte-wise hash of the same data.
    for (const Building& b : buildings_) {
        h.I32(b.centreX);
        h.I32(b.centreY);
        h.I32(b.halfX);
        h.I32(b.halfY);
        h.I32(b.height);
        h.U32(b.blockIndex);
        h.U8(static_cast<u8>(b.use));
        h.U8(b.palette);
        h.U8(b.pad0);
        h.U8(b.pad1);
    }
}

void City::Clear() {
    buildings_.clear();
    for (u32 u = 0; u < kUseCount; ++u) byUse_[u].clear();
    blocksX_ = 0;
    blocksY_ = 0;
}

} // namespace lc
