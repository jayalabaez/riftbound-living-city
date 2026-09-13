// Tests for livingcity/core/Rng.h.
//
// These are invariant tests, not getter tests. What they are actually defending:
//   * I1 — a seed pins a sequence. Every hard-coded constant below was produced by this
//     implementation and is now frozen: any refactor that changes a single output bit
//     changes a saved world, so it must break a test loudly rather than pass quietly.
//   * R3 — subsystem streams are independent. Adding a draw to one system must not move
//     any other system by one bit.
//   * I7 — generator state round-trips exactly, and is covered by the world hash.
//
// Note on the frozen constants: they were captured from this implementation, so they are
// regression anchors, not proofs of correctness. The one exception is the SplitMix64 test,
// whose expected values come from Vigna's published reference output — that one is a real
// external check that the algorithm is the algorithm it claims to be.
#include "TestFramework.h"

#include "livingcity/core/Rng.h"

#include <cstring>

using namespace lc;

namespace {

// One seed used across the whole file so the frozen vectors are all comparable.
constexpr u64 kSeed = 0x5EEDC0DE12345678ull;

// Strings are compared by content, never by pointer identity — comparing two `const char*`
// with == would be a pointer comparison, which is exactly the kind of thing that is allowed
// to differ between builds.
bool SameStr(const char* a, const char* b) {
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

// Advance `behind` one NextU64 at a time until it matches `ahead`, and report how many
// draws that took. Used to assert exact draw accounting: a method that silently skipped or
// doubled a draw would desync a replay on the very next call, so the count is a contract.
// Returns -1 if the states never converge within the bound.
int DrawsConsumed(const Rng& ahead, Rng behind) {
    u64 a[4];
    u64 b[4];
    ahead.GetState(a);
    for (int d = 0; d <= 16; ++d) {
        behind.GetState(b);
        if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]) return d;
        (void)behind.NextU64();
    }
    return -1;
}

u64 HashOf(const Rng& r) {
    Hasher h;
    r.HashInto(h);
    return h.Value();
}

u64 HashOf(const RngStreams& s) {
    Hasher h;
    s.HashInto(h);
    return h.Value();
}

} // namespace

// ---------------------------------------------------------------------------------------
// rng_detail::RotL underpins every draw, and the header advertises it as reusable by other
// core headers — so its two documented edge cases are pinned here.
//
// These are static_asserts rather than runtime checks on purpose. The failure mode being
// guarded against is `x >> (64 - k)` with k == 0, i.e. a shift by 64, which is undefined
// behaviour: at runtime it silently returns whatever the target's shift instruction does
// (x86 masks the count and yields x, ARM yields 0), so a runtime check can pass on the
// machine that wrote it and fail on the machine that ships. In a constant expression the
// same shift is ill-formed, so a regression here is a compile error on every toolchain.
static_assert(rng_detail::RotL(0x0123456789ABCDEFull, 0u) == 0x0123456789ABCDEFull,
              "RotL(x, 0) must be the identity, never a shift by 64");
static_assert(rng_detail::RotL(0x0123456789ABCDEFull, 64u) == 0x0123456789ABCDEFull,
              "RotL must mask its count to 6 bits, so 64 means 0");
static_assert(rng_detail::RotL(1ull, 1u) == 2ull, "RotL(1, 1)");
static_assert(rng_detail::RotL(1ull, 63u) == 0x8000000000000000ull, "RotL(1, 63)");
static_assert(rng_detail::RotL(0x8000000000000000ull, 1u) == 1ull,
              "RotL must wrap the top bit round to the bottom, not drop it");
// The only two counts xoshiro256** actually uses.
static_assert(rng_detail::RotL(0xDEADBEEFCAFEF00Dull, 7u) == 0x56DF77E57F7806EFull, "RotL 7");
static_assert(rng_detail::RotL(0xDEADBEEFCAFEF00Dull, 45u) == 0xDE01BBD5B7DDF95Full, "RotL 45");

// ---------------------------------------------------------------------------------------
// SplitMix64 — checked against Vigna's published reference output for state 0. This is the
// only external anchor in the file; if it fails, the seeding algorithm itself is wrong.
LC_TEST(rng_splitmix64_matches_published_reference_vectors) {
    SplitMix64 sm(0);
    LC_CHECK_EQ(sm.Next(), 0xE220A8397B1DCDAFull);
    LC_CHECK_EQ(sm.Next(), 0x6E789E6AA1B965F4ull);
    LC_CHECK_EQ(sm.Next(), 0x06C45D188009454Full);
    LC_CHECK_EQ(sm.Next(), 0xF88BB8A8724C81ECull);

    // SplitMix64 must never sit still, all-zero state included — that is exactly the state
    // Rng::Seed(0) hands it, so a fixed point here would collapse the default seed.
    SplitMix64 a(0);
    SplitMix64 b(0);
    LC_CHECK_EQ(a.Next(), b.Next());
    LC_CHECK_NE(a.state, 0ull);
}

