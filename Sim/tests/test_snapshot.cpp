// LIVING CITY — invariant tests for ARCHITECTURE.md Boundary 1.
//
// What is actually under test here is not "does the getter return the value". It is:
//   * a Command's byte layout and hash are pinned, because both are part of the replay log
//     format that I1 depends on;
//   * the SPSC command ring is FIFO, rejects rather than corrupts when full, and stays correct
//     across the capacity boundary indefinitely;
//   * the snapshot ring never hands the reader a torn or half-written snapshot, including
//     under real concurrent load;
//   * neither structure allocates in steady state (I5).
#include "livingcity/sim/Commands.h"
#include "livingcity/sim/Snapshot.h"

#include "TestFramework.h"

#include <atomic>
#include <cstring>
#include <thread>

using namespace lc;

namespace {

// ---------------------------------------------------------------- fixtures
// A deterministic, non-degenerate command. Every field varies with i so a field that gets
// dropped, swapped, or truncated shows up in the hash and in the FIFO checks.
Command MakeCommand(u32 i) {
    Command c;
    c.type         = static_cast<CommandType>(i % 7u);
    c.arg0         = i * 17u;
    c.arg1         = -static_cast<i64>(i) * 1000;
    c.arg2         = static_cast<i64>(i) * static_cast<i64>(i);
    c.issuedAtTick = static_cast<TickIndex>(100u + i);
    return c;
}

bool SameCommand(const Command& a, const Command& b) {
    return a.type == b.type && a.pad[0] == b.pad[0] && a.pad[1] == b.pad[1] &&
           a.pad[2] == b.pad[2] && a.arg0 == b.arg0 && a.arg1 == b.arg1 && a.arg2 == b.arg2 &&
           a.issuedAtTick == b.issuedAtTick;
}

// worldHash is a PURE FUNCTION OF tick on purpose. That is what makes tearing detectable: if
// the two halves of an acquired snapshot came from different writes, this identity breaks and
// the reader can prove it. Every other field is derived from tick for the same reason.
u64 SnapshotHashForTick(TickIndex tick) {
    return tick * 2654435761ull;
}

void FillSnapshot(Snapshot& s, TickIndex tick) {
    // BeginWrite() hands back a stale buffer, so the writer must set EVERY field.
    s.tick         = tick;
    s.worldHash    = SnapshotHashForTick(tick);
    s.placeholderX = static_cast<i64>(tick) * 3;
    s.placeholderY = -static_cast<i64>(tick);
    s.placeholderZ = static_cast<i64>(tick) * 7;
    s.agentCount   = static_cast<u32>(tick & 0xFFFFu);
    s.paused       = static_cast<u8>((tick & 1u) != 0u ? 1 : 0);
    s.pad[0]       = 0;
    s.pad[1]       = 0;
    s.pad[2]       = 0;
}

// CAREFUL: every field is derived from tick, so the ALL-ZERO snapshot is self-consistent —
// tick 0 maps to hash 0, position 0, agentCount 0, paused false. That is unavoidable for a
// tick-derived fixture, and it means this predicate alone cannot tell a genuine snapshot from
// a slot the writer has never touched. Callers that need that distinction must also bound the
// tick against the range the writer actually published; the threaded test below does.
bool SnapshotSelfConsistent(const Snapshot& s) {
    if (s.worldHash != SnapshotHashForTick(s.tick)) return false;
    if (s.placeholderX != static_cast<i64>(s.tick) * 3) return false;
    if (s.placeholderY != -static_cast<i64>(s.tick)) return false;
    if (s.placeholderZ != static_cast<i64>(s.tick) * 7) return false;
    if (s.agentCount != static_cast<u32>(s.tick & 0xFFFFu)) return false;
    if (s.paused != static_cast<u8>((s.tick & 1u) != 0u ? 1 : 0)) return false;
    return true;
}

// A value the writer never publishes, used to prove that a FAILED acquire does not scribble on
// the caller's buffers. Deliberately NOT self-consistent, so a partial write of a real snapshot
// over the top of it is detectable even if only some fields land.
Snapshot SentinelSnapshot() {
    Snapshot s;
    s.tick         = 0xFEEDFACEu;
    s.worldHash    = 0x0123456789ABCDEFull;
    s.placeholderX = -1;
    s.placeholderY = -2;
    s.placeholderZ = -3;
    s.agentCount   = 0xABCDu;
    s.paused       = 1u;
    return s;
}

// Byte-image comparison. This is only a legitimate thing to do because Snapshot spells its
// padding out as an always-zero member, so sizeof == sum-of-members and there are no
// indeterminate bytes; memcmp over a struct with compiler padding would be a real bug.
bool SameSnapshotBytes(const Snapshot& a, const Snapshot& b) {
    return std::memcmp(&a, &b, sizeof(Snapshot)) == 0;
}

// Counting log sink, so the overflow test can assert that Push() actually warns without
// spraying 8 lines of WARN across the test output.
u32 g_warnCount = 0;

void CountingSink(LogLevel level, const char* /*msg*/) {
    if (level == LogLevel::Warn) ++g_warnCount;
}

} // namespace

// ---------------------------------------------------------------- Command: layout and hash

