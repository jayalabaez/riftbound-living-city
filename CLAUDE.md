# LIVING CITY — Engineering Contract

Read this file at the start of every session, before anything else.
Then read the checklist for the current phase in `docs/phases/`.
Decisions and rationale live in `DECISIONS.md`. Module boundaries live in `docs/ARCHITECTURE.md`.

---

## 0. What this is

A real-time first/third-person 3D city simulation. The player lives as one ordinary citizen
inside a fully simulated economy, alongside tens of thousands of NPCs who each have a home, a
job, money, needs, and legal accountability.

**Prime directive:** a small thing that runs at 60 FPS beats a large thing that doesn't run.
Never scaffold ten systems at once. Every phase must build, run, and be profiled before the
next one starts.

---

## 1. Verified platform

Checked on this machine 2026-09-09. **Re-verify after any engine upgrade.**

| Item | Verified state |
|---|---|
| Engine | **Unreal Engine 5.8.2**, `++UE5+Release-5.8`, CL 56702186, `C:\Program Files\Epic Games\UE_5.8` |
| Other engines installed | none — 5.5 is **not** on this machine |
| Host project | `Riftbound.uproject`, `EngineAssociation` 5.8 |
| Compiler | Visual Studio **Build Tools 2022** at `C:\BuildTools` (VC x86/x64 workload present, no full IDE) |
| Git / LFS | git 2.53.0, git-lfs 3.7.1 |
| Ollama | 0.33.3, `llama3.2:3b` pulled |
| CPU / RAM | 16 logical cores, 31.1 GB |
| GPU | **Radeon RX 7900 XT** — high-end. See R-07: this GPU will lie to you about performance. |

### 1.1 Framework status — VERIFIED, not assumed

Read from the `.uplugin` descriptors under `UE_5.8/Engine/Plugins`. **Five of the seven
frameworks the brief names as load-bearing are Experimental or Beta, and one does not ship
at all.**

| Framework | Verified status in 5.8.2 | Consequence |
|---|---|---|
| **MassEntity** | **Promoted to an engine module** — `Engine/Source/Runtime/MassEntity`, no longer a plugin | Good news. Stable location, always available. |
| MassGameplay | Plugin v0.4, `IsExperimentalVersion: true`, off by default | API churn risk on every engine upgrade |
| MassAI | Plugin v0.4, `IsExperimentalVersion: true`, off by default | same |
| MassCrowd | Plugin v0.4, `IsExperimentalVersion: true`, off by default | same |
| **MassTraffic** | **DOES NOT EXIST IN THE ENGINE.** Not present anywhere in 5.8.2. | Ships only inside Epic's City Sample project. See D-002. |
| ZoneGraph | Plugin v0.5, `IsExperimentalVersion: true`, off by default | Experimental, and oriented toward editor authoring |
| ZoneGraphAnnotations | Plugin v0.5, `IsExperimentalVersion: true`, off by default | same |
| StateTree | Plugin v0.1, not beta-flagged, off by default | usable |
| GameplayStateTree | Plugin v1.0, not beta-flagged, off by default | usable |
| **PCG** | Plugin v1.0, **enabled by default**, production | safe to rely on. `PCGBiome*` and `PCGPrimitives` are still Experimental — avoid those. |
| GeometryCollectionPlugin | v0.1, **`IsBetaVersion: true`**, off by default | visuals only, per R7 |
| ChaosSolverPlugin | v0.1, **`IsBetaVersion: true`**, enabled by default | visuals only, per R7 |
| SmartObjects | v0.1, not beta-flagged, off by default | usable |
| ProceduralMeshComponent | v1.0, production, **already enabled in this project** | Phase 1 default voxel mesh path |
| **RealtimeMeshComponent** | **NOT INSTALLED** — third party, not in the engine | must be fetched; license must be re-verified before adoption |
| MassInsights | v1.0, Beta, enabled by default | useful for profiling Mass |

