// LIVING CITY - tests for the replay harness (I1) and the telemetry feed.
//
// These are invariant tests, not getter tests. What they actually pin down:
//   * the serialised byte layout, exactly, including endianness and total size, so that a
//     refactor which changes the format is caught here rather than by a replay that silently
//     decodes into a different world;
//   * that a corrupt or truncated buffer is always rejected and never read out of bounds;
//   * that CompareCheckpoints names the FIRST divergent tick - the only fact that is
//     diagnostically useful when determinism breaks;
//   * that the record paths allocate nothing (I5).
//
// No main() here: the runner supplies one.
#include "TestFramework.h"

#include "livingcity/sim/Commands.h"
#include "livingcity/sim/Replay.h"
#include "livingcity/sim/Telemetry.h"

#include <string>
#include <vector>

namespace {

using namespace lc;

Command MakeCommand(CommandType type, u32 arg0, i64 arg1, i64 arg2, TickIndex issuedAt) {
    Command c{};                      // zero-initialised, so pad is deterministic
    c.type         = type;
    c.arg0         = arg0;
    c.arg1         = arg1;
    c.arg2         = arg2;
    c.issuedAtTick = issuedAt;
    return c;
}

bool SameCommand(const Command& a, const Command& b) {
    return a.type == b.type
        && a.arg0 == b.arg0
        && a.arg1 == b.arg1
        && a.arg2 == b.arg2
        && a.issuedAtTick == b.issuedAtTick;
}

// FNV-1a over the whole buffer, via the engine's own Hasher. Used as a compact known-answer
// fingerprint of the wire format: if any field, order, or width changes, this changes.
u64 HashBytes(const std::vector<u8>& bytes) {
    Hasher h;
    h.Bytes(bytes.empty() ? nullptr : bytes.data(), bytes.size());
    h.U64(static_cast<u64>(bytes.size()));
    return h.Value();
}

std::size_t CountChar(const std::string& s, char c) {
    std::size_t n = 0;
    for (char ch : s) {
        if (ch == c) ++n;
    }
    return n;
}

// A small, fully populated log used by several tests. Deliberately includes negative i64
// arguments and a high bit pattern so sign handling and byte order are both exercised.
InputLog MakeSampleLog() {
    InputLog log;
    log.SetWorldSeed(0x0123456789ABCDEFull);
    log.RecordCommand(5,  MakeCommand(CommandType::SetPaused,       1u, 0, 0, 5));
    log.RecordCommand(5,  MakeCommand(CommandType::SetTimeScale,    7u, -2, 1ll << 40, 4));
    log.RecordCommand(9,  MakeCommand(CommandType::MovePlaceholder, 0xFFFFFFFFu,
                                      -9223372036854775807ll - 1, 123456789ll, 9));
    log.RecordCommand(11, MakeCommand(CommandType::Quit,            0u, 0, 0, 11));
    log.RecordCheckpoint(0,  0xDEADBEEFCAFEBABEull);
    log.RecordCheckpoint(10, 0x0000000000000001ull);
    log.RecordCheckpoint(20, 0xFFFFFFFFFFFFFFFFull);
    return log;
}

// Builds just a header, so a test can hand Parse a deliberately absurd count without having
// to fabricate matching payload bytes.
std::vector<u8> MakeHeaderBytes(u64 seed, u64 commandCount, u64 checkpointCount, u32 version) {
    std::vector<u8> b;
    for (std::size_t i = 0; i < 8; ++i) b.push_back(static_cast<u8>(kInputLogMagic[i]));
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>((version >> (i * 8)) & 0xFFu));
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<u8>((seed >> (i * 8)) & 0xFFull));
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<u8>((commandCount >> (i * 8)) & 0xFFull));
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<u8>((checkpointCount >> (i * 8)) & 0xFFull));
    return b;
}

} // namespace

// =====================================================================================
// InputLog: serialisation round trip
// =====================================================================================

LC_TEST(replay_roundtrip_preserves_seed_commands_and_checkpoints) {
    const InputLog original = MakeSampleLog();
    const std::vector<u8> bytes = original.Serialise();

    // Parse into a log that already holds junk: a failed or partial reuse must not leak
    // through into the parsed result.
    InputLog parsed;
    parsed.SetWorldSeed(999);
    parsed.RecordCommand(77, MakeCommand(CommandType::Interact, 5u, 5, 5, 77));
    parsed.RecordCheckpoint(77, 77);

    std::string error = "not cleared";
    const bool ok = InputLog::Parse(bytes, parsed, error);
    LC_CHECK(ok);
    LC_CHECK_EQ(error, std::string());

    LC_CHECK_EQ(parsed.WorldSeed(), original.WorldSeed());
    LC_CHECK_EQ(static_cast<u64>(parsed.Commands().size()),
                static_cast<u64>(original.Commands().size()));
    LC_CHECK_EQ(static_cast<u64>(parsed.Checkpoints().size()),
                static_cast<u64>(original.Checkpoints().size()));

    for (std::size_t i = 0; i < original.Commands().size() && i < parsed.Commands().size(); ++i) {
        LC_CHECK_EQ(parsed.Commands()[i].tick, original.Commands()[i].tick);
        LC_CHECK(SameCommand(parsed.Commands()[i].command, original.Commands()[i].command));
    }
    for (std::size_t i = 0;
         i < original.Checkpoints().size() && i < parsed.Checkpoints().size(); ++i) {
        LC_CHECK_EQ(parsed.Checkpoints()[i].tick, original.Checkpoints()[i].tick);
        LC_CHECK_EQ(parsed.Checkpoints()[i].worldHash, original.Checkpoints()[i].worldHash);
    }

    // Re-serialising a parsed log must reproduce the identical byte stream. This is the
    // property replay actually depends on: record -> save -> load -> record again is a
    // fixed point, so a replay can be chained without drifting.
    const std::vector<u8> again = parsed.Serialise();
    LC_CHECK_EQ(static_cast<u64>(again.size()), static_cast<u64>(bytes.size()));
    LC_CHECK_EQ(HashBytes(again), HashBytes(bytes));
}