// Compile-time half of the contract. A Command is handed across a thread boundary by value and
// memcpy'd into the binary replay log; if it ever stops being trivially copyable, that is a
// silent correctness break, so it fails the build instead.
static_assert(std::is_trivially_copyable_v<Command>, "Command must be trivially copyable");
static_assert(std::is_trivially_copyable_v<Snapshot>, "Snapshot must be trivially copyable");
static_assert(std::is_standard_layout_v<Command>, "Command must be standard layout");
static_assert(std::is_standard_layout_v<Snapshot>, "Snapshot must be standard layout");

LC_TEST(command_layout_is_pinned_for_the_replay_log) {
    // These numbers are the on-disk format of the input log (M0.6). If a field is added,
    // reordered or resized, this test fails and the log version must be bumped — a silent
    // layout change would make every recorded run replay as a different world.
    LC_CHECK_EQ(static_cast<u64>(sizeof(Command)), static_cast<u64>(32));
    LC_CHECK_EQ(static_cast<u64>(alignof(Command)), static_cast<u64>(8));
    LC_CHECK_EQ(static_cast<u64>(offsetof(Command, arg0)), static_cast<u64>(4));
    LC_CHECK_EQ(static_cast<u64>(offsetof(Command, arg1)), static_cast<u64>(8));
    LC_CHECK_EQ(static_cast<u64>(offsetof(Command, arg2)), static_cast<u64>(16));
    LC_CHECK_EQ(static_cast<u64>(offsetof(Command, issuedAtTick)), static_cast<u64>(24));

    // sizeof == sum of the members means there is no compiler-inserted padding anywhere: the
    // byte image of a Command is fully determined by its field values (I1).
    const u64 memberBytes = sizeof(CommandType) + sizeof(u8) * 3 + sizeof(u32) + sizeof(i64) * 2 +
                            sizeof(TickIndex);
    LC_CHECK_EQ(static_cast<u64>(sizeof(Command)), memberBytes);

    // A default-constructed Command is fully defined, not stack garbage.
    Command c;
    LC_CHECK(c.type == CommandType::None);
    LC_CHECK_EQ(c.arg0, 0u);
    LC_CHECK_EQ(c.arg1, static_cast<i64>(0));
    LC_CHECK_EQ(c.arg2, static_cast<i64>(0));
    LC_CHECK_EQ(c.issuedAtTick, static_cast<TickIndex>(0));
    LC_CHECK_EQ(c.pad[0], static_cast<u8>(0));
    LC_CHECK_EQ(c.pad[1], static_cast<u8>(0));
    LC_CHECK_EQ(c.pad[2], static_cast<u8>(0));

    // Enum values are part of the log format too.
    LC_CHECK_EQ(static_cast<u8>(CommandType::None), static_cast<u8>(0));
    LC_CHECK_EQ(static_cast<u8>(CommandType::SetPaused), static_cast<u8>(1));
    LC_CHECK_EQ(static_cast<u8>(CommandType::StepOnce), static_cast<u8>(2));
    LC_CHECK_EQ(static_cast<u8>(CommandType::SetTimeScale), static_cast<u8>(3));
    LC_CHECK_EQ(static_cast<u8>(CommandType::MovePlaceholder), static_cast<u8>(4));
    LC_CHECK_EQ(static_cast<u8>(CommandType::Interact), static_cast<u8>(5));
    LC_CHECK_EQ(static_cast<u8>(CommandType::Quit), static_cast<u8>(6));
}

LC_TEST(command_hash_is_a_known_answer_and_order_sensitive) {
    // KNOWN ANSWER. Hard-coded from this implementation's actual output so that any future
    // change to HashInto, to the field order, or to a field's width is caught here rather than
    // three phases later as a determinism divergence in a 100k-tick replay.
    Hasher h;
    for (u32 i = 0; i < 8u; ++i) {
        const Command c = MakeCommand(i);
        HashInto(h, c);
    }
    LC_CHECK_EQ(h.Value(), 0xD5A7ADDC309EE0AAull);

    // A default (all-zero) command still contributes; it is not a no-op.
    Hasher hDefault;
    const Command none;
    HashInto(hDefault, none);
    LC_CHECK_EQ(hDefault.Value(), 0x171BB79DC8E18503ull);
    LC_CHECK_NE(hDefault.Value(), Hasher::kOffsetBasis);

    // Order sensitivity: commands arriving in a different order must produce a different
    // world. If this ever collides, the replay harness cannot see reordered input.
    Hasher forward;
    HashInto(forward, MakeCommand(1));
    HashInto(forward, MakeCommand(2));
    Hasher backward;
    HashInto(backward, MakeCommand(2));
    HashInto(backward, MakeCommand(1));
    LC_CHECK_EQ(backward.Value(), 0x9406BF1262B29031ull);
    LC_CHECK_NE(forward.Value(), backward.Value());

    // Every field must reach the hash. Flip one at a time and demand a different digest.
    const Command base = MakeCommand(5);
    Hasher baseHash;
    HashInto(baseHash, base);

    Command variants[5] = {base, base, base, base, base};
    variants[0].type         = CommandType::Quit;
    variants[1].arg0         = base.arg0 + 1u;
    variants[2].arg1         = base.arg1 + 1;
    variants[3].arg2         = base.arg2 + 1;
    variants[4].issuedAtTick = base.issuedAtTick + 1u;
    for (const Command& v : variants) {
        Hasher vh;
        HashInto(vh, v);
        LC_CHECK_NE(vh.Value(), baseHash.Value());
    }

    // pad is hashed deliberately: it must always be zero, and hashing it turns stray garbage
    // in those bytes into a loud divergence instead of a silent one.
    Command dirty = base;
    dirty.pad[1]  = 0xABu;
    Hasher dirtyHash;
    HashInto(dirtyHash, dirty);
    LC_CHECK_NE(dirtyHash.Value(), baseHash.Value());
}

