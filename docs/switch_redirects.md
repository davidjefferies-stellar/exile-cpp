# Switches, redirects, and the X-only-in-range scan

This doc explains how Exile defines switches in the world — including the
"redirect" trick where a non-switch landscape tile-type ends up rendering
and behaving as a switch. It's the result of debugging the pair of
switches at world tiles **(227, 156)** and **(227, 188)**, which were
initially invisible to all of our overlays because they aren't raw
`SWITCH (0x08)` cells in the landscape grid.

## The two layers

A switch in the running game is the product of two pieces of data:

1. **A static ROM source row.** The four parallel arrays
   `tertiary_objects_x_data`, `tertiary_objects_tile_and_flip_data`,
   `tertiary_objects_data_bytes`, `tertiary_objects_type_data`
   (`src/objects/object_tables.h`) hold one row per source entry.
   Each row carries:
   - an X coordinate (`x_data`)
   - the tile-and-flip byte to render (`tile_and_flip_data`)
   - a runtime data byte (effect-id + state for switches, open/locked +
     colour for doors, etc.)
   - an object-type byte (used by FROM_TYPE / FROM_DATA tiles)

2. **A landscape cell** with a CHECK_TERTIARY tile-type (raw type
   `0x00..0x08`). At bake time the cell looks up its source row by:
   ```
   for i in tertiary_ranges[type] .. tertiary_ranges[type+1]:
       if tertiary_objects_x_data[i] == cell.x: return i
   ```
   The Y coordinate is **ignored** — only the cell's X has to match. If
   no row matches, the cell ends up `NO_TERTIARY` and renders as the
   feature-tile fallback (`feature_tiles_table[type]`).

`tertiary_ranges` slices the 255-entry source table into 9 contiguous
type-keyed sub-ranges:

| Range | Tile type             | Indices       | Notes                               |
|-------|-----------------------|---------------|-------------------------------------|
| 0     | INVISIBLE_SWITCH 0x00 | `[0x00, 0x1d)`| Hidden switches; data byte = effect-id MSB |
| 1     | TRANSPORTER 0x01      | `[0x1d, 0x39)`| Transporter beams                   |
| 2     | SPACE_W_OBJECT_FROM_DATA 0x02 | `[0x39, 0x57)` | data byte = ObjectType to spawn |
| 3     | METAL_DOOR 0x03       | `[0x57, 0x7a)`| Metal doors                         |
| 4     | STONE_DOOR 0x04       | `[0x7a, 0x9e)`| Stone doors                         |
| 5     | STONE_HALF_W_TYPE 0x05| `[0x9e, 0xbc)`| Stone-half + spawned object         |
| 6     | SPACE_W_OBJECT_FROM_TYPE 0x06 | `[0xbc, 0xd8)` | type byte = ObjectType to spawn |
| 7     | GREENERY_W_TYPE 0x07  | `[0xd8, 0xf6)`| Greenery + spawned object           |
| 8     | SWITCH 0x08           | `[0xf6, 0xfe)`| Direct switches (only 8 source rows)|

## Direct switches (range 8)

Cells with raw landscape type `SWITCH (0x08)` look up source rows in
range 8. Their `tile_and_flip_data` is *not* itself the SWITCH tile-type
— it's a background tile (CONSTANT_WIND `0x0b` or POSSIBLE_LEAF `0x11`):

```
tertiary_objects_tile_and_flip_data[0xf6 .. 0xfd] =
    0x0b 0x0b 0xd1 0x91 0xd1 0xd1 0x91 0x91
```

The visible switch graphic isn't drawn from the tile — it's a primary
**SWITCH object** (ObjectType `0x42`) spawned on top by
`spawn_tertiary_object`, which dispatches on the resolved tile_type
`0x08`. So a "direct" switch cell is: wind/leaf background tile +
overlaid switch sprite.

The 8 direct-switch X values are:

| Source idx | X       |
|------------|---------|
| 0xf6       | 0xb8 / 184 |
| 0xf7       | 0xb9 / 185 |
| 0xf8       | 0xd9 / 217 |
| 0xf9       | 0x59 /  89 |
| 0xfa       | 0x79 / 121 |
| 0xfb       | 0x39 /  57 |
| 0xfc       | 0x48 /  72 |
| 0xfd       | 0xe8 / 232 |

Any procedural or authored cell with raw type `SWITCH` at one of those X
columns becomes a direct switch.

## Redirect switches

This is the gotcha we hit. A cell whose **raw landscape type is something
else entirely** can become a switch if its source row's
`tile_and_flip_data` happens to be `0x08` SWITCH. The bake's lookup is
keyed only on `(tile_type, X)`, but the source row itself can rewrite
the rendered/dispatched tile type to anything.

Case in point: source row **idx 116**, in the **METAL_DOOR (0x03)
range**:

| Field                | Value     |
|----------------------|-----------|
| `x_data[116]`        | `0xe3` (227) |
| `tile_and_flip_data[116]` | `0x08` (SWITCH) |
| `data_bytes[…]`      | `0x64` → effect_id `0x64 >> 3 = 12` |

So *any* procedural METAL_DOOR cell on column X=227 is rewritten by this
source row into a SWITCH. The cells (227, 156) and (227, 188) hit
exactly that pattern: their landscape byte is `0x03` (METAL_DOOR), the
bake's x-only scan over the METAL_DOOR range finds idx 116, and the
resolved `tile_and_flip` is `0x08`. `resolve_tile_with_tertiary` returns
SWITCH; `spawn_tertiary_object` dispatches on the resolved type and
spawns a SWITCH primary; `process_switch_effects` reads the data byte
to find effect_id 12.

Both cells share the same source row, so they share the same data byte
→ same effect_id → same target list. effect_id 12's targets in
`switch_effects_table` are data_offsets `0x6a` and `0x8b`, which
decode (via `resolve_data_offset_to_tile`) to world cells at:

- **(230, Y)**: a METAL_DOOR. `0x6a − tertiary_data_offset[3] = 112`,
  in METAL_DOOR range, `x_data[112] = 0xe6 = 230`.
- **(103, Y)**: a STONE_DOOR. `0x8b − tertiary_data_offset[4] = 147`,
  in STONE_DOOR range, `x_data[147] = 0x67 = 103`.

Both switches at (227, 156) and (227, 188) toggle the same pair of
doors.
Pressing either switch toggles both doors at once — that's the gameplay
intent: a stacked switch arrangement where two world locations control
the same room.

The same trick goes the other way too. CLAUDE.md notes "INVISIBLE_SWITCH
with door-graphic redirect" — a raw `0x00` cell whose
`tile_and_flip_data` is `METAL_DOOR (0x03)`. That makes a switch *look
like* a door while still functioning as a switch (data byte stores the
effect-id rather than door state). The two redirect directions cover the
common patterns Exile uses to reuse marker tiles.

## Why other code paths missed this

Several debug overlays initially keyed off the **raw landscape
tile_type** rather than the **resolved tile_and_flip type**:

- The `is_switch` flag for the yellow grid outline checked
  `raw_type == 0x08`, missing every redirect switch.
- The `switch_x_aliased` count for the yellow alias hatch did the same.
- The wiring overlay's tertiary-storage loop iterated only the SWITCH
  source range (`[0xf6, 0xfe)`), so redirect switches living in other
  ranges (METAL_DOOR / STONE_DOOR / FROM_TYPE / FROM_DATA) had no
  source-side wire drawn.

All three now key off the resolved type — every cell whose
`tertiary_entry.tile_and_flip & 0x3F == 0x08` is treated as a switch.

## Two earlier dead-ends worth recording

1. **Variable shadowing in `bake_tertiary_lookup`.** When the bake
   gained a "is the resolved type a switch?" check, it declared a new
   inner `uint8_t resolved` that shadowed the outer `uint16_t resolved`
   used to store the new entry index. The `resolved = new_idx`
   assignment wrote to the inner shadow, so every cell ended up
   `NO_TERTIARY` despite the bake creating ~878 entries that nothing
   pointed at. Renamed the inner variable to `resolved_type`.

2. **`TERTIARY_CAPACITY` undersized.** The per-cell tertiary array was
   originally sized for "several hundred" entries; observed bake counts
   are closer to 1000 in this world, with thousands more theoretically
   possible across CHECK_TERTIARY types. Bumped to `16384`. (Capacity
   wasn't the bug we hit, but the old 2048 limit would silently swallow
   later entries the same way the shadow did, so leaving it small was
   another landmine.)

## Operationally: what to look at when a switch seems to "do nothing"

1. Read the cell's raw landscape byte (`Landscape::get_tile`). If it's
   `0x00..0x08`, it's a CHECK_TERTIARY marker — keep going.
2. Read its tertiary index (`Landscape::tertiary_index_at`). If
   `NO_TERTIARY`, no source row matched on X — the cell renders as the
   feature-tile fallback, no behaviour.
3. Read the entry (`Landscape::tertiary_entry(idx)`). The
   `tile_and_flip` byte is what actually renders / dispatches; check
   `& TileFlip::TYPE_MASK` to see whether it's a switch (`0x08`), door,
   transporter, or something else.
4. If it's a switch, the data byte's `>> 3` is the effect-id, and
   `Behaviors::switch_effect_targets(effect_id, …)` returns the list of
   data_offsets the press toggles. `resolve_data_offset_to_tile` walks
   them back to world coordinates.

The `# switch @x,y` block in `exile-debug.log` (set up in
`Game::init`) does exactly this for X=227 and is a good template for
ad-hoc dumps elsewhere.
