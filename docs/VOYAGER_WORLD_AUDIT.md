# Voyager production audit

Audit opened 2026-09-13 against `432724f31e295a0b048243ddc25c129025ed655e`,
Unreal 5.8.2, branch `codex/public-release`. This is the first deliverable of the owner's
master production brief. It is a source audit with identified runtime evidence, not a
claim that every procedural seed or every future phase has been validated.

## Executive summary

The game has a working continuous spherical flight scene, a persistent planetary city
economy, cooperative authority, bounded visible populations and useful integration tests.
The architecture is worth preserving. Fifteen `PlanetaryCity` cores own 54,000 resident
records; the separate Living City mode does not run behind Voyager. Existing Voyager
actors still own flight, observation, combat and building integrity under D-030–D-032.
New reusable decision rules should move into the pure core rather than expand that exception.

The biggest quality gap is coherence: terrain is additive noise with one climate per
planet; weather coverage is static; lighting has no exposure adaptation; cities use one
repeated grid; wildlife is a proximity population rather than a persistent ecology.
There is no convincing ambient soundscape. These are implementation limits, not missing
plugins. Installing a marketplace package does not resolve their ownership or integration.

Two immediately actionable defects are verified in source: landing snaps to analytical
terrain after only an altitude check, and disembarking chooses one unchecked side position.
These can place a ship or player in obstacles. Packaged linker warnings also have a clear
cause: shared-library import/export macros are enabled for monolithic game targets.
Fix these first, then build measured visual improvements on one reference planet.

## Evidence and rating key

F = functionality; R = realism; V = visual quality; S = scalability. Ratings are qualitative
source/visual assessments: solid, partial, weak or absent. Stability is reported as observed
tests or an unverified risk. MP = multiplayer; P = persistence. "Tested" names a bounded
fixture, never a universal guarantee. Existing captures include the 0.9.1 police exterior
and earlier nature/cosmos/interior fixtures. Previously passing audits remain historical
until rerun after changes affecting their coverage.

Inspected contracts and decisions, both play guides, build guide, all seven phase files,
project/plugin descriptors, configuration and module rules. Reviewed the four modules'
entry points/boundaries and the main Voyager world, ship, character, settlement, citizen,
city adapter, vegetation, wildlife, survival, law, drone, destruction, cosmos, dialogue,
HUD and audit paths. Provenance for nature, characters, ships, security, cosmos and terrain
is under `Art/`. Some older docs describe pre-integration architecture; source and newer
D-030–D-032 take precedence.

## System matrix

