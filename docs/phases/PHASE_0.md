# PHASE 0 — SKELETON

**Goal.** A deterministic simulation heartbeat with no game in it, provable by test, plus an
Unreal client that renders a placeholder driven by real sim state.

**Done when:**
1. The headless binary runs **100,000 empty ticks deterministically** — two runs, same seed,
   identical checkpoint hashes. — **DONE**
2. **CI is green** — purity, layering, determinism, and replay checks all pass. — **DONE**
3. The **Unreal client renders a placeholder reading from sim state** (not from a hardcoded
   value, not from an `AActor` that computes it). — **DONE, verified running**

**Not in this phase.** No voxels, no agents, no economy, no traffic. Resist all of it.
An empty tick means the loop, the clock, the RNG, the event bus, and the snapshot buffer — and
nothing else.

---

## 1. Measured baseline

Recorded 2026-09-09 on the dev machine (Ryzen-class 16 threads, MSVC 19.44, Release `/O2`).
These are the numbers every later phase is compared against.

| Metric | Target | **Measured** |
|---|---|---|
| Headless throughput | ≥ 20,000 ticks/s (1000× real time) | **2,388,227 ticks/s — 119,411× real time** |
| Per-tick cost | — | **419 ns** |
| Steady-state allocations | 0 (I5) | **0** |
| 100k-tick run, wall clock | — | **0.042 s**, world hash `84411fa28151625c` |
| Invariant tests | — | **120, all passing** |
| Full CI gate | — | **20.9 s** |
| Sim core build (clean, `/W4 /WX`) | — | **~20 s** (headless 5 s, tests 15 s) |

The throughput headroom is enormous *because Phase 0 has no game in it*. Expect it to fall by
orders of magnitude as agents, economy and traffic land. That is fine and expected — the point
of recording it now is to know exactly which phase spends it.

---

## 2. Prerequisites — resolved

| # | Blocker | Outcome |
|---|---|---|
| P1 | OPEN decisions **D-001** (role of Mass) and **D-002** (traffic) | still open — neither blocks Phase 0, both must settle before Phase 2 |
| P2 | CMake availability | **resolved without it.** CMake and Ninja are absent; `Scripts/Build-Sim.ps1` drives `cl.exe` directly through `vcvars64.bat`. No install needed. See D-010. |
| P3 | git remote for GitHub Actions | **still none.** `Scripts/Run-CI.ps1` is the real gate and is installed as a pre-commit hook. `.github/workflows/ci.yml` is written and dormant. |

---

## 3. Milestones

### M0.1 — Skeleton and guardrails — **DONE**
- [x] `Scripts/Build-Sim.ps1` producing `livingcity_headless` and `livingcity_tests` (C++20, `/W4 /WX`)
- [x] `Sim/src/{core,world,agents,economy,transport,law,destruct,director,sim}/` laid out
- [x] `Source/LivingCitySim/LivingCitySim.Build.cs` compiling `Sim/src` (`bUseUnity=false`, `PCHUsage=NoPCHs`)
- [x] `Source/LivingCityGame/` and `Source/LivingCityEditor/` modules
- [x] ~~`Source/LivingCityHeadless.Target.cs`~~ — dropped, see D-010
- [x] `Riftbound.uproject`: three modules registered; StateTree, GameplayStateTree, PCG enabled
- [x] `Scripts/Build-Sim.ps1`, `Scripts/Check-SimPurity.ps1`
- [ ] **Verify `Riftbound` still builds and `Play Voyager.cmd` still launches** — blocked, see §5

### M0.2 — Deterministic primitives — **DONE**
- [x] `Xoshiro256**` + `SplitMix64`, named per-subsystem streams, verified against published reference vectors
- [x] Fixed-point `Q32.32` with a portable 32-bit-limb 128-bit intermediate (no `__int128`, no MSVC intrinsics)
- [x] `Money` — `int64` minor units, transfer-only, `Money * Money` deliberately does not compile
- [x] `Ledger` — mint/burn only via the central bank; `OpenAccount` with a balance is a mint plus a transfer
- [x] Monotonic `IdAllocator`, never reuses a value
- [x] `Hasher` — FNV-1a-64 over canonical bytes; never fed a pointer or a float

