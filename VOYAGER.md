# Riftbound: Voyager

An original procedural space-adventure prototype made in Unreal Engine 5.8.2. Begin beside your scout ship, explore and mine a spherical planet, fly through its atmosphere into space, fight corsairs, cruise to another world, and descend to its surface without a scene transition.

## Play

**[Play Voyager City.cmd](Play%20Voyager%20City.cmd)** starts beside a market entrance on your saved planet, with your ship parked on the central plaza. It keeps your expedition progress. Use this launcher to try the new city interiors and residents immediately.

Double-click **[Play Voyager.cmd](Play%20Voyager.cmd)**, **[Play Riftbound.cmd](Play%20Riftbound.cmd)**, or the **VOYAGER** desktop shortcut. The launcher uses the standalone Windows build when present, otherwise the compiled game through the installed Unreal executable. The previous forest horde game is still available through **[Play Survival.cmd](Play%20Survival.cmd)**; see the [survival guide](SURVIVAL.md).

Your first planet has two glowing crystal deposits near the landing area. **F** records the planet as a discovery and earns 30 minerals. Hold **left mouse** on crystals to extract more. Your ship starts nearby; **E** boards it when you are close enough.

The settlement marker leads to a city about **320 metres** from the landing site. Follow a signed entrance ramp to walk directly into a cafe, clinic, market, workshop, residence or security office. Approach a resident and look at them to talk. Watch for roaming animals outside the city; the HUD identifies a nearby species.

## On foot

- **WASD** walk; **mouse** look; **Shift** sprint; **Space** jump.
- **F** scan and record this planet. A first discovery earns 30 minerals.
- **Left mouse** mines a targeted crystal within 18 metres. **V** switches to the pulse sidearm; left mouse then fires out to 1,200 metres. Both tools can hurt residents, so aim carefully.
- **E** talks to the resident you are looking toward within 4 metres, or boards your own ship within 11 metres.
- During a conversation, **1** greets, **2** asks about daily life, **3** asks for local advice, and **Backspace** ends the conversation. Walking away also ends it.
- On foot, **C** switches between the bodycam-style walking view and a steady camera. The bodycam view uses a wider field of view, restrained movement and subtle grain; the mining tool appears while used.
- **U** upgrades your ship's lasers for 75 minerals when aboard or within 13 metres. Five upgrades are available; each also restores hull and shields.
- **F5** saves the expedition. **Escape** opens the menu.

## Flying

- **Space** lifts off and rises away from the nearest planet. **Left Ctrl** descends toward it. These directions follow local gravity as you move around a spherical world.
- **W** increases forward speed. **S** brakes to a stop. **A/D** strafe. **Mouse** steers.
- **Shift** boosts forward flight and ascent/descent. **Left mouse** fires twin lasers.
- Keep climbing to fly through the **60 km** atmosphere into space. The planet stays beneath you throughout the flight; crossing the atmosphere never teleports the ship.
- On a planet, descend below 10 metres and press **E** to park and disembark.

The HUD identifies the physically nearest planet and measures altitude above its local terrain. It shows metres near the ground, kilometres higher up, actual speed in m/s or km/s, atmospheric density, and a reentry indicator while descending quickly. Selecting a different navigation target does not change these local flight readings.

## Visiting more worlds

Every system contains five planets. Above **60 km** altitude, **Tab** selects a planet and **J** starts continuous autopilot cruise to it. The ship travels through the system and finishes above that planet's north pole at **90 km** altitude. Press **J** again, steer with the mouse, or use the movement controls to cancel cruise and take over.

Hold **Left Ctrl** to descend, with **Shift** for faster descent. Atmospheric entry happens as you fly downward; **E** is only for boarding, landing near the ground, and disembarking. You can also fly around a planet and choose another landing location manually.

**H**, available above **60 km**, jumps to the next procedurally generated star system through a short transition. There is no fuel cost. System and planet seeds generate their appearance consistently. Five biome families produce gardens, crystal deserts, frozen frontiers, ember wastes, and violet fungal landscapes.

