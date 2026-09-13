// LIVING CITY - money, and the ledger that is the only thing allowed to move it.
//
// Invariant I3: total money is conserved exactly, and money is created or destroyed only by
// the central bank. This header exists to make violating that STRUCTURALLY HARD rather than
// merely forbidden by a comment:
//
//   * Money is an i64 count of MINOR units (cents). No float ever touches a balance, so
//     there is no drift to accumulate and no rounding that varies by machine (R3).
//   * There is no implicit conversion into or out of Money. You cannot accidentally add a
//     tick count to a balance, and you cannot leak a balance into a float expression.
//   * Money * Money and Money / Money are explicitly DELETED. The product of two amounts of
//     money is not money and is not anything else either; scaling is ApplyRate.
//   * A balance lives only inside Ledger, and Ledger::Transfer is the only path between two
//     accounts. It is all-or-nothing: it never partially applies.
//   * Mint and Burn are the only operations that change TotalIssued, and they exist only on
//     the central bank account. Even OpenAccount with an opening balance is implemented as
//     Mint + Transfer, so conservation holds from the account's first instant rather than
//     being violated at birth and patched up later.
//
// R1: zero Unreal types. No exceptions: rejections are bool returns, contract violations are
// LC_ASSERT / LC_FATAL.
#pragma once

#include "livingcity/core/Core.h"
#include "livingcity/core/Fixed.h"   // for the portable 128-bit helpers in lc::fx

#include <vector>

namespace lc {

// ---------------------------------------------------------------- Money
struct Money {
    // Minor units. 100 minor units = 1 major unit. Public so Money stays a trivially
    // copyable POD for snapshots and serialisation; every other route in is explicit.
    i64 minor = 0;

    static constexpr i64 kMinorPerMajor = 100;

    // 10000 basis points = 100%. See ApplyRate.
    static constexpr u64 kBasisPointScale = 10000ull;

    constexpr Money() = default;

    // Explicit on purpose: an i64 is never silently money, and money is never silently an
    // i64. This is the whole reason the type exists.
    constexpr explicit Money(i64 minorUnits) : minor(minorUnits) {}

    static constexpr Money Zero() { return Money(0); }

    static Money FromMajor(i64 major) {
        LC_ASSERT_MSG(major <= (0x7FFFFFFFFFFFFFFFll / kMinorPerMajor) &&
                      major >= (-0x7FFFFFFFFFFFFFFFll - 1) / kMinorPerMajor,
                      "Money::FromMajor overflow");
        return Money(major * kMinorPerMajor);
    }

    constexpr i64 Raw() const { return minor; }

    // Major/minor split for FORMATTING ONLY - the pair is not an alternative representation
    // and must never be recombined as major*100 + minor for a negative amount.
    // MajorPart truncates toward zero (C++ integer division), MinorPart is taken from the
    // magnitude, so -1.05 reports as (major -1, minor 5) rather than (-1, -5): the sign
    // lives on the major part alone.
    constexpr i64 MajorPart() const { return minor / kMinorPerMajor; }
    i64 MinorPart() const {
        return static_cast<i64>(fx::Magnitude(minor) % static_cast<u64>(kMinorPerMajor));
    }

    // ------------------------------------------------------------ arithmetic
    // Addition and subtraction are the only closed operations on money, and both refuse to
    // wrap: a wrapped balance is a silently created or destroyed fortune.
    Money operator+(Money o) const {
        const i64 s = static_cast<i64>(static_cast<u64>(minor) + static_cast<u64>(o.minor));
        LC_ASSERT_MSG(((minor ^ s) & (o.minor ^ s)) >= 0, "Money addition overflow");
        return Money(s);
    }

    Money operator-(Money o) const {
        const i64 s = static_cast<i64>(static_cast<u64>(minor) - static_cast<u64>(o.minor));
        LC_ASSERT_MSG(((minor ^ o.minor) & (minor ^ s)) >= 0, "Money subtraction overflow");
        return Money(s);
    }

    Money operator-() const {
        LC_ASSERT_MSG(minor != static_cast<i64>(0x8000000000000000ull), "Money negation overflow");
        return Money(static_cast<i64>(0ull - static_cast<u64>(minor)));
    }