// ---------------------------------------------------------------------------------------
// Frozen output. If this test fails, every saved world and every replay log is invalidated.
LC_TEST(rng_known_answer_first_eight_draws) {
    static const u64 kExpected[8] = {
        0xB1AE17F7F7D49CEEull,
        0xAA0332E004A14D8Bull,
        0xD3D1879689D85642ull,
        0x34CCF58ADA2917C1ull,
        0xEA2F0B5C02ACF8F2ull,
        0x433A59E0B00B94C6ull,
        0xB33F975FC90F1CEAull,
        0x0A1288EA4E18608Eull,
    };

    Rng r(kSeed);
    for (u32 i = 0; i < 8u; ++i) {
        LC_CHECK_EQ(r.NextU64(), kExpected[i]);
    }

    // The seeded state itself is frozen too, so a change to the SplitMix64 expansion is
    // caught even if it happened to leave the first outputs alone.
    Rng fresh(kSeed);
    u64 st[4];
    fresh.GetState(st);
    LC_CHECK_EQ(st[0], 0xBFD8250E2741ED3Eull);
    LC_CHECK_EQ(st[1], 0xFF961E7E38882B95ull);
    LC_CHECK_EQ(st[2], 0xD4DE2D95CED88314ull);
    LC_CHECK_EQ(st[3], 0x63C8AC209A54EED3ull);

    // Seed 0 is the default-constructed generator and is worth pinning on its own: it is
    // the seed a forgotten Reseed() call would leave behind.
    Rng zero(0);
    LC_CHECK_EQ(zero.NextU64(), 0x99EC5F36CB75F2B4ull);
    LC_CHECK_EQ(zero.NextU64(), 0xBF6E1F784956452Aull);
    LC_CHECK_EQ(zero.NextU64(), 0x1A5F849D4933E6E0ull);
    LC_CHECK_EQ(zero.NextU64(), 0x6AA594F1262D2D2Cull);

    Rng defaulted;
    Rng explicitZero(0);
    LC_CHECK(defaulted == explicitZero);

    // NextU32 must be the documented high half of NextU64, not an independent draw.
    Rng lo(kSeed);
    Rng hi(kSeed);
    LC_CHECK_EQ(hi.NextU32(), static_cast<u32>(lo.NextU64() >> 32));
    LC_CHECK(lo == hi);
}

// ---------------------------------------------------------------------------------------
LC_TEST(rng_same_seed_gives_identical_sequences) {
    Rng a(kSeed);
    Rng b(kSeed);

    // Interleave every draw method: a divergence in draw accounting between two instances
    // would show up here even if each individual method were self-consistent.
    for (u32 i = 0; i < 2000u; ++i) {
        LC_CHECK_EQ(a.NextU64(), b.NextU64());
        LC_CHECK_EQ(a.NextU32(), b.NextU32());
        LC_CHECK_EQ(a.Range(37u), b.Range(37u));
        LC_CHECK_EQ(a.RangeI(-11, 23), b.RangeI(-11, 23));
        LC_CHECK_EQ(a.NextBool(), b.NextBool());
    }
    LC_CHECK(a == b);
    LC_CHECK_EQ(HashOf(a), HashOf(b));

    // Re-seeding an already-used generator must land back on the fresh state, not on
    // something mixed with the old one.
    a.Seed(kSeed);
    Rng c(kSeed);
    LC_CHECK(a == c);
}

// ---------------------------------------------------------------------------------------
LC_TEST(rng_different_seeds_diverge) {
    // A one-bit seed difference must not survive the SplitMix64 expansion: if it did,
    // neighbouring chunk seeds or citizen seeds would produce visibly correlated worlds.
    Rng a(kSeed);
    Rng b(kSeed ^ 1ull);
    Rng c(kSeed + 1ull);

    u32 abMatches = 0;
    u32 acMatches = 0;
    for (u32 i = 0; i < 256u; ++i) {
        const u64 va = a.NextU64();
        const u64 vb = b.NextU64();
        const u64 vc = c.NextU64();
        if (va == vb) ++abMatches;
        if (va == vc) ++acMatches;
    }
    // Two unrelated 64-bit streams colliding even once in 256 draws is a ~1e-17 event.
    LC_CHECK_EQ(abMatches, 0u);
    LC_CHECK_EQ(acMatches, 0u);

    // And the very first draw must already differ — divergence must not take a warm-up.
    LC_CHECK_NE(Rng(kSeed).NextU64(), Rng(kSeed ^ 1ull).NextU64());
    LC_CHECK_NE(Rng(0).NextU64(), Rng(1).NextU64());
}

