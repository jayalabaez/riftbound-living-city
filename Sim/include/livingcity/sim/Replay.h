// LIVING CITY - deterministic replay harness. This is the machinery behind invariant I1:
//   same seed + same input log => identical world hash at every checkpoint, bit for bit.
//
// ---------------------------------------------------------------------------------------
// THIS MODULE PERFORMS NO FILE IO. NOT ANYWHERE. NOT EVER.
//
// Invariant I9 says the sim thread does no IO, no HTTP, no engine calls. The input log is
// recorded *by the sim thread* while it ticks, so if this module could open a file the
// invariant would be broken by construction and no amount of discipline at the call site
// would fix it. Instead:
//
//   InputLog::Serialise()  ->  std::vector<u8>   the caller writes those bytes wherever
//   InputLog::Parse(bytes) <-  std::vector<u8>   the caller supplies bytes it already read
//
// The headless application (or the test) owns fopen/fread/fwrite. This file owns the byte
// layout and nothing else. Keep it that way.
// ---------------------------------------------------------------------------------------
//
// THREADING: InputLog has no synchronisation. RecordCommand/RecordCheckpoint are called by
// the SIM THREAD as it ticks, so the log belongs to that thread; Serialise() must not run
// concurrently with recording (it walks both vectors, which push_back can reallocate under
// it). The app serialises after the sim has stopped, or from the sim thread itself.
//
// R1: zero Unreal types. R3: no floats, no wall-clock, no pointer hashing, no unordered
// iteration. The serialised format is explicit little-endian, field by field - never a
// memcpy of a struct, because padding bytes are indeterminate and would make the format
// differ between compilers, which would silently break I1 across toolchains.
#pragma once

#include "livingcity/core/Core.h"
#include "livingcity/sim/Commands.h"

#include <cstddef>
#include <string>
#include <vector>

namespace lc {

// ---------------------------------------------------------------- format constants
// Bump this whenever ANY field, order, or width in the serialised layout changes.
// Parse() rejects a mismatch loudly rather than misinterpreting bytes: a replay that
// silently decodes wrong is worse than one that refuses to load.
inline constexpr u32 kInputLogFormatVersion = 1u;

// Spelled as a brace list, not as the string literal "LCREPLAY": a 9-byte literal
// (8 chars + NUL) does not fit a char[8] in C++, and we deliberately do not store the
// terminator - the magic is 8 raw bytes on the wire.
inline constexpr char kInputLogMagic[8] = {'L', 'C', 'R', 'E', 'P', 'L', 'A', 'Y'};

// Exact on-wire sizes. Asserted against the writer in Replay.cpp so the two cannot drift.
inline constexpr std::size_t kInputLogHeaderBytes   = 8 + 4 + 8 + 8 + 8;      // 36
inline constexpr std::size_t kCommandRecordBytes    = 8 + 1 + 4 + 8 + 8 + 8;  // 37
inline constexpr std::size_t kCheckpointRecordBytes = 8 + 8;                  // 16

// Wire header. The in-memory struct is never blitted; Serialise writes each field
// explicitly in the order declared here.
//
// `magic` has to be spelled out again here because a char[8] member cannot be initialised
// from another array. That makes it a SECOND copy of the format's most load-bearing eight
// bytes, so the static_assert below pins it to kInputLogMagic: without it, editing one copy
// and not the other would make Serialise emit a file that Parse rejects - a writer and a
// reader in the same build silently disagreeing about the format.
struct InputLogHeader {
    char magic[8]        = {'L', 'C', 'R', 'E', 'P', 'L', 'A', 'Y'};
    u32  formatVersion   = kInputLogFormatVersion;
    u64  worldSeed       = 0;
    u64  commandCount    = 0;
    u64  checkpointCount = 0;
};

constexpr bool InputLogHeaderMagicIsCanonical() {
    const InputLogHeader h{};
    for (std::size_t i = 0; i < 8; ++i) {
        if (h.magic[i] != kInputLogMagic[i]) return false;
    }
    return true;
}
static_assert(InputLogHeaderMagicIsCanonical(),
              "InputLogHeader::magic has drifted from kInputLogMagic - the writer and the "
              "reader would disagree about the wire format");

// ---------------------------------------------------------------- records
// The tick a command is *applied* on is authoritative for replay and is stored separately
// from Command::issuedAtTick, which is when the input layer produced it. They differ
// whenever a command crosses the input->sim queue boundary, and replay must reproduce the
// application tick, not the issue tick.
struct CommandRecord {
    TickIndex tick    = 0;
    Command   command = {};
};

struct CheckpointRecord {
    TickIndex tick      = 0;
    u64       worldHash = 0;
};

// ---------------------------------------------------------------- input log
class LC_API InputLog {
public:
    // --- recording (called from the sim thread; see Reserve for the I5 story)
    void RecordCommand(TickIndex tick, const Command& command);
    void RecordCheckpoint(TickIndex tick, u64 worldHash);

