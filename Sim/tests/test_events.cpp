// LIVING CITY - event bus invariant tests.
//
// These test the promises in Events.h that the rest of the sim is allowed to rely on:
// FIFO order, priority order that is independent of registration order, a stable tie-break,
// deferral of anything published inside a handler, zero allocation on the hot path, and a
// world hash that sees queued events. They are not getter tests.
//
// Several expectations are hard-coded known answers derived from this implementation's
// actual behaviour, so a refactor that quietly changes dispatch order or the hash of a
// queued event fails here instead of on tick 40,000 of a replay.

#include "TestFramework.h"

#include "livingcity/core/Events.h"

#include <cstring>
#include <string>

using namespace lc;

// A type registered NON-intrusively, to exercise the LC_DECLARE_EVENT path. Must sit in a
// named namespace at file scope: the macro specialises lc::EventTraits on the qualified name.
namespace lc_events_test {
struct Plain {
    u32 a;
    u32 b;
};
} // namespace lc_events_test

LC_DECLARE_EVENT(lc_events_test::Plain)

namespace {

// ---------------------------------------------------------------- event types under test
struct Alpha {
    LC_EVENT_TYPE(test::Alpha);
    u32 id;
    u32 value;
};

struct Beta {
    LC_EVENT_TYPE(test::Beta);
    u32 id;
    u32 value;
};

// Mixed field widths, explicitly padded so the byte representation is fully determined.
struct Gamma {
    LC_EVENT_TYPE(test::Gamma);
    i64 amount;
    u32 who;
    u32 pad_;
};

static_assert(std::is_trivially_copyable_v<Alpha>, "event types must be trivially copyable");
static_assert(std::has_unique_object_representations_v<Alpha>, "Alpha must have no padding");
static_assert(std::has_unique_object_representations_v<Gamma>, "Gamma must have no padding");
static_assert(sizeof(Gamma) == 16, "Gamma layout changed; the known-answer hash below moves");
static_assert(EventTypeOf<Alpha>() != EventTypeOf<Beta>(), "distinct names, distinct ids");
static_assert(EventTypeOf<Alpha>().IsValid(), "an event id must never be the reserved 0");
// Ids are compile-time constants: no RTTI, no static-initialisation order dependence.
static_assert(EventTypeOf<Alpha>() == EventTypeId(HashString("test::Alpha")), "id is stable");

// An event id must belong to the struct that declared it. Static members are inherited, so
// without the self-type probe in LC_EVENT_TYPE this derived struct would answer with
// test::Alpha's id and name - and being the same size, it would slide past the runtime
// collision check and be handed to Alpha's handlers as an Alpha. The guard turns that into
// a compile error, so here we can only assert the guard itself: instantiating
// EventTraits<InheritsAlphaId> (via EventTypeOf<>) is deliberately ill-formed.
struct InheritsAlphaId : Alpha {};
static_assert(detail::HasIntrusiveEventType<InheritsAlphaId>::value,
              "the base's static members really are visible through the derived type");
static_assert(!detail::EventTypeSelfMatches<InheritsAlphaId>::value,
              "an inherited LC_EVENT_TYPE must be rejected, not silently reused");
static_assert(detail::EventTypeSelfMatches<Alpha>::value, "Alpha declares its own id");
static_assert(detail::DeclaresOwnEventType<Alpha>::value, "Alpha declares its own id");
static_assert(!detail::DeclaresOwnEventType<InheritsAlphaId>::value,
              "a derived type declares nothing; LC_DECLARE_EVENT is its escape hatch");

// The probe is a declaration only: it must not disturb layout, triviality or aggregates.
static_assert(sizeof(Alpha) == 8, "LC_EVENT_TYPE must not change the struct's size");
static_assert(std::is_aggregate_v<Alpha>, "Alpha{1, 2} must keep working");
static_assert(std::is_standard_layout_v<Alpha>, "event structs stay standard layout");

// ---------------------------------------------------------------- recording helpers
// Fixed capacity on purpose: the recorder itself must not allocate inside a measured drain.
constexpr u32 kTraceCap = 512;

struct Trace {
    char marks[kTraceCap] = {};
    u32  n                = 0;