| System / source evidence | F | R | V | S | Stability / problem | MP | P |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Planet generation — `VoyagerData.h` | solid | partial | partial | solid | Hash seeds; 600–900 km radii; fixed system arrangement | shared seed | seed |
| Terrain — `VoyagerWorld.cpp` | solid | weak | partial | partial | Cube-sphere LOD; additive noise; synchronous collision cooking | view-dependent patches | seed |
| Foliage — `VoyagerVegetation.cpp` | solid | partial | partial | solid | HISM, three LODs, slope/city exclusion; capped cells | local views, shared seed | seed |
| Biomes — `VoyagerData.h` | partial | weak | partial | solid | One family per planet; no climate field | seed | seed |
| Weather — `VoyagerWorld.cpp` | partial | weak | partial | partial | Static coverage/erosion; no weather simulation or precipitation | no evolving state | seed parameters |
| Atmosphere — `BuildLighting/UpdateAtmosphere` | solid | partial | partial | partial | Native spherical scattering; fixed sun; parameter arrays | local camera | regenerated |
| Clouds — native volume + distant shells | partial | partial | partial | partial | Different near/orbit representations; continuity needs measured QA | local rendering | seed parameters |
| Star systems — `VoyagerData/World` | solid | abstraction | partial | solid | Fixed five centers; procedural identities; anomaly approximation | shared system | current system |
| Interplanetary travel — `VoyagerShip` | solid | abstraction | partial | solid | Continuous flight tested; star jump remains a short transition | host/client tested | destination |
| Spacecraft — `VoyagerShip/ShipVisuals` | solid | partial | partial | solid | Original three-LOD airframe; input-predicted flight; weak mass/audio cues | prediction/reconciliation | upgrades/cargo |
| Landing — `ServerInteract` | partial | weak | partial | solid | Altitude-only snap; no footprint/slope/speed clearance gate | server owns | landing site only |
| Disembark — `VoyagerGameMode::LeaveShip` | partial | weak | partial | solid | One unchecked side point; forced spawn fallback | server owns | no exact position |
| City generation — `VoyagerSettlement` | solid | weak | partial | partial | 60 buildings per site; repeating 8x8 grid; synchronous construction | shared seed | seed + damage |
| Buildings — `VoyagerSettlement` | solid | partial | partial | partial | 3–18 floors; stairs/roof; fixed modular silhouettes; limited tested seeds | local geometry | damage flags |
| Interiors — settlement role layouts | solid | partial | partial | partial | Six room archetypes; furniture/access routes; no full apartment planning | local geometry | seeded |
| Citizens — `VoyagerCitizen/Visuals` | solid | partial | partial | partial | Skinned CC0 derivatives, scripted gait; actor-iterator avoidance | bounded replicated actors | core identity/death |
| Population virtualization — city adapter | solid | partial | n/a | solid | 54,000 records versus 72 embodied; no VAT/Mass crowd tier | core server only | city archive |
| Jobs — `PlanetaryCity` | solid | abstraction | UI | solid | Real presence required for wages; no full labor market | authority commands | yes |
| Economy — `PlanetaryCity` | solid | abstraction | UI | solid | Eight goods; exact transfers; conservation tests; no full supply-chain economy | authority commands | yes |
| Shops — city core + UI | solid | abstraction | partial | solid | Real stock and location; furnishings do not mirror every stock unit | authority commands | yes |
| Needs — city core + survival | solid | abstraction | UI | solid | Nine needs; medical ordering regressions; pacing needs playtesting | shared health path | yes |
| Housing — city core | solid | abstraction | partial | solid | Local home/rent/utilities; no ownership market | authority commands | yes |
| Law/crime — `VoyagerLaw` + core citations | solid | abstraction | partial | partial | Witness/last-contact/search; some dispatch placement still uses current pawn | per-player cases | fines/records; pursuit session |
| Police — `VoyagerPolice/Law` | solid | partial | partial | partial | Armed officers, hover cruiser and ships; limited tactics | replicated authority | session |
| Drones — `VoyagerPolice` | solid | partial | partial | solid | 1–3 per suspect; swept movement, LOS, real damage, destructible | 0.9.1 peer audit | session |
| Wildlife — `VoyagerWildlife` | partial | weak | weak | solid | 24 nearby animals; primitive articulated models; no far ecology | server population | harvested items only |
| Combat — character/ship/police | solid | partial | partial | solid | Hitscan, reload, damage; little material-specific feedback | server validation | inventory/upgrades |
| Inventory — `VoyagerItems/Survival` | solid | abstraction | UI | solid | Bounded stacks, atomic recipes; host expedition saves | replicated stacks | host |
| Mining — character/crystal path | partial | weak | partial | solid | Generic nearby crystals; not geology-linked; no excavation | server grants | cargo; deposits regenerate |
| Crafting — `VoyagerSurvival` | solid | abstraction | UI | solid | Small functional recipe set; validation tests | server commands | host items |
| Hunting — wildlife + survival | solid | partial | partial | solid | One harvest per session carcass; range/LOS/capacity checks | peer tests | loot; animals respawn |
| Destruction — `VoyagerDestruction` | partial | abstraction | partial | partial | Integrity removes floors; 96 Chaos debris cap; no support graph | server decisions | building deltas |
| Save/load — game mode + city adapter | solid | n/a | n/a | partial | Ordered async encoding; game-thread clone/serialize; legacy archives | host persistence | versioned deltas |
| Multiplayer — actor/RPC paths | solid | n/a | partial | partial | Two-player fixtures passed; four-player bandwidth/stress unmeasured | up to four configured | joining cargo session |
| UI — HUD/phone/backpack | solid | partial | partial | solid | Actual data; large panels, footers and markers compete for attention | owner views | UI mode not saved |
| Audio — game sound calls | partial | weak | weak | solid | Weapon/explosion clips; no ambient or propulsion system | local effects | n/a |
| Local dialogue — `VoyagerLocalAI` | solid | abstraction | n/a | solid | Optional validated rewriting, authored fallback; no sim mutation | local owner flavor | cache session |
| Performance — existing reports/config | partial | n/a | n/a | partial | High-end-only rendered evidence; 8 GB forced pool; missing detailed GPU baseline | bandwidth unmeasured | n/a |

## Top 20 immersion problems

Ordered by likely player impact; these are source/capture assessments, not survey results.

