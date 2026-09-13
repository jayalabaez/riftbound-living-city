// Tests for lc::Money and lc::Ledger.
//
// The headline is INVARIANT I3: total money is conserved exactly, and money is created or
// destroyed only by the central bank. Everything here exists to attack that:
//   * ten thousand pseudo-random transfers, with conservation checked after EVERY single
//     one, including the ones that get rejected;
//   * every rejection path checked for side effects, because a transfer that half-applies
//     before failing is exactly how a conservation bug ships;
//   * OpenAccount with an opening balance checked to have MINTED that balance rather than
//     conjured it, which is the one place conservation can be violated at birth;
//   * ApplyRate checked for exactness and for sign symmetry, since a rounding rule that is
//     not symmetric quietly transfers value every time a negative amount is taxed;
//   * the random run checked against an independent shadow model of every balance, not just
//     against conservation - a Transfer that moved nothing, or moved to the wrong account,
//     conserves perfectly and would otherwise pass;
//   * the account-id boundary, because valid ids are 1..AccountCount() and the id that
//     breaks a dense one-based vector is the one immediately past the end;
//   * the i64 extremes of Money itself, where the 128-bit intermediate in ApplyRate is the
//     only thing standing between a near-limit balance and a wrapped fortune;
//   * I5, measured rather than asserted in a comment: the steady-state ledger paths must
//     allocate nothing at all.
//
// The random source is a local integer LCG rather than lc::Rng: this test must have no
// cross-unit dependency, so a change in the RNG cannot change what this test exercises.
// No float or double appears in this file.

#include "TestFramework.h"
#include "livingcity/core/Money.h"

#include <type_traits>

namespace {

using lc::AccountId;
using lc::i64;
using lc::Ledger;
using lc::Money;
using lc::u32;
using lc::u64;

// Structural guarantees, checked by the compiler rather than at run time. These are the
// properties that make I3 hard to violate by accident; if any of them is ever relaxed, the
// build stops here.
static_assert(!std::is_convertible_v<i64, Money>,
              "an i64 must never implicitly become Money");
static_assert(!std::is_convertible_v<Money, i64>,
              "Money must never implicitly decay to a number");
static_assert(!std::is_convertible_v<Money, bool>,
              "Money must never implicitly decay to bool");
static_assert(std::is_trivially_copyable_v<Money>,
              "Money must stay memcpy-able for snapshots and save files");
// Money * Money and Money / Money are deleted in the header, so they cannot be tested here
// - a test that tried would simply fail to compile, which is the entire point.

// Local LCG (Knuth/MMIX constants). Unsigned arithmetic, so the wraparound is defined and
// this sequence is identical on every compiler and every machine, forever.
struct Lcg {
    u64 s;

    explicit Lcg(u64 seed) : s(seed) {}

    u64 Next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return s >> 33;   // discard the low bits: they have short periods in an LCG
    }

    u32 Below(u32 n) { return static_cast<u32>(Next() % static_cast<u64>(n)); }
};

} // namespace

// ---------------------------------------------------------------------------------------
LC_TEST(money_construction_and_accessors) {
    LC_CHECK_EQ(Money::Zero().Raw(), static_cast<i64>(0));
    LC_CHECK_EQ(Money(1234).Raw(), static_cast<i64>(1234));
    LC_CHECK_EQ(Money::FromMajor(12).Raw(), static_cast<i64>(1200));
    LC_CHECK_EQ(Money::FromMajor(-12).Raw(), static_cast<i64>(-1200));
    LC_CHECK_EQ(Money::FromMajor(0).Raw(), static_cast<i64>(0));

    LC_CHECK(Money::Zero().IsZero());
    LC_CHECK(Money(-1).IsNegative());
    LC_CHECK(Money(1).IsPositive());
    LC_CHECK_FALSE(Money::Zero().IsNegative());
    LC_CHECK_FALSE(Money::Zero().IsPositive());

    // Major/minor split is taken from the magnitude, so a negative amount does not report a
    // negative minor part.
    LC_CHECK_EQ(Money(-105).MajorPart(), static_cast<i64>(-1));
    LC_CHECK_EQ(Money(-105).MinorPart(), static_cast<i64>(5));
    LC_CHECK_EQ(Money(105).MajorPart(), static_cast<i64>(1));
    LC_CHECK_EQ(Money(105).MinorPart(), static_cast<i64>(5));
}