// ---------------------------------------------------------------- CommandQueue

LC_TEST(command_queue_preserves_fifo_order) {
    CommandQueue q;
    LC_CHECK(q.Empty());
    LC_CHECK_EQ(q.Count(), 0u);

    Command drained;
    LC_CHECK_FALSE(q.Pop(drained));  // popping an empty queue is a clean false, not a fault

    const u32 kBatch = 64u;
    for (u32 i = 0; i < kBatch; ++i) {
        LC_CHECK(q.Push(MakeCommand(i)));
    }
    LC_CHECK_EQ(q.Count(), kBatch);

    // Drained order must be push order, and hashing the drained sequence must reproduce the
    // known answer from the hash test — i.e. the queue is a pure FIFO with no reordering,
    // duplication or loss. This is what makes the replay log replayable.
    Hasher h;
    u32 mismatches = 0;
    for (u32 i = 0; i < kBatch; ++i) {
        Command got;
        if (!q.Pop(got)) {
            ++mismatches;
            break;
        }
        if (!SameCommand(got, MakeCommand(i))) ++mismatches;
        if (i < 8u) HashInto(h, got);
    }
    LC_CHECK_EQ(mismatches, 0u);
    LC_CHECK_EQ(h.Value(), 0xD5A7ADDC309EE0AAull);
    LC_CHECK(q.Empty());
    LC_CHECK_FALSE(q.Pop(drained));
}

LC_TEST(command_queue_full_rejects_rather_than_corrupting) {
    CommandQueue q;
    const u32 cap = CommandQueue::kCapacity;

    // The boundary itself: one short of capacity is NOT full and still accepts. An off-by-one
    // in the "is there room" test hides here — `>` instead of `>=` lets a 257th command in and
    // overwrites the oldest, and a test that only ever checks the full state would miss the
    // other direction, where the queue refuses its own last legal slot.
    for (u32 i = 0; i + 1u < cap; ++i) {
        LC_CHECK(q.Push(MakeCommand(i)));
    }
    LC_CHECK_EQ(q.Count(), cap - 1u);
    LC_CHECK_FALSE(q.Full());
    LC_CHECK(q.Push(MakeCommand(cap - 1u)));  // exactly fills it

    LC_CHECK(q.Full());
    LC_CHECK_EQ(q.Count(), cap);
    LC_CHECK_EQ(q.RejectedPushCount(), static_cast<u64>(0));

    // Overflow must REFUSE, never overwrite: overwriting the oldest unread command would
    // silently change the input log and therefore the world (I1). And it must be audible —
    // a full command queue that only manifests as a keypress doing nothing is unfindable.
    g_warnCount = 0;
    SetLogSink(&CountingSink);
    const u32 kRejects = 8u;
    u32 acceptedWhileFull = 0;
    for (u32 i = 0; i < kRejects; ++i) {
        if (q.Push(MakeCommand(90000u + i))) ++acceptedWhileFull;
    }

    LC_CHECK_EQ(acceptedWhileFull, 0u);
    LC_CHECK_EQ(g_warnCount, kRejects);  // a full queue is loud, never silent
    LC_CHECK_EQ(q.RejectedPushCount(), static_cast<u64>(kRejects));
    LC_CHECK_EQ(q.Count(), cap);  // rejection did not disturb the cursors

    // Freeing exactly one slot must make room for exactly one command, and no more. This walks
    // the ring across the capacity boundary with the cursors offset from each other, which is
    // where a mask or comparison that is right at head==tail==0 tends to come apart. Still
    // inside the counting sink, so the one extra rejection stays out of the test output.
    const u32 kBoundaryCommand = 12345u;
    {
        Command popped;
        LC_CHECK(q.Pop(popped));
        LC_CHECK(SameCommand(popped, MakeCommand(0)));  // FIFO: the oldest came out
        LC_CHECK_FALSE(q.Full());
        LC_CHECK_EQ(q.Count(), cap - 1u);
        LC_CHECK(q.Push(MakeCommand(kBoundaryCommand)));
        LC_CHECK(q.Full());
        LC_CHECK_EQ(q.Count(), cap);
        LC_CHECK_FALSE(q.Push(MakeCommand(12346u)));  // and no further
        LC_CHECK_EQ(q.RejectedPushCount(), static_cast<u64>(kRejects) + 1u);
    }
    SetLogSink(nullptr);
    LC_CHECK_EQ(g_warnCount, kRejects + 1u);

    // Every command pushed before the overflow is still present, in order, byte-identical.
    // (Index 0 was popped above, so the remaining window is 1..cap-1, then the boundary push.)
    u32 corrupted = 0;
    for (u32 i = 1; i < cap; ++i) {
        Command got;
        if (!q.Pop(got) || !SameCommand(got, MakeCommand(i))) ++corrupted;
    }
    LC_CHECK_EQ(corrupted, 0u);

    // ...and the command that went in at the boundary comes out last, not in place of the one
    // it followed. That is the assertion an overwrite-on-full ring would fail.
    Command boundary;
    LC_CHECK(q.Pop(boundary));
    LC_CHECK(SameCommand(boundary, MakeCommand(kBoundaryCommand)));
    LC_CHECK(q.Empty());

    // A drained queue is usable again — the full condition is not sticky.
    LC_CHECK(q.Push(MakeCommand(7)));
    LC_CHECK_EQ(q.Count(), 1u);

    // Clear() discards the remainder and leaves the queue usable.
    q.Clear();
    LC_CHECK(q.Empty());
    LC_CHECK_EQ(q.Count(), 0u);
    LC_CHECK(q.Push(MakeCommand(11)));
    Command afterClear;
    LC_CHECK(q.Pop(afterClear));
    LC_CHECK(SameCommand(afterClear, MakeCommand(11)));
}

