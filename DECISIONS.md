# DECISIONS — LIVING CITY

Append-only log. What was chosen, what was rejected, and why.
Each entry is `DECIDED` (settled, change requires a new entry) or `OPEN` (waiting on the
project owner — do not start work that depends on it).

Current integration boundary: **D-030** supersedes the alongside-only restrictions in
D-000 and D-029. Historical entries below retain their original wording.

---

## D-000 — Engine and host project — DECIDED — 2026-09-09

**Chosen:** Unreal Engine **5.8.2**, building LIVING CITY as new modules inside the existing
`Riftbound.uproject` in this repository.

**Rejected — Godot 4.4 + C#.** The first brief specified it; the owner overrode to Unreal.
No further consideration.

**Rejected — Unreal 5.5.** The second brief specified 5.5. It is **not installed** on this
machine, and the only engine present is 5.8.2. Installing 5.5 alongside would cost ~100 GB, and
the existing project is already associated with 5.8. The owner then confirmed "latest version".
5.5 is not revisited.

**Rejected — a separate `.uproject` for LIVING CITY.** The owner explicitly said to build on top
of the existing project. Two `.uproject` files sharing one `Source/` and `Content/` tree is a
known source of UBT confusion. One project, separate modules, is cleaner and keeps the working
Riftbound launchers, build scripts, and packaging intact.

**Rejected — renaming `Riftbound.uproject` to `LivingCity.uproject`.** The project name is baked
into target names, binary names, `Scripts/Play.ps1`, `Scripts/Build.ps1`, `Scripts/Package.ps1`,
the `.cmd` launchers, and a 332 MB shipped `Riftbound.exe`. A rename breaks all of it for a
cosmetic gain. Revisit only if LIVING CITY is ever split into its own repository, at which point
the module layout makes the split cheap.

**Consequence:** strict non-interference. No module dependency in either direction between
`Riftbound` and `LivingCity*`. See `CLAUDE.md` §9.

---

## D-001 — What role Mass actually plays — **OPEN**

**The conflict.** The brief says two things that cannot both be true:

- Rule 1: the sim core is engine-agnostic C++ with zero Unreal types, and Rule 3: it is
  deterministic, bit for bit, same seed same result.
- Stack: "MassEntity / MassAI / MassCrowd — ECS-based agent representation and LOD. This is the
  single biggest reason we chose Unreal. Do not hand-roll an agent system."

Mass processors are Unreal types, run on the engine tick, and are **multithreaded with
non-deterministic execution order**. If Mass is the agent representation, then agents are not in
the pure sim, the sim is not deterministic, and none of I1, I3, I5, I6, I7 or I11 can be tested
headlessly. Determinism and Mass-as-agent-system are mutually exclusive.

Verified status also matters here: MassGameplay, MassAI and MassCrowd are all
`IsExperimentalVersion: true`, v0.4, off by default. MassEntity itself is now a stable engine
module.

### Options

| | Approach | Cost | Consequence |
|---|---|---|---|
| **A** | Mass is the agent system, as the brief literally reads | Lowest up-front code | Determinism dies. Headless testing dies. The replay harness, the money-conservation test, and save/load equality all become untestable. Locked to three Experimental v0.4 plugins that churn every engine upgrade. |
| **B** | **Sim owns agents; Mass is the crowd *rendering* layer** | ~2–3 weeks writing our own SoA agent store — which R1/R3 require anyway | All 50k citizens live in `LivingCitySim` as deterministic SoA rows. MassEntity holds only the transient *visual* representation of the 5k–15k currently visible: transform, velocity, anim state, LOD. Fed from the sim each frame, never writes back. "Do not hand-roll an agent system" is honoured where it actually pays — instanced crowd rendering and visual LOD, which is the genuinely hard part Mass solves. |
| **C** | No Mass at all; HISM/ISM plus our own batching | ~1–2 weeks more than B | Zero Experimental dependency. Loses Mass's LOD and visualisation machinery. Viable as the escape hatch if Mass churns badly on an engine upgrade. |

**Recommendation: B**, with C kept as a named fallback behind an `ICrowdRenderer` interface we
own. B is the only option that keeps the determinism the rest of the plan is built on, and the
"hand-rolled" part is a flat array of structs, not an ECS framework.

**Blocks:** Phase 2. Does not block Phase 0 or Phase 1.

---

## D-002 — Traffic, given that MassTraffic does not exist — **OPEN**

**The finding.** MassTraffic is **not in Unreal Engine 5.8.2**. A full scan of
`UE_5.8/Engine/Plugins` returns no `MassTraffic.uplugin` and no MassTraffic module anywhere in
the engine. It has never been an engine plugin — it ships inside Epic's **City Sample** project
(the Matrix Awakens demo). The brief treats it as a given; it is not one.

ZoneGraph *does* ship, but as an Experimental v0.5 plugin oriented toward **editor-authored**
lane graphs. LIVING CITY generates its road network procedurally from a seed, so the authoring
half of ZoneGraph is of little use to us.

### Options

| | Approach | Cost | Consequence |
|---|---|---|---|
| **A** | Port MassTraffic out of City Sample | Weeks. It is tightly coupled to City Sample's assets and conventions, targets an older engine version, and drags in the Experimental Mass stack | Large, opaque, third-party surface area that we did not write and cannot easily debug, sitting on the critical path of Phases 4 and 6 |
| **B** | **Own the lane graph and car-following in `LivingCitySim`; render through Mass/ISM** | ~2 weeks for a 1D lane-queue model with car-following and lane-changing | Deterministic, headless-testable, and congestion feeds the economy through the same event bus as everything else. We generate the graph anyway. Traffic is 1D per lane — this is genuinely not hard. |
| **C** | ZoneGraph as the lane data format, custom processors on top | ~1.5 weeks | Buys an Experimental v0.5 dependency for a data structure we could define in a header |

**Recommendation: B.** Optionally emit a ZoneGraph representation on the presentation side later
if MassCrowd pathing turns out to want one — but the authoritative graph stays in the sim.