    Money& operator+=(Money o) { *this = *this + o; return *this; }
    Money& operator-=(Money o) { *this = *this - o; return *this; }

    // DELIBERATELY ABSENT, and deleted rather than merely missing so that the compiler says
    // what is wrong instead of "no operator found": multiplying two amounts of money is
    // meaningless, and dividing them silently discards the units. Use ApplyRate for a rate,
    // and integer counts for a quantity.
    friend Money operator*(Money, Money) = delete;
    friend Money operator/(Money, Money) = delete;

    // ------------------------------------------------------------ comparison
    constexpr bool operator==(Money o) const { return minor == o.minor; }
    constexpr bool operator!=(Money o) const { return minor != o.minor; }
    constexpr bool operator< (Money o) const { return minor <  o.minor; }
    constexpr bool operator<=(Money o) const { return minor <= o.minor; }
    constexpr bool operator> (Money o) const { return minor >  o.minor; }
    constexpr bool operator>=(Money o) const { return minor >= o.minor; }

    constexpr bool IsZero()     const { return minor == 0; }
    constexpr bool IsNegative() const { return minor <  0; }
    constexpr bool IsPositive() const { return minor >  0; }

    Money Abs() const { return IsNegative() ? -(*this) : *this; }

    static constexpr Money Min(Money a, Money b) { return (a.minor < b.minor) ? a : b; }
    static constexpr Money Max(Money a, Money b) { return (a.minor > b.minor) ? a : b; }

    // ------------------------------------------------------------ rates
    // Scale by basisPoints/10000 - the one sanctioned way to apply an interest rate, a tax
    // rate, a wage percentage or a discount. 10000 bp = 100%, 250 bp = 2.5%.
    //
    // ROUNDING RULE: HALF AWAY FROM ZERO, applied to the magnitude of the exact product.
    //   Money(101).ApplyRate(5000)  ==  Money(51)    // exactly 50.5 -> 51
    //   Money(-101).ApplyRate(5000) ==  Money(-51)   // exactly -50.5 -> -51
    //   Money(100).ApplyRate(5000)  ==  Money(50)    // exact, no rounding
    //   Money(1).ApplyRate(4999)    ==  Money(0)     // 0.4999 -> 0
    // Because the sign is stripped first and reapplied last, the rule is exactly symmetric:
    //   (-m).ApplyRate(r) == -(m.ApplyRate(r))  for every m and every r.
    // This is NOT banker's rounding. Half-away-from-zero has a small outward bias under
    // repeated application, which we accept: it is trivially reproducible on every compiler,
    // whereas round-half-to-even on a signed magnitude is easy to get subtly and silently
    // wrong, and silence is the one thing determinism cannot survive.
    //
    // The whole product is carried at 128 bits, so a balance near the i64 limit scaled by a
    // large rate is still exact rather than overflowing on the way to being divided down.
    //
    // NOTE FOR CALLERS: ApplyRate on its own does not conserve money - rounding creates or
    // destroys minor units. The result must be moved with Ledger::Transfer (the counterparty
    // paying exactly the value returned here), never by crediting one account and debiting a
    // separately rounded other account.
    Money ApplyRate(i32 basisPoints) const {
        const bool negative = (minor < 0) != (basisPoints < 0);

        const u64 a = fx::Magnitude(minor);
        const u64 b = fx::Magnitude(static_cast<i64>(basisPoints));

        // Exact |minor| * |bp|, then + 5000 for the half-away-from-zero bias, then / 10000.
        // Truncating that biased quotient is precisely "round half away from zero".
        const fx::U128 biased = fx::Add128(fx::Mul64x64(a, b), kBasisPointScale / 2);
        const u64      mag    = fx::Div128By64(biased, kBasisPointScale);

        return Money(fx::SignedFromMagnitude(mag, negative));
    }

