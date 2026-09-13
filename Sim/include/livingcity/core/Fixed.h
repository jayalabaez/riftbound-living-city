// LIVING CITY - Q32.32 signed fixed-point arithmetic.
//
// R1: zero Unreal types. R3: this file is a determinism primitive, so:
//   * there is not a single float or double below this line - every operation is exact
//     integer arithmetic on i64 raw values;
//   * the 64x64 -> 128 intermediates that multiplication and division need are built by
//     hand out of 32-bit limbs. No __int128 (MSVC does not have it), no _mul128/_udiv128
//     intrinsics (they are not available on every toolchain we must build under), no
//     compiler-specific anything. The same bits come out of cl.exe and out of UBT's clang;
//   * every sign is handled explicitly by computing on magnitudes and reapplying the sign
//     at the end, so nothing depends on how a given compiler shifts a negative value.
//
// Format: value = raw / 2^32. The integer part spans [-2^31, 2^31-1]; the smallest
// representable step is 1/2^32.
//
// Rounding contract, stated once and relied on everywhere:
//   * operator* and operator/ TRUNCATE TOWARD ZERO (they truncate the magnitude).
//   * ToInt() truncates toward zero. Floor() and Ceil() go the obvious ways.
//   * Round() is half AWAY FROM ZERO, so Round(0.5) == 1 and Round(-0.5) == -1.
// Overflow is never silent: it trips LC_ASSERT rather than wrapping, because a wrapped
// position or balance would corrupt a save and a replay without ever being noticed.
#pragma once

#include "livingcity/core/Core.h"

