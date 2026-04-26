# Object spawning

This document describes how the game creates runtime objects from the static
map data. It's a port of the 6502's tile/tertiary/placeholder machinery at
`&1715 get_tile_and_check_for_tertiary_objects`, `&4042 create_primary_object_from_tertiary`,
and the per-tile update routines starting at `&3e1b`. The code is split across
`src/world/tertiary.{h,cpp}`, `src/game/tertiary_spawn.cpp`, and the behaviour
files in `src/ai/behaviors/`.

## Three object tiers

Exile runs three parallel object stores. Everything alive in the world lives
in one of them at any moment:

| Tier | Slots | Storage | Purpose |
|---|---:|---|---|
| **Primary**   | 16 | Full `Object` struct (`object_mgr_.object(i)`) | Active simulation. Physics, AI, rendering all iterate these. Slot 0 is always the player. |
| **Secondary** | 32 | Compact `SecondaryObject` (`object_mgr_.secondary(i)`) | Objects that went offscreen but whose state we want to remember. Holds type + x/y (whole tile only) + packed energy/fraction. Promoted back to primary when the player approaches. |
| **Tertiary**  | 255 | Read-mostly data tables in `object_tables.h` + mutable 235-byte `tertiary_data_[]` | Static map fixtures — nests, doors, switches, dropped equipment, parked robots. The existence of the object lives in the map data, not in the simulation. |

`GameConstants::PRIMARY_OBJECT_SLOTS = 16` and
`GameConstants::SECONDARY_OBJECT_SLOTS = 32` are fixed; the tertiary data is
259 bytes (the `tertiary_data_[235]` buffer overlaps the start of other
script data in the 6502, which we mirror directly).

See DISTANCE_THRESHOLDS.md (or `&19a7 distances_to_remove_objects_table`)
for when objects move between tiers.

## Tertiary data: the map's inventory

Each tertiary "slot" is an index 0..255 (actually 0..253 used) that selects
a row out of four parallel tables:

| Table | Address | What it stores |
|---|---|---|
| `tertiary_objects_x_data[]`           | `&05ef` | Tile x coordinate for the lookup scan |
| `tertiary_objects_tile_and_flip_data[]` | `&06ee` | Tile byte this slot resolves to (the **game tile**, not the landscape marker) |
| `tertiary_objects_data_bytes[]`       | `&0986` | Mutable data byte (creature count, door state, stored object type…). Bit 7 = "still tertiary, spawn when seen". |
| `tertiary_objects_type_data[]`        | `&0a71` | Object type for `FROM_TYPE` tile variants |

The 255 slots are partitioned by **range**, one range per landscape marker
tile type (0x00..0x08):

```
tertiary_ranges[10] = { 0x00, 0x1d, 0x39, 0x57, 0x7a, 0x9e, 0xbc, 0xd8, 0xf6, 0xfe };
```

Range `[ranges[T], ranges[T+1])` belongs to landscape markers with tile_type
`T`. The data/type offsets for a slot are computed from two small signed
tables:

```
tertiary_data_offset[9] = { 0x01, 0xfe, 0xfb, 0xfa, 0xf8, 0xf5, 0xf5, 0xf3, 0xf3 };
tertiary_type_offset[9] = { 0x00, 0xf5, 0xe9, 0xde, 0xce, 0xbe, 0xb1, 0xa1, 0x98 };

data_offset = (tertiary_index + tertiary_data_offset[T]) & 0xff
type_offset = (data_offset   + tertiary_type_offset[T]) & 0xff
```

These produce sparse indices into the data/type tables. Most tertiary slots
don't use all four tables — e.g. a door tile has a meaningful `data` byte
but no `type` byte; an `OBJECT_FROM_DATA` tile has a data byte that's
the object type and no separate type entry.

## Landscape → game tile resolution

Every rendered cell runs through `world/tertiary.cpp::resolve_tile_with_tertiary()`.
The landscape (procedural generator + map overlay in `data/map_overlay.h`)
returns a tile byte of the form `<flip><tile_type>`. If `tile_type` is
0x09 or higher, the byte passes through unchanged — the game tile is the
landscape tile. If it's 0x00..0x08, the landscape value is a
**CHECK_TERTIARY_OBJECT_RANGE_N marker**, not a real tile:

```
ResolvedTile r = resolve_tile_with_tertiary(landscape_, x, y);
    // r.tile_and_flip   — the ACTUAL tile byte the rest of the game uses
    // r.tertiary_index  — >= 0 if the marker hit a tertiary slot
    // r.data_offset     — index into tertiary_data_bytes[]
    // r.type_offset     — index into tertiary_objects_type_data[]
```

