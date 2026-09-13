// Tests for livingcity/world/City.h — the deterministic city layout.
//
// These are invariant tests, not getter tests. What each group actually defends:
//
//   * I1  — a seed pins a city. The known-answer hash at the bottom is frozen: any change
//           to the layout, the zoning, the draw order or the hash walk moves a saved world,
//           so it has to break a test loudly instead of passing quietly.
//   * I4  — a generated city always has somewhere to live and somewhere to work.
//   * I10 — every home and workplace is reachable, which for a grid city reduces to two
//           geometric facts: no footprint sits in a road corridor, and NearestRoadX/Y
//           really do land on a corridor. If either fails, every commuter in the city walks
//           through a wall and nothing anywhere says so. That is the pair of tests below
//           that matter most.
//   * R3  — Clear() actually clears, so regenerating on a live object cannot accumulate
//           residue that would make the second run differ from the first.
#include "TestFramework.h"

#include "livingcity/world/City.h"

#include <cstdint>
#include <cstring>

using namespace lc;

namespace {

// Several shapes, deliberately mixing parities. An origin offset that is only correct for an
// even block count is a real and easy mistake, and it is invisible on a 6x6 grid alone.
struct Layout {
    u64 seed;
    i32 blocksX;
    i32 blocksY;
};

constexpr Layout kLayouts[] = {
    {1337ull,               6, 6},   // even x even
    {2468ull,               5, 7},   // odd  x odd
    {99ull,                 4, 9},   // even x odd
    {0ull,                  8, 3},   // seed 0 is a legal seed and must behave like any other
    {0xFEEDFACEDEADBEEFull, 7, 4},   // odd  x even
};

u64 HashOf(const City& c) {
    Hasher h;
    c.HashInto(h);
    return h.Value();
}

i64 Abs64(i64 v) { return (v < 0) ? -v : v; }

// "No coordinate failed." Far outside any sweep below, because every plausible in-range
// sentinel (-1, 0) is itself a coordinate these tests probe.
constexpr i64 kNoCoord = 1LL << 40;

// The origin offset, re-derived here from the header constants ALONE rather than read back
// out of City. If the implementation ever phases its road grid differently from the way it
// phases its blocks, this independent derivation is what notices.
i32 RoadBaseFor(i32 blocks) { return -((blocks * City::kStride) / 2); }

// Do two axis-aligned boxes share any area? Touching edges is not overlap.
bool Overlaps(const Building& a, const Building& b) {
    return Abs64(static_cast<i64>(a.centreX) - b.centreX) < static_cast<i64>(a.halfX) + b.halfX &&
           Abs64(static_cast<i64>(a.centreY) - b.centreY) < static_cast<i64>(a.halfY) + b.halfY;
}

bool SameStr(const char* a, const char* b) {
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

} // namespace

// ---------------------------------------------------------------------------------------
// I1 — the seed is the city.
// ---------------------------------------------------------------------------------------

LC_TEST(city_same_seed_generates_an_identical_city) {
    for (const Layout& l : kLayouts) {
        City a;
        City b;
        a.Generate(l.seed, l.blocksX, l.blocksY);
        b.Generate(l.seed, l.blocksX, l.blocksY);

        LC_CHECK_EQ(HashOf(a), HashOf(b));
        LC_CHECK_EQ(a.Count(), b.Count());

        // The hash is a summary; confirm the underlying records are field-for-field equal so
        // a hash collision could never be what makes this test pass.
        if (a.Count() != b.Count()) continue;
        i32 firstDifferent = -1;
        for (u32 i = 0; i < a.Count() && firstDifferent < 0; ++i) {
            const Building& x = a.At(i);
            const Building& y = b.At(i);
            if (x.centreX != y.centreX || x.centreY != y.centreY || x.halfX != y.halfX ||
                x.halfY != y.halfY || x.height != y.height || x.blockIndex != y.blockIndex ||
                x.use != y.use || x.palette != y.palette) {
                firstDifferent = static_cast<i32>(i);
            }
        }
        LC_CHECK_EQ(firstDifferent, -1);
    }
}

LC_TEST(city_different_seeds_generate_different_cities) {
    City a;
    City b;
    City c;
    a.Generate(1337ull, 6, 6);
    b.Generate(1338ull, 6, 6);
    c.Generate(1337ull, 6, 7);   // same seed, different shape

    LC_CHECK_NE(HashOf(a), HashOf(b));
    LC_CHECK_NE(HashOf(a), HashOf(c));
}

// ---------------------------------------------------------------------------------------
// Extent and packing.
// ---------------------------------------------------------------------------------------

LC_TEST(city_buildings_stay_inside_the_city_extent) {
    for (const Layout& l : kLayouts) {
        City city;
        city.Generate(l.seed, l.blocksX, l.blocksY);

        const i64 extentX = city.ExtentX();
        const i64 extentY = city.ExtentY();

        i32 firstOutside = -1;
        for (u32 i = 0; i < city.Count() && firstOutside < 0; ++i) {
            const Building& b = city.At(i);
            const i64 reachX = Abs64(static_cast<i64>(b.centreX)) + b.halfX;
            const i64 reachY = Abs64(static_cast<i64>(b.centreY)) + b.halfY;
            if (reachX > extentX || reachY > extentY) firstOutside = static_cast<i32>(i);
        }
        LC_CHECK_EQ(firstOutside, -1);
    }
}

LC_TEST(city_buildings_never_overlap_each_other) {
    // O(n^2) is fine at a few hundred footprints, and it is the only check that actually
    // catches a bad block sub-grid: a sub-grid that lets two lots share a cell produces a
    // city that still hashes stably and still passes every other test here.
    for (const Layout& l : kLayouts) {
        City city;
        city.Generate(l.seed, l.blocksX, l.blocksY);

        i32 collisions = 0;
        i32 firstA = -1;
        i32 firstB = -1;
        for (u32 i = 0; i < city.Count(); ++i) {
            for (u32 j = i + 1u; j < city.Count(); ++j) {
                if (Overlaps(city.At(i), city.At(j))) {
                    ++collisions;
                    if (firstA < 0) {
                        firstA = static_cast<i32>(i);
                        firstB = static_cast<i32>(j);
                    }
                }
            }
        }
        LC_CHECK_EQ(collisions, 0);
        LC_CHECK_EQ(firstA, -1);
        LC_CHECK_EQ(firstB, -1);
    }
}

LC_TEST(city_block_index_matches_the_footprint_position) {
    // Ties blockIndex to geometry rather than trusting it. blockIndex is what the renderer
    // and later the parcel/ownership pass will group by, so a footprint filed under a block
    // it does not sit in is a bug that surfaces much later and much less legibly.
    for (const Layout& l : kLayouts) {
        City city;
        city.Generate(l.seed, l.blocksX, l.blocksY);

        const u32 blockCount = static_cast<u32>(l.blocksX) * static_cast<u32>(l.blocksY);
        const i32 baseX      = RoadBaseFor(l.blocksX);
        const i32 baseY      = RoadBaseFor(l.blocksY);

        i32 firstBad = -1;
        for (u32 i = 0; i < city.Count() && firstBad < 0; ++i) {
            const Building& b = city.At(i);
            if (b.blockIndex >= blockCount) { firstBad = static_cast<i32>(i); break; }

            const i32 bx = static_cast<i32>(b.blockIndex % static_cast<u32>(l.blocksX));
            const i32 by = static_cast<i32>(b.blockIndex / static_cast<u32>(l.blocksX));

            const i32 minX = baseX + bx * City::kStride + City::kRoadWidth / 2;
            const i32 minY = baseY + by * City::kStride + City::kRoadWidth / 2;

            if (b.centreX - b.halfX < minX || b.centreX + b.halfX > minX + City::kBlockSize ||
                b.centreY - b.halfY < minY || b.centreY + b.halfY > minY + City::kBlockSize) {
                firstBad = static_cast<i32>(i);
            }
        }
        LC_CHECK_EQ(firstBad, -1);
    }
}

// ---------------------------------------------------------------------------------------
// I10 — the road grid. These two tests are the ones that protect pathing.
// ---------------------------------------------------------------------------------------

LC_TEST(city_no_building_stands_in_a_road_corridor) {
    // Citizens walk the corridors. A footprint that intrudes into one does not fail loudly:
    // it silently routes a commuter through a wall, every day, for the rest of the run.
    // Probed over the whole footprint boundary rather than at its centre, because a footprint
    // can straddle a corridor edge with its centre still well clear of it.
    for (const Layout& l : kLayouts) {
        City city;
        city.Generate(l.seed, l.blocksX, l.blocksY);

        i32 firstOnRoad = -1;
        for (u32 i = 0; i < city.Count() && firstOnRoad < 0; ++i) {
            const Building& b = city.At(i);
            // Nine points on the footprint itself - centre, corners and edge midpoints - and
            // the same nine one metre further out. The layout promises at least 2 m of air
            // between any wall and any corridor, and the outer ring is what actually holds it
            // to that: a footprint whose wall sat exactly on a corridor boundary would still
            // pass an edge-only probe, and would still put a citizen inside a building the
            // moment they stepped off the kerb.
            for (i32 grow = 0; grow <= 1 && firstOnRoad < 0; ++grow) {
                for (i32 dx = -1; dx <= 1 && firstOnRoad < 0; ++dx) {
                    for (i32 dy = -1; dy <= 1; ++dy) {
                        const i32 px = b.centreX + dx * (b.halfX + grow);
                        const i32 py = b.centreY + dy * (b.halfY + grow);
                        if (city.IsOnRoad(px, py)) {
                            firstOnRoad = static_cast<i32>(i);
                            break;
                        }
                    }
                }
            }
        }
        LC_CHECK_EQ(firstOnRoad, -1);
    }
}

LC_TEST(city_nearest_road_is_idempotent_and_really_is_a_road) {
    // Idempotence is the property pathing leans on: a citizen snaps to the grid once and
    // every later snap of the same point must be a no-op, or a walker oscillates forever
    // between two "nearest" roads.
    for (const Layout& l : kLayouts) {
        City city;
        city.Generate(l.seed, l.blocksX, l.blocksY);

        const i32 baseX  = RoadBaseFor(l.blocksX);
        const i32 baseY  = RoadBaseFor(l.blocksY);
        // Past BOTH extents: sweeping blocksX strides on the Y axis too would leave the far
        // end of a tall city (4x9, say) unvisited on the axis that needed it most.
        const i32 sweep  = ((l.blocksX > l.blocksY) ? l.blocksX : l.blocksY) * City::kStride;

        // These hold the first COORDINATE that failed, so the sentinel cannot be -1 or 0 —
        // both are coordinates this sweep visits, and a failure at x = -1 recorded as -1
        // would be indistinguishable from "nothing failed". kNoCoord is outside the sweep.
        i64 notIdempotent = kNoCoord;
        i64 notOnRoad     = kNoCoord;
        i64 notNearest    = kNoCoord;
        i64 offGrid       = kNoCoord;

        for (i32 v = -sweep; v <= sweep; ++v) {
            const i32 rx = city.NearestRoadX(v);
            const i32 ry = city.NearestRoadY(v);

            if (city.NearestRoadX(rx) != rx || city.NearestRoadY(ry) != ry) {
                if (notIdempotent == kNoCoord) notIdempotent = v;
            }
            // A road centreline is on a road by any axis, so one coordinate on the grid is
            // enough for IsOnRoad to agree — that is exactly how a citizen path is built.
            if (!city.IsOnRoad(rx, v) || !city.IsOnRoad(v, ry)) {
                if (notOnRoad == kNoCoord) notOnRoad = v;
            }
            // Nearest means nearest: neither adjacent centreline may be strictly closer.
            const i64 d0 = Abs64(static_cast<i64>(v) - rx);
            const i64 dm = Abs64(static_cast<i64>(v) - (static_cast<i64>(rx) - City::kStride));
            const i64 dp = Abs64(static_cast<i64>(v) - (static_cast<i64>(rx) + City::kStride));
            if (d0 > dm || d0 > dp) {
                if (notNearest == kNoCoord) notNearest = v;
            }
            // And it must sit on the same lattice the blocks were laid out against.
            if ((rx - baseX) % City::kStride != 0 || (ry - baseY) % City::kStride != 0) {
                if (offGrid == kNoCoord) offGrid = v;
            }
        }

        LC_CHECK_EQ(notIdempotent, kNoCoord);
        LC_CHECK_EQ(notOnRoad, kNoCoord);
        LC_CHECK_EQ(notNearest, kNoCoord);
        LC_CHECK_EQ(offGrid, kNoCoord);
    }
}

LC_TEST(city_nearest_road_holds_at_the_ends_of_the_coordinate_range) {
    // NearestRoad* is total by contract: pathing hands it whatever coordinate it happens to
    // be holding, and both guarantees - the answer is on a road, and snapping it again is a
    // no-op - are claimed for EVERY i32, not merely for coordinates inside the city.
    //
    // That is where an unbounded periodic lattice bites back. A coordinate within half a
    // stride of the end of the i32 range has its true nearest centreline OUTSIDE the range
    // altogether, and returning that by truncation wraps it to the far side of the world:
    // IsOnRoad denies the result, a second snap moves it again, and a citizen sent to walk
    // there crosses the entire coordinate space. The sweep above cannot see any of that,
    // because it only ever visits coordinates near the city.
    for (const Layout& l : kLayouts) {
        City city;
        city.Generate(l.seed, l.blocksX, l.blocksY);

        const i64 baseX = RoadBaseFor(l.blocksX);
        const i64 baseY = RoadBaseFor(l.blocksY);

        i64 notIdempotent = kNoCoord;
        i64 notOnRoad     = kNoCoord;
        i64 offGrid       = kNoCoord;
        i64 tooFar        = kNoCoord;

        // Counted in i64 on purpose: `for (i32 v = INT32_MAX - n; v <= INT32_MAX; ++v)` never
        // terminates and its last increment is undefined behaviour. One full stride in from
        // each end brackets every coordinate whose nearest road can leave the range.
        for (i64 off = 0; off <= City::kStride; ++off) {
            const i32 probes[2] = {static_cast<i32>(static_cast<i64>(INT32_MAX) - off),
                                   static_cast<i32>(static_cast<i64>(INT32_MIN) + off)};
            for (const i32 v : probes) {
                const i32 rx = city.NearestRoadX(v);
                const i32 ry = city.NearestRoadY(v);

                if (city.NearestRoadX(rx) != rx || city.NearestRoadY(ry) != ry) {
                    if (notIdempotent == kNoCoord) notIdempotent = v;
                }
                if (!city.IsOnRoad(rx, v) || !city.IsOnRoad(v, ry)) {
                    if (notOnRoad == kNoCoord) notOnRoad = v;
                }
                // Still an exact point of the same lattice the blocks were laid out against.
                if ((static_cast<i64>(rx) - baseX) % City::kStride != 0 ||
                    (static_cast<i64>(ry) - baseY) % City::kStride != 0) {
                    if (offGrid == kNoCoord) offGrid = v;
                }
                // It may be one stride short of the true nearest where the true nearest
                // cannot be expressed in an i32 at all. It may never be further away than
                // that, and it may certainly never land on the far side of the origin.
                if (Abs64(static_cast<i64>(v) - rx) > City::kStride ||
                    Abs64(static_cast<i64>(v) - ry) > City::kStride) {
                    if (tooFar == kNoCoord) tooFar = v;
                }
            }
        }

        LC_CHECK_EQ(notIdempotent, kNoCoord);
        LC_CHECK_EQ(notOnRoad, kNoCoord);
        LC_CHECK_EQ(offGrid, kNoCoord);
        LC_CHECK_EQ(tooFar, kNoCoord);
    }

    // And on a city that was never generated, where both block counts are 0. The lattice has
    // to stay a lattice there rather than become a division by the block count.
    City empty;
    const i32 snapped = empty.NearestRoadX(12345);
    LC_CHECK_EQ(empty.NearestRoadX(snapped), snapped);
    LC_CHECK(empty.IsOnRoad(snapped, 0));
}

LC_TEST(city_road_corridors_and_block_interiors_partition_the_grid) {
    // Every metre of a stride belongs to exactly one of the two, and the corridor is
    // kRoadWidth wide. If it were wider the blocks would not fit; if narrower, two footprints
    // in adjacent blocks could touch across what is supposed to be a street.
    City city;
    city.Generate(1337ull, 6, 6);

    const i32 base = RoadBaseFor(6);
    i32 onRoadMetres = 0;
    for (i32 x = base; x < base + City::kStride; ++x) {
        // Probe with a y that is deep inside a block, so only the X axis can report road.
        const i32 yInsideBlock = base + City::kStride / 2;
        if (city.IsOnRoad(x, yInsideBlock)) ++onRoadMetres;
    }
    LC_CHECK_EQ(onRoadMetres, City::kRoadWidth);

    // The junction at the origin-most corner is on a road by both axes.
    LC_CHECK(city.IsOnRoad(base, base));
}

// ---------------------------------------------------------------------------------------
// I4 — zoning produces a city somebody can actually live in.
// ---------------------------------------------------------------------------------------

LC_TEST(city_zoning_supplies_both_homes_and_workplaces) {
    for (const Layout& l : kLayouts) {
        City city;
        city.Generate(l.seed, l.blocksX, l.blocksY);

        const u32 homes = static_cast<u32>(city.OfUse(LotUse::Home).size());
        const u32 jobs  = static_cast<u32>(city.OfUse(LotUse::Work).size());

        // "In quantity", not "at least one": a city with a single house is technically past
        // the I4 guard and still useless as a place to run a population.
        LC_CHECK(homes >= 8u);
        LC_CHECK(jobs >= 4u);
        LC_CHECK(city.Count() >= static_cast<u32>(l.blocksX) * static_cast<u32>(l.blocksY) * 2u);
    }
}

LC_TEST(city_by_use_is_an_ascending_partition_of_every_building) {
    for (const Layout& l : kLayouts) {
        City city;
        city.Generate(l.seed, l.blocksX, l.blocksY);

        u32 total       = 0;
        i32 notAscending = -1;
        i32 outOfRange   = -1;
        i32 wrongUse     = -1;

        for (u32 u = 0; u < static_cast<u32>(LotUse::Count); ++u) {
            const LotUse use = static_cast<LotUse>(u);
            const std::vector<u32>& list = city.OfUse(use);
            total += static_cast<u32>(list.size());

            for (std::size_t k = 0; k < list.size(); ++k) {
                if (list[k] >= city.Count() && outOfRange < 0) outOfRange = static_cast<i32>(u);
                if (k > 0 && list[k] <= list[k - 1] && notAscending < 0) {
                    notAscending = static_cast<i32>(u);
                }
                if (list[k] < city.Count() && city.At(list[k]).use != use && wrongUse < 0) {
                    wrongUse = static_cast<i32>(u);
                }
            }
        }

        // A partition: every building filed exactly once, under its own use.
        LC_CHECK_EQ(total, city.Count());
        LC_CHECK_EQ(notAscending, -1);
        LC_CHECK_EQ(outOfRange, -1);
        LC_CHECK_EQ(wrongUse, -1);
    }
}

LC_TEST(city_has_exactly_one_civic_building_and_it_is_the_tallest) {
    for (const Layout& l : kLayouts) {
        City city;
        city.Generate(l.seed, l.blocksX, l.blocksY);

        LC_CHECK_EQ(static_cast<u32>(city.OfUse(LotUse::Civic).size()), 1u);
        if (city.OfUse(LotUse::Civic).size() != 1u) continue;

        const u32       civicIndex = city.OfUse(LotUse::Civic)[0];
        const Building& civic      = city.At(civicIndex);

        i32 tallerThanCivic = -1;
        i32 nearerThanCivic = -1;
        const i64 civicD = static_cast<i64>(civic.centreX) * civic.centreX +
                           static_cast<i64>(civic.centreY) * civic.centreY;

        for (u32 i = 0; i < city.Count(); ++i) {
            if (i == civicIndex) continue;
            const Building& b = city.At(i);
            if (b.height >= civic.height && tallerThanCivic < 0) {
                tallerThanCivic = static_cast<i32>(i);
            }
            const i64 d = static_cast<i64>(b.centreX) * b.centreX +
                          static_cast<i64>(b.centreY) * b.centreY;
            if (d < civicD && nearerThanCivic < 0) nearerThanCivic = static_cast<i32>(i);
        }

        LC_CHECK_EQ(tallerThanCivic, -1);   // strictly the tallest thing in the city
        LC_CHECK_EQ(nearerThanCivic, -1);   // and the most central building there is
    }
}

LC_TEST(city_zoning_puts_the_tall_work_buildings_in_the_middle) {
    // The point of ring zoning is that the shape of the city is readable from the air. If
    // the core stopped being denser and taller than the edge, the layout would still hash
    // stably and still pass every geometric test above while looking like noise.
    City city;
    city.Generate(1337ull, 8, 8);

    const i64 innerRadius = 2 * City::kStride;   // the central 4x4 blocks, roughly

    i64 innerHeightSum = 0, innerCount = 0;
    i64 outerHeightSum = 0, outerCount = 0;
    u32 innerHomes = 0, outerHomes = 0;

    for (u32 i = 0; i < city.Count(); ++i) {
        const Building& b = city.At(i);
        const bool inner = Abs64(b.centreX) <= innerRadius && Abs64(b.centreY) <= innerRadius;
        if (inner) {
            innerHeightSum += b.height;
            ++innerCount;
            if (b.use == LotUse::Home) ++innerHomes;
        } else {
            outerHeightSum += b.height;
            ++outerCount;
            if (b.use == LotUse::Home) ++outerHomes;
        }
    }

    LC_CHECK(innerCount > 0 && outerCount > 0);
    if (innerCount == 0 || outerCount == 0) return;

    // Mean height, compared as a cross-multiplied integer ratio — no float anywhere, not
    // even in a test (R3 applies to our own tooling).
    LC_CHECK(innerHeightSum * outerCount > outerHeightSum * innerCount);

    // And housing is pushed outward.
    LC_CHECK(static_cast<i64>(outerHomes) * innerCount >
             static_cast<i64>(innerHomes) * outerCount);
}

LC_TEST(city_palette_indices_stay_in_the_art_direction_range) {
    for (const Layout& l : kLayouts) {
        City city;
        city.Generate(l.seed, l.blocksX, l.blocksY);

        i32 firstBad = -1;
        for (u32 i = 0; i < city.Count() && firstBad < 0; ++i) {
            if (city.At(i).palette > 7u) firstBad = static_cast<i32>(i);
        }
        LC_CHECK_EQ(firstBad, -1);
    }
}

// ---------------------------------------------------------------------------------------
// Clear / regenerate.
// ---------------------------------------------------------------------------------------

LC_TEST(city_regenerating_in_place_leaves_no_residue) {
    // If Clear() forgot byUse_, the second Generate would append to it and the city would
    // report twice as many homes as it has — while buildings_ and the hash both looked fine
    // for a while. That is the exact bug this test exists for.
    City city;
    city.Generate(1337ull, 6, 6);

    const u64 firstHash  = HashOf(city);
    const u32 firstCount = city.Count();
    const u32 firstHomes = static_cast<u32>(city.OfUse(LotUse::Home).size());

    city.Generate(1337ull, 6, 6);

    LC_CHECK_EQ(HashOf(city), firstHash);
    LC_CHECK_EQ(city.Count(), firstCount);
    LC_CHECK_EQ(static_cast<u32>(city.OfUse(LotUse::Home).size()), firstHomes);

    // A different shape on the same live object must not inherit anything from the old one.
    City fresh;
    fresh.Generate(2468ull, 5, 7);
    city.Generate(2468ull, 5, 7);
    LC_CHECK_EQ(HashOf(city), HashOf(fresh));
}

LC_TEST(city_clear_empties_every_index_it_owns) {
    City city;
    city.Generate(1337ull, 6, 6);
    LC_CHECK(city.Count() > 0u);

    city.Clear();

    LC_CHECK_EQ(city.Count(), 0u);
    LC_CHECK_EQ(city.BlocksX(), 0);
    LC_CHECK_EQ(city.BlocksY(), 0);
    for (u32 u = 0; u < static_cast<u32>(LotUse::Count); ++u) {
        LC_CHECK_EQ(static_cast<u32>(city.OfUse(static_cast<LotUse>(u)).size()), 0u);
    }

    // An empty city still hashes, and two empty cities agree.
    City other;
    LC_CHECK_EQ(HashOf(city), HashOf(other));
}

LC_TEST(city_generation_does_not_depend_on_what_was_generated_before) {
    // A city is a pure function of (seed, dimensions) and of nothing else. The way that stops
    // being true is a generator that reaches for shared, static or member RNG state instead
    // of deriving its own from the seed: every city would then quietly depend on how many
    // cities preceded it, a world saved in one session would not reload in another, and the
    // known-answer hash below would keep passing purely because the test binary happens to
    // generate things in one fixed order.
    City reference;
    reference.Generate(1337ull, 6, 6);
    const u64 referenceHash = HashOf(reference);

    City busy;
    for (const Layout& l : kLayouts) busy.Generate(l.seed, l.blocksX, l.blocksY);
    busy.Generate(1337ull, 6, 6);
    LC_CHECK_EQ(HashOf(busy), referenceHash);

    // A different object with a different history again, in case the residue hid in the
    // object rather than in a static.
    City fresh;
    fresh.Generate(99ull, 4, 9);
    fresh.Generate(0ull, 8, 3);
    fresh.Generate(1337ull, 6, 6);
    LC_CHECK_EQ(HashOf(fresh), referenceHash);
}

// ---------------------------------------------------------------------------------------
// Naming.
// ---------------------------------------------------------------------------------------

LC_TEST(city_lot_use_names_are_stable) {
    // Compared by content. Comparing two const char* with == would be a pointer comparison,
    // which is exactly the kind of thing allowed to differ between builds (R3).
    LC_CHECK(SameStr(LotUseName(LotUse::Home), "Home"));
    LC_CHECK(SameStr(LotUseName(LotUse::Work), "Work"));
    LC_CHECK(SameStr(LotUseName(LotUse::Shop), "Shop"));
    LC_CHECK(SameStr(LotUseName(LotUse::Civic), "Civic"));
}

// ---------------------------------------------------------------------------------------
// Known answer.
// ---------------------------------------------------------------------------------------

LC_TEST(city_known_hash_for_seed_1337_on_a_6x6_grid) {
    // Frozen from this implementation. It is a regression anchor, not a proof: its job is to
    // make ANY change to the generator — a reordered draw, a different jitter range, a new
    // field in the hash walk — fail here rather than silently invalidate every saved world
    // and every recorded replay that was made against the old layout.
    //
    // If you changed the generator on purpose, re-capture this number in the same commit and
    // say so in the message. Do not "fix" it by loosening the test.
    constexpr u64 kExpected = 0x6B04BB07EB88B13Dull;

    City city;
    city.Generate(1337ull, 6, 6);
    LC_CHECK_EQ(HashOf(city), kExpected);

    // Pinned alongside the hash so a failure says WHAT moved, not merely that something did.
    LC_CHECK_EQ(city.Count(), 109u);
    LC_CHECK_EQ(static_cast<u32>(city.OfUse(LotUse::Home).size()), 73u);
    LC_CHECK_EQ(static_cast<u32>(city.OfUse(LotUse::Work).size()), 26u);
    LC_CHECK_EQ(static_cast<u32>(city.OfUse(LotUse::Shop).size()), 9u);
    LC_CHECK_EQ(static_cast<u32>(city.OfUse(LotUse::Civic).size()), 1u);
}