namespace lc {

// ---------------------------------------------------------------- portable 128-bit helpers
// These live in a named namespace rather than inside Fixed because Money.h needs them too:
// exactly one implementation of the wide arithmetic, so exactly one thing to get right.
namespace fx {

// A 128-bit unsigned magnitude as two 64-bit halves. Unsigned only, on purpose - every
// caller strips the sign first, so none of this code ever shifts or divides a negative.
struct U128 {
    u64 hi = 0;
    u64 lo = 0;
};

// |v| as a u64. Correct for INT64_MIN, where negating in i64 would overflow: unsigned
// negation is defined to be modular, so 0 - (u64)INT64_MIN is exactly 2^63.
inline u64 Magnitude(i64 v) {
    const u64 uv = static_cast<u64>(v);
    return (v < 0) ? (0ull - uv) : uv;
}

// Reapply a sign to a magnitude, asserting that the result is representable.
// C++20 mandates two's complement, so the u64 -> i64 conversion here is defined and
// value-preserving modulo 2^64, not implementation-defined.
inline i64 SignedFromMagnitude(u64 mag, bool negative) {
    if (negative) {
        LC_ASSERT_MSG(mag <= 0x8000000000000000ull, "fixed-point negative overflow");
        return static_cast<i64>(0ull - mag);
    }
    LC_ASSERT_MSG(mag <= 0x7FFFFFFFFFFFFFFFull, "fixed-point positive overflow");
    return static_cast<i64>(mag);
}

// Exact 64 x 64 -> 128 unsigned multiply by 32-bit limb decomposition.
// With a = a1*2^32 + a0 and b = b1*2^32 + b0, a*b = a1b1*2^64 + (a1b0 + a0b1)*2^32 + a0b0.
// Every partial product fits in a u64 because each limb is at most 2^32-1, and the middle
// accumulator cannot overflow either: it is bounded by 3*(2^32-1), well under 2^64.
inline U128 Mul64x64(u64 a, u64 b) {
    const u64 kLoMask = 0xFFFFFFFFull;
    const u64 a0 = a & kLoMask, a1 = a >> 32;
    const u64 b0 = b & kLoMask, b1 = b >> 32;

    const u64 p00 = a0 * b0;
    const u64 p01 = a0 * b1;
    const u64 p10 = a1 * b0;
    const u64 p11 = a1 * b1;

    const u64 mid = (p00 >> 32) + (p01 & kLoMask) + (p10 & kLoMask);

    U128 r;
    r.lo = (p00 & kLoMask) | (mid << 32);
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return r;
}

// 128-bit + 64-bit with carry into the high half. Used to add a rounding bias to a
// full-width product before the divide, where truncating first would lose the tie.
inline U128 Add128(U128 a, u64 b) {
    U128 r;
    r.lo = a.lo + b;
    r.hi = a.hi + ((r.lo < a.lo) ? 1ull : 0ull);
    return r;
}

// 128 / 64 -> 64 unsigned division, truncating.
//
// Restoring shift-subtract long division, 64 iterations. Deliberately plain: it uses only
// shifts, comparisons and subtraction on u64, so it produces identical bits on every
// compiler and CPU we will ever run on. The carry bit holds the 65th bit of the running
// remainder, which is what keeps this correct for divisors close to 2^64.
//
// Precondition: n.hi < d, i.e. the quotient fits in 64 bits. That is asserted, not assumed
// - a quotient that does not fit means the caller asked for a value outside Q32.32 range.
inline u64 Div128By64(U128 n, u64 d) {
    LC_ASSERT_MSG(d != 0ull, "fixed-point division by zero");
    LC_ASSERT_MSG(n.hi < d, "fixed-point division overflow (quotient exceeds 64 bits)");

    if (n.hi == 0ull) return n.lo / d;   // fast path: an ordinary 64-bit divide

    u64 rem = n.hi;                      // loop invariant: rem < d
    u64 q   = 0ull;
    for (int i = 63; i >= 0; --i) {
        const u64 carry = rem >> 63;
        rem = (rem << 1) | ((n.lo >> i) & 1ull);
        // The conceptual remainder is carry*2^64 + rem and is always below 2*d, so at most
        // one subtraction is needed. When carry is set the value certainly exceeds d, and
        // the wrapping subtraction below yields exactly (2^64 + rem - d) mod 2^64.
        if (carry != 0ull || rem >= d) {
            rem -= d;
            q |= (1ull << i);
        }
    }
    return q;
}

// Narrow an integer part to i32, refusing rather than silently truncating.
inline i32 NarrowToI32(i64 v) {
    LC_ASSERT_MSG(v >= -2147483648LL && v <= 2147483647LL,
                  "fixed-point integer part does not fit in i32");
    return static_cast<i32>(v);
}

} // namespace fx

// ---------------------------------------------------------------- Fixed
struct Fixed {
    static constexpr int kFractionBits = 32;
    static constexpr i64 kOneRaw       = static_cast<i64>(1) << kFractionBits; // 4294967296
    // DERIVED from kFractionBits, never spelled out. Written as literals these three would
    // silently fall out of step with kOneRaw the first time anyone changed the format, and
    // Floor/Ceil/Round would go on compiling and start returning wrong answers - the exact
    // failure mode this header exists to prevent.
    static constexpr u64 kFracMask     = (1ull << kFractionBits) - 1ull;
    static constexpr u64 kHalfRaw      = 1ull << (kFractionBits - 1);          // 0.5
    static constexpr i64 kMinRaw       = static_cast<i64>(0x8000000000000000ull);
    static constexpr i64 kMaxRaw       = static_cast<i64>(0x7FFFFFFFFFFFFFFFull);

    // The format itself, pinned. A replay recorded under Q32.32 cannot be read back under
    // any other split, so changing kFractionBits has to be a deliberate, loud act.
    static_assert(kFractionBits == 32, "Q32.32 is the recorded save/replay format");
    static_assert(kOneRaw == 4294967296LL, "one whole unit is 2^32 raw steps");
    static_assert(kFracMask == 0xFFFFFFFFull, "fraction mask must cover exactly kFractionBits");
    static_assert(kHalfRaw == 0x80000000ull, "half must be exactly one bit below kOneRaw");
    static_assert(static_cast<u64>(kOneRaw) == kFracMask + 1ull, "mask and scale agree");
    static_assert(kHalfRaw * 2ull == static_cast<u64>(kOneRaw), "half is half");