**Blocks:** Phase 4. Does not block Phases 0–3.

---

## D-003 — Nanite, Lumen, and runtime voxel meshes — DECIDED — 2026-09-09

**The trade-off the brief already anticipated:** runtime-generated voxel meshes cannot use
Nanite. Nanite requires an offline-built cluster hierarchy; there is no runtime Nanite build path
for geometry generated during play.

**The part the brief did not anticipate, and it is worse:** Lumen's *software* ray tracing
resolves geometry through **Mesh Distance Fields**, which are also built offline. Runtime-
generated voxel meshes have no distance field. The consequence is not "slightly lower quality" —
it is that **voxel terrain is invisible to software Lumen entirely**: no bounce light off the
ground, no ambient occlusion from terrain, and objects sitting in a dug pit lit as if the pit
were not there.

**Decision:**

1. Nanite and Lumen carry the **static** content — buildings, vehicles, props, interiors. These
   are conventional static meshes and behave normally.
2. Voxel terrain gets conventional LOD chains and aggressive chunk culling. **Budget terrain
   triangles explicitly. Nanite will not bail us out.**
3. For terrain in Lumen, the choice is made in Phase 1 once there is something to measure:
   - **Lumen hardware ray tracing** — correct, sees dynamic geometry, costs materially more GPU
     and requires an RT-capable card. The dev 7900 XT has RT, so this will look fine here and
     may not on the mid-range target (see R-07).
   - **Global Distance Field only** — coarse, cheap, better than nothing.
   - **Accept terrain not contributing to GI** — cheapest, and with a flat-shaded art direction
     and strong directional lighting it may genuinely be acceptable. Test before assuming not.
4. Art direction helps here rather than hurting: flat-shaded low-poly with a tight palette does
   not depend on subtle bounce lighting.

---

## D-004 — Voxel mesh component — **OPEN**

**The finding.** `RealtimeMeshComponent` is **not installed** and is not part of the engine. It
is a third-party GitHub dependency that must be fetched. Its licensing has changed over time and
the brief's "free, open source" description **must be re-verified against the current upstream
license before we take the dependency** — this cannot be checked offline from here.
`ProceduralMeshComponent` v1.0 ships with the engine, is production (not Beta), and is **already
enabled in `Riftbound.uproject`**.

### Options

| | Approach | Cost | Consequence |
|---|---|---|---|
| **A** | RealtimeMeshComponent | Fetch, vendor, verify license, maintain a third-party dep | Purpose-built for exactly this, best update path for streaming chunks |
| **B** | **ProceduralMeshComponent** | Zero — already present and enabled | Heavier per-section update cost and more churn on remesh. Fine for Phase 1 correctness, may not hold the 4 ms streaming budget at scale |
| **C** | Custom `UPrimitiveComponent` with a static-mesh-style render proxy | ~1 week | Full control, no dependency, most work |

**Recommendation: start on B, keep the door open.** The greedy mesher produces plain vertex and
index buffers; the component is only the sink. Put an `IChunkMeshSink` interface between them in
Phase 1 M-early and the swap to A or C later costs a day. Decide with a profile, not a guess.

**Blocks:** nothing yet. Phase 1 starts on B regardless.

---

## D-005 — How `LivingCitySim` builds both with and without Unreal — DECIDED — 2026-09-09

R1 requires the sim to build as a standalone console executable with no Unreal, *and* it must
also be linked into the Unreal game module.

**Chosen: compile the same sources twice.**

- **CMake** (`Sim/CMakeLists.txt`) builds `livingcity_headless.exe` and `livingcity_tests.exe`
  using the MSVC toolchain at `C:\BuildTools`. This is the fast loop and what CI runs.
- **UBT** builds `Source/LivingCitySim/LivingCitySim.Build.cs`, which compiles the *same* files
  from `Sim/src` with `bUseUnity = false` and `PCHUsage = NoPCHs`.

**Rejected — build a static library with CMake and link it into Unreal as a third-party module.**
Architecturally cleaner, but it means matching UE's CRT, exception, RTTI and iterator-debug
settings exactly. Mismatches there produce link errors and heap corruption that cost days to
diagnose. Not worth it.

**Note on R1's letter vs its spirit.** `LivingCitySim.Build.cs` will list `Core` as a dependency,
because UBT requires every module to have one. **No sim source file includes any Unreal header.**
That is the invariant that matters (I2), and it is what `Check-SimPurity.ps1` enforces — it greps
the `.cpp`/`.h` files, not the `.Build.cs`.

---

## D-006 — Reconciling two conflicting briefs — DECIDED — 2026-09-09

Three briefs arrived: a Godot 4.4 version, an Unreal 5.5 version, and then the Godot text again
prefixed "use latest version or unreal".

**Resolution:** the **Unreal** brief is authoritative for stack, rules, and targets, because it
is the one written for this engine and it is strictly more complete — it adds the pure-C++ sim
module rule, the no-binary-game-data rule, the thin-Blueprints rule, Git LFS, the Chaos-is-
visuals-only rule, the ≥1000× headless throughput target, and the Mass-rendered crowd target.
Engine version is the **latest installed, 5.8.2**, not the 5.5 that brief named.

Items unique to the Godot brief that carry over unchanged because they are engine-agnostic: the
nine-need list, the goods list, the mineral list, the generation pipeline order, the violation
categories, the art direction, and the phase plan (identical in both).

---

## D-007 — Tier 2 keeps persistent identity — DECIDED — 2026-09-09

The brief describes Tier 2 citizens as aggregated into cohorts that behave "as distributions",
with individuals "re-materialised with full identity on approach".

**This breaks invariant I6.** If a Tier 2 citizen exists only as a statistic, then promoting them
has to *invent* their bank balance and criminal record. Promotion is then lossless only by
accident, and a player who follows a citizen across a district boundary and back can catch the
seam.

