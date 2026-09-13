# RIFTBOUND: VOYAGER

An original procedural space-adventure prototype in Unreal Engine 5.8.2. Explore spherical worlds with textured terrain, alien wildlife, cloud layers, and procedural cities. Mine crystals, discover planets, pilot your own ship, fight corsairs, and fly continuously between five planets per star system. Planetary cities now have persistent residents, needs, jobs, shops and accounts. Generate another system whenever you want a new destination.

**[Build from source](BUILDING.md)** using Git LFS and Unreal Engine 5.8. This project also contains the separate **[Living City](LIVINGCITY.md)** simulation mode.

Version **0.9.2** improves landing safety, graphics presets, exposure, furniture alignment, usable supply models and structural destruction. Chairs face their tables; workstation screens face the user, with keyboards and mice supported by the desk. Seven original supply models connect to existing inventory actions, and deterministic support failure drives a bounded pool of native Chaos debris. The [world audit](docs/VOYAGER_WORLD_AUDIT.md) records the remaining realism work; [production validation](docs/phases/VOYAGER_PRODUCTION.md) contains build, multiplayer, save, screenshot and full-resolution benchmark evidence. Jail remains removed.

**Double-click [Play Voyager.cmd](Play%20Voyager.cmd), [Play Riftbound.cmd](Play%20Riftbound.cmd), or the VOYAGER desktop shortcut.** The launcher uses the standalone Windows build when available, otherwise the compiled game through the installed Unreal Engine.

**[Play Riftbound Nature.cmd](Play%20Riftbound%20Nature.cmd)** starts beside your ship on the garden planet in your current system, keeping your cargo and discoveries. Explore scan-textured woodland with grass, ferns, shrubs, rocks and three foliage detail levels. The physical atmosphere and lighting continue from the surface into space. On foot, **C** toggles the bodycam-style walking view.

Residents can use an installed local Ollama `llama3.2:3b` model for short conversational variations. Authored dialogue appears immediately; a valid local reply replaces it while the same conversation remains open. This runs on the CPU to leave GPU memory for the game. Use `voyager.LocalAI 0` in the console or launch with `-NoVoyagerLocalAI` to disable it. Dialogue does not control gameplay, and the game works with Ollama stopped. See [nature asset credits](Art/Nature/PROVENANCE.md) for CC0 sources.

## Your first journey

1. Press **F** to discover the starting planet and earn minerals. Hold **left mouse** on glowing crystals to mine them.
2. Approach your ship and press **E** to board. Hold **Space** to launch and rise away from the planet. Hold **Shift** to boost through the atmosphere.
3. Use **W/S** to accelerate/brake, **A/D** to strafe, **mouse** to steer, **Shift** to boost, and **left mouse** to fire.
4. Above **60 km**, press **Tab** to choose a planet and **J** to cruise there continuously. The ship arrives at **90 km** altitude. Hold **Left Ctrl** to descend, release thrust and slow to **5 m/s** below **10 m**, then press **E** over a clear, level landing area.
5. Press **H** above **60 km** to jump to the next star system. **U** buys a laser upgrade for 75 minerals while aboard or near your ship.

On foot: **WASD** move, **Shift** sprint, **Space** jump. In flight: **Space** rises, **Left Ctrl** descends; **E** requests a safe landing or exit. **F5** saves; **Escape** opens the menu and graphics presets.

**I** opens the backpack; **1–8** selects food, cooking, medkits, demolition charges or ammunition recipes. **Q** cycles owned supplies into your hand; **left mouse** uses held food, a medkit or a demolition charge. Crafting materials remain available in the backpack. **V** clears the held supply and switches to the sidearm; **R** reloads its 24-shot magazine. Hunt wildlife, approach a carcass and press **E** to collect meat, hide and bone. Cook beside your landed ship or inside a cafe or residence. A planted demolition charge has a three-second fuse; retreat at least 22 metres.