    // --- inspection
    const std::vector<CommandRecord>&    Commands() const { return commands_; }
    const std::vector<CheckpointRecord>& Checkpoints() const { return checkpoints_; }

    u64  WorldSeed() const { return worldSeed_; }
    void SetWorldSeed(u64 seed) { worldSeed_ = seed; }

    // The header this log would serialise. Handy for logging a rejection.
    InputLogHeader Header() const;

    // --- lifetime
    // Clear() keeps the buffers' capacity so a replay can be re-run without re-allocating.
    void Clear();
    // Call once up front, off the hot path, so RecordCommand/RecordCheckpoint never grow a
    // vector mid-tick (invariant I5: the sim tick allocates zero bytes in steady state).
    void Reserve(std::size_t commandCapacity, std::size_t checkpointCapacity);

    // --- serialisation (allocates; call from the app, not from a tick)
    std::vector<u8> Serialise() const;

    // Returns false and fills `error` on bad magic, unknown formatVersion, truncation,
    // an out-of-range CommandType, or trailing garbage. Never reads outside `bytes`.
    // On failure `out` is left cleared, never half-populated.
    static bool Parse(const std::vector<u8>& bytes, InputLog& out, std::string& error);

    // --- replay
    // Appends nothing but this tick's commands, in the exact order they were recorded.
    // `out` is cleared first; reserve it once and this does not allocate.
    void CommandsForTick(TickIndex tick, std::vector<Command>& out) const;

private:
    std::vector<CommandRecord>    commands_;
    std::vector<CheckpointRecord> checkpoints_;
    u64                           worldSeed_ = 0;
    // True while recorded command ticks are non-decreasing, which is the normal case and
    // lets CommandsForTick binary-search instead of scanning. Purely an optimisation:
    // the results are identical either way.
    bool                          commandTicksOrdered_ = true;
};

// ---------------------------------------------------------------- divergence report
enum class DivergenceReason : u8 {
    None = 0,           // the two logs agree over everything comparable
    SeedMismatch,       // different world seeds: nothing after this is meaningful
    HashMismatch,       // same tick, different world hash - the classic I1 failure
    TickMismatch,       // checkpoints were taken on different ticks: structural divergence
    ActualEndedEarly,   // actual has fewer checkpoints than expected
    ExpectedEndedEarly, // actual has more checkpoints than expected
};

// How to read Divergence::expectedHash / actualHash for each reason. Getting this wrong
// sends an operator hunting for a hash bug that is not there, so it is spelled out:
//
//   None                - both zero; nothing diverged.
//   SeedMismatch        - the two SEEDS, not hashes. Nothing was compared.
//   HashMismatch        - the two world hashes at firstDivergentTick. Directly comparable.
//   TickMismatch        - hashes from DIFFERENT ticks (expected's at firstDivergentTick,
//                         actual's at whatever tick it recorded there). Comparing them is
//                         meaningless; the tick disagreement is the finding.
//   ActualEndedEarly    - expectedHash is the orphaned checkpoint; actualHash is 0.
//   ExpectedEndedEarly  - actualHash is the orphaned checkpoint; expectedHash is 0.

LC_API const char* DivergenceReasonName(DivergenceReason reason);

struct Divergence {
    bool             diverged           = false;
    TickIndex        firstDivergentTick = 0;
    u64              expectedHash       = 0;
    u64              actualHash         = 0;
    // Number of checkpoint pairs examined, INCLUDING the divergent pair. On a clean run
    // this is simply the number of pairs compared.
    u64              comparedCount      = 0;
    DivergenceReason reason             = DivergenceReason::None;
};

// Reports the FIRST tick at which the two runs disagree - that is the entire diagnostic
// value of this function; a count of differences tells you nothing about the cause.
//
// Order of checks, deliberately: seed first (a seed mismatch makes every later comparison
// noise), then checkpoints pairwise front to back, then length. So a short log whose common
// prefix already diverged reports the hash divergence, not the truncation.
//
// On SeedMismatch, expectedHash/actualHash carry the two seeds rather than hashes; the
// reason field tells you how to read them.
LC_API Divergence CompareCheckpoints(const InputLog& expected, const InputLog& actual);

} // namespace lc