**Chosen:** every citizen keeps a persistent identity and ledger row for the entire run.
50,000 × ~200 bytes ≈ **10 MB**. This is nothing. What the tiers change is **tick frequency and
behavioural fidelity**, not existence. Tier 2 ticks the *cohort* and writes results back to
members as a shared delta plus a per-agent residual, so an individual's balance is always exact
and promotion is genuinely lossless rather than plausibly lossless.

**Rejected — true statistical-only Tier 2 with re-materialisation.** Saves ~10 MB of RAM and
costs the single most verifiable promise in the design.

---

## D-008 — Tick hierarchy, because the throughput targets contradict each other — DECIDED — 2026-09-09

The brief asks for "≥ 1000× real time" headless *and* "a simulated year in minutes". These are
not the same requirement. A year at 20 Hz is 365 × 86,400 × 20 = **630,720,000 ticks**. At 1000×
real time that run takes **8.8 hours**.

**Chosen:** the sim has a tick hierarchy, not one rate. Markets, taxes and demography do not need
20 Hz and never did.

| Rate | What runs |
|---|---|
| 20 Hz | embodied agents, physics-adjacent state, observation and law events |
| 1 Hz | Tier 1 needs |
| 1 tick / sim-minute | market clearing, wages, rents, utilities, demography |
| daily / monthly | taxes, budgets, mortgages, elections |

A simulated year is then ~525,600 coarse ticks ≈ **30 seconds**, which is what "a year in
minutes" actually wanted. The ≥1000× fine-tick target stands separately as the Tier-2 throughput
figure (≥ 20,000 ticks/s).

---

## D-009 — Git and LFS — DECIDED — 2026-09-09

This directory was **not** a git repository. It is now initialised, with LFS configured from the
first commit as the brief requires. Repository content excluding build artefacts is 17 MB with
14 `.uasset` and 1 `.umap`, so there is no large-binary history problem to unwind.

`.gitattributes` routes art and binaries to LFS and explicitly marks code, CSV, JSON and docs as
text so they stay diffable — which R6 depends on.

---

## D-010 — Build the sim core with `cl.exe` directly, not CMake — DECIDED — 2026-09-09

D-005 assumed CMake. **Neither CMake nor Ninja is installed on this machine.** MSVC 19.44 and
`vcvars64.bat` are, at `C:\BuildTools`.

**Chosen:** `Scripts/Build-Sim.ps1` invokes `cl.exe` directly through `vcvars64.bat`. It globs
`Sim/**/*.cpp`, compiles in sorted order for a deterministic link, and emits both binaries.
Zero external dependencies, and a clean build of the whole core takes 17 s.

**Rejected - install CMake.** It would buy nothing here. The sim core is a flat set of `.cpp`
files with no external dependencies, no generated code, and one platform. CMake's value is
managing complexity this project does not have. Revisit only if a second platform appears.

**Also dropped: `Source/LivingCityHeadless.Target.cs`.** It declared `LaunchModuleName =
LivingCitySim`, but that module has no `main()`, so a UE Program target could never link.
A working one would need an extra entry-point module, and it would duplicate what
`Build-Sim.ps1` already does faster and without the engine. What actually needed proving -
that `LivingCitySim` compiles under UE's toolchain - is proven when `LivingCityGame` builds.
Shipping a target that cannot link would leave the repo non-building, which CLAUDE.md forbids.

---

## D-011 — Every `.ps1` in this repository must be pure ASCII — DECIDED — 2026-09-09

**Found by breaking.** `Check-SimPurity.ps1` failed to parse with errors pointing at lines that
were syntactically perfect.

**Cause.** Windows PowerShell 5.1 decodes a `.ps1` without a BOM as Windows-1252, not UTF-8.
A UTF-8 em-dash is `E2 80 94`; read as Windows-1252 that is three characters ending in `"`
(U+201D, right double quotation mark). PowerShell 5.1 accepts curly quotes as **string
delimiters**, so the em-dash silently closed a string early and every construct after it
mis-parsed.

Em-dashes inside `#` comments are harmless - the comment ends at the newline either way - which
is why `Build-Sim.ps1` worked and made the failure look intermittent and confusing.

**Chosen:** all `.ps1` files, and the inline PowerShell inside `.github/workflows/ci.yml`, are
kept pure ASCII. Markdown and C++ are unaffected and keep their typography.

**Rejected - save `.ps1` as UTF-8 with BOM.** It works, but a BOM is easy to lose to any editor
or tool that rewrites the file, and the failure it reintroduces is this same baffling one.
ASCII cannot regress.

**Incident note:** the first automated fix pass opened files in `'w'` mode and then hit an
encode error on a character missing from its replacement table. `'w'` truncates on open, so
`Check-SimPurity.ps1` was left at 0 bytes and had to be rewritten. Encode first, write second.

---

## D-012 — `CommandQueue` counts rejections, not drops — DECIDED — 2026-09-09

The queue originally exposed `DroppedCount()` and logged a warning every time `Push` returned
false. A threaded test that retries on a full queue - losing nothing - still reported 81 "drops"
and emitted 81 warnings.

**The queue cannot know the caller's policy.** `Push` returning false means *not accepted*. A
producer that retries has lost nothing; one that discards has genuinely lost player intent.

**Chosen:** rename to `RejectedPushCount()`, and remove the per-rejection log. The caller that
chooses to discard is the one that logs it - `ULivingCitySubsystem::PushCommand` does exactly
that, because it does not retry. Logging inside `Push` would also spam once per spin of any
retrying producer.

`Push` still never overwrites an unread command. Overwriting would silently change the input log
and therefore the world (I1), which is far worse than refusing one keypress.

---

## D-013 — Event payloads must have no padding bytes — DECIDED — 2026-09-09

Queued events are hashed byte-for-byte into the world hash. Padding bytes inserted by the
compiler are **indeterminate**, so a struct with padding would make the world hash vary between
builds, or even between runs - silently breaking I1 in a way that looks like a heisenbug.

