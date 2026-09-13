// Tests for lc::Fixed - Q32.32 fixed point.
//
// These are invariant tests, not getter tests. What is actually being defended:
//   * the rounding contract at negative values, which is where every fixed-point library
//     that has ever shipped has had its bug;
//   * that the 128-bit intermediate is real, by feeding it products that a naive i64
//     intermediate would have wrapped;
//   * algebraic laws (commutativity, associativity, a*b/b == a) that a future "optimisation"
//     of the wide arithmetic would break;
//   * hard-coded raw values and hash values, so that a refactor which changes results at all
//     is caught here rather than in a diverging replay six months from now;
//   * the ends of the representable range, where every magnitude is 2^63 or one step below
//     it - the values that only survive if the sign is stripped before the shift;
//   * the shape of the type itself: trivially copyable, no padding, no implicit conversion
//     in or out, all checked by the compiler.
//
// No float or double appears in this file, by design. Every expectation is an exact integer.

#include "TestFramework.h"
#include "livingcity/core/Fixed.h"

#include <cstring>
#include <type_traits>

namespace {

using lc::Fixed;
using lc::i32;
using lc::i64;
using lc::u64;

// INT32_MIN written so it never passes through a wider type: an unsuffixed -2147483648 is a
// long long on MSVC, and handing that to FromInt(i32) is a narrowing-conversion warning,
// which is an error under /W4 /WX.
constexpr i32 kI32Min = -2147483647 - 1;
constexpr i32 kI32Max = 2147483647;

// One whole unit, as a raw value. Spelled out rather than derived so a change to
// kFractionBits shows up as a test failure instead of being silently absorbed.
constexpr i64 kOne = 4294967296LL;

// Structural guarantees, checked by the compiler rather than at run time. Fixed is stored
// directly in sim state and goes into snapshots and the world hash, so these are the
// properties that keep a memcpy of it meaningful and keep a raw i64 from becoming a length
// by accident. If any of them is ever relaxed, the build stops here.
static_assert(std::is_trivially_copyable_v<Fixed>,
              "Fixed must stay memcpy-able for snapshots and save files");
static_assert(std::is_standard_layout_v<Fixed>,
              "Fixed must stay standard-layout so its bytes are well defined");
static_assert(sizeof(Fixed) == sizeof(i64),
              "Fixed must have no padding: an indeterminate pad byte would poison every "
              "snapshot memcpy and every world hash taken over its bytes");
static_assert(!std::is_convertible_v<i64, Fixed>,
              "an i64 must never implicitly become a Fixed");
static_assert(!std::is_constructible_v<Fixed, i64>,
              "a raw i64 must go through FromRaw/FromInt, never a constructor");
static_assert(!std::is_convertible_v<Fixed, i64>,
              "Fixed must never implicitly decay to a number");
static_assert(!std::is_convertible_v<Fixed, bool>,
              "Fixed must never implicitly decay to bool");

} // namespace

// ---------------------------------------------------------------------------------------
LC_TEST(fixed_from_int_to_int_round_trips) {
    const i32 values[] = { 0, 1, -1, 2, -2, 7, -7, 1000000, -1000000, kI32Max, kI32Min };

    for (i32 v : values) {
        LC_CHECK_EQ(Fixed::FromInt(v).ToInt(), v);
        LC_CHECK_EQ(Fixed::FromInt(v).Floor(), v);
        LC_CHECK_EQ(Fixed::FromInt(v).Ceil(),  v);
        LC_CHECK_EQ(Fixed::FromInt(v).Round(), v);
    }

    // The scaling itself, exactly.
    LC_CHECK_EQ(Fixed::FromInt(1).Raw(),  kOne);
    LC_CHECK_EQ(Fixed::FromInt(-1).Raw(), -kOne);
    LC_CHECK_EQ(Fixed::FromInt(0).Raw(),  static_cast<i64>(0));
    LC_CHECK_EQ(Fixed::One().Raw(),       kOne);
    LC_CHECK_EQ(Fixed::Zero().Raw(),      static_cast<i64>(0));

    // The extremes land exactly on the ends of the representable range.
    LC_CHECK_EQ(Fixed::FromInt(kI32Min).Raw(), static_cast<i64>(0x8000000000000000ull));
}

