// LIVING CITY — sim -> engine state channel.
//
// ARCHITECTURE.md Boundary 1, outbound half: "Sim -> engine: state only."
// The sim publishes an immutable Snapshot at the end of every tick. The renderer reads the two
// most recent published snapshots and interpolates between them for the frame it is drawing.
// The renderer NEVER mutates sim state and NEVER calls into a sim system mid-tick; if it needs
// to influence the world it pushes a Command (see sim/Commands.h).
//
// R1: zero Unreal types. R3: no floats cross this boundary — see the note on Snapshot below.
// I5: SnapshotRing allocates nothing after construction.
#pragma once

#include "livingcity/core/Core.h"

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace lc {

// ---------------------------------------------------------------- Snapshot
// The immutable view the renderer reads. Phase 0 content only — no voxels, no agents, no
// economy (PHASE_0.md: "an empty tick means the loop, the clock, the RNG, the event bus, and
// the snapshot buffer, and nothing else").
//
// WHY THERE ARE NO FLOATS HERE. Sim positions are fixed-point; this struct carries their RAW
// integer representation. The float conversion happens on the renderer's side of the boundary,
// at the last possible moment, so no float ever exists on a code path that could feed back into
// sim state (R3). A `float placeholderX` here would be an invitation to round-trip a rendered
// position back into the sim and lose determinism to the FPU.
//
// `pad` is an explicit, always-zero member rather than compiler-inserted padding, so a
// Snapshot's byte image is fully determined by its field values — memcmp and byte hashing over
// a Snapshot are then meaningful rather than dependent on what the compiler left behind.
// Every member has a default initialiser: a Snapshot is never born holding stack garbage.
// Upper bound on citizens carried to the renderer in one frame. The simulation may hold more
// than this; only the ones the view can draw travel across the boundary.
constexpr u32 kMaxSnapshotCitizens = 2560u;

// Field order is deliberate: 8-byte members, then 4-byte scalars, then 4-byte arrays, then the
// byte array and the flags last. That leaves the struct completely free of compiler padding,
// which matters because padding bytes are indeterminate and this struct is copied wholesale
// between threads (see D-013 for the same reasoning applied to events).
struct Snapshot {
    TickIndex tick         = 0;
    u64       worldHash    = 0;
    i64       placeholderX = 0;  // raw fixed-point units, NOT float
    i64       placeholderY = 0;
    i64       placeholderZ = 0;

    // Terrain: bumps on every change, so the view can rebuild only what moved.
    u64       terrainVersion     = 0;

    // Economy, in minor units (cents). totalMoney must equal what the central bank has
    // issued on every single tick - that is invariant I3, reported live on the HUD.
    i64       totalMoneyMinor    = 0;
    i64       governmentMinor    = 0;
    i64       avgShopPriceMinor  = 0;
    i64       followedMoneyMinor = 0;

    u32       agentCount   = 0;
    u32       citizenCount = 0;

    // How the population is spending this moment, for the HUD. Indexed by Activity.
    u32       atHome       = 0;
    u32       travelWork   = 0;
    u32       atWork       = 0;
    u32       travelHome   = 0;
    u32       travelShop   = 0;
    u32       atShop       = 0;

    u32       hungryCount      = 0;
    u32       starvationEvents = 0;   // I11: must stay 0 while shops have stock and people have money
    u32       totalShopStock   = 0;
    u32       followedIndex    = 0;   // the one citizen the HUD tracks in detail

    // Decomposed sim calendar, so the view never has to know the tick hierarchy.
    i32       year         = 0;
    i32       day          = 0;
    i32       hour         = 0;
    i32       minute       = 0;

    i32       followedX    = 0;   // metres
    i32       followedY    = 0;

    u16       followedHunger  = 0;
    u16       followedFatigue = 0;

    // Citizen positions in METRES from the city centre. Integers, never floats: the renderer
    // converts to Unreal centimetres on its own side of the boundary (R3).
    i32       citizenX[kMaxSnapshotCitizens]     = {};
    i32       citizenY[kMaxSnapshotCitizens]     = {};
    u8        citizenState[kMaxSnapshotCitizens] = {};

    u8        followedActivity = 0;
    u8        paused           = 0;
    u8        pad[10]          = {};
};

static_assert(std::is_trivially_copyable_v<Snapshot>,
              "Snapshot is copied out of the ring by value on the render thread");
static_assert(std::is_standard_layout_v<Snapshot>, "Snapshot must be standard layout");

// R3, enforced at compile time rather than trusted to review. A float reaching this struct is
// a determinism bug that would only surface as a replay divergence on someone else's CPU, so
// it must fail the build the moment it is written, not a test run later.
static_assert(!std::is_floating_point_v<decltype(Snapshot::tick)>, "R3: no floats in Snapshot");
static_assert(!std::is_floating_point_v<decltype(Snapshot::worldHash)>,
              "R3: no floats in Snapshot");
static_assert(!std::is_floating_point_v<decltype(Snapshot::placeholderX)>,
              "R3: sim positions cross this boundary as raw fixed-point, never as float");
static_assert(!std::is_floating_point_v<decltype(Snapshot::placeholderY)>,
              "R3: sim positions cross this boundary as raw fixed-point, never as float");
static_assert(!std::is_floating_point_v<decltype(Snapshot::placeholderZ)>,
              "R3: sim positions cross this boundary as raw fixed-point, never as float");
static_assert(!std::is_floating_point_v<decltype(Snapshot::agentCount)>,
              "R3: no floats in Snapshot");
static_assert(alignof(Snapshot) == 8, "Snapshot alignment changed");
static_assert(offsetof(Snapshot, tick) == 0, "Snapshot layout changed");
static_assert(offsetof(Snapshot, worldHash) == 8, "Snapshot layout changed");
static_assert(offsetof(Snapshot, placeholderX) == 16, "Snapshot layout changed");
// 5 x 8-byte fields (terrain version + four money totals) sit between the placeholder and
// agentCount, so it starts at 16 + 24 + 40 = 80.
static_assert(offsetof(Snapshot, agentCount) == 80, "Snapshot layout changed");

// Padding-free: the sum of the members must equal the whole. A compiler-inserted gap here
// would be indeterminate bytes copied between threads on every publish.
static_assert(sizeof(Snapshot) ==
                  sizeof(TickIndex) + sizeof(u64) + 3 * sizeof(i64)     // tick, hash, placeholder
                  + sizeof(u64) + 4 * sizeof(i64)                       // terrain version, money
                  + 12 * sizeof(u32) + 6 * sizeof(i32) + 2 * sizeof(u16)
                  + 2 * sizeof(i32) * kMaxSnapshotCitizens
                  + sizeof(u8) * kMaxSnapshotCitizens
                  + 12 * sizeof(u8),
              "Snapshot gained padding - reorder the fields largest-first");

// ---------------------------------------------------------------- SnapshotRing
//
// SINGLE WRITER (the sim thread), SINGLE READER (the render thread). Three buffers.
//
//   Writer: BeginWrite() -> fill every field -> Publish().        Never call Acquire*().
//   Reader: AcquireLatest() / AcquireLatestPair().                Never call BeginWrite().
//
// -------- why the reader can never observe a torn snapshot --------
//
// There is one monotonically increasing counter, `published_`, holding the number of snapshots
// published so far. Snapshot number `s` (0-based) lives in slot `s % 3`. Therefore:
//
//   * the writer is always working on slot `published_ % 3`;
//   * the newest published snapshot ("curr") is in slot `(published_ - 1) % 3`;
//   * the one before it ("prev")            is in slot `(published_ - 2) % 3`.
//
// Those three indices are pairwise distinct, so at any instant the writer's slot is neither of
// the two the reader wants. That alone is not enough — the writer may advance WHILE the reader
// is copying — so every acquire is validated seqlock-style: read `published_`, copy, then read
// `published_` again and reject the copy if the writer could have reached a slot we touched.
//
// The exact bound, which is where the "three" in triple buffering is earned:
//
//   Let `before` and `after` be the two observations. The counter only increases, so between
//   the two loads the writer wrote slots `before % 3 .. (after - 1) % 3` and may currently be
//   inside slot `after % 3`.
//
//   AcquireLatestPair needs BOTH `(before-1) % 3` and `(before-2) % 3` intact, and
//   `(before-2) % 3 == (before+1) % 3` is the very next slot the writer moves to. So a single
//   publish during the copy already endangers `prev`: the pair is valid only if
//   `after == before`. Because the counter is monotonic, observing the same value twice proves
//   it held that value for the whole interval, which proves the writer never left slot
//   `before % 3`.
//
//   AcquireLatest needs only `(before-1) % 3`. The writer touches `before % 3` and then
//   `(before+1) % 3`, neither of which is `(before-1) % 3`, so one publish is tolerable and the
//   single-snapshot acquire is valid if `after - before <= 1`. Two publishes would put the
//   writer in `(before+2) % 3 == (before-1) % 3`, so that is the cut-off.
//
// std::atomic_thread_fence(acquire) sits between the copy and the second load so neither the
// compiler nor the CPU can float the field reads past the validating load; the leading acquire
// load stops them being hoisted above it. The copy therefore happens strictly inside the
// window the two observations bound.
//
// THE WRITE SIDE NEEDS A BARRIER TOO, and it is the easy one to forget because x86 hides it.
// Everything above rests on "counter value V bounds which slots the writer can have touched",
// which requires the publish store to become visible to the reader BEFORE the writer's stores
// into the next slot. A release store does not provide that — release stops earlier accesses
// sinking past the store, not later stores being hoisted above it. So Publish() ends with a
// release fence; see the comment there. Without it, ARM64 may expose a slot store before the
// counter update that licensed it, and a reader validating against the stale counter would
// accept a torn snapshot. On x64 the fence costs nothing (TSO already orders stores), which is
// exactly why the omission cannot be caught by any test on this machine.
//
// Honest caveat: like every seqlock, the field reads race with the writer in the formal C++
// memory model. The validation is what makes any torn read observable and discarded rather
// than returned, and no torn value ever escapes this class. If a future phase wants strict
// model conformance the buffers become arrays of std::atomic words; Phase 0 does not need it.
//
// A rejected acquire retries a bounded number of times and then returns false rather than
// spinning — the render thread must never block on the sim thread. Returning false means
// "nothing stable to draw this instant"; the caller reuses the previous frame's pair.
//
// What that costs in practice. At the real 20 Hz sim rate the writer publishes every 50 ms and
// the reader needs tens of nanoseconds, so it effectively never loses. The measured failure
// rate only becomes interesting when the writer publishes flat out — the threaded test, and
// fast-forward. There, AcquireLatest still succeeds essentially always (it tolerates one
// publish), while AcquireLatestPair loses often, because with three buffers "prev" is exactly
// the slot the writer moves to next. The renderer then keeps interpolating the pair it already
// holds, which is the right answer anyway: a renderer that cannot keep up with a fast-forwarded
// sim should not be trying to draw every intermediate tick. A fourth buffer would make the
// pair acquire contention-free; it is deliberately not spent here, because Phase 0 does not
// need it and three buffers make the invariant easy to state and easy to prove.
class LC_API SnapshotRing {
public:
    static constexpr u32 kBufferCount = 3u;

    // Bounded retry. The render thread gives up rather than spinning on a sim thread that is
    // outrunning it (fast-forward, or a machine under load).
    static constexpr u32 kMaxAcquireAttempts = 16u;

    SnapshotRing()                               = default;
    SnapshotRing(const SnapshotRing&)            = delete;
    SnapshotRing& operator=(const SnapshotRing&) = delete;

    // Writer. Returns the back buffer, which still holds the snapshot published three
    // publishes ago — it is NOT cleared, because clearing would be work the writer does not
    // need. The writer must therefore assign EVERY field before calling Publish(); a
    // half-filled snapshot leaks stale state to the renderer.
    // Idempotent: calling it repeatedly before Publish() returns the same object.
    Snapshot& BeginWrite();

    // Writer. Makes whatever is currently in the back buffer visible to the reader, atomically,
    // and moves the writer on to the next slot.
    void Publish();

    // Reader. The two most recent published snapshots, for render interpolation:
    // `prev` is the older, `curr` the newer, and they are always consecutive publishes.
    // Returns false until at least two snapshots have been published, and false if the writer
    // outran the reader for kMaxAcquireAttempts consecutive tries. `prev` and `curr` are left
    // untouched when it returns false.
    bool AcquireLatestPair(Snapshot& prev, Snapshot& curr) const;

    // Reader. The most recent published snapshot. False until at least one publish, and false
    // if the writer outran the reader for kMaxAcquireAttempts consecutive tries. Like
    // AcquireLatestPair, `out` is left untouched when it returns false: the copy is made into
    // a local and only assigned on success, so a failed acquire can never leave the caller
    // holding half of a snapshot it did not ask for.
    bool AcquireLatest(Snapshot& out) const;

    // Total snapshots published since construction. Also the sequence number the writer's
    // current back buffer will be published as.
    u64 PublishedCount() const;

private:
    static constexpr std::size_t kCacheLine = 64u;

    // Slots are spaced a whole number of cache lines apart, however large Snapshot is. The
    // original scheme pinned a slot to exactly one line, which stopped working the moment the
    // snapshot began carrying citizen positions. Rounding UP to the next multiple keeps the
    // property that actually mattered - no two slots share a line - without capping the
    // payload. The +1 guarantees at least one pad byte even when sizeof(Snapshot) is already
    // an exact multiple, because a zero-length array is not valid standard C++.
    static constexpr std::size_t kSlotSize =
        ((sizeof(Snapshot) / kCacheLine) + 1u) * kCacheLine;

    // Slots are padded to 64 bytes so consecutive slots are a full cache line APART. Note what
    // that does and does not buy: the spacing is real, but SnapshotRing's alignment is only 8
    // (alignas on a member trips MSVC C4324 under /W4, and warnings are errors here; alignas
    // on the class would push alignof(Simulation) to 64 and route its allocation through the
    // aligned operator new, which Core.cpp does not instrument — so the I5 allocation counters
    // would quietly stop seeing it). A ring that happens to start mid-line therefore still has
    // two slots sharing one line at the seam. This is a cache-contention detail only — the
    // correctness argument above rests on the counter and the fences, never on layout — and at
    // 20 Hz it is unmeasurable. Do not "fix" it by adding alignas without checking the
    // allocation-tracking consequence.
    struct Slot {
        Snapshot value;
        u8       cacheLinePad[kSlotSize - sizeof(Snapshot)];
    };

    static_assert(sizeof(Slot) == kSlotSize, "Slot padding is wrong");
    static_assert(sizeof(Slot) % kCacheLine == 0,
                  "slots must stay a whole number of cache lines apart");

    // published_ first, spaced a full line ahead of slot 0 so the counter the reader hammers
    // does not share a line with it.
    std::atomic<u64> published_{0};

    // [[maybe_unused]]: pure spacing, never read. MSVC does not warn, but clang's
    // -Wunused-private-field does, and these headers are compiled by UBT — which uses clang
    // on Android, Mac, iOS and Windows-on-ARM. Without this the module fails to build there.
    [[maybe_unused]] u8 counterPad_[kCacheLine - sizeof(std::atomic<u64>)] = {};

    Slot slots_[kBufferCount] = {};
};

// Pinned so a change to the padding scheme is a deliberate edit rather than a silent one.
static_assert(sizeof(SnapshotRing) % 64u == 0, "SnapshotRing must stay cache-line aligned in size");

} // namespace lc
