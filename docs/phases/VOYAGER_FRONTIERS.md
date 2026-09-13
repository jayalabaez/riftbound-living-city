# Voyager Frontiers — version 0.9.0

Requested 2026-09-13: extend Riftbound's existing spherical worlds with destruction,
physics, police vehicles/officers and jail, animal food and crafting, improved NPC motion
and eyes, natural atmosphere presentation, and other-system/black-hole encounters.

**Status: version 0.9 implementation and local Windows validation complete.** Current
packaged evidence is recorded below; version 0.8 results remain historical evidence.

## Implemented source scope

- Seven bounded field stacks: raw meat, cooked meat, hide, bone, medkits, demolition
  charges and energy cells. Authority validates entire recipes and rejects invalid input,
  absent ingredients and full outputs before changing inventory.
- Animal damage/death and one harvest per carcass, with distance, sight and capacity
  validation. Loot persists in the host inventory; individual animal deaths are session state.
- Raw food risks health; cooked meals and medkits affect the shared city needs/health core.
  Cooking requires a landed ship, cafe or residence. Equipment actions require a living,
  free character on foot.
- Sidearm ammunition and a 24-shot magazine with a 1.4-second reload. Field recipes make
  replacement cells and demolition charges.
- Building integrity and broken-glass floor flags, persistent across system streaming and
  host saves. Upper floors disappear as integrity drops; fully damaged buildings collapse.
  A maximum of 96 temporary debris actors use native Chaos rigid bodies with radial gravity.
  Persistent structure is decided before debris simulation and does not depend on its results.
- Ground response adds an armored hover cruiser and up to four skinned officers per suspect.
  Existing civic guards can aim and fire. Patrol ships descend near surface suspects.
  Four-second warning, firing at two or more stars, per-shot line of sight, and independent cases.
- G surrender under observation within 250 m; incapacitation can also lead to arrest.
  Healthy surrender preserves health. Civic holding cells release after 20 seconds plus
  eight per star. Host custody saves and resumes; citations and records remain. Boarding,
  equipment and shared system jumps are blocked during custody. Other players retain
  independent cases and ordinary in-system movement.
- Corrected citizen eye placement and pupil presentation, walk/run foot targets, pelvis
  motion and a guard aiming clip. Dispatched officers share the same imported guard assets.
- Retuned native atmosphere/cloud presentation, a project instance of the installed engine
  weather material, original distant orbital cloud materials and seeded black-hole geometry,
  accretion disk and halo. B surveys from orbit via the existing continuous cruise system;
  closer approaches have a bounded tidal hazard and warning. H still changes the shared
  star system through the existing brief transition.
- Save version 4 adds field items, building deltas and host custody. Item stacks clamp to
  0–999; legacy saves gain 120 energy cells. Teardown caches retain these additions when
  controllers or services retire during final saving.

## Controls

- **I** backpack; **1–8** select the displayed recipe/action; **I/Escape/Backspace** close.
- **V** sidearm; **left mouse** fires; **R** reloads; **E** harvests a nearby carcass.
- **G** surrender; custody countdown appears on the HUD.
- **B** anomaly survey while in orbit; **Tab + J** returns to a selected planet;
  **H** jumps to the next star when all crew members are free.
- Existing **P** city phone, **N** entrance guide, **C** camera treatment and **F5** save remain.

## Verification

The version 0.9 Windows build was compiled, cooked and staged on 2026-09-13 with
Unreal Engine 5.8.2. Initial BuildCookRun took 86.59 seconds; the final audit-camera-only
rebuild took 59.84 seconds. Both completed successfully. Local runtime reports are under
`Saved` and are intentionally excluded from source control.

The packaged runtime results below all passed and preserved both normal expedition saves:

- **Survival / field care: 25/25 checks**, 46.28 seconds. `Saved/VoyagerSurvivalPackagedHeadlessReport.json`.
- **Hunting / multiplayer: 22/22 checks**, 33.34 seconds. `Saved/VoyagerHuntPackagedNetworkHeadlessReport.json`.
- **Hunting / rendered: 17/17 checks**, 36.87 seconds. `Saved/VoyagerHuntPackagedSoloRenderedReport.json`.
- **Destruction / multiplayer: 31/31 checks**, 40.48 seconds. `Saved/VoyagerDestructionPackagedNetworkHeadlessReport.json`.
- **Cosmos / rendered: 18/18 checks**, 64.12 seconds. `Saved/VoyagerCosmosPackagedRenderedReport.json`.
- **Police / multiplayer rendered: 35/35 checks**, 105.66 seconds. `Saved/VoyagerPolicePackagedNetworkRenderedReport.json`.
- **City economy / rendered: 47/47 checks**, 129.20 seconds. `Saved/VoyagerCityLifePackagedSoloRenderedReport.json`.

The standalone city run used D3D12 at 1280 x 720 on this PC's Radeon RX 7900 XT, with
no other rendered game or build running. It measured **59.86 average FPS**, a
**91.556 ms worst frame**, and **0.244 ms peak city-worker time** across 54,000
resident records. The run was capped at 60 FPS; these figures describe this fixture and
hardware, not a performance guarantee for every resolution, city or combat encounter.

Additional current integration evidence:

- The pure simulation gate passes 206 invariant tests, same/different-seed selfchecks,
  separate-process determinism and record/replay. The same gate runs in the pre-commit hook.
- `Saved/VoyagerNetworkReport.json`: 24/24 editor host/client flight checks in 125.01 seconds,
  including continuous ascent, interplanetary cruise, descent, second-planet walking and
  a shared next-star transition.
- `Saved/VoyagerSurvivalEditorRenderedReport.json`: 27/27 checks in 46.05 seconds. This
  includes immediate healing, ordered damage/care, the lethal-hit boundary, and rejected
  dead/jailed care without item loss. Accepted care updates the authority pawn immediately;
  the worker snapshot acknowledges the ordered commands before replacing that projection.
- `Saved/VoyagerDestructionEditorNetworkRenderedReport.json`: 32/32 checks in 42.858 seconds.
  `Saved/VoyagerDestructionEditorSoloRenderedReport.json`: 27/27 in 36.887 seconds, adding
  actual backpack placement, the three-second fuse and detonation.
- `Scripts/check_voyager_character_geometry.py` verifies all three humanoid variants:
  corrected eye shells and forward axes, planted-foot motion, and ground clearance through
  Walk, Run and Death frames. The original Aim clips and skeletal assets are cooked.

Rendered screenshots were inspected: white cloud banks over forest, curved orbital
terrain/oceans, the cloud deck during flight, the black-hole disk, citizen faces, carcass
harvesting, building collapse, the police officer/cruiser response and actual Civic cell bars.
The initial police screenshot timing exposed the test cover wall and a stale camera;
separate settle/capture/hold stages now require camera readiness before recording images.
Those initial images are superseded by the final packaged police report above.

The runners use isolated `Voyager-Automation-*` slots, hash both normal saves before and
after, and stop only their own processes. Save version 4 retains the existing expedition
while adding field stacks, building damage and host custody. A pre-release backup is kept
locally under `Backups/FrontierRelease-20260913-101110`; neither saves nor backups are
published. Source is released on the repository's `codex/public-release` branch.

## Limits and provenance

This is a bounded prototype extension. It does not add Geometry Collection fracture,
voxel excavation, a deterministic structural support solver, fire propagation, hydrology,
earthquakes, civilian traffic/transit, trials or a funded prison economy. Hover cruisers
use authority-driven motion; Chaos is used for building debris. Black-hole visuals are
an approximation, not general-relativistic ray tracing or astrophysical gravity.

No paid Fab plugin or asset was purchased, downloaded or redistributed for this pass.
Existing character/nature provenance remains in `Art/Characters/PROVENANCE.md` and
`Art/Nature/PROVENANCE.md`; the procedural ship and cosmos work is original. Ollama can
still vary validated dialogue, but it does not control combat, custody, recipes or damage.
The visible citizen and wildlife budgets remain distinct from 54,000 city simulation rows.

Native volume weather is credited separately: `MI_CosmosWeather` references the installed
`/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst` parent. The parent engine
assets are not copied into this repository or claimed as original/CC0 project art.
`Art/Cosmos/PROVENANCE.md` documents this distinction. `Build.ps1` runs the cosmos asset
pass, and `/Game/Cosmos` is always cooked with hard-reference dependencies.