// ---------------------------------------------------------------------------------------
LC_TEST(fixed_to_int_truncates_toward_zero) {
    // 1.5 -> 1 and -1.5 -> -1. A shift-based implementation would give -2 for the second,
    // which is the classic asymmetry bug.
    LC_CHECK_EQ(Fixed::FromRatio(3, 2).ToInt(),  1);
    LC_CHECK_EQ(Fixed::FromRatio(-3, 2).ToInt(), -1);

    LC_CHECK_EQ(Fixed::FromRatio(1, 3).ToInt(),  0);
    LC_CHECK_EQ(Fixed::FromRatio(-1, 3).ToInt(), 0);
    LC_CHECK_EQ(Fixed::FromRatio(-99, 100).ToInt(), 0);

    // A value one raw step below a whole number still truncates down, both signs.
    LC_CHECK_EQ(Fixed::FromRaw(kOne - 1).ToInt(),    0);
    LC_CHECK_EQ(Fixed::FromRaw(-(kOne - 1)).ToInt(), 0);
    LC_CHECK_EQ(Fixed::FromRaw(2 * kOne - 1).ToInt(),    1);
    LC_CHECK_EQ(Fixed::FromRaw(-(2 * kOne - 1)).ToInt(), -1);
}

// ---------------------------------------------------------------------------------------
// The negative side of Floor/Ceil/Round, tested on its own because it is the only place
// these three disagree and the only place implementations get it wrong.
LC_TEST(fixed_floor_ceil_round_on_negatives) {
    const Fixed minusHalf        = Fixed::FromRatio(-1, 2);   // -0.5
    const Fixed minusOnePointFive = Fixed::FromRatio(-3, 2);  // -1.5
    const Fixed minusTwoPointFive = Fixed::FromRatio(-5, 2);  // -2.5
    const Fixed minusThird       = Fixed::FromRatio(-1, 3);   // -0.333...
    const Fixed minusTwoThirds   = Fixed::FromRatio(-2, 3);   // -0.666...

    LC_CHECK_EQ(minusHalf.Floor(), -1);
    LC_CHECK_EQ(minusHalf.Ceil(),   0);
    LC_CHECK_EQ(minusHalf.Round(), -1);   // half away from zero
    LC_CHECK_EQ(minusHalf.ToInt(),  0);

    LC_CHECK_EQ(minusOnePointFive.Floor(), -2);
    LC_CHECK_EQ(minusOnePointFive.Ceil(),  -1);
    LC_CHECK_EQ(minusOnePointFive.Round(), -2);
    LC_CHECK_EQ(minusOnePointFive.ToInt(), -1);

    LC_CHECK_EQ(minusTwoPointFive.Floor(), -3);
    LC_CHECK_EQ(minusTwoPointFive.Ceil(),  -2);
    LC_CHECK_EQ(minusTwoPointFive.Round(), -3);   // NOT -2: ties go away from zero

    LC_CHECK_EQ(minusThird.Floor(), -1);
    LC_CHECK_EQ(minusThird.Ceil(),   0);
    LC_CHECK_EQ(minusThird.Round(),  0);

    LC_CHECK_EQ(minusTwoThirds.Floor(), -1);
    LC_CHECK_EQ(minusTwoThirds.Ceil(),   0);
    LC_CHECK_EQ(minusTwoThirds.Round(), -1);

    // Exact negative integers must not be nudged by the fraction handling.
    LC_CHECK_EQ(Fixed::FromInt(-2).Floor(), -2);
    LC_CHECK_EQ(Fixed::FromInt(-2).Ceil(),  -2);
    LC_CHECK_EQ(Fixed::FromInt(-2).Round(), -2);

    // Smallest possible negative magnitude: one raw step below zero.
    const Fixed tinyNegative = Fixed::FromRaw(-1);
    LC_CHECK_EQ(tinyNegative.Floor(), -1);
    LC_CHECK_EQ(tinyNegative.Ceil(),   0);
    LC_CHECK_EQ(tinyNegative.Round(),  0);
    LC_CHECK_EQ(tinyNegative.ToInt(),  0);
}

// ---------------------------------------------------------------------------------------
LC_TEST(fixed_round_is_symmetric_about_zero) {
    // Round(-x) == -Round(x) for every value, which is what "half away from zero computed on
    // the magnitude" buys us. Half-to-even or a shift-based round would break this.
    for (i32 num = -13; num <= 13; ++num) {
        const Fixed v = Fixed::FromRatio(num, 4);
        const Fixed n = Fixed::FromRatio(-num, 4);
        LC_CHECK_EQ(n.Round(), -v.Round());
        LC_CHECK_EQ(n.ToInt(), -v.ToInt());
        // Floor and Ceil are reflections of each other, not of themselves.
        LC_CHECK_EQ(n.Floor(), -v.Ceil());
        LC_CHECK_EQ(n.Ceil(),  -v.Floor());
    }

    LC_CHECK_EQ(Fixed::FromRatio(1, 2).Round(),  1);
    LC_CHECK_EQ(Fixed::FromRatio(5, 2).Round(),  3);
    LC_CHECK_EQ(Fixed::FromRatio(-5, 2).Round(), -3);
}