The resolver:

1. Reads the tile byte from the landscape.
2. If `tile_type > 0x08`, returns the raw byte unchanged.
3. Otherwise scans `tertiary_objects_x_data[range_start..range_end)` for
   a slot whose x equals the tile's x.
4. **Hit**: replaces the tile byte with
   `tertiary_objects_tile_and_flip_data[found]` and computes
   `data_offset` / `type_offset` from the signed offset tables.
5. **Miss**: replaces the tile byte with
   `feature_tiles_table[tile_type] | <original flip bits>` — a harmless
   filler tile (typically stone or grass) that keeps the flip from the
   landscape so two adjacent misses don't produce identical orientations.

> The bit-exactness of the 6502's scan matters: the algorithm temporarily
> overwrites the sentinel byte at `tertiary_ranges[T+1]` to make the
> linear scan terminate; we replicate the same `for (i = start; i < end; i++)`
> linear walk in C++. Don't "optimise" to a binary search — the original's
> sparse data isn't sorted, and the current hit rate doesn't justify it.

## Tile types that spawn objects

Once the tile byte is resolved, the **game tile** (`r.tile_and_flip`) drives
behaviour. `Game::spawn_tertiary_object()` is called from the rendering loop
whenever `r.tertiary_index >= 0`, and dispatches on the resolved tile_type:

| Game tile | Constant | Object spawned | Data byte means |
|---|---|---|---|
| 0x01 | `TILE_TRANSPORTER`                         | `OBJECT_TRANSPORTER_BEAM` (0x41) | low bit = stationary beam; bits 1-4 = destination index |
| 0x02 | `TILE_SPACE_WITH_OBJECT_FROM_DATA`         | **PLACEHOLDER** (see below), real type from data byte | bit 7 = still tertiary; bits 0-6 = real object type |
| 0x03 | `TILE_METAL_DOOR`                          | `OBJECT_HORIZONTAL_METAL_DOOR` (0x3c) or `OBJECT_VERTICAL_METAL_DOOR` (0x3d) | door state (locked, opening, colour…) |
| 0x04 | `TILE_STONE_DOOR`                          | `OBJECT_HORIZONTAL_STONE_DOOR` (0x3e) or `OBJECT_VERTICAL_STONE_DOOR` (0x3f) | same format as metal |
| 0x05 | `TILE_STONE_HALF_WITH_OBJECT_FROM_TYPE`    | `tertiary_objects_type_data[type_offset]` | creature-specific state |
| 0x06 | `TILE_SPACE_WITH_OBJECT_FROM_TYPE`         | same | same |
| 0x07 | `TILE_GREENERY_WITH_OBJECT_FROM_TYPE`      | same | same |
| 0x08 | `TILE_SWITCH`                              | `OBJECT_SWITCH` (0x42) | switch effect + toggle state |

The distinction between 0x02 (FROM_DATA) and 0x05/0x06/0x07 (FROM_TYPE) is
load-bearing:

- **`FROM_DATA`** stores the object type in the **data byte**. Bit 7
  doubles as the "not-yet-spawned" flag. All rolling robots, inactive
  chatters, boulders, and most dropped keys live on `FROM_DATA` tiles.
- **`FROM_TYPE`** stores the object type in the **type byte** at a
  separate offset, and uses the data byte for per-creature state (e.g.
  turret's "fire timer" low bit, nest's remaining-creature count).

Door orientation (tile_type 0x03/0x04) comes from the tile_flip bits:
`tile_flip == TileFlip::HORIZONTAL` or `TileFlip::VERTICAL` → vertical door,
everything else → horizontal. The door object types go in pairs
(`3c`/`3d`, `3e`/`3f`), with +1 for vertical.

### Sub-tile placement (`&4069..&407e`)

When the primary object is created, its x/y fraction are chosen to place the
sprite into the correct half of the cell based on `tile_flip`:

```
x_frac = flip_h ? 0 - (sprite_w - 1) * 16 : 0
y_frac = flip_v ? 0                        : 0 - (sprite_h - 1) * 8
```

The 6502 reads these from `sprites_width_and_horizontal_flip_table` /
`sprites_height_and_vertical_flip_table`. We recompute them from the
`sprite_atlas[]` pixel dimensions — close enough for positioning and avoids
duplicating the two-byte "dimension + flip" packing. The 8/16 factors come
from the BBC's fraction unit: 1 pixel = 16 x-fractions horizontally, 1 row =
8 y-fractions vertically (32 rows × 8 = 256 fractions per tile).

