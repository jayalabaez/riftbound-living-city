// LIVING CITY — the economy.
//
// R1: zero Unreal types. R3: deterministic, integer-only. R4: the player is an NPC — there is
// no player account, no player wage, no player price. Whatever the player will one day do,
// they will do through exactly these code paths.
//
// This is the Phase 3 skeleton, deliberately small and legible:
//
//   ONE closed monetary loop.  Money is created only by the central bank (the government)
//   and only ever moves by transfer. Invariant I3 must hold on every single tick.
//
//       government --contracts--> firms --wages--> citizens --food--> shops
//            ^                                                          |
//            +--------------- wholesale food + sales tax ---------------+
//
//   ONE good. Food. It is produced (created from nothing - goods are not money) by the
//   government acting as wholesaler, sold to shops, and eaten by citizens.
//
//   TWO needs. Hunger and fatigue, each a pressure from 0 (satisfied) to 1000 (critical).
//   Hunger drives shopping; fatigue is restored at home. Both are INTEGER counters that move
//   on the 1 Hz schedule, never floats, never per frame.
//
//   PRICES EMERGE. A shop's food price is a function of its own inventory - low stock, higher
//   price - never a lookup table. Cut off the wholesale supply and prices must rise. There is
//   a test for that.
//
// Layering (docs/ARCHITECTURE.md section 3): economy depends on agents and core, never the
// reverse. The economy DECIDES that a citizen should shop and WHERE; Citizens executes the
// walk. That is why SetPendingErrand exists on Citizens and why nothing in agents/ includes
// this header.
#pragma once

#include "livingcity/core/Core.h"
#include "livingcity/core/Clock.h"
#include "livingcity/core/Money.h"
#include "livingcity/core/Rng.h"
#include "livingcity/world/City.h"
#include "livingcity/agents/Citizens.h"

#include <vector>

namespace lc {

// Tunables. Plain integers in minor units (cents) and pressure points, so they can move to
// Data/economy.csv in a later phase without changing a type (rule R6).
struct EconomyParams {
    // Money the government mints at generation and how it is distributed.
    i64 initialFirmCapitalMinor     = 5000000;   // $50,000 per firm
    i64 initialCitizenCashMinor     = 20000;     // $200 per citizen
    i64 governmentReserveMinor      = 100000000; // $1,000,000 held back

    // Labour. Paid once per sim-hour to every citizen who is AtWork at the hour boundary.
    i64 hourlyWageMinor             = 1800;      // $18/hour

    // Government contracts: what a Work firm is paid per worker-hour for its output. This is
    // the only way money reaches firms other than sales, so it is what makes wages payable.
    i64 contractPerWorkerHourMinor  = 2400;      // $24 per worker-hour

    // Food.
    i64 wholesaleFoodMinor          = 250;       // shops buy at $2.50
    i64 baseRetailFoodMinor         = 500;       // shops sell at $5.00 when stock is normal
    u32 shopTargetStock             = 200;       // inventory the price is "normal" at
    u32 shopRestockBatch            = 100;       // units bought per restock
    u32 salesTaxBasisPoints         = 1000;      // 10%

    // Needs, in pressure points on the 1 Hz schedule.
    u16 hungerPerSimMinute          = 3;         // 0 -> 1000 in about 5.5 sim-hours
    u16 hungerRestoredByFood        = 700;
    u16 hungerShopThreshold         = 550;       // go shopping above this
    u16 fatiguePerSimMinuteAwake    = 2;
    u16 fatigueRestoredPerSimMinuteAtHome = 6;
};

class LC_API Economy {
public:
    // Opens an account for every citizen and every Work/Shop building, mints the initial
    // money supply through the ledger's central bank, and distributes it. Call once, after
    // Citizens::Generate. Deterministic.
    //
    // I3: total issued after this call equals the sum of every balance, and stays equal.
    void Generate(u64 worldSeed, const City& city, const Citizens& citizens, Ledger& ledger,
                  const EconomyParams& params);

    // One 20 Hz step. Almost all of the work is gated onto the D-008 tick hierarchy:
    //   every second   - needs move
    //   every minute   - errand decisions, shop restocking, price adjustment
    //   every hour     - wages and government contracts
    //   every tick     - purchases by citizens who have just arrived at a shop
    // May call citizens.SetPendingErrand. Must not allocate (I5).
    void Tick(const SimClock& clock, const City& city, Citizens& citizens, Ledger& ledger);

    // ---- per-citizen ----------------------------------------------------------------
    AccountId CitizenAccount(u32 i) const;
    Money     CitizenBalance(u32 i, const Ledger& ledger) const;
    u16       Hunger(u32 i) const;     // 0 satisfied .. 1000 starving
    u16       Fatigue(u32 i) const;    // 0 rested    .. 1000 exhausted

    // ---- per-firm (indexed by City building index; non-firms return defaults) --------
    bool      IsFirm(u32 building) const;
    AccountId FirmAccount(u32 building) const;
    Money     FirmBalance(u32 building, const Ledger& ledger) const;
    u32       ShopStock(u32 building) const;     // 0 for non-shops
    Money     ShopPrice(u32 building) const;     // current retail price; Zero for non-shops

    // ---- aggregates, for the HUD and the tests ---------------------------------------
    u32   CitizenCount() const { return static_cast<u32>(hunger_.size()); }
    u32   HungryCount() const;                    // hunger above the shop threshold
    u32   StarvationEvents() const;               // times any citizen's hunger hit 1000 (I11)
    Money TotalWagesPaid() const;
    Money TotalRetailSales() const;
    Money TotalTaxCollected() const;
    Money AverageShopPrice() const;               // across shops with stock; Zero if none
    u32   TotalShopStock() const;

    // Deliberately cuts the wholesale supply so the shortage test can watch prices react.
    // Off by default; the game never calls this. A real disaster system will, in Phase 6.
    void  SetWholesaleSupplyEnabled(bool enabled) { wholesaleEnabled_ = enabled; }

    const EconomyParams& Params() const { return params_; }

    void HashInto(Hasher& h) const;
    void Clear();

private:
    EconomyParams params_;
    bool          wholesaleEnabled_ = true;

    // Citizens, parallel to Citizens' own arrays.
    std::vector<AccountId> citizenAccount_;
    std::vector<u16>       hunger_;
    std::vector<u16>       fatigue_;

    // Firms, indexed by building. Non-firm buildings hold an invalid AccountId.
    std::vector<AccountId> firmAccount_;
    std::vector<u32>       shopStock_;
    std::vector<i64>       shopPriceMinor_;

    // Building indices of every shop, ascending, so "nearest shop" is a short scan.
    std::vector<u32>       shops_;

    // Running totals. Derived, so NOT hashed - see HashInto.
    i64 wagesPaidMinor_   = 0;
    i64 retailSalesMinor_ = 0;
    i64 taxMinor_         = 0;
    u32 starvationEvents_ = 0;

    void TickNeeds(const Citizens& citizens);
    void TickErrands(const City& city, Citizens& citizens, const Ledger& ledger);
    void TickMarket(Ledger& ledger);
    void TickWages(const Citizens& citizens, Ledger& ledger);
    void TickPurchases(const Citizens& citizens, Ledger& ledger);
    u32  NearestShop(const City& city, i32 xMetres, i32 yMetres) const;
    void RepriceShop(u32 building);
};

} // namespace lc