LC_TEST(command_queue_survives_many_wraps_across_the_capacity_boundary) {
    CommandQueue q;

    // Hold the ring half full throughout, so the producer cursor and the consumer cursor cross
    // the capacity boundary at different moments — the case a naive "empty means head==tail"
    // ring gets wrong.
    const u32 kInFlight   = CommandQueue::kCapacity / 2u;
    const u32 kIterations = 200000u;  // ~780 wraps of a 256-slot ring

    for (u32 i = 0; i < kInFlight; ++i) {
        LC_CHECK(q.Push(MakeCommand(i)));
    }

    // Counters, not per-iteration LC_CHECKs: one broken iteration would otherwise emit 200k
    // failure lines, and the failure count is the useful signal anyway.
    u32 pushFailures = 0;
    u32 popFailures  = 0;
    u32 outOfOrder   = 0;
    u32 expected     = 0;

    for (u32 i = kInFlight; i < kIterations; ++i) {
        if (!q.Push(MakeCommand(i))) ++pushFailures;

        Command got;
        if (!q.Pop(got)) {
            ++popFailures;
        } else {
            if (!SameCommand(got, MakeCommand(expected))) ++outOfOrder;
            ++expected;
        }
        if (q.Count() != kInFlight) ++outOfOrder;
    }

    LC_CHECK_EQ(pushFailures, 0u);
    LC_CHECK_EQ(popFailures, 0u);
    LC_CHECK_EQ(outOfOrder, 0u);
    LC_CHECK_EQ(expected, kIterations - kInFlight);
    LC_CHECK_EQ(q.Count(), kInFlight);
    LC_CHECK_EQ(q.RejectedPushCount(), static_cast<u64>(0));

    // Drain the tail and confirm the last in-flight window is intact and in order.
    u32 tailCorrupt = 0;
    while (true) {
        Command got;
        if (!q.Pop(got)) break;
        if (!SameCommand(got, MakeCommand(expected))) ++tailCorrupt;
        ++expected;
    }
    LC_CHECK_EQ(tailCorrupt, 0u);
    LC_CHECK_EQ(expected, kIterations);
    LC_CHECK(q.Empty());
}

// ---------------------------------------------------------------- Snapshot layout

LC_TEST(snapshot_carries_no_floats_and_has_a_pinned_layout) {
    // R3: the sim -> engine boundary must not carry a float. The renderer converts on its own
    // side. static_assert would be stronger, but this keeps the reason visible in the report.
    LC_CHECK_FALSE(std::is_floating_point_v<decltype(Snapshot::placeholderX)>);
    LC_CHECK_FALSE(std::is_floating_point_v<decltype(Snapshot::placeholderY)>);
    LC_CHECK_FALSE(std::is_floating_point_v<decltype(Snapshot::placeholderZ)>);
    LC_CHECK_FALSE(std::is_floating_point_v<decltype(Snapshot::worldHash)>);

    LC_CHECK_EQ(static_cast<u64>(alignof(Snapshot)), static_cast<u64>(8));

    // No compiler-inserted padding: the byte image is fully determined by the field values.
    // The size is not pinned to a literal any more - the snapshot now carries the citizen
    // arrays, so it grows whenever kMaxSnapshotCitizens changes. What must stay true is that
    // it contains no indeterminate bytes, because the whole struct is copied between threads.
    const u64 memberBytes =
        sizeof(TickIndex) + sizeof(u64) + sizeof(i64) * 3
        + sizeof(u64) + sizeof(i64) * 4
        + sizeof(u32) * 12
        + sizeof(i32) * 6
        + sizeof(u16) * 2
        + sizeof(i32) * 2 * kMaxSnapshotCitizens
        + sizeof(u8) * kMaxSnapshotCitizens
        + sizeof(u8) * 12;
    LC_CHECK_EQ(static_cast<u64>(sizeof(Snapshot)), memberBytes);

    // The citizen arrays must be integers too - the whole point of the boundary.
    LC_CHECK_FALSE(std::is_floating_point_v<std::remove_extent_t<decltype(Snapshot::citizenX)>>);
    LC_CHECK_FALSE(std::is_floating_point_v<std::remove_extent_t<decltype(Snapshot::citizenY)>>);

    Snapshot s;
    LC_CHECK_EQ(s.tick, static_cast<TickIndex>(0));
    LC_CHECK_EQ(s.worldHash, static_cast<u64>(0));
    LC_CHECK_EQ(s.agentCount, 0u);
    LC_CHECK_EQ(s.citizenCount, 0u);
    LC_CHECK_EQ(s.paused, static_cast<u8>(0));
}

// ---------------------------------------------------------------- SnapshotRing

