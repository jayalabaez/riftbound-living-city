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

## Validation status

This is a review candidate, not a qualified playable release. Final runtime qualification
is blocked by Windows application control. Do not replace the public release branch until
the final game binary passes the checks below.

Completed before the policy blocks:

- Pure simulation: 210/210 tests, including four new landing tests.
- Editor landing audit: nine checks on each of seeds 1, 42 and 9001. A subsequent fix makes
  non-default test seeds select their initial spawn consistently; these earlier checks
  still exercised their requested systems but did not certify the intended arrival planet.
- Four rendered scenes on system 42 / planet 4 with fixed view direction, hidden HUD and
  separate save slot. All images inspected. The recorded High tier used **87% internal
  resolution** at a 1920×1080 output, an engine default found by the new instrumentation.
  The final source forces 100%; that final performance measurement remains blocked.
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

Outstanding qualification:

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