// ---------------------------------------------------------------------------------------
LC_TEST(money_addition_subtraction_and_ordering) {
    const Money a(700);
    const Money b(-250);

    LC_CHECK_EQ((a + b).Raw(), static_cast<i64>(450));
    LC_CHECK_EQ((b + a).Raw(), static_cast<i64>(450));       // commutative
    LC_CHECK_EQ((a - b).Raw(), static_cast<i64>(950));
    LC_CHECK_EQ((a + (-b)).Raw(), (a - b).Raw());
    LC_CHECK_EQ((-(-a)).Raw(), a.Raw());
    LC_CHECK_EQ((a + Money::Zero()).Raw(), a.Raw());
    LC_CHECK_EQ((a - a).Raw(), static_cast<i64>(0));

    const Money c(13);
    LC_CHECK_EQ(((a + b) + c).Raw(), (a + (b + c)).Raw());   // associative

    Money acc = a;
    acc += b;
    acc -= c;
    LC_CHECK_EQ(acc.Raw(), ((a + b) - c).Raw());

    LC_CHECK(b < a);
    LC_CHECK(a > b);
    LC_CHECK(a >= a);
    LC_CHECK(a <= a);
    LC_CHECK(a == Money(700));
    LC_CHECK(a != b);
    LC_CHECK_EQ(Money::Min(a, b).Raw(), b.Raw());
    LC_CHECK_EQ(Money::Max(a, b).Raw(), a.Raw());
    LC_CHECK_EQ(b.Abs().Raw(), static_cast<i64>(250));
}

// ---------------------------------------------------------------------------------------
// ApplyRate is exact integer arithmetic, rounding HALF AWAY FROM ZERO on the magnitude.
LC_TEST(money_apply_rate_is_exact) {
    // Exact cases - no rounding involved at all.
    LC_CHECK_EQ(Money(100).ApplyRate(10000).Raw(), static_cast<i64>(100));   // 100%
    LC_CHECK_EQ(Money(100).ApplyRate(5000).Raw(),  static_cast<i64>(50));    // 50%
    LC_CHECK_EQ(Money(100).ApplyRate(0).Raw(),     static_cast<i64>(0));     // 0%
    LC_CHECK_EQ(Money(100).ApplyRate(20000).Raw(), static_cast<i64>(200));   // 200%
    LC_CHECK_EQ(Money::Zero().ApplyRate(9999).Raw(), static_cast<i64>(0));

    // Exact halves round away from zero.
    LC_CHECK_EQ(Money(101).ApplyRate(5000).Raw(), static_cast<i64>(51));     // 50.5 -> 51
    LC_CHECK_EQ(Money(1).ApplyRate(5000).Raw(),   static_cast<i64>(1));      // 0.5  -> 1
    LC_CHECK_EQ(Money(100).ApplyRate(250).Raw(),  static_cast<i64>(3));      // 2.5  -> 3

    // Just under and just over a half.
    LC_CHECK_EQ(Money(1).ApplyRate(4999).Raw(), static_cast<i64>(0));        // 0.4999 -> 0
    LC_CHECK_EQ(Money(1).ApplyRate(5001).Raw(), static_cast<i64>(1));        // 0.5001 -> 1
    LC_CHECK_EQ(Money(3).ApplyRate(3333).Raw(), static_cast<i64>(1));        // 0.9999 -> 1
    LC_CHECK_EQ(Money(3).ApplyRate(1666).Raw(), static_cast<i64>(0));        // 0.4998 -> 0

    // A realistic tax/interest rate on a realistic balance, worked out by hand:
    // 123456 * 725 / 10000 = 8950.56 -> 8951.
    LC_CHECK_EQ(Money(123456).ApplyRate(725).Raw(), static_cast<i64>(8951));
}

// ---------------------------------------------------------------------------------------
LC_TEST(money_apply_rate_is_symmetric_in_sign) {
    // (-m).ApplyRate(r) == -(m.ApplyRate(r)) for every m and r. If this ever fails, a system
    // that taxes debits and credits at the same rate starts leaking value in one direction.
    const i64 amounts[] = { 0, 1, 3, 7, 50, 99, 100, 101, 12345, 999999, 123456789 };
    const lc::i32 rates[] = { 0, 1, 5, 250, 333, 4999, 5000, 5001, 10000, 12345, 99999 };

    for (i64 m : amounts) {
        for (lc::i32 r : rates) {
            const i64 pos = Money(m).ApplyRate(r).Raw();
            LC_CHECK_EQ(Money(-m).ApplyRate(r).Raw(), -pos);
            LC_CHECK_EQ(Money(m).ApplyRate(-r).Raw(), -pos);
            LC_CHECK_EQ(Money(-m).ApplyRate(-r).Raw(), pos);
        }
    }

    // Spot-check the negative half case explicitly rather than only relatively.
    LC_CHECK_EQ(Money(-101).ApplyRate(5000).Raw(), static_cast<i64>(-51));
    LC_CHECK_EQ(Money(-1).ApplyRate(5000).Raw(),   static_cast<i64>(-1));
    LC_CHECK_EQ(Money(-100).ApplyRate(250).Raw(),  static_cast<i64>(-3));
    LC_CHECK_EQ(Money(100).ApplyRate(-250).Raw(),  static_cast<i64>(-3));
}