**Chosen:** `EventBus::Publish` static_asserts `std::has_unique_object_representations_v<T>`.
Event authors either order fields largest-first or add an explicit, always-zeroed pad member.
This caught the very first event type written against it (`TickBoundaryEvent`, 8 + 4 bytes,
padded to 16) at compile time rather than as a determinism failure weeks later.

The same reasoning drives the replay format: `InputLog::Serialise` writes every field explicitly
in little-endian order and never `memcpy`s a struct.

---

## D-014 — Smart App Control blocks our own build output — **OPEN, owner decision required**

**The finding.** Windows Smart App Control is **enforced** on this machine
(`HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy\VerifiedAndReputablePolicyState = 1`).
It blocked `livingcity_headless.exe` mid-session, after the same binary had run fine minutes
earlier:

```
Program 'livingcity_headless.exe' failed to run:
An Application Control policy has blocked this file
```

Code Integrity log (`Microsoft-Windows-CodeIntegrity/Operational`, events 3033 and 3077)
confirms it: the file "did not meet the Enterprise signing level requirements". The binary is
`NotSigned`, as every freshly compiled executable is.

**Why this matters more than one failed command.** Smart App Control is reputation-based and
per-binary. `livingcity_tests.exe` - equally unsigned, built in the same run - still executes,
while the headless host does not. So the failure is *intermittent and unpredictable*, and it
lands on exactly the thing this project does constantly: produce new unsigned executables. It
already broke the pre-commit gate at step 4.

**Options.**

| | Approach | Cost |
|---|---|---|
| **A** | **Turn Smart App Control off** (Windows Security -> App & browser control -> Smart App Control -> Off) | **Irreversible.** Microsoft provides no supported way to re-enable it; that requires reinstalling Windows. In exchange, self-compiled binaries run normally. |
| **B** | Code-sign every build | Does not reliably help. SAC wants a *reputable* signature; a self-signed certificate is not one, and an EV certificate is a paid annual product - which the brief rules out. |
| **C** | Leave it on and work around it | There is no exclusion list. Every rebuild is a coin flip. Not workable for daily development. |

**Recommendation: A**, with the irreversibility stated plainly because it is the whole cost.
Smart App Control and compiling your own C++ are fundamentally incompatible, and this project
will produce thousands of unsigned binaries.

**Not actioned.** Disabling SAC is a one-way security change to the owner's machine. That is
their decision, not the agent's, and nothing here touched it.

**Until it is decided:** `Scripts/Run-CI.ps1` may fail at step 4 or 5 with an Application
Control error. That is an environment failure, not a determinism failure - the giveaway is
`ApplicationFailedException` rather than a hash mismatch. Steps 1-3 (purity, build, 110 tests)
are unaffected and still meaningful.

### D-014 update - 2026-09-09, later the same session

The block is best described as **unpredictable** rather than intermittent or persistent.
`livingcity_headless.exe` was blocked through five consecutive retries AND a clean rebuild,
then ran normally after a later rebuild. Three further findings:

- A clean rebuild does not **reliably** clear it. The build is deterministic, so it usually
  reproduces the same bytes and therefore the same verdict - but a later rebuild did produce
  a binary that ran. Do not count on either outcome.
- Copying the binary to a different name and directory is **also** blocked, which proves the
  verdict follows the file contents rather than the path.
- `livingcity_tests.exe`, built in the same run from the same sources and equally unsigned,
  still runs. So this is a per-binary reputation judgement, not a blanket rule we can predict
  or design around.

Practical effect: any run may lose the determinism demo, the replay check and the benchmark,
and `Scripts/Run-CI.ps1` may be unable to complete steps 4-6. When that happens nothing was
verified either way - which is exactly why it must not be reported as a determinism failure.
`Scripts/Run-LivingCitySim.ps1` and `Run-CI.ps1` now both detect this and report it as an
environment block, distinct from a code failure, so it can never be mistaken for a
determinism bug.

This moves D-014 from an annoyance to a **blocker on running and gating the project**. The
recommendation stands and so does its cost: turning Smart App Control off is irreversible
without reinstalling Windows.

---

## D-015 — The sim module must export its symbols, and implement a module — DECIDED — 2026-09-09

**Symptom.** `LivingCityGame` failed to link with 11 unresolved externals, every one of them
a sim function defined in a `.cpp`: `SnapshotRing::AcquireLatestPair`,
`Telemetry::LastTickTotalNs`, the whole of `SimThreadHost`. Anything header-only linked fine.

**Cause.** In an Unreal EDITOR build every module is a separate DLL, and a DLL exports
nothing unless it is told to. `dumpbin /EXPORTS` on `UnrealEditor-LivingCitySim.dll` returned
exactly one name: `ThisIsAnUnrealEngineModule`. The sim compiled perfectly and exported
nothing. Header-only code linked because it is compiled into the consumer, which is precisely
why the failures fell along that line.

This was a real hole in D-005. Compiling the same sources two ways was right; assuming both
ways *link* the same way was not.

**Chosen:** a portable `LC_API` macro in `Core.h`, driven by two defines from
`LivingCitySim.Build.cs` - `LC_SHARED_BUILD` public (consumers get `dllimport`) and
`LC_EXPORTS` private (the module gets `dllexport`). Neither is defined by
`Scripts/Build-Sim.ps1`, so `LC_API` vanishes in the standalone build and nothing changes
there. Applied to the seven classes with out-of-line members and the ten free functions.
Exports went from 1 to 136.

**Rejected - compile the sim directly into `LivingCityGame` and drop the module.** Simpler,
and it would have removed the DLL boundary entirely. But `LivingCityEditor` will want the sim
directly in Phase 1 for headless city-generation commandlets, and routing it through the game
module then would be worse than one macro now.

**R1 is untouched.** `LC_API` is plain MSVC/GCC visibility syntax. No source under `Sim/`
mentions Unreal, and `Check-SimPurity.ps1` - which scans `Sim/`, not `Source/` - stays clean.