    // Public so Fixed stays a trivially copyable POD that a snapshot can memcpy, but every
    // construction goes through a named factory: there is no implicit i64 -> Fixed, which
    // is what stops a raw tick count from being used as a length by accident.
    i64 raw = 0;

    constexpr Fixed() = default;

    static constexpr Fixed FromRaw(i64 r) { Fixed f; f.raw = r; return f; }

    // Exact: an i32 always fits the integer part. Routed through u64 so no signed shift is
    // involved at all.
    static constexpr Fixed FromInt(i32 v) {
        const u64 shifted = static_cast<u64>(static_cast<i64>(v)) << kFractionBits;
        return FromRaw(static_cast<i64>(shifted));
    }

    // num/den evaluated with the same truncate-toward-zero rule as operator/, so
    // FromRatio(a, b) and FromInt(a) / FromInt(b) agree bit for bit.
    static Fixed FromRatio(i32 num, i32 den) {
        LC_ASSERT_MSG(den != 0, "Fixed::FromRatio division by zero");
        return FromInt(num) / FromInt(den);
    }

    static constexpr Fixed Zero() { return FromRaw(0); }
    static constexpr Fixed One()  { return FromRaw(kOneRaw); }

    constexpr i64 Raw() const { return raw; }

    // ------------------------------------------------------------ integer extraction
    // Integer part with the fraction discarded, sign preserved: 1.9 -> 1, -1.9 -> -1.
    // This is not a floor; -1.9 floors to -2. See Floor().
    i32 ToInt() const { return fx::NarrowToI32(TruncatedI64()); }

    // Largest integer <= value. -1.5 -> -2, -1.0 -> -1, 1.5 -> 1.
    i32 Floor() const {
        i64 t = TruncatedI64();
        if (raw < 0 && FractionMagnitude() != 0ull) --t;
        return fx::NarrowToI32(t);
    }

    // Smallest integer >= value. -1.5 -> -1, 1.5 -> 2, 1.0 -> 1.
    i32 Ceil() const {
        i64 t = TruncatedI64();
        if (raw > 0 && FractionMagnitude() != 0ull) ++t;
        return fx::NarrowToI32(t);
    }

    // Nearest integer, ties broken AWAY FROM ZERO: 0.5 -> 1, -0.5 -> -1, -2.5 -> -3.
    // Computed on the magnitude, so Round(-x) == -Round(x) for every representable x.
    i32 Round() const {
        const u64 mag = fx::Magnitude(raw);                  // at most 2^63
        const u64 r   = (mag + kHalfRaw) >> kFractionBits;   // 2^63 + 2^31 cannot overflow
        const i64 t   = (raw < 0) ? -static_cast<i64>(r) : static_cast<i64>(r);
        return fx::NarrowToI32(t);
    }

    // ------------------------------------------------------------ arithmetic
    Fixed operator+(Fixed o) const {
        const i64 s = static_cast<i64>(static_cast<u64>(raw) + static_cast<u64>(o.raw));
        // Overflowed exactly when both operands share a sign that the result does not.
        LC_ASSERT_MSG(((raw ^ s) & (o.raw ^ s)) >= 0, "Fixed addition overflow");
        return FromRaw(s);
    }

    Fixed operator-(Fixed o) const {
        const i64 s = static_cast<i64>(static_cast<u64>(raw) - static_cast<u64>(o.raw));
        // Overflowed exactly when the operands differ in sign and the result's sign differs
        // from the minuend's.
        LC_ASSERT_MSG(((raw ^ o.raw) & (raw ^ s)) >= 0, "Fixed subtraction overflow");
        return FromRaw(s);
    }