// ---------------------------------------------------------------------------------------
// The whole product is carried at 128 bits, so a near-limit balance scaled by a large rate
// is still exact. A naive i64 intermediate would have wrapped long before here.
LC_TEST(money_apply_rate_survives_naive_i64_overflow) {
    const Money vast(9000000000000000000LL);   // 9e18, about 98% of the i64 range

    // 9e18 * 10000 is 9e22 - roughly ten thousand times the i64 limit.
    LC_CHECK_EQ(vast.ApplyRate(10000).Raw(), 9000000000000000000LL);
    LC_CHECK_EQ(vast.ApplyRate(5000).Raw(),  4500000000000000000LL);
    LC_CHECK_EQ(vast.ApplyRate(1).Raw(),      900000000000000LL);
    LC_CHECK_EQ((-vast).ApplyRate(5000).Raw(), -4500000000000000000LL);
}

// ---------------------------------------------------------------------------------------
// The structural heart of I3: an opening balance is minted and transferred, never conjured.
LC_TEST(ledger_open_account_mints_rather_than_conjures) {
    Ledger ledger;
    ledger.Reserve(8);

    const AccountId bank = ledger.OpenAccount();
    ledger.SetCentralBank(bank);

    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(0));
    LC_CHECK_EQ(ledger.SumOfBalances().Raw(), static_cast<i64>(0));
    LC_CHECK(ledger.ConservationHolds());

    const AccountId a = ledger.OpenAccount(Money(2500));

    // The 2500 exists because it was issued, and the bank is exactly where it was: the mint
    // credited it and the funding transfer debited it again.
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(2500));
    LC_CHECK_EQ(ledger.BalanceOf(a).Raw(),  static_cast<i64>(2500));
    LC_CHECK_EQ(ledger.BalanceOf(bank).Raw(), static_cast<i64>(0));
    LC_CHECK(ledger.ConservationHolds());

    // A zero-balance account issues nothing.
    const AccountId b = ledger.OpenAccount();
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(2500));
    LC_CHECK_EQ(ledger.BalanceOf(b).Raw(), static_cast<i64>(0));
    LC_CHECK(ledger.ConservationHolds());

    // Ids are dense, monotonic, and in creation order - the only ordering the ledger has.
    LC_CHECK_EQ(ledger.AccountCount(), 3u);
    LC_CHECK_EQ(ledger.AccountAt(0).value, bank.value);
    LC_CHECK_EQ(ledger.AccountAt(1).value, a.value);
    LC_CHECK_EQ(ledger.AccountAt(2).value, b.value);
    LC_CHECK(ledger.AccountAt(0).value < ledger.AccountAt(1).value);
}

// ---------------------------------------------------------------------------------------
LC_TEST(ledger_mint_and_burn_are_the_only_supply_changes) {
    Ledger ledger;
    const AccountId bank = ledger.OpenAccount();
    ledger.SetCentralBank(bank);
    const AccountId a = ledger.OpenAccount();

    LC_CHECK(ledger.Mint(Money(10000)));
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(10000));
    LC_CHECK_EQ(ledger.BalanceOf(bank).Raw(), static_cast<i64>(10000));
    LC_CHECK(ledger.ConservationHolds());

    // Minting again raises the supply by exactly the amount, no more and no less.
    LC_CHECK(ledger.Mint(Money(777)));
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(10777));
    LC_CHECK_EQ(ledger.BalanceOf(bank).Raw(), static_cast<i64>(10777));

    // Burning lowers it by exactly the amount.
    LC_CHECK(ledger.Burn(Money(777)));
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(10000));
    LC_CHECK_EQ(ledger.BalanceOf(bank).Raw(), static_cast<i64>(10000));
    LC_CHECK(ledger.ConservationHolds());

    // A transfer moves money but never changes the supply.
    LC_CHECK(ledger.Transfer(bank, a, Money(4000)));
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(10000));
    LC_CHECK_EQ(ledger.BalanceOf(bank).Raw(), static_cast<i64>(6000));
    LC_CHECK_EQ(ledger.BalanceOf(a).Raw(), static_cast<i64>(4000));
    LC_CHECK(ledger.ConservationHolds());

    // The bank cannot burn what it does not hold: that would be a negative bank balance,
    // which is money owed into existence rather than money destroyed.
    LC_CHECK_FALSE(ledger.Burn(Money(6001)));
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(10000));
    LC_CHECK_EQ(ledger.BalanceOf(bank).Raw(), static_cast<i64>(6000));

    // Negative mint and negative burn are rejected outright - each would be the other one
    // wearing a disguise, bypassing its own limits.
    LC_CHECK_FALSE(ledger.Mint(Money(-1)));
    LC_CHECK_FALSE(ledger.Burn(Money(-1)));
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(10000));

    // Zero is a no-op that succeeds.
    LC_CHECK(ledger.Mint(Money::Zero()));
    LC_CHECK(ledger.Burn(Money::Zero()));
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(10000));
    LC_CHECK(ledger.ConservationHolds());

    // The supply can be driven all the way down to exactly zero. Bring the money home first
    // - only the bank can burn, and only what it actually holds.
    LC_CHECK(ledger.Transfer(a, bank, ledger.BalanceOf(a)));
    LC_CHECK_EQ(ledger.BalanceOf(bank).Raw(), static_cast<i64>(10000));
    LC_CHECK(ledger.Burn(Money(10000)));
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(0));
    LC_CHECK_EQ(ledger.SumOfBalances().Raw(), static_cast<i64>(0));
    LC_CHECK_EQ(ledger.BalanceOf(bank).Raw(), static_cast<i64>(0));
    LC_CHECK(ledger.ConservationHolds());

    // One minor unit past empty is refused. A negative supply would be a mint running
    // backwards, and it is the boundary an off-by-one in the burn guard lands on.
    LC_CHECK_FALSE(ledger.Burn(Money(1)));
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(0));
    LC_CHECK_EQ(ledger.SumOfBalances().Raw(), static_cast<i64>(0));
    LC_CHECK(ledger.ConservationHolds());

    // And the ledger is still usable afterwards: a fresh mint starts the supply again.
    LC_CHECK(ledger.Mint(Money(7)));
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(7));
    LC_CHECK(ledger.ConservationHolds());
}

