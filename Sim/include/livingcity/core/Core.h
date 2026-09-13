// LIVING CITY — core foundation.
//
// R1: ZERO Unreal types below this line, ever. No UObject, AActor, FMath, TArray, UE_LOG.
// R3: everything here must be deterministic. No floats in sim state, no wall-clock time,
//     no pointer values hashed, no unordered iteration.
//
// This header is the shared contract every other sim header compiles against.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>

// ---------------------------------------------------------------- symbol visibility
//
// The sim is compiled two ways (DECISIONS.md D-005): as plain objects by
// Scripts/Build-Sim.ps1, and as an Unreal module by UBT. In an Unreal EDITOR build every
// module is its own DLL, and NOTHING crosses a DLL boundary unless it is explicitly
// exported. Without this, LivingCityGame links against an empty LivingCitySim.dll and every
// out-of-line sim function is an unresolved external.
//
// This is plain compiler visibility syntax, not an Unreal type, so R1 still holds: the sim
// knows nothing about Unreal - only about possibly being packaged as a shared library.
// Both flags are off in the standalone build, so LC_API vanishes there.
#if defined(LC_SHARED_BUILD)
  #if defined(_MSC_VER)
    #if defined(LC_EXPORTS)
      #define LC_API __declspec(dllexport)
    #else
      #define LC_API __declspec(dllimport)
    #endif
  #else
    #define LC_API __attribute__((visibility("default")))
  #endif
#else
  #define LC_API
#endif

// C4251: "needs to have dll-interface to be used by clients of class". MSVC raises this for
// every std::vector or std::string member of an exported class. It warns that a client
// compiled against a DIFFERENT standard library could disagree about the layout - which
// cannot happen here, because UBT compiles every module in a build with one identical
// toolchain, CRT and flags. Scoped to the shared build only; the standalone build never
// exports anything and so never raises it.
#if defined(LC_SHARED_BUILD) && defined(_MSC_VER)
  #pragma warning(disable : 4251)
#endif