// ---------------------------------------------------------------------------------------
// I7: generator state is world state and must survive save/load byte for byte.
LC_TEST(rng_state_round_trips_exactly) {
    Rng r(kSeed);
    for (u32 i = 0; i < 313u; ++i) (void)r.NextU64();   // land on an arbitrary mid-sequence state

    u64 saved[4];
    r.GetState(saved);
    const u64 savedHash = HashOf(r);

    u64 firstPass[100];
    for (u32 i = 0; i < 100u; ++i) firstPass[i] = r.NextU64();

    // Restore into a *different* object: a save/load path never restores in place.
    Rng restored;
    restored.SetState(saved);
    LC_CHECK_EQ(HashOf(restored), savedHash);

    for (u32 i = 0; i < 100u; ++i) {
        LC_CHECK_EQ(restored.NextU64(), firstPass[i]);
    }
    LC_CHECK(restored == r);
    LC_CHECK_EQ(HashOf(restored), HashOf(r));

    // Round-trip must also hold across the derived draw methods, not just NextU64.
    u64 mid[4];
    restored.GetState(mid);
    const u32  ra = restored.Range(97u);
    const i32  rb = restored.RangeI(-1000, 1000);
    const bool rc = restored.NextBool();

    Rng again;
    again.SetState(mid);
    LC_CHECK_EQ(again.Range(97u), ra);
    LC_CHECK_EQ(again.RangeI(-1000, 1000), rb);
    LC_CHECK_EQ(again.NextBool(), rc);

    // Equality and the hash must both depend on ALL FOUR state words. Every other test in
    // this file compares generators that differ in every word, so a comparison that quietly
    // ignored one word — or a HashInto that skipped one — would satisfy all of them while
    // letting two genuinely different generators be declared identical. That is precisely
    // the bug that would make a desync invisible to the replay harness, so it is checked
    // one word at a time.
    const u64 base[4] = {0x1111111111111111ull, 0x2222222222222222ull,
                         0x3333333333333333ull, 0x4444444444444444ull};
    for (std::size_t w = 0; w < 4u; ++w) {
        u64 tweaked[4] = {base[0], base[1], base[2], base[3]};
        tweaked[w] ^= 0x8000000000000000ull;   // flip one bit of one word

        Rng lhs;
        lhs.SetState(base);
        Rng rhs;
        rhs.SetState(tweaked);

        LC_CHECK(lhs != rhs);
        LC_CHECK_FALSE(lhs == rhs);
        LC_CHECK_NE(HashOf(lhs), HashOf(rhs));

        // ...and the difference must actually reach the output, not just the comparison.
        Rng lhsDraw;
        lhsDraw.SetState(base);
        Rng rhsDraw;
        rhsDraw.SetState(tweaked);
        u32 sameDraws = 0;
        for (u32 i = 0; i < 8u; ++i) {
            if (lhsDraw.NextU64() == rhsDraw.NextU64()) ++sameDraws;
        }
        LC_CHECK(sameDraws < 8u);
    }

    // Reflexivity, and equality of two independently-restored copies.
    Rng self;
    self.SetState(base);
    LC_CHECK(self == self);
    Rng twin;
    twin.SetState(base);
    LC_CHECK(self == twin);
    LC_CHECK_FALSE(self != twin);
}

// ---------------------------------------------------------------------------------------
// All-zero is the one xoshiro state that emits zeros forever. SetState() takes words from a
// save file, so a truncated or zero-filled file must be repaired identically everywhere
// rather than producing a generator that has silently stopped generating.
LC_TEST(rng_set_state_repairs_all_zero_state) {
    const u64 zero[4] = {0ull, 0ull, 0ull, 0ull};

    Rng a;
    a.SetState(zero);
    u64 got[4];
    a.GetState(got);
    LC_CHECK_EQ(got[0], 0x9E3779B97F4A7C15ull);
    LC_CHECK_EQ(got[1], 0xBF58476D1CE4E5B9ull);
    LC_CHECK_EQ(got[2], 0x94D049BB133111EBull);
    LC_CHECK_EQ(got[3], 0x2545F4914F6CDD1Dull);

    // Repaired, it must actually generate — not sit on zeros.
    u32 nonZero = 0;
    for (u32 i = 0; i < 32u; ++i) {
        if (a.NextU64() != 0ull) ++nonZero;
    }
    LC_CHECK_EQ(nonZero, 32u);

    // The repair must be deterministic across instances (R3): two machines loading the same
    // corrupt save must end up on the same state, not on two different "reasonable" ones.
    Rng b;
    b.SetState(zero);
    Rng c(kSeed);
    c.SetState(zero);
    LC_CHECK(b == c);

    // A state with a single non-zero word is legal and must NOT be rewritten.
    const u64 sparse[4] = {0ull, 0ull, 0ull, 1ull};
    Rng d;
    d.SetState(sparse);
    d.GetState(got);
    LC_CHECK_EQ(got[0], 0ull);
    LC_CHECK_EQ(got[3], 1ull);
}

// ---------------------------------------------------------------------------------------
LC_TEST(rng_range_stays_in_bounds_and_covers_every_bucket) {
    Rng r(kSeed);

    for (u32 n = 1u; n <= 16u; ++n) {
        u32 seen[16] = {0};
        u32 outOfRange = 0;
        for (u32 i = 0; i < 4000u; ++i) {
            const u32 v = r.Range(n);
            // The whole point of Range: v >= n is a hard failure, not a rare one.
            if (v < n) ++seen[v]; else ++outOfRange;
        }
        LC_CHECK_EQ(outOfRange, 0u);
        u32 emptyBuckets = 0;
        for (u32 b = 0; b < n; ++b) {
            if (seen[b] == 0u) ++emptyBuckets;
        }
        // 4000 draws over at most 16 buckets: missing one means the top of the range is
        // unreachable, which is the classic off-by-one in a rejection bound.
        LC_CHECK_EQ(emptyBuckets, 0u);
    }

    // Range(1) is degenerate but must still be well defined and total.
    for (u32 i = 0; i < 64u; ++i) {
        LC_CHECK_EQ(r.Range(1u), 0u);
    }

    // Boundaries of the u32 domain.
    LC_CHECK(r.Range(0xFFFFFFFFu) < 0xFFFFFFFFu);
    LC_CHECK(r.Range(0x80000000u) < 0x80000000u);
    LC_CHECK(r.Range(2u) < 2u);
}