// ---------------------------------------------------------------------------------------
LC_TEST(ledger_rejects_overdraft_with_no_side_effects) {
    Ledger ledger;
    const AccountId bank = ledger.OpenAccount();
    ledger.SetCentralBank(bank);
    const AccountId a = ledger.OpenAccount(Money(100));
    const AccountId b = ledger.OpenAccount(Money(50));

    const i64 issuedBefore = ledger.TotalIssued().Raw();

    LC_CHECK_FALSE(ledger.Transfer(a, b, Money(101)));
    LC_CHECK_EQ(ledger.BalanceOf(a).Raw(), static_cast<i64>(100));
    LC_CHECK_EQ(ledger.BalanceOf(b).Raw(), static_cast<i64>(50));
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), issuedBefore);
    LC_CHECK(ledger.ConservationHolds());

    // One over is refused; exactly the balance is allowed. The boundary is where an
    // off-by-one would hide.
    LC_CHECK(ledger.Transfer(a, b, Money(100)));
    LC_CHECK_EQ(ledger.BalanceOf(a).Raw(), static_cast<i64>(0));
    LC_CHECK_EQ(ledger.BalanceOf(b).Raw(), static_cast<i64>(150));
    LC_CHECK(ledger.ConservationHolds());

    // An emptied account cannot go below zero, not even by one minor unit.
    LC_CHECK_FALSE(ledger.Transfer(a, b, Money(1)));
    LC_CHECK_EQ(ledger.BalanceOf(a).Raw(), static_cast<i64>(0));
    LC_CHECK(ledger.ConservationHolds());

    // Zero from an empty account is legal and changes nothing.
    LC_CHECK(ledger.Transfer(a, b, Money::Zero()));
    LC_CHECK_EQ(ledger.BalanceOf(a).Raw(), static_cast<i64>(0));
    LC_CHECK_EQ(ledger.BalanceOf(b).Raw(), static_cast<i64>(150));
    LC_CHECK(ledger.ConservationHolds());
}

// ---------------------------------------------------------------------------------------
LC_TEST(ledger_rejects_negative_transfer) {
    Ledger ledger;
    const AccountId bank = ledger.OpenAccount();
    ledger.SetCentralBank(bank);
    const AccountId a = ledger.OpenAccount(Money(100));
    const AccountId b = ledger.OpenAccount(Money(100));

    // A negative transfer is a reversed transfer with no audit trail, and it would also
    // bypass the overdraft check on the account it actually drains.
    LC_CHECK_FALSE(ledger.Transfer(a, b, Money(-1)));
    LC_CHECK_FALSE(ledger.Transfer(a, b, Money(-1000000)));
    LC_CHECK_EQ(ledger.BalanceOf(a).Raw(), static_cast<i64>(100));
    LC_CHECK_EQ(ledger.BalanceOf(b).Raw(), static_cast<i64>(100));
    LC_CHECK(ledger.ConservationHolds());
}

// ---------------------------------------------------------------------------------------
LC_TEST(ledger_self_transfer_conserves) {
    Ledger ledger;
    const AccountId bank = ledger.OpenAccount();
    ledger.SetCentralBank(bank);
    const AccountId a = ledger.OpenAccount(Money(100));

    const i64 issued = ledger.TotalIssued().Raw();

    // Whether this implementation accepts it as a no-op or refuses it, the balance and the
    // conservation invariant must be untouched. Anything that doubles or zeroes the balance
    // here is a conservation bug.
    const bool selfAccepted = ledger.Transfer(a, a, Money(40));
    LC_CHECK_EQ(ledger.BalanceOf(a).Raw(), static_cast<i64>(100));
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), issued);
    LC_CHECK(ledger.ConservationHolds());

    // Either answer is legal by contract. This implementation treats an affordable
    // self-transfer as a conserving no-op and reports success; pinning that here makes a
    // future change to it a visible decision rather than a silent one.
    LC_CHECK(selfAccepted);

    // A self-transfer the account could not afford is still not allowed to change anything.
    LC_CHECK_FALSE(ledger.Transfer(a, a, Money(101)));
    LC_CHECK_EQ(ledger.BalanceOf(a).Raw(), static_cast<i64>(100));
    LC_CHECK(ledger.ConservationHolds());

    // Nor is a negative self-transfer.
    LC_CHECK_FALSE(ledger.Transfer(a, a, Money(-5)));
    LC_CHECK_EQ(ledger.BalanceOf(a).Raw(), static_cast<i64>(100));
    LC_CHECK(ledger.ConservationHolds());
}