**Standing rule.** Every Experimental/Beta dependency sits behind an interface we own, with a
named fallback. When an engine upgrade breaks one, we swap the implementation, not the
architecture. Record every such interface in `DECISIONS.md`.

---

## 2. Non-negotiable architecture rules

**R1 — The simulation core is engine-agnostic C++.**
`LivingCitySim` compiles with **zero Unreal types**: no `UObject`, no `AActor`, no `FMath`, no
`TArray`, no `UE_LOG`, no engine tick. Standard C++20 and our own small container library only.
It must build and run as a standalone console executable with no Unreal present at all. This is
what buys determinism, fast tests, and sub-second iteration on economy and law logic.
Enforced by `Scripts/Check-SimPurity.ps1` in CI, not by good intentions.

**R2 — Unreal is a presentation layer.**
Engine → sim is *input only* (player commands). Sim → engine is *state only*. Gameplay logic
inside an `AActor` is in the wrong place.

**R3 — Determinism is the foundation.**
Fixed 20 Hz sim timestep. One seeded RNG stream per subsystem. Strictly ordered iteration —
never iterate an unordered container. Same seed + same input log = same world, bit for bit.
Chaos physics is non-deterministic and therefore **may never influence sim state**. The replay
harness is built in Phase 0 and runs in CI on every commit.

**R4 — The player is an NPC.**
Player and NPCs share every needs, money, job, legal, and health code path. If you write
`if (bIsPlayer)` inside a simulation system, stop and redesign. Enforced by a CI grep.

**R5 — Save = seed + deltas.**
The world generates from a seed; the save stores only divergence. Never serialize untouched
terrain.

**R6 — No game data in binary assets.**
Goods, prices, jobs, laws, fines, materials, vehicle stats → CSV/JSON loaded into plain C++
structs. `.uasset` is for art only. Binary data assets are invisible to review, undiffable in
git, and uneditable outside the editor.

**R7 — Chaos is a costume, not a decision-maker.**
Support propagation and collapse decisions happen in `LivingCitySim` and are authoritative and
deterministic. Chaos and Geometry Collections only *animate* a collapse the sim already decided.

**R8 — Blueprints are thin wrappers.**
UI layout and simple visual glue only. No logic, no state machines, no math. Anything with a
branch belongs in C++.

**R9 — Budget everything.**
16.6 ms frame. 10 ms sim tick at 20 Hz. A system that overruns gets an LOD tier, not more
milliseconds.

**R10 — Worker threads compute, they do not decide.**
Worker threads run only *pure* functions. Results are consumed in deterministic order (job index
order, never completion order). Meshing, LLM calls, and IO never write sim state.

**R11 — Compile-time discipline.**
Unreal C++ iteration is slow; protect it. Lean headers, forward declarations, and keep the sim
core buildable without Unreal so most logic work happens in a standalone compiler loop rather than
a multi-minute UBT loop. Report build times when they regress.

---

## 3. Invariants

Test these. Do not test getters.

| # | Invariant | How it is checked |
|---|---|---|
| I1 | Same seed + same input log ⇒ identical world hash at every checkpoint | replay harness, CI every commit |
| I2 | No Unreal token appears in `LivingCitySim` sources | `Check-SimPurity.ps1` in CI |
| I3 | Total money is conserved exactly; money is created and destroyed only by the central bank | 100k-tick conservation test; integer minor units only |
| I4 | Every citizen has a valid home address and an explicit employment status | generator assertion; unassigned is a loud failure, never papered over |
| I5 | Sim tick allocates zero bytes in steady state | allocator counter assertion in headless test |
| I6 | Tier promotion is lossless for identity, address, job, balance, record | `demote(promote(x)) == x` property test |
| I7 | `hash(world) == hash(load(save(world)))` | save round-trip test |
| I8 | No `if (bIsPlayer)` in any sim system | CI grep |
| I9 | Sim thread performs no IO, no HTTP, no engine calls | CI grep plus a debug-build runtime guard |
| I10 | Every home→workplace pair a citizen uses is routable | reachability test after generation |
| I11 | No agent starves while it has money and an open shop is reachable | long-run invariant test |
| I12 | Chaos state never flows into sim state | CI grep on sim includes |
| I13 | LLM output is discarded unless it validates against schema *and* against real sim numbers | Director validator unit tests |