// ---------------------------------------------------------------------------------------
// Modulo bias is the failure this test exists to catch: `NextU32() % n` for a non-power-of-
// two n over-weights the low buckets by about n / 2^32 each, which is far too small to see
// at these sample sizes — so the test instead pins the *distribution*, and a future
// "optimisation" that drops the rejection loop for a multiply-shift trick with a real bias
// (or an off-by-one in the span) shows up as a blown chi-square.
//
// Integer arithmetic only. chi-square is accumulated scaled by 1000 to keep resolution
// without touching a float.
LC_TEST(rng_range_is_unbiased_for_non_power_of_two) {
    {
        constexpr u32 kN     = 7u;          // 2^32 % 7 != 0, so the rejection path matters
        constexpr u32 kDraws = 700000u;     // 100000 expected per bucket
        constexpr u64 kExpected = kDraws / kN;

        Rng  r(kSeed);
        u64  counts[kN] = {0};
        u32  outOfRange = 0;
        for (u32 i = 0; i < kDraws; ++i) {
            const u32 v = r.Range(kN);
            if (v < kN) ++counts[v]; else ++outOfRange;
        }
        LC_CHECK_EQ(outOfRange, 0u);   // counted, not asserted per draw, to keep failures readable

        u64 total = 0;
        u64 chiX1000 = 0;
        for (u32 b = 0; b < kN; ++b) {
            total += counts[b];
            const i64 d = static_cast<i64>(counts[b]) - static_cast<i64>(kExpected);
            chiX1000 += (static_cast<u64>(d * d) * 1000ull) / kExpected;
        }
        LC_CHECK_EQ(total, static_cast<u64>(kDraws));   // every draw landed somewhere

        // E[chi-square] == df == 6, i.e. 6000 scaled. 60000 is ten times the mean and far
        // beyond anything a fair generator produces; a modulo-biased one blows past it.
        constexpr u64 kChiLimit = 60000ull;
        if (chiX1000 >= kChiLimit) {
            LC_CHECK_EQ(chiX1000, kChiLimit);   // prints the observed statistic on failure
        }
    }

    {
        constexpr u32 kN     = 100u;        // also not a power of two
        constexpr u32 kDraws = 400000u;     // 4000 expected per bucket
        constexpr u64 kExpected = kDraws / kN;

        Rng  r(kSeed ^ 0xA5A5A5A5A5A5A5A5ull);
        u64  counts[kN] = {0};
        u32  outOfRange = 0;
        for (u32 i = 0; i < kDraws; ++i) {
            const u32 v = r.Range(kN);
            if (v < kN) ++counts[v]; else ++outOfRange;
        }
        LC_CHECK_EQ(outOfRange, 0u);

        u64 chiX1000 = 0;
        for (u32 b = 0; b < kN; ++b) {
            const i64 d = static_cast<i64>(counts[b]) - static_cast<i64>(kExpected);
            chiX1000 += (static_cast<u64>(d * d) * 1000ull) / kExpected;
        }
        // df == 99, so E[chi-square] == 99000 scaled. 400000 is a four-fold margin.
        constexpr u64 kChiLimit = 400000ull;
        if (chiX1000 >= kChiLimit) {
            LC_CHECK_EQ(chiX1000, kChiLimit);
        }
    }
}

// ---------------------------------------------------------------------------------------
// Draw accounting is part of the determinism contract: the number of NextU64 calls a given
// method consumes must be a function of the call sequence alone. A "fast path" that
// returned early for n == 1 without drawing would shift every subsequent draw in the
// stream, which is a save-invalidating change disguised as an optimisation.
LC_TEST(rng_range_draw_accounting_is_fixed) {
    {
        Rng a(kSeed);
        (void)a.Range(1u);
        LC_CHECK_EQ(DrawsConsumed(a, Rng(kSeed)), 1);
    }
    {
        // Powers of two divide 2^32 exactly, so rejection is impossible: exactly one draw
        // per call, every call, forever.
        Rng a(kSeed);
        for (u32 i = 0; i < 256u; ++i) (void)a.Range(4u);
        Rng b(kSeed);
        for (u32 i = 0; i < 256u; ++i) (void)b.NextU64();
        LC_CHECK(a == b);
    }
    {
        Rng a(kSeed);
        (void)a.RangeI(-100, 100);          // width 200 -> one Range call
        LC_CHECK_EQ(DrawsConsumed(a, Rng(kSeed)), 1);
    }
    {
        Rng a(kSeed);
        (void)a.NextBool();
        LC_CHECK_EQ(DrawsConsumed(a, Rng(kSeed)), 1);
    }
    {
        Rng a(kSeed);
        (void)a.NextU32();
        LC_CHECK_EQ(DrawsConsumed(a, Rng(kSeed)), 1);
    }
    {
        // RangeI must be exactly Range shifted by lo, consuming the same draws.
        Rng a(kSeed);
        Rng b(kSeed);
        for (u32 i = 0; i < 100u; ++i) {
            const i32 viaI = a.RangeI(-7, 43);
            const i32 viaR = -7 + static_cast<i32>(b.Range(50u));
            LC_CHECK_EQ(viaI, viaR);
        }
        LC_CHECK(a == b);
    }
}

