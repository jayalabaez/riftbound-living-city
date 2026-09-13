// LIVING CITY - deterministic replay harness implementation.
//
// NO FILE IO IN THIS TRANSLATION UNIT (invariant I9). Bytes in, bytes out.
//
// Every multi-byte field is written and read one byte at a time, little-endian, by shifts.
// Not memcpy, not a reinterpret_cast of a struct: shifts are endianness-explicit, immune to
// struct padding, and identical under every compiler we will ever build with. That property
// is what makes a replay recorded by the headless CI binary loadable by the Unreal build,
// which is the whole point of I1.
#include "livingcity/sim/Replay.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace lc {

namespace {

// ---------------------------------------------------------------- little-endian writers
void PutU8(std::vector<u8>& out, u8 v) {
    out.push_back(v);
}

void PutU32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v & 0xFFu));
    out.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 16) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 24) & 0xFFu));
}

void PutU64(std::vector<u8>& out, u64 v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<u8>((v >> (i * 8)) & 0xFFull));
    }
}

// Signed values travel as their two's-complement bit pattern. C++20 guarantees two's
// complement for the signed integer types, so this is exact and portable in both
// directions; no implementation-defined narrowing anywhere.
void PutI64(std::vector<u8>& out, i64 v) {
    PutU64(out, static_cast<u64>(v));
}

// ---------------------------------------------------------------- bounds-checked reader
// Every read is length-checked before a byte is touched, so a truncated or hostile buffer
// can only produce a clean "false", never an out-of-bounds read.
class ByteReader {
public:
    ByteReader(const u8* data, std::size_t size) : data_(data), size_(size) {}

    // pos_ only ever advances through a Remaining() check, so pos_ <= size_ holds and this
    // subtraction can never wrap.
    std::size_t Remaining() const { return size_ - pos_; }

    bool Raw(u8* dst, std::size_t n) {
        if (Remaining() < n) return false;
        for (std::size_t i = 0; i < n; ++i) dst[i] = data_[pos_ + i];
        pos_ += n;
        return true;
    }

    bool U8v(u8& v) {
        if (Remaining() < 1) return false;
        v = data_[pos_];
        pos_ += 1;
        return true;
    }