1. Unsafe landing and exits can visibly intersect geometry.
2. Uniform tower grid and repeated facades dominate the city silhouette.
3. Fixed exposure cannot adapt between interiors and outdoors.
4. Static weather has no visible approach, activity change or precipitation.
5. Terrain lacks connected geological formations and drainage logic.
6. Entire planets largely share one biome rather than climate transitions.
7. Wildlife consists of visibly primitive bodies and local respawning.
8. Citizen motion remains scripted rather than blended grounded locomotion.
9. Most embodied residents use similar walking speeds and limited idle variation.
10. Ambient wilderness, city and propulsion audio is missing.
11. Clouds use distinct volume and orbital representations with limited continuity evidence.
12. Street/building streaming is perceptible at transition distances.
13. Nearby populations materialize and disappear with proximity.
14. Repeated furnished layouts lack distinct households and workplaces.
15. Exterior shadows and window reflections remain limited by current rendering choices.
16. Fast flight changes velocity without enough mass or engine feedback.
17. Mining resources appear as generic crystals rather than geological deposits.
18. Combat impacts/tracers lack material-specific weight and sound.
19. Destruction removes floors by aggregate integrity rather than local support failure.
20. HUD panels, labels and footers obscure the place the player is exploring.

## Top 20 technical risks

"Verified" means observable in source/config/logs; "risk" requires further reproduction.

1. Verified: unchecked landing footprint and fixed disembark point (`VoyagerShip`, game mode).
2. Verified: synchronous terrain collision cooking can stall the game thread (`InstallPatch`).
3. Verified: monolithic builds receive DLL-import annotations (`LivingCitySim.Build.cs`), producing LNK4217.
4. Verified: save cloning and UObject serialization remain on game thread; previous worst frames exceed 16.6 ms.
5. Verified: 8192 MB texture pool and high sky settings are forced across hardware (`DefaultEngine.ini`).
6. Risk: four players in separate cities multiply geometry, animation and replication cost; no stress baseline.
7. Verified: no automated representative multi-seed landing/world safety sweep.
8. Risk: tall buildings, hills and flight collision approximations may disagree at footprint edges.
9. Verified: road/grid routing is simpler than generated interior geometry; comprehensive all-seed route proof absent.
10. Risk: actor-iterator avoidance is quadratic across nearby citizens and security actors.
11. Verified: no lossless persistent far-wildlife population; streaming regenerates individuals.
12. Verified: `LivingCityEditor` is an entry-point stub, not a suite of world validators.
13. Verified: `LivingCitySim` owns economic state, while broader Voyager gameplay remains actor-owned; avoid expanding the split.
14. Risk: asynchronous terrain work plus system transitions need sustained cancellation/lifetime stress tests.
15. Verified: numerous obsolete custody gates remain, although the law APIs always return free.
16. Risk: distant world shader precision/coordinate conversions need more non-polar regression locations.
17. Verified: no GPU/render-thread/draw-call/texture-memory breakdown in existing short integration reports.
18. Risk: visibility-based police search can be undermined by dispatch using an unobserved pawn location.
19. Verified: old docs still contain stale alongside-only claims and obsolete platform blockers.
20. Verified: installed experimental plugin availability is not evidence of implemented traffic/crowd systems.

## Performance baseline

Host: RX 7900 XT (20 GB VRAM), about 31 GB RAM, 16 logical CPU threads. This is not a
mid-range qualification machine. Configuration at audit start: DX12, TAA, no Lumen GI,
screen-space reflections supplied by post-process, 2048 shadow maps, fixed exposure,
8192 MB texture pool, atmosphere FastSkyLUT disabled, sample maximum 96, view scale 2.

Historical 0.9 rendered city fixture, 1280x720, 60 FPS cap: 59.86 average FPS, 91.556 ms
worst frame, 0.244 ms peak worker sample. The earlier 0.8 fixture separately measured
0.386–1.915 ms game-thread save cloning, 1.940–4.122 ms serialization and 78.014–94.551 ms
background encoding. These are different runs, not a single combined benchmark.

Current 0.9.1 evidence: packaged police 22/22 (32.01 s), survival 25/25 (46.37 s), city
41/41 (137.07 s); editor multiplayer flight 24/24 (129.22 s); core 206/206 and CI green.
None of those establish sustained 1080p/60 on a mid-range GPU. GPU time, draw calls,
resident actor/component counts and runtime texture allocation must be measured; unknown
values are deliberately not represented as zero. New benchmark measurements will be
appended with build, preset, seed, scene, resolution and sampling interval.

## Proposed implementation phases and acceptance criteria

### A — Stability and measurement (first implementation)