    void Push(char c) {
        LC_ASSERT_MSG(n < kTraceCap, "test trace overflow");
        marks[n++] = c;
    }
    void Reset() { n = 0; }
    bool Equals(const char* s) const {
        const u32 len = static_cast<u32>(std::strlen(s));
        return len == n && std::memcmp(marks, s, static_cast<std::size_t>(n)) == 0;
    }
    std::string Str() const { return std::string(marks, marks + n); }  // failure messages only
};

// user data for a handler that just leaves a fingerprint
struct Tap {
    Trace* trace;
    char   tag;
};

void TapHandler(void* user, const void* /*event*/) {
    Tap* t = static_cast<Tap*>(user);
    t->trace->Push(t->tag);
}

void RecordAlphaId(void* user, const void* event) {
    Trace*       t = static_cast<Trace*>(user);
    const Alpha& a = EventAs<Alpha>(event);
    t->Push(static_cast<char>('0' + static_cast<int>(a.id % 10u)));
}

void RecordBetaId(void* user, const void* event) {
    Trace*      t = static_cast<Trace*>(user);
    const Beta& b = EventAs<Beta>(event);
    t->Push(static_cast<char>('a' + static_cast<int>(b.id % 26u)));
}

// Reads the in-flight event, publishes a fresh one, then reads the in-flight event AGAIN.
// On a nearly full ring the new event lands in the slot the delivered one came out of, so
// the two reads only agree if Drain() really did hand the handler a private copy.
struct Republisher1 {
    EventBus* bus;
    Trace*    trace;
    u32       remaining;
};

void OnAlphaEchoThenRepublish(void* user, const void* event) {
    Republisher1* r  = static_cast<Republisher1*>(user);
    const u32     id = EventAs<Alpha>(event).id;
    r->trace->Push(static_cast<char>('0' + static_cast<int>(id % 10u)));
    if (r->remaining > 0u) {
        --r->remaining;
        r->bus->Publish(Alpha{id + 1u, 0});
    }
    r->trace->Push(static_cast<char>('0' + static_cast<int>(EventAs<Alpha>(event).id % 10u)));
}

} // namespace

// ---------------------------------------------------------------------------------------
// FIFO
// ---------------------------------------------------------------------------------------
LC_TEST(event_bus_dispatches_in_publish_order) {
    EventBus bus(64, 16, 8);
    Trace    trace;
    bus.Subscribe<Alpha>(0, &RecordAlphaId, &trace);

    bus.Publish(Alpha{1, 100});
    bus.Publish(Alpha{2, 200});
    bus.Publish(Alpha{3, 300});
    LC_CHECK_EQ(bus.PendingCount(), 3u);

    bus.Drain();
    LC_CHECK_EQ(bus.PendingCount(), 0u);
    LC_CHECK_EQ(trace.Str(), std::string("123"));

    // FIFO holds across drains, and across repeated laps of the ring. (This loop drains
    // between publishes, so the queue itself never straddles the wrap - that harder case
    // is event_bus_ring_wrap_preserves_order_and_hash below.)
    EventBus small(4, 4, 4);
    Trace    wrapped;
    small.Subscribe<Alpha>(0, &RecordAlphaId, &wrapped);
    for (u32 round = 0; round < 5u; ++round) {
        small.Publish(Alpha{round * 2u + 1u, 0});
        small.Publish(Alpha{round * 2u + 2u, 0});
        small.Drain();
    }
    LC_CHECK_EQ(wrapped.Str(), std::string("1234567890"));
    LC_CHECK_EQ(small.TotalDispatched(), 10ull);

    // An event with no subscribers is still consumed, in order, and never leaks.
    small.Publish(Beta{1, 0});
    LC_CHECK_EQ(small.PendingCount(), 1u);
    small.Drain();
    LC_CHECK_EQ(small.PendingCount(), 0u);
}

