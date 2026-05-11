# The Tertiary Object Pool

The tertiary pool is the most compressed of Exile's three object tiers —
small enough that the entire world's persistent content lives in fewer
than 1 kB of static tables.

## Four parallel arrays, one row per tertiary

Disassembly addresses &05ef / &06ee / &0986 / &0a71:

| Array                                 | Addr  | Length | Per entry                                                                     |
| ------------------------------------- | ----- | ------ | ----------------------------------------------------------------------------- |
| `tertiary_objects_x_data`             | &05ef | 255    | World tile-x                                                                  |
| `tertiary_objects_tile_and_flip_data` | &06ee | 255    | tile-type \| flip bits                                                        |
| `tertiary_objects_data_bytes`         | &0986 | 235    | Runtime state (door open/closed, creature count, bit 7 = "not yet promoted") |
| `tertiary_objects_type_data`          | &0a71 | 129    | Spawn-type for "object from type/data" tiles                                  |

There's no y-coordinate column — that's the key. A tertiary entry is
**an x-coordinate plus a tile byte**, not a specific world cell.

## How a tertiary attaches to a world cell

Port of `get_tile_and_check_for_tertiary_objects` at &1715, implemented
in `resolve_tile_with_tertiary`:

1. The procedural landscape (a deterministic function of `(x, y)`)
   emits one of the nine `TILE_CHECK_TERTIARY_OBJECT_RANGE_N` marker
   types (0x00–0x08) at the spots where the map's pattern calls for
   "look for a tertiary here".
2. Each marker type `N` is a lookup hint into one sub-range of the
   tertiary array — the ranges at &05d3:

   ```
   tertiary_ranges = {0x00, 0x1d, 0x39, 0x57, 0x7a, 0x9e, 0xbc, 0xd8, 0xf6, 0xfe};
   ```

   So marker type 0 uses tertiaries 0x00–0x1c, type 1 uses 0x1d–0x38,
   …, type 8 uses 0xf6–0xfd. That's why there are only 254 tertiaries
   total.

3. Within that range, the resolver scans `tertiary_objects_x_data`
   looking for any entry whose x equals the current tile's x.
4. **If found**, the tertiary's `tile_and_flip_data` byte replaces the
   marker — that's what actually gets rendered and collided against.
   The `data` and `type` byte arrays carry the entry's runtime state
   (for doors / switches / spawnable objects).
5. **If not found**, it falls back to `feature_tiles_table[N]` (at
   &117c in the 6502), which is basically the "filler" tile for that
   range — `TALL_BUSH`, `SHORT_BUSH | flip_v`, `SPACE`, `STONE_TWO`,
   `STONE_SLOPE_45_FULL`, `STONE_SLOPE_22_ONE`,
   `EARTH_HORIZONTAL_QUARTER_WITH_EDGE`, etc. for ranges 0–8.

## Why you see lots of the same `Tnnn`

A single tertiary entry is not "an object at (x, y)" — it's "an object
at **column x** of **whatever landscape row** is currently emitting the
matching marker type". One entry can legitimately display in dozens of
cells, because the procedural landscape emits `CHECK_TERTIARY_n`
markers at many y-positions along the same x. The 6502 and this port
simply paint the tertiary's tile wherever the marker + x coincide.

## Worked example: `T250`

Tertiary-index 250 falls in range 8 (`tertiary_ranges[8..9] =
[0xf6, 0xfe)`). Its `x = 0x79`, and its tile byte is `0xd1` =
`TILE_POSSIBLE_LEAF (0x11) | FLIP_H | FLIP_V`. It's a leaf decoration,
not a bush — but because this leaf gets splattered wherever the
landscape emits `CHECK_TERTIARY_8` at column 0x79, you see dozens of
identical `T250` debug labels stacked vertically at that column.

Bushes specifically come from the **fallback** path for
`CHECK_TERTIARY_0` (no tertiary matched) where
`feature_tiles_table[0] = 0x1b TALL_BUSH`, so they'd be the cells
*without* a `T…` marker at all.

## Lifecycle at runtime

The `data_bytes` array is not just a spawn flag — it carries live
runtime state that survives offscreen demotion.

- For door / switch / transporter / `*_WITH_OBJECT_FROM_TYPE`
  tertiaries, the 6502 spawns a primary object from the tertiary
  (matching `create_primary_object_from_tertiary` at &4042). Bit 7 of
  the tertiary's `data_bytes` entry is cleared when that happens —
  this port does the same in `Game::spawn_tertiary_object`.
- When the primary wanders far enough to be demoted and its type flags
  include `KEEP_AS_TERTIARY`, `ObjectManager::return_to_tertiary` sets
  bit 7 back so the next render pass respawns it.
- Pure-decoration tertiaries (leaves, bushes that happen to match,
  stone chunks that use tertiary placement rather than fallback)
  don't go through spawning at all — only their `tile_and_flip_data`
  is consumed by the tile renderer.

So three things are happening at a `T…` cell:

1. Tile-byte substitution for that cell's render.
2. Optional primary-object spawn whose type comes from
   `tertiary_objects_type_data[type_offset]` or the per-update-routine
   hard-coded type.
3. A persistent flag in `tertiary_data_bytes[data_offset]` that
   survives offscreen demotion so the object comes back when the tile
   reappears.