    Fixed operator-() const {
        LC_ASSERT_MSG(raw != kMinRaw, "Fixed negation overflow");
        return FromRaw(static_cast<i64>(0ull - static_cast<u64>(raw)));
    }

    // a*b needs the full 128-bit product before the >> 32 renormalisation: for any value
    // with an integer part above roughly 2^15 the naive i64 product has already overflowed.
    Fixed operator*(Fixed o) const {
        const bool     negative = (raw < 0) != (o.raw < 0);
        const fx::U128 p        = fx::Mul64x64(fx::Magnitude(raw), fx::Magnitude(o.raw));

        // The renormalised magnitude is p >> 32, which fits in 64 bits only if the top
        // 32 bits of p.hi are clear.
        LC_ASSERT_MSG((p.hi >> 32) == 0ull, "Fixed multiplication overflow");
        const u64 mag = (p.lo >> 32) | (p.hi << 32);
        return FromRaw(fx::SignedFromMagnitude(mag, negative));
    }

    // a/b is (|a| << 32) / |b|, and (|a| << 32) is a 128-bit quantity.
    Fixed operator/(Fixed o) const {
        LC_ASSERT_MSG(o.raw != 0, "Fixed division by zero");
        const bool negative = (raw < 0) != (o.raw < 0);
        const u64  a        = fx::Magnitude(raw);
        const u64  b        = fx::Magnitude(o.raw);

        fx::U128 n;
        n.hi = a >> 32;
        n.lo = a << 32;
        return FromRaw(fx::SignedFromMagnitude(fx::Div128By64(n, b), negative));
    }

    Fixed& operator+=(Fixed o) { *this = *this + o; return *this; }
    Fixed& operator-=(Fixed o) { *this = *this - o; return *this; }
    Fixed& operator*=(Fixed o) { *this = *this * o; return *this; }
    Fixed& operator/=(Fixed o) { *this = *this / o; return *this; }

    // ------------------------------------------------------------ comparison
    constexpr bool operator==(Fixed o) const { return raw == o.raw; }
    constexpr bool operator!=(Fixed o) const { return raw != o.raw; }
    constexpr bool operator< (Fixed o) const { return raw <  o.raw; }
    constexpr bool operator<=(Fixed o) const { return raw <= o.raw; }
    constexpr bool operator> (Fixed o) const { return raw >  o.raw; }
    constexpr bool operator>=(Fixed o) const { return raw >= o.raw; }

    constexpr bool IsZero()     const { return raw == 0; }
    constexpr bool IsNegative() const { return raw <  0; }

    // ------------------------------------------------------------ utility
    Fixed Abs() const { return (raw < 0) ? -(*this) : *this; }

    static constexpr Fixed Min(Fixed a, Fixed b) { return (a.raw < b.raw) ? a : b; }
    static constexpr Fixed Max(Fixed a, Fixed b) { return (a.raw > b.raw) ? a : b; }

    static Fixed Clamp(Fixed v, Fixed lo, Fixed hi) {
        LC_ASSERT_MSG(lo.raw <= hi.raw, "Fixed::Clamp inverted range");
        return Min(Max(v, lo), hi);
    }

    // ------------------------------------------------------------ hashing (I1, I7)
    // Feeds the raw integer, never a decimal rendering: a rendering would need division and
    // would throw away exactly the low bits the world hash exists to catch.
    void HashInto(Hasher& h) const { h.I64(raw); }

private:
    // Integer part with the fraction dropped and the sign reapplied. Kept as i64 so that
    // Floor() and Ceil() can adjust by one before the range check narrows to i32.
    i64 TruncatedI64() const {
        const u64 ip = fx::Magnitude(raw) >> kFractionBits;
        return (raw < 0) ? -static_cast<i64>(ip) : static_cast<i64>(ip);
    }

    u64 FractionMagnitude() const { return fx::Magnitude(raw) & kFracMask; }
};

} // namespace lc