### M0.3 — Fixed-timestep sim loop — **DONE**
- [x] `SimClock` at 20 Hz with the D-008 tick hierarchy, boundaries as pure functions of tick index
- [x] `SimThreadHost` — sim owns its thread; wall clock schedules ticks but never enters one
- [x] Inbound `CommandQueue` (SPSC, lock-free, engine → sim, input only)
- [x] Triple-buffered `SnapshotRing` (sim → engine, state only, tear-free)
- [x] Pause and single-step
- [x] **Zero steady-state allocation, asserted by counter — measured 0**

### M0.4 — Event bus — **DONE**
- [x] Fixed-capacity ring, zero allocation on publish
- [x] Deterministic drain order: descending priority, ties by registration index
- [x] Overflow is `LC_FATAL`, never a silent drop
- [x] Events published during a drain land in the *next* drain, so order cannot depend on handler behaviour
- [x] Compile-time guard: event types must be trivially copyable **with no padding bytes** — padding would poison the world hash

### M0.5 — Headless console target — **DONE**
- [x] `livingcity_headless run|verify|bench|selfcheck`
- [x] `--seed --ticks --hash-every --record --csv --print-final-hash --quiet`
- [x] CSV telemetry per system per tick
- [x] Exit codes usable as CI gates (0 ok, 1 failure, 2 usage)

### M0.6 — Replay and determinism harness — **DONE, including the negative control**
- [x] Binary input log, magic + version header, explicit little-endian field-by-field writes (never `memcpy` of a padded struct)
- [x] Checkpoint hashes every N ticks
- [x] `verify` reports the **first** divergent tick
- [x] Cross-**process** determinism (two separate launches, not two in-process runs)
- [x] **Negative control passed.** A single extra RNG draw injected at tick 50000 — one draw out of roughly 300,000 — was caught, and reported as first divergence at tick 51000, exactly as predicted. A determinism test that has never failed has not been tested; this one has.

### M0.7 — Profiler and telemetry — **DONE**
- [x] Per-system timings in a preallocated ring, zero allocation in the record path
- [x] Budgets from `CLAUDE.md` §4 encoded as data; overruns counted per system
- [x] Headless CSV out; in-engine overlay written
- [x] Telemetry deliberately has **no** `HashInto` and is excluded from world state — timings are observation, never state
- [ ] Mid-range GPU preset for honest profiling (R-07) — deferred to Phase 1, when there is something to render

### M0.8 — Unreal client placeholder — **DONE, verified running**
- [x] `ULivingCitySubsystem` starts and stops the sim thread with the world
- [x] View sync reads the snapshot ring and interpolates between the last two snapshots
- [x] `ALivingCityPlaceholder` — a cube whose transform comes from sim state, no engine-side logic
- [x] Player input pushed as sim commands, never applied locally
- [x] `ALivingCityHUD` profiler overlay against the frame and tick budgets
- [x] `ALivingCityGameMode::BuildScene` spawns ground, sun and sky in code — no `.umap` of our own (D-016)
- [x] **Compiled, launched and verified.** The sim ran **2129 ticks over ~106 s of uptime — exactly 20.0 Hz** — on its own thread inside Unreal, with a clean start and a clean shutdown and no errors in the log.

### M0.9 — CI, docs, close-out — **DONE**
- [x] `Scripts/Run-CI.ps1`, installed as a pre-commit hook
- [x] `.github/workflows/ci.yml`, dormant until a remote exists
- [x] Layer-DAG and module-isolation checks in `Check-SimPurity.ps1`
- [x] `CLAUDE.md` and `DECISIONS.md` current
- [ ] `docs/phases/PHASE_1.md` drafted — next session