---

## 4. Performance budget

### Sim tick — 10 ms design budget at 20 Hz

20 Hz gives 50 ms of wall clock per tick, so a 10 ms budget carries a **5× safety margin**.
That margin is why 50,000 citizens is achievable at all. Do not spend it casually.

| System | Budget |
|---|---|
| Agents (needs, utility scoring, planning) | 3.5 ms |
| Transport / traffic | 2.0 ms |
| Economy | 1.5 ms |
| Law / observation | 0.8 ms |
| World streaming coordination + collapse queue | 1.2 ms |
| Events, journaling, bookkeeping | 0.5 ms |
| Headroom | 0.5 ms |

### Frame — 16.6 ms CPU at 1080p/60

| Item | Budget |
|---|---|
| Engine + physics (player + ~50 nearby bodies) | 2.5 ms |
| View sync: pooled agent nodes, interpolation | 2.5 ms |
| Voxel chunk mesh apply (hard cap, amortised) | 2.0 ms |
| UI | 1.0 ms |
| Culling + draw submission | 3.0 ms |
| Headroom / spikes | 5.6 ms |

### Targets

| Metric | Target |
|---|---|
| Citizens simulated | 50,000+ statistical, 100,000 stretch |
| Mass-rendered crowd agents | 5,000–15,000 visible |
| Fully embodied agents | 400–800 near the player |
| Voxel chunk stream/remesh | no frame spike > 4 ms, meshing on worker threads |
| Frame rate | 60 FPS at 1080p on a **mid-range** GPU |
| Headless throughput | ≥ 20,000 ticks/s at full Tier-2 load (= 1000× real time) — see R-06 |
| Save/load of a mature city | < 5 s |

---

## 5. Known risks

These are the places where this design, as written, will not survive contact with the machine.
Each has a proposed resolution. Options and costs are in `DECISIONS.md`.

- **R-01 — MassTraffic does not exist in 5.8.** It ships in City Sample, not the engine.
  Anything that assumed it must be re-planned. → D-002.
- **R-02 — Mass and determinism are in direct conflict.** Mass processors are multithreaded and
  non-deterministic; R1 and R3 require a pure, ordered sim. Both cannot be the agent system.
  → Mass becomes the *crowd rendering* layer; the sim owns agents. → D-001.
- **R-03 — Runtime voxel meshes cannot use Nanite,** and they also have **no Mesh Distance
  Fields**, so Lumen's *software* tracing will not see the terrain at all — wrong GI and missing
  occlusion, not merely lower quality. → D-003.
- **R-04 — Skeletal animation for 400–800 embodied agents will not hold 60 FPS.** Needs three
  visual tiers: real skeletons for the nearest ~48, instanced/VAT beyond. Decide in Phase 2,
  not Phase 8.
- **R-05 — Per-voxel distance-to-ground-support is not affordable city-wide.** A district is on
  the order of 10^8 voxels. Buildings need a coarse structural graph (footings → columns → beams
  → slabs), with the per-voxel flood fill running only inside a bounded, budgeted damage region.
- **R-06 — "≥1000× real time" and "a simulated year in minutes" contradict each other.**
  A year at 20 Hz is 630,720,000 ticks; at 1000× real time that is **8.8 hours**, not minutes.
  → The economy must not run at 20 Hz. Tick hierarchy: 20 Hz embodied, 1 Hz Tier-1 needs,
  1 tick per sim-minute for markets and demography. A year is then ~525,600 coarse ticks ≈ 30 s.