// ---------------------------------------------------------------------------------------
LC_TEST(ledger_rejects_unknown_accounts) {
    Ledger ledger;
    const AccountId bank = ledger.OpenAccount();
    ledger.SetCentralBank(bank);
    const AccountId a = ledger.OpenAccount(Money(100));

    const AccountId none;                 // default-constructed: value 0, never valid
    const AccountId stale(9999);          // never allocated

    LC_CHECK_FALSE(ledger.IsValidAccount(none));
    LC_CHECK_FALSE(ledger.IsValidAccount(stale));
    LC_CHECK(ledger.IsValidAccount(a));

    LC_CHECK_FALSE(ledger.Transfer(a, stale, Money(10)));
    LC_CHECK_FALSE(ledger.Transfer(stale, a, Money(10)));
    LC_CHECK_FALSE(ledger.Transfer(a, none, Money(10)));
    LC_CHECK_FALSE(ledger.Transfer(none, a, Money(10)));

    LC_CHECK_EQ(ledger.BalanceOf(a).Raw(), static_cast<i64>(100));
    LC_CHECK(ledger.ConservationHolds());
}

// ---------------------------------------------------------------------------------------
// INVARIANT I3, the real one: 50 accounts, a known issue, ten thousand pseudo-random
// transfers, and conservation asserted after every single one - accepted or rejected.
LC_TEST(ledger_conserves_across_ten_thousand_random_transfers) {
    constexpr u32 kAccounts = 50u;
    constexpr int kTransfers = 10000;

    Ledger ledger;
    ledger.Reserve(kAccounts + 1u);

    const AccountId bank = ledger.OpenAccount();
    ledger.SetCentralBank(bank);

    // A known total, issued once, by the central bank only.
    const Money issued = Money::FromMajor(1000000);      // 100,000,000 minor units
    LC_CHECK(ledger.Mint(issued));
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(100000000));

    AccountId accounts[kAccounts];
    for (u32 i = 0; i < kAccounts; ++i) {
        accounts[i] = ledger.OpenAccount();
        LC_CHECK(ledger.Transfer(bank, accounts[i], Money(1000000)));
    }
    LC_CHECK(ledger.ConservationHolds());
    LC_CHECK_EQ(ledger.BalanceOf(bank).Raw(), static_cast<i64>(50000000));

    Lcg rng(0x9E3779B97F4A7C15ull);

    // An independent shadow model of every balance. Conservation on its own is a weak
    // oracle: a Transfer that quietly moved nothing, or one that credited the wrong
    // account, conserves perfectly. The model says exactly where every minor unit belongs
    // and exactly which calls should have been refused, so the ledger has to agree with a
    // second implementation of the rules rather than merely with itself.
    i64 model[kAccounts];
    for (u32 i = 0; i < kAccounts; ++i) model[i] = 1000000;

    u32  accepted = 0u;
    u32  rejected = 0u;
    u32  selfMoves = 0u;
    bool conserved = true;
    int  firstDivergentIteration = -1;
    bool verdictsMatched = true;
    int  firstVerdictMismatch = -1;

    for (int t = 0; t < kTransfers; ++t) {
        const u32 fromIndex = rng.Below(kAccounts);
        const u32 toIndex   = rng.Below(kAccounts);

        // Amounts deliberately straddle every rejection boundary: some negative, many larger
        // than any account holds, some zero, most ordinary.
        const i64 amount = static_cast<i64>(rng.Next() % 3000001ull) - 500000LL;

        if (fromIndex == toIndex) ++selfMoves;

        // What the rules say must happen, decided before the ledger is asked.
        const bool expected = (amount >= 0) && (model[fromIndex] >= amount);

        const bool ok = ledger.Transfer(accounts[fromIndex], accounts[toIndex], Money(amount));
        if (ok) { ++accepted; } else { ++rejected; }

        if (ok != expected) {
            verdictsMatched = false;
            firstVerdictMismatch = t;
            break;
        }
        if (ok && fromIndex != toIndex) {
            model[fromIndex] -= amount;
            model[toIndex]   += amount;
        }

        // After EVERY transfer, accepted or not.
        if (!ledger.ConservationHolds()) {
            conserved = false;
            firstDivergentIteration = t;
            break;
        }
    }

    LC_CHECK(conserved);
    LC_CHECK_EQ(firstDivergentIteration, -1);
    LC_CHECK(verdictsMatched);
    LC_CHECK_EQ(firstVerdictMismatch, -1);

    // Every minor unit is exactly where the model says it is - not merely somewhere.
    for (u32 i = 0; i < kAccounts; ++i) {
        LC_CHECK_EQ(ledger.BalanceOf(accounts[i]).Raw(), model[i]);
    }

    // The test is worthless unless it actually exercised both paths, so prove it did. A
    // Transfer that always returned false would otherwise "conserve" perfectly.
    LC_CHECK(accepted > 0u);
    LC_CHECK(rejected > 0u);
    LC_CHECK(selfMoves > 0u);

    // Supply unchanged by ten thousand transfers, and the books still balance.
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(100000000));
    LC_CHECK_EQ(ledger.SumOfBalances().Raw(), static_cast<i64>(100000000));
    LC_CHECK(ledger.ConservationHolds());

    // No account went negative along the way.
    for (u32 i = 0; i < ledger.AccountCount(); ++i) {
        LC_CHECK_FALSE(ledger.BalanceOf(ledger.AccountAt(i)).IsNegative());
    }
}

