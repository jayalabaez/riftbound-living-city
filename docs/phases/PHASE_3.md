# PHASE 3 — ECONOMY

**Done when:** money conservation holds over a simulated year and prices respond sensibly to
an induced shortage.

**Status: the loop is in; the full phase is not complete.** Conservation over a day and price
response to a shortage are tested; year-long conservation remains below. One good, two
needs, one closed monetary circuit. See D-022 for the shape and why it is this small.

---

## What is in

| Item | State | Where |
|---|---|---|
| Closed-loop money (I3) | **in** — government is the central bank; every other movement is a `Transfer`; conservation asserted every tick in the test | `Economy::Generate`, `test_economy.cpp` |
| Wages | **in** — hourly, employer firm → citizen, only while AtWork | `TickWages` |
| Firms | **in as accounts** — every Work/Shop building has one; funded by government contracts per worker-hour | `firmAccount_` |
| One good, produced | **in** — food, created by the wholesaler (goods are not money) | `TickMarket` |
| Inventory-based prices | **in** — price rises as stock falls; no lookup table | `RepriceShop` |
| Prices respond to shortage | **tested** — wholesale cut off, prices rise; restored, they fall | `test_economy.cpp` |
| Sales tax → government | **in** — 10%, `Money::ApplyRate`, exact to the cent | `TickPurchases` |
| Needs: hunger, fatigue | **in** — integer pressures on the 1 Hz schedule | `TickNeeds` |
| No starvation with money and shops (I11) | **tested** — over a simulated day |  |

## What is not

- **A simulated year.** The conservation test runs a day. The year needs the coarse tick
  hierarchy from D-008 wired so markets run at 1 tick per sim-minute, and then a headless
  run that is fast enough to be a test. Not yet done. At today's 18 µs/tick (D-027) a year
  of fine ticks is 3.2 hours; the coarse schedule is what makes it 30 s.
- **Seven of the eight goods.** Food only.
- **Firms that hire, produce, expand, or fail.** Firms are accounts with a contract income.
  Nobody is hired or fired; nothing goes bankrupt. A firm that cannot make payroll simply
  skips that hour, which is the right behaviour and also the whole extent of it.
- **Labour market, housing market, mortgages, eviction, homelessness.** None.
- **Utilities and the waste chain.** None.
- **Tunables in data (R6).** `EconomyParams` is a plain struct with defaults; it is shaped to
  move to `Data/economy.csv` without a type change, but it has not moved.

## Next

1. Wire the D-008 coarse schedule and run the conservation test over a simulated year.
2. Move `EconomyParams` to `Data/economy.csv` and load it (R6).
3. Second good and a second firm type, so a supply chain exists (raw → food).
4. Rent: households pay a landlord firm monthly. First step toward the housing market.
