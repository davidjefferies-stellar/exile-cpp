# The Landscape Generator

Exile's world is a 256×256 grid of tiles whose contents are *not* stored
anywhere — every cell is computed on demand by a pure function of `(x, y)`
plus a 1 kB hand-authored overlay. The whole map fits in the BBC Micro's
ROM because there is no map; there's a 700-byte algorithm that hallucinates
one.

This doc explains what `Landscape::get_tile(x, y)` is doing under the hood
and where each piece lives.

## Inputs and outputs

```cpp
uint8_t Landscape::get_tile(uint8_t tile_x, uint8_t tile_y) const;
```

Inputs: two `uint8_t`s. Coordinates wrap at 256 in both axes — the world
is a torus. There is no global state; the same `(x, y)` always returns
the same byte.

Output: one byte encoding `tile_type | flip_flags`:

```
bit 7  : FLIP_HORIZONTAL
bit 6  : FLIP_VERTICAL
bits 5-0 : tile type (one of TileType, 0x00..0x3f)
```

Tile types 0x00..0x08 are special: they are `CHECK_TERTIARY_OBJECT_RANGE_N`
markers, not real tiles. They get rewritten by `resolve_tile_with_tertiary`
into either a real tile-from-tertiary or one of nine fallback feature
tiles. See `docs/TERTIARY.md`.

Tile types 0x09..0x3f are the actual visible/collidable tiles (SPACE,
EARTH, STONE, slopes, walls, switches, doors, …).

## Two implementations

The code has two implementations of the generator behind `get_tile`:

| Path                  | File                  | Purpose                                              |
| --------------------- | --------------------- | ---------------------------------------------------- |
| `get_tile_pseudo_6502` | `landscape.cpp`     | Faithful Alu-emulating port. The reference.         |
| `get_tile_cpp`        | `landscape_cpp.cpp` | Native C++ rewrite. Same algorithm, plain ints.     |

Toggle via `exile.ini`'s `[landscape] use_cpp_impl`. They MUST produce a
byte-identical map for every `(x, y)`; the toggle exists so the rewrite
can be A/B-tested against the reference and so future tweaks have a
known-good baseline to diff against. The pseudo-6502 path is the default.

The pseudo-6502 path threads the 6502's accumulator and carry through an
`Alu` struct (`landscape.cpp:39-74`) so each line maps 1:1 onto the
disassembly at `&178d-&19a6`. The C++ rewrite collapses chains of
`LSR / ADC / SBC / ROL` into ordinary unsigned arithmetic, keeping the
9-bit running value in an `unsigned` so `>> 8` recovers the carry when
the next chained add needs it.

## The per-tile hash `f1`

Almost every downstream decision is gated by an 8-bit per-tile hash
called `f1`:

```cpp
uint8_t calc_f1(uint8_t x, uint8_t y) {
    const uint8_t step = uint8_t(((((y >> 1) ^ x) & 0xf8) >> 1) + x);
    return uint8_t((step >> 1) + (step & 1) + y);
}
```

Port of `&178d-&179d`. `f1` looks like noise but is a deterministic mix
of `x` and `y`, biased so that nearby tiles tend to share `f1` bits —
which is what gives the procedural caves their patches of similar
material rather than per-tile chaos. Consumers downstream pick out
specific bits:

- bit 0 → surface H-flip
- bit 4 → slope orientation pick
- bit 5 → slope fall direction (left vs right)
- bits 4..1 → earth/stone variant (`leave_with_earth_or_stone`)
- bits 6..3 → various passage gates

## Region selection

`get_tile` first decides which of two paths produces the byte: the
**procedural algorithm** or the **map overlay**. Port of `&179d-&17ce`.

```
                          y range          → path
   above ground          y < 0x4e          → algorithm (returns SPACE)
   upper surface row     y == 0x4e         → algorithm (surface tile)
   first earth row       y == 0x4f         → algorithm (EARTH)
   shallow underground   0x4f < y < 0x79   → algorithm OR overlay (gated)
   mid-belt              0x79 ≤ y < 0xbf   → algorithm only
   deep underground      0xbf ≤ y < 0xff   → algorithm OR overlay
                                              (y is folded down by 0x46
                                               to share the same overlay
                                               address space)
```

The "gated" rows in the overlay band still run a sub-hash (`f2 / f3 / f4`)
that decides per-tile whether to read from the overlay or fall back to
the algorithm. That's how the spaceship interior at the start of the game
sits embedded inside otherwise procedural caves.

### The map overlay

`data/map_overlay.h` holds 1024 bytes extracted from `&4fec-&53eb` of the
ROM. These contain hand-authored tiles for set-piece areas: the starting
spaceship, Triax's lab, the destinator chamber, puzzle rooms, the
endgame.