// ---------------------------------------------------------------------------------------
// Ring wrap - a queue whose events straddle the end of the buffer
//
// The loop above always drains before the queue can span the wrap, so it never exercises
// the modular indexing in PublishRaw/Drain/HashInto with a genuinely split queue. This one
// does, and it pins the I7 property the save/load round trip depends on: the hash sees the
// queue's CONTENTS IN ORDER and nothing about where they physically sit in the ring.
// ---------------------------------------------------------------------------------------
LC_TEST(event_bus_ring_wrap_preserves_order_and_hash) {
    EventBus bus(4, 4, 4);
    Trace    trace;
    bus.Subscribe<Alpha>(0, &RecordAlphaId, &trace);

    // Advance head_ to 3 of 4, so the next three events occupy slots 3, 0, 1.
    bus.Publish(Alpha{1, 0});
    bus.Publish(Alpha{2, 0});
    bus.Publish(Alpha{3, 0});
    bus.Drain();
    LC_CHECK_EQ(trace.Str(), std::string("123"));

    bus.Publish(Alpha{4, 0});
    bus.Publish(Alpha{5, 0});
    bus.Publish(Alpha{6, 0});
    LC_CHECK_EQ(bus.PendingCount(), 3u);

    // A wrapped queue must hash exactly like the same three events on a fresh bus, whose
    // head_ is 0. If HashInto ever hashed slot positions, or walked the ring linearly
    // instead of from head_, this is where it would show.
    Hasher hWrapped;
    bus.HashInto(hWrapped);

    EventBus fresh(4, 4, 4);
    fresh.Publish(Alpha{4, 0});
    fresh.Publish(Alpha{5, 0});
    fresh.Publish(Alpha{6, 0});
    Hasher hFresh;
    fresh.HashInto(hFresh);
    LC_CHECK_EQ(hWrapped.Value(), hFresh.Value());

    // ...and it must dispatch in publish order, not slot order (slot order would be "564").
    trace.Reset();
    bus.Drain();
    LC_CHECK_EQ(trace.Str(), std::string("456"));

    // Filled to EXACTLY capacity: the last slot before overflow must still be usable, and
    // a full lap must leave head_ where it started.
    EventBus brim(8, 4, 4);
    Trace    brimTrace;
    brim.Subscribe<Alpha>(0, &RecordAlphaId, &brimTrace);
    for (u32 i = 1; i <= 8u; ++i) brim.Publish(Alpha{i, 0});
    LC_CHECK_EQ(brim.PendingCount(), brim.Capacity());
    brim.Drain();
    LC_CHECK_EQ(brimTrace.Str(), std::string("12345678"));
    brimTrace.Reset();
    for (u32 i = 1; i <= 8u; ++i) brim.Publish(Alpha{i, 0});
    LC_CHECK_EQ(brim.PendingCount(), 8u);
    brim.Drain();
    LC_CHECK_EQ(brimTrace.Str(), std::string("12345678"));

    // Clear() on a wrapped queue must land on the pristine-bus hash, not on a half-reset
    // ring that still reports leftovers.
    EventBus wrapped(4, 4, 4);
    wrapped.Publish(Alpha{1, 0});
    wrapped.Publish(Alpha{2, 0});
    wrapped.Publish(Alpha{3, 0});
    wrapped.Drain();
    wrapped.Publish(Alpha{4, 0});
    wrapped.Publish(Alpha{5, 0});
    wrapped.Publish(Alpha{6, 0});
    wrapped.Clear();
    LC_CHECK_EQ(wrapped.PendingCount(), 0u);
    Hasher afterClear;
    wrapped.HashInto(afterClear);
    EventBus pristineBus(4, 4, 4);
    Hasher   pristine;
    pristineBus.HashInto(pristine);
    LC_CHECK_EQ(afterClear.Value(), pristine.Value());

    // A capacity-1 ring is the worst case for the copy-before-dispatch rule: the handler
    // republishes into the very slot the event it is holding was just read from.
    EventBus one(1, 4, 4);
    Trace    oneTrace;
    Republisher1 rep{&one, &oneTrace, 3u};
    one.Subscribe<Alpha>(0, &OnAlphaEchoThenRepublish, &rep);
    one.Publish(Alpha{1, 0});
    for (int d = 0; d < 4; ++d) one.Drain();
    // Each handler run stamps the id, republishes, then stamps the id AGAIN: if publishing
    // could alias the in-flight event the second stamp would differ from the first.
    LC_CHECK_EQ(oneTrace.Str(), std::string("11223344"));
}