// ---------------------------------------------------------------------------------------
// The rejection branch is otherwise essentially unreachable — it fires for roughly one draw
// in 2^32 at n == 3 — so it is reached here by construction. The state below was solved
// backwards through the xoshiro256** scrambler (result = rotl(s1*5, 7) * 9, all steps
// invertible mod 2^64) so that the first NextU32 is exactly 0xFFFFFFFF.
//
// For n == 3 the accepted span is (2^32 / 3) * 3 == 0xFFFFFFFF, so 0xFFFFFFFF is the single
// rejected value and Range(3) must draw again. Without the rejection loop this would return
// 0xFFFFFFFF % 3 == 0 after one draw, and the bias would be real but invisible.
LC_TEST(rng_range_rejects_a_draw_outside_the_span) {
    static const u64 kForced[4] = {
        0xBFD8250E2741ED3Eull,
        0x4FC71C71C71C71C7ull,   // solved so that the first output is all ones
        0xD4DE2D95CED88314ull,
        0x63C8AC209A54EED3ull,
    };

    {
        Rng probe;
        probe.SetState(kForced);
        LC_CHECK_EQ(probe.NextU32(), 0xFFFFFFFFu);
    }
    {
        Rng a;
        a.SetState(kForced);
        Rng behind;
        behind.SetState(kForced);
        const u32 v = a.Range(3u);
        LC_CHECK(v < 3u);
        LC_CHECK_EQ(v, 1u);                        // the *second* draw's value, not 0xFFFFFFFF % 3
        LC_CHECK_EQ(DrawsConsumed(a, behind), 2);  // proof the first draw was thrown away
    }
    {
        // Same state, power-of-two n: span is 2^32, nothing can be rejected, one draw.
        Rng a;
        a.SetState(kForced);
        Rng behind;
        behind.SetState(kForced);
        LC_CHECK_EQ(a.Range(4u), 3u);
        LC_CHECK_EQ(DrawsConsumed(a, behind), 1);
    }
    {
        // And n == 1: span is 2^32 as well, so even 0xFFFFFFFF is accepted.
        Rng a;
        a.SetState(kForced);
        Rng behind;
        behind.SetState(kForced);
        LC_CHECK_EQ(a.Range(1u), 0u);
        LC_CHECK_EQ(DrawsConsumed(a, behind), 1);
    }
}

// ---------------------------------------------------------------------------------------
// Frozen output of the derived draw methods. NextU64 being stable is not enough: a change
// to which bits Range or NextBool consume would leave NextU64 untouched and still rewrite
// every world.
LC_TEST(rng_derived_draws_known_answer) {
    {
        static const u32 kExpected[16] = {3u, 5u, 3u, 3u, 2u, 0u, 3u, 5u,
                                          6u, 6u, 6u, 6u, 1u, 4u, 2u, 3u};
        Rng r(kSeed);
        for (u32 i = 0; i < 16u; ++i) LC_CHECK_EQ(r.Range(7u), kExpected[i]);
    }
    {
        static const i32 kExpected[12] = {-4, -3, -5, -1, -1, 3, -2, 3, 3, -1, -1, 1};
        Rng r(kSeed);
        for (u32 i = 0; i < 12u; ++i) LC_CHECK_EQ(r.RangeI(-5, 5), kExpected[i]);
    }
    {
        static const bool kExpected[16] = {true,  true,  true,  false, true,  false, true,  false,
                                           true,  true,  true,  true,  false, false, false, false};
        Rng r(kSeed);
        for (u32 i = 0; i < 16u; ++i) LC_CHECK_EQ(r.NextBool(), kExpected[i]);
    }
    {
        // Range(6) on this seed opens with five consecutive multiples of six. That looked
        // like a bug and is not: the uniformity test above draws 700k samples and passes,
        // and the run is a genuine 1-in-7776 coincidence. Frozen deliberately, with this
        // note, so nobody "fixes" it twice.
        static const u32 kExpected[8] = {3u, 0u, 0u, 0u, 0u, 0u, 5u, 2u};
        Rng r(kSeed);
        for (u32 i = 0; i < 8u; ++i) LC_CHECK_EQ(r.Range(6u), kExpected[i]);
    }
}

// ---------------------------------------------------------------------------------------
LC_TEST(rng_rangei_handles_the_full_i32_span) {
    // lo == INT32_MIN with hi == INT32_MAX makes (hi - lo) overflow i32. If the width were
    // computed in 32 bits this would go negative and the range would collapse.
    constexpr i32 kLo = -2147483647 - 1;
    constexpr i32 kHi = 2147483647;

    Rng r(kSeed);
    i32 minSeen = kHi;
    i32 maxSeen = kLo;
    u32 outOfRange = 0;
    for (u32 i = 0; i < 100000u; ++i) {
        const i32 v = r.RangeI(kLo, kHi);
        if (v < kLo || v >= kHi) ++outOfRange;
        if (v < minSeen) minSeen = v;
        if (v > maxSeen) maxSeen = v;
    }
    LC_CHECK_EQ(outOfRange, 0u);
    // Both halves of the span must be reachable — a width bug typically strands one side.
    LC_CHECK(minSeen < 0);
    LC_CHECK(maxSeen > 0);

    // Bounds alone are weak: a width that was off by one, or that saturated, would still
    // land inside them. Pin the full-span path to exactly `lo + Range(width)` — same values,
    // same draws — so the widest range is anchored as tightly as the small ones.
    {
        Rng viaI(kSeed);
        Rng viaR(kSeed);
        for (u32 i = 0; i < 64u; ++i) {
            const i64 expected =
                static_cast<i64>(kLo) + static_cast<i64>(viaR.Range(0xFFFFFFFFu));
            LC_CHECK_EQ(viaI.RangeI(kLo, kHi), static_cast<i32>(expected));
        }
        LC_CHECK(viaI == viaR);   // identical draw accounting on the widest possible range
    }

    // Negative-only ranges, and a width of exactly 1.
    outOfRange = 0;
    for (u32 i = 0; i < 1000u; ++i) {
        const i32 v = r.RangeI(-30, -20);
        if (v < -30 || v >= -20) ++outOfRange;
    }
    LC_CHECK_EQ(outOfRange, 0u);
    for (u32 i = 0; i < 16u; ++i) {
        LC_CHECK_EQ(r.RangeI(9, 10), 9);
        LC_CHECK_EQ(r.RangeI(-1, 0), -1);
    }

    // Every value of a small range must be reachable, endpoints included.
    bool hitLo = false;
    bool hitHi = false;
    for (u32 i = 0; i < 400u; ++i) {
        const i32 v = r.RangeI(-2, 3);
        LC_CHECK(v >= -2);
        LC_CHECK(v < 3);
        if (v == -2) hitLo = true;
        if (v == 2)  hitHi = true;
    }
    LC_CHECK(hitLo);
    LC_CHECK(hitHi);
}