// ---------------------------------------------------------------------------------------
LC_TEST(fixed_addition_is_commutative_and_associative) {
    // Addition is exact at every representable value, so this holds for arbitrary operands.
    const Fixed a = Fixed::FromRatio(1, 3);
    const Fixed b = Fixed::FromRatio(-7, 11);
    const Fixed c = Fixed::FromRatio(1234, 97);

    LC_CHECK_EQ((a + b).Raw(), (b + a).Raw());
    LC_CHECK_EQ((b + c).Raw(), (c + b).Raw());
    LC_CHECK_EQ(((a + b) + c).Raw(), (a + (b + c)).Raw());

    // Identity and inverse.
    LC_CHECK_EQ((a + Fixed::Zero()).Raw(), a.Raw());
    LC_CHECK_EQ((a + (-a)).Raw(), static_cast<i64>(0));
    LC_CHECK_EQ((a - b).Raw(), (a + (-b)).Raw());

    // Compound assignment agrees with the free operator.
    Fixed acc = a;
    acc += b;
    acc -= c;
    LC_CHECK_EQ(acc.Raw(), ((a + b) - c).Raw());
}

// ---------------------------------------------------------------------------------------
LC_TEST(fixed_multiplication_is_commutative_and_associative) {
    // Commutativity holds for ANY operands, exact or not: the wide multiply is symmetric and
    // the sign is derived symmetrically.
    const Fixed messy[] = {
        Fixed::FromRatio(1, 3), Fixed::FromRatio(-7, 11), Fixed::FromRatio(22, 7),
        Fixed::FromRatio(-1, 1000), Fixed::FromInt(123), Fixed::FromInt(-45),
    };
    for (const Fixed& x : messy) {
        for (const Fixed& y : messy) {
            LC_CHECK_EQ((x * y).Raw(), (y * x).Raw());
        }
    }

    // Associativity only holds where the intermediate products are exactly representable,
    // because operator* truncates. Dyadic rationals with small numerators are exact, so
    // these are the honest values to assert associativity on.
    const Fixed a = Fixed::FromRatio(1, 2);   // exact
    const Fixed b = Fixed::FromRatio(1, 4);   // exact
    const Fixed c = Fixed::FromRatio(3, 8);   // exact
    LC_CHECK_EQ(((a * b) * c).Raw(), (a * (b * c)).Raw());
    LC_CHECK_EQ(((a * b) * c).Raw(), Fixed::FromRatio(3, 64).Raw());

    const Fixed na = -a;
    LC_CHECK_EQ(((na * b) * c).Raw(), (na * (b * c)).Raw());
    LC_CHECK_EQ((na * b).Raw(), -( (a * b).Raw() ));

    // Identity, zero, and sign.
    LC_CHECK_EQ((a * Fixed::One()).Raw(), a.Raw());
    LC_CHECK_EQ((a * Fixed::Zero()).Raw(), static_cast<i64>(0));
    LC_CHECK((Fixed::FromInt(-3) * Fixed::FromInt(-4)) == Fixed::FromInt(12));
    LC_CHECK((Fixed::FromInt(-3) * Fixed::FromInt(4)) == Fixed::FromInt(-12));
}

// ---------------------------------------------------------------------------------------
LC_TEST(fixed_mul_then_div_round_trips_exact_values) {
    // a*b/b == a wherever a*b is exactly representable. Integers and dyadic fractions are.
    const Fixed as[] = {
        Fixed::FromInt(1), Fixed::FromInt(-1), Fixed::FromInt(17), Fixed::FromInt(-17),
        Fixed::FromInt(30000), Fixed::FromInt(-30000),
        Fixed::FromRatio(1, 4), Fixed::FromRatio(-3, 8), Fixed::FromRatio(5, 16),
    };
    const Fixed bs[] = {
        Fixed::FromInt(1), Fixed::FromInt(2), Fixed::FromInt(-2), Fixed::FromInt(7),
        Fixed::FromInt(-64), Fixed::FromRatio(1, 2), Fixed::FromRatio(-1, 8),
    };

    for (const Fixed& a : as) {
        for (const Fixed& b : bs) {
            LC_CHECK_EQ(((a * b) / b).Raw(), a.Raw());
        }
    }

    // Division by one and by minus one.
    LC_CHECK_EQ((Fixed::FromRatio(22, 7) / Fixed::One()).Raw(), Fixed::FromRatio(22, 7).Raw());
    LC_CHECK_EQ((Fixed::FromRatio(22, 7) / Fixed::FromInt(-1)).Raw(),
                -Fixed::FromRatio(22, 7).Raw());
}