Follow the settlement marker to the first city, about **320 m** from the landing site. All **60 buildings** have furnished occupied floors, glazed windows, open doors, and connected stairs to every level and the roof. Residents use clothed skeletal character meshes and animated walks, commute, work, visit cafes and react to nearby weapon fire. Look toward someone nearby and press **E** to talk; **1–3** choose topics and **Backspace** ends the conversation. Watch animals graze outside the city. Each biome has its own city palette and animal species.

**P** opens the city phone. Use **Left/Right** to change pages and **1–8** for the displayed actions; **P**, **Escape** or **Backspace** closes it. Buy supplies inside a cafe or market, eat and drink from your bag, apply for a job at a workplace, work there for wages, rest at your home, pay bills, recycle waste, or sell ten mined minerals for city credits. Credits and expedition minerals are separate inventories. The phone also shows needs, your real home and employer, outstanding fines, and news from the city accounts. The world keeps running while you use it.

On foot near a city, **N** cycles entrance guidance for your **home**, **workplace**, the nearest **market**, **Civic Security**, and **off**. The marker shows the building and distance, with an arrow when its entrance is outside your view. You can also cycle it with **8** on the phone's Work & Home page.

The simulation keeps **54,000 resident records** across the system's 15 cities, including offscreen residents. The visible population remains bounded to **36 per nearby city, 72 globally**. Moving residents retain their identity, possessions and needs; local home/job assignments refer to actual buildings. This is a bounded integration of the Living City brief, not a claim that traffic, transit, courts, mortgages or voxel disasters are implemented. See the [integration checklist](docs/phases/VOYAGER_CITY_LIFE.md) for validation status and remaining scope.

**V** switches the extraction tool to a pulse sidearm; **left mouse** fires. Reported attacks on residents or patrols raise a **one-to-five-star wanted level** and city citations. Patrol ships respond, pursue visible suspects and search the last observed position when sight is lost. Hide behind opaque building walls or escape in your ship until the search ends; outstanding fines remain in the city account and can be paid at Civic Security. See [character credits](Art/Characters/PROVENANCE.md) and [ship sources](Art/Ships/PROVENANCE.md).

Ground response includes one Vesper hover cruiser, up to four armed officers and one to three Sentinel attack drones per suspect, alongside civic guards and patrol ships. Drones arrive at two or more stars near the surface, follow observed positions and fire after a warning. Opaque cover blocks sight; glass stops shots. Drones can be shot down. The cruiser has a shaped automotive body, windows, emergency lights and four lift pods. Jail and surrender are removed: incapacitation uses ordinary medical recovery, and old saved sentences are discarded. Citations remain payable at Civic Security.

Building windows can break and damaged towers lose upper floors before collapsing. Unreal's native **Chaos rigid bodies** animate a bounded debris pool; structural damage is saved separately. Black-hole encounters use original procedural visuals: **B** in orbit cruises to a safe survey point; **Tab + J** returns to a planet. The dark horizon, disk and halo are a visual approximation with a gameplay tidal hazard, not a general-relativistic ray tracer. No paid Fab plugin or asset was acquired for this feature pass.

Planetary volume weather uses a project instance of Unreal's installed cloud material with native atmosphere/cloud components. Anomaly and distant orbital cloud materials are project-authored. See [cosmos and weather provenance](Art/Cosmos/PROVENANCE.md).

These original worlds have **600–900 km radii**, with radial gravity and procedural terrain. Ascent, atmospheric entry, and interplanetary cruise happen in the same world without a loading transition. Travel to another star system still uses a short jump transition.

The host runs a co-op expedition for up to four players. Players fly and land independently within one system; **H** moves the shared expedition to the next system. The host's progression saves locally; joining players' cargo and discoveries last for that session.

- **[Full Voyager guide](VOYAGER.md):** controls, progression, co-op, saves, building, and test commands.
- **[Original survival mode](SURVIVAL.md):** forest exploration and three enemy waves. Launch with [Play Survival.cmd](Play%20Survival.cmd).
