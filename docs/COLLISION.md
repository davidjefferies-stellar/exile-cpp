# Collision

How the port handles tile and object collision, and how the 22.5° / 45°
slope tiles fall out of the same machinery as a flat floor.

All `&xxxx` addresses refer to `exile-standard-disassembly.txt`.

## Coordinates

Positions are 16-bit fixed point: an 8-bit `whole` tile number and an
8-bit `fraction` (`Fixed8_8` in `src/core/fixed_point.h`). The world is
256 × 256 tiles; positions wrap mod 256.

Sprite extents are stored in the same fraction units as the position:

```
width_units  = (atlas_w - 1) * 16     // src/objects/collision.cpp:sprite_width_units
height_units = (atlas_h - 1) * 8      // src/objects/collision.cpp:sprite_height_units
```

That matches the 6502's `sprites_width_and_horizontal_flip_table` (&5e0c)
and `sprites_height_and_vertical_flip_table` (&5e89). A whole tile is
0x100 fraction units; one section is 0x20 (eight per tile).

## Tile geometry

Tile shapes are not solid rectangles. Each tile type (low 6 bits of the
tile byte) selects an **obstruction pattern**: an 8-element threshold
table indexed by which eighth of the tile width contains the test point.

```
patterns[group][x_section] -> y_threshold
```

The 21 patterns ship in `src/world/obstruction.h`, ported from `&0100-&01a0`.
Patterns are organised into 10 groups of 4 variants each (normal, v-flip,
h-flip, both); flip bits in the tile byte (bits 6/7) pick the variant.

For a probe point `(x_frac, y_frac)` in a tile, the surface line at
`x_frac` is read out of the pattern, an additional `y_offset` (high
nibble of `tiles_obstruction_y_offsets[type]`, port at `&056b`) is added,
and the point counts as obstructed when

```
(threshold >= y_frac) == effective_v_flip
```

`effective_v_flip` is the landscape's V-flip bit XOR'd with the tile
type's "ceiling-like" bit from `tile_obstruction_v_flip_bit` (`&04ab`
bit 7; port in `src/world/tile_data.h`). Ground-like tiles count points
*below* the surface as solid; ceiling-like tiles invert.

The full point-in-pattern test is `Obstruction::is_obstructed` in
`src/world/obstruction.h` (port of `&39dd-&39f5`).

## Slopes

Slope tiles use the same patterns as everything else; only the threshold
table changes.

| Pattern | Bytes                                                           | Shape                              |
|---------|-----------------------------------------------------------------|------------------------------------|
| `&00`   | `00 00 00 00 00 00 00 00`                                       | flat full-tile                     |
| `&08`   | `00 08 10 18 20 28 30 38`                                       | gentle slope (≈ 22.5°, low half)   |
| `&10`   | `00 10 20 30 40 50 60 70`                                       | full **45°** rise                  |
| `&18`   | `08 28 48 68 88 a8 c8 e8`                                       | steep / 67.5° rise                 |
| `&40-…` | mirrors of `&08-&18`                                            | h-flipped variants                 |

Slope tile types map onto pattern groups via `tiles_y_offset_and_pattern`
(`&04eb`):

| Tile type                                 | Pattern group | Effective shape          |
|-------------------------------------------|---------------|--------------------------|
| `STONE_SLOPE_45 (0x23)`                   | 3             | one tile, full 45°       |
| `STONE_SLOPE_22_ONE (0x24)` + `_TWO (0x25)` | 1 + 2       | two tiles, 22.5° pair    |
| `STONE_SLOPE_45_FULL (0x13)`              | 7            | solid + 45° finish       |
| `STONE_SLOPE_78 (0x2a)`                   | 4            | quarter-solid (door substitute) |
| `EARTH_SLOPE_45 (0x2e)` etc.              | (same groups) | earth-coloured copies    |

A 22.5° slope spans **two horizontal tiles**: tile A uses the gentle-rise
pattern (low half of the slope, group 1) and tile B continues with a
steeper pattern that picks up where tile A left off (group 2). Walking
across the pair the player crosses both tiles' surfaces in turn — there's
no "cross-tile slope" object; it's just two ordinary tiles whose
thresholds line up at the boundary.

A 45° slope (group 3) is a single tile that rises a full tile across its
8 sections. The h-flip variant (group offset `&40 / &48`) gives the
mirror; the v-flip variant produces overhangs / ceilings via the
ground/ceiling toggle described above.

The pattern address table at `&05ab` lists each group's four variants:

```
group 1 (gentle, &08): [&08, &40, &40, &08]   // normal, v-flip, h-flip, both
group 2 (medium, &10): [&10, &48, &48, &10]
group 3 (steep,  &18): [&18, &50, &50, &18]
```

so each tile-type-byte's flip bits index into the same group's four
variants. There's no per-slope code — collision treats slopes as just
another threshold curve.

## Player motion

`Game::integrate_player_motion` (`src/player/player_motion.cpp`) runs the
physics chain after `apply_player_input` builds the frame's acceleration
vector:

```
wind → acceleration → X integrate (revert if blocked, mass-ratio bounce)
                    → Y integrate (snap to surface or revert)
                    → water effects → object touching → camera follow
```

The X and Y axes are integrated independently so a wall on one axis
doesn't pin motion on the other.

### X axis: per-row diff with slope step-up tolerance

`column_move_blocked` walks every 0x20-fraction row down the sprite
height and asks "is this row obstructed at the new column but **not**
at the old column?". Rows that were already obstructed at the old
position never block — so head-already-in-ceiling and feet-already-on-floor
states don't freeze sideways motion.