LC_TEST(snapshot_ring_has_nothing_to_show_until_two_are_published) {
    SnapshotRing ring;
    Snapshot prev;
    Snapshot curr;
    Snapshot latest;

    LC_CHECK_EQ(ring.PublishedCount(), static_cast<u64>(0));
    LC_CHECK_FALSE(ring.AcquireLatest(latest));
    LC_CHECK_FALSE(ring.AcquireLatestPair(prev, curr));

    // One publish: a latest exists, but interpolation still has nothing to interpolate between.
    FillSnapshot(ring.BeginWrite(), static_cast<TickIndex>(1));
    ring.Publish();
    LC_CHECK_EQ(ring.PublishedCount(), static_cast<u64>(1));
    LC_CHECK(ring.AcquireLatest(latest));
    LC_CHECK_EQ(latest.tick, static_cast<TickIndex>(1));
    LC_CHECK_FALSE(ring.AcquireLatestPair(prev, curr));

    // A failed pair acquire must leave the caller's buffers untouched, so the renderer can
    // safely keep interpolating the pair it already holds.
    LC_CHECK_EQ(prev.tick, static_cast<TickIndex>(0));
    LC_CHECK_EQ(curr.tick, static_cast<TickIndex>(0));

    // Second publish: now there is a pair.
    FillSnapshot(ring.BeginWrite(), static_cast<TickIndex>(2));
    ring.Publish();
    LC_CHECK(ring.AcquireLatestPair(prev, curr));
    LC_CHECK_EQ(prev.tick, static_cast<TickIndex>(1));
    LC_CHECK_EQ(curr.tick, static_cast<TickIndex>(2));
}

LC_TEST(snapshot_ring_treats_tick_zero_as_a_real_snapshot) {
    // Tick 0 is a real tick: Simulation publishes a snapshot at construction, before the first
    // Step(), and its tick IS zero. The ring must key "is there anything to show" off the
    // publish counter and nothing else. An implementation that used tick != 0 (or a nonzero
    // worldHash, or a nonzero anything) as its "has been written" marker would leave the
    // renderer with nothing to draw on the very first frame of every session — and every
    // other test in this file publishes from tick 1, so none of them would notice.
    SnapshotRing ring;
    Snapshot     latest;

    FillSnapshot(ring.BeginWrite(), static_cast<TickIndex>(0));
    ring.Publish();

    LC_CHECK_EQ(ring.PublishedCount(), static_cast<u64>(1));
    LC_CHECK(ring.AcquireLatest(latest));
    LC_CHECK_EQ(latest.tick, static_cast<TickIndex>(0));
    LC_CHECK_EQ(latest.worldHash, static_cast<u64>(0));
    LC_CHECK_EQ(latest.placeholderX, static_cast<i64>(0));

    // The all-zero snapshot is indistinguishable from a never-written slot by value, so the
    // proof that this one was really published is that the pair acquire still refuses: one
    // publish is one publish, whatever its contents.
    Snapshot prev;
    Snapshot curr;
    LC_CHECK_FALSE(ring.AcquireLatestPair(prev, curr));

    // Tick 0 then tick 1 must interpolate as a normal consecutive pair.
    FillSnapshot(ring.BeginWrite(), static_cast<TickIndex>(1));
    ring.Publish();
    LC_CHECK(ring.AcquireLatestPair(prev, curr));
    LC_CHECK_EQ(prev.tick, static_cast<TickIndex>(0));
    LC_CHECK_EQ(curr.tick, static_cast<TickIndex>(1));
    LC_CHECK(SnapshotSelfConsistent(prev));
    LC_CHECK(SnapshotSelfConsistent(curr));
}

LC_TEST(snapshot_ring_pair_is_always_the_last_two_published) {
    SnapshotRing ring;
    Snapshot prev;
    Snapshot curr;

    const TickIndex kPublishes = 64;  // 21 full cycles of a 3-slot ring, plus one
    for (TickIndex t = 1; t <= kPublishes; ++t) {
        FillSnapshot(ring.BeginWrite(), t);
        ring.Publish();

        LC_CHECK_EQ(ring.PublishedCount(), t);

        Snapshot latest;
        LC_CHECK(ring.AcquireLatest(latest));
        LC_CHECK_EQ(latest.tick, t);
        LC_CHECK(SnapshotSelfConsistent(latest));

        if (t < 2u) {
            LC_CHECK_FALSE(ring.AcquireLatestPair(prev, curr));
        } else {
            LC_CHECK(ring.AcquireLatestPair(prev, curr));
            LC_CHECK_EQ(curr.tick, t);
            LC_CHECK_EQ(prev.tick, t - 1u);  // prev is exactly the publish before curr
            LC_CHECK(SnapshotSelfConsistent(prev));
            LC_CHECK(SnapshotSelfConsistent(curr));
        }
    }

    // Known answer after 64 publishes. 64 * 2654435761 == 169883888704.
    LC_CHECK_EQ(ring.PublishedCount(), static_cast<u64>(64));
    LC_CHECK_EQ(curr.tick, static_cast<TickIndex>(64));
    LC_CHECK_EQ(prev.tick, static_cast<TickIndex>(63));
    LC_CHECK_EQ(curr.worldHash, 169883888704ull);
    LC_CHECK_EQ(prev.worldHash, 167229452943ull);
    LC_CHECK_EQ(curr.placeholderX, static_cast<i64>(192));
    LC_CHECK_EQ(curr.placeholderY, static_cast<i64>(-64));

    // Acquiring twice with no publish in between is idempotent — the reader is not consuming.
    Snapshot prev2;
    Snapshot curr2;
    LC_CHECK(ring.AcquireLatestPair(prev2, curr2));
    LC_CHECK_EQ(curr2.tick, curr.tick);
    LC_CHECK_EQ(prev2.tick, prev.tick);
    LC_CHECK_EQ(ring.PublishedCount(), static_cast<u64>(64));
}