// ---------------------------------------------------------------------------------------
// Priority - high to low, whatever order the subscriptions were made in
// ---------------------------------------------------------------------------------------
LC_TEST(event_bus_priority_order_ignores_registration_order) {
    // Registered mid, low, high - deliberately scrambled.
    EventBus busA(32, 8, 4);
    Trace    ta;
    Tap      midA{&ta, 'M'}, lowA{&ta, 'L'}, highA{&ta, 'H'};
    busA.Subscribe<Alpha>(5, &TapHandler, &midA);
    busA.Subscribe<Alpha>(1, &TapHandler, &lowA);
    busA.Subscribe<Alpha>(10, &TapHandler, &highA);
    busA.Publish(Alpha{1, 0});
    busA.Drain();
    LC_CHECK_EQ(ta.Str(), std::string("HML"));

    // Same three priorities, registered in a different order: identical dispatch order.
    EventBus busB(32, 8, 4);
    Trace    tb;
    Tap      highB{&tb, 'H'}, midB{&tb, 'M'}, lowB{&tb, 'L'};
    busB.Subscribe<Alpha>(10, &TapHandler, &highB);
    busB.Subscribe<Alpha>(5, &TapHandler, &midB);
    busB.Subscribe<Alpha>(1, &TapHandler, &lowB);
    busB.Publish(Alpha{1, 0});
    busB.Drain();
    LC_CHECK_EQ(tb.Str(), std::string("HML"));

    // ...and a third order, with a negative priority proving i32 is compared as signed.
    EventBus busC(32, 8, 4);
    Trace    tc;
    Tap      lowC{&tc, 'L'}, negC{&tc, 'N'}, highC{&tc, 'H'}, midC{&tc, 'M'};
    busC.Subscribe<Alpha>(1, &TapHandler, &lowC);
    busC.Subscribe<Alpha>(-7, &TapHandler, &negC);
    busC.Subscribe<Alpha>(10, &TapHandler, &highC);
    busC.Subscribe<Alpha>(5, &TapHandler, &midC);
    busC.Publish(Alpha{1, 0});
    busC.Drain();
    LC_CHECK_EQ(tc.Str(), std::string("HMLN"));

    // Priority partitions per event type; it does not leak across types.
    Trace tmix;
    Tap   alphaHi{&tmix, 'A'}, betaLo{&tmix, 'b'};
    EventBus busD(32, 8, 4);
    busD.Subscribe<Beta>(-100, &TapHandler, &betaLo);
    busD.Subscribe<Alpha>(1, &TapHandler, &alphaHi);
    busD.Publish(Beta{1, 0});
    busD.Publish(Alpha{1, 0});
    busD.Drain();
    LC_CHECK_EQ(tmix.Str(), std::string("bA"));  // publish order wins between types
}

// ---------------------------------------------------------------------------------------
// Tie-break - equal priority runs in registration order, identically on a fresh bus
// ---------------------------------------------------------------------------------------
LC_TEST(event_bus_equal_priority_breaks_ties_by_registration_order) {
    Trace first;
    {
        EventBus bus(32, 8, 4);
        Tap      a{&first, 'A'}, b{&first, 'B'}, c{&first, 'C'};
        bus.Subscribe<Alpha>(7, &TapHandler, &a);
        bus.Subscribe<Alpha>(7, &TapHandler, &b);
        bus.Subscribe<Alpha>(7, &TapHandler, &c);
        bus.Publish(Alpha{1, 0});
        bus.Drain();
    }
    LC_CHECK_EQ(first.Str(), std::string("ABC"));

    // A second, freshly built bus given the same script must agree byte for byte.
    Trace second;
    {
        EventBus bus(32, 8, 4);
        Tap      a{&second, 'A'}, b{&second, 'B'}, c{&second, 'C'};
        bus.Subscribe<Alpha>(7, &TapHandler, &a);
        bus.Subscribe<Alpha>(7, &TapHandler, &b);
        bus.Subscribe<Alpha>(7, &TapHandler, &c);
        bus.Publish(Alpha{1, 0});
        bus.Drain();
    }
    LC_CHECK_EQ(second.n, first.n);
    LC_CHECK_EQ(std::memcmp(first.marks, second.marks, kTraceCap), 0);

    // Ties nested inside a priority ordering: high tier keeps its own registration order.
    Trace nested;
    {
        EventBus bus(32, 8, 4);
        Tap      p{&nested, 'p'}, q{&nested, 'q'}, r{&nested, 'r'}, s{&nested, 's'};
        bus.Subscribe<Alpha>(2, &TapHandler, &p);   // reg 0
        bus.Subscribe<Alpha>(9, &TapHandler, &q);   // reg 1
        bus.Subscribe<Alpha>(2, &TapHandler, &r);   // reg 2
        bus.Subscribe<Alpha>(9, &TapHandler, &s);   // reg 3
        bus.Publish(Alpha{1, 0});
        bus.Drain();
    }
    LC_CHECK_EQ(nested.Str(), std::string("qspr"));
}

