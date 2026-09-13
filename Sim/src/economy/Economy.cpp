// LIVING CITY - the economy. Implements include/livingcity/economy/Economy.h.
//
// R1: zero Unreal types. R3: integers only, no wall-clock, no unordered container; every
// periodic action is gated on a SimClock boundary predicate, never on accumulated time.
// R4: no player branch anywhere in this file.
//
// ---------------------------------------------------------------------------------------
// MONEY. The government account IS the ledger's central bank, so the one Mint in Generate is
// the only creation of money in this file, and every other movement below is a checked
// Ledger::Transfer. A transfer that fails for want of funds is an ECONOMIC EVENT - a firm that
// cannot make payroll simply does not pay this hour, a shop that cannot afford a full batch
// buys what it can - never an assert and never a log line. That is what lets I3 hold on every
// tick by construction rather than by audit: nothing here can conjure or destroy a cent.
//
// The one transfer that is asserted rather than tolerated is the sales-tax leg of a purchase,
// and only because the citizen's balance was already checked against the FULL price before
// the first leg moved: if the second leg then fails, the ledger itself is broken.
//
// ---------------------------------------------------------------------------------------
// TICK HIERARCHY (D-008). Needs, errands and the market run once a sim-minute, wages and
// contracts once a sim-hour, purchases every tick. The needs tunables are PER SIM-MINUTE and
// are applied on the minute boundary, so the integer arithmetic is exact: there is no
// per-second fraction of three points to round.
//
// ORDER WITHIN A TICK. Sim.cpp ticks Citizens before Economy, so on the tick a citizen
// arrives at a shop, JustArrivedAtShop is already true here and the sale is rung up on the
// same tick. Within Economy::Tick the order is needs, errands, market, wages, purchases -
// fixed, and part of the contract: reordering it changes the world hash.
//
// PRICES. A shop's price is a pure function of its own stock (RepriceShop). It is recomputed
// once a sim-minute after restocking, and the stored value is what purchases and errand
// decisions read in between - so shopPriceMinor_ is state, not a cache, and it is hashed.
#include "livingcity/economy/Economy.h"

