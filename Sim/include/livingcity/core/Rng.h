// LIVING CITY — seeded random number generation.
//
// R3: one seeded RNG stream per subsystem, derived from the world seed. Same seed =>
//     bit-identical sequence, on every machine, in every build, forever.
//
// Deliberate omissions, and why:
//   * There is NO floating-point output method. No NextFloat, no NextDouble, no Gaussian.
//     A float RNG in a deterministic sim is a trap: the compiler is free to contract,
//     reassociate and widen float expressions differently between the CMake build and the
//     UBT build, so the same draw would yield different sim state. Callers that want a
//     fraction take an integer numerator out of Range() and feed it to Fixed.
//   * There is no thread-local or global default generator. A generator is always owned by
//     the state that draws from it, so it round-trips through save/load (I7) and is covered
//     by the world hash (I1).
//   * No allocation, no virtuals, no exceptions, no wall clock (I5, R1).
#pragma once

#include "livingcity/core/Core.h"

namespace lc {

// ---------------------------------------------------------------- detail
namespace rng_detail {

// Left rotate. Masking the count keeps k == 0 well defined (a shift by 64 is UB), which
// matters because this is a constexpr helper that other headers may reuse.
constexpr u64 RotL(u64 x, unsigned k) {
    k &= 63u;
    return (k == 0u) ? x : ((x << k) | (x >> (64u - k)));
}

} // namespace rng_detail

// ---------------------------------------------------------------- SplitMix64
// Vigna's SplitMix64. Used ONLY to expand a single u64 seed into generator state and to
// derive per-stream seeds. It is never the generator a subsystem draws from: its state is a
// single word, so it has short-range structure that xoshiro does not.
//
// Every u64 state is legal, all-zero included, so it is safe to seed it from anything.
struct SplitMix64 {
    u64 state = 0;

    constexpr SplitMix64() = default;
    constexpr explicit SplitMix64(u64 seed) : state(seed) {}

    constexpr u64 Next() {
        state += 0x9E3779B97F4A7C15ull;
        u64 z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
};

// ---------------------------------------------------------------- Rng (xoshiro256**)
// Blackman and Vigna's xoshiro256**. 256 bits of state, period 2^256-1, every output bit
// passes BigCrush. Chosen over std::mt19937_64 because it is 4 words of state instead of
// 312, has no dynamic initialisation, and — decisively — because the standard library only
// guarantees reproducibility for the *engines*, not for the *distributions*, whose
// implementations differ between libstdc++ and MSVC's STL. We own every bit of this.
//
// Draw accounting is part of the contract. Every public draw method consumes at least one
// NextU64() call, Range(1) included. Skipping a draw is not worth the cycles: if one build
// skipped it and another did not, the two worlds diverge on the very next call.
class Rng {
public:
    Rng() { Seed(0); }
    explicit Rng(u64 seed) { Seed(seed); }

    // Expand one u64 into 256 bits of state by running SplitMix64 four times.
    void Seed(u64 seed) {
        SplitMix64 sm(seed);
        s_[0] = sm.Next();
        s_[1] = sm.Next();
        s_[2] = sm.Next();
        s_[3] = sm.Next();
        FixupZeroState();
    }

    // ---- draws ------------------------------------------------------------------

    u64 NextU64() {
        const u64 result = rng_detail::RotL(s_[1] * 5ull, 7u) * 9ull;
        const u64 t      = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rng_detail::RotL(s_[3], 45u);
        return result;
    }

    // Takes the HIGH 32 bits. xoshiro256**'s low bits are as good as its high bits, but
    // pinning the choice here gives Range() and NextBool() one documented source.
    u32 NextU32() { return static_cast<u32>(NextU64() >> 32); }

