# Voyager City Life integration

Current milestone: version **0.8.0**, requested 2026-09-13. Add a playable city economy and
daily life to Riftbound's existing planets. This supersedes the old alongside-only boundary;
the separate Living City mode remains available. See CLAUDE section 9 and D-030.

**Status: release ready.** The bounded integration below passes its pure-core, editor
multiplayer and standalone runtime checks. Larger original roadmap targets remain outside
this milestone.

## Implemented source scope

- Pure C++ `PlanetaryCity` core, shared by the standalone MSVC tests and Unreal module.
  `Scripts/Build-Sim.ps1` invokes `cl.exe`; no CMake project is required or present.
- Fifteen planetary cities with 3,600 persistent residents each: 54,000 simulated records.
- Existing 36 embodied residents per nearby city, 72 globally; utility goals drive actual
  street and doorway routes. Homes, jobs, names, balances, needs and deaths persist in core rows.
- Eight goods: food, water, fuel, raw materials, building materials, consumer goods,
  electronics and medicine. Shop inventory changes tax-inclusive prices.
- Nine needs: hunger, thirst, hygiene, rest, health, energy, social, safety and comfort.
- Closed transfers for wages, taxes, purchases, rent, utility bills, fines and recycling.
  Mining cargo can be exchanged for city credits at a shop.
- Player resident accounts use the same core actions as NPCs. Commands require valid
  inventory, funds and physical presence where appropriate.
- City phone: **P** opens; **Left/Right** changes pages; **1–8** selects the displayed action;
  **P**, **Escape** or **Backspace** closes. The world continues running while it is open.
- **N** cycles home, workplace, nearest market, Civic Security and off. On foot near a city,
  the guide points to the real entrance, shows distance and confirms arrival. Work & Home
  action **8** also cycles it; there is no automatic walking or teleportation.
- Factual dialogue from each resident's actual home, workplace, shift, money and needs.
  Optional Ollama remains non-authoritative and has an immediate authored fallback.
- Citation balances and records connect to Voyager's existing wanted patrol system.
- Text tunables in `Content/CityData/economy.json`, staged into the package as UFS text.
- Autosave requests coalesce; matched city/cargo snapshots are encoded and written in
  background jobs in order. Explicit saves and shutdown wait for completion. Core cloning
  and Unreal save-object serialization still use the game thread.
- The latest save wrapper compresses city deltas into additive `PackedState` bulk strings;
  legacy `State` byte-array archives remain supported. Production decoding, corruption
  rejection and state round trips pass in the final editor audit.

## Verification checklist

The pure gate is `Scripts/Run-CI.ps1`. The runtime integration command is
`Scripts/Test-VoyagerCityLife.ps1`; use `-Network` for real host/client accounts,
`-Render` for the five phone-page captures, and `-Packaged` for the standalone build.
Reports are named `Saved/VoyagerCityLife<Editor|Packaged><Solo|Network><Rendered|Headless>Report.json`.
The runtime script uses `Voyager-Automation-CityLife`, hashes normal expedition saves,
and stops only its own processes. A complete wage hour takes 60 real seconds with the
shipped clock configuration.

The following checks have completed. Reports describe their actual coverage; core transfer
tests and network flight tests are distinct from an end-to-end resident commute measurement.

- [x] Pure core builds and passes **205/205 invariant tests**, including 11 planetary tests.
- [x] Full `Run-CI.ps1` gate passes: 205 invariants, architecture/purity checks, separate-process
  100,000-tick determinism and a 604,000-tick replay, in approximately 44 seconds.
- [x] Final editor and packaged runtime verification pass after packed archive and teardown changes.
- [x] Planetary conservation, needs, travel/promotion, damage/death, evidence and save
  round-trip invariants pass, including invalid-command and corrupted-save cases.
- [x] Profile 54,000 records at 20 Hz in the pure core and final standalone audit.
- [x] Record final performance from the standalone packed-save build running alone.
- [x] Unreal editor target builds and the city economy runs in a real host/client session.
- [x] Standalone build/cook/package succeeds in **73.16 seconds**; the UFS manifest includes `CityData/economy.json`.
- [x] Final standalone rendered runtime passes **47/47 checks** in **136.812 seconds**.
- [x] In-game phone performs purchases/consumption, a complete paid hour, mineral trade and fines.
- [x] Home washing/rest, rental transfer, bill-action acknowledgement and N/phone entrance
  guidance pass in the extended editor audit. The short run does not cross a billing day.