Planets are original compact worlds with **600–900 km radii** and **1,200–1,800 km diameters**, comparable in size to smaller moons. Their terrain and gravity follow spherical surfaces. They are not replicas of Earth, and flight speeds and atmospheric behavior are tuned for a playable adventure.

The system index supports over two billion system seeds, with five destinations per system, and wraps back to the first system at its limit. Detailed terrain is generated near players while the spherical planets remain visible at a distance. Ascent, descent, and travel between planets share one continuous scene; only travel to another star system uses a transition.

## Cities, wildlife, and landscapes

Each planet has three deterministic settlement sites: one near the north landing area and two remote sites. Each city is roughly 400 metres across, with **60 fully enterable buildings**, terrain foundations, roads, roof equipment, antennae, and a central landing plaza. The city palette follows the planet's biome. Street ramps, open doorways, rooms and the surrounding planet share the same coordinates and collision. There are no interior loading screens. Buildings have 3�18 occupied floors with 4.8-metre floor spacing. Switchback stairs connect every floor to an accessible roof. Window openings have glass and frames; entrance doors stand open. Smooth ramp collision under visible steps lets characters walk naturally. No elevator is required for access.

Six furnished layouts provide cafe seating and counters, clinic beds, market shelves, workshop benches, residential lounges and security dispatch desks. Original procedural materials add plaster, wood, metal, fabric and ceramic finishes. Nearby rooms receive warm lighting from a pool of at most eight active lights, with four more lights illuminating nearby streets. The multitool is lowered while you explore a city; using it brings it back up.

Cities populate with up to **36 humanoid residents** nearby, with a shared **72-person cap** distributed across cities visited by co-op players. Seeded identities supply names, clothing, homes and workplaces. Residents follow street and doorway routes, visit cafes, work, rest, deliver supplies or patrol. Mining tools and nearby ship weapons can make civilians seek shelter and guards investigate. Conversations start with authored responses tied to roles and locations; optional local Ollama rewrites are validated before display. The routine clock advances one in-game hour per real minute; it is not yet a planetary day/night lighting simulation. Citizens stream out when everyone leaves; their names remain stable when revisiting, while their moment-to-moment routines restart. Dead residents stay absent when a city streams out and back in during the same star-system visit; those deaths and active wanted cases are session state, not part of the expedition save.

Five biome species graze, wander, and flee from approaching explorers: crestback grazers, dune sailrunners, tundra tuskbeasts, ashwing striders, and lanternback browsers. Their articulated bodies animate as they move over the spherical ground. The server manages a shared population of up to 24 nearby animals; distant animals and city detail stream out to keep runtime memory bounded.

Terrain combines an original generated rock-and-grit albedo texture with biome tinting and procedural variation. Continuous surface coordinates keep material detail aligned across terrain patches. Planetary atmospheres, spherical cloud layers, distant cloud coverage, and changing views of the curved horizon connect the ground and orbital views. Texture provenance is recorded in [Art/Textures/PROVENANCE.md](Art/Textures/PROVENANCE.md).

## Combat and progression

**V** equips a pulse sidearm. Residents have authoritative health, react to assault, and can die; their non-graphic collapse, stopped routine and closed conversations replicate to co-op players. Skin, fitted clothing, hair, eyes and skeletal animation use documented CC0 MakeHuman assets and original clips.

Crimes trigger emergency reports and **one to five wanted stars**. Repeated assault, homicide and attacks on law enforcement escalate the response. Up to four patrol ships respond to each suspect, with independent cases for co-op players. They use line-of-sight sensors, follow the last observed position when the suspect is hidden, and stand down after 35�75 seconds without contact. Opaque walls and roofs break sight; window glass stops shots but allows vision. Patrol shots have a short targeting warning so movement can evade them. Patrols can be damaged and destroyed; doing so escalates the case. Escape in your ship, hide inside a building, or leave the star system. Emergency suit/ship recovery ends a pursuit and preserves cargo.