// ---------------------------------------------------------------------------------------
LC_TEST(rng_nextbool_is_balanced) {
    Rng r(kSeed);
    u32 trues = 0;
    constexpr u32 kDraws = 200000u;
    for (u32 i = 0; i < kDraws; ++i) {
        if (r.NextBool()) ++trues;
    }
    // Expected 100000, sd ~224. A 5000 window is ~22 sd — this only trips if NextBool has
    // become structurally lopsided (a stuck bit, or the wrong bit selected).
    const u32 expected = kDraws / 2u;
    const u32 dev = (trues > expected) ? (trues - expected) : (expected - trues);
    if (dev >= 5000u) {
        LC_CHECK_EQ(dev, 5000u);   // prints the observed deviation on failure
    }

    // It must not alternate or repeat in blocks either — a run of 40 identical values in
    // 200k draws would mean the bit is not being refreshed per draw.
    Rng q(kSeed);
    bool prev = q.NextBool();
    u32 run = 1;
    u32 longestRun = 1;
    for (u32 i = 1; i < 100000u; ++i) {
        const bool v = q.NextBool();
        run = (v == prev) ? (run + 1u) : 1u;
        if (run > longestRun) longestRun = run;
        prev = v;
    }
    LC_CHECK(longestRun > 1u);    // never alternating
    LC_CHECK(longestRun < 40u);   // never stuck
}

// ---------------------------------------------------------------------------------------
// I1 / I7: RNG state feeds the world hash, so equal states must hash equal and any advance
// must move the hash.
LC_TEST(rng_hash_tracks_state) {
    Rng a(kSeed);
    Rng b(kSeed);
    LC_CHECK_EQ(HashOf(a), HashOf(b));

    // Frozen so a change to the hashed layout (word order, width) is caught.
    LC_CHECK_EQ(HashOf(a), 0x8D559FBD83897060ull);

    u64 prev = HashOf(a);
    for (u32 i = 0; i < 1000u; ++i) {
        (void)a.NextU64();
        const u64 now = HashOf(a);
        LC_CHECK_NE(now, prev);          // every single draw must move the hash
        prev = now;
    }
    LC_CHECK_NE(HashOf(a), HashOf(b));

    // Catching up must reconverge the hash exactly — the hash is a function of state alone,
    // never of how the state was reached.
    for (u32 i = 0; i < 1000u; ++i) (void)b.NextU64();
    LC_CHECK_EQ(HashOf(a), HashOf(b));
    // Frozen: hash of Rng(kSeed) after exactly 1000 NextU64 calls. If you change the loop
    // count above, this constant is no longer the right one — regenerate it, do not paste in
    // whatever the failure happened to print.
    LC_CHECK_EQ(HashOf(a), 0x41B593483C1FD9A4ull);

    // Hashing must not mutate the generator.
    const u64 before = HashOf(a);
    Hasher scratch;
    a.HashInto(scratch);
    a.HashInto(scratch);
    LC_CHECK_EQ(HashOf(a), before);

    // A hash restored from a save must match the hash of the live generator (I7).
    u64 st[4];
    a.GetState(st);
    Rng loaded;
    loaded.SetState(st);
    LC_CHECK_EQ(HashOf(loaded), HashOf(a));
}

// ---------------------------------------------------------------------------------------
// R3: this is the test the stream design exists for. Drawing hard from one subsystem must
// leave every other subsystem bit-identical to a world where that subsystem never ran.
LC_TEST(rng_streams_are_independent) {
    RngStreams busy(kSeed);
    RngStreams fresh(kSeed);

    for (u32 i = 0; i < 100000u; ++i) {
        (void)busy.Stream(StreamId::Agents).NextU64();
    }

    // Every other stream must be untouched — state, hash, and next output.
    for (std::size_t i = 0; i < kStreamCount; ++i) {
        const StreamId id = static_cast<StreamId>(i);
        if (id == StreamId::Agents) continue;
        LC_CHECK(busy.Stream(id) == fresh.Stream(id));
        LC_CHECK_EQ(HashOf(busy.Stream(id)), HashOf(fresh.Stream(id)));
    }
    LC_CHECK_EQ(busy.Stream(StreamId::Economy).NextU64(),
                fresh.Stream(StreamId::Economy).NextU64());
    LC_CHECK(busy.Stream(StreamId::Agents) != fresh.Stream(StreamId::Agents));

    // The converse: interleaving draws across streams in a different order must not change
    // any one stream's sequence. This is what lets two subsystems be reordered in the tick
    // without invalidating a replay.
    RngStreams forward(kSeed);
    RngStreams reversed(kSeed);
    for (u32 round = 0; round < 500u; ++round) {
        for (std::size_t i = 0; i < kStreamCount; ++i) {
            (void)forward.Stream(static_cast<StreamId>(i)).NextU64();
        }
        for (std::size_t i = kStreamCount; i > 0; --i) {
            (void)reversed.Stream(static_cast<StreamId>(i - 1)).NextU64();
        }
    }
    LC_CHECK_EQ(HashOf(forward), HashOf(reversed));

    // Streams must not start out aliased: identical opening draws would mean two subsystems
    // are sharing a sequence even though they hold separate objects.
    RngStreams s(kSeed);
    u64 firstDraw[kStreamCount];
    for (std::size_t i = 0; i < kStreamCount; ++i) {
        firstDraw[i] = s.Stream(static_cast<StreamId>(i)).NextU64();
    }
    for (std::size_t i = 0; i < kStreamCount; ++i) {
        for (std::size_t j = i + 1; j < kStreamCount; ++j) {
            LC_CHECK_NE(firstDraw[i], firstDraw[j]);
        }
    }
}

