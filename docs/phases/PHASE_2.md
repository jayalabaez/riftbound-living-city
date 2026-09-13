# PHASE 2 — CITIZENS

**Done when:** 10,000 citizens wake, commute, work, eat, and sleep — and one can be followed
all day.

**Status: partially met at 1,500.** Citizens commute, work, shop and go home. Needs exist but
are owned by the economy. One citizen is followed on the HUD. Nobody sleeps yet.

---

## What is in

| Item | State | Where |
|---|---|---|
| Struct-of-arrays population | **in** — every field its own vector, sized once in `Generate` | `Sim/src/agents/Citizens.cpp` |
| Home + job + shift for everyone (I4) | **in** — a citizen without either is `LC_FATAL`, never skipped | `Citizens::Generate` |
| Daily routine on the road grid | **in** — L-shaped walks on road centrelines, integer-exact, never through a building | `citizens_never_walk_through_a_building` |
| Two shift bands | **in** — 60% start 06–10, 40% start 11–14; streets have commuters and shoppers through the day, not only at two rush hours (D-026) | `Citizens::Generate` |
| Indoors is not drawn | **in** — AtWork / AtShop citizens are inside a building; only AtHome and walkers are rendered (D-026) | `ALivingCityWorldView` |
| Errands | **in** — the economy decides whether and where; citizens execute the walk | `SetPendingErrand`, `Activity::TravelShop/AtShop` |
| Follow one citizen | **in** — index, activity, position, money, hunger, fatigue on the HUD | `SimConfig::followedCitizen`, `Snapshot::followed*` |
| Day/night cycle | **clock in, lighting not** — the calendar exists; the sun does not move |  |
| Snapshot cap | 2,560 rendered; the sim can hold more, the surplus is simulated but not drawn | `kMaxSnapshotCitizens` |

## What is not

- **10,000.** The population is 1,500. The SoA layout is the one that has to survive to 50k,
  but nothing has measured it past 1,500. Raise it and profile before claiming the number.
- **Three LOD tiers.** Every citizen is Tier 0 today: ticked every tick, rendered if in the
  snapshot. The tier system, D-001 (Mass as the crowd renderer) and I6 (lossless promotion)
  are all still open.
- **Needs in `agents/`.** Hunger and fatigue live in `Economy` because that is what consumes
  them. The nine-need model, utility scoring and GOAP planning are not started. Today's
  behaviour is a fixed schedule with one errand hook.
- **Sleep.** Fatigue is restored at home but nobody "sleeps" — it is a number, not a state.
- **Staggered departures within the hour.** Everyone on a given shift still leaves on the
  same tick (the two bands spread departures across the day, not within an hour). Tried and
  reverted; the citizen tests model an hour as 2,000 ticks and would have needed a real
  clock first. See D-018.
- **Navigation.** There is no navmesh; there is a road grid. That is enough for a grid city
  and nothing else.

## Next

1. Give the citizen tests a real clock, then bring back staggered departures (D-018).
2. Move needs into `agents/` as the nine-need model and let the economy read them.
3. Utility scoring over a small goal set (eat, sleep, work, shop) replacing the fixed schedule.
4. Raise the population and measure — the tick budget in CLAUDE.md §4 is 3.5 ms for agents.