The scout, corsairs and patrols use a shared original Kestrel airframe with separate liveries, a framed canopy, recessed engines, retracting landing gear, streamed PBR materials and three mesh detail levels. Patrols have flashing beacons and a search light.

Red corsair ships appear in orbit. Their shots are telegraphed, so move or boost to evade. Your scout has shields and hull integrity; shields start recharging after six seconds without damage. Defeating a corsair awards 25 minerals. Each laser upgrade adds five damage. Emergency recovery repairs your ship if it is destroyed and preserves cargo.

Solo and host progression save automatically on rewards, upgrades, and travel. The save contains the current system and planet, the host's mineral inventory, upgrades, pirate kills, and discovered planets. **F5**, **Escape → Save Expedition**, and **Save and Quit** also request a save. A resumed expedition starts on foot at the last selected planet's landing site; exact walking and flight positions are not saved.

The save is `Voyager-Expedition.sav` under the running game's `Saved/SaveGames` folder. For the supplied standalone layout, that folder is `Packaged/Riftbound/Windows/Riftbound/Saved/SaveGames`. The editor-run game and packaged game use separate Saved folders. Development tests use a separate `Voyager-Automation` slot. A client's save command saves the host's expedition; it does not create persistent client progression.

## Co-op

Choose **Escape → Host Co-op Expedition**, or use **[Host Co-op.cmd](Host%20Co-op.cmd)**. Up to three friends with the same build can use **[Join Co-op.cmd](Join%20Co-op.cmd)** or enter the host's LAN IP in the menu and select **Connect to Expedition**. Use `127.0.0.1` for a second instance on this computer. The default port is UDP 7777. To copy the standalone game to another Windows computer, copy the entire `Packaged/Riftbound/Windows` folder.

The host runs one shared star system. Players have separate ships and cargo and can fly, cross atmospheres, and land independently. **H** changes the system for the whole expedition, so coordinate that jump with the host and other explorers. Persistent saves preserve the host's progression; joining players' inventories, upgrades, and discoveries are session-only. This is direct IP/LAN co-op, without EOS/Steam matchmaking or NAT traversal. Internet play needs a reachable host and suitable network routing/firewall settings.

## Build and test

- `Scripts/Build.ps1`: compile the Unreal editor module, import the original terrain texture, and create materials, cloud assets, and audio.
- `Scripts/bootstrap_voyager_materials.py`: import terrain albedo and generate terrain, planet, atmosphere, architecture, and animal materials in Unreal's Python environment.
- `Scripts/bootstrap_voyager_clouds.py`: generate the volumetric and distant cloud materials.
- `Scripts/bootstrap_voyager_city_materials.py`: generate the interior and citizen materials.
- `Scripts/bootstrap_voyager_characters.py`: import the clothed skeletal characters, five animation clips per variant and three LODs.
- `Scripts/bootstrap_voyager_ships.py`: import the original Kestrel meshes, LODs and PBR materials.
- `Scripts/bootstrap_voyager_building_materials.py`: build clear architectural glazing.
- `Scripts/Test-VoyagerCrime.ps1`: actual sidearm hits, deaths, patrol arrival/fire, hiding, search expiry and recovery. Add `-Network` for replicated cases/deaths and an innocent peer, `-Render` for game captures, `-Packaged` for the standalone build.
- `Scripts/Test-VoyagerBuildings.ps1`: walk ordinary stairs through every floor and roof in four buildings and check collision across two complete cities.
- `Scripts/Package.ps1`: build/cook the standalone Windows game.
- `Scripts/Test-Voyager.ps1`: automated solo exploration test.
- `Scripts/Test-Voyager.ps1 -Network`: separate listen host and client, including client-driven boarding, flight, travel, mining, and discovery.
- `Scripts/Test-VoyagerSurface.ps1`: walk and verify collision at three distant locations on a spherical planet.
- `Scripts/Test-VoyagerLife.ps1`: audit settlements, animals, and all five biomes. Add `-Packaged` to run this audit against the standalone build.
- `Scripts/Test-VoyagerCity.ps1 -Render`: physically walk into and out of six room types and a remote planetary settlement, climb stairs, and check population, movement, conversations, validation and danger reactions. Add `-Packaged` for the standalone build. Both normal expedition saves are checked for preservation.
- `Scripts/Test-VoyagerCityNetwork.ps1`: two real processes test city populations on different planets, replicated citizen identities/movement, dialogue and alarm RPCs. Add `-Packaged` to test the standalone build.
- `Scripts/Test-VoyagerRealism.ps1`: renders natural ground cover, atmosphere fixtures and a remote planet, checks streaming, radial collision and materials, and preserves normal saves. Add `-AI` for a real local Ollama conversation, or `-Packaged` for the standalone build. The altitude fixtures supplement the continuous flight test above.
- Add `-Render` to an audit/test script to create rendered screenshots.
- Test reports/logs are retained in `Saved`; tests stop only their own processes.

