# ARCHITECTURE — repository layout and module boundaries

Companion to `CLAUDE.md`. This document defines **where code lives** and **which direction each
dependency points**. A dependency that points the wrong way is a bug, not a style preference.

---

## 1. Repository layout

LIVING CITY and Voyager share `Riftbound.uproject`. The original tree below records the
initial module split. Since the owner's 2026-09-13 integration request, Voyager also uses
the pure simulation module through its own city adapter; its gameplay files may be edited.
The separate Living City presentation remains independent. See D-030.

```
GameProject/                          repo root, Unreal project root
├─ Riftbound.uproject                 add LivingCity modules + plugin enables here
├─ CLAUDE.md                          NEW  engineering contract, read every session
├─ DECISIONS.md                       NEW  decision log
├─ README.md                          RIFTBOUND's. Do not repurpose.
├─ SURVIVAL.md  VOYAGER.md            RIFTBOUND's.
├─ .gitattributes                     NEW  LFS routing (§4)
├─ .gitignore                         extended for Sim build output
│
├─ docs/                              NEW
│  ├─ ARCHITECTURE.md                 this file
│  └─ phases/PHASE_0.md ... PHASE_9.md
│
├─ Sim/                               NEW  the pure C++ core — no Unreal anywhere below here
│  ├─ build/<Config>/                standalone MSVC output: headless exe + test exe
│  ├─ include/livingcity/             public headers
│  ├─ src/                            implementation, compiled by standalone MSVC and UBT
│  │  ├─ core/        rng, fixed-point, time, ids, containers, event bus, hashing, logging
│  │  ├─ world/       chunk store, voxel, generation, hydrology, minerals
│  │  ├─ agents/      SoA store, needs, utility scoring, planning, LOD tiers
│  │  ├─ economy/     money, goods, firms, markets, government, utilities, waste
│  │  ├─ transport/   lane graph, routing, traffic, transit
│  │  ├─ law/         observation, violations, citations, courts, jail
│  │  ├─ destruct/    support propagation, collapse, fire, disasters
│  │  ├─ director/    LLM proposal validation only — no HTTP, no IO
│  │  └─ sim/         the tick orchestrator that wires the above
│  ├─ apps/headless/  console entry point: run, replay, verify, benchmark
│  └─ tests/          invariant tests (I1–I13)
│
├─ Source/
│  ├─ Riftbound/                      Voyager runtime, including VoyagerCityLife adapter/UI
│  ├─ LivingCitySim/                  NEW  UBT wrapper that compiles Sim/src (D-005)
│  ├─ LivingCityGame/                 NEW  Unreal runtime: rendering, input, view sync
│  ├─ LivingCityEditor/               NEW  editor-only tools, commandlets, validators
│  ├─ Riftbound.Target.cs             EXISTING, + LivingCity modules
│  └─ RiftboundEditor.Target.cs       EXISTING, + LivingCity modules
│
├─ Data/                              NEW  ALL tunable game data. CSV/JSON only (R6).
│  ├─ goods.csv  materials.csv  jobs.csv  laws.csv  fines.csv  vehicles.csv
│
├─ Content/                           binary files are art only (R6)
│  ├─ CityData/economy.json           reviewable planetary tunables, staged as text
│  ├─ (existing Riftbound content)    natural terrain, characters, ships and interiors
│  └─ LivingCity/                     NEW  meshes, materials, UI widgets
│
├─ Scripts/                           EXISTING, extended
│  ├─ Build.ps1  Package.ps1  Play.ps1  Test-*.ps1   RIFTBOUND's — do not repurpose
│  ├─ Build-Sim.ps1                   direct cl.exe build of headless and invariant binaries
│  └─ Check-SimPurity.ps1             NEW  enforces I2, I8, I9, I12
│
└─ .github/workflows/ci.yml           NEW  builds Sim, runs invariant tests, runs replay
```

**Why `Sim/` sits outside `Source/`.** `Source/` is UBT's territory; anything there is an Unreal
module by convention. Putting the pure core outside makes R1 visible in the directory tree
itself, and lets the standalone MSVC script build it without UBT interference. There is no
`Sim/CMakeLists.txt`; D-010 supersedes the original CMake plan. `Source/LivingCitySim/` is a thin
`.Build.cs` that points UBT at `Sim/src`.

---

## 2. Module dependency graph

Arrows mean **"depends on"**. There are no arrows in the other direction, ever.