    // ------------------------------------------------------------ hashing (I1, I7)
    void HashInto(Hasher& h) const { h.I64(minor); }
};

// ---------------------------------------------------------------- Ledger
// Accounts and the only legal way to move value between them.
//
// Storage is a std::vector indexed by (AccountId.value - 1). Ids come from the monotonic
// IdAllocator in Core.h, so they are dense, ordered, and never reused - which means every
// traversal here is in id order and there is no unordered container anywhere in the type
// (R3). Reserve() up front and the steady-state paths allocate nothing (I5); only
// OpenAccount can grow the vector, and account creation is not a steady-state hot path.
class Ledger {
public:
    Ledger() = default;

    explicit Ledger(u32 reserveAccounts) { Reserve(reserveAccounts); }

    // Call once at world build with the expected account count so that Transfer, Mint, Burn
    // and SumOfBalances never allocate afterwards.
    void Reserve(u32 accounts) { balances_.reserve(static_cast<std::size_t>(accounts)); }

    // ------------------------------------------------------------ accounts
    // An opening balance is NOT conjured. It is minted into the central bank and then
    // transferred out, which is why this requires a central bank whenever the balance is
    // non-zero, and why ConservationHolds() is true at every point during the call.
    AccountId OpenAccount(Money initialBalance = Money::Zero()) {
        LC_ASSERT_MSG(!initialBalance.IsNegative(), "Ledger::OpenAccount negative opening balance");

        const AccountId id = ids_.Next();
        balances_.push_back(Money::Zero());

        if (!initialBalance.IsZero()) {
            // Both of these are the ordinary, checked paths - no back door.
            const bool minted = Mint(initialBalance);
            LC_ASSERT_MSG(minted, "Ledger::OpenAccount mint failed");
            const bool moved = Transfer(centralBank_, id, initialBalance);
            LC_ASSERT_MSG(moved, "Ledger::OpenAccount funding transfer failed");
        }
        return id;
    }

    bool IsValidAccount(AccountId id) const {
        return id.IsValid() && static_cast<std::size_t>(id.value - 1u) < balances_.size();
    }

    u32 AccountCount() const { return static_cast<u32>(balances_.size()); }

    // Index order is id order, by construction. This is the deterministic way to walk the
    // ledger; there is no other.
    AccountId AccountAt(u32 index) const {
        LC_ASSERT_MSG(index < AccountCount(), "Ledger::AccountAt out of range");
        return AccountId(index + 1u);
    }

    Money BalanceOf(AccountId id) const {
        LC_ASSERT_MSG(IsValidAccount(id), "Ledger::BalanceOf unknown account");
        return balances_[static_cast<std::size_t>(id.value - 1u)];
    }

    // ------------------------------------------------------------ central bank
    void SetCentralBank(AccountId id) {
        LC_ASSERT_MSG(IsValidAccount(id), "Ledger::SetCentralBank unknown account");
        centralBank_ = id;
    }

    AccountId CentralBank() const { return centralBank_; }
    bool      HasCentralBank() const { return centralBank_.IsValid(); }

    // The ONLY creation of money in the simulation. Credits the central bank and raises
    // TotalIssued by exactly the same amount, so conservation is preserved by construction.
    // Rejects a negative amount (that would be a Burn wearing a disguise) and refuses to
    // overflow. Minting with no central bank set is a design error, not a runtime condition.
    bool Mint(Money amount) {
        if (!HasCentralBank()) LC_FATAL("Ledger::Mint with no central bank set");
        if (amount.IsNegative()) return false;
        if (amount.IsZero())     return true;

        Money& cb = BalanceRef(centralBank_);
        if (AddOverflows(cb.Raw(), amount.Raw()))          return false;
        if (AddOverflows(totalIssued_.Raw(), amount.Raw())) return false;

        cb           += amount;
        totalIssued_ += amount;
        return true;
    }

