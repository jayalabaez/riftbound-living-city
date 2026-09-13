// Invariant tests for the economy.
//
// Not getter tests. What they pin down:
//   * I3  - money is conserved on EVERY tick of a simulated day, and nothing is minted after
//           Generate. This is the promise the whole project made; it is asserted per tick,
//           not sampled;
//   * I11 - nobody starves while they have money and a stocked shop is reachable;
//   * that prices are inventory-driven: cut the wholesale supply and the average price rises,
//           strictly, and falls again when supply returns;
//   * that wages go only to citizens AtWork at the hour boundary, that TotalWagesPaid is the
//           exact sum, and that the government's contract money flows only to Work lots;
//   * that sales tax is the sum of the per-sale tax to the cent, audited sale by sale from
//           outside the economy, with the "walked for nothing" outcomes counted;
//   * that an empty shelf sells nothing and leaves the arriving citizen's hunger unchanged;
//   * that errand decisions obey the stated rule - hungry, can pay, nearest shop by Chebyshev
//           distance with ties to the lower index - checked against the City independently;
//   * I1 / I7 - same seed, identical economy hash; the hash covers stock and prices;
//   * I5  - Tick allocates nothing, with purchases, restocks and payroll all live.
//
// TIME. Like test_citizens.cpp, an "hour" is a 2000-tick window at the top of each sim-hour,
// which keeps a day at ~49k ticks. Unlike test_citizens.cpp the window starts kHourLeadTicks
// BEFORE the hour, so the loop steps ACROSS a real hour boundary (and the minute boundary
// that coincides with it) by advancing, rather than only ever landing on one by seeking.
// Each window therefore contains exactly two minute boundaries and one hour boundary.
//
// TUNING. The shipped hunger rate (3/min) needs ~3 sim-hours to make anyone hungry; a
// compressed day has only 48 minute boundaries, so at that rate nobody would ever shop and
// every test below would pass vacuously. The fixture uses 20/min: hunger crosses the shop
// threshold in the early afternoon, and a citizen whose errand is deferred through the rest
// of a shift still cannot starve (550 + 10 boundaries x 20 = 750 < 1000). These are
// tunables by design (EconomyParams, rule R6); nothing in the economy's logic is changed.
//
// No main() here: the runner supplies one.
#include "TestFramework.h"

#include "livingcity/agents/Citizens.h"
#include "livingcity/core/Clock.h"
#include "livingcity/core/Core.h"
#include "livingcity/core/Money.h"
#include "livingcity/economy/Economy.h"
#include "livingcity/world/City.h"

#include <string>
#include <vector>

using namespace lc;