```
                    ┌─────────────────────────────┐
                    │       LivingCitySim         │
                    │   pure C++20, zero Unreal   │
                    │   deterministic, testable   │
                    └─────────────────────────────┘
                       ▲          ▲            ▲
                       │          │            │
        ┌──────────────┘          │            └──────────────┐
        │                         │                           │
┌───────────────┐    ┌────────────────────────┐   ┌───────────────────────┐
│ Sim/apps/     │    │    LivingCityGame      │   │      Sim/tests        │
│  headless     │    │  Unreal runtime module │   │  invariant tests      │
│ console exe   │    │  render / input / sync │   │  console exe          │
└───────────────┘    └────────────────────────┘   └───────────────────────┘
                                 ▲
                                 │
                     ┌───────────────────────┐
                     │   LivingCityEditor    │
                     │  editor-only tools    │
                     └───────────────────────┘

┌───────────────┐
│   Riftbound   │──→ LivingCitySim only; its own VoyagerCityLife presentation boundary.
└───────────────┘
```

| Module | Depends on | Explicitly must NOT depend on |
|---|---|---|
| `LivingCitySim` | the C++20 standard library, and nothing else | Unreal, Chaos, Mass, ZoneGraph, StateTree, HTTP, the filesystem outside an injected IO interface |
| `Sim/apps/headless` | `LivingCitySim` | Unreal |
| `Sim/tests` | `LivingCitySim` | Unreal |
| `LivingCityGame` | `LivingCitySim`, Engine, MassEntity, Mass* (behind our interfaces), StateTree, PCG, Chaos, mesh component | `Riftbound`, `UnrealEd` |
| `LivingCityEditor` | `LivingCityGame`, `LivingCitySim`, `UnrealEd` | `Riftbound` |
| `Riftbound` | its existing dependencies, `LivingCitySim` | `LivingCityGame`, `LivingCityEditor` |

### The two boundaries that matter most

**Boundary 1 — `LivingCityGame` → `LivingCitySim`.** One direction only.

- **Engine → sim: input only.** Player intents are pushed onto a command queue
  (`MoveTo`, `Interact`, `Purchase`, `ApplyForJob`, `Dig`). Commands are timestamped by sim tick
  and enter the replay log. They are the *only* way the engine influences the sim.
- **Sim → engine: state only.** The engine reads an immutable snapshot from a triple-buffered
  ring and interpolates between the last two for rendering. It never mutates sim state, and it
  never calls into a sim system mid-tick.
- Anything that would need a third channel is a design error. Say so rather than adding one.

**Boundary 2 — sim → worker threads.** Worker threads receive pure inputs and return pure
outputs (R10). Results are consumed in **job index order**, never completion order. Meshing, LLM
calls, and file IO live entirely on this side and can never write sim state.

### Voyager's planetary adapter

`PlanetaryCity` under `Sim/include/livingcity/sim` and `Sim/src/sim` is a second pure core entry point tailored to Voyager's already
generated spherical settlements. It receives 60 real building indices per city, so homes,
jobs and shops refer to the same doors the player can enter. Fifteen city instances keep
54,000 resident records. The game advances them at a fixed 20 Hz on one dedicated worker.
The original `LivingCityGame` simulation subsystem is not started in Voyager mode.

`Source/Riftbound/VoyagerCityLife.cpp` owns the worker boundary, ordered command queue,
save archives and validated player actions. Its worker may mutate pure city state; it may
not access actors. Read-only views copy state under the host lock. The current adapter does
not use the original mode's triple-buffered snapshot ring; lock time and save duration are
part of the integration's measured budget.

`VoyagerCitizen` represents only the nearby embodied population. Utility goals originate
in `PlanetaryCity`; actors execute existing street/doorway routes and report observed
presence. Embodied residents cannot complete abstract travel while their actor is still
outside. Demotion changes representation, not identity, money, inventory, needs or death.
Player input uses the same core command, account, need and health paths as residents.
The phone receives authoritative views by RPC and never authors account balances.

Planetary tunables are plain `Content/CityData/economy.json`, staged through
`DirectoriesToAlwaysStageAsUFS`; `.uasset` files hold no economy configuration. City save
archives contain deterministic seed/configuration and state deltas, with no untouched
planet terrain. Resident relocation transfers possessions and legal/need state while
assigning a home and job from the destination's actual buildings; it is not persistent
ownership of properties across planets.

Autosaves coalesce requests over a short window and allow one encode/write sequence at a
time. `TryPrepareSave` checks the command batch, drains its results and clones the city
cores under the same lock; trade refunds settle before expedition cargo is captured.
The returned encoder owns those copies and historical archives, with no actor references.
Background jobs encode city deltas and write immutable save bytes. Unreal's save-object
serialization remains on the game thread. Explicit saves and teardown join pending jobs;
serial numbers preserve write order so an older snapshot cannot overwrite a newer one.
Capture/clone, encode and game-thread serialization timings are logged separately.

The latest archive wrapper encodes compressed city deltas into the additive `PackedState`
string field, avoiding reflected serialization of each byte in a large array. Packing uses
bounded zlib data with a size header and base64 transport in the background encoder.
The production decoder retains support for legacy `State` byte arrays and rejects invalid
packed payloads. Final editor and standalone integration audits verify production decoding,
corruption rejection, resident persistence and teardown cargo preservation. Fifteen measured
standalone saves averaged 1.422 ms for game-thread cloning, 2.982 ms for Unreal serialization
and 85.142 ms for background encoding. Full ranges and coverage are in the integration checklist.

