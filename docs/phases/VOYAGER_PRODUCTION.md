# Voyager production 0.9.2 — stability and reference measurements

The master brief's first delivery is [the world audit](../VOYAGER_WORLD_AUDIT.md).
This is a bounded foundation milestone. The complete reference-planet standard and
phases B–K remain open; existing procedural art is not being relabelled photorealistic.

## Implemented

- Pure, allocation-free landing suitability rules consume nine quantized support heights,
  normal alignment, altitude, speed and a host-supplied obstruction result. Heights use
  an int64 range calculation; invalid inputs fail closed. Limits live in
  `Content/CityData/flight_safety.json`.
- Authority checks the full 7.4 × 7.9 m scout footprint and descent sweep. Unsafe landings
  retain the original position and possession. Successful landing resets flight prediction
  through the existing epoch mechanism. A landed ship is not snapped again when exiting.
- Eight bounded exit candidates require a real collision floor, acceptable slope/drop,
  an empty standing capsule and an unobstructed swept route. Spawning fails rather than
  forcing a character into collision. Roof landing and tall landing platforms are not
  supported by this terrain-based rule.
- Shared DLL exports apply only to modular targets, fixing monolithic import/link warnings
  at their cause. The simulation module retains its dependency boundary.
- Native Low/Medium/High controls in the Escape menu retain full display resolution and
  save their selection locally. Texture pools are 1024/2048/4096 MiB, limited by VRAM.
  Spherical aerial perspective remains enabled on every tier. Rendering choices do not
  change deterministic city state, terrain collision, obstacle IDs or population limits.
- Bounded histogram exposure adapts between interiors and daylight. It uses 0.08–1.25
  luminance limits and 3/1 stops-per-second bright/dark adaptation. This does not make
  the existing low-intensity sun, artificial fill, static weather or lighting physically
  calibrated. Those remaining limitations stay visible in the audit.
- A reproducible four-scene benchmark reports wall-frame percentiles, game/render thread
  work, native RHI GPU busy samples, process residency, texture allocations, draw calls,
  actor/component counts and the last city-worker tick. GPU allocation totals are not
  inferred from texture allocations. Camera direction and active settings are recorded.

## Reproduce

Build the editor, then run:

```powershell
.\Scripts\Test-VoyagerProduction.ps1
.\Scripts\Test-VoyagerProduction.ps1 -Benchmark -Quality 2
.\Scripts\Test-Voyager.ps1 -Network
.\Scripts\Test-VoyagerRealism.ps1 -Render
```

The production runner accepts `-Packaged` when Windows permits the built executable.
Safety runs seeds 1, 42 and 9001 sequentially using an isolated save slot. Benchmarks use
system 42, planet 4 (Ilyra f), 1920×1080, a 60 FPS cap, 8 seconds settling and 10 seconds
sampling per scene. They block mouse/movement input and hide the HUD for consistent views.
Timing excludes transitions/settling, so it is not a travel-hitch or long-session soak test.

Normal editor and packaged expedition saves are hashed before and after the new audits.
Save backups are retained locally outside Git. The original network runner remains a
two-process regression, not a four-player scalability qualification.

## Initial validation and temporary execution block

The first candidate could not complete runtime qualification because Windows application
control refused its binaries. This section preserves that initial evidence. Later rebuilt
binaries were permitted and exercised; see the current qualification update below.

Completed before the policy blocks:

- Pure simulation: 210/210 tests, including four new landing tests.
- Editor landing audit: nine checks on each of seeds 1, 42 and 9001. A subsequent fix makes
  non-default test seeds select their initial spawn consistently; these earlier checks
  still exercised their requested systems but did not certify the intended arrival planet.
- Four rendered scenes on system 42 / planet 4 with fixed view direction, hidden HUD and
  separate save slot. All images inspected. The recorded High tier used **87% internal
  resolution** at a 1920×1080 output, an engine default found by the new instrumentation.
  The final source forces 100%; its measurement was blocked at that initial checkpoint.
- Preview scene means: 16.667/16.667/16.670/16.667 ms wall frame; GPU busy
  4.647/3.588/3.725/7.486 ms (wilderness/city/interior/orbit). Worst measured wall frame
  19.654 ms. Process residency 3009.7–3114.2 MiB. Last worker samples 0.042–0.072 ms.
  These capped, settled measurements exclude streaming transitions and are not an FPS
  improvement claim. [Raw measurements](../benchmarks/production-preview-20260913.json).