// ---------------------------------------------------------------------------------------
// The reason the 128-bit intermediate exists. Every product below has a raw operand product
// far outside i64; a naive (a.raw * b.raw) >> 32 would wrap and return garbage.
LC_TEST(fixed_multiplication_survives_naive_i64_overflow) {
    // 40000 * 40000 = 1.6e9, comfortably inside Q32.32 range - but the raw product is
    // 171798691840000^2, about 2.9e28, which is more than 3e9 times the i64 limit.
    const Fixed big = Fixed::FromInt(40000);
    const Fixed sq  = big * big;
    LC_CHECK_EQ(sq.Raw(), 6871947673600000000LL);   // 1600000000 * 2^32, exactly
    LC_CHECK_EQ(sq.ToInt(), 1600000000);

    // Near the top of the range: 2e9 * 0.5. The raw operands are 8.59e18 and 2^31, so the
    // naive product is about 1.8e28.
    const Fixed huge = Fixed::FromInt(2000000000);
    const Fixed half = Fixed::FromRatio(1, 2);
    LC_CHECK_EQ((huge * half).Raw(), 4294967296000000000LL);   // 1e9 * 2^32
    LC_CHECK_EQ((huge * half).ToInt(), 1000000000);

    // Signs survive the wide path unchanged.
    LC_CHECK_EQ(((-big) * big).Raw(), -6871947673600000000LL);
    LC_CHECK_EQ(((-big) * (-big)).Raw(), 6871947673600000000LL);
    LC_CHECK_EQ((big * (-big)).Raw(), -6871947673600000000LL);

    // And the wide result still divides back exactly.
    LC_CHECK_EQ((sq / big).Raw(), big.Raw());
}

// ---------------------------------------------------------------------------------------
// Known answers. These are hard-coded from this implementation's actual behaviour on
// purpose: if a refactor changes a single low bit, a replay recorded before the refactor
// stops reproducing, and this is where we want to find out.
LC_TEST(fixed_from_ratio_known_answers) {
    // 1/3 truncated toward zero at 32 fractional bits: floor(2^32 / 3) == 1431655765.
    LC_CHECK_EQ(Fixed::FromRatio(1, 3).Raw(), 1431655765LL);

    // Truncation is toward zero, so the negative is the exact mirror, not one step further.
    LC_CHECK_EQ(Fixed::FromRatio(-1, 3).Raw(), -1431655765LL);
    LC_CHECK_EQ(Fixed::FromRatio(1, -3).Raw(), -1431655765LL);
    LC_CHECK_EQ(Fixed::FromRatio(-1, -3).Raw(), 1431655765LL);

    // Exactly representable ratios are exact.
    LC_CHECK_EQ(Fixed::FromRatio(1, 2).Raw(), 2147483648LL);
    LC_CHECK_EQ(Fixed::FromRatio(2, 4).Raw(), 2147483648LL);
    LC_CHECK_EQ(Fixed::FromRatio(1, 4).Raw(), 1073741824LL);
    LC_CHECK_EQ(Fixed::FromRatio(3, 8).Raw(), 1610612736LL);
    LC_CHECK_EQ(Fixed::FromRatio(1, 1).Raw(), kOne);
    LC_CHECK_EQ(Fixed::FromRatio(6, 3).Raw(), 2 * kOne);
    LC_CHECK_EQ(Fixed::FromRatio(0, 7).Raw(), static_cast<i64>(0));

    // 2/3 and 22/7, also hard-coded.
    LC_CHECK_EQ(Fixed::FromRatio(2, 3).Raw(), 2863311530LL);
    LC_CHECK_EQ(Fixed::FromRatio(22, 7).Raw(), 13498468644LL);

    // FromRatio and operator/ must agree bit for bit - they are the same code path, and a
    // future "fast path" for FromRatio that broke this would silently fork determinism.
    // Swept rather than spot-checked, over both signs of both operands, because a fast path
    // that was right for 22/7 and wrong for -22/7 is exactly the shape that bug takes.
    LC_CHECK_EQ(Fixed::FromRatio(22, 7).Raw(),
                (Fixed::FromInt(22) / Fixed::FromInt(7)).Raw());
    for (i32 num = -20; num <= 20; ++num) {
        for (i32 den = -9; den <= 9; ++den) {
            if (den == 0) continue;
            LC_CHECK_EQ(Fixed::FromRatio(num, den).Raw(),
                        (Fixed::FromInt(num) / Fixed::FromInt(den)).Raw());
        }
    }
}