    // Uniform in [0, n). Unbiased by rejection, never by modulo.
    //
    // span is the largest multiple of n that fits in 2^32, so `v % n` is exactly uniform
    // for every accepted v. Expected draws is below 2 for any n, and exactly 1 whenever n
    // divides 2^32 — which includes n == 1 and every power of two, because span is then
    // 2^32 and no 32-bit value can be rejected.
    //
    // n == 0 is a caller bug: it asserts and returns 0, having consumed no draw.
    // n == 1 consumes exactly one draw and returns 0.
    u32 Range(u32 n) {
        LC_ASSERT_MSG(n != 0u, "Rng::Range(0) has no uniform answer");
        if (n == 0u) return 0u;
        const u64 span = (0x100000000ull / static_cast<u64>(n)) * static_cast<u64>(n);
        u32 v = NextU32();
        while (static_cast<u64>(v) >= span) {
            v = NextU32();
        }
        return v % n;
    }

    // Uniform in [lo, hiExclusive). Widths up to the full i32 span are handled: the
    // arithmetic runs through i64 and u32, so lo == INT32_MIN with hi == INT32_MAX cannot
    // overflow. An empty or inverted range is a caller bug: it asserts and returns lo,
    // consuming no draw.
    i32 RangeI(i32 lo, i32 hiExclusive) {
        LC_ASSERT_MSG(hiExclusive > lo, "Rng::RangeI requires hiExclusive > lo");
        if (hiExclusive <= lo) return lo;
        const i64 width = static_cast<i64>(hiExclusive) - static_cast<i64>(lo);
        const u32 span  = static_cast<u32>(width);   // 1 .. 2^32-1, always exact
        const i64 v     = static_cast<i64>(lo) + static_cast<i64>(Range(span));
        return static_cast<i32>(v);
    }

    // Consumes one full draw and returns its top bit. It deliberately does NOT cache the
    // other 63 bits for the next call: a hidden bit cache is state that save/load and the
    // world hash would both have to know about, and forgetting it is a silent desync.
    bool NextBool() { return (NextU64() >> 63) != 0ull; }

    // ---- state (I7: must round-trip through save/load) ---------------------------

    void GetState(u64 out[4]) const {
        out[0] = s_[0];
        out[1] = s_[1];
        out[2] = s_[2];
        out[3] = s_[3];
    }

    void SetState(const u64 in[4]) {
        s_[0] = in[0];
        s_[1] = in[1];
        s_[2] = in[2];
        s_[3] = in[3];
        FixupZeroState();
    }

    // I1 / I7: generator state is world state, so it feeds the world hash.
    void HashInto(Hasher& h) const {
        h.U64(s_[0]);
        h.U64(s_[1]);
        h.U64(s_[2]);
        h.U64(s_[3]);
    }

    bool operator==(const Rng& o) const {
        return s_[0] == o.s_[0] && s_[1] == o.s_[1] && s_[2] == o.s_[2] && s_[3] == o.s_[3];
    }
    bool operator!=(const Rng& o) const { return !(*this == o); }

private:
    // All-zero is the one state xoshiro can never leave: it emits zeros forever. SplitMix64
    // cannot produce four zeros in a row from any seed, but SetState() takes words from a
    // save file, which may be truncated, zero-filled or hostile. Substituting a fixed
    // constant keeps the repair deterministic — every machine repairs it identically.
    void FixupZeroState() {
        if ((s_[0] | s_[1] | s_[2] | s_[3]) == 0ull) {
            s_[0] = 0x9E3779B97F4A7C15ull;
            s_[1] = 0xBF58476D1CE4E5B9ull;
            s_[2] = 0x94D049BB133111EBull;
            s_[3] = 0x2545F4914F6CDD1Dull;
        }
    }

