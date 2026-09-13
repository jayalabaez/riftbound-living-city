# Planetary city simulation boundary

`Sim/include/livingcity/sim/PlanetaryCity.h` is the pure C++ boundary used by
Voyager planetary settlements. The existing standalone `Simulation`, `Citizens`
and one-good `Economy` remain unchanged. The new core reuses their checked integer
`Money`, central-bank `Ledger`, deterministic hashing and test harness.

The host supplies a sorted-compatible set of actual building indices and their
economic roles. Generation rejects duplicate indices and layouts without homes,
workplaces or shops. Each city has 3,600 persistent residents by default. A resident
has an identity, real home/work indices, an explicit unemployment state, staggered
shift, account, inventory, nine need pressures and legal record. Health is a
pressure too: zero healthy, 1,000 dead.

`Step()` advances one 20 Hz step. Twenty disjoint resident slices distribute one
game minute of needs and activity work over each real second; markets update once
per game minute. No steady-state core step allocates memory or performs IO. The
host owns the fixed-step worker, presentation, server validation and persistence.

## Shared resident commands

Externally controlled residents use the same purchase, consumption, wage, tax,
rent, utilities and evidence functions as autonomous residents. Controlled input
suppresses autonomous decisions, while needs still age. Embodiment is separate:
an embodied autonomous resident receives a `goalBuilding`, but the host must report
arrival through `SetPresence` before work or shopping can transact. Changing these
presentation traits never regenerates an identity or account.

Commands carry a strictly increasing nonzero sequence per resident. Invalid and
duplicate commands leave economic state unchanged. Purchases validate location,
quantity, stock, inventory capacity and the entire price including tax before any
transfer. Working requires actual presence and 60 accumulated game minutes; job
applications cannot produce an instant wage. Rent is a daily transfer to the
building owner. Utilities accrue invoices; prolonged nonpayment disconnects them
and prevents washing until the bill is paid at home.

Eight goods have inventories and prices derived from stock scarcity. Funded
wholesale orders replenish them, with material inputs for advanced goods. Existing
government reserves fund service/output payroll contracts. Every monetary movement
is a ledger transfer; physical goods and money are distinct. Consumed goods create
waste; recycling recovers raw materials. `SellRawMaterials` expects units already
validated and escrowed by the authoritative host from the mining inventory. It
pays from the shop account and never creates currency.

Observed offenses require confidence at least 600/1000 and a new evidence event
ID. Accepted observations create fines and a retained record. Fines are paid into
the city budget. `Kill` retains the complete identity and ledger row. Only an
explicit `EmergencyRecovery` revives a resident; it charges available funds up to
1,000 minor units to the government and preserves the record.

## Travel and save invariants

`SwapResident` moves an ordinary resident between existing reserved slots in two
cities. Identity, credits, needs, items and fines travel together. Valid destination
home/work addresses replace the previous city's addresses; interplanetary property
ownership is not modeled. Paired central-bank clearing moves the difference between
the two resident account balances. Each city's issued supply may change, while the
combined issued supply remains exactly conserved. No arrival grants a new wallet.

`SaveDelta` returns bytes only. A seed/config/layout fingerprint regenerates the
baseline, then changed resident records, shop inventories and account deltas restore
divergence. Issued supply and cumulative accounting are included. Checksums, strict
counts/order, state validation, conserved balances and a final world hash protect
loading. Invalid, truncated or incompatible bytes leave the original city untouched.
No terrain, textures, meshes or unmodified baseline residents are serialized here.

Voyager autosaves clone the live pure city objects while draining completed command
results in one brief host lock. Rejected mineral trades are refunded before the
matching cargo snapshot is copied. Delta encoding then runs on a background worker.
The game thread serializes the save object, and a single joinable worker writes its
immutable bytes. Autosave requests coalesce while a write is in progress. Explicit
save, logout and system arrival finish earlier writes before capturing and writing
fresh state, so an older asynchronous save cannot overwrite a newer explicit save.
The save object remains a reflected property until its write finishes; background
jobs hold copied data rather than actors or save objects.

## Verification and limits

`test_planetary_city.cpp` tests 100,000-step deterministic histories and exact money
conservation, valid addresses, atomic/replayed purchases, actual work time, embodied
arrival, evidence, death, emergency recovery, scarcity prices, bills, recycling,
interplanetary wallet continuity, delta round trips and corrupt input rejection.
The full suite passed 205 tests after integration. The standalone 54,000-resident,
15-city allocation test measured approximately 0.03 ms for a combined step on the
development PC; this excludes engine synchronization, navigation and rendering.

This is an initial planetary city life loop, not completion of the full Living City
brief. Firms have accounts, payroll and inventory production rather than a complete
competitive business lifecycle. Utility billing is aggregated, collection has no
truck routes, and evidence creates fines rather than a court/jail simulation.
Mortgages, evictions, transport networks, disasters, structural destruction and
50,000 rendered crowd actors are not represented by this core.