Open `Riftbound.uproject` in Unreal Engine to edit the project. The saved Forest map is intentionally empty: the selected game mode builds its scenery at runtime. Build scripts currently target `C:\Program Files\Epic Games\UE_5.8` and require Visual Studio C++ Build Tools and a Windows SDK. Test commands describe the available checks; consult the generated reports for the result of a particular build.

Version **0.7.0** adds textured skeletal residents, killable NPCs, pulse-sidearm and per-player wanted cases, searching patrol ships, original Kestrel airframes and fully connected tower interiors. Validation includes 21 solo exploration/flight checks, 24 co-op flight checks, rendered crime/replication checks, and 120-building collision audits with four complete roof round trips. Rendered audits also verify material assignments and glazing; death motion is checked for ground contact. Reports are generated locally under `Saved` and are not distributed as source assets.

Version **0.5.0** passed **26 city checks** in both the editor and packaged game, including real character movement through seven entrances, six furnished room types, stairs and a remote spherical settlement. Both builds also passed **15 city network checks** using two real processes on planets about 6,326 km apart: identities, motion, dialogue topics, ending conversations, and alarm replication. The existing **24 co-op exploration/flight checks** passed again. Both normal Expedition files retained their original SHA256 hashes during the city audits. See [packaged city results](Saved/VoyagerCityPackagedReport.json), [packaged city network results](Saved/VoyagerCityNetworkPackagedReport.json), [flight regression results](Saved/VoyagerNetworkReport.json), and [release evidence](Saved/VoyagerCityRelease.json).

Version 0.4.0 previously passed 21 solo checks, 24 host/client checks, three distant surface walking checks, five-biome city/wildlife and ship-clearance audits, and rendered surface/15 km/65 km atmosphere checks. Those older reports are retained in `Saved`. Hidden-window frame cadence, including the new audit logs, is not a benchmark of sustained visible gameplay frame rate.

## Implementation

`VoyagerWorld` renders spherical planets and generates detailed terrain near players. `VoyagerData` defines deterministic system geometry, radial terrain height, gravity direction, atmosphere density, names, and biomes. Local flight readings are derived from position instead of a shared surface/space mode.

`VoyagerSettlement` generates deterministic cities and interiors with instanced components, local origins for large-world precision, and separate distance limits for visual detail and collision. `VoyagerCitizenManager` allocates bounded city populations; citizens follow a street/door graph with server-authoritative routines and double-precision replicated poses. `VoyagerWildlifeManager` streams the shared animal population. These systems do not use atmosphere or planet-selection changes to reset the scene.

`VoyagerShip` uses client prediction with authoritative input replay and reconciliation; weapons, resources, travel, rewards and upgrades are resolved by the server. `VoyagerGameMode` coordinates the shared expedition and save file. `VoyagerCharacter` and `VoyagerHUD` provide walking, extraction, scanning, menus, navigation and flight telemetry.

This remains a prototype with original generated ground art and stylized procedural buildings, people and animals. Rooms can be explored and residents can be spoken to; shop transactions, medical services, a persistent economy, base building and crafting are not implemented. Resource deposits can refresh when their scene is regenerated; mined terrain is not permanently deformed.