### Two consequences worth writing down

**C4702, unreachable code.** UBT compiles the sim through one aggregate translation unit, so
MSVC can see `FailAssert`'s body, notices it ends in `abort()`, and correctly concludes it
never returns - making every statement after an `LC_FATAL` dead code, which Unreal treats as
an error. The standalone build compiles those files separately and cannot see it. The fix was
to declare the truth: `FailAssert` is now `[[noreturn]]` and the dead trailing `return`
statements are gone. The compiler was right and the code was lying to it.

**Every UE module needs `IMPLEMENT_MODULE`.** Without it the DLL loads and Unreal reports
*"The game module 'LivingCitySim' could not be successfully initialized after it was loaded"*.
`Source/LivingCitySim/Private/LivingCitySimModule.cpp` supplies it, using
`FDefaultModuleImpl` rather than `FDefaultGameModuleImpl` because the module carries no
gameplay and owns no UObjects.

---

## D-016 — Phase 0 needs no level asset of its own — DECIDED — 2026-09-09

The plan was to generate `/Game/LivingCity/Maps/L_LivingCity` with a Python commandlet. The
commandlet started the editor and exited without running the script, and rather than debug
editor scripting the requirement itself was questioned.

**Chosen:** launch the stock `/Engine/Maps/Entry` and have `ALivingCityGameMode::BuildScene`
spawn the ground, sun and sky in code. Verified working.

This is better than the original plan on every axis that matters here. A `.umap` is a binary
asset - undiffable, unreviewable, uneditable outside the editor - and rule R6 pushes hard
against adding one. Generating the scene in code also deletes a failure mode: no editor
commandlet has to succeed before the game will run, so the launcher cannot get stuck on a
step that has nothing to do with the simulation.

`Scripts/bootstrap_livingcity_map.py` is kept for Phase 1, when an authored city level is
actually wanted, but nothing depends on it now.

---

## D-017 — The sim restarts on level transition — **OPEN, not urgent**

Observed in the log during verification:

```
LIVING CITY simulation started on its own thread (seed 1337, 20 Hz).
LIVING CITY simulation stopped after 6 ticks.
LIVING CITY simulation started on its own thread (seed 1337, 20 Hz).
```

`ULivingCitySubsystem` is a `UWorldSubsystem`, so its lifetime is the world's. Browsing to a
map tears the simulation down and starts a fresh one. Correct for Phase 0 - the sim carries
nothing worth keeping yet - and it proves start/stop is clean, which is worth having verified.

It is wrong from Phase 1 onward: a city must survive a level transition. The fix is to move
ownership to a `UGameInstanceSubsystem` and leave only the view-sync in the world subsystem.
Cheap now, expensive once saves exist. Do it early in Phase 1.

---

## D-018 — Crowd scatter is a rendering concern, not a simulation one — DECIDED — 2026-09-09

The first populated build showed **one** citizen on a street holding 1,500 of them. The
simulation was right: fourteen people who live in the same building all stand at the same
doorway, so fourteen capsules occupied one coordinate and read as a single person.

**Chosen:** the view scatters citizens around their exact simulated position - a stable
per-index offset of 0.6 to 3.0 m. The sim position stays exact, so the invariant tests that
pin arrival to a precise coordinate keep their meaning, and `citizens_arrive_exactly_and_never_overshoot`
still means what it says.

**Rejected - jitter the position in the simulation.** It would have made a rendering problem
into a simulation one, weakened a real invariant, and put a cosmetic value into the world hash.

The offset is derived from the citizen index rather than randomised per frame: a citizen
shuffling around their doorway every tick would be worse than the stack.

**Also fixed here:** citizens were being placed at their building's CENTRE on the first tick -
inside the geometry, invisible and standing in a wall. Arrivals already used the kerb outside
the building; only the initial placement did not. Now both agree, so "at home" means one thing
everywhere.

### Staggered departures - tried, reverted, and why

Everyone on a shift still leaves on the same tick. Spreading departures across the hour is the
obvious next improvement and it was implemented, then reverted: the citizen tests model an
"hour" as 2,000 ticks (about 100 simulated seconds) rather than the real 72,000, so a departure
scheduled for minute 35 never fires inside the test window and every arrival invariant fails.

The honest fix is to give those tests a real clock first, not to loosen invariants around a
cosmetic improvement. Left out deliberately, with the reason recorded in `Citizens.cpp`.

---

## D-019 — `verify` replayed from tick zero — DECIDED — 2026-09-09

Adding `SimConfig::startHour` broke replay, and CI caught it.

`CmdVerify` stepped `for (t = 0; t < lastTick; ++t)`, which assumes the world begins at tick 0.
Once it opened at 07:00 - tick 504,000 - a run recorded over 20,000 ticks replayed 524,000
ticks instead, ran twenty-six times past anything that was ever recorded, and reported a
divergence entirely of the harness's own making.

**Fixed:** `while (sim.CurrentTick() < lastTick) sim.Step()` - replay *to* the recorded tick,
never *from* zero.

**Worth noting how it presented.** The first symptom was a PowerShell `NativeCommandError`,
which looked like the `2>&1` native-stderr trap the CI script already had elsewhere. That trap
was real and was fixed, but it was not this. Running the binary directly showed a genuine
`DETERMINISM FAILURE` underneath. A plausible explanation for a failure is not a diagnosis.

**Known gap:** the replay log persists only the world seed, not the whole `SimConfig`. A log
recorded under different defaults will not replay correctly against changed ones. Both `run`
and `verify` use the defaults today so they agree, but the format should carry the config -
worth doing before saves exist.

---

## D-020 — Never run a blanket literal substitution across a source file — DECIDED — 2026-09-09

Filling in a known-answer test's placeholder constants, a `str.replace` of
`LC_CHECK_EQ(city.Count(), 0u)` -> `109u` was applied to the whole file. It hit the intended
placeholder **and** the assertion inside `city_clear_empties_every_index_it_owns`, rewriting it
to claim that `Clear()` leaves 109 buildings behind - a test that could only pass against a
broken `Clear()`. The subagent that owned the file caught it and restored it.