// ---------------------------------------------------------------------------------------
// I1/I7: the ledger hash is a pure function of ledger state, taken in id order. Two ledgers
// built by the same sequence of operations must hash identically; one minor unit in a
// different place must not.
LC_TEST(ledger_hash_is_state_determined_and_order_stable) {
    auto Build = [](Ledger& l, i64 extraToSecondAccount) {
        const AccountId bank = l.OpenAccount();
        l.SetCentralBank(bank);
        const AccountId a = l.OpenAccount(Money(5000));
        const AccountId b = l.OpenAccount(Money(3000));
        const AccountId c = l.OpenAccount(Money(1000));
        l.Transfer(a, b, Money(250));
        l.Transfer(b, c, Money(125));
        l.Transfer(c, a, Money(60));
        if (extraToSecondAccount != 0) l.Transfer(a, b, Money(extraToSecondAccount));
    };

    Ledger first, second, perturbed;
    Build(first, 0);
    Build(second, 0);
    Build(perturbed, 1);   // exactly one minor unit somewhere else

    lc::Hasher h1, h2, h3;
    first.HashInto(h1);
    second.HashInto(h2);
    perturbed.HashInto(h3);

    LC_CHECK_EQ(h1.Value(), h2.Value());
    LC_CHECK_NE(h1.Value(), h3.Value());

    // Hashing the same ledger twice is idempotent (no hidden mutation, no ordering state).
    lc::Hasher again;
    first.HashInto(again);
    LC_CHECK_EQ(h1.Value(), again.Value());

    // Same balances but a different number of accounts ever created must not collide: the
    // id cursor is part of world state and must round-trip through a save (I7).
    Ledger withExtraAccount;
    Build(withExtraAccount, 0);
    withExtraAccount.OpenAccount();
    lc::Hasher h4;
    withExtraAccount.HashInto(h4);
    LC_CHECK_NE(h1.Value(), h4.Value());
}

// ---------------------------------------------------------------------------------------
// Rates and the ledger together: applying a rate does not itself move money, so the caller
// has to route the exact result through Transfer. This test documents that contract by
// doing it correctly and checking conservation survives.
LC_TEST(ledger_rate_driven_transfer_conserves) {
    Ledger ledger;
    const AccountId bank = ledger.OpenAccount();
    ledger.SetCentralBank(bank);
    const AccountId payer = ledger.OpenAccount(Money(123456));
    const AccountId taxman = ledger.OpenAccount();

    const Money due = ledger.BalanceOf(payer).ApplyRate(725);   // 7.25%
    LC_CHECK_EQ(due.Raw(), static_cast<i64>(8951));             // 8950.56 -> 8951

    LC_CHECK(ledger.Transfer(payer, taxman, due));

    // The payer paid exactly what the taxman received - one rounded figure, moved once.
    LC_CHECK_EQ(ledger.BalanceOf(payer).Raw(), static_cast<i64>(123456 - 8951));
    LC_CHECK_EQ(ledger.BalanceOf(taxman).Raw(), static_cast<i64>(8951));
    LC_CHECK_EQ(ledger.TotalIssued().Raw(), static_cast<i64>(123456));
    LC_CHECK(ledger.ConservationHolds());
}

