# Building and playing

This repository contains two separate prototypes in one Unreal Engine 5.8 project:

- **Riftbound: Voyager**: spherical planets, ship flight, combat, wildlife, enterable city ground floors and co-op. See [VOYAGER.md](VOYAGER.md).
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

Natural foliage meshes, PBR textures and materials are included through Git LFS under `Content/Nature`. `Build.ps1` can regenerate them from the included `Art/Nature` sources; optional `python Scripts/bootstrap_voyager_nature.py --prepare` refreshes the documented CC0 downloads and original mesh sources. Runtime streaming uses the existing Procedural Mesh plugin and Unreal's instanced mesh LODs, Sky Atmosphere and Volumetric Cloud components. No paid foliage or bodycam plugin is required. Local NPC dialogue optionally uses Ollama with `llama3.2:3b`; missing Ollama leaves authored dialogue available.

Double-click **Play Voyager City.cmd** to visit a planetary city, **Play Voyager.cmd** for a normal expedition, or **Play Living City.cmd** for the separate simulation. The launchers prefer the local packaged game. Living City's `-Rebuild` option builds and launches through Unreal Editor instead.

## Validation

`Scripts/Test-LivingCity.ps1` checks the running Unreal client. Voyager has separate city, multiplayer and flight audit scripts in `Scripts/`; see its guide for commands. Unreal checks need the installed engine or an already built package and run locally; GitHub Actions validates the pure simulation, not the graphics client.

## Publication and progress

The public repository starts with a clean source snapshot. Earlier development history remains local because it contained personal save backups and an Android development file-server token. Saves, backups, build outputs and tokens are excluded; Android File Server is disabled. Existing Voyager progress stays on the player's PC.

These are playable prototypes. Living City's larger phase plan remains a roadmap: its current terrain is a heightfield, its economy has one good, and its player does not yet participate as an employed citizen. It is a separate mode, not a planetary economy plugged into Voyager.
