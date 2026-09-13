# LIVING CITY

A real-time city simulation where you live as one ordinary citizen inside a fully simulated
economy. **Currently: a playable slice across Phases 1–3** — a procedural city under
Riftbound's sky, 1,500 citizens with homes, jobs, wages, hunger and shopping trips, a closed
money loop, and ground you can dig. See `docs/phases/PHASE_1.md`, `PHASE_2.md`, `PHASE_3.md`
for exactly what is and is not in.

Engineering docs: [CLAUDE.md](CLAUDE.md) · [DECISIONS.md](DECISIONS.md) ·
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) · [docs/phases/PHASE_0.md](docs/phases/PHASE_0.md)

This lives alongside **RIFTBOUND: VOYAGER** in the same Unreal project. They share the
`.uproject` and nothing else — Riftbound's launchers, content and code are untouched.

---

## Two ways to run it

### 1. The simulation — works today, no engine needed

**Double-click `LIVING CITY (sim only)` on the desktop**, or
[Run Living City Sim.cmd](Run%20Living%20City%20Sim.cmd).

Needs no game engine at all. It builds the sim core with the C++ compiler directly, then
demonstrates the Phase 0 result: invariant tests, a determinism proof across two separate
processes, a record-and-replay check, and a throughput benchmark.

> **Windows Smart App Control may refuse to run it.** It blocks unsigned executables, which
> is every binary a compiler produces, and its verdict is unpredictable — the same binary has
> been blocked and then allowed after a rebuild. If it happens, the script says so plainly
> instead of looking like a crash, and the invariant tests still run. See
> [Troubleshooting](#troubleshooting) and **D-014** in [DECISIONS.md](DECISIONS.md).

Expect roughly:

```
194 run, 194 passed, 0 failed

process 1, seed 1337  ->  ab2d88bece8d8a7c
process 2, seed 1337  ->  ab2d88bece8d8a7c
process 3, seed 9001  ->  adcdfaf5dafa45b7

same seed  -> identical world, bit for bit.
other seed -> different world, as it must be.

throughput   : 55,566 ticks/s  (2,778x real time)
steady-state allocations : 0 (I5 holds)
```

The hashes change whenever the simulation gains a system - they are the fingerprint of the
whole world, and a new economy is a new world. What must never change is that the two
processes agree.

Those two identical hashes are the whole point of Phase 0. The same seed produces the same
world down to the last bit, every time, in a fresh process. Saves, tests and debugging all
rest on that.

### 2. The Unreal client — works

**Double-click `LIVING CITY` on the desktop**, or
[Play Living City.cmd](Play%20Living%20City.cmd).

The launcher now uses the standalone Windows build when present. To build and run through
Unreal Editor instead, use `Scripts/Play-LivingCity.ps1 -Rebuild`. Both maps are included by
`Scripts/Package.ps1`; see [BUILDING.md](BUILDING.md) for a fresh checkout.

The simulation runs on its own thread at **20 Hz**. Its thread starts only in the Living City
GameMode; playing Voyager or Survival does not run a hidden city simulation.

There is no level asset to build. The GameMode spawns the ground, sun and sky in code and
runs on the stock `/Engine/Maps/Entry`, which keeps a binary `.umap` out of the repository
entirely (rule R6).

**If you ever need to rebuild it, close the Unreal Editor first.** Unreal Build Tool cannot
compile while the editor holds a Live Coding session. The launcher checks and tells you
rather than failing cryptically.

---

## Controls

| Key | Does |
|---|---|
| **WASD + mouse** | walk and look |
| **Shift** / **Space** | sprint / jump |
| **F** | first- or third-person camera |
| **Left mouse** (hold) | dig the ground under the crosshair |
| **Right mouse** (hold) | pile it up |
| **1 / 2 / 3 / 4** | time: real time, ×10, ×60 (an hour a minute), ×300 (a day in five) |
| **P** / **O** | pause / step exactly one tick |
| **Escape** | quit |

Digging is a simulation command, not a mesh edit: the click goes onto the same queue as every
other input, the sim changes its heightfield on the next tick, and the view redraws the chunk.
A dig is in the replay log; a replayed world has the same holes in it.

---

## What you are looking at

A procedurally generated city of **109 buildings** across a 6x6 street grid, and **1,500
citizens** who each have a home at a real address, a job at a real workplace, and a shift to
work. At 07:00 most are still at home; by 09:00 most are at work. Walk the streets and you are
walking among them.

Everything you can see is computed by a simulation that has never heard of Unreal:

- the street grid, block subdivision, zoning and every building footprint, from one seed
- who lives where, who works where, and what hours they keep
- where each of the 1,500 people is standing, right now, in integer centimetres

Unreal reads that state and draws it. It decides nothing. Press **P** and the city stops,
because the *simulation* stopped.

The HUD reports the day and time, how the population is split between home, commute and work,
the world hash, and frame and tick cost against their budgets.

**Use the time controls.** A simulated day at 1x takes a real day. Press **3** and an hour
passes in a minute; press **4** and the whole day runs in five. Speed changes how often the
simulation ticks, never what a tick does - a city fast-forwarded to noon is bit-identical to
one that got there in real time.

---

## The economy, and how to read the HUD

Every citizen has a bank account. Every Work and Shop building is a firm with one. The
government is the central bank and the only place money is ever created.

```
government --contracts--> firms --wages--> citizens --food--> shops
     ^                                                          |
     +--------------- wholesale food + sales tax ---------------+
```

- **money in world** is every account summed. It must never move except when the central
  bank mints. If it drifts, money conservation is broken and this line is where you see it.
- **food price** is set by each shop from its own stock — low stock, high price. There is no
  price table. **stock** is the food on shelves across the city.
- **hungry** is how many citizens are past the threshold where they go shopping. **starved**
  must stay at 0 while shops have stock and people have money; it is a tested invariant.
- **citizen #N** is one real person followed in full: what they are doing, where they are,
  their balance, hunger and fatigue. Press **3** and watch a day happen to them.

The same rules apply to everyone. There is no player account and no player price — when the
player becomes a citizen (Phase 8), they will earn and spend through exactly this code.

---

## What is not there yet

Being straight about the state of it: this is a playable slice, not a finished game.

- **One good.** Food. The full brief has eight, plus firms that hire, expand and fail, rent,
  utilities and a waste chain. This loop is the seed of that, tested, not the whole thing.
- **A heightfield, not voxels.** You can dig and pile, but not tunnel. Caves and overhangs
  need the Phase 1 voxel world, which replaces this ground rather than extending it.
- Shifts come in two bands (morning and afternoon) so there are always some people out, but
  everyone on a given shift still leaves on the same tick, so departures come in waves. See
  **D-018** and **D-026**.
- The player is still a camera with legs. Becoming an actual citizen — job, rent, a bank
  balance the economy can touch — is Phase 8.
- Buildings are boxes wearing Riftbound's architecture material. The art direction in
  `CLAUDE.md` is deliberate but not yet applied.

---

## Troubleshooting

**"BLOCKED: the Unreal Editor is running."**
Close the editor and run it again. Or press Ctrl+Alt+F11 inside the editor to compile there.

**"Windows Smart App Control refused to run livingcity_headless.exe."**
Smart App Control is enforced on this machine and blocks unsigned executables — which is every
binary a compiler produces. Its verdict is unpredictable: the same binary has been blocked
through several retries and a clean rebuild, then allowed after a later one. Rebuilding
sometimes clears it, so it is worth trying once.

The only real fix is to turn it off:
**Windows Security → App and browser control → Smart App Control → Off.**
**This cannot be undone** — Microsoft provides no supported way to re-enable it without
reinstalling Windows. That is a genuine trade-off, and it is your call, which is why nothing
here touched it. Full write-up: **D-014** in [DECISIONS.md](DECISIONS.md).

**Level generation failed.**
Open the project in the editor and run `Scripts/bootstrap_livingcity_map.py` from
Tools > Execute Python Script.

**Build fails.**
Full log at `%LOCALAPPDATA%\UnrealBuildTool\Log.txt`.

---

## Useful commands

```powershell
# the full commit gate: purity, build, 120 tests, determinism, replay
.\Scripts\Run-CI.ps1

# just the sim core
.\Scripts\Build-Sim.ps1
.\Sim\build\Release\livingcity_tests.exe
.\Sim\build\Release\livingcity_headless.exe run --seed 1337 --ticks 100000 --hash-every 1000
.\Sim\build\Release\livingcity_headless.exe bench --ticks 500000

# record a session and prove the replay matches it
.\Sim\build\Release\livingcity_headless.exe run --seed 42 --ticks 100000 --record run.lcreplay
.\Sim\build\Release\livingcity_headless.exe verify --log run.lcreplay
```
