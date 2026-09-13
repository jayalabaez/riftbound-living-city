# ARCHITECTURE — repository layout and module boundaries

Companion to `CLAUDE.md`. This document defines **where code lives** and **which direction each
dependency points**. A dependency that points the wrong way is a bug, not a style preference.

---

## 1. Repository layout

LIVING CITY is built inside the existing `Riftbound.uproject`. New paths are marked **NEW**.
Everything unmarked already exists and belongs to RIFTBOUND — do not touch it.

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
│  ├─ CMakeLists.txt                  standalone build: headless exe + test exe
│  ├─ include/livingcity/             public headers
│  ├─ src/                            implementation, compiled by BOTH CMake and UBT (D-005)
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
│  ├─ Riftbound/                      EXISTING — legacy, untouched
│  ├─ LivingCitySim/                  NEW  UBT wrapper that compiles Sim/src (D-005)
│  ├─ LivingCityGame/                 NEW  Unreal runtime: rendering, input, view sync
│  ├─ LivingCityEditor/               NEW  editor-only tools, commandlets, validators
│  ├─ Riftbound.Target.cs             EXISTING, + LivingCity modules
│  └─ RiftboundEditor.Target.cs       EXISTING, + LivingCity modules
│
├─ Data/                              NEW  ALL tunable game data. CSV/JSON only (R6).
│  ├─ goods.csv  materials.csv  jobs.csv  laws.csv  fines.csv  vehicles.csv
│
├─ Content/                           art only (R6)
│  ├─ (existing Riftbound content)    do not touch
│  └─ LivingCity/                     NEW  meshes, materials, UI widgets
│
├─ Scripts/                           EXISTING, extended
│  ├─ Build.ps1  Package.ps1  Play.ps1  Test-*.ps1   RIFTBOUND's — do not repurpose
│  ├─ Build-Sim.ps1                   NEW  CMake configure + build + test
│  └─ Check-SimPurity.ps1             NEW  enforces I2, I8, I9, I12
│
└─ .github/workflows/ci.yml           NEW  builds Sim, runs invariant tests, runs replay
```

**Why `Sim/` sits outside `Source/`.** `Source/` is UBT's territory; anything there is an Unreal
module by convention. Putting the pure core outside makes R1 visible in the directory tree
itself, and lets CMake own it without UBT interference. `Source/LivingCitySim/` is a thin
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
│   Riftbound   │   EXISTING, legacy. No arrow to or from any LivingCity module.
└───────────────┘
```

| Module | Depends on | Explicitly must NOT depend on |
|---|---|---|
| `LivingCitySim` | the C++20 standard library, and nothing else | Unreal, Chaos, Mass, ZoneGraph, StateTree, HTTP, the filesystem outside an injected IO interface |
| `Sim/apps/headless` | `LivingCitySim` | Unreal |
| `Sim/tests` | `LivingCitySim` | Unreal |
| `LivingCityGame` | `LivingCitySim`, Engine, MassEntity, Mass* (behind our interfaces), StateTree, PCG, Chaos, mesh component | `Riftbound`, `UnrealEd` |
| `LivingCityEditor` | `LivingCityGame`, `LivingCitySim`, `UnrealEd` | `Riftbound` |
| `Riftbound` | its existing dependencies | any `LivingCity*` module |

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

Boundaries that are only written down get violated. These run in CI on every commit.

| Check | Enforces | Implementation |
|---|---|---|
| Sim purity grep | I2 | `Check-SimPurity.ps1` — fails if `Sim/src` or `Sim/include` contains `UObject`, `AActor`, `FMath`, `TArray`, `UE_LOG`, `#include "Engine`, `#include "CoreMinimal`, `Chaos`, `Mass` |
| Player-path grep | I8 | fails on `bIsPlayer` or `isPlayer` anywhere under `Sim/` |
| IO grep | I9 | fails on `fopen`, `ifstream`, `ofstream`, `curl`, `socket` outside the injected IO interface |
| Include-graph test | §3 layering | parses `#include` lines under `Sim/src`, asserts the layer DAG and reports any cycle |
| Module-reference test | §2 table | parses every `.Build.cs`, asserts `Riftbound` and `LivingCity*` never reference each other |
| Determinism replay | I1 | headless run, two seeds, checkpoint hashes compared |

`Check-SimPurity.ps1` greps `.cpp`/`.h` only, never `.Build.cs` — see D-005 for why
`LivingCitySim.Build.cs` is allowed to list `Core`.