LC_TEST(replay_wire_layout_is_pinned) {
    const InputLog log = MakeSampleLog();
    const std::vector<u8> bytes = log.Serialise();

    // Size is a pure function of the record counts. 36 + 4*37 + 3*16 = 232.
    const u64 expectedSize = static_cast<u64>(kInputLogHeaderBytes)
                           + 4ull * static_cast<u64>(kCommandRecordBytes)
                           + 3ull * static_cast<u64>(kCheckpointRecordBytes);
    LC_CHECK_EQ(static_cast<u64>(bytes.size()), expectedSize);
    LC_CHECK_EQ(expectedSize, static_cast<u64>(232));

    // Magic, verbatim, no NUL terminator.
    LC_CHECK_EQ(bytes[0], static_cast<u8>('L'));
    LC_CHECK_EQ(bytes[1], static_cast<u8>('C'));
    LC_CHECK_EQ(bytes[2], static_cast<u8>('R'));
    LC_CHECK_EQ(bytes[3], static_cast<u8>('E'));
    LC_CHECK_EQ(bytes[4], static_cast<u8>('P'));
    LC_CHECK_EQ(bytes[5], static_cast<u8>('L'));
    LC_CHECK_EQ(bytes[6], static_cast<u8>('A'));
    LC_CHECK_EQ(bytes[7], static_cast<u8>('Y'));

    // formatVersion, little-endian u32.
    LC_CHECK_EQ(bytes[8],  static_cast<u8>(kInputLogFormatVersion & 0xFFu));
    LC_CHECK_EQ(bytes[9],  static_cast<u8>(0));
    LC_CHECK_EQ(bytes[10], static_cast<u8>(0));
    LC_CHECK_EQ(bytes[11], static_cast<u8>(0));

    // worldSeed 0x0123456789ABCDEF, little-endian: least significant byte first.
    LC_CHECK_EQ(bytes[12], static_cast<u8>(0xEF));
    LC_CHECK_EQ(bytes[13], static_cast<u8>(0xCD));
    LC_CHECK_EQ(bytes[14], static_cast<u8>(0xAB));
    LC_CHECK_EQ(bytes[15], static_cast<u8>(0x89));
    LC_CHECK_EQ(bytes[16], static_cast<u8>(0x67));
    LC_CHECK_EQ(bytes[17], static_cast<u8>(0x45));
    LC_CHECK_EQ(bytes[18], static_cast<u8>(0x23));
    LC_CHECK_EQ(bytes[19], static_cast<u8>(0x01));

    // commandCount = 4, checkpointCount = 3, both little-endian u64.
    LC_CHECK_EQ(bytes[20], static_cast<u8>(4));
    LC_CHECK_EQ(bytes[27], static_cast<u8>(0));
    LC_CHECK_EQ(bytes[28], static_cast<u8>(3));
    LC_CHECK_EQ(bytes[35], static_cast<u8>(0));

    // First command record starts at 36: tick 5, then the type byte at 44.
    LC_CHECK_EQ(bytes[36], static_cast<u8>(5));
    LC_CHECK_EQ(bytes[44], static_cast<u8>(CommandType::SetPaused));

    // Whole-buffer fingerprint. Hard-coded from this implementation's actual output, so a
    // future change to field order, width, or endianness fails here loudly.
    LC_CHECK_EQ(HashBytes(bytes), 0x0DC55138729662F7ull);
}

LC_TEST(replay_record_normalises_command_padding) {
    // Commands.h hashes Command::pad on purpose, so those three bytes are part of the world
    // hash - but they are NOT on the wire. That combination is an I1 trap: a command that
    // reached the sim with dirty pad would hash one way live and another way after a replay
    // rebuilt it with pad == 0. RecordCommand closes it by scrubbing pad as the log is
    // written. Without this test the scrub can be deleted and every other test still passes.
    Command dirty = MakeCommand(CommandType::Interact, 7u, -5, 9, 3);
    dirty.pad[0] = 0xABu;
    dirty.pad[1] = 0xCDu;
    dirty.pad[2] = 0xEFu;

    InputLog log;
    log.RecordCommand(3, dirty);
    LC_CHECK_EQ(static_cast<u64>(log.Commands().size()), static_cast<u64>(1));
    if (log.Commands().size() != 1) return;

    const Command stored = log.Commands()[0].command;
    LC_CHECK_EQ(stored.pad[0], static_cast<u8>(0));
    LC_CHECK_EQ(stored.pad[1], static_cast<u8>(0));
    LC_CHECK_EQ(stored.pad[2], static_cast<u8>(0));
    // The scrub must touch nothing but the padding.
    LC_CHECK(SameCommand(stored, dirty));

    // The property that actually matters: the hash the live sim computes from the stored
    // command equals the hash a replay computes after reloading it. If pad survived recording
    // these two differ, and I1 fails on a run nobody can reproduce.
    Hasher liveH;
    HashInto(liveH, stored);

    const std::vector<u8> bytes = log.Serialise();
    InputLog parsed;
    std::string error;
    LC_CHECK(InputLog::Parse(bytes, parsed, error));
    LC_CHECK_EQ(static_cast<u64>(parsed.Commands().size()), static_cast<u64>(1));
    if (parsed.Commands().size() != 1) return;

    const Command reloaded = parsed.Commands()[0].command;
    LC_CHECK_EQ(reloaded.pad[0], static_cast<u8>(0));
    LC_CHECK_EQ(reloaded.pad[1], static_cast<u8>(0));
    LC_CHECK_EQ(reloaded.pad[2], static_cast<u8>(0));

    Hasher replayH;
    HashInto(replayH, reloaded);
    LC_CHECK_EQ(replayH.Value(), liveH.Value());
}

// =====================================================================================
// InputLog::Parse rejection paths - none of these may read out of bounds
// =====================================================================================

LC_TEST(replay_parse_rejects_bad_magic) {
    std::vector<u8> bytes = MakeSampleLog().Serialise();
    bytes[3] = static_cast<u8>('X');

    InputLog out;
    std::string error;
    LC_CHECK_FALSE(InputLog::Parse(bytes, out, error));
    LC_CHECK(!error.empty());
    LC_CHECK_EQ(static_cast<u64>(out.Commands().size()), static_cast<u64>(0));
    LC_CHECK_EQ(static_cast<u64>(out.Checkpoints().size()), static_cast<u64>(0));
    LC_CHECK_EQ(out.WorldSeed(), static_cast<u64>(0));
}