Final source checks completed despite the runtime block: editor and game targets compile
without compiler/linker warnings; the complete quick CI gate passes in 40.4 s. It includes
210 tests, purity checks, separate-process 20,000-tick determinism and record/replay
verification (40 checkpoints, no divergence). This does not replace blocked Unreal tests.

The first packaged build compiled and cooked with zero compiler/linker/cook warnings.
Windows Smart App Control then refused its executable (Code Integrity event 3077).
It subsequently refused the rebuilt editor DLL too (`GetLastError=4551`, event 3077).
Both local signing stores contain no code-signing certificate. No security policy was
changed. A successful build is not proof of a successful packaged run. Microsoft's
[signing guidance](https://learn.microsoft.com/en-us/windows/apps/develop/smart-app-control/code-signing-for-smart-app-control)
requires a trusted signing provider; a self-signed certificate is not an equivalent fix.

Outstanding at that initial checkpoint:

- Re-run the three-seed safety sequence with actual collision-floor enforcement.
- Re-run network flight: the earlier client reached the second planet but its one-shot
  landing request did not transfer possession. The fixture now waits at less than 1 m/s
  for 0.75 s before asking the authority to land, and authority refusals are logged.
  This is an unverified timing hypothesis until the regression passes; no safety threshold
  was relaxed and no test result was forced to pass.
- Re-run nature/atmosphere validation. Its old coefficient check failed because it measured
  the optical ground 3 km below terrain; the corrected check compares density at terrain
  height against the same original bounds. It is not yet a passed result.
- Measure Low/Medium/High at 100% resolution and repeat real rendered ascent/descent.
- Re-cook, package and launch the final revision after trusted execution is available.

## Observations driving the next art pass

The inspected reference captures still show sparse conifer crowns, repeated ground cover,
oversized emissive resource crystals, simple furniture and repeated city facades. Orbit
shows an atmospheric limb and clouds, but low-resolution cloud edges need improvement.
The master brief's climate, hydrology, weather, regional ecology, traffic, architecture
and stronger character-animation phases remain separate, measurable work.

## Furniture, supplies and structural physics follow-up

The owner specifically prioritized misoriented chairs/computers, usable item models and
real Chaos physics. This extends the review candidate without declaring phases B–K done.

- Shared furniture assemblies derive orientation from each seated user and table. Chair
  backrests stay behind the sitter; opaque monitor housings have a single emissive face.
  Keyboards/mice rest on the tabletop. Ground and upper-floor desks share the same rule,
  whole assemblies preserve stair clearance, and instancing/building IDs remain intact.
- Seven original supply models use at most one component per held item, three material
  sections and 1,408 triangles. Q cycles owned stacks; LMB uses existing food, care or
  charge transactions. V clears the supply. Authority verifies the expected selection
  and quantity, and depletion clears the visual. Models have no collision or Tick.
- Structural decisions now use a pure integer rule with up to 18 floors and four support
  regions each. With default tuning, two failed regions collapse that floor and all above.
  Aggregate damage thresholds remain compatible. This adds local failure, not a detailed
  beam/column or room fracture simulation. `destruction.json` contains reviewed limits.
- Glass/concrete Chaos debris uses actual density-based mass, surface response, damping
  and mass-dependent momentum. The existing 96-body pool recycles instead of suppressing
  new impacts at capacity. Maximum lifetime stays 45 seconds. Replicated generation changes
  reset reused poses; clients do not simulate gameplay rubble collision.
- Schema 5 adds sparse support records and a collapse ceiling. Old empty-support saves
  retain integrity/glass behavior; invalid support lengths are rejected. Fully collapsed
  buildings refuse city services, and explosions above ruins test surviving geometry.

Run `Scripts/Test-VoyagerCity.ps1 -Render -Seed 42` for traversed interiors and generated
furniture checks; repeat seeds 1 and 9001. The engine regression
`Riftbound.Voyager.Furniture.FacingSupportAndRadialFrames` also tests intentionally reversed
chairs/screens and unsupported desktop objects. `Scripts/Test-VoyagerSurvival.ps1 -Render`
adds seven-model validation, component bounds/lifetime, real equipment cycling, stale-use
rejection and depletion, with a held-medkit capture. `Scripts/Test-VoyagerDestruction.ps1
-Network -Render` adds support collision, material/mass/reuse, disk restore, legacy migration
and service/blast regressions. Normal expedition saves remain isolated from these fixtures.

The pure quick gate passes all 216 tests, architecture purity, cross-process determinism
and record/replay checks in 36.3 seconds. No performance or visual-realism claim is inferred
from the earlier preview captures.

## Current qualification update — 2026-09-13

Windows permitted later rebuilt editor and packaged game binaries. No security settings
were changed. The editor compiled without warnings (35.78 s, then 10.92 s for the final
item/police changes); the game compiled in 37.29 s. Build/cook/stage completed in 57.49 s,
with zero cook errors/warnings, and the actual packaged executable passed nine landing
safety checks on seed 1. Editor safety passed all nine checks on each of 1, 42 and 9001.

The previously incomplete two-player flight test now passes all 24 checks in 128.9 s:
ascent, continuous cruise, descent, safe landing, disembark, second-planet walking and
next-system replication. The corrected nature audit passes 11 gameplay checks including
terrain-height scattering coefficients, radial streaming and deterministic reload.

Native furniture and item-winding automation tests both passed with explicit engine queue
completion and exit code 0. Rendered survival passes all 37 gameplay checks and all 43
report checks. Screenshot inspection caught a backface convention error in the first item
model implementation; it was fixed against Unreal's own box generator, then re-rendered.
The final medkit has a closed lid, visible medical marking and correctly culled exterior.

Editor destruction passes 33 host and 11 peer checks in both headless and rendered runs.
The peer observed 53 actual recycled pose updates and verified support deltas, removed
collision and client authority rejection. Invalid-save fixtures produce the expected
explicit rejection warning. Repeated police assignment no longer recreates its components;
the former resource-cleanup overwrite warnings are absent. A final audit-only change delays
the rubble capture until before synthetic pool stress injection. The final cooked package
then passed the same 33 host/11 peer checks in 57.819 s; all four captures were inspected.
The final package also passed all 37 survival gameplay checks (40 report checks) in 44.70 s.
Its build/cook/stage took 33.39 s with zero cook warnings/errors. Normal expedition save
hashes were unchanged. Destruction configuration was present in the staged UFS manifest,
and no settings fallback warning appeared.

The rendered city test on seed 42 passed traversal and 1,279 furniture assemblies across
two settlements. Headless city seeds 1 and 9001 also passed, in 119.77 and 118.16 s, with
normal saves unchanged. Each sequence includes six building roles, stairs, a remote city,
citizen routines and generated furniture checks. The furniture uses procedural primitives;
neither chair sitting nor interactive computer software is implemented.

Across all three city seeds, 3,919 assemblies and 52,864 individual furniture parts passed
facing, support and circulation checks in six settlements. The workshop and clinic captures
also show supported, aisle-facing computer/diagnostic screens; automated transform checks
cover desktop placement more precisely than the distant screenshots.

Final standalone benchmarks ran sequentially with no concurrent Unreal test or build,
at 1920x1080 and 100% internal resolution on the RX 7900 XT. All four scene means stayed
at approximately 16.667 ms, the 60 FPS cap, in every preset. Mean GPU busy ranges were
2.87-5.80 ms on Low, 3.49-8.18 ms on Medium and 3.78-9.13 ms on High. The worst sampled
wall frame across all twelve scenes was 18.64 ms. High process residency was
1,334.46-1,453.57 MiB; scene-end actor counts ranged from 31 in orbit to 168 indoors.
These settled 10-second samples exclude streaming transitions and do not establish a
mid-range hardware target or a four-player budget. All twelve captures were inspected;
remaining issues include repeated vegetation/facades, oversized resource crystals,
dark interior ceilings and coarse orbital/cloud edges. Raw measurements:
[Low](../benchmarks/production-quality-tier0-20260913.json),
[Medium](../benchmarks/production-quality-tier1-20260913.json),
[High](../benchmarks/production-quality-tier2-20260913.json).

The final packaged rendered ascent passed walking, mining, boarding, seamless ascent and
orbit checks in 21.86 s. Three fresh captures at the surface, 15.03 km and 65.04 km were
inspected: the cloud deck, darkening upper atmosphere and curved blue limb remain coherent.
Both normal expedition save hashes were unchanged. Capture/streaming frames in this run
reached 247.34 ms, so this is continuity evidence, not a claim of hitch-free 60 FPS flight.

Inspected examples: [cafe](../benchmarks/quality-cafe-20260913.png),
[workstation](../benchmarks/quality-workstation-20260913.png),
[held medkit](../benchmarks/quality-medkit-20260913.png),
[packaged destruction](../benchmarks/quality-destruction-20260913.png).
Structural loss does not yet relocate businesses, rehouse citizens or simulate rubble
hazards; debris remains presentation-only. Those consequences need a separate core slice.
This is still a prototype: four-player stress qualification, mid-range hardware validation,
long-session streaming budgets and the reference planet's broader art/simulation work remain.