Fix modular/monolithic library annotations. Introduce deterministic landing suitability
rules fed by authoritative geometry queries; reject excessive speed, steep/uneven ground
and blocked footprint. Search bounded safe exit candidates before changing possession.
Keep normal saves, input prediction and continuous flight. Add pure regression cases and
real runtime landing/exit tests over several system seeds, plus reproducible profiling.
Acceptance: both Unreal targets link without the known warnings; pure gate and relevant
packaged/network/save tests pass; unsafe fixtures refuse without moving/losing the player.

### B — One reference planet's visual foundation

Reference system 42, verdant planet 4. Establish wilderness, exterior, interior and orbit
capture locations. Introduce measured scalability, controlled exposure adaptation and
coherent atmosphere/cloud parameters before changing terrain generation. Then implement
pure seeded climate/weather state with smooth presentation and observable consequences.
Acceptance: comparable before/after captures, bounded frame/memory cost, no light clipping
or atmosphere jump, stable seed identity and save/network tests. A weather phase is not
complete merely because a cloud material parameter changes.

### C — Character quality

Grounded locomotion, identity-stable representation tiers, varied schedules and route
validation. Acceptance: full observed commutes, no wall penetration in regression seeds,
no duplicate identity on promotion and measured animation cost.

### D — City realism

Terrain-aware zones, road hierarchy, varied silhouettes and constrained room archetypes.
Acceptance: generated entrances, stairs, workplaces and homes remain reachable across a
seed suite; furnishing never blocks required circulation; geometry budgets hold.

### E — Living simulation

Improve the existing economy/need/job loops with verified reactions rather than parallel
managers. Acceptance: conservation, deterministic replay, meaningful stock/wage changes,
player/NPC parity and complete save round trips.

### F — Vehicles and spacecraft

Tune acceleration, braking, touchdown and feedback around the safety foundation. Civilian
traffic remains a separate pure lane-model slice. Acceptance: continuous flight and safe
landing over representative terrain, independent co-op pilots and budgeted visible traffic.

### G — Wildlife

Persistent seeded population/ecology with embodied presentation and state-driven activity.
Acceptance: demotion preserves individuals, hunting changes real populations, regional
budgets hold and wildlife stays out of interiors.

### H — Combat and security

Information-driven dispatch, readable drone states, cover/positioning and better effects.
Acceptance: hidden suspects cannot be tracked from unobserved positions; range/LOS and
authority tests pass; no return of jail or surrender.

### I — Destruction

Pure support/damage decisions and bounded Chaos presentation. Acceptance: seeded collapse
is deterministic and persistent; debris cannot alter authoritative state or exceed budgets.

### J — Multiplayer qualification

Four independent players, separate cities/planets, combat and travel under measured network
conditions. Acceptance: no duplicate rewards or cross-account mutation; report bandwidth,
latency, relevancy and worst-frame cost instead of only the configured four-player limit.

### K — Polish

Environment-aware audio, restrained UI, readable interaction, accessibility and pacing.
Acceptance: blind playthrough can follow the central explore/work/trade/upgrade loop;
atmosphere audio fades consistently and UI reports only actual state.

The first reference-planet milestone is complete only when its terrain, foliage, lighting,
weather, wildlife, city life, interiors, security, landing, continuous flight, co-op, saves
and measured performance meet the brief together. This audit does not mark that milestone
complete. Each completed slice will record its own validation and remaining limitations.

## First implementation follow-up — 2026-09-13

The first foundation candidate adds quantized landing suitability, authority footprint and
exit queries, modular-only DLL export definitions, graphics tiers, bounded exposure and
instrumented reference scenes. Details and status are in
[VOYAGER_PRODUCTION.md](phases/VOYAGER_PRODUCTION.md). The inventory above remains the
pre-change audit; it is not retroactively rewritten to imply these fixes were present.

A four-scene preview measured approximately 16.67 ms capped wall frames, 3.59–7.49 ms mean
GPU busy time and 3009.7–3114.2 MiB process residency. The instrumentation caught an 87%
internal-resolution default despite a 1080p output. Source now specifies 100%; final native
resolution performance is **unknown**, not extrapolated from that run. Recorded last-frame
draw calls were 1290/646/675/160 across wilderness/city/interior/orbit; these are scene
samples, not a worst-case renderer budget. [Raw data](benchmarks/production-preview-20260913.json).

Windows Smart App Control blocked the newly packaged executable, then the rebuilt editor
DLL. A two-player flight regression reached the destination but did not complete landing;
the settling change cannot yet be re-tested. Final qualification therefore remains open.
The source builds and earlier successful runs must not be presented as a finished
reference planet or a fully tested release.