---

## 4. How the build and CI work

### Path A — `cl.exe` directly, no Unreal, no CMake (the loop we live in)

```powershell
.\Scripts\Build-Sim.ps1                 # Release, warnings as errors
.\Scripts\Build-Sim.ps1 -Config Debug
.\Sim\build\Release\livingcity_tests.exe
.\Sim\build\Release\livingcity_headless.exe run --seed 1337 --ticks 100000 --hash-every 1000
```

`Build-Sim.ps1` locates `vcvars64.bat`, compiles every `.cpp` under `Sim/` in sorted order for a
deterministic link, and defines `LC_TRACK_ALLOCATIONS` — which replaces global `operator new` so
I5 is measurable. **That define must never reach an Unreal build**; `LivingCitySim.Build.cs`
deliberately omits it.

### Path B — UBT, inside Unreal

```powershell
& "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" `
    RiftboundEditor Win64 Development `
    -Project="C:\Path\To\GameProject\Riftbound.uproject"
```

Proves `LivingCitySim` compiles and links under UE's toolchain and that the UHT-generated
`LivingCityGame` classes are sound. **Not** the iteration loop.

### The commit gate

`Scripts/Run-CI.ps1` runs six steps and fails fast: purity → build → tests → selfcheck →
cross-process determinism → record-and-replay. Installed with `-InstallHook`.

Step 5 uses two **separate process launches** on purpose. An in-process comparison would miss
determinism bugs caused by address-space layout or static-initialisation order — exactly the
bugs that are hardest to find later.

---

## 5. What is blocked, and why

**Nothing, in the Unreal client.** M0.8 was blocked by an active Live Coding session; once the
editor was closed it compiled, launched and ran correctly.

Three real defects surfaced on the way, all now fixed and written up:

| Defect | Cause | Entry |
|---|---|---|
| 11 unresolved externals | a UE module is a DLL and exports nothing without `__declspec(dllexport)`; the sim DLL exported exactly **1** symbol | D-015 |
| C4702 unreachable code | the aggregate translation unit lets MSVC see `FailAssert` ends in `abort()`, so it correctly called the code after `LC_FATAL` dead | D-015 |
| "module could not be successfully initialized" | `LivingCitySim` had no `IMPLEMENT_MODULE` | D-015 |

One open item, not urgent: the sim restarts on level transition because it is owned by a
`UWorldSubsystem`. Fine for Phase 0, wrong from Phase 1. See **D-017**.

---

## 5b. Second blocker: Smart App Control

Windows Smart App Control is **enforced** on this machine and blocked
`livingcity_headless.exe` part-way through the session, after the same binary had run fine.
It is reputation-based and per-binary, so it hits unpredictably - `livingcity_tests.exe`, built
in the same run and equally unsigned, still executes.

It already broke the pre-commit gate at step 4. If `Run-CI.ps1` fails with
`An Application Control policy has blocked this file` / `ApplicationFailedException`, that is
this, not a determinism failure. Steps 1-3 remain valid.

Turning it off is irreversible without reinstalling Windows, so it was left alone.
See **D-014** for the options and the recommendation.

---

## 6. Encoding rule for PowerShell files — learned the hard way

**Every `.ps1` in this repository must be pure ASCII.**

Windows PowerShell 5.1 decodes a `.ps1` without a BOM as Windows-1252. A UTF-8 em-dash
(`E2 80 94`) therefore becomes three characters, the last of which is `”` (U+201D) — and 5.1
accepts a curly quote as a **string delimiter**. The result is a string that closes early and a
cascade of parse errors pointing at lines that are perfectly fine.

`Check-SimPurity.ps1` hit exactly this and was unusable until it was rewritten in ASCII. Em-dashes
inside `#` comments are harmless (the line ends anyway), which is why `Build-Sim.ps1` survived and
made the failure look inconsistent.