// ---------------------------------------------------------------------------------------
// The ends of the i64 range, on the side where the answer exists. A balance near the limit
// is not hypothetical: TotalIssued is the sum of every account, and the whole point of the
// 128-bit intermediate in ApplyRate is that scaling such a balance stays exact. The abort
// side of the contract (FromMajor one step too far, negating the most negative amount,
// overflowing an add) cannot be exercised without a death-test facility, so only the side
// that must produce an exact answer is asserted here.
LC_TEST(money_boundary_values_at_the_i64_extremes) {
    constexpr i64 kMax = 9223372036854775807LL;
    constexpr i64 kMin = -9223372036854775807LL - 1LL;   // never as a bare literal

    // FromMajor accepts exactly as far as major * 100 still fits, and not one step further.
    LC_CHECK_EQ(Money::FromMajor(92233720368547758LL).Raw(),   9223372036854775800LL);
    LC_CHECK_EQ(Money::FromMajor(-92233720368547758LL).Raw(), -9223372036854775800LL);

    // The formatting split at both ends. MinorPart comes off the magnitude, so the minor
    // part of the most negative representable amount is a positive 8, not a negative one -
    // and MajorPart carries the whole sign, which is why the two must never be recombined
    // as major * 100 + minor.
    LC_CHECK_EQ(Money(kMax).MajorPart(),  92233720368547758LL);
    LC_CHECK_EQ(Money(kMax).MinorPart(),  static_cast<i64>(7));
    LC_CHECK_EQ(Money(kMin).MajorPart(), -92233720368547758LL);
    LC_CHECK_EQ(Money(kMin).MinorPart(),  static_cast<i64>(8));

    // Addition and subtraction touch both ends exactly without tripping the overflow guard.
    LC_CHECK_EQ((Money(kMax) + Money::Zero()).Raw(), kMax);
    LC_CHECK_EQ((Money(kMax - 1) + Money(1)).Raw(),  kMax);
    LC_CHECK_EQ((Money(kMin + 1) - Money(1)).Raw(),  kMin);
    LC_CHECK_EQ((Money(kMin) + Money::Zero()).Raw(), kMin);
    LC_CHECK_EQ((Money(kMax) - Money(kMax)).Raw(),   static_cast<i64>(0));

    // Negation and Abs are exact right next to the one value where they are undefined.
    LC_CHECK_EQ((-Money(kMin + 1)).Raw(),      kMax);
    LC_CHECK_EQ(Money(kMin + 1).Abs().Raw(),   kMax);
    LC_CHECK_EQ(Money(kMax).Abs().Raw(),       kMax);

    // A 100% rate on a near-limit balance is the identity in both directions, and a 0% rate
    // is zero with no sign residue. The intermediate product here is about 9e22, so a naive
    // i64 multiply would have wrapped long before the divide.
    LC_CHECK_EQ(Money(kMax).ApplyRate(10000).Raw(), kMax);
    LC_CHECK_EQ(Money(kMin).ApplyRate(10000).Raw(), kMin);
    LC_CHECK_EQ(Money(kMax).ApplyRate(0).Raw(), static_cast<i64>(0));
    LC_CHECK_EQ(Money(kMin).ApplyRate(0).Raw(), static_cast<i64>(0));

    // Ordering across the full span.
    LC_CHECK(Money(kMin) < Money(kMax));
    LC_CHECK(Money(kMin).IsNegative());
    LC_CHECK(Money(kMax).IsPositive());
    LC_CHECK_EQ(Money::Min(Money(kMin), Money(kMax)).Raw(), kMin);
    LC_CHECK_EQ(Money::Max(Money(kMin), Money(kMax)).Raw(), kMax);
}

// ---------------------------------------------------------------------------------------
// Account ids are dense and one-based, so the valid set is exactly 1..AccountCount(). The
// id that matters is not 9999 - it is the one immediately past the end, which is where an
// off-by-one in the (id - 1) < size test would let a caller read or write one element past
// the balance vector.
LC_TEST(ledger_account_id_validity_at_the_boundary) {
    Ledger ledger;
    const AccountId bank = ledger.OpenAccount();
    ledger.SetCentralBank(bank);
    const AccountId a = ledger.OpenAccount(Money(100));
    const AccountId b = ledger.OpenAccount(Money(50));

    LC_CHECK_EQ(ledger.AccountCount(), 3u);
    LC_CHECK_EQ(bank.value, 1u);          // ids start at one; zero is reserved for "none"
    LC_CHECK_EQ(b.value, 3u);

    const AccountId last(ledger.AccountCount());
    const AccountId pastEnd(ledger.AccountCount() + 1u);

    LC_CHECK(ledger.IsValidAccount(AccountId(1u)));
    LC_CHECK(ledger.IsValidAccount(last));
    LC_CHECK_FALSE(ledger.IsValidAccount(pastEnd));
    LC_CHECK_FALSE(ledger.IsValidAccount(AccountId(0u)));
    LC_CHECK_FALSE(AccountId(0u).IsValid());

    // The one-past-the-end id must be refused from either side of a transfer, with no side
    // effects on either party.
    LC_CHECK_FALSE(ledger.Transfer(a, pastEnd, Money(10)));
    LC_CHECK_FALSE(ledger.Transfer(pastEnd, a, Money(10)));
    LC_CHECK_FALSE(ledger.Transfer(pastEnd, pastEnd, Money::Zero()));
    LC_CHECK_EQ(ledger.BalanceOf(a).Raw(), static_cast<i64>(100));
    LC_CHECK_EQ(ledger.BalanceOf(b).Raw(), static_cast<i64>(50));
    LC_CHECK(ledger.ConservationHolds());

    // The last index AccountAt accepts is exactly the last valid id, and the id cursor keeps
    // step with the account count (I7: both have to round-trip through a save).
    LC_CHECK_EQ(ledger.AccountAt(ledger.AccountCount() - 1u).value, last.value);
    LC_CHECK_EQ(ledger.IdCursor(), ledger.AccountCount());
}

