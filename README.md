# RIFTBOUND: VOYAGER

An original procedural space-adventure prototype in Unreal Engine 5.8.2. Explore spherical worlds with textured terrain, alien wildlife, cloud layers, and procedural cities. Mine crystals, discover planets, pilot your own ship, fight corsairs, and fly continuously between five planets per star system. Generate another system whenever you want a new destination.

**[Build from source](BUILDING.md)** using Git LFS and Unreal Engine 5.8. This project also contains the separate **[Living City](LIVINGCITY.md)** simulation mode.

**Double-click [Play Voyager.cmd](Play%20Voyager.cmd), [Play Riftbound.cmd](Play%20Riftbound.cmd), or the VOYAGER desktop shortcut.** The launcher uses the standalone Windows build when available, otherwise the compiled game through the installed Unreal Engine.

## Your first journey

1. Press **F** to discover the starting planet and earn minerals. Hold **left mouse** on glowing crystals to mine them.
2. Approach your ship and press **E** to board. Hold **Space** to launch and rise away from the planet. Hold **Shift** to boost through the atmosphere.
3. Use **W/S** to accelerate/brake, **A/D** to strafe, **mouse** to steer, **Shift** to boost, and **left mouse** to fire.
4. Above **60 km**, press **Tab** to choose a planet and **J** to cruise there continuously. The ship arrives at **90 km** altitude. Hold **Left Ctrl** to descend through the atmosphere, then press **E** below **10 m** to land and step outside.
5. Press **H** above **60 km** to jump to the next star system. **U** buys a laser upgrade for 75 minerals while aboard or near your ship.

On foot: **WASD** move, **Shift** sprint, **Space** jump. In flight: **Space** rises, **Left Ctrl** descends; **E** below **10 m** lands and disembarks. **F5** saves; **Escape** opens the menu.

Follow the settlement marker to the first city, about **320 m** from the landing site. All **60 ground floors** have open entrances and furnished rooms, with stairs to galleries in selected buildings. Residents commute, work, visit cafes and react to nearby weapon fire. Look toward someone nearby and press **E** to talk; **1–3** choose topics and **Backspace** ends the conversation. Watch animals graze outside the city. Each biome has its own city palette and animal species.

These original worlds have **600–900 km radii**, with radial gravity and procedural terrain. Ascent, atmospheric entry, and interplanetary cruise happen in the same world without a loading transition. Travel to another star system still uses a short jump transition.

The host runs a co-op expedition for up to four players. Players fly and land independently within one system; **H** moves the shared expedition to the next system. The host's progression saves locally; joining players' cargo and discoveries last for that session.

- **[Full Voyager guide](VOYAGER.md):** controls, progression, co-op, saves, building, and test commands.
- **[Original survival mode](SURVIVAL.md):** forest exploration and three enemy waves. Launch with [Play Survival.cmd](Play%20Survival.cmd).