namespace lc {

// ---------------------------------------------------------------- fixed-width aliases
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// Sim time is counted in ticks. Never in seconds, never in wall-clock.
using TickIndex = u64;

// ---------------------------------------------------------------- assertions
// No exceptions anywhere in the sim: UE game modules compile with exceptions disabled,
// and D-005 requires these sources to build under both toolchains.
// [[noreturn]] because it genuinely never returns - it ends in std::abort(). Saying so is
// not decoration: the Unreal build compiles these sources through one aggregate translation
// unit, so MSVC can see this body, infers the same thing, and raises C4702 on any statement
// written after an LC_FATAL. Declaring the truth lets those dead statements be deleted
// rather than worked around.
[[noreturn]] LC_API void FailAssert(const char* expr, const char* file, int line, const char* msg);

#define LC_ASSERT(expr)                                                             \
    do {                                                                            \
        if (!(expr)) ::lc::FailAssert(#expr, __FILE__, __LINE__, nullptr);          \
    } while (false)

#define LC_ASSERT_MSG(expr, msg)                                                    \
    do {                                                                            \
        if (!(expr)) ::lc::FailAssert(#expr, __FILE__, __LINE__, (msg));            \
    } while (false)

// I4: a generator that cannot satisfy an invariant must fail loudly, never paper over it.
#define LC_FATAL(msg) ::lc::FailAssert("FATAL", __FILE__, __LINE__, (msg))

// ---------------------------------------------------------------- typed identifiers
// Distinct types so a FirmId can never be passed where a CitizenId belongs.
// Value 0 is reserved as "none" so a zero-initialised id is detectably invalid.
template <typename Tag>
struct Id {
    u32 value = 0;

    constexpr Id() = default;
    constexpr explicit Id(u32 v) : value(v) {}

    constexpr bool IsValid() const { return value != 0; }
    constexpr bool operator==(const Id& o) const { return value == o.value; }
    constexpr bool operator!=(const Id& o) const { return value != o.value; }
    constexpr bool operator<(const Id& o) const { return value < o.value; }
};

struct CitizenTag {};
struct FirmTag {};
struct ParcelTag {};
struct BuildingTag {};
struct AccountTag {};

using CitizenId  = Id<CitizenTag>;
using FirmId     = Id<FirmTag>;
using ParcelId   = Id<ParcelTag>;
using BuildingId = Id<BuildingTag>;
using AccountId  = Id<AccountTag>;

// Monotonic id allocator. Never reuses a value, so a stale id is always detectable
// rather than silently aliasing a new entity (I6 depends on this).
template <typename IdT>
class IdAllocator {
public:
    IdT Next() {
        LC_ASSERT_MSG(next_ != 0xFFFFFFFFu, "id space exhausted");
        return IdT(++next_);
    }
    u32  Count() const { return next_; }
    void Reset(u32 to = 0) { next_ = to; }

    // Part of world state, so it must round-trip through save/load (I7).
    u32  RawCursor() const { return next_; }
    void SetRawCursor(u32 v) { next_ = v; }

private:
    u32 next_ = 0;
};

// ---------------------------------------------------------------- hashing
// FNV-1a 64. Incremental, order-sensitive, and never fed a pointer or a float —
// this is the backbone of I1 (determinism) and I7 (save round-trip equality).
class Hasher {
public:
    static constexpr u64 kOffsetBasis = 1469598103934665603ull;
    static constexpr u64 kPrime       = 1099511628211ull;

    void Bytes(const void* data, std::size_t len) {
        const u8* p = static_cast<const u8*>(data);
        for (std::size_t i = 0; i < len; ++i) {
            h_ ^= static_cast<u64>(p[i]);
            h_ *= kPrime;
        }
    }

    void U8(u8 v)   { Bytes(&v, sizeof(v)); }
    void U32(u32 v) { Bytes(&v, sizeof(v)); }
    void U64(u64 v) { Bytes(&v, sizeof(v)); }
    void I32(i32 v) { Bytes(&v, sizeof(v)); }
    void I64(i64 v) { Bytes(&v, sizeof(v)); }
    void Bool(bool v) { U8(v ? 1u : 0u); }

    template <typename Tag>
    void Ident(Id<Tag> id) { U32(id.value); }

    void Str(const char* s) {
        if (!s) { U8(0); return; }
        for (const char* p = s; *p; ++p) U8(static_cast<u8>(*p));
        U8(0);
    }

    u64 Value() const { return h_; }
    void Reset() { h_ = kOffsetBasis; }

private:
    u64 h_ = kOffsetBasis;
};

// Compile-time string hash, used to derive named RNG streams (see Rng.h).
constexpr u64 HashString(const char* s) {
    u64 h = Hasher::kOffsetBasis;
    for (const char* p = s; *p; ++p) {
        h ^= static_cast<u64>(static_cast<u8>(*p));
        h *= Hasher::kPrime;
    }
    return h;
}

// ---------------------------------------------------------------- allocation tracking (I5)
// Only compiled into the headless/test build. UBT must NEVER define LC_TRACK_ALLOCATIONS:
// replacing global operator new inside an Unreal module would fight UE's own allocator.
struct AllocStats {
    u64 allocations = 0;
    u64 frees       = 0;
    u64 bytes       = 0;
};

LC_API AllocStats GetAllocStats();
LC_API void ResetAllocStats();
LC_API bool AllocTrackingEnabled();

// Scoped guard asserting that a region allocated nothing at all.
class NoAllocScope {
public:
    explicit NoAllocScope(const char* what) : what_(what), start_(GetAllocStats().allocations) {}
    ~NoAllocScope() = default;

    u64  AllocationsSinceStart() const { return GetAllocStats().allocations - start_; }
    bool Clean() const { return AllocationsSinceStart() == 0; }
    const char* What() const { return what_; }

private:
    const char* what_;
    u64         start_;
};

// ---------------------------------------------------------------- logging
// The sim never touches stdio directly outside this sink; the headless app installs a
// writer and the Unreal side installs one that forwards to UE_LOG on the game thread.
enum class LogLevel : u8 { Debug = 0, Info = 1, Warn = 2, Error = 3 };

using LogSink = void (*)(LogLevel, const char*);

LC_API void SetLogSink(LogSink sink);
LC_API void Log(LogLevel level, const char* fmt, ...);

#define LC_LOG_INFO(...)  ::lc::Log(::lc::LogLevel::Info,  __VA_ARGS__)
#define LC_LOG_WARN(...)  ::lc::Log(::lc::LogLevel::Warn,  __VA_ARGS__)
#define LC_LOG_ERROR(...) ::lc::Log(::lc::LogLevel::Error, __VA_ARGS__)

} // namespace lc