LC_TEST(replay_parse_rejects_wrong_format_version) {
    std::vector<u8> bytes = MakeSampleLog().Serialise();
    bytes[8] = static_cast<u8>(kInputLogFormatVersion + 1u);   // a "newer" log

    InputLog out;
    std::string error;
    LC_CHECK_FALSE(InputLog::Parse(bytes, out, error));
    LC_CHECK(!error.empty());
    LC_CHECK_EQ(static_cast<u64>(out.Commands().size()), static_cast<u64>(0));

    // A version of zero must be rejected too - an all-zero header is what a partially
    // written file looks like.
    bytes[8] = 0;
    LC_CHECK_FALSE(InputLog::Parse(bytes, out, error));
}

LC_TEST(replay_parse_rejects_every_truncation) {
    const std::vector<u8> full = MakeSampleLog().Serialise();
    LC_CHECK_EQ(static_cast<u64>(full.size()), static_cast<u64>(232));

    // Every single prefix, byte by byte, from empty to one-short. Any prefix that parsed
    // "successfully" would mean a half-written replay file could load as a whole one.
    u64 accepted = 0;
    for (std::size_t len = 0; len < full.size(); ++len) {
        const std::vector<u8> prefix(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(len));
        InputLog out;
        std::string error;
        if (InputLog::Parse(prefix, out, error)) {
            ++accepted;
        } else {
            // A rejected parse must leave nothing behind and must say why. WorldSeed is part
            // of "nothing": the seed is read from the header BEFORE the records are, so it is
            // the one field a half-finished parse could plausibly leave behind.
            if (!out.Commands().empty() || !out.Checkpoints().empty()
                || out.WorldSeed() != 0 || error.empty()) {
                ++accepted;   // counts as a failure of the contract
            }
        }
    }
    LC_CHECK_EQ(accepted, static_cast<u64>(0));

    // Spot-check the interesting boundaries explicitly so a failure names the length.
    const std::size_t interesting[] = {
        0, 1, 7, 8, 11, 12, 35, 36, 37, 72, 73, 183, 215, 231,
    };
    for (std::size_t len : interesting) {
        const std::vector<u8> prefix(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(len));
        InputLog out;
        std::string error;
        const bool ok = InputLog::Parse(prefix, out, error);
        if (ok) {
            ::lc::test::ReportFailure(__FILE__, __LINE__,
                std::string("truncated buffer of length ") + std::to_string(len) + " parsed");
        }
    }

    // ...and the full buffer still parses, so the loop above was not vacuous.
    InputLog out;
    std::string error;
    LC_CHECK(InputLog::Parse(full, out, error));
}

// These two rejections are the ones that matter for the "never partially mutates" contract:
// they fire AFTER the seed has been read and AFTER records have been appended, so they are
// the only paths where a half-populated log could escape. Everything is asserted, not just
// the command count - dropping the Clear() from either path must fail this test.
LC_TEST(replay_parse_rejects_trailing_bytes) {
    std::vector<u8> bytes = MakeSampleLog().Serialise();
    bytes.push_back(0);

    InputLog out;
    std::string error;
    LC_CHECK_FALSE(InputLog::Parse(bytes, out, error));
    LC_CHECK(!error.empty());
    LC_CHECK_EQ(static_cast<u64>(out.Commands().size()), static_cast<u64>(0));
    // Every record parsed cleanly and the seed was already stored before the trailing byte
    // was noticed - so this is precisely the case where a missing rollback would show.
    LC_CHECK_EQ(static_cast<u64>(out.Checkpoints().size()), static_cast<u64>(0));
    LC_CHECK_EQ(out.WorldSeed(), static_cast<u64>(0));
}

LC_TEST(replay_parse_rejects_out_of_range_command_type) {
    std::vector<u8> bytes = MakeSampleLog().Serialise();
    bytes[44] = 200;   // the first command record's type byte

    InputLog out;
    std::string error;
    LC_CHECK_FALSE(InputLog::Parse(bytes, out, error));
    LC_CHECK(!error.empty());
    LC_CHECK_EQ(static_cast<u64>(out.Commands().size()), static_cast<u64>(0));
    LC_CHECK_EQ(static_cast<u64>(out.Checkpoints().size()), static_cast<u64>(0));
    LC_CHECK_EQ(out.WorldSeed(), static_cast<u64>(0));

    // Reject on a LATER record too, so the rollback is exercised with commands already
    // appended rather than on the very first one.
    const std::size_t thirdTypeByte = 44u + 2u * kCommandRecordBytes;   // 118
    bytes[44] = static_cast<u8>(CommandType::SetPaused);
    bytes[thirdTypeByte] = 99;
    LC_CHECK_FALSE(InputLog::Parse(bytes, out, error));
    LC_CHECK_EQ(static_cast<u64>(out.Commands().size()), static_cast<u64>(0));
    LC_CHECK_EQ(out.WorldSeed(), static_cast<u64>(0));

    // Both ends of the legal range must still be accepted, or the bound is off by one.
    bytes[thirdTypeByte] = static_cast<u8>(CommandType::MovePlaceholder);
    bytes[44] = static_cast<u8>(CommandType::ModifyTerrain); // highest legal value
    LC_CHECK(InputLog::Parse(bytes, out, error));
    bytes[44] = static_cast<u8>(CommandType::None);          // lowest legal value
    LC_CHECK(InputLog::Parse(bytes, out, error));

    // ...and one past the top is still rejected, pinning the bound from the other side.
    bytes[44] = static_cast<u8>(static_cast<u8>(CommandType::ModifyTerrain) + 1u);
    LC_CHECK_FALSE(InputLog::Parse(bytes, out, error));
}

LC_TEST(replay_parse_rejects_absurd_counts_without_overflowing) {
    // A header claiming ~2^64 commands with no payload. The size check must be done by
    // division, not multiplication, or this wraps and sails through into a huge read.
    {
        const std::vector<u8> bytes = MakeHeaderBytes(1, 0xFFFFFFFFFFFFFFFFull, 0,
                                                      kInputLogFormatVersion);
        InputLog out;
        std::string error;
        LC_CHECK_FALSE(InputLog::Parse(bytes, out, error));
        LC_CHECK(!error.empty());
    }
    {
        const std::vector<u8> bytes = MakeHeaderBytes(1, 0, 0x2000000000000000ull,
                                                      kInputLogFormatVersion);
        InputLog out;
        std::string error;
        LC_CHECK_FALSE(InputLog::Parse(bytes, out, error));
    }
    // An empty log is legal and must round-trip: zero commands, zero checkpoints.
    {
        const std::vector<u8> bytes = MakeHeaderBytes(4242, 0, 0, kInputLogFormatVersion);
        LC_CHECK_EQ(static_cast<u64>(bytes.size()),
                    static_cast<u64>(kInputLogHeaderBytes));
        InputLog out;
        std::string error;
        LC_CHECK(InputLog::Parse(bytes, out, error));
        LC_CHECK_EQ(out.WorldSeed(), static_cast<u64>(4242));
        LC_CHECK_EQ(static_cast<u64>(out.Commands().size()), static_cast<u64>(0));
    }
}