    bool U32v(u32& v) {
        if (Remaining() < 4) return false;
        v = static_cast<u32>(data_[pos_])
          | (static_cast<u32>(data_[pos_ + 1]) << 8)
          | (static_cast<u32>(data_[pos_ + 2]) << 16)
          | (static_cast<u32>(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return true;
    }

    bool U64v(u64& v) {
        if (Remaining() < 8) return false;
        u64 acc = 0;
        for (int i = 0; i < 8; ++i) {
            acc |= static_cast<u64>(data_[pos_ + static_cast<std::size_t>(i)]) << (i * 8);
        }
        v = acc;
        pos_ += 8;
        return true;
    }

    bool I64v(i64& v) {
        u64 raw = 0;
        if (!U64v(raw)) return false;
        v = static_cast<i64>(raw);
        return true;
    }

private:
    const u8*   data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t pos_  = 0;
};

// Highest legal CommandType value. Commands.h declares:
//   None, SetPaused, StepOnce, SetTimeScale, MovePlaceholder, Interact, Quit
// Parsing anything above this is a corrupt or newer log, and we refuse it loudly instead of
// handing the sim a garbage command that would fork the timeline.
constexpr u8 kMaxCommandTypeValue = static_cast<u8>(CommandType::ModifyTerrain);

void SetError(std::string& error, const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    error = buf;
}

} // namespace

// ---------------------------------------------------------------- recording
void InputLog::RecordCommand(TickIndex tick, const Command& command) {
    if (!commands_.empty() && tick < commands_.back().tick) {
        // Out-of-order recording is legal (a tool may splice a log) but costs CommandsForTick
        // its binary search. It is never produced by the live sim loop.
        commandTicksOrdered_ = false;
    }

    CommandRecord rec;
    rec.tick    = tick;
    rec.command = command;
    // Normalise the padding bytes to zero. This is NOT cosmetic, and it is not only about
    // uninitialised memory - it closes a real I1 hole:
    //
    //   Commands.h::HashInto hashes pad deliberately, so those three bytes are part of the
    //   world hash. But pad is NOT serialised - the wire format has no room for it and it
    //   carries no meaning. So a command that reached the sim with dirty pad would hash one
    //   way on the live run and, after Parse rebuilt it with pad == 0, a different way on the
    //   replay. Same seed, same log, different hash: a divergence with no visible cause.
    //
    // Scrubbing here makes the log canonical at the moment it is written, so what the live
    // run hashes after RecordCommand is byte-for-byte what the replay hashes after Parse.
    // Pinned by replay_record_normalises_command_padding.
    rec.command.pad[0] = 0;
    rec.command.pad[1] = 0;
    rec.command.pad[2] = 0;

    commands_.push_back(rec);
}

void InputLog::RecordCheckpoint(TickIndex tick, u64 worldHash) {
    CheckpointRecord rec;
    rec.tick      = tick;
    rec.worldHash = worldHash;
    checkpoints_.push_back(rec);
}

InputLogHeader InputLog::Header() const {
    InputLogHeader h;
    h.formatVersion   = kInputLogFormatVersion;
    h.worldSeed       = worldSeed_;
    h.commandCount    = static_cast<u64>(commands_.size());
    h.checkpointCount = static_cast<u64>(checkpoints_.size());
    return h;
}

void InputLog::Clear() {
    commands_.clear();
    checkpoints_.clear();
    worldSeed_           = 0;
    commandTicksOrdered_ = true;
}

void InputLog::Reserve(std::size_t commandCapacity, std::size_t checkpointCapacity) {
    commands_.reserve(commandCapacity);
    checkpoints_.reserve(checkpointCapacity);
}

// ---------------------------------------------------------------- serialisation
std::vector<u8> InputLog::Serialise() const {
    const InputLogHeader header = Header();

    std::vector<u8> out;
    out.reserve(kInputLogHeaderBytes
                + commands_.size() * kCommandRecordBytes
                + checkpoints_.size() * kCheckpointRecordBytes);

    // --- header, field by field, in declaration order.
    // The magic comes from kInputLogMagic, not from header.magic: the format constant is the
    // single authority for what goes on the wire, and Parse compares against that same
    // constant. (A static_assert in Replay.h pins header.magic to it, so the two agree.)
    for (std::size_t i = 0; i < 8; ++i) PutU8(out, static_cast<u8>(kInputLogMagic[i]));
    PutU32(out, header.formatVersion);
    PutU64(out, header.worldSeed);
    PutU64(out, header.commandCount);
    PutU64(out, header.checkpointCount);
    LC_ASSERT_MSG(out.size() == kInputLogHeaderBytes, "header size constant is stale");

    // --- commands. Command::pad is deliberately NOT written: padding carries no meaning
    // and writing it would let uninitialised bytes into the format.
    for (const CommandRecord& rec : commands_) {
        const std::size_t before = out.size();
        PutU64(out, rec.tick);
        PutU8(out, static_cast<u8>(rec.command.type));
        PutU32(out, rec.command.arg0);
        PutI64(out, rec.command.arg1);
        PutI64(out, rec.command.arg2);
        PutU64(out, rec.command.issuedAtTick);
        LC_ASSERT_MSG(out.size() - before == kCommandRecordBytes,
                      "command record size constant is stale");
    }

    // --- checkpoints.
    for (const CheckpointRecord& rec : checkpoints_) {
        const std::size_t before = out.size();
        PutU64(out, rec.tick);
        PutU64(out, rec.worldHash);
        LC_ASSERT_MSG(out.size() - before == kCheckpointRecordBytes,
                      "checkpoint record size constant is stale");
    }

    return out;
}

bool InputLog::Parse(const std::vector<u8>& bytes, InputLog& out, std::string& error) {
    // Start from a clean slate so a failed parse can never leave a caller holding half a log
    // and thinking it holds a whole one.
    out.Clear();
    error.clear();

    ByteReader reader(bytes.empty() ? nullptr : bytes.data(), bytes.size());

    // --- magic
    u8 magic[8] = {};
    if (!reader.Raw(magic, 8)) {
        // %llu, not %u: a size_t narrowed to unsigned would misreport a >4 GiB buffer, and an
        // error message that lies about a length is worse than no message at all.
        SetError(error, "truncated: need 8 bytes of magic, have %llu",
                 static_cast<unsigned long long>(bytes.size()));
        return false;
    }
    if (std::memcmp(magic, kInputLogMagic, 8) != 0) {
        SetError(error,
                 "bad magic: expected LCREPLAY, got %02X %02X %02X %02X %02X %02X %02X %02X",
                 magic[0], magic[1], magic[2], magic[3],
                 magic[4], magic[5], magic[6], magic[7]);
        return false;
    }

    // --- version. Refuse anything we were not compiled for; misreading a future layout
    // would produce a plausible-looking but wrong replay, which is the worst outcome.
    u32 version = 0;
    if (!reader.U32v(version)) {
        SetError(error, "truncated: header ends inside formatVersion");
        return false;
    }
    if (version != kInputLogFormatVersion) {
        SetError(error, "format version mismatch: file is %u, this build reads %u",
                 static_cast<unsigned>(version),
                 static_cast<unsigned>(kInputLogFormatVersion));
        return false;
    }

    u64 seed = 0, commandCount = 0, checkpointCount = 0;
    if (!reader.U64v(seed)) {
        SetError(error, "truncated: header ends inside worldSeed");
        return false;
    }
    if (!reader.U64v(commandCount)) {
        SetError(error, "truncated: header ends inside commandCount");
        return false;
    }
    if (!reader.U64v(checkpointCount)) {
        SetError(error, "truncated: header ends inside checkpointCount");
        return false;
    }

    // --- size sanity BEFORE reserving anything. Division, never multiplication: a corrupt
    // count near 2^64 would overflow a multiply and sail straight past the check.
    const u64 remaining = static_cast<u64>(reader.Remaining());
    if (commandCount > remaining / static_cast<u64>(kCommandRecordBytes)) {
        SetError(error, "truncated: header claims %llu commands, only %llu bytes remain",
                 static_cast<unsigned long long>(commandCount),
                 static_cast<unsigned long long>(remaining));
        return false;
    }
    const u64 afterCommands = remaining - commandCount * static_cast<u64>(kCommandRecordBytes);
    if (checkpointCount > afterCommands / static_cast<u64>(kCheckpointRecordBytes)) {
        SetError(error, "truncated: header claims %llu checkpoints, only %llu bytes remain",
                 static_cast<unsigned long long>(checkpointCount),
                 static_cast<unsigned long long>(afterCommands));
        return false;
    }

    out.SetWorldSeed(seed);
    out.Reserve(static_cast<std::size_t>(commandCount),
                static_cast<std::size_t>(checkpointCount));

    // --- commands
    for (u64 i = 0; i < commandCount; ++i) {
        u64 tick = 0;
        u8  type = 0;
        u32 arg0 = 0;
        i64 arg1 = 0, arg2 = 0;
        u64 issuedAt = 0;

        const bool ok = reader.U64v(tick) && reader.U8v(type) && reader.U32v(arg0)
                     && reader.I64v(arg1) && reader.I64v(arg2) && reader.U64v(issuedAt);
        if (!ok) {
            SetError(error, "truncated: command record %llu is incomplete",
                     static_cast<unsigned long long>(i));
            out.Clear();
            return false;
        }
        if (type > kMaxCommandTypeValue) {
            SetError(error, "command record %llu has out-of-range CommandType %u",
                     static_cast<unsigned long long>(i), static_cast<unsigned>(type));
            out.Clear();
            return false;
        }

        Command cmd{};                       // zero-initialised: pad stays 0
        cmd.type         = static_cast<CommandType>(type);
        cmd.arg0         = arg0;
        cmd.arg1         = arg1;
        cmd.arg2         = arg2;
        cmd.issuedAtTick = issuedAt;
        out.RecordCommand(tick, cmd);
    }

    // --- checkpoints
    for (u64 i = 0; i < checkpointCount; ++i) {
        u64 tick = 0, hash = 0;
        if (!reader.U64v(tick) || !reader.U64v(hash)) {
            SetError(error, "truncated: checkpoint record %llu is incomplete",
                     static_cast<unsigned long long>(i));
            out.Clear();
            return false;
        }
        out.RecordCheckpoint(tick, hash);
    }

    // --- nothing may follow. Trailing bytes mean the writer and this reader disagree about
    // the layout, and guessing which of us is right is not a service to anybody.
    if (reader.Remaining() != 0) {
        SetError(error, "%llu trailing bytes after the last record",
                 static_cast<unsigned long long>(reader.Remaining()));
        out.Clear();
        return false;
    }

    return true;
}

// ---------------------------------------------------------------- replay lookup
void InputLog::CommandsForTick(TickIndex tick, std::vector<Command>& out) const {
    out.clear();

    if (commandTicksOrdered_) {
        // Ticks are non-decreasing, so the run for `tick` is contiguous and lower_bound
        // lands on its first element - which preserves the recorded order within the tick,
        // because a stable partition point cannot reorder equal keys.
        const auto begin = commands_.begin();
        const auto end   = commands_.end();
        auto it = std::lower_bound(begin, end, tick,
                                   [](const CommandRecord& rec, TickIndex t) {
                                       return rec.tick < t;
                                   });
        for (; it != end && it->tick == tick; ++it) {
            out.push_back(it->command);
        }
        return;
    }

    // Fallback for a spliced/edited log: a front-to-back scan, which by construction yields
    // the exact recording order.
    for (const CommandRecord& rec : commands_) {
        if (rec.tick == tick) out.push_back(rec.command);
    }
}

// ---------------------------------------------------------------- divergence
const char* DivergenceReasonName(DivergenceReason reason) {
    switch (reason) {
        case DivergenceReason::None:               return "none";
        case DivergenceReason::SeedMismatch:       return "seed-mismatch";
        case DivergenceReason::HashMismatch:       return "hash-mismatch";
        case DivergenceReason::TickMismatch:       return "tick-mismatch";
        case DivergenceReason::ActualEndedEarly:   return "actual-ended-early";
        case DivergenceReason::ExpectedEndedEarly: return "expected-ended-early";
    }
    return "unknown";
}

Divergence CompareCheckpoints(const InputLog& expected, const InputLog& actual) {
    Divergence d;

    // 1. Seeds. If these differ the two runs are not the same experiment at all, and every
    // checkpoint comparison below would report noise. Fail here, before comparing anything.
    if (expected.WorldSeed() != actual.WorldSeed()) {
        d.diverged           = true;
        d.reason             = DivergenceReason::SeedMismatch;
        d.firstDivergentTick = 0;
        d.expectedHash       = expected.WorldSeed();  // seeds, not hashes - see the header
        d.actualHash         = actual.WorldSeed();
        d.comparedCount      = 0;
        return d;
    }

    const std::vector<CheckpointRecord>& e = expected.Checkpoints();
    const std::vector<CheckpointRecord>& a = actual.Checkpoints();
    const std::size_t common = e.size() < a.size() ? e.size() : a.size();

    // 2. Pairwise, front to back, stopping at the FIRST disagreement.
    for (std::size_t i = 0; i < common; ++i) {
        d.comparedCount = static_cast<u64>(i) + 1u;

        if (e[i].tick != a[i].tick) {
            d.diverged           = true;
            d.reason             = DivergenceReason::TickMismatch;
            d.firstDivergentTick = e[i].tick;
            d.expectedHash       = e[i].worldHash;
            d.actualHash         = a[i].worldHash;
            return d;
        }
        if (e[i].worldHash != a[i].worldHash) {
            d.diverged           = true;
            d.reason             = DivergenceReason::HashMismatch;
            d.firstDivergentTick = e[i].tick;
            d.expectedHash       = e[i].worldHash;
            d.actualHash         = a[i].worldHash;
            return d;
        }
    }

    // 3. Same prefix, different length: one run stopped early. Report the tick of the first
    // checkpoint that has no counterpart, so the operator knows where the run died.
    if (a.size() < e.size()) {
        d.diverged           = true;
        d.reason             = DivergenceReason::ActualEndedEarly;
        d.firstDivergentTick = e[common].tick;
        d.expectedHash       = e[common].worldHash;
        d.actualHash         = 0;
        return d;
    }
    if (e.size() < a.size()) {
        d.diverged           = true;
        d.reason             = DivergenceReason::ExpectedEndedEarly;
        d.firstDivergentTick = a[common].tick;
        d.expectedHash       = 0;
        d.actualHash         = a[common].worldHash;
        return d;
    }

    return d;  // diverged == false, comparedCount == common
}

} // namespace lc