A substitution keyed on a literal as ordinary as `0u` hits correct code as readily as
placeholders, and the damage is invisible: the suite still goes green, but one test now asserts
the opposite of what it was written to prove.

**Rule:** edit by anchor with surrounding context, or by explicit line, and never `replace_all`
on a short literal. Where a global change is genuinely wanted, print every site first.

---

## D-021 — Take Riftbound's atmosphere, not its planet — DECIDED — 2026-09-11

The owner asked to "modify Riftbound to be a real living game - the game already had an
atmosphere". Two readings, materially different work:

| | Reading | Cost |
|---|---|---|
| A | Put the living-city simulation INTO Riftbound's world | Riftbound's world is a **sphere**: radial gravity, `FaceRotation` overridden, settlements placed by unit direction on a planet. A street grid with 1,500 people walking L-shaped integer paths cannot live on a sphere without rewriting every coordinate in the sim, the pathing, the snapshot and the tests. Weeks, and it would put simulation logic back inside `AActor`s. |
| B | **Give LIVING CITY Riftbound's atmosphere** | Port the sky/cloud/fog/exposure recipe from `VoyagerWorld.cpp` (lines 495-555) into a flat-world actor, and reuse `M_VoyagerArchitecture` and `M_VoyagerTerrain` on our buildings and ground. Read-only reuse of existing assets. Days, not weeks. |

**Chosen: B.** What Riftbound has that is worth taking is the look. Its world model is the
wrong shape for a city. `Source/Riftbound/**` stays untouched, as CLAUDE.md §9 requires.

Two things could not be ported literally. `M_VoyagerCloudVolume` takes a `PlanetCenter`
parameter and assumes a sphere - the engine's default volumetric cloud material is used
instead. And `SkyAtmosphere` runs in `PlanetTop` mode, not Riftbound's
`PlanetCenterAtComponentTransform`.

A spherical LIVING CITY is not ruled out forever. It is a coordinate-system decision for the
voxel world in Phase 1, and it should be made then, on purpose, with the cost in view.

---

## D-022 — The first economy is one closed loop with one good — DECIDED — 2026-09-11

The brief's economy is large: eight goods, firms that hire and expand, labour and housing
markets, utilities, waste. Building all of it before any of it is verified is exactly the
"scaffold ten systems" failure CLAUDE.md warns against.

**Chosen:** the smallest economy that has every property the real one must have.

```
government --contracts--> firms --wages--> citizens --food--> shops
     ^                                                          |
     +--------------- wholesale food + sales tax ---------------+
```

- **One closed monetary loop.** The government is the central bank. It mints once at
  generation; every other movement is a `Ledger::Transfer`. Invariant I3 is asserted on every
  tick in the test, not sampled.
- **One good.** Food. Created from nothing by the wholesaler - goods are not money - sold to
  shops, eaten by citizens.
- **Two needs.** Hunger and fatigue, integer pressures 0-1000, moved on the 1 Hz schedule.
  Hunger drives shopping; fatigue is restored at home.
- **Prices emerge.** A shop's price is a function of its own stock. Cut the wholesale supply
  and prices must rise; there is a test that turns the supply off and checks.
- **The economy decides, citizens execute.** `Economy::Tick` calls `Citizens::SetPendingErrand`;
  Citizens walks there on its next departure. That keeps the layer graph acyclic: economy
  depends on agents, never the reverse.

Everything in the full brief - more goods, firms that hire, rent, utilities - is a widening
of this loop, not a replacement for it. Widen only after each ring is tested.

---

## D-023 — Terrain modification starts as a heightfield, not voxels — DECIDED — 2026-09-11

The brief specifies 32x32x32 voxel chunks with greedy meshing, caves, mineral seams and a
"clay" density field. That is Phase 1 in full and it is weeks of work. The owner asked for
terrain modification now.

**Chosen:** a chunked integer heightfield the player can dig into and pile up, on the SAME
command/replay path as every other input. A dig is a `CommandType::ModifyTerrain` in the log,
so a replayed world has the same holes in it. Chunk versions let the view rebuild only what
moved; a copy behind a lock is the one read the render thread may perform.

Every rule this establishes carries over to voxels unchanged: integer heights, chunked dirty
tracking, the sim owning the authoritative data, saves as seed-plus-deltas (R5 -
`ModifiedCellCount` is the delta list). What does not carry over is caves and overhangs, which
a heightfield cannot represent. That is the Phase 1 job, and it will replace this class
rather than extend it.

**Rejected - ProceduralMeshComponent as the source of truth.** Tempting, because the mesh is
right there. But then the terrain would live in an `AActor`, the replay log could not
reproduce it, and rule R2 would be broken on the first click.

---

## D-024 — `economy` may depend on `world` — DECIDED — 2026-09-11

The layer DAG in `docs/ARCHITECTURE.md` §3 had `economy -> agents, core`. The economy needs the
city's building list to know where shops are and which buildings are firms. Added the edge
`economy -> world`; `Check-SimPurity.ps1` updated to match. No cycle: `world` depends only on
`core`.

---

## D-025 — Both builds write the UBT aggregate — DECIDED — 2026-09-11

**Found by breaking.** Two new sim sources (`Economy.cpp`, `Terrain.cpp`) built and passed
191 tests in the standalone build, then vanished from the Unreal link with ten unresolved
`Economy::*` symbols.

**Cause.** UBT compiles the sim through one generated aggregate,
`Source/LivingCitySim/Private/LivingCitySimUnity.cpp`, written by `LivingCitySim.Build.cs`.
UBT only re-evaluates module rules when a `.Build.cs` or `.Target.cs` changes. A new `.cpp`
under `Sim/src` changes neither, so the aggregate went stale and the old object was linked.