// =====================================================================================
// CommandsForTick
// =====================================================================================

LC_TEST(replay_commands_for_tick_returns_only_that_tick_in_order) {
    InputLog log;
    log.RecordCommand(3, MakeCommand(CommandType::SetPaused,    1u, 0, 0, 3));
    log.RecordCommand(7, MakeCommand(CommandType::SetTimeScale, 10u, 0, 0, 7));
    log.RecordCommand(7, MakeCommand(CommandType::Interact,     20u, 0, 0, 7));
    log.RecordCommand(7, MakeCommand(CommandType::StepOnce,     30u, 0, 0, 7));
    log.RecordCommand(9, MakeCommand(CommandType::Quit,         40u, 0, 0, 9));

    std::vector<Command> out;
    out.push_back(MakeCommand(CommandType::Interact, 999u, 0, 0, 0));   // must be cleared

    log.CommandsForTick(7, out);
    LC_CHECK_EQ(static_cast<u64>(out.size()), static_cast<u64>(3));
    // Order within a tick is the recorded order. Not sorted, not reversed: recorded.
    LC_CHECK_EQ(out[0].arg0, static_cast<u32>(10));
    LC_CHECK_EQ(out[1].arg0, static_cast<u32>(20));
    LC_CHECK_EQ(out[2].arg0, static_cast<u32>(30));
    LC_CHECK(out[0].type == CommandType::SetTimeScale);
    LC_CHECK(out[1].type == CommandType::Interact);
    LC_CHECK(out[2].type == CommandType::StepOnce);

    log.CommandsForTick(3, out);
    LC_CHECK_EQ(static_cast<u64>(out.size()), static_cast<u64>(1));
    LC_CHECK_EQ(out[0].arg0, static_cast<u32>(1));

    // A tick with no commands yields an empty result, not the previous one.
    log.CommandsForTick(8, out);
    LC_CHECK_EQ(static_cast<u64>(out.size()), static_cast<u64>(0));
    log.CommandsForTick(0, out);
    LC_CHECK_EQ(static_cast<u64>(out.size()), static_cast<u64>(0));
    log.CommandsForTick(1000, out);
    LC_CHECK_EQ(static_cast<u64>(out.size()), static_cast<u64>(0));
}

LC_TEST(replay_commands_for_tick_handles_an_out_of_order_log) {
    // A spliced or hand-edited log is not tick-ordered. The lookup must still return the
    // right commands in recorded order, because replay determinism depends on the order,
    // not on the storage layout.
    InputLog log;
    log.RecordCommand(5, MakeCommand(CommandType::SetTimeScale, 1u, 0, 0, 5));
    log.RecordCommand(3, MakeCommand(CommandType::SetPaused,    2u, 0, 0, 3));
    log.RecordCommand(5, MakeCommand(CommandType::Interact,     3u, 0, 0, 5));
    log.RecordCommand(1, MakeCommand(CommandType::StepOnce,     4u, 0, 0, 1));
    log.RecordCommand(5, MakeCommand(CommandType::Quit,         5u, 0, 0, 5));

    std::vector<Command> out;
    log.CommandsForTick(5, out);
    LC_CHECK_EQ(static_cast<u64>(out.size()), static_cast<u64>(3));
    LC_CHECK_EQ(out[0].arg0, static_cast<u32>(1));
    LC_CHECK_EQ(out[1].arg0, static_cast<u32>(3));
    LC_CHECK_EQ(out[2].arg0, static_cast<u32>(5));

    log.CommandsForTick(1, out);
    LC_CHECK_EQ(static_cast<u64>(out.size()), static_cast<u64>(1));
    LC_CHECK_EQ(out[0].arg0, static_cast<u32>(4));

    // The same log through serialise/parse must answer identically: parsing replays the
    // records in file order, so the out-of-order structure survives.
    const std::vector<u8> bytes = log.Serialise();
    InputLog parsed;
    std::string error;
    LC_CHECK(InputLog::Parse(bytes, parsed, error));
    std::vector<Command> out2;
    parsed.CommandsForTick(5, out2);
    LC_CHECK_EQ(static_cast<u64>(out2.size()), static_cast<u64>(3));
    LC_CHECK_EQ(out2[0].arg0, static_cast<u32>(1));
    LC_CHECK_EQ(out2[1].arg0, static_cast<u32>(3));
    LC_CHECK_EQ(out2[2].arg0, static_cast<u32>(5));
}