    u64 s_[4] = {0, 0, 0, 0};
};

// The generator is exactly its four state words: no vtable, no bit cache, no padding.
// Snapshot code sizes its records against this, and a hidden member added later would
// both break those records and be a piece of state that HashInto() silently omits.
static_assert(sizeof(Rng) == 32, "Rng must be exactly its four state words");
static_assert(alignof(Rng) == alignof(u64), "Rng must not acquire extra alignment");

// ---------------------------------------------------------------- named streams
// R3: one independent stream per subsystem. Subsystems must never share a generator. If the
// economy and the agents drew from one, adding a single draw to the economy would shift
// every agent decision for the rest of the run, and a bug fix in one system would surface
// as an unexplained regression in the other.
enum class StreamId : u8 {
    World = 0,   // world-generation scaffolding, top-level layout
    Terrain,     // heightfield, caves, minerals
    Agents,      // citizen generation and per-agent decisions
    Economy,     // firms, prices, markets
    Transport,   // traffic, transit
    Law,         // observation, courts, sentencing
    Destruct,    // structural failure and disasters
    Director,    // City Director / narrative
    Test,        // reserved for tests and tools; no shipping system may draw from it
    Count
};

inline constexpr std::size_t kStreamCount = static_cast<std::size_t>(StreamId::Count);

// Stream names are compile-time constants: a derived seed must never depend on anything
// loaded at runtime. Order matches StreamId exactly. `inline constexpr` gives this constant
// initialisation, so there is no static-initialisation-order dependency between TUs.
inline constexpr const char* const kStreamNames[kStreamCount] = {
    "world",
    "terrain",
    "agents",
    "economy",
    "transport",
    "law",
    "destruct",
    "director",
    "test",
};

inline const char* StreamName(StreamId id) {
    const std::size_t i = static_cast<std::size_t>(id);
    LC_ASSERT_MSG(i < kStreamCount, "StreamName: id out of range");
    return (i < kStreamCount) ? kStreamNames[i] : "invalid";
}

// The seed a stream derives from the world seed. Exposed so tools and tests can reproduce a
// single stream without building the whole set, and constexpr so a stream seed is a
// compile-time constant whenever the world seed is.
constexpr u64 DeriveStreamSeed(u64 worldSeed, const char* streamName) {
    return worldSeed ^ HashString(streamName);
}

// The full set of per-subsystem generators.
//
// Derivation is Rng(worldSeed ^ HashString(name)), and Rng's own seeding step is the "run
// SplitMix64 four times" expansion — so a stream's 256-bit state is exactly SplitMix64
// seeded with (worldSeed ^ HashString(name)), as specified. FNV-1a over the name spreads
// the stream seeds far apart, and SplitMix64 then decorrelates them: xoshiro256** states
// differing in a single bit still diverge completely within a handful of draws.
//
// The whole object is plain data. Copying it snapshots every stream; comparing two copies
// compares whole RNG histories.
class RngStreams {
public:
    RngStreams() { Reseed(0); }
    explicit RngStreams(u64 worldSeed) { Reseed(worldSeed); }

    void Reseed(u64 worldSeed) {
        worldSeed_ = worldSeed;
        for (std::size_t i = 0; i < kStreamCount; ++i) {
            streams_[i].Seed(DeriveStreamSeed(worldSeed, kStreamNames[i]));
        }
    }

    Rng& Stream(StreamId id) {
        const std::size_t i = static_cast<std::size_t>(id);
        LC_ASSERT_MSG(i < kStreamCount, "RngStreams::Stream: id out of range");
        return streams_[(i < kStreamCount) ? i : 0];
    }

    const Rng& Stream(StreamId id) const {
        const std::size_t i = static_cast<std::size_t>(id);
        LC_ASSERT_MSG(i < kStreamCount, "RngStreams::Stream: id out of range");
        return streams_[(i < kStreamCount) ? i : 0];
    }

    u64 WorldSeed() const { return worldSeed_; }

    // Covers the world seed and every stream, in StreamId order (I1, I7).
    void HashInto(Hasher& h) const {
        h.U64(worldSeed_);
        for (std::size_t i = 0; i < kStreamCount; ++i) {
            streams_[i].HashInto(h);
        }
    }

private:
    Rng streams_[kStreamCount];
    u64 worldSeed_ = 0;
};

} // namespace lc