namespace lc {

namespace {

// File-local names are prefixed: the Unreal build compiles the whole sim as ONE translation
// unit (D-005), so an unprefixed constant here would collide with another file's.
constexpr u32 kEconomyNeedMax = 1000u;   // the "critical" end of both pressure scales

// Add `delta` to a pressure, clamped to [0, kEconomyNeedMax].
u16 EconomyRaise(u16 value, u16 delta) {
    const u32 sum = static_cast<u32>(value) + static_cast<u32>(delta);
    return static_cast<u16>((sum > kEconomyNeedMax) ? kEconomyNeedMax : sum);
}

u16 EconomyLower(u16 value, u16 delta) {
    return (value > delta) ? static_cast<u16>(value - delta) : static_cast<u16>(0u);
}

i64 EconomyAbsI64(i64 v) { return (v < 0) ? -v : v; }

// True if `building` is in `sortedShops`, which is ascending by construction (City::OfUse
// order). Binary search, so the per-hour contract pass is O(workers * log shops) and
// touches no scratch storage.
bool EconomyIsShop(const std::vector<u32>& sortedShops, u32 building) {
    std::size_t lo = 0;
    std::size_t hi = sortedShops.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (sortedShops[mid] == building) return true;
        if (sortedShops[mid] < building) lo = mid + 1;
        else                             hi = mid;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------------------
void Economy::Clear() {
    params_           = EconomyParams{};
    wholesaleEnabled_ = true;
    citizenAccount_.clear();
    hunger_.clear();
    fatigue_.clear();
    firmAccount_.clear();
    shopStock_.clear();
    shopPriceMinor_.clear();
    shops_.clear();
    wagesPaidMinor_   = 0;
    retailSalesMinor_ = 0;
    taxMinor_         = 0;
    starvationEvents_ = 0;
}

// The world seed is deliberately unnamed: nothing in the economy's generation is random.
// Every account, balance and stock level is a function of the city and the population, both
// of which are already functions of the seed. The parameter stays in the signature so that a
// later phase that DOES need the Economy stream can take it without changing the contract.
void Economy::Generate(u64 /*worldSeed*/, const City& city, const Citizens& citizens,
                       Ledger& ledger, const EconomyParams& params) {
    Clear();
    params_ = params;

    // Contract checks on the tunables. These are design errors, not runtime conditions: a
    // negative wage or a 200% sales tax is a broken data file, and I4 says fail loudly.
    LC_ASSERT_MSG(ledger.HasCentralBank(), "Economy::Generate: ledger has no central bank");
    LC_ASSERT_MSG(params_.initialFirmCapitalMinor >= 0 && params_.initialCitizenCashMinor >= 0 &&
                  params_.governmentReserveMinor >= 0,
                  "Economy::Generate: initial money amounts must be non-negative");
    LC_ASSERT_MSG(params_.hourlyWageMinor >= 0 && params_.contractPerWorkerHourMinor >= 0,
                  "Economy::Generate: wage and contract rates must be non-negative");
    LC_ASSERT_MSG(params_.wholesaleFoodMinor >= 0 && params_.baseRetailFoodMinor > 0,
                  "Economy::Generate: wholesale must be non-negative and base retail positive");
    LC_ASSERT_MSG(params_.shopTargetStock > 0u, "Economy::Generate: shopTargetStock must be positive");
    LC_ASSERT_MSG(params_.salesTaxBasisPoints <= static_cast<u32>(Money::kBasisPointScale),
                  "Economy::Generate: sales tax above 100%");
    // The two products the tick path forms in plain i64 - RepriceShop's base * target (and
    // its base * 8 ceiling) and TickMarket's wholesale * batch - are checked ONCE here, so a
    // data file that would make either wrap fails at generation rather than as a negative
    // price or a signed-overflow UB three sim-years in. Money::operator+ guards the sums; it
    // cannot guard a product it never sees.
    {
        constexpr i64 kI64Max = 0x7FFFFFFFFFFFFFFFll;
        LC_ASSERT_MSG(params_.baseRetailFoodMinor <= kI64Max / 8 &&
                      params_.baseRetailFoodMinor <= kI64Max / static_cast<i64>(params_.shopTargetStock),
                      "Economy::Generate: baseRetailFoodMinor * shopTargetStock would overflow i64");
        LC_ASSERT_MSG(params_.shopRestockBatch == 0u ||
                      params_.wholesaleFoodMinor <= kI64Max / static_cast<i64>(params_.shopRestockBatch),
                      "Economy::Generate: wholesaleFoodMinor * shopRestockBatch would overflow i64");
    }

    const u32 citizenCount  = citizens.Count();
    const u32 buildingCount = city.Count();

    const std::vector<u32>& works = city.OfUse(LotUse::Work);
    const std::vector<u32>& shops = city.OfUse(LotUse::Shop);
    const u32 firmCount = static_cast<u32>(works.size()) + static_cast<u32>(shops.size());

    // I5: every array is sized once, here. Tick() never grows one.
    citizenAccount_.reserve(citizenCount);
    hunger_.reserve(citizenCount);
    fatigue_.reserve(citizenCount);
    firmAccount_.reserve(buildingCount);
    shopStock_.reserve(buildingCount);
    shopPriceMinor_.reserve(buildingCount);
    shops_.reserve(shops.size());
    ledger.Reserve(ledger.AccountCount() + citizenCount + firmCount);

    // ---- accounts, in a fixed order: citizens by index, then firms by building index -----
    for (u32 i = 0; i < citizenCount; ++i) {
        citizenAccount_.push_back(ledger.OpenAccount());
        hunger_.push_back(static_cast<u16>(0u));
        fatigue_.push_back(static_cast<u16>(0u));
    }
    for (u32 b = 0; b < buildingCount; ++b) {
        const LotUse use  = city.At(b).use;
        const bool   firm = (use == LotUse::Work) || (use == LotUse::Shop);
        firmAccount_.push_back(firm ? ledger.OpenAccount() : AccountId{});
        shopStock_.push_back(0u);
        shopPriceMinor_.push_back(0);
    }
    for (const u32 s : shops) shops_.push_back(s);

    // ---- ONE mint, then distribution by transfer ---------------------------------------
    // Money::operator+ asserts on overflow, so an absurd parameter set fails here, loudly,
    // rather than as a wrapped balance three sim-years in.
    Money supply = Money(params_.governmentReserveMinor);
    for (u32 k = 0; k < firmCount; ++k)    supply += Money(params_.initialFirmCapitalMinor);
    for (u32 i = 0; i < citizenCount; ++i) supply += Money(params_.initialCitizenCashMinor);

    const bool minted = ledger.Mint(supply);
    LC_ASSERT_MSG(minted, "Economy::Generate: initial mint failed");

    const AccountId government = ledger.CentralBank();
    const Money     cash       = Money(params_.initialCitizenCashMinor);
    const Money     capital    = Money(params_.initialFirmCapitalMinor);

    for (u32 i = 0; i < citizenCount; ++i) {
        const bool ok = ledger.Transfer(government, citizenAccount_[i], cash);
        LC_ASSERT_MSG(ok, "Economy::Generate: citizen funding transfer failed");
    }
    for (u32 b = 0; b < buildingCount; ++b) {
        if (!firmAccount_[b].IsValid()) continue;
        const bool ok = ledger.Transfer(government, firmAccount_[b], capital);
        LC_ASSERT_MSG(ok, "Economy::Generate: firm capital transfer failed");
    }

    // ---- shelves ----------------------------------------------------------------------
    // Goods are not money: stock is created here from nothing, and the price follows from it.
    for (const u32 s : shops_) {
        shopStock_[s] = params_.shopTargetStock;
        RepriceShop(s);
    }
}

// ---------------------------------------------------------------------------------------
void Economy::Tick(const SimClock& clock, const City& city, Citizens& citizens, Ledger& ledger) {
    LC_ASSERT_MSG(citizens.Count() == CitizenCount(),
                  "Economy::Tick: population changed since Generate");

    if (clock.IsMinuteBoundary()) {
        TickNeeds(citizens);
        TickErrands(city, citizens, ledger);
        TickMarket(ledger);
    }
    if (clock.IsHourBoundary()) {
        TickWages(citizens, ledger);
    }
    TickPurchases(citizens, ledger);
}

// Once a sim-minute. Hunger only ever rises here; food is the only relief (TickPurchases).
// Fatigue rises everywhere except at home, where it falls. A starvation event is counted on
// the transition INTO the critical value, so a citizen pinned at 1000 counts once, not once
// per minute - I11 measures events, not minutes of suffering.
void Economy::TickNeeds(const Citizens& citizens) {
    const u32 n = CitizenCount();
    for (u32 i = 0; i < n; ++i) {
        const u16 before = hunger_[i];
        hunger_[i]       = EconomyRaise(before, params_.hungerPerSimMinute);
        if (before < kEconomyNeedMax && hunger_[i] == kEconomyNeedMax) ++starvationEvents_;

        if (citizens.State(i) == Activity::AtHome) {
            fatigue_[i] = EconomyLower(fatigue_[i], params_.fatigueRestoredPerSimMinuteAtHome);
        } else {
            fatigue_[i] = EconomyRaise(fatigue_[i], params_.fatiguePerSimMinuteAwake);
        }
    }
}

// Once a sim-minute. The DECISION to shop lives here; Citizens executes the walk. A citizen
// is sent only when they are somewhere an errand can start from, have none pending, are
// hungrier than the threshold, and can afford one unit at today's price at the nearest shop.
// Affordability is checked against the price NOW; by the time they arrive it may have moved,
// and then they walk home empty-handed. That is a real outcome, not a bug.
void Economy::TickErrands(const City& city, Citizens& citizens, const Ledger& ledger) {
    if (shops_.empty()) return;

    const u32 n = CitizenCount();
    for (u32 i = 0; i < n; ++i) {
        const Activity a = citizens.State(i);
        if (a != Activity::AtHome && a != Activity::AtWork) continue;
        if (citizens.HasPendingErrand(i)) continue;
        if (hunger_[i] <= params_.hungerShopThreshold) continue;

        const u32 shop = NearestShop(city, citizens.PosX(i), citizens.PosY(i));
        if (ledger.BalanceOf(citizenAccount_[i]) < Money(shopPriceMinor_[shop])) continue;

        citizens.SetPendingErrand(i, shop);
    }
}

// Once a sim-minute. A shop below half its target buys a batch from the government acting
// as wholesaler; goods appear from nothing, the money for them does not. If the full batch
// is beyond the shop's means it buys what it can, which may be nothing. Every shop is then
// repriced from its new stock, whether or not it restocked.
void Economy::TickMarket(Ledger& ledger) {
    const AccountId government = ledger.CentralBank();
    const Money     unitCost   = Money(params_.wholesaleFoodMinor);

    for (const u32 s : shops_) {
        if (wholesaleEnabled_ && shopStock_[s] < params_.shopTargetStock / 2u) {
            u32   units = params_.shopRestockBatch;
            Money cost  = Money(unitCost.Raw() * static_cast<i64>(units));

            if (!ledger.Transfer(firmAccount_[s], government, cost)) {
                // Could not afford the batch: buy as many whole units as the balance covers.
                // A zero wholesale price never reaches this branch - the full batch would have
                // cost nothing and the transfer above would have succeeded.
                const i64 affordable = ledger.BalanceOf(firmAccount_[s]).Raw() / unitCost.Raw();
                units = (affordable < static_cast<i64>(units)) ? static_cast<u32>(affordable) : units;
                cost  = Money(unitCost.Raw() * static_cast<i64>(units));
                if (units == 0u || !ledger.Transfer(firmAccount_[s], government, cost)) units = 0u;
            }
            shopStock_[s] += units;
        }
        RepriceShop(s);
    }
}

// Once a sim-hour. Every citizen at work is paid by their employer - a Work lot or a Shop -
// and the government then pays each Work lot a contract for that worker-hour. Shops earn
// from sales instead. Both legs are checked: an employer that cannot cover a wage does not
// pay it this hour, and the worker goes unpaid. Payroll runs for the whole population before
// any contract money arrives, so this hour's contracts can never fund this hour's wages.
void Economy::TickWages(const Citizens& citizens, Ledger& ledger) {
    const u32       n          = CitizenCount();
    const AccountId government = ledger.CentralBank();
    const Money     wage       = Money(params_.hourlyWageMinor);
    const Money     contract   = Money(params_.contractPerWorkerHourMinor);

    for (u32 i = 0; i < n; ++i) {
        if (citizens.State(i) != Activity::AtWork) continue;
        const u32       employer = citizens.WorkBuilding(i);
        const AccountId firm     = FirmAccount(employer);
        if (!firm.IsValid()) continue;
        if (ledger.Transfer(firm, citizenAccount_[i], wage)) wagesPaidMinor_ += wage.Raw();
    }

    for (u32 i = 0; i < n; ++i) {
        if (citizens.State(i) != Activity::AtWork) continue;
        const u32 employer = citizens.WorkBuilding(i);
        // Only Work lots hold contracts. A shop is a firm too, but nothing is "made" there
        // for the government to buy; it lives on its counter.
        if (!IsFirm(employer) || EconomyIsShop(shops_, employer)) continue;
        // Result deliberately unchecked beyond the boolean: a government that cannot pay its
        // contractors is a legitimate fiscal state, and the firm simply is not paid.
        static_cast<void>(ledger.Transfer(government, firmAccount_[employer], contract));
    }
}

// Every tick. A citizen who has just reached the counter buys one unit if the shelf is not
// empty and they can pay the full price. The price splits into the shop's share and the
// sales tax, and the two move as two transfers so that every cent lands somewhere and the
// rounding inside ApplyRate can never create or destroy one (see the note in Money.h).
void Economy::TickPurchases(const Citizens& citizens, Ledger& ledger) {
    const u32       n          = CitizenCount();
    const AccountId government = ledger.CentralBank();
    const i32       taxRate    = static_cast<i32>(params_.salesTaxBasisPoints);

    for (u32 i = 0; i < n; ++i) {
        if (!citizens.JustArrivedAtShop(i)) continue;

        const u32 shop = citizens.PendingErrand(i);
        // The errand was set by TickErrands with a shop index, and Citizens keeps it readable
        // through the visit. Anything else is a broken contract, not a slow day.
        LC_ASSERT_MSG(shop < static_cast<u32>(firmAccount_.size()),
                      "Economy::TickPurchases: arrival at a building the economy does not know");
        if (!firmAccount_[shop].IsValid()) continue;   // not a firm at all: nothing to buy
        if (shopStock_[shop] == 0u) continue;          // empty shelf: they walked for nothing

        const Money price = Money(shopPriceMinor_[shop]);
        if (ledger.BalanceOf(citizenAccount_[i]) < price) continue;   // cannot afford it today

        const Money tax = price.ApplyRate(taxRate);
        const Money net = price - tax;

        if (!ledger.Transfer(citizenAccount_[i], firmAccount_[shop], net)) continue;
        const bool taxed = ledger.Transfer(citizenAccount_[i], government, tax);
        LC_ASSERT_MSG(taxed, "Economy::TickPurchases: tax leg failed after the balance covered the price");

        shopStock_[shop] -= 1u;
        hunger_[i] = EconomyLower(hunger_[i], params_.hungerRestoredByFood);
        retailSalesMinor_ += price.Raw();
        taxMinor_         += tax.Raw();
    }
}

// Chebyshev distance over shops_, which is in ascending building order; a strict comparison
// keeps the lowest index on a tie. Requires at least one shop - callers check.
u32 Economy::NearestShop(const City& city, i32 xMetres, i32 yMetres) const {
    LC_ASSERT_MSG(!shops_.empty(), "Economy::NearestShop: the city has no shops");
    u32 best  = shops_[0];
    i64 bestD = 0;
    for (std::size_t k = 0; k < shops_.size(); ++k) {
        const Building& b  = city.At(shops_[k]);
        const i64       dx = EconomyAbsI64(static_cast<i64>(b.centreX) - static_cast<i64>(xMetres));
        const i64       dy = EconomyAbsI64(static_cast<i64>(b.centreY) - static_cast<i64>(yMetres));
        const i64       d  = (dx > dy) ? dx : dy;
        if (k == 0 || d < bestD) {
            bestD = d;
            best  = shops_[k];
        }
    }
    return best;
}

// Inventory pricing: price = base * target / max(stock, target/8), so a full shelf sells at
// base, a half-empty one at double, and an empty one at the 8x ceiling. Computed in i64 with
// the division LAST so the integer result is exact, and clamped to [base/2, base*8] so a shop
// swimming in stock still charges something and a bare one cannot charge infinity.
void Economy::RepriceShop(u32 building) {
    const i64 base   = params_.baseRetailFoodMinor;
    const i64 target = static_cast<i64>(params_.shopTargetStock);
    const i64 stock  = static_cast<i64>(shopStock_[building]);

    i64 floorStock = target / 8;
    if (floorStock < 1) floorStock = 1;                 // never divide by zero for a tiny target
    const i64 denom = (stock > floorStock) ? stock : floorStock;

    i64 price = (base * target) / denom;
    const i64 lo = base / 2;
    const i64 hi = base * 8;
    if (price < lo) price = lo;
    if (price > hi) price = hi;
    shopPriceMinor_[building] = price;
}

// ---------------------------------------------------------------------------------------
AccountId Economy::CitizenAccount(u32 i) const {
    LC_ASSERT_MSG(i < CitizenCount(), "Economy::CitizenAccount: index out of range");
    return citizenAccount_[i];
}

Money Economy::CitizenBalance(u32 i, const Ledger& ledger) const {
    return ledger.BalanceOf(CitizenAccount(i));
}

u16 Economy::Hunger(u32 i) const {
    LC_ASSERT_MSG(i < CitizenCount(), "Economy::Hunger: index out of range");
    return hunger_[i];
}

u16 Economy::Fatigue(u32 i) const {
    LC_ASSERT_MSG(i < CitizenCount(), "Economy::Fatigue: index out of range");
    return fatigue_[i];
}

bool Economy::IsFirm(u32 building) const {
    return building < static_cast<u32>(firmAccount_.size()) && firmAccount_[building].IsValid();
}

AccountId Economy::FirmAccount(u32 building) const {
    return IsFirm(building) ? firmAccount_[building] : AccountId{};
}

Money Economy::FirmBalance(u32 building, const Ledger& ledger) const {
    return IsFirm(building) ? ledger.BalanceOf(firmAccount_[building]) : Money::Zero();
}

u32 Economy::ShopStock(u32 building) const {
    return IsFirm(building) ? shopStock_[building] : 0u;
}

Money Economy::ShopPrice(u32 building) const {
    return IsFirm(building) ? Money(shopPriceMinor_[building]) : Money::Zero();
}

u32 Economy::HungryCount() const {
    u32 count = 0u;
    for (std::size_t i = 0; i < hunger_.size(); ++i) {
        if (hunger_[i] > params_.hungerShopThreshold) ++count;
    }
    return count;
}

u32   Economy::StarvationEvents() const { return starvationEvents_; }
Money Economy::TotalWagesPaid() const    { return Money(wagesPaidMinor_); }
Money Economy::TotalRetailSales() const  { return Money(retailSalesMinor_); }
Money Economy::TotalTaxCollected() const { return Money(taxMinor_); }

// Mean over shops that have something to sell, truncated. An empty shop's price is a
// ceiling nobody can pay, so it is left out rather than allowed to drag the figure upward.
Money Economy::AverageShopPrice() const {
    i64 sum   = 0;
    i64 count = 0;
    for (const u32 s : shops_) {
        if (shopStock_[s] == 0u) continue;
        sum += shopPriceMinor_[s];
        ++count;
    }
    return (count == 0) ? Money::Zero() : Money(sum / count);
}

u32 Economy::TotalShopStock() const {
    u32 total = 0u;
    for (const u32 s : shops_) total += shopStock_[s];
    return total;
}

// I1 / I7. Everything that decides the future, in index order: accounts, both needs, every
// firm's account, every shelf and every price, and the wholesale switch. The running totals
// are NOT hashed - they are bookkeeping about the past, recomputable from the ledger's
// history, and hashing them would let a stats bug hide behind a hash that still matched.
// shops_ is derived from the city, which hashes itself.
void Economy::HashInto(Hasher& h) const {
    const u32 n = CitizenCount();
    h.U32(n);
    for (u32 i = 0; i < n; ++i) h.Ident(citizenAccount_[i]);
    for (u32 i = 0; i < n; ++i) h.U32(static_cast<u32>(hunger_[i]));
    for (u32 i = 0; i < n; ++i) h.U32(static_cast<u32>(fatigue_[i]));

    const u32 b = static_cast<u32>(firmAccount_.size());
    h.U32(b);
    for (u32 k = 0; k < b; ++k) h.Ident(firmAccount_[k]);
    for (u32 k = 0; k < b; ++k) h.U32(shopStock_[k]);
    for (u32 k = 0; k < b; ++k) h.I64(shopPriceMinor_[k]);

    h.Bool(wholesaleEnabled_);
}

} // namespace lc