LC_TEST(replay_clear_resets_the_tick_ordering_state) {
    // CommandsForTick picks between a binary search and a linear scan on a cached "ticks are
    // non-decreasing" flag. If Clear() left that flag stale, a log that was spliced, cleared,
    // and then re-recorded IN ORDER would binary-search a range it had wrongly concluded was
    // unsorted (harmless), and - far worse - the reverse would binary-search an unsorted
    // range and silently return the wrong commands for a tick. That is an I1 break that no
    // other test in this file would notice, so pin both directions.
    InputLog log;

    // Out of order first, so the flag is definitely in its non-default state.
    log.RecordCommand(9, MakeCommand(CommandType::Quit,      1u, 0, 0, 9));
    log.RecordCommand(2, MakeCommand(CommandType::StepOnce,  2u, 0, 0, 2));
    log.SetWorldSeed(1234);

    std::vector<Command> out;
    log.CommandsForTick(2, out);
    LC_CHECK_EQ(static_cast<u64>(out.size()), static_cast<u64>(1));

    log.Clear();
    LC_CHECK_EQ(static_cast<u64>(log.Commands().size()), static_cast<u64>(0));
    LC_CHECK_EQ(static_cast<u64>(log.Checkpoints().size()), static_cast<u64>(0));
    LC_CHECK_EQ(log.WorldSeed(), static_cast<u64>(0));   // the seed is data, and Clear clears it

    // Now strictly in order. The binary-search path must be correct here.
    for (u32 i = 0; i < 12; ++i) {
        log.RecordCommand(i / 3u, MakeCommand(CommandType::Interact, i, 0, 0, i));
    }
    log.CommandsForTick(2, out);
    LC_CHECK_EQ(static_cast<u64>(out.size()), static_cast<u64>(3));
    LC_CHECK_EQ(out[0].arg0, static_cast<u32>(6));
    LC_CHECK_EQ(out[1].arg0, static_cast<u32>(7));
    LC_CHECK_EQ(out[2].arg0, static_cast<u32>(8));

    // And the other direction: in-order log, cleared, then re-recorded out of order.
    log.Clear();
    log.RecordCommand(7, MakeCommand(CommandType::SetPaused, 70u, 0, 0, 7));
    log.RecordCommand(1, MakeCommand(CommandType::SetPaused, 10u, 0, 0, 1));
    log.RecordCommand(7, MakeCommand(CommandType::SetPaused, 71u, 0, 0, 7));
    log.CommandsForTick(7, out);
    LC_CHECK_EQ(static_cast<u64>(out.size()), static_cast<u64>(2));
    LC_CHECK_EQ(out[0].arg0, static_cast<u32>(70));
    LC_CHECK_EQ(out[1].arg0, static_cast<u32>(71));
    log.CommandsForTick(1, out);
    LC_CHECK_EQ(static_cast<u64>(out.size()), static_cast<u64>(1));
    LC_CHECK_EQ(out[0].arg0, static_cast<u32>(10));
}

LC_TEST(replay_header_reports_what_serialise_writes) {
    // Header() is what a caller logs when it rejects a file, so it has to describe the bytes
    // Serialise actually emits rather than some stale snapshot of them.
    InputLog log;
    const InputLogHeader empty = log.Header();
    LC_CHECK_EQ(empty.formatVersion, kInputLogFormatVersion);
    LC_CHECK_EQ(empty.worldSeed, static_cast<u64>(0));
    LC_CHECK_EQ(empty.commandCount, static_cast<u64>(0));
    LC_CHECK_EQ(empty.checkpointCount, static_cast<u64>(0));

    const InputLog sample = MakeSampleLog();
    const InputLogHeader h = sample.Header();
    LC_CHECK_EQ(h.worldSeed, sample.WorldSeed());
    LC_CHECK_EQ(h.commandCount, static_cast<u64>(sample.Commands().size()));
    LC_CHECK_EQ(h.checkpointCount, static_cast<u64>(sample.Checkpoints().size()));

    // The header's magic and the format constant are two separate spellings of the same
    // eight bytes; a drift between them would make this build write files it cannot read.
    const std::vector<u8> bytes = sample.Serialise();
    for (std::size_t i = 0; i < 8; ++i) {
        LC_CHECK_EQ(bytes[i], static_cast<u8>(h.magic[i]));
        LC_CHECK_EQ(bytes[i], static_cast<u8>(kInputLogMagic[i]));
    }
    // ...and the counts in the header are the counts on the wire.
    LC_CHECK_EQ(static_cast<u64>(bytes.size()),
                static_cast<u64>(kInputLogHeaderBytes)
                    + h.commandCount * static_cast<u64>(kCommandRecordBytes)
                    + h.checkpointCount * static_cast<u64>(kCheckpointRecordBytes));
}

// =====================================================================================
// CompareCheckpoints
// =====================================================================================

namespace {

InputLog MakeCheckpointLog(u64 seed, std::size_t count) {
    InputLog log;
    log.SetWorldSeed(seed);
    for (std::size_t i = 0; i < count; ++i) {
        log.RecordCheckpoint(static_cast<TickIndex>(i * 100u),
                             0x1000ull + static_cast<u64>(i));
    }
    return log;
}

} // namespace

LC_TEST(replay_compare_identical_logs_reports_no_divergence) {
    const InputLog a = MakeCheckpointLog(77, 8);
    const InputLog b = MakeCheckpointLog(77, 8);

    const Divergence d = CompareCheckpoints(a, b);
    LC_CHECK_FALSE(d.diverged);
    LC_CHECK_EQ(d.comparedCount, static_cast<u64>(8));
    LC_CHECK_EQ(std::string(DivergenceReasonName(d.reason)), std::string("none"));
    LC_CHECK_EQ(d.firstDivergentTick, static_cast<TickIndex>(0));

    // Two empty logs with the same seed also agree: nothing to compare is not a divergence.
    const InputLog e1 = MakeCheckpointLog(5, 0);
    const InputLog e2 = MakeCheckpointLog(5, 0);
    const Divergence de = CompareCheckpoints(e1, e2);
    LC_CHECK_FALSE(de.diverged);
    LC_CHECK_EQ(de.comparedCount, static_cast<u64>(0));
}

LC_TEST(replay_compare_reports_the_first_divergent_tick_not_the_last) {
    InputLog expected = MakeCheckpointLog(77, 8);
    InputLog actual;
    actual.SetWorldSeed(77);
    for (std::size_t i = 0; i < 8; ++i) {
        u64 hash = 0x1000ull + static_cast<u64>(i);
        if (i == 3) hash ^= 0x40ull;   // first break, at tick 300
        if (i == 6) hash ^= 0x80ull;   // a later one that must NOT be reported
        actual.RecordCheckpoint(static_cast<TickIndex>(i * 100u), hash);
    }

    const Divergence d = CompareCheckpoints(expected, actual);
    LC_CHECK(d.diverged);
    LC_CHECK_EQ(std::string(DivergenceReasonName(d.reason)), std::string("hash-mismatch"));
    // The FIRST divergent tick. Not 600, not "2 differences".
    LC_CHECK_EQ(d.firstDivergentTick, static_cast<TickIndex>(300));
    LC_CHECK_EQ(d.expectedHash, 0x1003ull);
    LC_CHECK_EQ(d.actualHash, 0x1003ull ^ 0x40ull);
    // Four pairs examined: indices 0,1,2 matched and index 3 broke.
    LC_CHECK_EQ(d.comparedCount, static_cast<u64>(4));

    // Comparison is directional in reporting but symmetric in detection.
    const Divergence rev = CompareCheckpoints(actual, expected);
    LC_CHECK(rev.diverged);
    LC_CHECK_EQ(rev.firstDivergentTick, static_cast<TickIndex>(300));
    LC_CHECK_EQ(rev.expectedHash, 0x1003ull ^ 0x40ull);
    LC_CHECK_EQ(rev.actualHash, 0x1003ull);

    // A divergence in the very first checkpoint is reported at that tick, with one pair
    // compared - the boundary case the loop above would hide.
    InputLog firstBad = MakeCheckpointLog(77, 8);
    InputLog other;
    other.SetWorldSeed(77);
    other.RecordCheckpoint(0, 0xBADull);
    for (std::size_t i = 1; i < 8; ++i) {
        other.RecordCheckpoint(static_cast<TickIndex>(i * 100u), 0x1000ull + static_cast<u64>(i));
    }
    const Divergence d0 = CompareCheckpoints(firstBad, other);
    LC_CHECK(d0.diverged);
    LC_CHECK_EQ(d0.firstDivergentTick, static_cast<TickIndex>(0));
    LC_CHECK_EQ(d0.comparedCount, static_cast<u64>(1));
}