- **R-07 — The dev GPU is a 7900 XT and will hide GPU cost.** Profile against a mid-range preset
  (render scale plus frame cap) or the first RTX 3060 this runs on will be a shock.
- **R-08 — Tier 2 as pure distributions breaks the losslessness promise in I6.** A promoted
  citizen's bank balance would have to be invented. → Every citizen keeps a persistent identity
  and ledger row (50k × ~200 B ≈ 10 MB, trivial). Tiers change *tick frequency*, not existence.

---

## 6. Agent LOD

| Tier | Scope | Representation |
|---|---|---|
| **0 — Embodied** | near player, 400–800 | navmesh pathing, physics capsule, full animation, per-tick needs, can be witnessed committing crimes |
| **1 — Crowd** | same district, 5k–15k visible | instanced rendering, no physics, moves along the lane graph, needs at 1 Hz, transactions still hit the real economy |
| **2 — Statistical** | rest of city | cohorts by (district, job class, household type) consume, earn, pay taxes, are hired and fired, and offend as distributions — **but each individual keeps a persistent identity and ledger row** (R-08) |

Promotion must be lossless: a Tier 2 citizen promoted to Tier 0 keeps a consistent name, home
address, job, bank balance, and criminal record. Enforced by I6.

---

## 7. Phase plan

Build in this order. Do not reorder.

| Phase | Content | Done when |
|---|---|---|
| **0 — Skeleton** | `LivingCitySim` module, fixed-timestep threaded loop, seeded RNG, event bus, standalone headless target, replay/determinism harness, CI, Unreal project shell, profiler overlay, `CLAUDE.md`, Git LFS | the headless binary runs 100k empty ticks deterministically, CI is green, and the Unreal client renders a placeholder reading from sim state |
| **1 — World** | voxel chunk store, streaming, greedy meshing, blocks + clay, terrain gen through caves and minerals, free-fly camera, tool-gated dig and build | fly a generated world, dig a cave, save/load as seed + deltas |
| **2 — Citizens** | agent SoA, three LOD tiers, needs, utility scoring + StateTree, navigation, homes, jobs, routines, day/night | 10,000 citizens wake, commute, work, eat, and sleep — and one can be followed all day |
| **3 — Economy** | firms, goods, prices, wages, labor and housing markets, banking, taxes, utilities, trash and recycling | money conservation holds over a simulated year and prices respond sensibly to an induced shortage |
| **4 — Transport** | lane graph, signals, cars with real costs, buses, trams, trains, congestion feeding the economy | — |
| **5 — Law** | observation, violations, citations, fines, courts, jail, funded institutions | — |
| **6 — Destruction & disasters** | support propagation, Chaos-visualised collapse, fire, earthquake, meteorite, flood, economic shocks | — |
| **7 — City Director** | Ollama integration, schema-validated JSON, news, dialogue, policy | — |
| **8 — The player** | character controller, inventory, needs UI, phone shell, job application, apartment rental, vehicle purchase, crime, arrest and jail | — |
| **9 — Polish** | audio, weather, seasons, balance, performance, tutorial | — |

---

## 8. How we work

- Start every session by reading this file and the current phase checklist.
- Small milestones. After each: **build, run, profile, update tests, commit**, then give a
  two-line status plus current frame and tick timings.
- One commit per completed milestone, tagged `phase-N-mM`.
- **Say what cannot be done from here.** Claude cannot click through the Unreal editor. When a
  step genuinely needs editor work — asset setup, StateTree wiring, project settings — stop and
  give precise click-by-click instructions rather than guessing or faking it. Where the Python
  scripting plugin can automate it, write the script instead.
- When a decision has real trade-offs, present two or three options with costs and a
  recommendation, then **wait**. Do not quietly pick.
- Say when something in the brief is a bad idea. A hard truth in week one beats a beautiful
  architecture that runs at 9 FPS in month six.
- Never end a session with the repo in a non-building state.

---

## 9. Coexistence with RIFTBOUND