// ---------------------------------------------------------------------------------------
// Determinism - an identical scripted sequence produces identical dispatches and hashes
// ---------------------------------------------------------------------------------------
namespace {

// Runs a fixed script of subscribe / publish / unsubscribe / drain operations. Everything it
// touches is integer state; nothing reads a clock or a pointer value.
void RunScript(Trace& trace, u64& outHash) {
    Hasher   h;
    EventBus bus(128, 32, 8);

    Tap alphaMid{&trace, 'a'};
    Tap betaOnly{&trace, 'B'};
    Tap alphaHigh{&trace, 'A'};
    Tap alphaTie{&trace, 'c'};

    const SubscriptionId sMid = bus.Subscribe<Alpha>(3, &TapHandler, &alphaMid);   // reg 0
    bus.Subscribe<Beta>(3, &TapHandler, &betaOnly);                                // reg 1
    bus.Subscribe<Alpha>(9, &TapHandler, &alphaHigh);                              // reg 2
    bus.Subscribe<Alpha>(3, &TapHandler, &alphaTie);                               // reg 3

    bus.Publish(Alpha{1, 11});
    bus.Publish(Beta{2, 22});
    bus.Publish(Alpha{3, 33});
    bus.HashInto(h);                     // checkpoint with three events queued
    h.U32(bus.PendingCount());

    bus.Drain();
    bus.HashInto(h);                     // checkpoint with an empty queue

    bus.Unsubscribe(sMid);
    bus.Publish(Beta{4, 44});
    bus.Publish(Alpha{5, 55});
    bus.HashInto(h);
    bus.Drain();

    bus.Publish(Gamma{-1234567890123ll, 7, 0});
    bus.HashInto(h);
    bus.Drain();

    h.U32(bus.SubscriberCount());
    h.U64(bus.TotalPublished());
    h.U64(bus.TotalDispatched());
    outHash = h.Value();
}

} // namespace

LC_TEST(event_bus_identical_scripts_produce_identical_dispatch) {
    Trace traceA;
    Trace traceB;
    u64   hashA = 0;
    u64   hashB = 0;

    RunScript(traceA, hashA);
    RunScript(traceB, hashB);

    // Byte-identical dispatch record, and byte-identical state hash.
    LC_CHECK_EQ(traceA.n, traceB.n);
    LC_CHECK_EQ(std::memcmp(traceA.marks, traceB.marks, kTraceCap), 0);
    LC_CHECK_EQ(hashA, hashB);

    // Known answers, derived from this implementation's actual behaviour:
    //   Alpha subscribers sort to A(pri 9), a(pri 3, reg 0), c(pri 3, reg 3).
    //   drain 1: Alpha -> "Aac", Beta -> "B", Alpha -> "Aac"
    //   drain 2 (after 'a' unsubscribed): Beta -> "B", Alpha -> "Ac"
    //   drain 3: Gamma has no subscriber -> nothing
    LC_CHECK_EQ(traceA.Str(), std::string("AacBAacBAc"));
    LC_CHECK_EQ(hashA, 0x50d7b6dae415aea9ull);
}

// ---------------------------------------------------------------------------------------
// Re-entrancy - publishing from a handler defers to the NEXT drain
// ---------------------------------------------------------------------------------------
namespace {

struct Republisher {
    EventBus* bus;
    Trace*    trace;
    u32       remaining;
};

void OnAlphaRepublish(void* user, const void* /*event*/) {
    Republisher* r = static_cast<Republisher*>(user);
    r->trace->Push('A');
    if (r->remaining > 0u) {
        --r->remaining;
        r->bus->Publish(Beta{1, 0});
    }
}

} // namespace

LC_TEST(event_bus_publish_inside_handler_defers_to_next_drain) {
    EventBus    bus(64, 8, 4);
    Trace       trace;
    Republisher rep{&bus, &trace, 1u};

    bus.Subscribe<Alpha>(0, &OnAlphaRepublish, &rep);
    bus.Subscribe<Beta>(0, &RecordBetaId, &trace);

    bus.Publish(Alpha{1, 0});
    bus.Drain();

    // The Beta published from inside the handler was NOT delivered by this drain...
    LC_CHECK_EQ(trace.Str(), std::string("A"));
    LC_CHECK_EQ(bus.PendingCount(), 1u);

    // ...and IS delivered by the next one.
    bus.Drain();
    LC_CHECK_EQ(trace.Str(), std::string("Ab"));
    LC_CHECK_EQ(bus.PendingCount(), 0u);

    // The drain window is snapshotted, so handler behaviour cannot reorder a drain that is
    // already in flight: two queued Alphas both run before either republished Beta.
    trace.Reset();
    rep.remaining = 2u;
    bus.Publish(Alpha{1, 0});
    bus.Publish(Alpha{2, 0});
    bus.Drain();
    LC_CHECK_EQ(trace.Str(), std::string("AA"));
    LC_CHECK_EQ(bus.PendingCount(), 2u);
    bus.Drain();
    LC_CHECK_EQ(trace.Str(), std::string("AAbb"));
}

