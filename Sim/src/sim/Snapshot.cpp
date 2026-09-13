// LIVING CITY — SnapshotRing implementation.
//
// The full correctness argument for the index scheme and the memory ordering lives in
// sim/Snapshot.h. Read it before touching anything here: every load and store below is
// load-bearing, and "tidying up" an ordering is how a lock-free ring starts returning torn
// data once a month on someone else's machine.
#include "livingcity/sim/Snapshot.h"

namespace lc {

namespace {

// Slot for a given publish sequence number. Snapshot `s` lives in slot `s % 3`.
inline std::size_t SlotIndex(u64 sequence) {
    return static_cast<std::size_t>(sequence % static_cast<u64>(SnapshotRing::kBufferCount));
}

} // namespace

Snapshot& SnapshotRing::BeginWrite() {
    // Writer-only path: the writer is the sole author of published_, so a relaxed load of its
    // own cursor is sufficient — there is nothing to synchronise with.
    const u64 sequence = published_.load(std::memory_order_relaxed);
    return slots_[SlotIndex(sequence)].value;
}

void SnapshotRing::Publish() {
    const u64 sequence = published_.load(std::memory_order_relaxed);
    // Release: every field the writer stored into the back buffer happens-before this counter
    // update, so a reader that observes the new count with an acquire load also observes the
    // complete snapshot. This single store is the entire publication step — which is why the
    // reader can never see a half-written snapshot become "the latest".
    published_.store(sequence + 1u, std::memory_order_release);

    // ...and the OTHER half of the write side, which is easy to miss because x86 hides it.
    //
    // The reader's whole argument is "counter value V bounds which slots the writer can have
    // touched". That needs the counter update above to become visible to the reader BEFORE the
    // writer's stores into the NEXT slot, which start as soon as this function returns. A
    // release store does not give that: release constrains earlier accesses from sinking past
    // it, never later stores from being hoisted above it. On x86/x64 the ordering is free
    // (TSO does not reorder stores) so the omission is invisible; on ARM64 a plain STR after
    // an STLR may be observed first, and then a reader that loads a stale counter can copy a
    // slot the writer has already re-entered and still validate. That is a torn snapshot.
    //
    // This is the same barrier a textbook seqlock needs between bumping the sequence and
    // writing the data it protects. Free on x64, a DMB on ARM, and it runs once per tick.
    std::atomic_thread_fence(std::memory_order_release);
}

u64 SnapshotRing::PublishedCount() const {
    return published_.load(std::memory_order_acquire);
}

bool SnapshotRing::AcquireLatest(Snapshot& out) const {
    for (u32 attempt = 0; attempt < kMaxAcquireAttempts; ++attempt) {
        const u64 before = published_.load(std::memory_order_acquire);
        if (before == 0u) return false;  // nothing published yet

        const Snapshot copy = slots_[SlotIndex(before - 1u)].value;

        // Keep the field reads above from sinking below the validating load.
        std::atomic_thread_fence(std::memory_order_acquire);
        const u64 after = published_.load(std::memory_order_acquire);

        // published_ never decreases, so after >= before. The writer touched slots
        // before%3 .. (after-1)%3 and may be inside after%3. Our slot is (before-1)%3, which
        // equals (before+2)%3 — the writer only reaches it once it has advanced by two.
        if (after - before <= 1u) {
            out = copy;
            return true;
        }
    }
    return false;  // the writer is persistently outrunning us; caller reuses its last pair
}

bool SnapshotRing::AcquireLatestPair(Snapshot& prev, Snapshot& curr) const {
    for (u32 attempt = 0; attempt < kMaxAcquireAttempts; ++attempt) {
        const u64 before = published_.load(std::memory_order_acquire);
        if (before < 2u) return false;  // interpolation needs two

        const Snapshot prevCopy = slots_[SlotIndex(before - 2u)].value;
        const Snapshot currCopy = slots_[SlotIndex(before - 1u)].value;

        std::atomic_thread_fence(std::memory_order_acquire);
        const u64 after = published_.load(std::memory_order_acquire);

        // Stricter than AcquireLatest: the writer's NEXT slot after before%3 is (before+1)%3,
        // which is exactly (before-2)%3 — the prev slot. So a single publish during the copy
        // already endangers prev, and only an unchanged counter proves the writer stayed put
        // for the whole interval (the counter is monotonic, so equal endpoints imply equal
        // throughout).
        if (after == before) {
            prev = prevCopy;
            curr = currCopy;
            return true;
        }
    }
    return false;
}

} // namespace lc