- [x] Visible NPC home/work assignments match core state; fatal damage and saved death/home/work round trips pass.
- [x] Player wallet, home and fines survive production save decoding and core reload.
- [x] Co-op clients receive distinct accounts; host trade leaves the peer account unchanged.
- [x] Network movement, planet ascent/cruise/descent and system travel pass **24/24 checks**.
- [x] Final packed-save decode/corruption, NPC death reload and controller-teardown cargo checks pass.
- [x] Prior editor crime regression passes **24/24 checks**, including rendered evidence.
- [x] All five rendered phone pages are readable; modal input and input restoration pass.
- [x] Both normal expedition saves retain their SHA256 hashes during the integration audit.

## Recorded results — 2026-09-13

`Saved/VoyagerCityLifePackagedSoloRenderedReport.json` passed **47/47 checks** in
**136.812 seconds**, including startup; the runtime portion took **116.19 seconds**.
The final standalone build ran alone with all 15 cities and 54,000 records. It verified
phone/navigation input, location-validated purchases and work, home services, citations,
actual NPC home/work assignments and fatal damage, production decoding of every saved
city, invalid packed-data rejection, player wallet/home/fine and NPC death round trips,
and cargo preservation after controller destruction. Six captures cover the five phone
pages and entrance guidance. Both normal expedition saves retained their hashes; no
economy-data or compression fallback occurred.

`Saved/VoyagerCityLifeEditorNetworkRenderedReport.json` passed **55/55 checks** in
**137.428 seconds**, including startup. The runtime portion completed in **117.18 seconds**
with a separate real co-op client. It verified 15 cities and 54,000 resident records,
transactions, a full paid hour, home services, observed citations, owner-specific snapshots,
N/phone navigation, visible NPC identity/death synchronization, production decoding of all
15 city archives, corrupted packed-data rejection, player/NPC state round trips and cargo
preservation after controller destruction. Five phone pages and an entrance-guide capture
were produced at 1280×720. Both normal expedition saves retained their hashes.

The standalone audit averaged **59.86 FPS** across **6,955 frames**, with an **89.665 ms
worst frame** and **0.329 ms** largest observed simulation-worker sample. Its packed
fifteen-city archive payload occupied **2,432,688 bytes**. Frame measurements include
captures, fixture relocation, streaming and saving after the first eight startup seconds;
they are not a guarantee of sustained gameplay FPS or the roadmap's larger visible crowds.

Across 15 standalone saves, game-thread cloning took **0.386–1.915 ms** (mean **1.422 ms**),
Unreal save serialization took **1.940–4.122 ms** (mean **2.982 ms**), and background city
encoding took **78.014–94.551 ms** (mean **85.142 ms**). The pure-core 54,000-resident test
measured approximately **0.03 ms combined mean tick time**, with zero steady-state tick
allocations. The complete paid-hour test credited **1,620 minor units** after income tax.

`Saved/VoyagerNetworkReport.json` passed **24/24 network flight checks** in **133.63 seconds**,
including real host/client boarding, continuous ascent/cruise/descent and a new star system.

`Saved/VoyagerCrimeEditorSoloRenderedReport.json` passed **24/24 checks** in **97.82 seconds**.
It covered sidearm hits, killable residents, wanted escalation, patrol weapons and sensors,
opaque cover, last-known-position searches, corpse retirement, recovery and patrol
destruction. Five captures were produced, and normal expedition saves were unchanged.
This run preceded the final navigation and asynchronous save changes.

The final editor rebuild completed in **5.49 seconds**. Standalone BuildCookRun succeeded
in **73.16 seconds**, with 8 package files updated and 40 unchanged. Its
`Manifest_UFSFiles_Win64.txt` includes `Riftbound/Content/CityData/economy.json` at line 1728.
The packed representation uses zlib's speed-oriented option without changing its format.

Coverage limits: pure tests verify resident transfer identity and money conservation;
network flight verifies physical interplanetary/system travel. The integration audits
verify visible NPC home/work identities and saved deaths, not a measured full-day NPC
commute. Their player fixtures use actual buildings but relocate the test character to
reach each service. Bill-action acknowledgement is covered; this short runtime does not
cross a billing day. No claim of a full housing market, court process or traffic simulation
follows from these checks.

## Limits of this milestone

The simulated population is not a visible crowd count. The game does not yet render
5,000–15,000 Mass agents or embody 400–800 characters. It uses the existing bounded skeletal
population and the new persistent records behind it.

Housing means a real local address and rent, not a real-estate ownership market. Relocation
keeps resident possessions and state while assigning valid local homes/jobs. Fines and
records are playable; trials, prison sentences, bail and a full evidence investigation UI
are not. There are no new road vehicles, transit schedules, voxel excavation, structural
collapse, flooding or economic disasters in this integration. The original Living City
mode's editable heightfield remains separate from Voyager's planet terrain.

Unreal 5.8.2's installed native components and existing project assets provide presentation.
No paid plugin or cloud service is required. Larger original phase targets stay in the
roadmap and require their own implementation, profiling and validation.