// ---------------------------------------------------------------------------------------
// The derivation rule itself is frozen: worldSeed ^ HashString(name), expanded by
// SplitMix64. Renaming a stream, reordering the enum, or changing the mix would silently
// regenerate every world from the same seed.
LC_TEST(rng_stream_derivation_is_frozen) {
    static const u64 kFirstDraw[kStreamCount] = {
        0xC6697D9CE3891940ull,   // world
        0xF01D93095133EE9Eull,   // terrain
        0xF33D3A8B07E1F5E9ull,   // agents
        0x9572E97412361BC9ull,   // economy
        0xC9DBFFF2AEB8BDB1ull,   // transport
        0x5DCA8FB8B774C32Eull,   // law
        0x25B90809E5042E32ull,   // destruct
        0x5640B86E980557ADull,   // director
        0xA971C4D83B6B5076ull,   // test
    };

    RngStreams s(kSeed);
    for (std::size_t i = 0; i < kStreamCount; ++i) {
        LC_CHECK_EQ(s.Stream(static_cast<StreamId>(i)).NextU64(), kFirstDraw[i]);
    }

    // Frozen hash of the whole freshly-seeded set.
    LC_CHECK_EQ(HashOf(RngStreams(kSeed)), 0xD90A92ADDBA26B89ull);

    // The rule must be exactly Rng(worldSeed ^ HashString(name)) — reconstructing a single
    // stream from the public helper has to reproduce it bit for bit, so tools can rebuild
    // one stream without instantiating the whole world.
    RngStreams t(kSeed);
    for (std::size_t i = 0; i < kStreamCount; ++i) {
        const StreamId id = static_cast<StreamId>(i);
        const u64 derived = DeriveStreamSeed(kSeed, kStreamNames[i]);
        LC_CHECK_EQ(derived, kSeed ^ HashString(kStreamNames[i]));
        Rng standalone(derived);
        LC_CHECK(t.Stream(id) == standalone);
    }

    // Derived seeds must be distinct: a collision would silently alias two subsystems.
    for (std::size_t i = 0; i < kStreamCount; ++i) {
        for (std::size_t j = i + 1; j < kStreamCount; ++j) {
            LC_CHECK_NE(DeriveStreamSeed(0ull, kStreamNames[i]),
                        DeriveStreamSeed(0ull, kStreamNames[j]));
        }
    }

    // DeriveStreamSeed must be usable at compile time — a stream seed may never depend on
    // anything that only exists at runtime.
    static_assert(DeriveStreamSeed(0ull, "agents") == HashString("agents"),
                  "DeriveStreamSeed must be constexpr and must be a pure XOR");
    static_assert(DeriveStreamSeed(kSeed, "agents") != DeriveStreamSeed(kSeed, "economy"),
                  "stream names must not collide");
}

// ---------------------------------------------------------------------------------------
LC_TEST(rng_streams_reseed_resets_every_stream) {
    RngStreams s(kSeed);
    const u64 pristine = HashOf(s);

    for (std::size_t i = 0; i < kStreamCount; ++i) {
        for (u32 d = 0; d < 97u; ++d) (void)s.Stream(static_cast<StreamId>(i)).NextU64();
    }
    LC_CHECK_NE(HashOf(s), pristine);

    s.Reseed(kSeed);
    LC_CHECK_EQ(HashOf(s), pristine);
    LC_CHECK_EQ(s.WorldSeed(), kSeed);
    LC_CHECK_EQ(HashOf(s), HashOf(RngStreams(kSeed)));

    // A different world seed must move every stream, not just some of them.
    RngStreams other(kSeed ^ 1ull);
    for (std::size_t i = 0; i < kStreamCount; ++i) {
        const StreamId id = static_cast<StreamId>(i);
        LC_CHECK(other.Stream(id) != s.Stream(id));
    }
    LC_CHECK_NE(HashOf(other), HashOf(s));

    // Default construction is seed 0 and must be reproducible.
    RngStreams defaulted;
    LC_CHECK_EQ(defaulted.WorldSeed(), 0ull);
    LC_CHECK_EQ(HashOf(defaulted), HashOf(RngStreams(0ull)));
}

// ---------------------------------------------------------------------------------------
// The set hash must cover the world seed and every single stream. A hash that skipped one
// stream would let that subsystem desync without the replay harness ever noticing (I1).
LC_TEST(rng_streams_hash_covers_every_stream_and_the_seed) {
    for (std::size_t i = 0; i < kStreamCount; ++i) {
        RngStreams base(kSeed);
        RngStreams moved(kSeed);
        (void)moved.Stream(static_cast<StreamId>(i)).NextU64();
        LC_CHECK_NE(HashOf(base), HashOf(moved));
    }

    // Same stream states, different world seed: the seed itself must be in the hash.
    RngStreams a(kSeed);
    RngStreams b(kSeed);
    LC_CHECK_EQ(HashOf(a), HashOf(b));
    LC_CHECK_NE(HashOf(a), HashOf(RngStreams(kSeed + 1ull)));

    // Hashing is non-mutating and repeatable.
    const u64 h1 = HashOf(a);
    const u64 h2 = HashOf(a);
    LC_CHECK_EQ(h1, h2);

    // Frozen: the whole set after a heavy run on one stream.
    RngStreams busy(kSeed);
    for (u32 i = 0; i < 10000u; ++i) (void)busy.Stream(StreamId::Agents).NextU64();
    LC_CHECK_EQ(HashOf(busy), 0xA350BF8B2A4047F9ull);
}