The address calculation at `&17d6-&17e8` mixes `f4` and `new_f2` to pick
an offset:

```cpp
const uint8_t addr_low  = uint8_t((f4 << 3) ^ new_f2);
const uint8_t addr_high = uint8_t((f4 & 0x03) + 0x4f + carry5);
const unsigned offset   = addr_high * 256u + addr_low + 0xec - 0x4fec;
return map_overlay_data[offset];
```

The 1 kB block is therefore a *paged* memory the algorithm indexes into;
nearby `(x, y)` values pick nearby overlay bytes, which is why the
hand-designed areas feel coherent.

## The procedural algorithm

When the region check sends us to `get_tile_from_algorithm`, the
algorithm walks an early-exit waterfall — each block decides "is this
tile something specific? → return; otherwise fall through". Port of
`&17f6-&19a6`.

### 1. Above ground, surface, first earth row

```
y <  0x4e   → SPACE                      (sky)
y == 0x4e   → tile_for_surface(x, f1)    (trees / bushes / clear sky)
y == 0x4f   → x == 0x40 ? LEAF | flip_v
                       : EARTH           (the surface earth row, with
                                          one specific leaf at x=0x40)
```

`tile_for_surface` (`&1937-&1945`) decides per-x whether to drop a
surface feature or leave clear sky. Clear surface tiles can be H-flipped
based on `f1 & 1`, giving a varied skyline.

### 2. Side/bottom fill

The world is a torus, but the playable region is an island. Outside a
roughly circular band the algorithm just returns earth-or-stone fill so
the player can't tunnel around the world. Implemented at `&1814-&1827`:

```cpp
if (y & 0x80) {                              // bottom half
    if (uint8_t(x + 0x07 + 1) < 0x2b) return earth_or_stone(f1);
} else {                                     // top half
    if (uint8_t(x + 0x1d + 1) < 0x5e) return earth_or_stone(f1);
}
if ((f1 & 0xe8) < y) return earth_or_stone(f1);
```

### 3. Square caverns (`f5`)

A four-step bit mash on `y`, ANDed with `x`:

```cpp
const unsigned step1 = 3u * y + (y >> 7);
const unsigned step2 = (uint8_t(step1) + 1) / 2 + y;
const unsigned step3 = uint8_t(step2 & 0xe0) + x + (step2 >> 8);
if ((uint8_t(step3) & 0xe8) == 0) {
    if (!(y & 0x80)) return SPACE;          // upper-world cavern
    return (x >> 3) == 0x0a ? VARIABLE_WIND_H : VARIABLE_WIND;
}
```

Hits about 1 in 32 cells, scattered in a roughly grid-aligned pattern —
the regular open chambers in mid-cave. In the lower half they become the
windy chambers (`VARIABLE_WIND` tile, `0x0e`); in the upper half just
empty `SPACE`.

### 4. Vertical shafts (`f6`)

Six bit-mashes of `f1` and `x` yield a 3-bit residue; if it's zero the
tile is a shaft (`0x08`). The pattern produces tall narrow vertical gaps
through the rock, biased to certain x columns by `tile_x & 0x80` and a
secondary `(x/2 + y) & 0x30` gate. See `&1852-&1878`.

### 5. Below `y = 0x52`: solid

Anything above this depth that survived the above checks is solid earth/
stone — the floor of the upper world that the player has to dig or
shaft through.

### 6. Passages (`f7` and below)

Below `y = 0x52` the algorithm tries to carve out passages. `f7` is yet
another hash:

```cpp
const unsigned f7 = ((((f1 & 0x68) + y + 1) & 0xff) + 1) / 2 + y;  // chained
```

Mask-and-XOR against `y` decides:

- `((f7 & 0xfc) ^ y) & 0x17 != 0` → branch into **sloping** passages
  (`handle_sloping_passage`).
- otherwise → branch into **horizontal** passages.

#### Horizontal passages

A second gate `((f1 + x + carry) & 0x50)` decides "is this tile inside a
horizontal passage at all". If yes:

- A `(gate & x) >> 2 + y >> 2` hash picks one of 16 features (table
  entries 0x1d-0x2c in `kTilesTable`).
- Special case at `y == 0xe0` toggles the V-flip — the flooded mushroom
  passage at the bottom of the map.

#### Sloping passages — `slope_function`

Port of `&1946-&19a6`. Detects three independent passage families:

1. **Sloping caverns**: every 4-row stripe where `((y/2) ^ y) & 0x06 == 0`.
   `f1` bit 5 picks fall direction (left or right). Position-along-slope
   `(y + 0x16 + x)` picks "middle of cavern" (clear) vs "edge"
   (slope-tile). Edge tiles get rotated via `kTileRotations` so the same
   pattern serves all four orientations.
2. **/ passages**: probe `(1 + x + y) & 0x8f == 0x01`. Diagonal corridors
   rising to the right.
3. **\\ passages**: probe `(y - x) & 0x2f == 0x01`. Diagonal corridors
   falling to the right.

Each `/` and `\\` probe additionally returns `y in [2..5]` for the
"close to the edge but not centred" case, which produces a graded slope
tile rather than pure SPACE.

A subtle interaction: the horizontal-passage path *also* asks
`slope_function` whether the current cell is in any sloping passage — if
yes, it overrides the horizontal feature lookup with SPACE so the slope
"cuts through" the horizontal passage cleanly without leaving stray
mushrooms or pipes.

Port-only convention: the slope detection uses `recalc_f1` (an inline
copy of `calc_f1`) when called from `slope_function` because the original
6502 routine doesn't always have `f1` in scope.

### 7. Earth-or-stone fill — `leave_with_earth_or_stone`

The catch-all "this tile is solid; pick a flavour" branch. Port of
`&191c`:

```cpp
if (f1 == 0) return EARTH;
const uint8_t idx = ((((f1 >> 3) & 0x0e) >> 1) + 1);
return kTilesTable[(idx > 8) ? 8 : idx];
// → one of nine entries: EARTH, three stone variants, three flipped
//   stone variants, two more (table at &114f-&1157)
```

So `f1` bits 4..1 pick between earth and seven stone variants, giving
the visible strata banding in the underground. Bit 0 is unused at this
layer — already consumed by the shift.

## Tile types as `CHECK_TERTIARY` markers

The bytes at `feature_tiles_table[N]` (in `landscape.h`) tell you what a
`CHECK_TERTIARY_OBJECT_RANGE_N` marker (`N` = 0..8) defaults to when no
tertiary entry matches the tile's x:

| `N` | feature tile               | range covers       |
| --- | -------------------------- | ------------------ |
| 0   | `TALL_BUSH (0x1b)`         | invisible switches |
| 1   | `SHORT_BUSH | flip_v`     | transporters       |
| 2   | `SPACE (0x19)`             | object-from-data   |
| 3   | `SPACE`                    | metal doors        |
| 4   | `STONE_TWO (0x1e)`         | stone doors        |
| 5   | `STONE_SLOPE_45_FULL`      | stone-half objects |
| 6   | `STONE_SLOPE_22_ONE`       | space-with-object  |
| 7   | `EARTH_HORIZ_QUARTER_EDGE` | greenery-with-obj  |
| 8   | `SPACE`                    | switches           |

So when the algorithm emits `0x03` at some `(x, y)` and there's no
tertiary entry at that `x` with type 0x03 — the tile renders as plain
SPACE, *not* as a metal door. The tertiary table populates the doors;
the algorithm just marks "look here".

## Observable invariants

These are properties downstream code relies on:

- **Determinism**: `get_tile(x, y)` is a pure function with no
  initialisation. The world is the same every time the game starts.
- **Wraparound**: tile coordinates use modular arithmetic. The world
  has no "edges" in code — only the side/bottom fill that prevents the
  player from tunnelling around.
- **No global state**: nothing in the algorithm reads anything other
  than `x`, `y`, the static tables, and the static map overlay. Render
  order doesn't matter; calling it from a debug overlay produces the
  same bytes the game uses.
- **Byte-identical implementations**: both `get_tile_pseudo_6502` and
  `get_tile_cpp` must agree for every `(x, y)`. If a change to one
  produces a different map than the other, that's a regression.

## Debugging notes

- The "Map mode" checkbox + right-drag panning lets you scroll the
  camera around without moving the player, so you can stare at the
  algorithm's output anywhere in the world.
- Left-clicking a tile in map mode populates the top-right tile-info
  overlay with its raw byte, type, flip, palette and (where relevant)
  the tertiary it resolved against. Indispensable for "is this tile
  what I think it is?" questions.
- Toggling `use_cpp_impl` mid-game flips which path runs from the next
  tile fetch onward. There's no caching, so the world re-shapes
  immediately to whichever implementation produces the current byte.
- Adding a tile-type override (e.g. force tile `(x, y)` to a specific
  type) is straightforward via a debug hook in `Landscape::get_tile`,
  but be aware that the renderer caches no per-tile state — collision,
  rendering, and tertiary spawning will all see the override
  consistently, which is what you want for testing.