// ---------------------------------------------------------------------------------------
LC_TEST(fixed_division_below_one_and_near_zero) {
    // Numerator smaller than the divisor: the result lives entirely in the fraction.
    LC_CHECK_EQ((Fixed::FromInt(1) / Fixed::FromInt(1000000)).Raw(), 4294LL);
    LC_CHECK_EQ((Fixed::FromInt(-1) / Fixed::FromInt(1000000)).Raw(), -4294LL);
    LC_CHECK_EQ((Fixed::FromInt(1) / Fixed::FromInt(-1000000)).Raw(), -4294LL);

    // Extreme: 1 / 2e9 is two raw steps. Still not zero, and still exact.
    LC_CHECK_EQ((Fixed::FromInt(1) / Fixed::FromInt(2000000000)).Raw(), 2LL);

    // Results that underflow all the way to zero do so cleanly, with no sign residue.
    const Fixed underflowed = Fixed::FromRaw(1) / Fixed::FromInt(3);
    LC_CHECK(underflowed.IsZero());
    LC_CHECK_EQ(underflowed.Raw(), static_cast<i64>(0));

    const Fixed underflowedNeg = Fixed::FromRaw(-1) / Fixed::FromInt(3);
    LC_CHECK(underflowedNeg.IsZero());
    LC_CHECK_EQ(underflowedNeg.Raw(), static_cast<i64>(0));   // not -0, not -1

    // The smallest representable value divided by one is itself.
    LC_CHECK_EQ((Fixed::FromRaw(1) / Fixed::One()).Raw(), 1LL);
    LC_CHECK_EQ((Fixed::FromRaw(-1) / Fixed::One()).Raw(), -1LL);

    // Zero numerator, any divisor.
    LC_CHECK_EQ((Fixed::Zero() / Fixed::FromInt(-7)).Raw(), static_cast<i64>(0));

    // Dividing by a fraction scales up, and the wide numerator path handles it.
    LC_CHECK_EQ((Fixed::FromInt(1) / Fixed::FromRatio(1, 8)).Raw(), 8 * kOne);
    LC_CHECK_EQ((Fixed::FromInt(1000000) / Fixed::FromRatio(1, 2)).Raw(), 2000000LL * kOne);
}

// ---------------------------------------------------------------------------------------
LC_TEST(fixed_negation_abs_and_ordering) {
    const Fixed a = Fixed::FromRatio(-22, 7);

    LC_CHECK_EQ(a.Abs().Raw(), -a.Raw());
    LC_CHECK_EQ(a.Abs().Raw(), Fixed::FromRatio(22, 7).Raw());
    LC_CHECK_EQ(Fixed::Zero().Abs().Raw(), static_cast<i64>(0));
    LC_CHECK_EQ((-(-a)).Raw(), a.Raw());

    LC_CHECK(a < Fixed::Zero());
    LC_CHECK(a.IsNegative());
    LC_CHECK(Fixed::Zero() > a);
    LC_CHECK(a <= a);
    LC_CHECK(a >= a);
    LC_CHECK(a == Fixed::FromRatio(-22, 7));
    LC_CHECK(a != Fixed::FromRatio(22, 7));

    // Ordering must agree with raw ordering across the sign boundary, including at the
    // one-step-either-side-of-zero values.
    LC_CHECK(Fixed::FromRaw(-1) < Fixed::Zero());
    LC_CHECK(Fixed::Zero() < Fixed::FromRaw(1));
    LC_CHECK(Fixed::FromRaw(-1) < Fixed::FromRaw(1));
}

// ---------------------------------------------------------------------------------------
LC_TEST(fixed_min_max_clamp) {
    const Fixed lo = Fixed::FromInt(-5);
    const Fixed hi = Fixed::FromInt(5);

    LC_CHECK_EQ(Fixed::Min(lo, hi).Raw(), lo.Raw());
    LC_CHECK_EQ(Fixed::Max(lo, hi).Raw(), hi.Raw());
    LC_CHECK_EQ(Fixed::Min(hi, lo).Raw(), lo.Raw());
    LC_CHECK_EQ(Fixed::Max(hi, lo).Raw(), hi.Raw());
    LC_CHECK_EQ(Fixed::Min(lo, lo).Raw(), lo.Raw());

    LC_CHECK_EQ(Fixed::Clamp(Fixed::FromInt(100), lo, hi).Raw(), hi.Raw());
    LC_CHECK_EQ(Fixed::Clamp(Fixed::FromInt(-100), lo, hi).Raw(), lo.Raw());
    LC_CHECK_EQ(Fixed::Clamp(Fixed::Zero(), lo, hi).Raw(), static_cast<i64>(0));

    // Clamping is idempotent and endpoint-inclusive.
    LC_CHECK_EQ(Fixed::Clamp(lo, lo, hi).Raw(), lo.Raw());
    LC_CHECK_EQ(Fixed::Clamp(hi, lo, hi).Raw(), hi.Raw());
    const Fixed once = Fixed::Clamp(Fixed::FromRatio(37, 5), lo, hi);
    LC_CHECK_EQ(Fixed::Clamp(once, lo, hi).Raw(), once.Raw());
}

