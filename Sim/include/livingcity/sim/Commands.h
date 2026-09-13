// LIVING CITY — engine -> sim command channel.
//
// ARCHITECTURE.md Boundary 1, inbound half: "Engine -> sim: input only."
// A player intent becomes a Command, is stamped with the sim tick that consumed it, and is
// appended to the replay log. This queue is the ONLY way the engine influences the sim.
// If something appears to need a second inbound channel, that is a design error — say so
// rather than adding one (ARCHITECTURE.md §2).
//
// R1: zero Unreal types. R3: no floats, no wall-clock, no pointers in the payload.
// R10/I5: no allocation on push or pop — the ring is allocated once with the queue.
#pragma once

#include "livingcity/core/Core.h"

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace lc {

// ---------------------------------------------------------------- command vocabulary
// Phase 0 is a skeleton. This list stays deliberately tiny: it is exactly what M0.3 and M0.8
// need (pause, single-step, fast-forward, a placeholder to move, and a way to quit).
// Values are explicit and MUST NOT be renumbered — they are written into the replay log
// (M0.6) and a renumbering would silently reinterpret every recorded run.
enum class CommandType : u8 {
    None            = 0,
    SetPaused       = 1,  // arg0 != 0 -> paused
    StepOnce        = 2,  // advance exactly one tick while paused
    SetTimeScale    = 3,  // arg1 = fixed-point time scale, raw units. Never a float.
    MovePlaceholder = 4,  // arg1 = dx, arg2 = dy, both raw fixed-point units
    Interact        = 5,  // arg0 = target kind, arg1 = target id
    Quit            = 6,

    // Dig or pile terrain with a round brush. Goes through the replay log like every other
    // command, so a dig is exactly as replayable as a keypress.
    //   arg0 = (radiusCm & 0xFFFF) | ((u32)(deltaCm + 32768) << 16)
    //   arg1 = x, centimetres      arg2 = y, centimetres
    ModifyTerrain   = 7,
};

// Packing helpers for ModifyTerrain, so the encoding lives in one place.
inline u32 PackTerrainBrush(i32 radiusCm, i32 deltaCm) {
    const u32 r = static_cast<u32>(radiusCm < 0 ? 0 : (radiusCm > 65535 ? 65535 : radiusCm));
    const i32 d = deltaCm < -32768 ? -32768 : (deltaCm > 32767 ? 32767 : deltaCm);
    return r | (static_cast<u32>(d + 32768) << 16);
}
inline i32 UnpackTerrainRadiusCm(u32 arg0) { return static_cast<i32>(arg0 & 0xFFFFu); }
inline i32 UnpackTerrainDeltaCm(u32 arg0)  { return static_cast<i32>(arg0 >> 16) - 32768; }

// ---------------------------------------------------------------- Command
// Trivially copyable POD with an explicitly pinned layout:
//   * no pointers and no std::string, so a Command can be handed across threads by value and
//     memcpy'd straight into the binary replay log;
//   * `pad` is a real, named, always-zero member rather than compiler-inserted padding, so the
//     byte image of a Command is fully determined by its field values (I1). Indeterminate
//     padding bytes reaching the world hash would be a nondeterminism bug that only shows up
//     on someone else's machine.
// Every member carries a default initialiser, so `Command c;` is fully defined — never a
// stack-garbage command entering the hash or the log.
struct Command {
    CommandType type         = CommandType::None;
    u8          pad[3]       = {0, 0, 0};
    u32         arg0         = 0;
    i64         arg1         = 0;
    i64         arg2         = 0;
    TickIndex   issuedAtTick = 0;
};

static_assert(std::is_trivially_copyable_v<Command>,
              "Command must be memcpy-able: it crosses a thread boundary and is serialised raw");
static_assert(std::is_standard_layout_v<Command>,
              "Command must be standard layout for offsetof and for the replay log format");

// The replay log format depends on these. Changing any of them is a format break.
static_assert(sizeof(Command) == 32, "Command layout changed — bump the replay log version");
static_assert(alignof(Command) == 8, "Command alignment changed — bump the replay log version");
static_assert(offsetof(Command, type) == 0, "Command layout changed");
static_assert(offsetof(Command, pad) == 1, "Command layout changed");
static_assert(offsetof(Command, arg0) == 4, "Command layout changed");
static_assert(offsetof(Command, arg1) == 8, "Command layout changed");
static_assert(offsetof(Command, arg2) == 16, "Command layout changed");
static_assert(offsetof(Command, issuedAtTick) == 24, "Command layout changed");

// R3, enforced at compile time rather than trusted to review. A float in a Command would be
// memcpy'd into the replay log and hashed into the world, and would then replay differently on
// a machine with a different FPU. Fail the build the moment it is written.
static_assert(!std::is_floating_point_v<decltype(Command::arg0)>, "R3: no floats in Command");
static_assert(!std::is_floating_point_v<decltype(Command::arg1)>,
              "R3: arg1 is raw fixed-point units, never a float");
static_assert(!std::is_floating_point_v<decltype(Command::arg2)>,
              "R3: arg2 is raw fixed-point units, never a float");
static_assert(!std::is_floating_point_v<decltype(Command::issuedAtTick)>,
              "R3: sim time is a tick count, never a float and never wall-clock");
static_assert(std::is_same_v<std::underlying_type_t<CommandType>, u8>,
              "CommandType's width is part of the replay log format");

// Feed a command into the world hash.
//
// Hashed field by field, in declaration order, rather than as a raw byte blob: hashing the
// byte image would make the result depend on the compiler's padding decisions. `pad` IS
// hashed, deliberately — it must always be zero, and hashing it turns any stray garbage in
// those three bytes into a loud determinism divergence instead of a silent one.
inline void HashInto(Hasher& h, const Command& cmd) {
    h.U8(static_cast<u8>(cmd.type));
    h.U8(cmd.pad[0]);
    h.U8(cmd.pad[1]);
    h.U8(cmd.pad[2]);
    h.U32(cmd.arg0);
    h.I64(cmd.arg1);
    h.I64(cmd.arg2);
    h.U64(cmd.issuedAtTick);
}

// ---------------------------------------------------------------- CommandQueue
//
// SINGLE PRODUCER, SINGLE CONSUMER. READ THIS BEFORE USING IT.
//
//   Producer  = the engine / game thread. It may call ONLY Push().
//   Consumer  = the sim thread.           It may call ONLY Pop() and Clear().
//
// Exactly one thread in each role, for the lifetime of the queue. There is no mutex and there
// is no CAS: correctness rests entirely on there being a single writer of `tail_` and a single
// writer of `head_`. Two producers, or two consumers, silently corrupt the ring — this is not
// checked at runtime because checking it would cost more than the queue does.
//
// Count(), Empty() and Full() are safe to call from either side but are only EXACT when called
// from the consumer or while both sides are quiesced; called concurrently they return a
// conservative, possibly stale value (see Count()).
//
// Memory ordering:
//   Push  publishes the slot contents with a release store to tail_.
//   Pop   observes them with an acquire load of tail_, which synchronises-with that release.
//   Pop   frees the slot with a release store to head_; Push observes it with an acquire load,
//         which is what stops the producer from overwriting a slot the consumer is still in.
// Each side loads its OWN cursor relaxed, because it is the only writer of it.
//
// I5: Push and Pop allocate nothing. The ring is a fixed-size member array. The only stdio in
// here is the overflow warning, which is a failure path on the producer thread, not the sim.
class CommandQueue {
public:
    // 256 commands is already absurd for one 20 Hz tick of player input; the queue exists to
    // absorb a burst, not to buffer a session. A power of two so the index mask is an AND, and
    // — because it divides 2^32 — so masking stays correct when the u32 cursors wrap.
    static constexpr u32 kCapacity = 256u;
    static constexpr u32 kMask     = kCapacity - 1u;

    static_assert(kCapacity != 0u && (kCapacity & kMask) == 0u,
                  "kCapacity must be a power of two");

    CommandQueue()                               = default;
    CommandQueue(const CommandQueue&)            = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    // ---- producer side ------------------------------------------------------------------
    // Returns false WITHOUT enqueuing if the ring is full. It never overwrites the oldest
    // unread command: doing so would silently change the input log and therefore the world
    // (I1), which is far worse than refusing one keypress.
    //
    // A full queue is loud, never silent. A rejection means the sim thread has fallen far
    // enough behind the game thread to overflow a whole tick's worth of input, which is
    // either a stall or a bug, and it must not be discoverable only by noticing that a
    // keypress did nothing.
    //
    // The counter is called REJECTED, not dropped, because whether a rejection becomes lost
    // player intent is the caller's policy and the queue cannot know it: a producer that
    // retries loses nothing. A producer that retries in a spin loop should test Full() before
    // calling Push(), so back-pressure costs it a yield rather than a warning per spin.
    bool Push(const Command& cmd) {
        const u32 t = tail_.load(std::memory_order_relaxed);  // producer owns tail_
        const u32 h = head_.load(std::memory_order_acquire);  // consumer's progress
        if (static_cast<u32>(t - h) >= kCapacity) {
            const u64 total = rejected_.fetch_add(1u, std::memory_order_relaxed) + 1u;
            LC_LOG_WARN("CommandQueue full (capacity %u): refused command type %u issued at "
                        "tick %llu; %llu refused in total",
                        static_cast<unsigned>(kCapacity),
                        static_cast<unsigned>(cmd.type),
                        static_cast<unsigned long long>(cmd.issuedAtTick),
                        static_cast<unsigned long long>(total));
            return false;
        }
        slots_[t & kMask] = cmd;
        tail_.store(t + 1u, std::memory_order_release);
        return true;
    }

    // ---- consumer side ------------------------------------------------------------------
    bool Pop(Command& out) {
        const u32 h = head_.load(std::memory_order_relaxed);  // consumer owns head_
        const u32 t = tail_.load(std::memory_order_acquire);  // producer's progress
        if (h == t) return false;
        out = slots_[h & kMask];
        head_.store(h + 1u, std::memory_order_release);
        return true;
    }

    // Discard everything pending. Consumer side, or with both sides quiesced.
    void Clear() {
        head_.store(tail_.load(std::memory_order_acquire), std::memory_order_release);
    }

    // ---- observation --------------------------------------------------------------------
    // head_ is loaded FIRST on purpose: head <= tail always holds and both cursors only ever
    // increase, so an older head against a newer tail can never underflow. The result may
    // read high (never negative, never wrapped) if the producer advances mid-call, so treat
    // it as a lower-bound-of-work-pending unless the queue is quiesced.
    u32 Count() const {
        const u32 h = head_.load(std::memory_order_acquire);
        const u32 t = tail_.load(std::memory_order_acquire);
        return static_cast<u32>(t - h);
    }

    bool Empty() const { return Count() == 0u; }
    bool Full() const { return Count() >= kCapacity; }

    static constexpr u32 Capacity() { return kCapacity; }

    // Push calls refused because the ring was full, over the lifetime of the queue.
    // Nonzero means the producer hit back-pressure. That is only data loss if the producer
    // chose not to retry — see the note on Push.
    u64 RejectedPushCount() const { return rejected_.load(std::memory_order_relaxed); }

private:
    // Cache-line separation, spelled out as padding rather than alignas: alignas on a member
    // trips MSVC C4324 under /W4, and warnings are errors here. 64-byte spacing removes the
    // false sharing between the producer's cursor and the consumer's cursor, which is the
    // whole reason an SPSC ring is cheap.
    static constexpr std::size_t kCacheLine = 64u;

    // Unlike SnapshotRing's slots, this separation is delivered unconditionally: head_ and
    // tail_ are 4-byte atomics 64 bytes apart, and 4-byte alignment means neither can straddle
    // a line, so they land on different lines wherever the queue itself starts.
    //
    // [[maybe_unused]] on both pads: they are pure spacing and are never read. MSVC does not
    // warn, but clang's -Wunused-private-field does, and this header is compiled by UBT —
    // which uses clang on Android, Mac, iOS and Windows-on-ARM. Without the attribute the
    // module fails to build on those platforms, and nothing on this machine can catch that.
    std::atomic<u32> head_{0};  // consumer writes, producer reads
    [[maybe_unused]] u8 headPad_[kCacheLine - sizeof(std::atomic<u32>)] = {};

    std::atomic<u32> tail_{0};  // producer writes, consumer reads
    [[maybe_unused]] u8 tailPad_[kCacheLine - sizeof(std::atomic<u32>)] = {};

    std::atomic<u64> rejected_{0};  // producer writes, anyone may read

    Command slots_[kCapacity] = {};
};

// head_ and tail_ land a full line apart, which is the point of the padding; pinned so a
// change to the layout is a deliberate edit rather than a silent one.
static_assert(sizeof(CommandQueue) == 8328, "CommandQueue layout changed");

static_assert(std::atomic<u32>::is_always_lock_free,
              "CommandQueue needs a lock-free 32-bit atomic; a locking fallback would "
              "put a mutex on the sim thread");
static_assert(std::atomic<u64>::is_always_lock_free,
              "SnapshotRing and the rejection counter need a lock-free 64-bit atomic");

} // namespace lc
