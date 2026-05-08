# Rendering

How a single landscape cell becomes pixels on screen. `&xxxx` addresses
refer to `exile-standard-disassembly.txt`.

## The data behind one tile byte

Each cell in the 256×256 landscape is a single byte: **6 type bits + 2
flip bits** (`0x40` = vertical, `0x80` = horizontal). Everything visual
flows out of that byte plus the cell's `(x, y)`.

Three parallel 64-entry tables, all keyed on the 6-bit type:

| Table | 6502 source | Purpose |
|---|---|---|
| `TILE_SPRITE_ID[64]` (`src/rendering/pixel_renderer.cpp`) | `&04ab` `tiles_sprite_and_y_flip_table` | tile type → atlas sprite ID; `0xff` = SPRITE_NONE (skip blit) |
| `tiles_y_offset_and_pattern[64]` (`src/world/tile_data.h`) | `&04eb` | high nibble = vertical sub-tile offset; low nibble = obstruction-pattern group (collision) |
| `tiles_palette_table[64]` (`src/world/tile_data.h`) | `&052b` | BBC MODE 2 palette byte (4 logical colours packed 4 bits each), or a procedural code 0..6 |

The same `tiles_y_offset_and_pattern` byte is consumed by both the
renderer and the collision system — see `docs/COLLISION.md` for the
obstruction-pattern half.

## The pipeline

`Game::render` → `IRenderer::render_tile` → `PixelRenderer::render_tile`:

1. **Resolve tertiary first.** `resolve_tile_with_tertiary`
   (`src/world/tertiary.cpp`) walks the tertiary x-data range for the
   cell's landscape byte. Tile types `0x00..0x08` are *markers*
   (`TILE_CHECK_TERTIARY_OBJECT_RANGE_N`); the matched tertiary entry's
   `tile_and_flip` byte is what actually renders. Doors, switches,
   transporters, level-specific overrides all enter the pipeline this
   way. See `docs/TERTIARY.md`.

2. **Pick sprite ID + flip.** `TILE_SPRITE_ID[type]` gives the atlas
   index. Tile flip bits come from the byte directly. `0xff`
   short-circuits to "no blit" — the cell is just background (e.g.
   SPACE, WATER, the wind tiles).

3. **Resolve sprite extent.** `sprite_atlas[sid]`
   (`src/rendering/sprite_atlas.h`) is computed at compile time from the
   BBC geometry tables in `src/rendering/sprite_data.h` — atlas
   `(x, y, w, h)` plus an `intrinsic_flip` so a sprite stored mirrored
   on the sheet renders correctly. The atlas itself is 128×81 BBC pixels.

4. **Compute sub-tile placement.** From
   `tiles_y_offset_and_pattern[type] >> 4` × 2 we get the atlas-pixel
   y offset of the tile's content (port of `&2420-&243f`). Flip bits
   flip that within the tile cell, so a v-flipped EARTH lands its grass
   at the bottom of the tile instead of the top.

5. **Resolve palette.**
   - The base byte is `tiles_palette_table[type]`.
   - If it's `0x00..0x06` it's a *procedural* code:
     `resolve_tile_palette` (`src/world/tile_data.h`) computes a final
     palette byte from tile position / flip / y. That's how
     stone-strata colour changes every 16 rows
     (`strata_palette_table`), bushes pick their hue from
     `bushes_palette_table`, leaves cycle green / yellow / white,
     mushrooms switch red / blue based on `flip_v`, and so on. Direct
     port of `&239c calculate_palette_for_tile`.
   - The resulting byte feeds `resolve_palette_with_fg`
     (`src/rendering/palette.cpp`), which unpacks the four 4-bit
     logical-colour slots and looks them up in `LOGICAL_TO_RGB[16]`
     (BBC MODE 2 hues) to produce a 4-entry RGB LUT plus a per-slot
     foreground flag.

6. **Blit through the LUT.** `Impl::blit_sprite`
   (`src/rendering/pixel_renderer.cpp`) walks the BBC 4-bit-per-pixel
   sprite data via `bbc_sprite_pixel`, indexes the 4-entry LUT, applies
   flip / scale / zoom, and writes pixels. Logical colour 0 renders
   transparent — that's what lets the water-column raster fill show
   through (port of the `&12a6` palette swap; see `render_water_column`).

7. **Foreground mask.** Pixels drawn with logical colours 8..15 set the
   `fg_mask` buffer. Subsequent object blits skip foreground pixels, so
   sprites hide behind foliage / fences / pipes — port of the 6502's
   `EOR / BMI skip_byte` at `&1066`. The per-slot flags from
   `resolve_palette_with_fg` decide which palette indices count as
   foreground.

## Worked example: `STONE_ONE` (0x12), unflipped

| Step | Lookup | Result |
|------|---|---|
| `TILE_SPRITE_ID[0x12]` | sprite atlas | `0x39` (SPRITE_STONE_ONE) |
| `tiles_y_offset_and_pattern[0x12]` | `0x07` | y_offset 0 (full tile); pattern group 7 (fully solid for collision) |
| `tiles_palette_table[0x12]` | `0x04` | procedural code 4 → spaceship palette path → final byte depends on `tile_y` |
| `resolve_palette_with_fg(palette, /*is_tile=*/true)` | | 4-entry RGB LUT + fg flags |
| `sprite_atlas[0x39]` | (x, y, w, h, intrinsic_flip) | 16×32 BBC pixels at the sheet position |
| `blit_sprite(sx, sy, 0x39, …)` | | pixels written, fg mask updated where logical colour ≥ 8 |

The resulting tile is one of several stone hues drawn at the natural
tile origin; collision treats the whole cell as solid.

## Procedural palette codes (0..6)

| Code | Meaning | Driver |
|------|---|---|
| 0 | Stone — strata changes every 16 rows from `&54` | `strata_palette_table` (`&1185`) indexed by `(tile_y - 0x54) / 16` |
| 1, 2 | Spaceship hull / interior | Different hue in upper world (rmy / rcy) vs Triax's machinery in the lower world (gyw / gmw); `&23ae-&23bb` |
| 3 | Bush | `bushes_palette_table` (`&1195`) indexed by tile flip bits |
| 4 | Generic spaceship variant | Falls through with `tile_y`-dependent shift |
| 5 | Possible leaf | `process_possible_leaf` returns one of three palettes plus may toggle flip / remove the leaf entirely |
| 6 | Mushroom | Red on the floor, blue on the ceiling (driven by `flip_v`) |

Anything `≥ 0x07` in the table is a literal palette byte and is used as-is.

## References

- `src/world/tile_data.h` — palette table + `resolve_tile_palette`
  procedural codes, `tiles_y_offset_and_pattern`,
  `tiles_obstruction_y_offsets`.
- `src/rendering/pixel_renderer.cpp` — `TILE_SPRITE_ID`, `render_tile`,
  `blit_sprite`, foreground mask, `render_water_column`.
- `src/rendering/sprite_atlas.h` / `sprite_data.h` — sprite geometry
  decoded from the BBC tables.
- `src/rendering/palette.cpp` — `resolve_palette_with_fg`,
  `LOGICAL_TO_RGB`.
- `docs/COLLISION.md` — the obstruction-pattern half of
  `tiles_y_offset_and_pattern`.
- `docs/TERTIARY.md` — the marker-tile redirection that runs ahead of
  this whole pipeline.