// ---------------------------------------------------------------------------------------
// White-box tests for the portable 128-bit primitives. They are the least obvious code in
// the header and the only part with a hand-rolled carry, so they are tested directly rather
// than only through Fixed.
LC_TEST(fixed_wide_128bit_primitives) {
    using lc::fx::Add128;
    using lc::fx::Div128By64;
    using lc::fx::Mul64x64;
    using lc::fx::U128;

    // 2^63 * 2 == 2^64: the product's low half is empty and everything is in the high half.
    U128 p = Mul64x64(0x8000000000000000ull, 2ull);
    LC_CHECK_EQ(p.hi, 1ull);
    LC_CHECK_EQ(p.lo, 0ull);

    // (2^64 - 1)^2 == (2^64 - 2) * 2^64 + 1. Exercises every limb and every carry.
    p = Mul64x64(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull);
    LC_CHECK_EQ(p.hi, 0xFFFFFFFFFFFFFFFEull);
    LC_CHECK_EQ(p.lo, 1ull);

    // ...and dividing it back by (2^64 - 1) returns (2^64 - 1) exactly. This is the case
    // that only passes if the 65th remainder bit is carried correctly.
    LC_CHECK_EQ(Div128By64(p, 0xFFFFFFFFFFFFFFFFull), 0xFFFFFFFFFFFFFFFFull);

    // Small, checkable products.
    p = Mul64x64(0ull, 0xFFFFFFFFFFFFFFFFull);
    LC_CHECK_EQ(p.hi, 0ull);
    LC_CHECK_EQ(p.lo, 0ull);

    p = Mul64x64(0x100000000ull, 0x100000000ull);   // 2^32 * 2^32 == 2^64
    LC_CHECK_EQ(p.hi, 1ull);
    LC_CHECK_EQ(p.lo, 0ull);

    // 2^64 / 3, truncated.
    U128 n;
    n.hi = 1ull;
    n.lo = 0ull;
    LC_CHECK_EQ(Div128By64(n, 3ull), 6148914691236517205ull);

    // The single-word fast path must agree with the general path's semantics.
    n.hi = 0ull;
    n.lo = 1000000ull;
    LC_CHECK_EQ(Div128By64(n, 7ull), 142857ull);
    LC_CHECK_EQ(Div128By64(n, 1000000ull), 1ull);
    LC_CHECK_EQ(Div128By64(n, 1000001ull), 0ull);

    // Add128 carries out of the low half.
    U128 a;
    a.hi = 0ull;
    a.lo = 0xFFFFFFFFFFFFFFFFull;
    U128 s = Add128(a, 1ull);
    LC_CHECK_EQ(s.hi, 1ull);
    LC_CHECK_EQ(s.lo, 0ull);

    s = Add128(a, 0ull);
    LC_CHECK_EQ(s.hi, 0ull);
    LC_CHECK_EQ(s.lo, 0xFFFFFFFFFFFFFFFFull);

    // The general path at its exact ceiling. n.hi == d - 1 with every low bit set is the
    // largest numerator whose quotient still fits in 64 bits, so the answer must be exactly
    // 2^64 - 1. An off-by-one in the loop bound, or a lost 65th remainder bit, wraps this
    // to 0 or to 2^63 instead - and this is the case a "faster" Knuth divide would get
    // wrong first.
    n.hi = 0x7FFFFFFFFFFFFFFFull;                    // 2^63 - 1, one below the divisor
    n.lo = 0xFFFFFFFFFFFFFFFFull;
    LC_CHECK_EQ(Div128By64(n, 0x8000000000000000ull), 0xFFFFFFFFFFFFFFFFull);

    // The same ceiling for the divisor Money::ApplyRate actually uses. This is the largest
    // biased product ApplyRate can hand the divider without tripping its own assert.
    n.hi = 9999ull;
    n.lo = 0xFFFFFFFFFFFFFFFFull;
    LC_CHECK_EQ(Div128By64(n, 10000ull), 0xFFFFFFFFFFFFFFFFull);

    // A quotient that lands exactly on the sign bit: 2^64 / 2. Nothing here is signed, and
    // this pins that it stays that way.
    n.hi = 1ull;
    n.lo = 0ull;
    LC_CHECK_EQ(Div128By64(n, 2ull), 0x8000000000000000ull);

    // Divisor larger than the whole numerator, general path unreachable, quotient zero.
    n.hi = 0ull;
    n.lo = 5ull;
    LC_CHECK_EQ(Div128By64(n, 0xFFFFFFFFFFFFFFFFull), 0ull);
}