// ---------------------------------------------------------------------------------------
LC_TEST(rng_stream_names_and_ids_agree) {
    LC_CHECK_EQ(static_cast<u32>(kStreamCount), 9u);
    LC_CHECK_EQ(static_cast<u8>(StreamId::World), static_cast<u8>(0));
    LC_CHECK_EQ(static_cast<u8>(StreamId::Count), static_cast<u8>(kStreamCount));

    // The name table is indexed by StreamId, so a reordered enum with a stale table would
    // reassign every subsystem's sequence. Spot-check the mapping end to end.
    LC_CHECK(SameStr(StreamName(StreamId::World),     "world"));
    LC_CHECK(SameStr(StreamName(StreamId::Terrain),   "terrain"));
    LC_CHECK(SameStr(StreamName(StreamId::Agents),    "agents"));
    LC_CHECK(SameStr(StreamName(StreamId::Economy),   "economy"));
    LC_CHECK(SameStr(StreamName(StreamId::Transport), "transport"));
    LC_CHECK(SameStr(StreamName(StreamId::Law),       "law"));
    LC_CHECK(SameStr(StreamName(StreamId::Destruct),  "destruct"));
    LC_CHECK(SameStr(StreamName(StreamId::Director),  "director"));
    LC_CHECK(SameStr(StreamName(StreamId::Test),      "test"));

    for (std::size_t i = 0; i < kStreamCount; ++i) {
        LC_CHECK(SameStr(StreamName(static_cast<StreamId>(i)), kStreamNames[i]));
    }

    // Stream() must hand back the live generator, not a copy: a copy would swallow every
    // draw made through it and the sequence would silently restart on each lookup.
    RngStreams s(kSeed);
    const u64 firstDraw  = s.Stream(StreamId::Law).NextU64();
    const u64 secondDraw = s.Stream(StreamId::Law).NextU64();
    LC_CHECK_NE(firstDraw, secondDraw);

    Rng standalone(DeriveStreamSeed(kSeed, "law"));
    LC_CHECK_EQ(standalone.NextU64(), firstDraw);
    LC_CHECK_EQ(standalone.NextU64(), secondDraw);
    LC_CHECK(s.Stream(StreamId::Law) == standalone);

    // The const overload must index exactly like the non-const one. It is a separate
    // function body, and it is the one a `const RngStreams&` gets — which is what a const
    // HashInto, a snapshot writer or a hash-comparison helper holds. Every other test in
    // this file reaches streams through a non-const object, so a const overload that
    // returned the wrong stream (or stream 0 for everything) would corrupt saves and world
    // hashes while the whole suite stayed green.
    RngStreams        mutableSet(kSeed);
    const RngStreams& constSet = mutableSet;

    for (std::size_t i = 0; i < kStreamCount; ++i) {
        const StreamId id = static_cast<StreamId>(i);
        LC_CHECK(constSet.Stream(id) == mutableSet.Stream(id));
        LC_CHECK_EQ(HashOf(constSet.Stream(id)), HashOf(mutableSet.Stream(id)));
    }

    // Nine distinct generators, not the same one nine times — this is the check that
    // actually pins the indexing, since comparing const and non-const views of the same
    // wrong slot would agree with each other.
    for (std::size_t i = 0; i < kStreamCount; ++i) {
        for (std::size_t j = i + 1; j < kStreamCount; ++j) {
            LC_CHECK(constSet.Stream(static_cast<StreamId>(i))
                     != constSet.Stream(static_cast<StreamId>(j)));
        }
    }

    // The const view must observe draws made through the non-const reference: it is a view
    // of the live generator, never a snapshot.
    (void)mutableSet.Stream(StreamId::Director).NextU64();
    LC_CHECK(constSet.Stream(StreamId::Director) == mutableSet.Stream(StreamId::Director));
    LC_CHECK(constSet.Stream(StreamId::Director) != RngStreams(kSeed).Stream(StreamId::Director));
    LC_CHECK_EQ(constSet.WorldSeed(), kSeed);
}

// ---------------------------------------------------------------------------------------
// I5: the sim tick allocates nothing in steady state, and every tick draws from these
// generators. Nothing here may reach the heap. (Only meaningful in the headless build,
// where LC_TRACK_ALLOCATIONS is on; harmless otherwise.)
LC_TEST(rng_draws_never_allocate) {
    RngStreams streams(kSeed);
    Rng        local(kSeed);
    u64        sink = 0;

    NoAllocScope guard("rng draws");
    for (u32 i = 0; i < 20000u; ++i) {
        sink ^= local.NextU64();
        sink ^= static_cast<u64>(local.NextU32());
        sink ^= static_cast<u64>(local.Range(1000u));
        sink ^= static_cast<u64>(static_cast<u32>(local.RangeI(-500, 500) + 500));
        sink ^= local.NextBool() ? 1ull : 0ull;
        sink ^= streams.Stream(StreamId::Test).NextU64();
    }
    u64 st[4];
    local.GetState(st);
    local.SetState(st);
    Hasher h;
    streams.HashInto(h);
    sink ^= h.Value();

    LC_CHECK(guard.Clean());
    LC_CHECK_NE(sink, 0ull);   // keeps the loop from being optimised away
}