There's also a slope step-up gate: only the **top half** of the sprite
counts toward an X block. Anything obstructing the lower half is treated
as a slope or step-up and the Y-axis snap below lifts the player onto
the new surface. This is the port's analogue of the 6502's
`apply_tile_collision_to_position_and_velocity` (`&306c`), which uses
obstruction-depth ratios to distinguish "slope, push UP" from "wall,
push BACK" — we don't compute the ratios but the half-sprite gate
reproduces the rule: walls have to be ≥ ½ tile tall to count.

### Y axis: per-section AABB sweep with surface snap

`player_aabb_obstructed` walks every 0x20-fraction x-section the player's
AABB overlaps, crossing tile boundaries on the way (port of `&2fb8` /
`&3033`). For each section it asks "is the leading edge inside the
resolved tile's pattern?". Door tiles are first run through
`Collision::substitute_door_for_obstruction` (`&3ebd-&3ec2`), which
swaps a closed `METAL_DOOR / STONE_DOOR` for `STONE_SLOPE_78` and an
open one for `SPACE`.

When downward motion is blocked by tile geometry, instead of plain
position revert the player **snaps** to the obstructing tile's surface:
the threshold at `player.x.fraction` is read from
`slope_tracking_threshold`, and the sprite top is positioned so the feet
sit exactly at that y. This:

- keeps SUPPORTED set continuously while walking across slopes (the
  per-frame leading-edge probe always fires after gravity), instead of
  toggling every ~5 frames as gravity accumulates over a small air gap
- lets the player track the slope surface frame-by-frame as they walk
  across it — the snap target follows `tile_threshold_at_x` at the
  centre x

`slope_tracking_threshold` falls back to `single_tile_effective_threshold`
(min/max over the sprite width) when the centre x sits over a
non-obstructing section of a partial-solid tile like the
STONE_SLOPE_78 door substitute, so the player still lands cleanly on
something whose solid band misses centre.

## Object–object collision

Other primaries don't run through the player's surface-snap path. The
generic loop in `Game::update_objects` (`src/game/object_update.cpp`) is:

```
type-specific update → reset touching from current overlap (Step 9b) →
  gravity → X integrate (revert if blocked, no snap) →
            Y integrate (revert if blocked, no snap) →
  water effects → tile environment → SUPPORTED probe → object touching
```

`Collision::check_object_collision` (port of `&2a64`) is broad-phase by
whole-tile distance (`±2` tiles) plus narrow-phase pixel-precise AABB.
INTANGIBLE objects (flag bit 7 in `object_types_flags`, `&0354`) are
skipped — explosions, lightning, transporter beams, fireballs.

`Collision::overlaps_solid_object` is a separate test for tile-collision
*back-up*: any active primary heavier than the test object whose AABB
overlaps. It's used:

- by player motion to block movement into doors / cannons / other
  static heavies (the door's `STONE_SLOPE_78` pattern only covers the
  left quarter of the tile; the AABB catches the rest of the sprite)
- by per-object physics integration to revert into-heavy moves

When the player or another light object hits a heavier one,
`Collision::apply_mass_ratio_velocity` runs the 6502's transfer math
(`&2bee-&2c14` + `&2bc6-&2bed`):

```
half_diff = (this_v - other_v) / 2
transfer  = half_diff / 2^|weight_diff|     // rounded toward -inf
lesser    = transfer                         // applied to heavier
greater   = half_diff - transfer             // applied to lighter
```

Each is then halved and, depending on which side was hit-from, doubled.
Signs flip so the colliders move apart.

## Static objects

Objects with weight ≥ 7 (doors, switches, cannons, gargoyles) are
"fully static": their integration zeroes velocity and skips the integrate
step entirely, but they still run object-object collision so a switch
fires when the player touches it.

Collectables (`type ≥ 0x4a`) start with energy bit 7 set. The per-object
loop reads this as `pin_undisturbed` and skips physics until something
touches the collectable, at which point its update routine clears the
bit and gravity resumes. That's how grenades / keys / mushroom-balls sit
still on shelves.

## Stuck-inside-solid

The 6502's `apply_tile_collision_to_position_and_velocity` (`&306c`)
actively pushes objects out of obstructions every frame ("Always try to
move out of obstruction" at `&308a`). The port doesn't yet implement that
push-out, so as a stand-in `Game::update_objects` skips the per-axis
position revert when the object **starts** the integration step already
inside solid geometry — gravity (or any constant force) then walks the
sprite out of the obstruction one fraction at a time. See
`docs/INTRO_TRIAX.md` for why this is load-bearing for the opening
beat.

## Useful references

- `src/objects/collision.cpp` — `check_tile_collision`,
  `check_object_collision`, `overlaps_solid_object`,
  `substitute_door_for_obstruction`, `apply_mass_ratio_velocity`.
- `src/world/obstruction.h` — the 21 patterns, `is_obstructed`.
- `src/world/tile_data.h` — `tile_threshold_at_x`,
  `tile_obstruction_v_flip_bit`, `get_obstruction_pattern_index`,
  `get_tile_y_offset`, `tiles_y_offset_and_pattern`,
  `tiles_obstruction_y_offsets`, `obstruction_pattern_addresses`.
- `src/player/player_motion.cpp` — per-row X diff, per-section Y sweep,
  surface snap, mass-ratio bounce.
- `src/game/object_update.cpp` — generic per-object integration with
  `start_blocked` gate.

The collision-debug overlay (bottom HUD checkbox "Collision") shades each
tile's solid region using exactly the same `tile_threshold_at_x` +
`tile_obstruction_v_flip_bit` calls the physics probes use, plus the
object AABBs, so any sink-through / slope / door-substitute regression
is immediately visible.
