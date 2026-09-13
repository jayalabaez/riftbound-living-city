# RIFTBOUND: SURVIVAL

Four explorers. One broken reality.

A playable Unreal Engine 5.8.2 cooperative survival FPS prototype built from the forest/UFO/anomaly brief. All scenery is generated at runtime, surfaces are procedural materials, and the four sound assets were synthesized for this project.

## Play

Double-click **[Play Survival.cmd](Play%20Survival.cmd)**. It opens the original survival mode directly. If a standalone build is present, the launcher uses it; otherwise it runs the compiled game through the installed Unreal executable. **Play Riftbound.cmd** now launches the procedural space adventure described in the [Voyager guide](VOYAGER.md).

Explore the clearing, then shoot the hovering spacecraft. Survive three incursions: flying alien drones, soldiers who leave proximity mines, and prehistoric beasts that can smash timber cover. Clearing each wave restores health, ammunition, and pulse charges. Survive the third wave to win. Press Enter after victory or defeat to reset the expedition.

- **WASD** move; **mouse** look; **Space** jump; **Left Shift** sprint.
- **Left mouse** fire; **right mouse** aim; **R** reload. Rifle reserves are unlimited, but magazines hold 30 rounds.
- **G** fires a pulse charge toward the aimed point, up to 15 metres away; it detonates after a short fuse. Three charges per wave. No friendly fire.
- **Escape** opens/closes the menu. Continue, host a new run, connect to a host address, or quit.
- Shoot timber walls and crates to open escape routes. Shoot red mines from a safe distance.
- Health slowly regenerates after nine seconds without damage. Downed explorers recover after six seconds if the run continues; the entire squad being down ends the run.

## Co-op

Launch survival, then choose **Escape → Host a new co-op run**. Alternatively, run `Scripts/Play.ps1 -Game Survival -Mode Host` in PowerShell. Up to three friends can connect with **Join Co-op.cmd** or the menu's host-IP field; the server selects the game mode. **Host Co-op.cmd** now hosts Voyager. The default port is **UDP 7777**. On this computer, use `127.0.0.1`; on the same network, use the host computer's LAN IP.

Every player needs the same build. A standalone build's entire `Packaged/Riftbound/Windows` folder can be copied to another Windows computer. Public internet connections require a reachable host address and appropriate network routing/firewall settings. This prototype provides direct IP/LAN play; it does not include EOS/Steam matchmaking, friend invitations, NAT traversal, or account services.

## Edit and build

Open `Riftbound.uproject` in Unreal Engine 5.8.2. The saved Forest map is intentionally empty in edit mode. In survival mode, `ARiftWorld` builds the forest, lighting, camp, and cover at runtime. The project now defaults to Voyager; **Play Survival.cmd** selects the original game mode explicitly.

`Scripts/Build.ps1` compiles the editor module and runs `Scripts/bootstrap_unreal.py`, which recreates the shared surface/glow materials, imports the original audio, and creates the map if absent. It also runs the Voyager planet-material bootstrap. Visual Studio 2022 C++ Build Tools and a Windows SDK are required. Engine paths in scripts currently point to this machine's UE_5.8 installation.

`Scripts/Package.ps1` builds and cooks the standalone Windows game into `Packaged/Riftbound/Windows`.

## Runtime design

- `RiftCharacter` / `RiftHUD`: first-person controls, rifle, aim, reload, sprint, pulse charges, recovery, HUD, and co-op menu.
- `RiftGameMode` / `RiftGameState`: authoritative three-wave director, squad loss, victory, restocking, four-player limit, and complete run reset.
- `RiftEnemy`: three enemy silhouettes, server pursuit with obstacle sweeps, attacks, replicated death effects, and proximity mines.
- `RiftWorld`: seeded instanced conifers and scenery, lighting, generated cabin and camp, replicated breakable cover, and spacecraft/portal.
- `RiftVisual`: shared primitive geometry and tinted surface/glow material creation.
- `bootstrap_unreal.py`: reproducible asset generation; original WAV sources are in `SourceArt/Audio`.

Gameplay damage, ammunition, cooldowns, enemies, phase transitions, and destruction are controlled by the server. Static scenery is regenerated deterministically on every machine; gameplay actors and state replicate through Unreal's networking framework. See [Epic's networking overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/networking-overview-for-unreal-engine).

## Validation

The original survival build was verified on this machine on September 7, 2026: UE 5.8.2 C++ editor and standalone Windows builds succeeded; all 15 survival network checks passed with a separate host and client, including actual client movement; the packaged game loaded its generated scenery/materials/audio and its rifle activated the spacecraft. These results apply to that survival build, not to Voyager. Visual screenshots are retained under Saved/Screenshots and the packaged game's Saved folder.

Run `Scripts/Test-Network.ps1` for a hidden listen server and a separate client on test port 7787. It checks that both see the squad, client firing/reload/charge RPCs work, broken cover and UFO activation replicate, all three waves reach victory on both instances, and restart restores the world. It saves evidence in `Saved/NetworkTestReport.json` and `Saved/Logs/NetworkHost.log` / `NetworkClient.log`.

Development flags `-RiftProbe -RiftCombatTest -RiftCapture` run a visible-rendering rifle/UFO smoke test and save a screenshot. `-RiftAutoTest` advances a test run by damaging the ship and clearing enemies; it is a development test switch, not a player mode.

This is the first playable prototype: stylized primitive characters, selective component destruction, a fixed arena, and three waves. It does not yet include animated production characters, terrain deformation, Chaos building collapse, persistent inventory, save progression, dedicated-server packaging, or online matchmaking.
