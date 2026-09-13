# Voyager security — version 0.9.1

Requested 2026-09-13: remove jail, add drones that attack wanted criminals, and replace
the police vehicle with a more believable hovering car. This supersedes 0.9 custody.

## Implemented

- No arrests, cells, sentence timer or surrender control. Legacy save fields deserialize
  for compatibility, but sentences are ignored and cleared when saving. Ordinary medical
  recovery replaces incapacitation arrest. Citations and wanted pursuit remain.
- Two or more stars dispatch Sentinel drones near the surface. Response scales from one
  to three per suspect, with bounded replacement timing. Each drone follows last observed
  positions, flies with swept obstacle avoidance, and fires only after an actual clear hit
  trace against its assigned criminal. Walls block vision; glass permits vision but stops
  shots. Drones take weapon damage, fall when disabled, and retire when pursuit resolves.
- Original Vesper hover sedan: shaped body panels, glazed cabin, door seams, mirrors,
  headlights, red/blue emergency lights and four lift pods. Existing road pursuit and
  radial terrain following drive it; visual hover motion is interpolated on every peer.
- Both models have three imported LODs. Vehicle geometry and materials are project work;
  no additional plugin or paid Fab asset is required. See `Art/Security/PROVENANCE.md`.

## Validation

Completed on 2026-09-13 using Unreal 5.8.2. The final Windows BuildCookRun succeeded in
51.55 seconds. A previous link attempt encountered the editor DLL held by the flight test;
the retry succeeded after that test exited. Local evidence under `Saved` is excluded from Git.

- Packaged multiplayer police with rendered host: **22/22**, 32.01 seconds,
  `VoyagerPolicePackagedNetworkRenderedReport.json`. The final screenshot was visually
  inspected for the car silhouette, lift pods, lights, drone and absence of the jail HUD.
- Packaged survival/field care: **25/25**, 46.37 seconds,
  `VoyagerSurvivalPackagedHeadlessReport.json`.
- Packaged city economy and persistence: **41/41**, 137.07 seconds,
  `VoyagerCityLifePackagedSoloHeadlessReport.json`, including real wages, city archive reload,
  NPC state and controller-teardown inventory preservation.
- Multiplayer flight regression: **24/24**, 129.22 seconds, `VoyagerNetworkReport.json`.
  This editor run preceded restoration of the unchanged autosave pump; the final packaged
  city and survival runs above validate saving after that correction.
- Asset import validated every LOD's triangle count and bounds;
  `VoyagerSecurityAssetsReport.json`. Source script compilation and PowerShell parsing pass.

All packaged runners preserved both normal expedition saves. A local pre-release backup
is retained at `Backups/SecurityRelease-20260913-174436`. Neither saves nor backups are
published. These short integration checks do not establish a new gameplay FPS guarantee;
earlier performance figures remain specific to their documented fixtures and hardware.

The police audit exercises real dispatch, attributed weapon damage, cover, drone death,
remote motion/death and authority rejection, independent innocent peers, old-save sentence
migration, and lethal recovery. Audits use isolated save slots and verify normal saves
remain unchanged. This release does not claim photorealistic art or drivable civilian cars.
