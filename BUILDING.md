# Building and playing

This repository contains two playable modes in one Unreal Engine 5.8 project, sharing a pure C++ simulation module:

- **Riftbound: Voyager**: spherical planets, ship flight, combat, wildlife, fully enterable city buildings, persistent residents, a planetary economy, wanted patrols and co-op. See [VOYAGER.md](VOYAGER.md).
- **Living City**: a deterministic simulation of 1,500 citizens, jobs, food shopping, a closed money loop and editable heightfield terrain. See [LIVINGCITY.md](LIVINGCITY.md).

## Get the source

Install Git with Git LFS, then clone the repository and run `git lfs pull` in its directory.
Textures, sounds, materials and maps use LFS; a ZIP source download may contain pointers instead of assets.
The repository includes original project assets and source. Unreal Engine and compiled Windows packages are separate local installations/builds.

## Run the simulation without Unreal

On Windows, install Visual Studio Build Tools with the Desktop development with C++ workload and a Windows SDK. Run:

```powershell
.\Scripts\Run-CI.ps1
```

This builds the C++20 core, checks module isolation, runs its invariant tests, compares separate deterministic runs, verifies record/replay and measures throughput. GitHub Actions runs this same gate. `LC_VCVARS64` can specify a custom `vcvars64.bat` path.

## Build the playable game

Install Unreal Engine 5.8 (developed with 5.8.2), the Visual Studio C++ game development tools and Windows SDK. The build scripts default to `C:\Program Files\Epic Games\UE_5.8`; adjust the engine path in the script if your installation differs. Living City's launcher also accepts `UE_ROOT`.

```powershell
.\Scripts\Build.ps1
.\Scripts\Package.ps1
```

Close the Unreal Editor before building. Packaging compiles all runtime modules and cooks both Voyager's Forest map and Living City's Entry map. It writes the Windows build under `Packaged/Riftbound/Windows`.

Voyager now links `LivingCitySim` for its planetary economy. It does not start or depend on the separate `LivingCityGame` presentation. When adding a pure core source file, run `Scripts/Build-Sim.ps1` before the Unreal build so its generated source aggregate is current. The usual CI gate checks the permitted one-way module boundary.

Planetary economy configuration is reviewable JSON at `Content/CityData/economy.json`. Packaging stages `CityData` as UFS text; it is not a binary Data Asset. Prices, initial funds, wage, daily rent/utilities, tax rates and the clock rate are read on city generation. `ticksPerMinute=20` means one in-game minute per real second with a fixed 20 Hz simulation. Population is currently fixed at 3,600 records per city in the adapter. Saves validate their configuration; changing economic settings is not a migration of an existing saved economy. Preserve your save before experimenting with configuration changes.

Natural foliage meshes, PBR textures and materials are included through Git LFS under `Content/Nature`. `Build.ps1` can regenerate them from the included `Art/Nature` sources; optional `python Scripts/bootstrap_voyager_nature.py --prepare` refreshes the documented CC0 downloads and original mesh sources. Runtime streaming uses the existing Procedural Mesh plugin and Unreal's instanced mesh LODs, Sky Atmosphere and Volumetric Cloud components. No paid foliage or bodycam plugin is required. Local NPC dialogue optionally uses Ollama with `llama3.2:3b`; missing Ollama leaves authored dialogue available.

Characters and ships are included under `Content/Characters` and `Content/Ships`; no paid marketplace character or ship plugin is required. Their documented sources are under `Art/Characters` and `Art/Ships`. `Build.ps1` also imports their meshes/materials and the new window material. Character source regeneration needs NumPy/SciPy; importing the included generated sources uses Unreal's bundled Python and Interchange pipeline. Runtime law enforcement, animation, stair collision and the planetary economy run locally without Ollama or external services. The city integration requires no additional Unreal plugin installation.

Double-click **Play Voyager City.cmd** to visit a planetary city, **Play Voyager.cmd** for a normal expedition, or **Play Living City.cmd** for the separate simulation. The launchers prefer the local packaged game. Living City's `-Rebuild` option builds and launches through Unreal Editor instead.

## Validation

`Scripts/Test-LivingCity.ps1` checks the running Unreal client. Voyager has separate city, multiplayer and flight audit scripts in `Scripts/`; see its guide for commands. Unreal checks need the installed engine or an already built package and run locally; GitHub Actions validates the pure simulation, not the graphics client.

Version 0.8.0's integration gate and measured results are tracked in [VOYAGER_CITY_LIFE.md](docs/phases/VOYAGER_CITY_LIFE.md). Earlier successful Voyager or standalone Living City reports do not validate the new economy adapter. Report core tick time separately from rendered frame rate, and distinguish the 54,000 simulated records from the 72-character embodiment cap.

The final version 0.8.0 build passed 205 pure-core invariants, 55 editor multiplayer integration checks and 47 standalone rendered checks, including archive decoding/corruption rejection and cargo preservation during controller teardown. The UFS package includes the economy JSON. Network flight and rendered crime regressions each passed 24 checks. Normal expedition saves were preserved by the final integration audits.

## Publication and progress

The public repository starts with a clean source snapshot. Earlier development history remains local because it contained personal save backups and an Android development file-server token. Saves, backups, build outputs and tokens are excluded; Android File Server is disabled. Existing Voyager progress stays on the player's PC.

These are playable prototypes. The original Living City mode still has its separate heightfield and one-good economy. Voyager now has its own eight-good planetary core and city phone using the shared pure module. The larger phase plan remains a roadmap: Mass traffic and large visible crowds, vehicles/transit, mortgages, court/jail simulation, voxel destruction and economic disasters are not part of this integration.