**Current work: Voyager Production 0.9.2, 2026-09-13.** The owner's master brief prioritizes
auditing, stability and a measured reference planet before broader feature growth. Its
autonomous engineering instruction supersedes the earlier routine-choice approval rule.
Start with [VOYAGER_WORLD_AUDIT.md](docs/VOYAGER_WORLD_AUDIT.md) and the
[production checklist](docs/phases/VOYAGER_PRODUCTION.md). The owner explicitly requested that the
attached Living City brief be added **into Riftbound**. This supersedes the previous
alongside-only restriction and the prohibition on editing Voyager. The original standalone
Living City mode remains available; it does not run behind Voyager.

- `Riftbound` may depend on the pure `LivingCitySim` module. Neither `Riftbound` nor the core
  may depend on `LivingCityGame` or `LivingCityEditor`. The two presentation modes remain
  independent. No reverse dependency from the core to any Unreal gameplay module is allowed.
- `Sim/include/livingcity/sim/PlanetaryCity.h` and its implementation own the planetary
  resident, needs, economy and citation rules. `AVoyagerCityLife` supplies ordered commands
  to a worker and exposes read-only state for NPC routes, the phone and multiplayer clients.
- The current integration uses 15 cities with 3,600 persistent residents each: **54,000
  simulated records**, with at most 36 embodied citizens per nearby city and 72 globally.
  These are separate quantities. Do not report 54,000 rendered characters or claim that
  the larger Mass crowd, traffic, vehicle, court or disaster phases are complete.
- Voyager's natural art, spherical terrain, ship flight and existing accessible buildings
  remain its presentation. The attached flat-shaded capsule art direction does not replace
  the owner's newer realism request.
- The earlier Frontiers request adds building destruction and native Chaos debris, armed ground police
  and (since removed) saved custody, wildlife hunting and field supplies, citizen gait/eyes, improved
  atmosphere presentation and seeded black-hole encounters to Voyager. This is the active
  foundation; the separate Living City transport/disaster phase order does not block it.
- The latest request removes jail, arrests and surrender, adds armed drones to wanted response,
  and replaces the police car with an original hover sedan. Legacy sentences are ignored and
  cleared on save. This overrides every earlier Voyager custody requirement.
- Existing authoritative Voyager actors own building integrity, police pursuit and animal
  combat. Chaos animates temporary debris; it never decides saved building damage or writes
  to `LivingCitySim`. This milestone does not implement the pure structural-support solver,
  Geometry Collection fracture, civilian traffic, voxel terrain damage or economic disasters.
- Anomaly and distant orbital cloud materials are original procedural implementations.
  Volume weather uses a project material instance of the installed native Unreal cloud
  material; credit that engine parent separately in `Art/Cosmos/PROVENANCE.md`.
  No paid Fab plugin or asset was acquired for this pass. Black-hole disk/halo
  presentation is a visual approximation with a gameplay hazard, not general-relativistic
  ray tracing or an astrophysical gravity simulation.
- Current milestone and verification checklist: [VOYAGER_PRODUCTION.md](docs/phases/VOYAGER_PRODUCTION.md).
  Version 0.9.1 evidence is retained in [VOYAGER_SECURITY.md](docs/phases/VOYAGER_SECURITY.md).
  Version 0.9 evidence is retained in [VOYAGER_FRONTIERS.md](docs/phases/VOYAGER_FRONTIERS.md).
  Version 0.8 evidence is retained in [VOYAGER_CITY_LIFE.md](docs/phases/VOYAGER_CITY_LIFE.md).
  Earlier `PHASE_*.md` files describe the separate Living City mode and historical milestones;
  they are not evidence that their entire roadmap is implemented in Voyager.

All core purity, deterministic stepping, money conservation, data transparency and measured
performance rules above still apply. Tunable planetary data lives as reviewable JSON under
`Content/CityData`, staged as text; binary Unreal assets remain art.