// ---------------------------------------------------------------------------------------
// I1/I7: the hash must be a pure, stable function of the raw value. The expected values are
// FNV-1a-64 over the eight little-endian bytes of raw, hard-coded from this implementation.
// If HashInto ever starts feeding something else - a rounded integer, a struct with padding,
// a decimal rendering - these change and the test fails, which is the point.
LC_TEST(fixed_hash_is_stable_and_distinguishing) {
    lc::Hasher h;
    Fixed::FromInt(1).HashInto(h);
    LC_CHECK_EQ(h.Value(), 12106627384235038034ull);

    h.Reset();
    Fixed::FromInt(-1).HashInto(h);
    LC_CHECK_EQ(h.Value(), 7752564647336749887ull);

    h.Reset();
    Fixed::FromRatio(1, 3).HashInto(h);
    LC_CHECK_EQ(h.Value(), 1946641373582261183ull);

    // Equal values hash equal; a one-raw-step difference does not collide.
    lc::Hasher x, y;
    Fixed::FromRatio(22, 7).HashInto(x);
    Fixed::FromRatio(22, 7).HashInto(y);
    LC_CHECK_EQ(x.Value(), y.Value());

    lc::Hasher z;
    Fixed::FromRaw(Fixed::FromRatio(22, 7).Raw() + 1).HashInto(z);
    LC_CHECK_NE(x.Value(), z.Value());

    // Order matters: hashing a then b differs from b then a. The world hash depends on this.
    lc::Hasher ab, ba;
    Fixed::FromInt(1).HashInto(ab);
    Fixed::FromInt(2).HashInto(ab);
    Fixed::FromInt(2).HashInto(ba);
    Fixed::FromInt(1).HashInto(ba);
    LC_CHECK_NE(ab.Value(), ba.Value());
}

