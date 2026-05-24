# Frog Bomber (itemtest)

Frog Bomber (`tf_ff_game_mode 3`) runs on **itemtest** using a **code-built maze inside your Hammer-measured room** — not a giant grid painted over the whole map.

## One rule: the play volume

Everything (grid, crates, spawns, floor) uses **`tf_bm_room_*`** — the basement box you measured:

| Corner | X | Y |
|--------|---|---|
| NE | 1304 | -281 |
| NW | 2024 | -280 |
| SW | 2024 | -2536 |
| SE | 1304 | -2536 |

- **`tf_bm_room_square 0`** (default): maze fills the **rectangle** inside that box (~11×33 cells @ 64).
- **`tf_bm_room_square 1`**: **inscribed square** maze (same size as the *shorter* side), centered in the room — still **never** grows outside the box.

Crates are only placed if the cell is inside the Hammer box **and** brush traces say the floor is valid (no props in vanilla rooms or inside walls).

## Quick start

```
ff_play bomber
```

Join RED/BLU Scout, MOUSE1 = bomb. Confirm DLL: `tf_bm_build_id` = **`bomber-classic-maze`**. Default maze: **hard stacked walls** (cannot blow up) + **wood crates** (MOUSE1). Movement: **`tf_bm_free_move 1`**. Empty maze: **`bm_fix`**.

## Architecture (no more “expand square over the map”)

| Layer | What |
|-------|------|
| Hammer room | `tf_bm_room_min_*` / `max_*` from your corners |
| Play volume | Grid sized to fit **inside** that room only |
| Maze | DFS corridors; **hard walls** on border + pillars; **soft crates** fill other cells |
| Spawn | Corners of **Hammer room**, not grid index (1,33) in the void |
| Spawn spot | `GetPlayerSpawnSpot` → `BM_PlacePlayerAtArenaSpawn` only |

## Later: dedicated map

When you add a Hammer entity (`func_bomber_play` / `info_bomber_room`), point the same play-volume code at that brush — stop hand-tuning `tf_bm_room_*`.

## Commands

| Command | Purpose |
|---------|---------|
| `bm_fix` | Rebuild maze inside room + warp players |
| `bm_letgo` / `bm_lock` | Free explore vs arena lock |