LC_TEST(replay_compare_flags_a_short_log) {
    const InputLog expected = MakeCheckpointLog(77, 8);
    const InputLog actual   = MakeCheckpointLog(77, 5);   // the run died after tick 400

    const Divergence d = CompareCheckpoints(expected, actual);
    LC_CHECK(d.diverged);
    LC_CHECK_EQ(std::string(DivergenceReasonName(d.reason)), std::string("actual-ended-early"));
    // The tick of the first checkpoint that has no counterpart: index 5 => tick 500.
    LC_CHECK_EQ(d.firstDivergentTick, static_cast<TickIndex>(500));
    LC_CHECK_EQ(d.expectedHash, 0x1005ull);
    LC_CHECK_EQ(d.actualHash, static_cast<u64>(0));
    LC_CHECK_EQ(d.comparedCount, static_cast<u64>(5));

    // The mirror case is reported distinctly, so an operator can tell which side stopped.
    const Divergence rev = CompareCheckpoints(actual, expected);
    LC_CHECK(rev.diverged);
    LC_CHECK_EQ(std::string(DivergenceReasonName(rev.reason)),
                std::string("expected-ended-early"));
    LC_CHECK_EQ(rev.firstDivergentTick, static_cast<TickIndex>(500));

    // A hash divergence inside the common prefix outranks the truncation: the first
    // divergent tick is the diagnostic, and it happened before the log ended.
    InputLog broken;
    broken.SetWorldSeed(77);
    for (std::size_t i = 0; i < 5; ++i) {
        u64 hash = 0x1000ull + static_cast<u64>(i);
        if (i == 2) hash = 0xBADBADull;
        broken.RecordCheckpoint(static_cast<TickIndex>(i * 100u), hash);
    }
    const Divergence dh = CompareCheckpoints(expected, broken);
    LC_CHECK(dh.diverged);
    LC_CHECK_EQ(std::string(DivergenceReasonName(dh.reason)), std::string("hash-mismatch"));
    LC_CHECK_EQ(dh.firstDivergentTick, static_cast<TickIndex>(200));
}

LC_TEST(replay_compare_flags_a_seed_mismatch) {
    const InputLog expected = MakeCheckpointLog(77, 8);
    const InputLog actual   = MakeCheckpointLog(78, 8);   // identical hashes, wrong seed

    const Divergence d = CompareCheckpoints(expected, actual);
    LC_CHECK(d.diverged);
    LC_CHECK_EQ(std::string(DivergenceReasonName(d.reason)), std::string("seed-mismatch"));
    // On a seed mismatch the hash fields carry the seeds - nothing was compared.
    LC_CHECK_EQ(d.expectedHash, static_cast<u64>(77));
    LC_CHECK_EQ(d.actualHash, static_cast<u64>(78));
    LC_CHECK_EQ(d.comparedCount, static_cast<u64>(0));
}

LC_TEST(replay_compare_flags_checkpoints_taken_on_different_ticks) {
    const InputLog expected = MakeCheckpointLog(77, 4);
    InputLog actual;
    actual.SetWorldSeed(77);
    actual.RecordCheckpoint(0,   0x1000ull);
    actual.RecordCheckpoint(100, 0x1001ull);
    actual.RecordCheckpoint(250, 0x1002ull);   // same hash, wrong tick: still a divergence
    actual.RecordCheckpoint(300, 0x1003ull);

    const Divergence d = CompareCheckpoints(expected, actual);
    LC_CHECK(d.diverged);
    LC_CHECK_EQ(std::string(DivergenceReasonName(d.reason)), std::string("tick-mismatch"));
    LC_CHECK_EQ(d.firstDivergentTick, static_cast<TickIndex>(200));
    LC_CHECK_EQ(d.comparedCount, static_cast<u64>(3));
}

// =====================================================================================
// I5: the recording path must not allocate
// =====================================================================================

LC_TEST(replay_record_path_allocates_nothing_after_reserve) {
    if (!AllocTrackingEnabled()) return;   // only meaningful in the instrumented build

    InputLog log;
    log.Reserve(4096, 256);
    std::vector<Command> scratch;
    scratch.reserve(64);
    const Command cmd = MakeCommand(CommandType::MovePlaceholder, 1u, 2, 3, 0);

    bool clean = false;
    {
        NoAllocScope scope("InputLog record path");
        for (u32 i = 0; i < 1000; ++i) {
            log.RecordCommand(static_cast<TickIndex>(i / 4u), cmd);
        }
        for (u32 i = 0; i < 256; ++i) {
            log.RecordCheckpoint(static_cast<TickIndex>(i), static_cast<u64>(i) * 31ull);
        }
        for (u32 i = 0; i < 200; ++i) {
            log.CommandsForTick(static_cast<TickIndex>(i), scratch);
        }
        clean = scope.Clean();
    }
    LC_CHECK(clean);
    LC_CHECK_EQ(static_cast<u64>(log.Commands().size()), static_cast<u64>(1000));
}

// =====================================================================================
// Telemetry
// =====================================================================================