// Money is compared through Raw() throughout: the framework has no Show(Money), and a
// failure message reading "12345 vs 12346" in minor units is exactly what a conservation
// bug needs.
namespace {

constexpr u64 kCitySeed        = 1337ull;
constexpr i32 kCityBlocks      = 6;
constexpr u32 kTestPopulation  = 128u;
constexpr u64 kHourWindowTicks = 2000ull;
constexpr u64 kHourLeadTicks   = 40ull;
static_assert(kHourLeadTicks > 0ull, "the window must step across the hour boundary, not start on it");
// [top - lead, top + window) holds exactly the minute boundaries at top and top + 1200:
// the lead is shorter than a minute, and the window is longer than one minute but no longer
// than two.
static_assert(kHourLeadTicks < kTicksPerSimMinute &&
              kHourWindowTicks > kTicksPerSimMinute &&
              kHourWindowTicks <= 2ull * kTicksPerSimMinute,
              "a window must contain exactly the two minute boundaries the comments promise");

constexpr u16 kTestHungerPerMinute = 20u;

// The compressed-day fixture parameters. See TUNING above.
EconomyParams TestParams() {
    EconomyParams p;
    p.hungerPerSimMinute = kTestHungerPerMinute;
    return p;
}

// Everything one test needs, built in the order Sim.cpp builds it.
struct World {
    City       city;
    Citizens   citizens;
    Ledger     ledger;
    Economy    economy;
    SimClock   clock;
    u32        shopCount = 0u;
};

void Build(World& w, u64 seed, u32 population, const EconomyParams& params) {
    w.city.Generate(kCitySeed, kCityBlocks, kCityBlocks);
    w.citizens.Generate(seed, w.city, population);
    w.ledger.SetCentralBank(w.ledger.OpenAccount());
    w.economy.Generate(seed, w.city, w.citizens, w.ledger, params);
    w.shopCount = static_cast<u32>(w.city.OfUse(LotUse::Shop).size());
}

// One 20 Hz step in Sim.cpp's order: citizens move, then the economy reacts, then the clock.
void Step(World& w) {
    w.citizens.Tick(w.clock, w.city);
    w.economy.Tick(w.clock, w.city, w.citizens, w.ledger);
    w.clock.Advance();
}

u64 TopOfHour(i32 day, i32 hour) {
    return (static_cast<u64>(day) * static_cast<u64>(kHoursPerDay) + static_cast<u64>(hour)) *
           kTicksPerSimHour;
}

// Seek to just before the top of the hour and run through it. `perTick` is called after
// every step so a test can assert an invariant on every tick without owning the loop.
template <typename PerTick>
void RunHour(World& w, i32 day, i32 hour, PerTick&& perTick) {
    const u64 top   = TopOfHour(day, hour);
    const u64 start = (top >= kHourLeadTicks) ? top - kHourLeadTicks : 0ull;
    w.clock.SetTick(start);
    while (w.clock.CurrentTick() < top + kHourWindowTicks) {
        Step(w);
        perTick(w);
    }
}

template <typename PerTick>
void RunDay(World& w, i32 day, PerTick&& perTick) {
    for (i32 hour = 0; hour < kHoursPerDay; ++hour) RunHour(w, day, hour, perTick);
}

void NoOp(World&) {}

u64 EconomyHash(const Economy& e) {
    Hasher h;
    e.HashInto(h);
    return h.Value();
}

u64 CitizensHash(const Citizens& z) {
    Hasher h;
    z.HashInto(h);
    return h.Value();
}

i64 AbsI64(i64 v) { return (v < 0) ? -v : v; }

// The errand rule, computed from the City alone: nearest shop by Chebyshev distance from the
// citizen's position, ties to the lower building index.
u32 ExpectedNearestShop(const City& city, i32 x, i32 y) {
    const std::vector<u32>& shops = city.OfUse(LotUse::Shop);
    u32 best  = Citizens::kNoErrand;
    i64 bestD = 0;
    for (const u32 s : shops) {
        const Building& b  = city.At(s);
        const i64       dx = AbsI64(static_cast<i64>(b.centreX) - static_cast<i64>(x));
        const i64       dy = AbsI64(static_cast<i64>(b.centreY) - static_cast<i64>(y));
        const i64       d  = (dx > dy) ? dx : dy;
        if (best == Citizens::kNoErrand || d < bestD) {
            best  = s;
            bestD = d;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------------------
// A per-tick audit that re-derives every purchase from the outside and checks the economy's
// totals against it to the cent. Between the citizens' move and the economy's reaction it
// notes who has just arrived at a shop and what they hold; afterwards, a citizen's balance
// can only have moved by the price of one unit (nobody at a counter is AtWork, so no wage
// lands on the same tick), so the gross of every sale is observable exactly, and the tax on
// it must be Money::ApplyRate of that gross.
//
// On a minute boundary the market restocks and reprices BEFORE purchases, so the pre-tick
// price and stock are not what the sale sees; the prediction (stock > 0 and balance >= price
// means a sale at exactly that price) is therefore only checked off the boundary, while the
// balance-delta identity is checked on every tick.
struct Audit {
    std::vector<u32> arrived;
    std::vector<i64> balanceBefore;
    std::vector<u16> hungerBefore;
    std::vector<u32> localStock;        // per building; a scratch copy for same-tick arrivals
    std::vector<u8>  errandBefore;

    u64   sales            = 0;
    u64   walkedForNothing = 0;
    u64   errandsGiven     = 0;
    u64   restockTicks     = 0;
    Money gross            = Money::Zero();
    Money tax              = Money::Zero();

    void Reserve(const World& w) {
        arrived.reserve(w.citizens.Count());
        balanceBefore.assign(w.citizens.Count(), 0);
        hungerBefore.assign(w.citizens.Count(), static_cast<u16>(0u));
        localStock.assign(w.city.Count(), 0u);
        errandBefore.assign(w.citizens.Count(), static_cast<u8>(0u));
    }
};

void StepAudited(World& w, Audit& a) {
    const EconomyParams& p        = w.economy.Params();
    const i32            taxRate  = static_cast<i32>(p.salesTaxBasisPoints);
    const bool           boundary = w.clock.IsMinuteBoundary();
    const u32            n        = w.citizens.Count();

    w.citizens.Tick(w.clock, w.city);

    a.arrived.clear();
    for (u32 b = 0; b < w.city.Count(); ++b) a.localStock[b] = w.economy.ShopStock(b);
    u64 predictedSales = 0;
    for (u32 i = 0; i < n; ++i) {
        a.errandBefore[i] = w.citizens.HasPendingErrand(i) ? static_cast<u8>(1u) : static_cast<u8>(0u);
        if (!w.citizens.JustArrivedAtShop(i)) continue;
        a.arrived.push_back(i);
        a.balanceBefore[i] = w.economy.CitizenBalance(i, w.ledger).Raw();
        a.hungerBefore[i]  = w.economy.Hunger(i);
        const u32 shop = w.citizens.PendingErrand(i);
        LC_CHECK(shop != Citizens::kNoErrand);
        if (shop == Citizens::kNoErrand) continue;
        if (a.localStock[shop] > 0u &&
            Money(a.balanceBefore[i]) >= w.economy.ShopPrice(shop)) {
            --a.localStock[shop];
            ++predictedSales;
        }
    }

    const i64 salesBefore = w.economy.TotalRetailSales().Raw();
    const i64 taxBefore   = w.economy.TotalTaxCollected().Raw();
    const u32 stockBefore = w.economy.TotalShopStock();

    w.economy.Tick(w.clock, w.city, w.citizens, w.ledger);

    Money gross = Money::Zero();
    Money tax   = Money::Zero();
    u64   sales = 0;
    for (const u32 i : a.arrived) {
        const i64 delta = a.balanceBefore[i] - w.economy.CitizenBalance(i, w.ledger).Raw();
        LC_CHECK(delta >= 0);
        if (delta > 0) {
            ++sales;
            gross += Money(delta);
            tax   += Money(delta).ApplyRate(taxRate);
            if (!boundary) {
                // Fed by exactly one unit, and charged exactly the posted price.
                const u16 expect = (a.hungerBefore[i] > p.hungerRestoredByFood)
                                 ? static_cast<u16>(a.hungerBefore[i] - p.hungerRestoredByFood)
                                 : static_cast<u16>(0u);
                LC_CHECK_EQ(w.economy.Hunger(i), expect);
                LC_CHECK_EQ(delta, w.economy.ShopPrice(w.citizens.PendingErrand(i)).Raw());
            }
        } else {
            ++a.walkedForNothing;
            if (!boundary) LC_CHECK_EQ(w.economy.Hunger(i), a.hungerBefore[i]);
        }
    }

    LC_CHECK_EQ(w.economy.TotalRetailSales().Raw() - salesBefore, gross.Raw());
    LC_CHECK_EQ(w.economy.TotalTaxCollected().Raw() - taxBefore, tax.Raw());
    if (!boundary) {
        LC_CHECK_EQ(sales, predictedSales);
        LC_CHECK_EQ(stockBefore - w.economy.TotalShopStock(), static_cast<u32>(sales));
    } else if (w.economy.TotalShopStock() + static_cast<u32>(sales) > stockBefore) {
        ++a.restockTicks;
    }

    // Errand decisions: only on a minute boundary, only to the hungry, only to the nearest
    // shop, and only to somebody who could pay for it at the moment of the decision.
    for (u32 i = 0; i < n; ++i) {
        if (a.errandBefore[i] != 0u || !w.citizens.HasPendingErrand(i)) continue;
        ++a.errandsGiven;
        LC_CHECK(boundary);
        LC_CHECK(w.economy.Hunger(i) > p.hungerShopThreshold);
        const u32 shop = w.citizens.PendingErrand(i);
        LC_CHECK_EQ(shop, ExpectedNearestShop(w.city, w.citizens.PosX(i), w.citizens.PosY(i)));
        LC_CHECK(w.city.At(shop).use == LotUse::Shop);
        LC_CHECK(w.economy.CitizenBalance(i, w.ledger) >= w.economy.ShopPrice(shop));
    }

    a.sales += sales;
    a.gross += gross;
    a.tax   += tax;
    w.clock.Advance();
}

} // namespace

// ---------------------------------------------------------------------------------------
// The money supply is minted once, by the formula in the header, and distributed by
// transfer - so conservation holds from the first instant and the government's balance is
// exactly the reserve it kept back.
LC_TEST(economy_generate_mints_once_and_distributes_by_transfer) {
    World w;
    Build(w, 1ull, kTestPopulation, EconomyParams{});
    const EconomyParams& p = w.economy.Params();

    const u32 works = static_cast<u32>(w.city.OfUse(LotUse::Work).size());
    const u32 shops = w.shopCount;
    LC_CHECK(works > 0u);
    LC_CHECK(shops > 0u);

    const i64 expected = p.initialFirmCapitalMinor * static_cast<i64>(works + shops)
                       + p.initialCitizenCashMinor * static_cast<i64>(kTestPopulation)
                       + p.governmentReserveMinor;
    LC_CHECK_EQ(w.ledger.TotalIssued().Raw(), expected);
    LC_CHECK(w.ledger.ConservationHolds());
    LC_CHECK_EQ(w.ledger.BalanceOf(w.ledger.CentralBank()).Raw(), p.governmentReserveMinor);

    // One account per citizen, one per Work/Shop lot, none for Home/Civic - and every
    // account is distinct, so no two citizens can ever share a purse.
    LC_CHECK_EQ(w.ledger.AccountCount(), 1u + kTestPopulation + works + shops);
    for (u32 i = 0; i < kTestPopulation; ++i) {
        LC_CHECK_EQ(w.economy.CitizenBalance(i, w.ledger).Raw(), p.initialCitizenCashMinor);
        for (u32 j = i + 1u; j < kTestPopulation; ++j) {
            LC_CHECK(w.economy.CitizenAccount(i) != w.economy.CitizenAccount(j));
        }
    }
    for (u32 b = 0; b < w.city.Count(); ++b) {
        const LotUse use  = w.city.At(b).use;
        const bool   firm = (use == LotUse::Work || use == LotUse::Shop);
        LC_CHECK_EQ(w.economy.IsFirm(b), firm);
        if (firm) LC_CHECK_EQ(w.economy.FirmBalance(b, w.ledger).Raw(), p.initialFirmCapitalMinor);
        if (use == LotUse::Shop) {
            LC_CHECK_EQ(w.economy.ShopStock(b), p.shopTargetStock);
            LC_CHECK_EQ(w.economy.ShopPrice(b).Raw(), p.baseRetailFoodMinor);   // full shelf: base
        } else {
            LC_CHECK_EQ(w.economy.ShopStock(b), 0u);
        }
    }
    LC_CHECK_EQ(w.economy.AverageShopPrice().Raw(), p.baseRetailFoodMinor);
    LC_CHECK_EQ(w.economy.TotalShopStock(), p.shopTargetStock * shops);
    LC_CHECK_EQ(w.economy.StarvationEvents(), 0u);
    LC_CHECK_EQ(w.economy.TotalWagesPaid().Raw(), 0);
}

// ---------------------------------------------------------------------------------------
// I3. A whole simulated day - payroll, contracts, restocks, sales and tax all live - and the
// books balance after EVERY tick, with the supply never touched again after Generate.
LC_TEST(economy_i3_money_is_conserved_on_every_tick_of_a_day) {
    World w;
    Build(w, 2026ull, kTestPopulation, TestParams());
    const i64 issued = w.ledger.TotalIssued().Raw();

    u64 ticks = 0;
    RunDay(w, 0, [&](World& x) {
        ++ticks;
        LC_CHECK(x.ledger.ConservationHolds());
        LC_CHECK_EQ(x.ledger.TotalIssued().Raw(), issued);
    });

    // The day was not vacuous: every flow in the loop actually moved money.
    LC_CHECK_EQ(ticks, 24ull * kHourWindowTicks + 23ull * kHourLeadTicks);
    LC_CHECK(w.economy.TotalWagesPaid() > Money::Zero());
    LC_CHECK(w.economy.TotalRetailSales() > Money::Zero());
    LC_CHECK(w.economy.TotalTaxCollected() > Money::Zero());
    LC_CHECK(w.ledger.BalanceOf(w.ledger.CentralBank()) < Money(w.economy.Params().governmentReserveMinor));
}

// I11. With money in every pocket and stocked shops in reach, nobody's hunger reaches
// critical - and the reason is that they actually went and bought food.
LC_TEST(economy_i11_nobody_starves_with_money_and_open_shops) {
    World w;
    Build(w, 2027ull, kTestPopulation, TestParams());

    u16 peakHunger = 0u;
    RunDay(w, 0, [&](World& x) {
        for (u32 i = 0; i < x.citizens.Count(); ++i) {
            if (x.economy.Hunger(i) > peakHunger) peakHunger = x.economy.Hunger(i);
        }
    });

    LC_CHECK_EQ(w.economy.StarvationEvents(), 0u);
    LC_CHECK(peakHunger < 1000u);
    LC_CHECK(peakHunger > w.economy.Params().hungerShopThreshold);   // they did get hungry
    LC_CHECK(w.economy.TotalRetailSales() > Money::Zero());
    LC_CHECK(w.economy.TotalShopStock() > 0u);
    // The precondition held throughout: everybody still has money at the end of the day.
    for (u32 i = 0; i < w.citizens.Count(); ++i) {
        LC_CHECK(w.economy.CitizenBalance(i, w.ledger) > Money::Zero());
    }
}

// A starvation EVENT is the moment hunger reaches critical, counted once per reach - not
// once per minute spent there. Hunger that jumps to critical on the first boundary and is
// never relieved (food restores nothing here) must produce exactly one event per citizen no
// matter how many boundaries follow; anything more is the per-minute bug.
LC_TEST(economy_starvation_is_counted_once_per_reach_not_per_minute) {
    EconomyParams p     = TestParams();
    p.hungerPerSimMinute = 1000u;
    p.hungerRestoredByFood = 0u;

    World w;
    Build(w, 9090ull, 32u, p);
    const u32 n = w.citizens.Count();
    LC_CHECK_EQ(w.economy.StarvationEvents(), 0u);

    u32 boundaries = 0u;
    for (i32 hour = 0; hour < 3; ++hour) {
        RunHour(w, 0, hour, [&](World& x) {
            // The clock has already advanced past the tick just simulated.
            if ((x.clock.CurrentTick() - 1ull) % kTicksPerSimMinute == 0ull) ++boundaries;
        });
    }
    LC_CHECK(boundaries >= 6u);
    LC_CHECK_EQ(w.economy.HungryCount(), n);
    for (u32 i = 0; i < n; ++i) LC_CHECK_EQ(w.economy.Hunger(i), 1000u);
    LC_CHECK_EQ(w.economy.StarvationEvents(), n);
}

// The errand rule's last clause: a citizen who cannot pay today's price is not sent. A
// population with no cash and no wages gets hungry, is never handed an errand, never walks
// to a shop, and buys nothing - and, having no money, is outside I11's promise: they starve,
// and that is counted rather than hidden.
LC_TEST(economy_no_errand_without_the_money_to_pay) {
    EconomyParams p          = TestParams();
    p.initialCitizenCashMinor = 0;
    p.hourlyWageMinor         = 0;
    p.hungerPerSimMinute      = 25u;   // 48 boundaries x 25 reaches critical inside the day

    World w;
    Build(w, 1111ull, 64u, p);
    RunDay(w, 0, [&](World& x) {
        for (u32 i = 0; i < x.citizens.Count(); ++i) LC_CHECK_FALSE(x.citizens.HasPendingErrand(i));
        LC_CHECK_EQ(x.citizens.CountIn(Activity::TravelShop) + x.citizens.CountIn(Activity::AtShop), 0u);
    });
    LC_CHECK(w.economy.HungryCount() > 0u);
    LC_CHECK_EQ(w.economy.TotalRetailSales().Raw(), 0);
    LC_CHECK_EQ(w.economy.TotalShopStock(), w.shopCount * p.shopTargetStock);
    LC_CHECK(w.economy.StarvationEvents() > 0u);
    LC_CHECK(w.ledger.ConservationHolds());
}

// ---------------------------------------------------------------------------------------
// Prices emerge from inventory. Cut the wholesale supply and the average price must rise -
// strictly, and monotonically per shop, because a shelf that is never restocked can only
// empty. Restore it and the average must fall again.
//
// A smaller shelf than the shipped default (80 / 40 rather than 200 / 100) is used so that
// one compressed day of demand is enough to trigger restocks; with the default the shops
// never drop below half and the supply switch would have nothing to switch.
LC_TEST(economy_prices_rise_under_shortage_and_fall_when_supply_returns) {
    EconomyParams p  = TestParams();
    p.shopTargetStock  = 80u;
    p.shopRestockBatch = 40u;

    World w;
    Build(w, 3030ull, kTestPopulation, p);
    const std::vector<u32>& shops = w.city.OfUse(LotUse::Shop);

    RunDay(w, 0, NoOp);
    const Money normal = w.economy.AverageShopPrice();
    LC_CHECK(normal > Money::Zero());
    LC_CHECK(w.economy.TotalRetailSales() > Money::Zero());

    // ---- shortage ----------------------------------------------------------------------
    w.economy.SetWholesaleSupplyEnabled(false);
    std::vector<i64> lastPrice(w.city.Count(), 0);
    for (const u32 s : shops) lastPrice[s] = w.economy.ShopPrice(s).Raw();
    u32       lastStock  = w.economy.TotalShopStock();
    const u32 stockAtCut = lastStock;
    const i64 salesAtCut = w.economy.TotalRetailSales().Raw();

    RunDay(w, 1, [&](World& x) {
        // No supply: stock never rises, and therefore no price ever falls.
        LC_CHECK(x.economy.TotalShopStock() <= lastStock);
        lastStock = x.economy.TotalShopStock();
        for (const u32 s : shops) {
            const i64 price = x.economy.ShopPrice(s).Raw();
            LC_CHECK(price >= lastPrice[s]);
            // However bare the shelf, the price is clamped to [base/2, base*8].
            LC_CHECK(price >= p.baseRetailFoodMinor / 2 && price <= p.baseRetailFoodMinor * 8);
            lastPrice[s] = price;
        }
    });
    const Money scarce = w.economy.AverageShopPrice();
    LC_CHECK(scarce > normal);
    // The day sold food and nothing replaced it.
    LC_CHECK(w.economy.TotalRetailSales().Raw() > salesAtCut);
    LC_CHECK(w.economy.TotalShopStock() < stockAtCut);

    // ---- supply returns ----------------------------------------------------------------
    w.economy.SetWholesaleSupplyEnabled(true);
    u32 restocks = 0u;
    RunDay(w, 2, [&](World& x) {
        if (x.economy.TotalShopStock() > lastStock) ++restocks;
        lastStock = x.economy.TotalShopStock();
    });
    LC_CHECK(restocks > 0u);
    const Money relieved = w.economy.AverageShopPrice();
    LC_CHECK(relieved < scarce);
    LC_CHECK(w.ledger.ConservationHolds());
}

// ---------------------------------------------------------------------------------------
// Repricing is exact and happens on the minute, not on the sale. One unit sold from a full
// shelf leaves the price at base until the next minute boundary, and then moves it to
// base * target / (target - 1) with the division done LAST - 500 * 200 / 199 = 502, where
// dividing first would give 500 * 1 and no shortage under 50% would ever move a price.
LC_TEST(economy_reprice_is_exact_and_happens_only_on_the_minute) {
    World w;
    Build(w, 1212ull, 16u, EconomyParams{});   // shipped hunger rate: nobody shops unprompted
    const EconomyParams& p = w.economy.Params();
    const u32 shop = w.city.OfUse(LotUse::Shop)[0];
    LC_CHECK_EQ(w.economy.ShopStock(shop), p.shopTargetStock);
    LC_CHECK_EQ(w.economy.ShopPrice(shop).Raw(), p.baseRetailFoodMinor);

    // The small hours of the next day, from home: a hand-set errand fires on the next tick.
    // (02:00, not 21:00 - with the afternoon shift band, shifts run as late as 23:00 and
    // 21:00 is no longer an hour when everybody is home.)
    w.clock.SetTick(TopOfHour(1, 2) + 1ull);
    w.citizens.SetPendingErrand(0u, shop);
    u64 guard = 0;
    while (!w.citizens.JustArrivedAtShop(0u) && guard < kHourWindowTicks) {
        Step(w);
        ++guard;
    }
    LC_CHECK(w.citizens.JustArrivedAtShop(0u));
    LC_CHECK_EQ(w.economy.ShopStock(shop), p.shopTargetStock - 1u);
    LC_CHECK_EQ(w.economy.ShopPrice(shop).Raw(), p.baseRetailFoodMinor);   // sold, not repriced

    // Nothing moves the price until the minute boundary, which the next step crosses.
    while (!w.clock.IsMinuteBoundary()) {
        LC_CHECK_EQ(w.economy.ShopPrice(shop).Raw(), p.baseRetailFoodMinor);
        Step(w);
    }
    Step(w);
    const i64 expected = (p.baseRetailFoodMinor * static_cast<i64>(p.shopTargetStock))
                       / static_cast<i64>(p.shopTargetStock - 1u);
    LC_CHECK(expected > p.baseRetailFoodMinor);
    LC_CHECK_EQ(w.economy.ShopPrice(shop).Raw(), expected);
    LC_CHECK_EQ(w.economy.ShopStock(shop), p.shopTargetStock - 1u);   // no restock above half
}

// ---------------------------------------------------------------------------------------
// Wages. At an hour boundary, exactly the citizens AtWork are paid exactly one wage by their
// own employer, nobody else's balance moves, TotalWagesPaid is the sum, and the government
// pays a contract to Work lots only - never to shops, which live on sales.
LC_TEST(economy_wages_are_paid_only_to_citizens_at_work_at_the_hour) {
    World w;
    Build(w, 4040ull, kTestPopulation, EconomyParams{});
    const EconomyParams& p = w.economy.Params();
    const u32 n = w.citizens.Count();

    // A normal morning up to 08:00, at which point some shifts have started and some have
    // not. Stop one tick short of the boundary.
    RunHour(w, 0, 6, NoOp);
    RunHour(w, 0, 7, NoOp);
    w.clock.SetTick(TopOfHour(0, 8) - kHourLeadTicks);
    while (w.clock.CurrentTick() < TopOfHour(0, 8)) Step(w);
    LC_CHECK(w.clock.IsHourBoundary());

    std::vector<i64> citizenBefore(n, 0);
    std::vector<i64> firmBefore(w.city.Count(), 0);
    for (u32 i = 0; i < n; ++i) citizenBefore[i] = w.economy.CitizenBalance(i, w.ledger).Raw();
    for (u32 b = 0; b < w.city.Count(); ++b) firmBefore[b] = w.economy.FirmBalance(b, w.ledger).Raw();
    const i64 govBefore   = w.ledger.BalanceOf(w.ledger.CentralBank()).Raw();
    const i64 wagesBefore = w.economy.TotalWagesPaid().Raw();
    const i64 salesBefore = w.economy.TotalRetailSales().Raw();

    // The boundary tick itself. Who is AtWork is decided by the citizens' move on this tick.
    w.citizens.Tick(w.clock, w.city);
    std::vector<u8> atWork(n, static_cast<u8>(0u));
    u32 paid = 0u;
    for (u32 i = 0; i < n; ++i) {
        atWork[i] = (w.citizens.State(i) == Activity::AtWork) ? static_cast<u8>(1u) : static_cast<u8>(0u);
        if (atWork[i] != 0u) ++paid;
    }
    LC_CHECK(paid > 0u);
    LC_CHECK(paid < n);   // a genuine mix, or the "only" in the test name is untested

    w.economy.Tick(w.clock, w.city, w.citizens, w.ledger);
    w.clock.Advance();

    std::vector<i64> firmExpectedDelta(w.city.Count(), 0);
    i64 contracts = 0;
    for (u32 i = 0; i < n; ++i) {
        const i64 delta = w.economy.CitizenBalance(i, w.ledger).Raw() - citizenBefore[i];
        LC_CHECK_EQ(delta, atWork[i] != 0u ? p.hourlyWageMinor : 0);
        if (atWork[i] == 0u) continue;
        const u32 employer = w.citizens.WorkBuilding(i);
        firmExpectedDelta[employer] -= p.hourlyWageMinor;
        if (w.city.At(employer).use == LotUse::Work) {
            firmExpectedDelta[employer] += p.contractPerWorkerHourMinor;
            contracts += p.contractPerWorkerHourMinor;
        }
    }
    LC_CHECK(contracts > 0);
    for (u32 b = 0; b < w.city.Count(); ++b) {
        LC_CHECK_EQ(w.economy.FirmBalance(b, w.ledger).Raw() - firmBefore[b], firmExpectedDelta[b]);
    }
    LC_CHECK_EQ(w.economy.TotalWagesPaid().Raw() - wagesBefore,
                p.hourlyWageMinor * static_cast<i64>(paid));
    LC_CHECK_EQ(govBefore - w.ledger.BalanceOf(w.ledger.CentralBank()).Raw(), contracts);
    LC_CHECK_EQ(w.economy.TotalRetailSales().Raw(), salesBefore);   // nothing else moved
    LC_CHECK(w.ledger.ConservationHolds());

    // And the tick after is an ordinary one: no second payroll.
    for (u32 i = 0; i < n; ++i) citizenBefore[i] = w.economy.CitizenBalance(i, w.ledger).Raw();
    Step(w);
    for (u32 i = 0; i < n; ++i) {
        LC_CHECK_EQ(w.economy.CitizenBalance(i, w.ledger).Raw(), citizenBefore[i]);
    }
}

// ---------------------------------------------------------------------------------------
// Sales tax, audited sale by sale from outside (see StepAudited): the collected total is the
// sum of Money::ApplyRate over every individual sale, to the cent, and every purchase, every
// wasted trip and every errand decision matched the stated rules on the tick it happened.
LC_TEST(economy_sales_tax_is_the_sum_of_per_sale_tax_to_the_cent) {
    World w;
    Build(w, 5050ull, kTestPopulation, TestParams());
    Audit a;
    a.Reserve(w);

    for (i32 hour = 0; hour < kHoursPerDay; ++hour) {
        const u64 top   = TopOfHour(0, hour);
        const u64 start = (top >= kHourLeadTicks) ? top - kHourLeadTicks : 0ull;
        w.clock.SetTick(start);
        while (w.clock.CurrentTick() < top + kHourWindowTicks) StepAudited(w, a);
    }

    LC_CHECK(a.sales > 0ull);
    LC_CHECK(a.errandsGiven > 0ull);
    LC_CHECK_EQ(w.economy.TotalTaxCollected().Raw(), a.tax.Raw());
    LC_CHECK_EQ(w.economy.TotalRetailSales().Raw(), a.gross.Raw());
    // Note what the identity above does NOT say: it is not ApplyRate over the gross total.
    // Prices move with stock, so sales happen at many different prices and each one rounds
    // on its own; only a per-sale tax can match a per-sale audit to the cent.
    LC_CHECK(a.walkedForNothing + a.sales > 0ull);
    LC_CHECK(w.ledger.ConservationHolds());
}

// ---------------------------------------------------------------------------------------
// An empty shelf. The first visitor buys the only unit; the second walks for nothing: no
// money moves, no stock moves, and their hunger is exactly what it was.
LC_TEST(economy_an_empty_shop_sells_nothing_and_leaves_hunger_unchanged) {
    EconomyParams p  = TestParams();
    p.shopTargetStock  = 1u;     // one unit per shop, and below-half-target can never be true
    p.shopRestockBatch = 1u;

    World w;
    Build(w, 6060ull, kTestPopulation, p);
    w.economy.SetWholesaleSupplyEnabled(false);
    const u32 shop = w.city.OfUse(LotUse::Shop)[0];
    LC_CHECK_EQ(w.economy.ShopStock(shop), 1u);

    // Build up some hunger first so "unchanged" is a real number, not a zero that cannot fall.
    for (i32 hour = 0; hour < 4; ++hour) RunHour(w, 0, hour, NoOp);
    LC_CHECK(w.economy.Hunger(0u) > 0u);
    LC_CHECK(w.economy.Hunger(1u) > 0u);

    // The small hours of the next day: everybody is home, so a hand-set errand fires on the
    // next tick. (02:00, not 21:00 - afternoon shifts run as late as 23:00.)
    w.clock.SetTick(TopOfHour(1, 2) + 1ull);
    LC_CHECK(w.citizens.State(0u) == Activity::AtHome);
    LC_CHECK(w.citizens.State(1u) == Activity::AtHome);

    // First visitor.
    w.citizens.SetPendingErrand(0u, shop);
    u64 guard = 0;
    while (!w.citizens.JustArrivedAtShop(0u) && guard < kHourWindowTicks) {
        Step(w);
        ++guard;
    }
    LC_CHECK(w.citizens.JustArrivedAtShop(0u));
    LC_CHECK_EQ(w.economy.ShopStock(shop), 0u);          // rung up on the arrival tick
    LC_CHECK_EQ(w.economy.TotalRetailSales().Raw(), w.economy.ShopPrice(shop).Raw());
    LC_CHECK_EQ(w.economy.Hunger(0u), 0u);               // fed: 700 restored covers any hunger here

    // Second visitor, to the now-empty shelf.
    w.citizens.SetPendingErrand(1u, shop);
    const i64 salesBefore = w.economy.TotalRetailSales().Raw();
    const i64 taxBefore   = w.economy.TotalTaxCollected().Raw();
    const i64 cashBefore  = w.economy.CitizenBalance(1u, w.ledger).Raw();
    const i64 shopBefore  = w.economy.FirmBalance(shop, w.ledger).Raw();
    guard = 0;
    u16 hungerBefore = 0u;
    for (;;) {
        w.citizens.Tick(w.clock, w.city);
        const bool arrived  = w.citizens.JustArrivedAtShop(1u);
        const bool boundary = w.clock.IsMinuteBoundary();
        hungerBefore = w.economy.Hunger(1u);
        w.economy.Tick(w.clock, w.city, w.citizens, w.ledger);
        if (arrived) {
            // The only thing allowed to touch hunger on this tick is the minute clock, and
            // it clamps at critical exactly as the economy does.
            const u32 raised = static_cast<u32>(hungerBefore) + static_cast<u32>(p.hungerPerSimMinute);
            const u16 expect = boundary ? static_cast<u16>(raised > 1000u ? 1000u : raised) : hungerBefore;
            LC_CHECK_EQ(w.economy.Hunger(1u), expect);
            LC_CHECK(w.economy.Hunger(1u) > 0u);
            break;
        }
        w.clock.Advance();
        if (++guard >= kHourWindowTicks) break;
    }
    LC_CHECK(guard < kHourWindowTicks);
    LC_CHECK_EQ(w.economy.ShopStock(shop), 0u);
    LC_CHECK_EQ(w.economy.TotalRetailSales().Raw(), salesBefore);
    LC_CHECK_EQ(w.economy.TotalTaxCollected().Raw(), taxBefore);
    LC_CHECK_EQ(w.economy.CitizenBalance(1u, w.ledger).Raw(), cashBefore);
    LC_CHECK_EQ(w.economy.FirmBalance(shop, w.ledger).Raw(), shopBefore);
    LC_CHECK(w.ledger.ConservationHolds());
}

// ---------------------------------------------------------------------------------------
// I1 / I7. Same seed, identical economy at every checkpoint of a day that includes trade;
// and the hash covers shelves and prices, shown by two worlds that differ ONLY there.
LC_TEST(economy_same_seed_is_identical_and_hash_covers_stock_and_prices) {
    EconomyParams p  = TestParams();
    p.shopTargetStock  = 80u;
    p.shopRestockBatch = 40u;

    World a;
    World b;
    Build(a, 7070ull, kTestPopulation, p);
    Build(b, 7070ull, kTestPopulation, p);
    const u64 generated = EconomyHash(a.economy);
    LC_CHECK_EQ(generated, EconomyHash(b.economy));

    // Stock and price are hashed EACH ON THEIR OWN, shown by a world that differs from `a`
    // in exactly one of them. Parameters are not hashed, so the only way a different
    // parameter can reach the hash is through the state it produces.
    const u32 shop0 = a.city.OfUse(LotUse::Shop)[0];
    {
        // One unit deeper at the same price: base * 81 / 81 is still base.
        EconomyParams q   = p;
        q.shopTargetStock = p.shopTargetStock + 1u;
        World c;
        Build(c, 7070ull, kTestPopulation, q);
        LC_CHECK_EQ(c.economy.ShopPrice(shop0).Raw(), a.economy.ShopPrice(shop0).Raw());
        LC_CHECK_NE(c.economy.ShopStock(shop0), a.economy.ShopStock(shop0));
        for (u32 i = 0; i < c.citizens.Count(); ++i) LC_CHECK_EQ(c.economy.Hunger(i), a.economy.Hunger(i));
        LC_CHECK_NE(EconomyHash(c.economy), generated);
    }
    {
        // The same shelf at a different price.
        EconomyParams q      = p;
        q.baseRetailFoodMinor = p.baseRetailFoodMinor + 1;
        World c;
        Build(c, 7070ull, kTestPopulation, q);
        LC_CHECK_EQ(c.economy.ShopStock(shop0), a.economy.ShopStock(shop0));
        LC_CHECK_NE(c.economy.ShopPrice(shop0).Raw(), a.economy.ShopPrice(shop0).Raw());
        LC_CHECK_NE(EconomyHash(c.economy), generated);
    }
    {
        // Hunger alone. One step at tick 0: everybody is home, so nobody moves, fatigue is
        // already at its floor, nothing is bought and no shelf is below half. The only state
        // the step can touch is hunger, which rises by one minute's worth.
        World c;
        Build(c, 7070ull, kTestPopulation, p);
        Step(c);
        LC_CHECK_EQ(CitizensHash(c.citizens), CitizensHash(a.citizens));
        for (u32 i = 0; i < c.citizens.Count(); ++i) {
            LC_CHECK_EQ(c.economy.Hunger(i), kTestHungerPerMinute);
            LC_CHECK_EQ(c.economy.Fatigue(i), a.economy.Fatigue(i));
        }
        LC_CHECK_EQ(c.economy.TotalShopStock(), a.economy.TotalShopStock());
        LC_CHECK_EQ(c.economy.ShopPrice(shop0).Raw(), a.economy.ShopPrice(shop0).Raw());
        LC_CHECK_EQ(c.economy.TotalRetailSales().Raw(), 0);
        LC_CHECK_NE(EconomyHash(c.economy), generated);
    }
    {
        // Fatigue alone. With the hunger rate zeroed, one step at 06:00 sends the early
        // shift out of the door, and the only economy state that moves is their fatigue.
        EconomyParams q      = p;
        q.hungerPerSimMinute = 0u;
        World c;
        World d;
        Build(c, 7070ull, kTestPopulation, q);
        Build(d, 7070ull, kTestPopulation, q);
        LC_CHECK_EQ(EconomyHash(c.economy), EconomyHash(d.economy));
        c.clock.SetTick(TopOfHour(0, 6));
        Step(c);
        u32 tired = 0u;
        for (u32 i = 0; i < c.citizens.Count(); ++i) {
            LC_CHECK_EQ(c.economy.Hunger(i), 0u);
            if (c.economy.Fatigue(i) != 0u) ++tired;
        }
        LC_CHECK(tired > 0u);
        LC_CHECK_EQ(tired, c.citizens.CountIn(Activity::TravelWork));
        LC_CHECK_EQ(c.economy.TotalShopStock(), d.economy.TotalShopStock());
        LC_CHECK_EQ(c.economy.TotalRetailSales().Raw(), 0);
        LC_CHECK_NE(EconomyHash(c.economy), EconomyHash(d.economy));
    }
    {
        // Account ids alone. One unrelated account opened before the economy shifts every
        // citizen's and firm's id by one; balances, needs, shelves and prices are identical.
        World c;
        c.city.Generate(kCitySeed, kCityBlocks, kCityBlocks);
        c.citizens.Generate(7070ull, c.city, kTestPopulation);
        c.ledger.SetCentralBank(c.ledger.OpenAccount());
        c.ledger.OpenAccount();
        c.economy.Generate(7070ull, c.city, c.citizens, c.ledger, p);
        LC_CHECK_EQ(c.economy.CitizenAccount(0u).value, a.economy.CitizenAccount(0u).value + 1u);
        LC_CHECK_EQ(c.economy.CitizenBalance(0u, c.ledger).Raw(), a.economy.CitizenBalance(0u, a.ledger).Raw());
        LC_CHECK_EQ(c.economy.TotalShopStock(), a.economy.TotalShopStock());
        LC_CHECK_NE(EconomyHash(c.economy), generated);
    }

    for (i32 hour = 0; hour < kHoursPerDay; ++hour) {
        RunHour(a, 0, hour, NoOp);
        RunHour(b, 0, hour, NoOp);
        LC_CHECK_EQ(EconomyHash(a.economy), EconomyHash(b.economy));
    }
    LC_CHECK_NE(EconomyHash(a.economy), generated);        // the day actually happened
    LC_CHECK(a.economy.TotalRetailSales() > Money::Zero());

    // The supply switch is state.
    a.economy.SetWholesaleSupplyEnabled(false);
    LC_CHECK_NE(EconomyHash(a.economy), EconomyHash(b.economy));

    // Now let the two run in lockstep, a without supply, until b restocks a shelf. Up to that
    // tick every citizen has done exactly the same thing in both worlds (asserted), so the
    // ONLY economy state that differs is one shop's stock and the price derived from it.
    bool diverged = false;
    for (i32 hour = 0; hour < kHoursPerDay && !diverged; ++hour) {
        const u64 top   = TopOfHour(1, hour);
        const u64 start = top - kHourLeadTicks;
        a.clock.SetTick(start);
        b.clock.SetTick(start);
        while (a.clock.CurrentTick() < top + kHourWindowTicks) {
            const u32 bStockBefore = b.economy.TotalShopStock();
            Step(a);
            Step(b);
            if (b.economy.TotalShopStock() > bStockBefore) { diverged = true; break; }
        }
    }
    LC_CHECK(diverged);
    a.economy.SetWholesaleSupplyEnabled(true);           // switch equal again
    LC_CHECK_EQ(CitizensHash(a.citizens), CitizensHash(b.citizens));
    for (u32 i = 0; i < a.citizens.Count(); ++i) {
        LC_CHECK_EQ(a.economy.Hunger(i), b.economy.Hunger(i));
        LC_CHECK_EQ(a.economy.Fatigue(i), b.economy.Fatigue(i));
    }
    LC_CHECK(a.economy.TotalShopStock() < b.economy.TotalShopStock());
    // Shop by shop, not as an average: AverageShopPrice leaves EMPTY shelves out, so if the
    // shelf that b restocked had run dry in a, a's average would drop that shop's 8x ceiling
    // and could sit BELOW b's even though every price in a is at least b's. Every shelf b
    // restocked is shallower in a and dearer for it; every other shelf is identical.
    u32 restocked = 0u;
    for (const u32 s : a.city.OfUse(LotUse::Shop)) {
        if (a.economy.ShopStock(s) == b.economy.ShopStock(s)) {
            LC_CHECK_EQ(a.economy.ShopPrice(s).Raw(), b.economy.ShopPrice(s).Raw());
            continue;
        }
        ++restocked;
        LC_CHECK(a.economy.ShopStock(s) < b.economy.ShopStock(s));
        LC_CHECK(a.economy.ShopPrice(s) > b.economy.ShopPrice(s));
    }
    LC_CHECK(restocked > 0u);
    LC_CHECK_NE(EconomyHash(a.economy), EconomyHash(b.economy));
}

// ---------------------------------------------------------------------------------------
// I11 over a REAL-CLOCK day: 1,728,000 ticks at 20 Hz, every minute and hour boundary
// genuine, nothing compressed. The compressed-day fixture above has only 48 minute
// boundaries, so it can never observe the one thing that decides I11 in this economy - a
// full shift's worth of hunger accruing while an errand waits, because Citizens defers a
// shop trip until shift end and never interrupts work for food.
//
// The parameters are DERIVED from that geometry, not pasted from a run. A citizen can leave
// for work with hunger just under the threshold (or just over it, handed an errand on the
// last minute at home that the shift then defers), work the longest shift Citizens hands
// out, and only then walk to the shop; I11 needs
//
//     threshold + hungerPerMinute x (longest shift + a generous commute)  <  1000.
//
// With one point a minute, a 400 threshold and half an hour allowed for the last minute at
// home plus the walk (the grid needs under one), the worst case is 400 + 540 + 30 = 970,
// and a meal restores the full scale, so a fed citizen's clock restarts from zero. The
// SHIPPED defaults in Economy.h do not satisfy the inequality (550 + 3 x 540 = 2170) and
// this test fails under them: measured on the game's own 6x6 / 1500-citizen configuration,
// every citizen starves once a day, at work, with money and a stocked shop in reach. That is
// a defect in the defaults (integrator-owned) or in the no-lunch-break rule (Citizens), not
// in the economy's logic - which is what this test pins.
LC_TEST(economy_i11_holds_over_a_real_clock_day_when_a_shift_cannot_outrun_the_threshold) {
    constexpr u32 kLongestShiftMinutes  = 9u * static_cast<u32>(kMinutesPerHour);   // Citizens: 8 or 9 hours
    constexpr u32 kCommuteMarginMinutes = 30u;                                       // the grid needs < 1
    constexpr u16 kRatePerMinute        = 1u;
    constexpr u16 kThreshold            = 400u;
    static_assert(kThreshold + kRatePerMinute * (kLongestShiftMinutes + kCommuteMarginMinutes) < 1000u,
                  "the fixture must leave a full shift's hunger short of critical");

    EconomyParams p;
    p.hungerPerSimMinute   = kRatePerMinute;
    p.hungerShopThreshold  = kThreshold;
    p.hungerRestoredByFood = 1000u;

    World w;
    Build(w, 2028ull, 32u, p);
    w.clock.SetTick(0ull);

    u16 peakHunger        = 0u;
    u64 ticksWithShoppers = 0ull;
    for (u64 t = 0; t < kTicksPerSimDay; ++t) {
        Step(w);
        if (w.clock.IsHourBoundary()) {
            for (u32 i = 0; i < w.citizens.Count(); ++i) {
                if (w.economy.Hunger(i) > peakHunger) peakHunger = w.economy.Hunger(i);
            }
        }
        if (w.citizens.CountIn(Activity::AtShop) != 0u) ++ticksWithShoppers;
    }

    LC_CHECK_EQ(w.economy.StarvationEvents(), 0u);
    LC_CHECK(peakHunger < 1000u);
    LC_CHECK(peakHunger > p.hungerShopThreshold);          // they got hungry at work
    LC_CHECK(ticksWithShoppers > 0ull);                    // and they went shopping
    LC_CHECK(w.economy.TotalRetailSales() > Money::Zero());
    LC_CHECK(w.economy.TotalWagesPaid() > Money::Zero());  // a real day: payroll ran
    LC_CHECK(w.ledger.ConservationHolds());
    for (u32 i = 0; i < w.citizens.Count(); ++i) {
        LC_CHECK(w.economy.CitizenBalance(i, w.ledger) > Money::Zero());
    }
}

// ---------------------------------------------------------------------------------------
// I5. The 20 Hz path allocates nothing, measured across an afternoon in which errands are
// handed out, purchases ring up, shelves restock and payroll runs.
LC_TEST(economy_tick_allocates_nothing) {
    if (!AllocTrackingEnabled()) {
        // Tracking is compiled out (e.g. under UBT). The loop still runs, so an assert or a
        // crash would still be caught here.
        LC_CHECK(true);
    }

    World w;
    Build(w, 8080ull, kTestPopulation, TestParams());
    // Warm up: the morning, so that by early afternoon hunger is at the threshold.
    for (i32 hour = 0; hour < 13; ++hour) RunHour(w, 0, hour, NoOp);
    const i64 salesBefore = w.economy.TotalRetailSales().Raw();

    const NoAllocScope guard("Economy::Tick steady state");
    // Deliberately no LC_CHECK inside this region - the failure macros build a std::string.
    for (i32 hour = 13; hour < 21; ++hour) {
        const u64 top   = TopOfHour(0, hour);
        w.clock.SetTick(top - kHourLeadTicks);
        while (w.clock.CurrentTick() < top + kHourWindowTicks) Step(w);
    }
    if (AllocTrackingEnabled()) {
        LC_CHECK_EQ(guard.AllocationsSinceStart(), 0ull);
        LC_CHECK(guard.Clean());
    }
    // The measured window genuinely contained trade and payroll.
    LC_CHECK(w.economy.TotalRetailSales().Raw() > salesBefore);
    LC_CHECK(w.economy.TotalWagesPaid() > Money::Zero());
    LC_CHECK(w.ledger.ConservationHolds());
}