**Chosen:** `Scripts/Build-Sim.ps1` now writes the aggregate too - it already globs the same
sources, and it is the build everyone runs first. The two generators emit **byte-identical**
text (ASCII header, ordinal sort, CRLF, no BOM), verified by md5 before and after a UBT run,
so they never fight over the file. Whichever build runs first leaves it current.

**Rejected - touch the `.Build.cs` after adding a file.** A rule people have to remember is
not a fix.

---

## D-026 — Two shift bands, so the streets are busy all day — DECIDED — 2026-09-11

With every shift starting 06:00-10:00, the city had two rush hours and was otherwise still:
everyone at work at 10:00, everyone home by 19:00. The owner asked for a lot of people on
the street.

**Chosen:** 60% of citizens start in the morning (06:00-10:00), 40% in the afternoon
(11:00-14:00), all with 8-9 hour shifts. No shift crosses midnight - 14:00 + 9 h = 23:00 -
so the schedule stays a same-day comparison. The afternoon band's commute and evening errands
now fall in the hours the morning band is indoors, and vice versa.

Four tests pinned the old single-band schedule as if it were an invariant - "everyone at work
by 10:00", "21:00 is quiet for the whole city". Each was updated to assert the actual
invariant: everyone whose shift covers an hour is at work at that hour, nobody is still
walking to work, and samples of "everybody is home" are taken at 02:00-05:00, the only hours
that now hold. Staggered departures *within* an hour (D-018) remain deferred for the reason
given there.

**Also this round:** citizens who are AtWork or AtShop are no longer drawn. They are indoors;
the kerb position is where their walk ended, not where they stand. Drawing all 1,500 at ~35
workplace kerbs produced a solid wall of capsules at the centre intersection. AtHome stays
visible - 73 home kerbs spread the population thinly enough to read as people around their
houses.

## D-027 — The world hash is taken once per sim-second, and the terrain hashes by chunk — DECIDED — 2026-09-11

The first bench after the economy and terrain landed showed 2,360 ticks/s: 118x real time,
against a target of 1000x and a Phase 0 baseline of 119,411x. Zero allocations, so it was not
the usual suspect. It was `ComputeWorldHash`, called every tick by `PublishSnapshot`, walking
all 102,400 terrain cells - 400 KB of hashing per tick, more than everything else in the tick
put together.

**Options.** (a) Hash less often. (b) Make the terrain hash cheap. (c) Drop the hash from the
snapshot. (c) was rejected: the HUD hash is how a human notices a determinism break during
play, and the replay harness compares it. (a) and (b) are independent and both were taken.

**Chosen.**
- `PublishSnapshot` computes the hash on second boundaries (and once at start) and republishes
  the last value in between. Checkpoints, the replay log and `selfcheck` still hash whenever
  they need to - this changes only what the snapshot carries, and the snapshot is presentation.
- `Terrain` keeps one FNV digest per 16x16 chunk, computed in `Generate` and refreshed for each
  chunk a brush touches. `HashInto` folds the 400 digests instead of the 102,400 cells. The
  digest is a pure function of the heights, so I1 and I7 hold exactly as before; only the walk
  order changed, and the terrain known-answer test was re-captured in the same commit and
  says so.

**Result.** 55,520 ticks/s, 2,776x real time, 18 us/tick, 0 allocations. The headroom is 2.8x
over target with 1,500 citizens, an economy and a terrain; the remaining budget is what Phase 2
spends getting to 10,000.

**Lesson recorded.** Anything hashed every tick must be O(things that change), never O(world).
The same rule will apply to the voxel chunks that replace this heightfield.

## D-028 — Drain a bounded input batch without allocating — DECIDED — 2026-09-12

The live command ring accepts 256 commands, but `Simulation` reserved only 64 slots for its
per-tick batch. A valid burst could therefore allocate inside `Step`, violating I5. The drain
also kept popping until the ring was empty; a producer refilling freed slots could extend
that loop indefinitely and grow the vector beyond even a 256-slot reservation.

**Chosen:** reserve the ring's full capacity at construction, sample its pending count at
the start of the tick, and consume exactly that batch. Commands arriving during the drain
remain in FIFO order for the next tick. Accepted commands are neither discarded nor reordered.
The recorded consumption tick continues to be the replay authority.

Replay setup also reserves an upper bound from the log's command count, before ticking.
This permits a tool-spliced log with more than 256 commands on one tick without allocating
during replay. The tradeoff is a command scratch buffer proportional to the loaded input log;
reducing that to the busiest recorded tick is an optional setup-memory optimization later.

Two regressions verify full-ring live consumption and a larger, out-of-order replay fixture,
checking exact movement, pause state, exclusion of future commands and zero tick allocations.
The terrain cache now has a separate uncached full-height oracle for seam edits, saturation,
no-ops and object regeneration. Its existing known-answer value did not change.

The release benchmark on this machine ran 500,000 ticks in 8.9982 s: **55,566 ticks/s,
2,778x real time, 18.0 us/tick, zero allocations**. This verifies the 20,000 ticks/s target
for today's 1,500-citizen default workload only, not the planned full Tier-2 population.

## D-029 - Portable build gate and separate playable modes - DECIDED - 2026-09-12

The owner requested a public GitHub repository and confirmed Living City should remain
alongside Voyager. GitHub Actions now invokes the same Run-CI script as the local gate.
MSVC discovery supports developer shells, vswhere and an explicit toolchain override.
Both aggregate generators write the same relative includes, so a checkout can move between
machines. Determinism validation requires a valid hash and a successful exit from all runs.

The Living City simulation starts at OnWorldBeginPlay only for its own GameMode, before
the scene reads its layout. Other game modes no longer pay for an unused simulation thread.
The Windows package includes Entry as well as Forest, and Living City's launcher prefers
the package with current walking, terrain-editing and time controls.

Publication starts with a clean source snapshot while prior Git history stays local.
The old history contains save backups and an Android File Server development token;
neither belongs in the public repository. Android File Server is disabled, audio provenance
paths are relative, and personal saves and backups are ignored.