    // The ONLY destruction of money. Debits the central bank and lowers TotalIssued by the
    // same amount. Refuses to burn more than the central bank actually holds, because a
    // negative central-bank balance is money owed into existence, not money destroyed - and
    // refuses, separately, to drive TotalIssued below zero.
    bool Burn(Money amount) {
        if (!HasCentralBank()) LC_FATAL("Ledger::Burn with no central bank set");
        if (amount.IsNegative()) return false;
        if (amount.IsZero())     return true;

        Money& cb = BalanceRef(centralBank_);
        if (cb < amount) return false;
        // Redundant while conservation holds (no balance is ever negative, so the bank can
        // never hold more than was issued), and kept anyway: it makes "the supply can never
        // go negative" a local property of Burn rather than something that has to be argued
        // from a global invariant. If a future save/load ever restores a ledger whose books
        // do not balance, this rejects instead of minting a negative supply.
        if (totalIssued_ < amount) return false;

        cb           -= amount;
        totalIssued_ -= amount;
        return true;
    }

    // ------------------------------------------------------------ the one mutation path
    // Move `amount` from one account to another. Returns false and changes NOTHING when:
    //   * either account id is unknown,
    //   * the amount is negative (a negative transfer is a reversed transfer with no audit
    //     trail, so it is rejected outright),
    //   * the source cannot cover it (no overdrafts - credit is a separate system that will
    //     be built out of two accounts and a contract, not out of a negative balance),
    //   * the destination balance would overflow i64.
    // A transfer to self is a conserving no-op: it still validates the account and the
    // amount, and still requires the funds to be there, but moves nothing.
    // There is no partial application: every check happens before any balance is written.
    bool Transfer(AccountId from, AccountId to, Money amount) {
        if (!IsValidAccount(from) || !IsValidAccount(to)) return false;
        if (amount.IsNegative()) return false;

        Money& src = BalanceRef(from);
        if (src < amount) return false;

        if (from == to) return true;   // conserving no-op

        Money& dst = BalanceRef(to);
        // Unreachable while conservation holds - no single balance can exceed TotalIssued,
        // and Mint already refuses to let TotalIssued pass the i64 limit - so there is
        // deliberately no test for this branch. It stays because "Transfer never wraps a
        // balance" must be true of Transfer itself, not only of the system around it.
        if (AddOverflows(dst.Raw(), amount.Raw())) return false;

        src -= amount;
        dst += amount;
        return true;
    }

    // ------------------------------------------------------------ conservation (I3)
    Money TotalIssued() const { return totalIssued_; }

    Money SumOfBalances() const {
        Money sum = Money::Zero();
        for (std::size_t i = 0; i < balances_.size(); ++i) sum += balances_[i];
        return sum;
    }

    // The invariant itself, cheap enough to assert every tick in a debug build and after
    // every transfer in a test.
    bool ConservationHolds() const { return SumOfBalances() == TotalIssued(); }

    // ------------------------------------------------------------ hashing (I1, I7)
    // Strictly in id order. Never iterate an unordered container into a world hash - it is
    // the single easiest way to make a replay diverge between two builds of the same code.
    void HashInto(Hasher& h) const {
        h.U32(AccountCount());
        for (std::size_t i = 0; i < balances_.size(); ++i) {
            h.U32(static_cast<u32>(i) + 1u);   // the AccountId at this index
            balances_[i].HashInto(h);
        }
        h.Ident(centralBank_);
        totalIssued_.HashInto(h);
        h.U32(ids_.RawCursor());
    }

    // Part of world state, so it has to round-trip through save/load (I7).
    u32  IdCursor() const { return ids_.RawCursor(); }

private:
    Money& BalanceRef(AccountId id) {
        LC_ASSERT_MSG(IsValidAccount(id), "Ledger internal: unknown account");
        return balances_[static_cast<std::size_t>(id.value - 1u)];
    }

    // Non-aborting overflow test: Transfer and Mint must be able to REJECT rather than die,
    // so they cannot lean on the assert inside Money::operator+.
    static bool AddOverflows(i64 a, i64 b) {
        const i64 s = static_cast<i64>(static_cast<u64>(a) + static_cast<u64>(b));
        return ((a ^ s) & (b ^ s)) < 0;
    }

    std::vector<Money>     balances_;                 // index == AccountId.value - 1
    IdAllocator<AccountId> ids_;
    AccountId              centralBank_{};
    Money                  totalIssued_ = Money::Zero();
};

} // namespace lc