### Bit-7 gate

Before creating anything, `spawn_tertiary_object` refuses if the tertiary's
data byte has bit 7 cleared:

```cpp
if (data_offset != 0 &&
    !(object_mgr_.tertiary_data_byte(data_offset) & 0x80)) {
    return;
}
```

`clear_tertiary_spawn_bit()` clears it immediately after creation. Bit 7 is
restored by `return_to_tertiary()` when the primary object demotes
offscreen, letting the next approach re-create the object from the same
tertiary slot. This is how the game maintains "same boulder in the same
place after you teleport away and back".

## The PLACEHOLDER mechanism (tile 0x02)

`TILE_SPACE_WITH_OBJECT_FROM_DATA` is special. The data byte encodes an
object like `RED_ROLLING_ROBOT` (0x1d) or `INACTIVE_CHATTER` (0x38). If we
spawned that object directly:

- The tile is `TILE_SPACE` semantically — empty air.
- Rolling robots have physics weight 4, so gravity applies.
- The robot falls through the tile it was placed on, accelerates downward,
  leaves the screen, gets demoted to secondary.
- Bit 7 of the tertiary data was cleared on spawn, so
  `resolve_tile_with_tertiary` → `spawn_tertiary_object` on the next frame
  refuses to re-create it. **The robot never appears.**

The 6502 solves this by a proxy object: `OBJECT_PLACEHOLDER` (0x49, flags
0xec). Placeholders are:

- **Intangible** (flag 0x80) → our physics step skips gravity for them.
- **`KEEP_AS_PRIMARY_FOR_LONGER`** (flag 0x20) → they cling to the primary
  list even if the player drifts a bit away.
- **`KEEP_AS_TERTIARY`** (flag 0x10) → when they finally demote, they
  return to the tertiary list (bit 7 set), not secondary.

`tertiary_spawn.cpp` creates the object of its **real** type first (so
position / flip / palette are right), then rewrites
`obj.type = PLACEHOLDER` before the update loop sees it. The real type
stays in `tertiary_data_[obj.tertiary_data_offset] & 0x7f`, ready to read
back later.

`behaviors/environment.cpp::update_placeholder()` is the conversion
trigger. Each frame it:

1. Zeros `velocity_x / velocity_y` (belt-and-braces in case wind or water
   got to it — intangible already exempts it from gravity).
2. Converts to the real object if the player is **touching** the
   placeholder (`obj.touching < PRIMARY_OBJECT_SLOTS`), OR
3. Converts if the player is within an 8-tile-x by 4-tile-y box.
4. Conversion: re-read real type from tertiary data, set
   `obj.type / sprite / palette / energy = 0xff`.

The 6502 version at `&4B64 update_placeholder_object` does a line-of-sight
check via `&359a check_for_obstruction_between_objects_80`. We use the
simpler Chebyshev distance — the player is always near the viewport when
placeholders spawn, so it's equivalent in practice. Collectables on
placeholder tiles (equipment range) should technically only convert on
touch; our port converts them early, which makes them visible earlier than
the original but doesn't break gameplay.

### Why rolling robots break without this

Before the placeholder fix, the code path was:

1. Player approaches a tile with `data_byte = 0x9d` (bit 7 set =
   "not yet spawned"; low 7 bits = RED_ROLLING_ROBOT).
2. Render loop calls `spawn_tertiary_object(0x02, ..., data_offset)`.
3. That creates a primary object of type `RED_ROLLING_ROBOT` at the tile.
4. `clear_tertiary_spawn_bit` clears bit 7.
5. Same-frame physics: weight-4 robot + gravity = robot starts falling.
6. Robot leaves screen, `check_demotion` returns true, robot goes to
   secondary with clean data (no more bit-7 tertiary flag).
7. Player turns away and back; secondary slot is already gone; no respawn.

With the placeholder fix the object sits still, invisible-in-behaviour,
until the player is actually next to it — at which point it converts
in-place and the rolling robot rolls away as intended.

## Tile types that DON'T go through spawn_tertiary_object

Not every tile with an update routine funnels through the tertiary-spawn
path. The 6502 calls per-tile update routines at
`&1778-&1787` for tile types 0x00..0x0f, which our port handles via other
mechanisms:

| Tile | Constant | What happens | Status |
|---|---|---|---|
| 0x09 | `TILE_NEST`    | `&3e1b update_nest_or_pipe_tile`. Randomly spawns a creature of the nest's type every ~32 frames while the tile is on-screen, decrementing the nest's creature count. | **Not ported.** Nests render, but don't yet spawn birds/wasps/frogmen. |
| 0x0a | `TILE_PIPE`    | Same routine as nests. | **Not ported.** |
| 0x0b | `TILE_CONSTANT_WIND` | Applies a wind force to on-screen objects via `Wind::apply_surface_wind`. No primary object created. | Partially ported (wind applies; tile update not). |
| 0x0c | `TILE_ENGINE`  | `&3e8a update_engine_tile` → `OBJECT_ENGINE_FIRE` (0x3b). | **Not ported.** |
| 0x0d | `TILE_WATER`   | No object spawn; handled by `Water::apply_water_effects` per primary object. | Ported. |
| 0x0e | `TILE_VARIABLE_WIND` | Variable-strength wind. | **Not ported.** |
| 0x0f | `TILE_MUSHROOMS` | `&3fd2 update_mushroom_tile` — sets player mushroom timers on touch. | Partially ported. |

### Extending to cover nests

To finish the nest/pipe port, add a case to `spawn_tertiary_object` for tile
types 0x09 and 0x0a. The data byte format is:

```
bit 7    : creature-needs-spawning flag (same as our bit-7 gate)
bits 5-3 : remaining creature count / 2
bits 2-1 : nest mode (00 = active)
bit 0    : unused
```

The creature type itself is at `tertiary_objects_type_data[type_offset]`,
same as the `FROM_TYPE` tiles. Spawn frequency is "1 in 32 per frame while
on-screen" (`&3e48 rnd / CMP #&f7`) with a guarantee of one immediate spawn
on first plot (`&3e27 LDX #&00` sets free-slot requirement to zero).

## Summary of the full spawn flow

```
┌──────────────────────────────────────────────┐
│ render loop visits tile (x, y)               │
└────────────────────┬─────────────────────────┘
                     ▼
┌──────────────────────────────────────────────┐
│ resolve_tile_with_tertiary(landscape, x, y)  │
│                                              │
│ landscape tile_type ∈ 0..8 ?                 │
│   no  → return raw tile, tertiary_index=-1   │
│   yes → scan tertiary_x_data in range for x  │
│     hit  → replace with tertiary's tile byte │
│     miss → replace with feature_tiles_table  │
└────────────────────┬─────────────────────────┘
                     ▼
    ┌───── tertiary_index >= 0 ─────┐
    ▼                               ▼
┌─────────────────────────┐   ┌─────────────────┐
│ spawn_tertiary_object() │   │ draw the tile  │
│  check bit-7 gate       │   │ (no spawn)     │
│  dispatch on tile_type  │   └─────────────────┘
│  create_object          │
│  copy flip bits         │
│  (tile 0x02: rewrite    │
│   type to PLACEHOLDER)  │
│  clear bit-7 gate       │
└─────────────────────────┘
```

Each new primary object then runs through the main 18-step object update
loop (`object_update.cpp`), which handles physics, AI (via
`behavior_dispatch.cpp`), explosions, and demotion/return back to secondary
or tertiary storage.

## Port notes and known deviations

- **Tile-level object collision.** Our `check_object_collision` uses a
  Chebyshev distance of 1 tile in both axes — cheaper than the 6502's
  pixel-level box test. This is why freshly-fired bullets need a spawn
  offset (`Weapon::fire`): without it, a bullet is "touching" the player's
  tile the frame after creation and `common_bullet_update` sets
  `energy = 0` immediately.
- **Placeholder visibility trigger.** We use a plain distance box rather
  than the 6502's obstruction-check raycast. Close enough — placeholders
  only exist near the viewport anyway.
- **PLACEHOLDER flag interaction.** `object_types_flags[0x49] = 0xec`
  includes `INTANGIBLE` (0x80) → the generic physics gate in
  `object_update.cpp` already skips gravity for it. Still, `update_placeholder`
  explicitly zeroes velocity each frame — defensively, not because anything
  currently writes to it.
- **Bit-7 race.** `clear_tertiary_spawn_bit` fires unconditionally after
  `create_object`, even if `create_object` failed (e.g. primary list full).
  In that edge case the tertiary is marked spawned but no object exists,
  and the next render frame won't retry. Low-priority bug; moving the
  clear into the `if (slot >= 0)` branch would fix it, at the cost of
  retry-every-frame when the primary list is saturated.