// ---------------------------------------------------------------------------------------
// Re-entrancy - the subscriber table is frozen for the duration of a drain
// ---------------------------------------------------------------------------------------
namespace {

struct LateJoiner {
    EventBus* bus;
    Trace*    trace;
    Tap*      tap;
    bool      done;
};

void OnAlphaSubscribeLate(void* user, const void* /*event*/) {
    LateJoiner* j = static_cast<LateJoiner*>(user);
    j->trace->Push('A');
    if (!j->done) {
        j->done = true;
        // Highest priority: if the table were live, this would jump ahead of us next event.
        j->bus->Subscribe<Alpha>(100, &TapHandler, j->tap);
    }
}

} // namespace

LC_TEST(event_bus_subscription_made_during_drain_starts_at_next_drain) {
    EventBus   bus(64, 8, 4);
    Trace      trace;
    Tap        late{&trace, 'L'};
    LateJoiner joiner{&bus, &trace, &late, false};

    bus.Subscribe<Alpha>(0, &OnAlphaSubscribeLate, &joiner);

    bus.Publish(Alpha{1, 0});
    bus.Publish(Alpha{2, 0});
    bus.Drain();
    LC_CHECK_EQ(trace.Str(), std::string("AA"));   // not "AALA": the table was frozen
    LC_CHECK_EQ(bus.SubscriberCount(), 2u);

    trace.Reset();
    bus.Publish(Alpha{3, 0});
    bus.Drain();
    LC_CHECK_EQ(trace.Str(), std::string("LA"));   // now it is live, and it sorts first
}

// ---------------------------------------------------------------------------------------
// Unsubscribe - removes exactly one handler, leaves the rest in order
// ---------------------------------------------------------------------------------------
LC_TEST(event_bus_unsubscribe_removes_exactly_one_handler) {
    EventBus bus(64, 16, 8);
    Trace    trace;
    Tap      a{&trace, 'A'}, b{&trace, 'B'}, c{&trace, 'C'}, d{&trace, 'D'};

    bus.Subscribe<Alpha>(0, &TapHandler, &a);
    const SubscriptionId idB = bus.Subscribe<Alpha>(0, &TapHandler, &b);
    bus.Subscribe<Alpha>(0, &TapHandler, &c);
    bus.Subscribe<Alpha>(0, &TapHandler, &d);
    LC_CHECK_EQ(bus.SubscriberCount(), 4u);

    bus.Publish(Alpha{1, 0});
    bus.Drain();
    LC_CHECK_EQ(trace.Str(), std::string("ABCD"));

    trace.Reset();
    bus.Unsubscribe(idB);
    LC_CHECK_EQ(bus.SubscriberCount(), 3u);
    bus.Publish(Alpha{1, 0});
    bus.Drain();
    LC_CHECK_EQ(trace.Str(), std::string("ACD"));   // exactly one gone, order undisturbed

    // Idempotent, and immune to ids it never issued.
    trace.Reset();
    bus.Unsubscribe(idB);
    bus.Unsubscribe(SubscriptionId(99999u));
    bus.Unsubscribe(SubscriptionId{});
    LC_CHECK_EQ(bus.SubscriberCount(), 3u);
    bus.Publish(Alpha{1, 0});
    bus.Drain();
    LC_CHECK_EQ(trace.Str(), std::string("ACD"));

    // Ids are never reused, so a resubscription lands at the END of the tie group.
    trace.Reset();
    Tap e{&trace, 'E'};
    const SubscriptionId idE = bus.Subscribe<Alpha>(0, &TapHandler, &e);
    LC_CHECK_NE(idE, idB);
    bus.Publish(Alpha{1, 0});
    bus.Drain();
    LC_CHECK_EQ(trace.Str(), std::string("ACDE"));
}