## D-030 — Put a bounded Living City economy into Voyager — DECIDED — 2026-09-13

The owner now explicitly requests that the Living City brief be added **to Riftbound**.
This supersedes the alongside-only choice in D-029 and CLAUDE section 9. Voyager's natural
art, enterable cities, seamless planetary flight, combat and co-op remain the game being
extended. The original Living City mode remains available independently.

**Chosen:** add `Riftbound` → `LivingCitySim` as a one-way dependency. A pure `PlanetaryCity`
core consumes Voyager's existing 60-building settlement layouts. Fifteen instances maintain
3,600 named resident records each, including reserved player resident slots. Each has a real
home, explicit employment status, account, inventory, nine needs and legal state. A 20 Hz
worker advances the core. Nearby actors execute its utility goals on the existing street
and doorway graph and report real arrivals; statistical residents retain the same records.
The current visual budget remains 36 residents per city and 72 globally, not 54,000 meshes.

The playable slice adds eight market goods, inventory-based prices, transfer-based wages
and taxes, rent and utility bills, consumption and recycling, mineral sales, and citations
connected to the existing wanted patrols. A keyboard city phone exposes authoritative
balances, needs, jobs, home, justice and factual bulletins. Residents and the player use the
same core accounts, action validation, needs and health rules. Existing optional Ollama
dialogue can vary truthful authored speech; it does not operate the economy or NPC utility AI.

**Persistence choice:** preserve resident identity, money, inventory, needs and records when
moving between settlement accounts. Assign valid local housing and employment on arrival.
This is not a property-ownership or mortgage simulation. The seed plus city deltas are saved
inside the expedition save; terrain remains Voyager's existing generated surface.

**Rejected:** coupling Voyager to `LivingCityGame` or loading its separate world behind the
planet scene. That would create two presentation owners and two unrelated cities, rather
than giving the existing streets an economy. Also rejected was claiming that enabling Mass,
ZoneGraph or StateTree completes traffic, crowds or behavior assets. Existing Unreal 5.8.2
facilities are sufficient for this bounded integration; no paid or third-party plugin is
required. The core remains independently buildable and testable through the direct MSVC
`Scripts/Build-Sim.ps1` workflow established in D-010; there is no CMake project.

**Scope:** full MassCrowd/Traffic representation, road vehicles and transit, mortgages,
eviction, courts and jail, destructible voxel terrain, hydrology and economic disasters
remain future work. The original brief is the roadmap, not the release checklist.
Validation and measured timings for this integration are tracked separately in
`docs/phases/VOYAGER_CITY_LIFE.md`; pending checks must not be reported as passed.

## D-031 — Bounded Voyager destruction, survival, police and cosmos — 2026-09-13

The owner's latest request extends the existing Riftbound planetary game with building
destruction, physical debris, police vehicles and armed officers, jail, hunting/crafting,
better NPC walking/eyes, atmosphere presentation and black-hole encounters. Version 0.9
implements these as a bounded extension of Voyager's existing authority and presentation.
It does not reorder or claim completion of the standalone Living City traffic/disaster phases.

The server owns building integrity and broken-glass flags. Saved damage drives which
procedural building levels remain; native Chaos rigid bodies provide at most 96 temporary
debris pieces with radial gravity. Physics does not feed the pure city simulation or decide
which persistent floors survive. This avoids a new paid dependency, while leaving the
full pure structural support solver and Geometry Collection fracture for future work.

Existing per-player wanted cases gain street-level hover cruisers, skinned officers,
armed civic guards and nearby patrol ships. Authority validates observation, weapon traces,
surrender and timed custody. The host's remaining sentence saves with the expedition;
joining-player cases remain session state. Shared hyperspace cannot carry a detained crew
member out of custody. Healthy surrender does not trigger paid medical recovery.

Field supplies use bounded replicated stacks and atomic server-side recipes. Hunting grants
each eligible carcass once after range, visibility and capacity checks; food and medicine
use the shared pure city health/needs path. The expedition wrapper now saves those item
stacks and building deltas alongside city archives, with caches for service/controller
teardown. Old saves keep their progress and receive starting energy cells.

Black-hole geometry and distant orbital cloud materials are original implementations.
Volume weather uses `MI_CosmosWeather`, a project instance of the installed Unreal cloud
material, alongside native atmosphere/cloud components. Engine content is credited
separately in `Art/Cosmos/PROVENANCE.md`. The anomaly's dark horizon, disk, halo and hazard are
gameplay/visual approximations, not general-relativistic ray tracing. No paid Fab assets
or plugins are purchased or redistributed for this pass. Existing CC0 character and nature
credits remain applicable. Current evidence and explicit exclusions live in
`docs/phases/VOYAGER_FRONTIERS.md`; prior release results do not validate the new features.


## D-032 — Remove Voyager custody; armed drones and original hover vehicles — 2026-09-13

The owner explicitly removed jail. Version 0.9.1 deletes cells, arrest transitions and
surrender input; old save fields and no-op law APIs remain for deserialization/call-site
compatibility. Restoring an old sentence never moves or restricts the player. Every new
save writes zero sentence fields, including controller teardown. Normal medical recovery
and citation balances remain; no replacement detention mechanic is introduced.

The existing authoritative police actor now also represents Sentinel drones. Two or more
stars dispatch up to three per suspect near the surface; movement sweeps avoid solid
geometry and weapon traces apply damage only to the assigned target. Peer clients render
replicated motion and health without applying damage locally. Fallen drones expire and
resolved cases retire their response actors. JSON controls shot damage and timing.

Original Vesper cruiser and Sentinel meshes replace primitive vehicle presentation. Both
use three authored LODs and existing project PBR material parents. Their source meshes,
reproducible import script and provenance ship with the public project. No marketplace
purchase is needed; this is an art and pursuit upgrade, not civilian traffic or a playable
car interior. Validation lives in `docs/phases/VOYAGER_SECURITY.md`.
