// LIVING CITY - deterministic event bus.
//
// R1: zero Unreal types. R3: deterministic, no floats, no wall-clock, no pointer hashing,
// no unordered iteration. No exceptions, no RTTI. I5: zero allocation on the publish path.
//
// ---------------------------------------------------------------------------------------
// WHY THIS EXISTS - the layering rule (docs/ARCHITECTURE.md section 3)
// ---------------------------------------------------------------------------------------
// Dependencies inside LivingCitySim flow strictly downward:
//
//     sim/  ->  law/ destruct/ transport/ economy/ agents/ world/ director/  ->  core/
//
// A lower layer NEVER includes a higher layer's header. When a lower layer needs to affect
// a higher one it PUBLISHES AN EVENT instead. `law` fines an offender by emitting a
// `FineIssued` event that `economy` consumes; `economy` never includes a `law/` header.
// `destruct` publishes `EconomicShock` rather than reaching into a firm's balance sheet.
//
// This bus is that mechanism, and it is the only sanctioned one. If you find yourself
// wanting a second channel between layers, that is a design error - say so instead of
// adding one.
//
// ---------------------------------------------------------------------------------------
// GUARANTEES
// ---------------------------------------------------------------------------------------
//  * FIFO.        Drain() dispatches queued events in publish order, always.
//  * ORDERED.     For one event, subscribers run in DESCENDING priority; ties break by
//                 ASCENDING registration index. Dispatch order therefore depends only on
//                 (priority, registration index) - for DIFFERING priorities it is entirely
//                 independent of the order in which subscriptions were made.
//  * DEFERRED.    An event published from INSIDE a handler is NOT delivered by the drain
//                 that is running. It is delivered by the NEXT Drain(). Without this rule
//                 the drain order would depend on handler behaviour, which is a determinism
//                 hazard and a re-entrancy hazard both. The subscriber table is likewise
//                 frozen for the duration of a drain: a subscription created during a drain
//                 starts receiving at the next one. Unsubscribe is the one exception - it
//                 takes effect immediately, because "stop calling me" must mean now.
//  * LOUD.        Ring overflow is LC_FATAL, naming the event type and the capacity.
//                 An event is never silently dropped.
//  * NO ALLOC.    The ring, the subscriber table and the type table are sized once at
//                 construction. Publish, Drain, Subscribe and Unsubscribe allocate nothing.
//                 Exceeding a capacity is LC_FATAL, never a quiet grow.
//
// NOT THREAD SAFE, deliberately. The sim is single-threaded by design (R3: two threads
// racing on a queue is a determinism hazard, not a performance opportunity). There are no
// atomics here and none are wanted; call the bus from the sim thread only.
//
// ---------------------------------------------------------------------------------------
// DEFINING AN EVENT TYPE
// ---------------------------------------------------------------------------------------
// Event type ids are compile-time FNV-1a-64 hashes of an explicitly written name. No RTTI,
// no typeid, no std::type_index, and no cross-translation-unit static-initialisation order
// dependence: every id is a constant expression.
//
//     namespace lc::law {
//     struct FineIssued {
//         LC_EVENT_TYPE(law::FineIssued);   // <- explicit, stable, globally unique name
//         i64 amountMinorUnits;             // largest first, so there is no padding
//         u32 offender;                     // ids and integers only
//         u32 pad_ = 0;                     // keep the struct free of padding (see below)
//     };
//     } // namespace lc::law
//
// For a type you CANNOT edit, register it non-intrusively at GLOBAL scope instead:
//
//     LC_DECLARE_EVENT(lc::law::FineIssued)
//
// Use exactly ONE of the two. They are alternatives, not layers: each hashes the name it
// was literally handed, so LC_EVENT_TYPE(law::FineIssued) and
// LC_DECLARE_EVENT(lc::law::FineIssued) name the SAME type with DIFFERENT ids. Applying
// both to one type is now a compile error rather than a silent id change.
//
// The name is the identity. Two distinct structs given the same name string are one event
// type as far as this bus is concerned - and if they also happen to be the same size, the
// runtime collision check cannot see it and a handler will silently reinterpret the wrong
// payload. Qualify every name with its subsystem and never copy-paste an LC_EVENT_TYPE
// line without changing the name. Deriving one event struct from another does not mint a
// new id either; that case IS caught, at compile time (see EventTraits below).
//
// Requirements enforced by static_assert at the Publish() call site:
//   - trivially copyable  (it is memcpy'd into the ring)
//   - std::has_unique_object_representations_v  - i.e. NO PADDING BYTES and no floating
//     point. Queued events are hashed byte-for-byte into the world hash, and padding bytes
//     are indeterminate, so a padded event type would make the world hash non-reproducible.
//     If this fires: reorder fields largest-first, or add an explicit zeroed pad member.
//     It also rejects float/double outright, which is exactly what R3 wants.
//   - sizeof(T) <= EventBus::kMaxEventPayloadSize and alignof(T) <= 8.
// Do not put pointers in an event. Nothing can catch that for you, and a pointer value is
// not reproducible across runs.