LC_TEST(snapshot_ring_back_buffer_is_never_what_the_reader_holds) {
    SnapshotRing ring;

    // Identify the three slots by address as the writer cycles through them. Comparing these
    // pointers is object identity in a fixed member array, not a hash of an address — no sim
    // state depends on the values, so R3 is untouched.
    const Snapshot* slotAddr[3] = {nullptr, nullptr, nullptr};
    for (u32 i = 0; i < 3u; ++i) {
        slotAddr[i] = &ring.BeginWrite();
        FillSnapshot(ring.BeginWrite(), static_cast<TickIndex>(i + 1u));
        ring.Publish();
    }
    LC_CHECK(slotAddr[0] != slotAddr[1]);
    LC_CHECK(slotAddr[1] != slotAddr[2]);
    LC_CHECK(slotAddr[0] != slotAddr[2]);

    // BeginWrite() is idempotent before Publish(): it hands back the same object every time.
    LC_CHECK(&ring.BeginWrite() == &ring.BeginWrite());

    // The back buffer is NOT cleared — it still holds the snapshot published three publishes
    // ago. That is a documented hazard, not an accident: the writer MUST assign every field or
    // it leaks stale state to the renderer. Pinning it here means a future "helpful" zeroing of
    // the buffer has to be a deliberate decision that updates the contract, rather than a
    // silent change that turns "the writer forgot a field" from visibly-wrong-old-data into
    // plausible-looking zeroes.
    LC_CHECK_EQ(ring.BeginWrite().tick, static_cast<TickIndex>(1));
    LC_CHECK(SnapshotSelfConsistent(ring.BeginWrite()));

    for (u32 publishes = 3u; publishes < 64u; ++publishes) {
        Snapshot prev;
        Snapshot curr;
        LC_CHECK(ring.AcquireLatestPair(prev, curr));

        const Snapshot* back     = &ring.BeginWrite();
        const Snapshot* currSlot = slotAddr[(publishes - 1u) % 3u];
        const Snapshot* prevSlot = slotAddr[(publishes - 2u) % 3u];

        // The whole point of the third buffer: the slot the writer is about to scribble on is
        // neither of the two the reader is entitled to read.
        LC_CHECK(back != currSlot);
        LC_CHECK(back != prevSlot);
        LC_CHECK(back == slotAddr[publishes % 3u]);

        // ...and what the reader holds really did come from those two slots.
        LC_CHECK_EQ(curr.tick, currSlot->tick);
        LC_CHECK_EQ(prev.tick, prevSlot->tick);

        // Scribbling on the back buffer must be invisible to the reader until Publish().
        FillSnapshot(ring.BeginWrite(), static_cast<TickIndex>(publishes + 1u));
        Snapshot prevMid;
        Snapshot currMid;
        LC_CHECK(ring.AcquireLatestPair(prevMid, currMid));
        LC_CHECK_EQ(currMid.tick, curr.tick);
        LC_CHECK_EQ(prevMid.tick, prev.tick);

        ring.Publish();

        Snapshot prevAfter;
        Snapshot currAfter;
        LC_CHECK(ring.AcquireLatestPair(prevAfter, currAfter));
        LC_CHECK_EQ(currAfter.tick, static_cast<TickIndex>(publishes + 1u));
        LC_CHECK_EQ(prevAfter.tick, curr.tick);
    }
}

LC_TEST(snapshot_ring_and_command_queue_allocate_nothing_in_steady_state) {
    // I5. Only meaningful in the headless/test build, which replaces global operator new;
    // under UBT the tracker is deliberately absent (D-005), so there is nothing to assert.
    if (!AllocTrackingEnabled()) return;

    SnapshotRing ring;
    CommandQueue queue;

    // Warm-up outside the guard: touch every slot and get the queue past its first wrap, so
    // the measured region is genuinely steady state and not first-use.
    for (u32 i = 0; i < 1024u; ++i) {
        FillSnapshot(ring.BeginWrite(), static_cast<TickIndex>(i + 1u));
        ring.Publish();
        Command warm;
        queue.Push(MakeCommand(i));
        queue.Pop(warm);
    }

    u64 sink = 0;
    const NoAllocScope guard("snapshot publish/acquire + command push/pop steady state");
    for (u32 i = 0; i < 50000u; ++i) {
        const TickIndex tick = static_cast<TickIndex>(2000u + i);

        FillSnapshot(ring.BeginWrite(), tick);
        ring.Publish();

        Snapshot prev;
        Snapshot curr;
        if (ring.AcquireLatestPair(prev, curr)) sink += curr.tick + prev.tick;

        Snapshot latest;
        if (ring.AcquireLatest(latest)) sink += latest.worldHash;

        queue.Push(MakeCommand(i));
        Command got;
        if (queue.Pop(got)) sink += static_cast<u64>(got.arg2);
    }

    LC_CHECK_EQ(guard.AllocationsSinceStart(), static_cast<u64>(0));
    LC_CHECK(guard.Clean());
    LC_CHECK_NE(sink, static_cast<u64>(0));  // keep the loop from being optimised away
    LC_CHECK_EQ(queue.RejectedPushCount(), static_cast<u64>(0));
}