---

## 3. Layering inside `LivingCitySim`

Dependencies flow **downward only**. No cycles. Enforced by an include-graph test in CI.

```
  sim/         ← the tick orchestrator; may see everything
    │
    ├── law/          depends on: agents, economy, transport, world, core
    ├── destruct/     depends on: world, economy, core
    ├── transport/    depends on: agents, world, core
    ├── economy/      depends on: agents, world, core
    ├── agents/       depends on: world, core
    ├── world/        depends on: core
    ├── director/     depends on: core + read-only views only
    └── core/         depends on: nothing
```

**Cycle-breaking rule.** When a lower layer needs to affect a higher one, it publishes an event;
it does not take a dependency. Law fines an offender by emitting a `FineIssued` event that
`economy` consumes — `economy` never includes a `law/` header. This is the mechanism that keeps
the graph acyclic as systems multiply, and it is also what makes disasters publish
`EconomicShockEvent` rather than reaching into firm balance sheets.

**`director/` is deliberately starved.** It holds only the *validator* for LLM proposals — schema
checks plus consistency checks against real sim numbers (I13). The HTTP call to Ollama lives in
`LivingCityGame` on a worker thread, because R1 forbids IO in the sim and R3 forbids anything
non-deterministic from touching sim state. The LLM proposes; the validator in the sim decides.

---

## 4. Git LFS configuration

The authoritative file is `.gitattributes` at the repo root. Two jobs: route art and binaries to
LFS, and **explicitly keep code and game data as text** so R6's "data must be diffable" promise
holds.

Gitattributes takes **one pattern per line** — a line may carry several *attributes*, but never
several patterns. Excerpt:

```gitattributes
* text=auto

# Unreal binary assets -> LFS
*.uasset filter=lfs diff=lfs merge=lfs -text
*.umap   filter=lfs diff=lfs merge=lfs -text

# Art, media, binaries -> LFS (one line each: fbx, obj, blend, png, wav, dll, exe, ...)
*.fbx filter=lfs diff=lfs merge=lfs -text
*.png filter=lfs diff=lfs merge=lfs -text
*.exe filter=lfs diff=lfs merge=lfs -text

# Code and game data stay TEXT and diffable (R6)
*.cpp  text
*.h    text
*.csv  text
*.json text
```

`*.uasset` is **not** marked `lockable`. Locking matters for a team fighting over undiffable
binaries; this is a solo project and locking would only add friction.

---

## 5. Enforcement

Voyager 0.9 retains the same module boundary. The pure planetary core receives ordered
field-care commands and continues to own needs, health, money and citations. Existing
Riftbound gameplay actors own bounded building integrity, animal combat, wanted response
and, in 0.9.1, attack drones. Jail has been removed; legacy save sentence fields are read
only for compatibility and cleared on the next save. Chaos debris is temporary presentation and never writes into the pure core
or determines saved structural damage. Original procedural cosmos actors are presentation
plus a server-owned hazard. This extension is described in D-031 and
`docs/phases/VOYAGER_FRONTIERS.md`. The production follow-up in D-034 moves bounded regional
support and surviving-floor decisions into `Sim/include/livingcity/destruct/BuildingStructure.h`.
The host quantizes impacts and archives returned state; the pure rule uses fixed arrays
and integer arithmetic. Chaos presents material-specific fragments, while support deltas
and a collapse ceiling survive in schema 5. This remains a coarse floor chain, not a full
beam-load solver. Held item geometry and furniture frames are presentation only.

Boundaries that are only written down get violated. These run in CI on every commit.

| Check | Enforces | Implementation |
|---|---|---|
| Sim purity grep | I2 | `Check-SimPurity.ps1` — fails if `Sim/src` or `Sim/include` contains `UObject`, `AActor`, `FMath`, `TArray`, `UE_LOG`, `#include "Engine`, `#include "CoreMinimal`, `Chaos`, `Mass` |
| Player-path grep | I8 | fails on `bIsPlayer` or `isPlayer` anywhere under `Sim/` |
| IO grep | I9 | fails on `fopen`, `ifstream`, `ofstream`, `curl`, `socket` outside the injected IO interface |
| Include-graph test | §3 layering | parses `#include` lines under `Sim/src`, asserts the layer DAG and reports any cycle |
| Module-reference test | §2 table | allows `Riftbound` → `LivingCitySim`; rejects dependencies between `Riftbound` and the Living City game/editor modules, and any reverse core dependency |
| Determinism replay | I1 | headless run, two seeds, checkpoint hashes compared |

`Check-SimPurity.ps1` greps `.cpp`/`.h` only, never `.Build.cs` — see D-005 for why
`LivingCitySim.Build.cs` is allowed to list `Core`.
