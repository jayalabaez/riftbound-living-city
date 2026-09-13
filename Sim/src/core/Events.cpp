// LIVING CITY - deterministic event bus implementation.
// See Events.h for the contract. Nothing here allocates after construction, nothing here
// reads a clock, and nothing here compares or hashes a pointer value.

#include "livingcity/core/Events.h"

#include <algorithm>
#include <cstdio>    // std::snprintf, for the fatal diagnostics below
#include <cstring>

namespace lc {

namespace {

// One shared scratch buffer for fatal messages. Formatting happens only on the way to
// std::abort(), so there is no reentrancy concern and no allocation.
constexpr int kMsgBufSize = 256;

const char* SafeName(const char* n) { return n ? n : "<unnamed>"; }

} // namespace

// ---------------------------------------------------------------- construction
EventBus::EventBus() {
    Init(kDefaultCapacity, kDefaultMaxSubscribers, kDefaultMaxEventTypes);
}

EventBus::EventBus(u32 capacity, u32 maxSubscribers, u32 maxEventTypes) {
    Init(capacity, maxSubscribers, maxEventTypes);
}

EventBus::~EventBus() = default;

void EventBus::Init(u32 capacity, u32 maxSubscribers, u32 maxEventTypes) {
    LC_ASSERT_MSG(capacity > 0, "EventBus capacity must be at least 1");
    LC_ASSERT_MSG(maxSubscribers > 0, "EventBus maxSubscribers must be at least 1");
    LC_ASSERT_MSG(maxEventTypes > 0, "EventBus maxEventTypes must be at least 1");
    // The ring indexes with (head_ + count_), both u32, and head_ < capacity_ while
    // count_ <= capacity_. Pinning capacity below 2^31 keeps that sum from wrapping.
    // Unreachable in practice - a ring this large is 172 GB - but the modular arithmetic
    // below depends on it, so state it here rather than leave it to be rediscovered.
    LC_ASSERT_MSG(capacity <= 0x7FFFFFFFu, "EventBus capacity must be under 2^31");

    capacity_       = capacity;
    maxSubscribers_ = maxSubscribers;
    maxEventTypes_  = maxEventTypes;

    // The one and only allocation site. Everything downstream works inside these buffers,
    // and every capacity is a hard, loud limit rather than a silent grow (I5).
    // resize() zero-fills, so unused slot bytes are defined rather than indeterminate.
    records_.resize(static_cast<std::size_t>(capacity_));
    subs_.reserve(static_cast<std::size_t>(maxSubscribers_));
    order_.reserve(static_cast<std::size_t>(maxSubscribers_));
    ranges_.reserve(static_cast<std::size_t>(maxSubscribers_));
    types_.reserve(static_cast<std::size_t>(maxEventTypes_));
}

// ---------------------------------------------------------------- type table
// Sorted by id so lookups are a binary search and iteration order is deterministic and
// independent of registration order (R3: never iterate an unordered container).
u32 EventBus::FindTypeSlot(EventTypeId type, bool& found) const {
    u32 lo = 0;
    u32 hi = static_cast<u32>(types_.size());
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (types_[mid].id.value < type.value) lo = mid + 1u;
        else                                   hi = mid;
    }
    found = (lo < static_cast<u32>(types_.size())) && (types_[lo].id.value == type.value);
    return lo;
}

void EventBus::RegisterTypeRaw(EventTypeId type, const char* name, u32 size) {
    LC_ASSERT_MSG(type.IsValid(), "event type id 0 is reserved as 'none'");
    LC_ASSERT_MSG(size <= kMaxEventPayloadSize, "event payload exceeds kMaxEventPayloadSize");

    bool found = false;
    const u32 slot = FindTypeSlot(type, found);

    if (found) {
        const TypeInfo& existing = types_[slot];
        // Compare by content, never by pointer identity.
        const bool sameName = (std::strcmp(SafeName(existing.name), SafeName(name)) == 0);
        if (!sameName || existing.size != size) {
            // Overwhelmingly the cause is two structs given the SAME LC_EVENT_TYPE name
            // (copy-paste), not a genuine 64-bit FNV collision. Say so, and note the case
            // this check is blind to so the reader knows not to trust silence.
            char msg[kMsgBufSize];
            std::snprintf(msg, sizeof(msg),
                          "event type id clash: '%s' (%u bytes) and '%s' (%u bytes) share "
                          "id 0x%016llx. Two structs were almost certainly given the same "
                          "LC_EVENT_TYPE name - rename one. (Same name AND same size is "
                          "undetectable here, so check the names, not just this message.)",
                          SafeName(existing.name), existing.size, SafeName(name), size,
                          static_cast<unsigned long long>(type.value));
            LC_FATAL(msg);
        }
        return;
    }

    if (static_cast<u32>(types_.size()) >= maxEventTypes_) {
        char msg[kMsgBufSize];
        std::snprintf(msg, sizeof(msg),
                      "event type table full at %u entries while registering '%s' - raise "
                      "EventBus maxEventTypes",
                      maxEventTypes_, SafeName(name));
        LC_FATAL(msg);
        return;
    }

    types_.insert(types_.begin() + static_cast<std::ptrdiff_t>(slot),
                  TypeInfo{type, name, size});

    // A newly named type may give an existing range its payload size.
    dirty_ = true;
}