LC_TEST(snapshot_ring_threaded_publish_and_acquire_never_tears) {
    // One writer, one reader, no locks. worldHash and every other field are pure functions of
    // tick, so a snapshot assembled from two different writes is provably detectable.
    static constexpr TickIndex kTicks = 20000;

    SnapshotRing ring;

    std::atomic<bool> writerDone{false};
    std::atomic<u32>  tornSingle{0};
    std::atomic<u32>  tornPair{0};
    std::atomic<u32>  nonConsecutive{0};
    std::atomic<u32>  wentBackwards{0};
    std::atomic<u32>  outOfRange{0};
    std::atomic<u32>  clobberedOnFailure{0};
    std::atomic<u64>  failedAcquires{0};
    std::atomic<u64>  observations{0};

    // The worker threads never call LC_CHECK: ReportFailure allocates a std::string and pushes
    // into a shared vector. Findings come back as atomics and are asserted after the join.
    std::thread writer([&]() {
        for (TickIndex t = 1; t <= kTicks; ++t) {
            Snapshot& s = ring.BeginWrite();
            FillSnapshot(s, t);
            ring.Publish();
        }
        writerDone.store(true, std::memory_order_release);
    });

    std::thread reader([&]() {
        const Snapshot sentinel = SentinelSnapshot();
        TickIndex      highWater = 0;
        for (;;) {
            // Read the done flag BEFORE acquiring, so the final iteration is guaranteed to
            // observe the writer's last publish.
            const bool done = writerDone.load(std::memory_order_acquire);

            // Start from the sentinel every iteration. A failed acquire must leave these
            // exactly as they were — that is the contract the renderer relies on when it keeps
            // interpolating the pair it already holds — and under a flat-out writer the pair
            // acquire fails constantly, so this path gets hammered. Without it the "copy to a
            // local, assign only on success" structure is untested: a version that writes
            // straight through the caller's reference and only THEN validates passes every
            // single-threaded test in this file.
            Snapshot latest = sentinel;
            if (ring.AcquireLatest(latest)) {
                // Range check as well as self-consistency: the all-zero snapshot is
                // self-consistent (see SnapshotSelfConsistent), so a never-written slot would
                // otherwise sail through. The writer only ever publishes ticks 1..kTicks.
                if (latest.tick == 0u || latest.tick > kTicks) {
                    outOfRange.fetch_add(1u, std::memory_order_relaxed);
                } else if (!SnapshotSelfConsistent(latest)) {
                    tornSingle.fetch_add(1u, std::memory_order_relaxed);
                } else if (latest.tick < highWater) {
                    wentBackwards.fetch_add(1u, std::memory_order_relaxed);
                } else {
                    highWater = latest.tick;
                }
                observations.fetch_add(1u, std::memory_order_relaxed);
            } else {
                if (!SameSnapshotBytes(latest, sentinel)) {
                    clobberedOnFailure.fetch_add(1u, std::memory_order_relaxed);
                }
                failedAcquires.fetch_add(1u, std::memory_order_relaxed);
            }

            Snapshot prev = sentinel;
            Snapshot curr = sentinel;
            if (ring.AcquireLatestPair(prev, curr)) {
                if (prev.tick == 0u || prev.tick > kTicks || curr.tick == 0u ||
                    curr.tick > kTicks) {
                    outOfRange.fetch_add(1u, std::memory_order_relaxed);
                } else if (!SnapshotSelfConsistent(prev) || !SnapshotSelfConsistent(curr)) {
                    tornPair.fetch_add(1u, std::memory_order_relaxed);
                } else {
                    if (prev.tick + 1u != curr.tick) {
                        nonConsecutive.fetch_add(1u, std::memory_order_relaxed);
                    }
                    if (curr.tick < highWater) {
                        wentBackwards.fetch_add(1u, std::memory_order_relaxed);
                    } else {
                        highWater = curr.tick;
                    }
                }
                observations.fetch_add(1u, std::memory_order_relaxed);
            } else {
                if (!SameSnapshotBytes(prev, sentinel) || !SameSnapshotBytes(curr, sentinel)) {
                    clobberedOnFailure.fetch_add(1u, std::memory_order_relaxed);
                }
                failedAcquires.fetch_add(1u, std::memory_order_relaxed);
            }

            if (done) break;
        }
    });

    writer.join();
    reader.join();

    LC_CHECK_EQ(tornSingle.load(std::memory_order_relaxed), 0u);
    LC_CHECK_EQ(tornPair.load(std::memory_order_relaxed), 0u);
    LC_CHECK_EQ(nonConsecutive.load(std::memory_order_relaxed), 0u);
    LC_CHECK_EQ(wentBackwards.load(std::memory_order_relaxed), 0u);
    // A never-written slot, or a tick outside what the writer published, reaching the reader.
    LC_CHECK_EQ(outOfRange.load(std::memory_order_relaxed), 0u);
    // A failed acquire that touched the caller's buffers anyway.
    LC_CHECK_EQ(clobberedOnFailure.load(std::memory_order_relaxed), 0u);
    // The reader's last loop runs after writerDone, with the writer quiesced, so at least one
    // acquire always succeeds — a run with zero observations would mean the test proved
    // nothing and must fail rather than pass silently.
    LC_CHECK(observations.load(std::memory_order_relaxed) > 0u);
    // Not asserted as nonzero: whether the pair acquire ever loses is a scheduling accident,
    // and a machine where the reader always wins is not a failure. The counter is here so the
    // clobber check above is honestly scoped — it only proves something on runs that did lose.
    (void)failedAcquires.load(std::memory_order_relaxed);

    // The ring is quiescent now, so the final state is exact.
    LC_CHECK_EQ(ring.PublishedCount(), static_cast<u64>(kTicks));
    Snapshot prev;
    Snapshot curr;
    LC_CHECK(ring.AcquireLatestPair(prev, curr));
    LC_CHECK_EQ(curr.tick, kTicks);
    LC_CHECK_EQ(prev.tick, kTicks - 1u);
    LC_CHECK_EQ(curr.worldHash, SnapshotHashForTick(kTicks));
    LC_CHECK_EQ(prev.worldHash, SnapshotHashForTick(kTicks - 1u));
    LC_CHECK(SnapshotSelfConsistent(curr));
    LC_CHECK(SnapshotSelfConsistent(prev));
}