// ---------------------------------------------------------------------------------------
// Subscribe/unsubscribe churn does not leak the fixed-size subscriber table
//
// The table is sized once and never grows, so a system that resubscribes every tick would
// hit the hard cap within seconds unless tombstones are reclaimed. This runs far more
// subscriptions than the table can hold, with only one alive at a time.
// ---------------------------------------------------------------------------------------
LC_TEST(event_bus_subscriber_table_survives_churn_past_its_capacity) {
    EventBus bus(16, 2, 4);          // room for TWO entries, total
    Trace    trace;
    Tap      tap{&trace, 'A'};

    SubscriptionId live = bus.Subscribe<Alpha>(0, &TapHandler, &tap);
    SubscriptionId previous = live;
    for (u32 i = 0; i < 200u; ++i) {
        live = bus.Subscribe<Alpha>(0, &TapHandler, &tap);   // forces reclaim of the dead one
        bus.Unsubscribe(previous);
        previous = live;
        LC_CHECK_EQ(bus.SubscriberCount(), 1u);
        LC_CHECK_NE(live, SubscriptionId{});
    }

    // ...and the survivor still works, exactly once.
    bus.Publish(Alpha{1, 0});
    bus.Drain();
    LC_CHECK_EQ(trace.Str(), std::string("A"));

    // Ids are never recycled, so the 201st id is well past the table's two slots.
    LC_CHECK(live.value > 2u);
}

// ---------------------------------------------------------------------------------------
// Unsubscribe from inside a handler takes effect immediately
// ---------------------------------------------------------------------------------------
namespace {

struct SelfCanceller {
    EventBus*      bus;
    Trace*         trace;
    SubscriptionId victim;
};

void OnAlphaCancelVictim(void* user, const void* /*event*/) {
    SelfCanceller* s = static_cast<SelfCanceller*>(user);
    s->trace->Push('X');
    s->bus->Unsubscribe(s->victim);
}

} // namespace

LC_TEST(event_bus_unsubscribe_during_drain_takes_effect_immediately) {
    EventBus      bus(64, 8, 4);
    Trace         trace;
    Tap           victim{&trace, 'V'};
    SelfCanceller canceller{&bus, &trace, SubscriptionId{}};

    // 'X' runs first (higher priority) and cancels 'V' before 'V' would have run.
    canceller.victim = bus.Subscribe<Alpha>(1, &TapHandler, &victim);
    bus.Subscribe<Alpha>(5, &OnAlphaCancelVictim, &canceller);

    bus.Publish(Alpha{1, 0});
    bus.Drain();
    LC_CHECK_EQ(trace.Str(), std::string("X"));
    LC_CHECK_EQ(bus.SubscriberCount(), 1u);
}

// ---------------------------------------------------------------------------------------
// Zero allocation on the steady-state path (I5)
// ---------------------------------------------------------------------------------------
LC_TEST(event_bus_publish_and_drain_allocate_nothing) {
    EventBus bus(512, 16, 8);
    Trace    trace;
    Tap      betaTap{&trace, 'b'};

    bus.Subscribe<Alpha>(0, &RecordAlphaId, &trace);
    bus.Subscribe<Beta>(5, &TapHandler, &betaTap);

    // Warm up: force the subscriber table and per-type ranges to be built and every type
    // to be registered, so the measured window sees only steady-state work.
    bus.Publish(Alpha{1, 0});
    bus.Publish(Beta{1, 0});
    bus.Drain();
    bus.Drain();
    trace.Reset();

    if (!AllocTrackingEnabled()) {
        return;  // tracking compiled out (e.g. inside an Unreal module) - nothing to assert
    }

    const AllocStats before = GetAllocStats();

    Hasher h;
    for (u32 i = 0; i < 100u; ++i) {
        bus.Publish(Alpha{i, i * 7u});
        bus.Publish(Beta{i, i});
    }
    bus.HashInto(h);
    bus.Drain();

    const AllocStats after = GetAllocStats();

    LC_CHECK_EQ(after.allocations, before.allocations);
    LC_CHECK_EQ(after.bytes, before.bytes);
    LC_CHECK_EQ(trace.n, 200u);
    LC_CHECK_NE(h.Value(), Hasher{}.Value());
}