LC_TEST(telemetry_p99_over_a_known_distribution) {
    Telemetry t;
    LC_CHECK_EQ(t.P99TickNs(), static_cast<u64>(0));
    LC_CHECK_EQ(t.LastTickTotalNs(), static_cast<u64>(0));

    // Totals 1000, 2000, ... 100000 ns, fed in ascending order.
    for (u64 i = 1; i <= 100; ++i) {
        t.BeginTick(i);
        t.RecordSystem(SimSystem::Agents, i * 1000ull);
        t.EndTick();
    }
    LC_CHECK_EQ(static_cast<u64>(t.SampleCount()), static_cast<u64>(100));
    LC_CHECK_EQ(t.LastTickTotalNs(), static_cast<u64>(100000));
    // Nearest rank: ceil(0.99 * 100) = 99, so the 99th smallest = 99000 ns.
    LC_CHECK_EQ(t.P99TickNs(), static_cast<u64>(99000));
    // Mean of 1000..100000 = 50500.
    LC_CHECK_EQ(t.MeanSystemNs(SimSystem::Agents), static_cast<u64>(50500));
    LC_CHECK_EQ(t.MeanSystemNs(SimSystem::Law), static_cast<u64>(0));

    // Insertion order must not matter: the same multiset descending gives the same p99.
    Telemetry desc;
    for (u64 i = 100; i >= 1; --i) {
        desc.BeginTick(101ull - i);
        desc.RecordSystem(SimSystem::Agents, i * 1000ull);
        desc.EndTick();
    }
    LC_CHECK_EQ(desc.P99TickNs(), static_cast<u64>(99000));

    // A single sample is its own p99 - the degenerate case the rank arithmetic must survive.
    Telemetry one;
    one.BeginTick(1);
    one.RecordSystem(SimSystem::Economy, 12345ull);
    one.EndTick();
    LC_CHECK_EQ(one.P99TickNs(), static_cast<u64>(12345));
}

LC_TEST(telemetry_p99_only_sees_the_ring) {
    Telemetry t;
    // 300 ticks into a 256-slot ring: the first 44 must fall out entirely.
    for (u64 i = 0; i < 300; ++i) {
        t.BeginTick(i + 1);
        t.RecordSystem(SimSystem::Transport, (i + 1) * 1000ull);
        t.EndTick();
    }
    LC_CHECK_EQ(static_cast<u64>(t.SampleCount()),
                static_cast<u64>(Telemetry::kRingCapacity));
    LC_CHECK_EQ(t.TotalTicksRecorded(), static_cast<u64>(300));
    LC_CHECK_EQ(t.LastTick(), static_cast<TickIndex>(300));
    LC_CHECK_EQ(t.LastTickTotalNs(), static_cast<u64>(300000));

    // Ring holds totals 45000..300000. n = 256, rank = ceil(0.99*256) = 254,
    // so index 253 => 45000 + 253*1000 = 298000.
    LC_CHECK_EQ(t.P99TickNs(), static_cast<u64>(298000));
    // Mean of 45000..300000 = (45000 + 300000) / 2 = 172500.
    LC_CHECK_EQ(t.MeanSystemNs(SimSystem::Transport), static_cast<u64>(172500));
}

LC_TEST(telemetry_ring_wraps_exactly_at_capacity) {
    // The existing p99 tests run 100 and 300 ticks - both comfortably clear of the wrap, so
    // neither can catch an off-by-one AT it. The interesting counts are capacity-1 (full but
    // not yet wrapped), capacity (the last tick that fits), and capacity+1 (the first tick
    // that evicts). A `<=` for a `<` in the sample-count update, or a stale head index, shows
    // up here and nowhere else.
    const std::size_t cap = Telemetry::kRingCapacity;
    const std::size_t counts[] = { cap - 1u, cap, cap + 1u, cap + 2u };

    for (std::size_t total : counts) {
        Telemetry t;
        for (std::size_t i = 0; i < total; ++i) {
            t.BeginTick(static_cast<TickIndex>(i + 1u));
            t.RecordSystem(SimSystem::World, static_cast<u64>(i + 1u) * 1000ull);
            t.EndTick();
        }

        // Sample count saturates at capacity and never exceeds it - P99TickNs copies this
        // many entries into a fixed stack array, so an overshoot is memory corruption.
        const std::size_t want = total < cap ? total : cap;
        LC_CHECK_EQ(static_cast<u64>(t.SampleCount()), static_cast<u64>(want));
        LC_CHECK(t.SampleCount() <= Telemetry::kRingCapacity);
        LC_CHECK_EQ(t.TotalTicksRecorded(), static_cast<u64>(total));
        LC_CHECK_EQ(t.LastTick(), static_cast<TickIndex>(total));

        // The newest tick must survive the wrap: this is the value the overlay shows.
        LC_CHECK_EQ(t.LastTickTotalNs(), static_cast<u64>(total) * 1000ull);

        // The oldest surviving sample is total-want+1; the mean pins the whole window, so an
        // eviction that dropped the wrong slot moves it.
        const u64 oldest = static_cast<u64>(total - want) + 1ull;
        const u64 newest = static_cast<u64>(total);
        LC_CHECK_EQ(t.MeanSystemNs(SimSystem::World), (oldest + newest) * 1000ull / 2ull);
    }

    // One tick of history is the degenerate wrap case: head has advanced but nothing evicted.
    Telemetry one;
    one.BeginTick(0);                         // tick 0 is a real tick, not "no tick"
    one.RecordSystem(SimSystem::Events, 42);
    one.EndTick();
    LC_CHECK_EQ(static_cast<u64>(one.SampleCount()), static_cast<u64>(1));
    LC_CHECK_EQ(one.TotalTicksRecorded(), static_cast<u64>(1));
    LC_CHECK_EQ(one.LastTick(), static_cast<TickIndex>(0));
    LC_CHECK_EQ(one.LastTickTotalNs(), static_cast<u64>(42));
    LC_CHECK_EQ(one.P99TickNs(), static_cast<u64>(42));
}

