# PHASE 1 — WORLD

**Done when:** fly a generated world, dig a cave, and it saves/loads as seed + deltas.

**Status: a slice is in, the phase is not.** What exists is a modifiable heightfield, not the
voxel world the brief describes. See D-023 for why the slice came first and what it does
not carry over.

---

## What is in

| Item | State | Where |
|---|---|---|
| Deterministic terrain from the seed | **in** — integer value noise, flat under the city, rolling beyond | `Sim/src/world/Terrain.cpp` |
| Player digs and piles | **in** — round brush with integer falloff, on the command/replay path | `CommandType::ModifyTerrain`, `ALivingCityPlayerCharacter` |
| Chunked dirty tracking | **in** — 16×16-cell chunks, per-chunk versions, the view rebuilds only what moved | `Terrain::ChunkVersion`, `ALivingCityTerrainView` |
| Chunked hashing | **in** — one digest per chunk, refreshed by the brush; the world hash folds 400 digests, not 102,400 cells (D-027) | `Terrain::HashChunk` |
| Save = seed + deltas (R5) | **data model in, no save file yet** — `ModifiedCellCount` / `ModifiedCell` is the delta list | `Terrain.h` |
| Height bounds | **in** — clamped at ±20 m / +40 m, never asserted | `kMinHeightCm` / `kMaxHeightCm` |
| Ground material | **in** — Riftbound's `M_VoyagerTerrain`, read-only reuse | `ALivingCityTerrainView` |

## What is not

- **Voxels.** 32³ chunks, greedy meshing, the block/clay split. A heightfield cannot represent
  a cave or an overhang, so "dig a cave" is genuinely not met. This replaces `Terrain`, it does
  not extend it.
- **Generation pipeline.** Heightmap → biomes → hydrology → caves → minerals → roads. Only the
  first and the road grid exist, and the road grid lives in `City`, not here.
- **Materials with hardness and tool tiers.** Every cell digs the same.
- **Actual save/load.** The delta list exists; nothing writes it to disk or reads it back.
- **Free-fly camera.** The player walks. `ADefaultPawn` flew; it was replaced when the
  character arrived.

## Measured

Recorded 2026-09-11, same machine and flags as the Phase 0 baseline, with the city, 1,500
citizens, the economy and the terrain all ticking.

| Metric | Phase 0 | **Now** |
|---|---|---|
| Headless throughput | 2,388,227 ticks/s (119,411×) | **55,520 ticks/s — 2,776×** (target ≥ 1000×) |
| Per-tick cost | 419 ns | **18.0 µs** |
| Steady-state allocations | 0 | **0** (I5 holds) |
| Invariant tests | 120 | **191, all passing** |

Before D-027 this read 2,360 ticks/s; the terrain hash was 95% of the tick.

Revalidated 2026-09-12 after the command-burst fix (D-028): 500,000 measured ticks after
10,000 warm-up ticks took 8.9982 s, or **55,566 ticks/s (2,778x real time), 18.0 us/tick**,
with **zero steady-state allocations**. This is the default 1,500-citizen headless workload;
it is not a measurement of the planned 50,000-citizen tiers or of Unreal frame rate.
The cache regression also reconstructs a hash from all live heights after seam edits,
saturation, refused brushes, regeneration and clearing; the frozen terrain hash is unchanged.
The full local CI gate passed all **194 tests**, the selfcheck, two independent 100,000-tick
runs (hash `ab2d88bece8d8a7c`), a different-seed control, and 100 replay checkpoints without
divergence. Replay ended at absolute tick 604,000 after the 100,000 recorded simulation steps.

## Known risks carried from CLAUDE.md §5

- **R-03** — runtime meshes have no Mesh Distance Fields, so software Lumen cannot see the
  terrain. The atmosphere port (D-021) makes this visible for the first time: judge whether
  hardware RT is needed *on a mid-range GPU*, not on the 7900 XT (R-07).
- **R-05** — per-voxel support propagation is unaffordable city-wide. Not yet relevant; will
  be the moment voxels land.

## Next

1. Save/load of the terrain delta list (small, proves R5 end to end).
2. Decide flat vs spherical world for good before the voxel store is designed (D-021).
3. Voxel chunk store, then greedy meshing on worker threads (R10), then the block/clay split.