LC_TEST(command_queue_threaded_single_producer_single_consumer_is_lossless) {
    // The queue's real job: the game thread pushes while the sim thread pops, and nothing is
    // reordered, duplicated or silently corrupted, over far more commands than the ring can
    // hold — so the ring wraps continuously with both cursors live on different cores.
    static constexpr u32 kCommands = 200000u;

    CommandQueue queue;
    std::atomic<bool> producerDone{false};
    std::atomic<u32>  corrupted{0};
    std::atomic<u32>  rejected{0};
    std::atomic<u32>  received{0};
    std::atomic<u32>  impossibleCount{0};

    // Count() loads head_ before tail_ on purpose, so that a stale head against a fresh tail
    // reads HIGH rather than underflowing to ~4e9 (which would latch Full() true forever and
    // wedge any producer that waits on it). Note WHO that ordering actually protects: neither
    // the producer nor the consumer can underflow whichever order is used, because each owns
    // one cursor exactly and reads its own value — the producer always sees the true tail, the
    // consumer always sees a tail it has already observed to be >= its own head. The load order
    // only matters for a THIRD thread that owns neither cursor and can therefore see the two
    // move independently: a debug HUD or telemetry read through Simulation::Commands(). So the
    // observer below is not decoration — it is the only participant that can catch the swap.
    //
    // The bound has to be chosen carefully, and capacity is NOT it. For an observer that owns
    // neither cursor the two loads are separated in time and BOTH cursors move in between, so a
    // legitimate reading can exceed capacity by an unbounded amount — the header says "read
    // high", and it means it. What is genuinely impossible is exceeding the total number of
    // commands this test ever pushes: tail_ tops out at kCommands and head_ starts at zero, so
    // any larger value can only have come from t - h wrapping, which is precisely the failure
    // the load order exists to prevent (and which reads as ~4.29e9, not 200001).
    const auto sampleCount = [&]() {
        if (queue.Count() > kCommands) {
            impossibleCount.fetch_add(1u, std::memory_order_relaxed);
        }
    };

    std::atomic<bool> observerStop{false};
    std::atomic<u64>  observerSamples{0};
    std::thread observer([&]() {
        // Owns no cursor. Only ever observes — Count()/Empty()/Full() are the only calls the
        // header permits from here. Yields rather than spinning flat out, so on a two-core
        // machine it samples the live cursors instead of starving them.
        while (!observerStop.load(std::memory_order_acquire)) {
            sampleCount();
            (void)queue.Empty();
            (void)queue.Full();
            observerSamples.fetch_add(1u, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    std::thread producer([&]() {
        for (u32 i = 0; i < kCommands; ++i) {
            // Wait for room rather than letting Push reject: with a single producer, a
            // not-full queue is guaranteed to still have room when we get there, because only
            // the consumer ever frees slots. Spinning on Push() instead would be correct but
            // would emit a WARN per rejection.
            while (queue.Full()) std::this_thread::yield();
            sampleCount();
            if (!queue.Push(MakeCommand(i))) rejected.fetch_add(1u, std::memory_order_relaxed);
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        u32 expected = 0;
        for (;;) {
            const bool done = producerDone.load(std::memory_order_acquire);
            sampleCount();
            Command got;
            bool drained = false;
            while (queue.Pop(got)) {
                drained = true;
                if (!SameCommand(got, MakeCommand(expected))) {
                    corrupted.fetch_add(1u, std::memory_order_relaxed);
                }
                ++expected;
            }
            if (done && !drained && queue.Empty()) break;
        }
        received.store(expected, std::memory_order_relaxed);
    });

    producer.join();
    consumer.join();
    observerStop.store(true, std::memory_order_release);
    observer.join();

    LC_CHECK(observerSamples.load(std::memory_order_relaxed) > 0u);
    LC_CHECK_EQ(corrupted.load(std::memory_order_relaxed), 0u);
    LC_CHECK_EQ(rejected.load(std::memory_order_relaxed), 0u);
    LC_CHECK_EQ(received.load(std::memory_order_relaxed), kCommands);
    // Count() never wrapped while both cursors were live and a third thread was reading it.
    LC_CHECK_EQ(impossibleCount.load(std::memory_order_relaxed), 0u);
    LC_CHECK(queue.Empty());
    // Every command arrived, exactly once, in order, with its payload intact — across ~780
    // wraps of a 256-slot ring while both cursors were live on different cores.
    // The producer absorbed back-pressure by waiting on Full() rather than by being refused,
    // so the rejection counter stays at zero and the log stays quiet.
}