LC_TEST(telemetry_counts_budget_overruns_per_system) {
    Telemetry t;
    // Defaults come straight from CLAUDE.md section 4.
    LC_CHECK_EQ(t.BudgetNs(SimSystem::Agents), static_cast<u64>(3500000));
    LC_CHECK_EQ(t.BudgetNs(SimSystem::Law), static_cast<u64>(800000));
    LC_CHECK_EQ(DefaultBudgetNs(SimSystem::Transport), static_cast<u64>(2000000));

    t.SetBudgetNs(SimSystem::Economy, 1000);

    const u64 samples[] = {500, 1500, 2000, 1000};
    for (u64 i = 0; i < 4; ++i) {
        t.BeginTick(i + 1);
        t.RecordSystem(SimSystem::Economy, samples[i]);
        t.EndTick();
    }
    // Strictly greater than the budget counts: 1500 and 2000. Exactly 1000 does not.
    LC_CHECK_EQ(t.BudgetOverrunCount(SimSystem::Economy), static_cast<u32>(2));
    LC_CHECK_EQ(t.BudgetOverrunCount(SimSystem::Agents), static_cast<u32>(0));

    // Overrun is judged on the whole tick's accumulated time for a system, not per call:
    // two 600 ns slices in one tick are 1200 ns and overrun a 1000 ns budget.
    t.BeginTick(5);
    t.RecordSystem(SimSystem::Economy, 600);
    t.RecordSystem(SimSystem::Economy, 600);
    t.EndTick();
    LC_CHECK_EQ(t.BudgetOverrunCount(SimSystem::Economy), static_cast<u32>(3));
    LC_CHECK_EQ(t.LastTickTotalNs(), static_cast<u64>(1200));

    // Overruns accumulate beyond the ring: they are a run-long count, not a windowed one.
    for (u64 i = 0; i < 400; ++i) {
        t.BeginTick(100 + i);
        t.RecordSystem(SimSystem::Economy, 9999);
        t.EndTick();
    }
    LC_CHECK_EQ(t.BudgetOverrunCount(SimSystem::Economy), static_cast<u32>(403));
}

LC_TEST(telemetry_csv_header_and_row_line_up) {
    Telemetry t;

    std::string header;
    t.WriteCsvHeader(header);
    LC_CHECK_EQ(header,
                std::string("tick,total_ns,agents_ns,transport_ns,economy_ns,law_ns,"
                            "world_ns,events_ns,other_ns,p99_ns\n"));

    // With nothing measured there is no row to write - a row of zeros would be a lie.
    std::string empty;
    t.AppendCsvRow(empty);
    LC_CHECK_EQ(empty, std::string());

    t.BeginTick(42);
    t.RecordSystem(SimSystem::Agents,    10);
    t.RecordSystem(SimSystem::Transport, 20);
    t.RecordSystem(SimSystem::Economy,   30);
    t.RecordSystem(SimSystem::Law,       40);
    t.RecordSystem(SimSystem::World,     50);
    t.RecordSystem(SimSystem::Events,    60);
    t.RecordSystem(SimSystem::Other,     70);
    t.EndTick();

    std::string row;
    t.AppendCsvRow(row);
    // tick, total, the seven systems in enum order, then p99 (= 280 with one sample).
    LC_CHECK_EQ(row, std::string("42,280,10,20,30,40,50,60,70,280\n"));
    LC_CHECK_EQ(static_cast<u64>(CountChar(row, ',')),
                static_cast<u64>(CountChar(header, ',')));
    LC_CHECK_EQ(static_cast<u8>(row[row.size() - 1]), static_cast<u8>('\n'));

    // AppendCsvRow appends; it must not clear what is already in the buffer.
    std::string doc = header;
    t.AppendCsvRow(doc);
    t.BeginTick(43);
    t.RecordSystem(SimSystem::Agents, 5);
    t.EndTick();
    t.AppendCsvRow(doc);
    LC_CHECK_EQ(static_cast<u64>(CountChar(doc, '\n')), static_cast<u64>(3));
    LC_CHECK_EQ(doc.compare(0, header.size(), header), 0);
}

LC_TEST(telemetry_reset_clears_measurements_but_keeps_budgets) {
    Telemetry t;
    t.SetBudgetNs(SimSystem::World, 123);
    for (u64 i = 0; i < 10; ++i) {
        t.BeginTick(i + 1);
        t.RecordSystem(SimSystem::World, 5000);
        t.EndTick();
    }
    LC_CHECK_EQ(t.BudgetOverrunCount(SimSystem::World), static_cast<u32>(10));

    t.Reset();
    LC_CHECK_EQ(static_cast<u64>(t.SampleCount()), static_cast<u64>(0));
    LC_CHECK_EQ(t.TotalTicksRecorded(), static_cast<u64>(0));
    LC_CHECK_EQ(t.LastTickTotalNs(), static_cast<u64>(0));
    LC_CHECK_EQ(t.P99TickNs(), static_cast<u64>(0));
    LC_CHECK_EQ(t.MeanSystemNs(SimSystem::World), static_cast<u64>(0));
    LC_CHECK_EQ(t.BudgetOverrunCount(SimSystem::World), static_cast<u32>(0));
    // Budgets are configuration, not measurement: they survive.
    LC_CHECK_EQ(t.BudgetNs(SimSystem::World), static_cast<u64>(123));

    std::string row;
    t.AppendCsvRow(row);
    LC_CHECK_EQ(row, std::string());
}

LC_TEST(telemetry_record_path_allocates_nothing) {
    if (!AllocTrackingEnabled()) return;

    Telemetry t;   // the ring is allocated here, deliberately outside the guarded region

    // Warm up past one full wrap so nothing lazy is left to happen inside the scope.
    for (u64 i = 0; i < 8; ++i) {
        t.BeginTick(i);
        t.RecordSystem(SimSystem::Agents, i);
        t.EndTick();
    }

    bool clean = false;
    u64 p99 = 0;
    {
        NoAllocScope scope("Telemetry record path");
        for (u64 tick = 0; tick < 600; ++tick) {
            t.BeginTick(tick);
            for (std::size_t s = 0; s < kSimSystemCount; ++s) {
                t.RecordSystem(static_cast<SimSystem>(s), tick * 7ull + static_cast<u64>(s));
            }
            t.EndTick();
        }
        // The queries must be allocation-free too: the overlay calls them every frame.
        p99 = t.P99TickNs();
        (void)t.MeanSystemNs(SimSystem::Agents);
        (void)t.LastTickTotalNs();
        t.Reset();
        clean = scope.Clean();
    }
    LC_CHECK(clean);
    LC_CHECK(p99 > 0);
}