// ---------------------------------------------------------------------------------------
// The world hash sees queued events
// ---------------------------------------------------------------------------------------
LC_TEST(event_bus_hash_covers_queued_events) {
    EventBus a(64, 8, 4);
    EventBus b(64, 8, 4);

    Hasher emptyA, emptyB;
    a.HashInto(emptyA);
    b.HashInto(emptyB);
    LC_CHECK_EQ(emptyA.Value(), emptyB.Value());

    // A queued event changes the hash.
    a.Publish(Alpha{1, 2});
    Hasher oneA;
    a.HashInto(oneA);
    LC_CHECK_NE(oneA.Value(), emptyA.Value());

    // The same queue on another bus hashes the same.
    b.Publish(Alpha{1, 2});
    Hasher oneB;
    b.HashInto(oneB);
    LC_CHECK_EQ(oneA.Value(), oneB.Value());

    // A different payload hashes differently...
    EventBus c(64, 8, 4);
    c.Publish(Alpha{1, 3});
    Hasher oneC;
    c.HashInto(oneC);
    LC_CHECK_NE(oneA.Value(), oneC.Value());

    // ...and so does the same payload under a different event type id.
    EventBus d(64, 8, 4);
    d.Publish(Beta{1, 2});
    Hasher oneD;
    d.HashInto(oneD);
    LC_CHECK_NE(oneA.Value(), oneD.Value());

    // Draining empties the queue, so the hash returns to the empty-bus value. Subscribers
    // are wiring, not world state, and never enter the hash.
    Trace trace;
    Tap   tap{&trace, 'A'};
    a.Subscribe<Alpha>(3, &TapHandler, &tap);
    a.Drain();
    Hasher drained;
    a.HashInto(drained);
    LC_CHECK_EQ(drained.Value(), emptyA.Value());

    // Clear() drops queued events without dispatching, and lands on the same hash.
    b.Clear();
    LC_CHECK_EQ(b.PendingCount(), 0u);
    Hasher cleared;
    b.HashInto(cleared);
    LC_CHECK_EQ(cleared.Value(), emptyB.Value());

    // Known answer: a specific queue hashes to a specific value, forever. (Hasher::Bytes
    // walks raw integer bytes, so this constant is little-endian x64 like every other world
    // hash in the sim - see Core.h. If this ever needs to be portable, the fix belongs in
    // Hasher, not here.)
    EventBus kat(64, 8, 4);
    kat.Publish(Alpha{1, 2});
    kat.Publish(Gamma{-1234567890123ll, 7, 0});
    kat.Publish(Beta{9, 9});
    Hasher katHash;
    kat.HashInto(katHash);
    LC_CHECK_EQ(kat.PendingCount(), 3u);
    LC_CHECK_EQ(katHash.Value(), 0x9ebf23bcf6328955ull);
}

// ---------------------------------------------------------------------------------------
// Non-intrusive registration (LC_DECLARE_EVENT) behaves exactly like the intrusive form
// ---------------------------------------------------------------------------------------
namespace {

void RecordPlain(void* user, const void* event) {
    Trace* t = static_cast<Trace*>(user);
    t->Push(static_cast<char>('0' + static_cast<int>(EventAs<lc_events_test::Plain>(event).a % 10u)));
}

} // namespace

LC_TEST(event_bus_non_intrusive_event_registration_works) {
    static_assert(EventTypeOf<lc_events_test::Plain>()
                      == EventTypeId(HashString("lc_events_test::Plain")),
                  "LC_DECLARE_EVENT must hash the fully qualified name it was given");
    static_assert(EventTypeOf<lc_events_test::Plain>() != EventTypeOf<Alpha>(),
                  "distinct names, distinct ids");

    // A type registered non-intrusively must not also be claiming an intrusive id: two
    // macros on one type means two different ids for one struct.
    static_assert(!detail::HasIntrusiveEventType<lc_events_test::Plain>::value,
                  "LC_DECLARE_EVENT is for types that carry no LC_EVENT_TYPE");

    EventBus bus(32, 8, 4);
    Trace    trace;
    bus.Subscribe<lc_events_test::Plain>(0, &RecordPlain, &trace);

    bus.Publish(lc_events_test::Plain{4, 0});
    bus.Publish(lc_events_test::Plain{5, 0});
    bus.Drain();

    LC_CHECK_EQ(trace.Str(), std::string("45"));
    LC_CHECK_EQ(std::string(bus.TypeName(EventTypeOf<lc_events_test::Plain>())),
                std::string("lc_events_test::Plain"));
    LC_CHECK_EQ(std::string(bus.TypeName(EventTypeId(1234u))), std::string("<unregistered>"));
}