#pragma once

#include "livingcity/core/Core.h"

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>   // std::declval, for the event-type self check below
#include <vector>    // the ring / subscriber / type tables are std::vector members

namespace lc {

// ---------------------------------------------------------------- event type ids
// A 64-bit compile-time hash of an explicitly authored name. Value 0 is "none".
struct EventTypeId {
    u64 value = 0;

    constexpr EventTypeId() = default;
    constexpr explicit EventTypeId(u64 v) : value(v) {}

    constexpr bool IsValid() const { return value != 0; }
    constexpr bool operator==(const EventTypeId& o) const { return value == o.value; }
    constexpr bool operator!=(const EventTypeId& o) const { return value != o.value; }
    constexpr bool operator<(const EventTypeId& o) const { return value < o.value; }
};

// Declares the intrusive id members. Put it on the FIRST line of the struct body.
// The argument is a token sequence used verbatim as the type's unique name; qualify it
// (`law::FineIssued`) so two subsystems cannot pick the same short name by accident.
//
// The third line is a self-type probe: a declared-but-never-defined member function whose
// return type is the enclosing struct. Static members are INHERITED, so without it a
// `struct Derived : SomeEvent {}` would silently answer with SomeEvent's id and name and
// publish itself as its base - a same-size derived type would sail straight past the
// runtime collision check and be reinterpreted by the base's handlers. The probe lets
// EventTraits below reject that at compile time. It is a declaration only: it changes
// neither the layout, the triviality, nor the aggregate-ness of the event struct.
#define LC_EVENT_TYPE(NameTokens)                                                         \
    static constexpr const char* kEventTypeName = #NameTokens;                            \
    static constexpr ::lc::u64   kEventTypeHash = ::lc::HashString(#NameTokens);          \
    auto LcEventTypeSelf() const -> ::std::remove_cvref_t<decltype(*this)>

namespace detail {

template <typename T, typename = void>
struct HasIntrusiveEventType : std::false_type {};

template <typename T>
struct HasIntrusiveEventType<T, std::void_t<decltype(T::kEventTypeHash),
                                            decltype(T::kEventTypeName)>> : std::true_type {};

// True when T's LC_EVENT_TYPE was written on T itself. A type with no probe at all has
// nothing to check and passes; a type that INHERITED its probe reports the base and fails.
template <typename T, typename = void>
struct EventTypeSelfMatches : std::true_type {};

template <typename T>
struct EventTypeSelfMatches<
    T, std::void_t<decltype(std::declval<const T&>().LcEventTypeSelf())>>
    : std::bool_constant<
          std::is_same_v<decltype(std::declval<const T&>().LcEventTypeSelf()), T>> {};

// T carries an LC_EVENT_TYPE of its very own (as opposed to none, or an inherited one).
template <typename T>
struct DeclaresOwnEventType
    : std::bool_constant<HasIntrusiveEventType<T>::value && EventTypeSelfMatches<T>::value> {};

} // namespace detail

// Primary template: reads the intrusive members planted by LC_EVENT_TYPE.
// LC_DECLARE_EVENT specialises this for types that cannot carry them.
template <typename T>
struct EventTraits {
    static_assert(detail::HasIntrusiveEventType<T>::value,
                  "Event type is not registered. Put LC_EVENT_TYPE(qualified::Name) inside "
                  "the struct, or LC_DECLARE_EVENT(qualified::Name) at global scope.");

    // Inheriting an id is never what you meant: the derived type would publish under its
    // base's id, and if the two are the same size nothing at runtime can tell them apart.
    static_assert(detail::EventTypeSelfMatches<T>::value,
                  "This event type INHERITED its LC_EVENT_TYPE from a base class instead of "
                  "declaring one, so it would silently reuse the base's event id. Give it "
                  "its own LC_EVENT_TYPE(qualified::Name), or register it with "
                  "LC_DECLARE_EVENT(qualified::Name) at global scope.");

    static constexpr EventTypeId Id() {
        if constexpr (detail::HasIntrusiveEventType<T>::value) {
            return EventTypeId(T::kEventTypeHash);
        } else {
            return EventTypeId(0);
        }
    }
    static constexpr const char* Name() {
        if constexpr (detail::HasIntrusiveEventType<T>::value) {
            return T::kEventTypeName;
        } else {
            return "<unregistered>";
        }
    }
};

// Non-intrusive registration. MUST be written at global scope; the argument must be the
// fully qualified type name, which is also used as the hashed name.
//
// The static_assert stops a type from carrying both macros: they hash whatever text they
// were handed, so a type with LC_EVENT_TYPE(law::FineIssued) that is also given
// LC_DECLARE_EVENT(lc::law::FineIssued) ends up with two different ids, and which one wins
// depends on whether the reader consults T::kEventTypeHash or EventTraits<T>::Id(). A type
// that merely INHERITS an intrusive id is still allowed here - that is exactly the fix.
#define LC_DECLARE_EVENT(QualifiedType)                                                    \
    namespace lc {                                                                        \
    template <>                                                                           \
    struct EventTraits<QualifiedType> {                                                   \
        static_assert(!::lc::detail::DeclaresOwnEventType<QualifiedType>::value,          \
                      "This type already declares LC_EVENT_TYPE. Adding LC_DECLARE_EVENT " \
                      "would give it a second, different event id. Keep exactly one.");    \
        static constexpr ::lc::EventTypeId Id() {                                         \
            return ::lc::EventTypeId(::lc::HashString(#QualifiedType));                   \
        }                                                                                 \
        static constexpr const char* Name() { return #QualifiedType; }                    \
    };                                                                                    \
    } /* namespace lc */

template <typename T>
constexpr EventTypeId EventTypeOf() { return EventTraits<T>::Id(); }

template <typename T>
constexpr const char* EventNameOf() { return EventTraits<T>::Name(); }

// ---------------------------------------------------------------- subscriptions
struct EventSubscriptionTag {};
using SubscriptionId = Id<EventSubscriptionTag>;

// Deliberately a raw function pointer plus opaque user data. std::function allocates and
// would violate I5; a virtual interface would drag RTTI-shaped design into core/.
using HandlerFn = void (*)(void* user, const void* event);

// Convenience for the first line of a handler. The bus memcpy'd a T into suitably aligned
// storage, which under C++20 implicitly created the object, so this cast is well defined.
template <typename T>
inline const T& EventAs(const void* e) {
    return *static_cast<const T*>(e);
}

// ---------------------------------------------------------------- the bus
class LC_API EventBus {
public:
    // A slot is fixed size so the ring is a flat array with O(1) indexing and a payload
    // whose bytes are trivially hashable. Events are small structs of integers; 64 bytes
    // is roomy. Exceeding it is a compile error, not a runtime surprise.
    static constexpr u32 kMaxEventPayloadSize   = 64;
    static constexpr u32 kDefaultCapacity       = 1024;
    static constexpr u32 kDefaultMaxSubscribers = 256;
    static constexpr u32 kDefaultMaxEventTypes  = 128;

    EventBus();
    EventBus(u32 capacity, u32 maxSubscribers, u32 maxEventTypes);
    ~EventBus();

    EventBus(const EventBus&)            = delete;
    EventBus& operator=(const EventBus&) = delete;

    // ---- publishing ------------------------------------------------------------------
    // Appends to the ring. Never allocates. On overflow this is LC_FATAL - it does not
    // return. The bool exists so call sites read the same way if a soft-fail policy is ever
    // introduced for a specific bus; under the current policy it is always true.
    template <typename T>
    bool Publish(const T& e) {
        static_assert(std::is_trivially_copyable_v<T>,
                      "event types must be trivially copyable PODs");
        static_assert(std::has_unique_object_representations_v<T>,
                      "event types must have no padding bytes and no floating point: queued "
                      "events are hashed byte-for-byte into the world hash. Reorder fields "
                      "largest-first or add an explicit zeroed pad member.");
        static_assert(sizeof(T) <= kMaxEventPayloadSize,
                      "event payload exceeds EventBus::kMaxEventPayloadSize");
        static_assert(alignof(T) <= 8, "event alignment must be <= 8");
        return PublishRaw(EventTraits<T>::Id(), EventTraits<T>::Name(), &e,
                          static_cast<u32>(sizeof(T)));
    }

    // Type-erased publish. `typeName` is used only for diagnostics and may be null.
    //
    // This is the escape hatch, and it is the ONE place where the compiler cannot help:
    // Publish<T>() static_asserts away padding and floats, but here the caller owns that
    // guarantee. These bytes go straight into the world hash, so handing this function a
    // struct with padding bytes makes the hash irreproducible run to run. Prefer Publish<T>.
    bool PublishRaw(EventTypeId type, const char* typeName, const void* data, u32 size);

    // ---- subscribing -----------------------------------------------------------------
    // Higher priority runs first. Equal priorities run in registration order.
    //
    // Unsubscribed slots are reclaimed lazily, so subscribe/unsubscribe churn does not
    // leak the table. The one case that cannot be reclaimed is a Subscribe() made from
    // inside a handler while the table is already full: compaction would move the entries
    // the running dispatch is indexing, so that is LC_FATAL instead. Size the table for
    // the peak, or subscribe outside the drain.
    SubscriptionId Subscribe(EventTypeId type, i32 priority, HandlerFn fn, void* user);

    // Typed form: identical, and additionally records the type's name and payload size so
    // diagnostics can name it and Drain() can catch a size mismatch.
    template <typename T>
    SubscriptionId Subscribe(i32 priority, HandlerFn fn, void* user) {
        RegisterEventType<T>();
        return Subscribe(EventTraits<T>::Id(), priority, fn, user);
    }

    // Optional: name a type up front, for a bus that is published to but not subscribed to.
    template <typename T>
    void RegisterEventType() {
        RegisterTypeRaw(EventTraits<T>::Id(), EventTraits<T>::Name(),
                        static_cast<u32>(sizeof(T)));
    }

    // Idempotent. An unknown or already-removed id is ignored.
    void Unsubscribe(SubscriptionId id);

    // ---- dispatch --------------------------------------------------------------------
    // Dispatches exactly the events queued when the drain began, in publish order.
    // Anything published by a handler stays queued for the next drain. Re-entering Drain()
    // from a handler is a fatal programming error.
    void Drain();

    // ---- state -----------------------------------------------------------------------
    u32  PendingCount() const { return count_; }
    u32  Capacity() const { return capacity_; }
    u32  SubscriberCount() const;              // live subscriptions only
    u64  TotalPublished() const { return totalPublished_; }
    u64  TotalDispatched() const { return totalDispatched_; }

    // Drops every queued event WITHOUT dispatching. For world reset and load, not for
    // ordinary flow control - dropping events silently in the tick loop is exactly the bug
    // the overflow policy exists to prevent. Subscriptions are untouched.
    void Clear();

    // Queued events are world state: two worlds are not equal if one has a fine pending.
    // Hashes the pending count, then per event the type id, the payload size, and exactly
    // `size` payload bytes - never the unused tail of a slot, never a `void* user`, never
    // a subscriber table (which is code wiring, not world state). The publish sequence
    // counter is deliberately excluded: it is diagnostics, not state.
    //
    // I7: the hash covers the queue's CONTENTS IN ORDER and nothing about where those
    // contents physically sit in the ring. A bus whose queue straddles the wrap hashes
    // identically to a freshly loaded bus holding the same events at head_ == 0, which is
    // what makes a save/load round trip compare equal. Locked down by a test.
    void HashInto(Hasher& h) const;

    // Diagnostics. Returns a registered name, or "<unregistered>". Never null.
    const char* TypeName(EventTypeId type) const;

private:
    struct Record {
        EventTypeId type;                      // 8 bytes, alignment 8
        u32         size;                      // payload bytes actually written
        u32         sequence;                  // publish ordinal, diagnostics only
        u8          payload[kMaxEventPayloadSize];
    };

    struct Sub {
        SubscriptionId id;
        EventTypeId    type;
        i32            priority;
        u32            regIndex;               // monotonic; the tie-break key
        HandlerFn      fn;
        void*          user;
        bool           alive;
    };

    struct TypeInfo {
        EventTypeId id;
        const char* name;
        u32         size;
    };

    // Contiguous run of `order_` entries belonging to one event type.
    struct TypeRange {
        EventTypeId type;
        u32         begin;
        u32         count;
        u32         payloadSize;               // 0 when the type was never named
    };

    void              Init(u32 capacity, u32 maxSubscribers, u32 maxEventTypes);
    void              RegisterTypeRaw(EventTypeId type, const char* name, u32 size);
    void              Rebuild();
    const TypeRange*  FindRange(EventTypeId type) const;
    u32               FindTypeSlot(EventTypeId type, bool& found) const;

    std::vector<Record>    records_;           // the ring, sized once
    std::vector<Sub>       subs_;              // registration order, with tombstones
    std::vector<u32>       order_;             // indices into subs_, sorted for dispatch
    std::vector<TypeRange> ranges_;            // per-type windows into order_
    std::vector<TypeInfo>  types_;             // sorted by id, for names and size checks

    u32  capacity_        = 0;
    u32  maxSubscribers_  = 0;
    u32  maxEventTypes_   = 0;
    u32  head_            = 0;                 // index of the oldest queued event
    u32  count_           = 0;                 // queued events
    u32  nextRegIndex_    = 0;
    u32  deadCount_       = 0;
    u64  totalPublished_  = 0;
    u64  totalDispatched_ = 0;
    bool dirty_           = false;             // order_/ranges_ need a rebuild
    bool draining_        = false;

    IdAllocator<SubscriptionId> ids_;
};

} // namespace lc