const char* EventBus::TypeName(EventTypeId type) const {
    bool found = false;
    const u32 slot = FindTypeSlot(type, found);
    if (!found) return "<unregistered>";
    return SafeName(types_[slot].name);
}

// ---------------------------------------------------------------- publishing
bool EventBus::PublishRaw(EventTypeId type, const char* typeName, const void* data, u32 size) {
    LC_ASSERT_MSG(type.IsValid(), "event type id 0 is reserved as 'none'");
    LC_ASSERT_MSG(size <= kMaxEventPayloadSize, "event payload exceeds kMaxEventPayloadSize");
    LC_ASSERT_MSG(data != nullptr || size == 0, "null event payload");

    if (count_ >= capacity_) {
        // I4/M0.4: overflow is loud. A dropped event is a divergence that shows up ten
        // thousand ticks later as an unexplained hash mismatch; refuse to be that bug.
        char msg[kMsgBufSize];
        std::snprintf(msg, sizeof(msg),
                      "event bus overflow: ring capacity %u exceeded publishing '%s' "
                      "(type 0x%016llx, %u bytes, publish #%llu). Raise the capacity or "
                      "drain more often - events are never dropped silently.",
                      capacity_, SafeName(typeName),
                      static_cast<unsigned long long>(type.value), size,
                      static_cast<unsigned long long>(totalPublished_ + 1ull));
        LC_FATAL(msg);  // [[noreturn]] - nothing below this line can run
    }

    const u32 tail = (head_ + count_) % capacity_;
    Record&   rec  = records_[tail];

    rec.type     = type;
    rec.size     = size;
    rec.sequence = static_cast<u32>(totalPublished_ & 0xFFFFFFFFull);
    if (size > 0) std::memcpy(rec.payload, data, static_cast<std::size_t>(size));

    ++count_;
    ++totalPublished_;
    return true;
}

// ---------------------------------------------------------------- subscribing
SubscriptionId EventBus::Subscribe(EventTypeId type, i32 priority, HandlerFn fn, void* user) {
    LC_ASSERT_MSG(type.IsValid(), "event type id 0 is reserved as 'none'");
    LC_ASSERT_MSG(fn != nullptr, "null event handler");

    if (static_cast<u32>(subs_.size()) >= maxSubscribers_) {
        // Reclaim tombstones first, but never while a drain is walking the table.
        if (!draining_ && deadCount_ > 0) Rebuild();
    }
    if (static_cast<u32>(subs_.size()) >= maxSubscribers_) {
        char msg[kMsgBufSize];
        std::snprintf(msg, sizeof(msg),
                      "event subscriber table full at %u entries (type '%s') - raise "
                      "EventBus maxSubscribers",
                      maxSubscribers_, TypeName(type));
        LC_FATAL(msg);
        return SubscriptionId{};
    }

    const SubscriptionId id = ids_.Next();
    subs_.push_back(Sub{id, type, priority, nextRegIndex_, fn, user, true});
    ++nextRegIndex_;
    dirty_ = true;
    return id;
}

void EventBus::Unsubscribe(SubscriptionId id) {
    if (!id.IsValid()) return;

    // Tombstone rather than erase: a handler may unsubscribe itself or a sibling mid-drain,
    // and Drain() is walking index-based views of this table. Compaction happens in
    // Rebuild(), which only ever runs outside a dispatch.
    for (std::size_t i = 0; i < subs_.size(); ++i) {
        if (subs_[i].id == id) {
            if (subs_[i].alive) {
                subs_[i].alive = false;
                ++deadCount_;
                dirty_ = true;
            }
            return;
        }
    }
}

u32 EventBus::SubscriberCount() const {
    return static_cast<u32>(subs_.size()) - deadCount_;
}