// ---------------------------------------------------------------------------------------
// I1/I7: which account is the central bank is world state, not a convenience pointer. It
// decides where the next Mint lands and what Burn is allowed to destroy, so two ledgers
// holding identical balances but pointing at different banks are NOT the same world. A
// world hash that could not tell them apart would let a save reload into a different
// economy, and the divergence would only surface ticks later, at the first Mint.
LC_TEST(ledger_hash_distinguishes_the_central_bank) {
    auto Build = [](Ledger& l) {
        const AccountId x = l.OpenAccount();
        l.SetCentralBank(x);
        LC_CHECK(l.Mint(Money(1000)));
        const AccountId y = l.OpenAccount();
        LC_CHECK(l.Transfer(x, y, Money(400)));
    };

    Ledger sameBank, otherBank;
    Build(sameBank);
    Build(otherBank);
    otherBank.SetCentralBank(otherBank.AccountAt(1));   // re-designate; balances untouched

    // Every other observable is identical...
    LC_CHECK_EQ(sameBank.AccountCount(), otherBank.AccountCount());
    LC_CHECK_EQ(sameBank.TotalIssued().Raw(), otherBank.TotalIssued().Raw());
    LC_CHECK_EQ(sameBank.SumOfBalances().Raw(), otherBank.SumOfBalances().Raw());
    LC_CHECK_EQ(sameBank.IdCursor(), otherBank.IdCursor());
    for (u32 i = 0; i < sameBank.AccountCount(); ++i) {
        LC_CHECK_EQ(sameBank.BalanceOf(sameBank.AccountAt(i)).Raw(),
                    otherBank.BalanceOf(otherBank.AccountAt(i)).Raw());
    }
    LC_CHECK_NE(sameBank.CentralBank().value, otherBank.CentralBank().value);

    // ...so if these hashes collide, the central bank is not in the hash at all.
    lc::Hasher h1, h2;
    sameBank.HashInto(h1);
    otherBank.HashInto(h2);
    LC_CHECK_NE(h1.Value(), h2.Value());
}

// ---------------------------------------------------------------------------------------
// I5: a sim tick allocates nothing. Money moves on every tick once the economy lands, so
// Transfer, Mint, Burn, BalanceOf, the conservation check and the world-hash fold all have
// to be allocation-free. Only OpenAccount may allocate, and account creation is not a
// steady-state path - which is exactly what Reserve() exists for. The header already claims
// all of this; the claim is worth nothing until something measures it.
LC_TEST(ledger_steady_state_paths_allocate_nothing) {
    constexpr u32 kAccounts = 32u;

    Ledger ledger;
    ledger.Reserve(kAccounts + 1u);       // the one allocation, taken up front

    const AccountId bank = ledger.OpenAccount();
    ledger.SetCentralBank(bank);
    LC_CHECK(ledger.Mint(Money(1000000)));

    AccountId accounts[kAccounts];
    for (u32 i = 0; i < kAccounts; ++i) {
        accounts[i] = ledger.OpenAccount();
        LC_CHECK(ledger.Transfer(bank, accounts[i], Money(10000)));
    }

    if (!lc::AllocTrackingEnabled()) {
        // Tracking is compiled out (e.g. under UBT). The loop below still runs, so an assert
        // or a crash on these paths would still be caught; only the count is unavailable.
        LC_CHECK(true);
    }

    u64 sink  = 0ull;
    u32 moved = 0u;
    lc::NoAllocScope guard("Ledger steady-state transfer/mint/burn/hash");

    for (u32 t = 0u; t < 5000u; ++t) {
        const u32 from = t % kAccounts;
        const u32 to   = (t * 7u + 1u) % kAccounts;   // never equal to the source index
        if (ledger.Transfer(accounts[from], accounts[to], Money(1))) ++moved;
        sink += static_cast<u64>(ledger.BalanceOf(accounts[to]).Raw());
        if (ledger.ConservationHolds()) ++sink;
    }
    ledger.Mint(Money(5));
    ledger.Burn(Money(5));

    lc::Hasher h;
    ledger.HashInto(h);
    sink += h.Value();

    const u64 allocations = guard.AllocationsSinceStart();

    LC_CHECK_EQ(moved, 5000u);            // every one of them was a real, accepted move
    LC_CHECK(sink > 0ull);
    LC_CHECK(ledger.ConservationHolds());
    if (lc::AllocTrackingEnabled()) {
        LC_CHECK_EQ(allocations, 0ull);
    }
}