// ---------------------------------------------------------------------------------------
// The ends of the representable range, on the side where the answer exists. Every one of
// these has magnitude 2^63 or one step below it, which is the value that only survives if
// the sign is stripped to an unsigned magnitude BEFORE any shift or divide and reapplied
// afterwards. An implementation that negated first would trip signed overflow here, and an
// implementation that shifted the signed value would depend on how its compiler shifts a
// negative. The abort side of the contract (Ceil/Round at kMaxRaw, negating kMinRaw,
// overflowing an add) cannot be exercised without a death-test facility, so it is not
// asserted here - only the side that must produce an exact answer.
LC_TEST(fixed_boundary_values_at_the_representable_extremes) {
    const Fixed lo = Fixed::FromRaw(Fixed::kMinRaw);   // exactly -2^31
    const Fixed hi = Fixed::FromRaw(Fixed::kMaxRaw);   // one raw step below +2^31

    // kMinRaw is an exact integer, so all four extractions agree and none steps outside i32.
    LC_CHECK_EQ(lo.ToInt(),  kI32Min);
    LC_CHECK_EQ(lo.Floor(),  kI32Min);
    LC_CHECK_EQ(lo.Ceil(),   kI32Min);
    LC_CHECK_EQ(lo.Round(),  kI32Min);
    LC_CHECK_EQ(Fixed::FromInt(kI32Min).Raw(), Fixed::kMinRaw);

    // kMaxRaw has every fraction bit set, so it truncates and floors to kI32Max. Ceil and
    // Round would both have to produce 2^31, which is not an i32; by contract they assert,
    // which is why they are not called on it.
    LC_CHECK_EQ(hi.ToInt(),  kI32Max);
    LC_CHECK_EQ(hi.Floor(),  kI32Max);

    // Addition and subtraction reach both ends exactly without tripping the overflow guard.
    LC_CHECK_EQ((Fixed::FromRaw(Fixed::kMaxRaw - 1) + Fixed::FromRaw(1)).Raw(), Fixed::kMaxRaw);
    LC_CHECK_EQ((Fixed::FromRaw(Fixed::kMinRaw + 1) - Fixed::FromRaw(1)).Raw(), Fixed::kMinRaw);
    LC_CHECK_EQ((lo - Fixed::Zero()).Raw(), Fixed::kMinRaw);
    LC_CHECK_EQ((hi + Fixed::Zero()).Raw(), Fixed::kMaxRaw);

    // Negation and Abs are exact right next to the one value where they are undefined.
    LC_CHECK_EQ((-Fixed::FromRaw(Fixed::kMinRaw + 1)).Raw(), Fixed::kMaxRaw);
    LC_CHECK_EQ(Fixed::FromRaw(Fixed::kMinRaw + 1).Abs().Raw(), Fixed::kMaxRaw);
    LC_CHECK_EQ(hi.Abs().Raw(), Fixed::kMaxRaw);

    // Multiply and divide by one must land back on the extreme bit for bit.
    LC_CHECK_EQ((lo * Fixed::One()).Raw(), Fixed::kMinRaw);
    LC_CHECK_EQ((lo / Fixed::One()).Raw(), Fixed::kMinRaw);
    LC_CHECK_EQ((hi * Fixed::One()).Raw(), Fixed::kMaxRaw);
    LC_CHECK_EQ((hi / Fixed::One()).Raw(), Fixed::kMaxRaw);

    // Halving the extremes: exact at kMinRaw, truncated toward zero at kMaxRaw. Note that
    // dividing kMinRaw by 2 and multiplying it by 0.5 agree, which they only do because both
    // truncate the same magnitude in the same direction.
    LC_CHECK_EQ((lo * Fixed::FromRatio(1, 2)).Raw(), -4611686018427387904LL);
    LC_CHECK_EQ((lo / Fixed::FromInt(2)).Raw(),      -4611686018427387904LL);
    LC_CHECK_EQ((hi / Fixed::FromInt(2)).Raw(),       4611686018427387903LL);

    // Ordering and clamping still behave across the full span.
    LC_CHECK(lo < hi);
    LC_CHECK(lo.IsNegative());
    LC_CHECK_FALSE(hi.IsNegative());
    LC_CHECK_EQ(Fixed::Min(lo, hi).Raw(), Fixed::kMinRaw);
    LC_CHECK_EQ(Fixed::Max(lo, hi).Raw(), Fixed::kMaxRaw);
    LC_CHECK_EQ(Fixed::Clamp(hi, Fixed::FromInt(-1), Fixed::FromInt(1)).Raw(), kOne);
    LC_CHECK_EQ(Fixed::Clamp(lo, Fixed::FromInt(-1), Fixed::FromInt(1)).Raw(), -kOne);
    LC_CHECK_EQ(Fixed::Clamp(lo, lo, hi).Raw(), Fixed::kMinRaw);
    LC_CHECK_EQ(Fixed::Clamp(hi, lo, hi).Raw(), Fixed::kMaxRaw);
}

// ---------------------------------------------------------------------------------------
// R3: Fixed is copied into snapshots by memcpy and folded into the world hash. Both are
// only meaningful if the object has no padding - an indeterminate pad byte would let two
// Fixeds holding the same number compare byte-unequal, and a save round-trip or a world
// hash would diverge for no reason at all and with nothing to point at. sizeof(Fixed) ==
// sizeof(i64) is asserted at the top of this file; this is the same claim checked at run
// time, on storage deliberately pre-poisoned with two different non-zero patterns.
LC_TEST(fixed_bytes_are_fully_determined_by_its_value) {
    lc::u8 a[sizeof(Fixed)];
    lc::u8 b[sizeof(Fixed)];
    std::memset(a, 0xAA, sizeof(a));
    std::memset(b, 0x55, sizeof(b));

    const Fixed x = Fixed::FromRatio(-22, 7);
    const Fixed y = Fixed::FromRaw(Fixed::FromRatio(-22, 7).Raw());
    std::memcpy(a, &x, sizeof(x));
    std::memcpy(b, &y, sizeof(y));

    // Equal values, byte-identical images, whatever was in the buffer beforehand.
    LC_CHECK_EQ(std::memcmp(a, b, sizeof(a)), 0);

    // ...and the image round-trips back to the same value.
    Fixed back = Fixed::Zero();
    std::memcpy(&back, a, sizeof(back));
    LC_CHECK_EQ(back.Raw(), x.Raw());
    LC_CHECK(back == x);

    // A one-raw-step difference must show up in the bytes, or a snapshot could not tell the
    // two apart and a replay would silently accept the wrong one.
    const Fixed z = Fixed::FromRaw(x.Raw() + 1);
    std::memcpy(b, &z, sizeof(z));
    LC_CHECK_NE(std::memcmp(a, b, sizeof(a)), 0);
}