// ---------------------------------------------------------------- ordering
// Sorted ONCE whenever the subscriber set changes, never per event.
//
// Key: (type id asc, priority DESC, registration index asc). The registration index is
// unique, so this is a strict total order - std::sort is therefore fully determined and
// std::stable_sort (which allocates) is not needed.
void EventBus::Rebuild() {
    LC_ASSERT_MSG(!draining_, "EventBus subscriber table rebuilt during dispatch");

    if (deadCount_ > 0) {
        subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
                                   [](const Sub& s) { return !s.alive; }),
                    subs_.end());
        deadCount_ = 0;
    }

    order_.clear();
    for (u32 i = 0; i < static_cast<u32>(subs_.size()); ++i) order_.push_back(i);

    const std::vector<Sub>& subs = subs_;
    std::sort(order_.begin(), order_.end(), [&subs](u32 ia, u32 ib) {
        const Sub& a = subs[ia];
        const Sub& b = subs[ib];
        if (a.type.value != b.type.value) return a.type.value < b.type.value;
        if (a.priority   != b.priority)   return a.priority > b.priority;   // high first
        return a.regIndex < b.regIndex;                                     // then oldest
    });

    ranges_.clear();
    u32 i = 0;
    const u32 n = static_cast<u32>(order_.size());
    while (i < n) {
        const EventTypeId type = subs_[order_[i]].type;
        u32 j = i + 1u;
        while (j < n && subs_[order_[j]].type == type) ++j;

        bool      found = false;
        const u32 slot  = FindTypeSlot(type, found);
        ranges_.push_back(TypeRange{type, i, j - i, found ? types_[slot].size : 0u});
        i = j;
    }

    dirty_ = false;
}

const EventBus::TypeRange* EventBus::FindRange(EventTypeId type) const {
    u32 lo = 0;
    u32 hi = static_cast<u32>(ranges_.size());
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (ranges_[mid].type.value < type.value) lo = mid + 1u;
        else                                      hi = mid;
    }
    if (lo < static_cast<u32>(ranges_.size()) && ranges_[lo].type == type) return &ranges_[lo];
    return nullptr;
}

// ---------------------------------------------------------------- dispatch
void EventBus::Drain() {
    LC_ASSERT_MSG(!draining_, "EventBus::Drain() re-entered from a handler");

    if (dirty_) Rebuild();

    draining_ = true;

    // Snapshot the queue length FIRST. Anything a handler publishes lands behind this
    // window and is delivered by the next drain, so drain order can never depend on what
    // handlers happen to do.
    const u32 window = count_;

    for (u32 e = 0; e < window; ++e) {
        // Copy the record out and free its slot before dispatching, so a handler that
        // publishes into a nearly full ring cannot overwrite the event being delivered.
        // Trivially copyable, so the memcpy below is well defined; default-init leaves it
        // indeterminate only for the instant before that memcpy overwrites all of it.
        Record scratch;
        std::memcpy(&scratch, &records_[head_], sizeof(Record));
        head_ = (head_ + 1u) % capacity_;
        --count_;
        ++totalDispatched_;

        const TypeRange* range = FindRange(scratch.type);
        if (range == nullptr) continue;   // nobody listening: still consumed, in order.

        if (range->payloadSize != 0u && range->payloadSize != scratch.size) {
            char msg[kMsgBufSize];
            std::snprintf(msg, sizeof(msg),
                          "event payload size mismatch for '%s' (publish #%u): subscribers "
                          "expect %u bytes, publisher sent %u - two types share one id",
                          TypeName(scratch.type), scratch.sequence,
                          range->payloadSize, scratch.size);
            LC_FATAL(msg);
        }

        for (u32 k = 0; k < range->count; ++k) {
            const Sub& s = subs_[order_[range->begin + k]];
            if (!s.alive) continue;       // unsubscribed mid-drain: effective immediately
            s.fn(s.user, scratch.payload);
        }
    }

    draining_ = false;
}

// ---------------------------------------------------------------- state
void EventBus::Clear() {
    LC_ASSERT_MSG(!draining_, "EventBus::Clear() called from a handler");
    head_  = 0;
    count_ = 0;
}

void EventBus::HashInto(Hasher& h) const {
    h.U32(count_);
    for (u32 i = 0; i < count_; ++i) {
        const Record& rec = records_[(head_ + i) % capacity_];
        h.U64(rec.type.value);
        h.U32(rec.size);
        // Exactly the bytes the publisher wrote. Never the stale tail of the slot - those
        // bytes are leftovers from an older, larger event and are not world state.
        h.Bytes(rec.payload, static_cast<std::size_t>(rec.size));
    }
}

} // namespace lc
