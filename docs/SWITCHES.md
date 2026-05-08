# Switches

How a switch press in Exile turns into a door (or other tertiary) toggle.
This is the end-to-end picture: where the data lives, how a switch
"finds" its targets, and how an Option-A 6502 `data_offset` becomes a
world tile in Option B's per-cell tertiary store.

For the broader mechanism that lets a non-switch landscape tile *render
and behave as a switch* (the "redirect" trick), see
`docs/SWITCH_REDIRECTS.md`. This doc focuses on press-to-effect.

## Data sources

Five static ROM tables (in `src/objects/object_tables.h`) plus one
runtime table (in `Landscape::tertiary_entries_`) define a switch:

| Table                                  | Origin | Indexed by | Holds |
|----------------------------------------|--------|------------|-------|
| `tertiary_objects_x_data[]`            | ROM    | source_idx (0..254) | The X column the source row applies to |
| `tertiary_objects_tile_and_flip_data[]`| ROM    | source_idx | The tile graphic to render (with flip bits). Type `0x08` here means SWITCH |
| `tertiary_objects_data_bytes[]`        | ROM    | data_offset (0..255) | Initial state byte: bit 7 = "needs spawning", bits 3-7 = effect-id, bits 1-2 = toggle mask, bit 0 = on/off |
| `tertiary_objects_type_data[]`         | ROM    | type_offset | Object type to spawn (used by FROM_TYPE / FROM_DATA tile types, not switches) |
| `switch_effects_table[]`               | ROM (`src/behaviours/environment.cpp` &4958) | walked group-by-group | For each effect-id: list of target data_offsets to toggle |
| `tertiary_entries_[]`                  | Runtime, baked | per-cell entry idx | The live mutable copy of the source row's `tile_and_flip` + `data` + `type` for one specific world cell |

`tertiary_ranges[]` and `tertiary_data_offset[]` slice the source tables
into nine type-keyed sub-ranges (one per CHECK_TERTIARY tile type
0x00..0x08) — see the table at the top of `docs/SWITCH_REDIRECTS.md`.

## At bake time: cell → entry → source row

`Landscape::bake_tertiary_lookup()` (`src/world/landscape.cpp:133`) walks
every world cell in row-major `(y, x)` order:

1. If the cell's raw landscape tile-type is in `[0x00, 0x08]` (a
   CHECK_TERTIARY marker), scan the matching source-table sub-range:
   ```
   for i in tertiary_ranges[type] .. tertiary_ranges[type+1]:
       if tertiary_objects_x_data[i] == cell.x:  found = i; break
   ```
   The Y is ignored — only X has to match. (This is the famous
   x-only-in-range scan that lets one source row drive multiple cells
   on the same column.)
2. If found, copy three bytes off the source row into a fresh
   `TertiaryEntry`:
   - `e.tile_and_flip = tertiary_objects_tile_and_flip_data[found]`
   - `e.data          = tertiary_objects_data_bytes[found + tertiary_data_offset[type]]`
   - `e.type          = tertiary_objects_type_data[…]`
3. `add_tertiary_entry(e)` returns a fresh per-cell entry index. Store
   it in `tertiary_idx_[(y, x)]`. Each matching cell gets its own copy.

So after bake:
- The static ROM tables hold the source-of-truth template state.
- `tertiary_entries_[idx]` holds the *live* state for one specific cell.
- The bake order determines `idx` — early `(y, x)` cells get low
  indices.

A "switch cell" is any per-cell entry whose
`e.tile_and_flip & TileFlip::TYPE_MASK == 0x08`. That includes both
direct switches (raw landscape type `SWITCH 0x08`) and redirect
switches (raw type 0x00..0x07 with a source row that rewrites
`tile_and_flip` to 0x08). See `SWITCH_REDIRECTS.md`.

## Pressing a switch

`update_switch` at `src/behaviours/environment.cpp:445` runs every
frame. The `obj.tx` field is a rolling 8-frame press-history register;
on the leading edge of a fresh press (`obj.tx == 0x80`):

```cpp
obj.tertiary_data_offset ^= 0x01;       // toggle bit 0 = "switch state"
uint8_t data   = obj.tertiary_data_offset;
uint8_t toggle = (data >> 1) & 0x03;    // bits 1-2 = which door bits XOR
uint8_t effect = data >> 3;             // bits 3-7 = effect-id
process_switch_effects(ctx.mgr, ctx.landscape,
                        effect, /*mask=*/0xff, toggle);
```

So the switch's own data byte encodes:

| Bit(s) | Meaning |
|--------|---------|
| 0      | On/off (visual: also used to flip the sprite horizontally) |
| 1-2    | `toggle` — bits to XOR into each target's data byte |
| 3-7    | `effect_id` — group index into `switch_effects_table` |

For effect_id 12, the toggle nibble is `0x02` (XOR bit 1, the door
OPENING bit). Pressing → door starts opening. Pressing again → toggle
bit 1 again → door starts closing.

## `switch_effects_table` and target resolution

`switch_effects_table` is a flat byte array delimited by `0x00`s. Each
group starts with a zero byte; the (effect_id+1)-th zero starts group
`effect_id`. Subsequent non-zero bytes are target data_offsets.

```
0x00  0xb0 0xbb 0x84   ; effect 0: targets 176, 187, 132
0x00  0x0f 0x29        ; effect 1: targets  15,  41
...
0x00  0x6a 0x8b        ; effect 12: targets 106, 139   ← (227,156)/(227,188)
...
0x00  0xa7 0xb9 0x10   ; effect 23
0x00                   ; end sentinel (24 zeros walked total)
```

`switch_effect_targets(effect_id, …)` walks zeros until it has seen
`effect_id + 1` of them, then collects subsequent bytes until the next
zero.

A *target* is a 6502-style data_offset — a position in the static
`tertiary_objects_data_bytes` array. To find the world cell:

1. Decode `(tile_type, source_idx)`. For each tile_type T in 0..8,
   compute `s = (data_offset − tertiary_data_offset[T]) mod 256` and
   check whether `s` is in `[tertiary_ranges[T], tertiary_ranges[T+1])`.
   The 256 offsets partition cleanly so exactly one type matches.
2. Look up `target_x = tertiary_objects_x_data[source_idx]` — the
   column the target row points at.
3. Find world cell(s) with raw landscape type T at column `target_x`.

`resolve_data_offset_to_tile()` (`src/world/tertiary.cpp:75`) does
steps 1-3 and returns the first matching cell. The wiring overlay uses
this to draw a green wire from the switch to its target tile.

### Worked example: data_offset 0x6a (effect 12, target 0)

| Step | Computation | Result |
|------|-------------|--------|
| Decode | `s = 0x6a − tertiary_data_offset[3]` (METAL_DOOR offset is `0xfa = -6`) | `s = 112` |
| In range? | `tertiary_ranges[3..4) = [87, 122)` | yes, METAL_DOOR |
| Source X | `tertiary_objects_x_data[112]` | `0xe6 = 230` |
| World cell | First cell with raw type METAL_DOOR (0x03) on column 230 | (230, Y) |

So target 0 of effect 12 is "the metal door on column 230". Target 1
(`0x8b`) decodes the same way to "the stone door on column 103". Both
switches at (227, 156) and (227, 188) — sharing the same effect_id —
toggle this same pair.

## At runtime: fanning the toggle across siblings

`process_switch_effects` (`src/behaviours/environment.cpp:379`) is the
6502's `&49db` plus an Option-B-specific fix.

For each target data_offset `b` in the effect's group:

1. Call `find_shared_entries_for_data_offset(landscape, b, …)` to get
   the list of per-cell tertiary-entry indices that share the source
   row. Implementation:
   - Decode `(T, source_idx, X)` exactly as `resolve_data_offset_to_
     tile` does.
   - Scan column X for every cell with raw type T; collect
     `landscape.tertiary_index_at(X, y)` for each.
2. Read the current value of the door state. Prefer a live primary's
   `tertiary_data_offset` (the visible door already onscreen) over the
   stored entry's `data` byte (which is stale while a primary is live).
3. Compute `newv = (prev & mask) ^ toggle`.
4. Write `newv` back to **every sibling entry**. If a primary owns a
   sibling, update its live `tertiary_data_offset` too. If not,
   re-arm bit 7 ("needs spawning") on the entry's stored byte so the
   tile's primary will re-spawn next time it comes into view.

The sibling fan-out is what makes "press one switch → both halves of a
2-tile-tall door open together" actually work in this port. Without it
each per-cell entry was independent and only one cell's data byte
flipped per press, leaving the other half of the door visually frozen.

It also restores the 6502's "two switches at the same X share state"
behaviour: pressing (227, 156) updates entries for *both* switch cells
at column 227 (the source row idx 116 wires both there) AND every
target door cell on columns 230 and 103. Pressing (227, 188) afterwards
sees the already-toggled state and flips it back.

## Diagnostics

`Game::init` writes a one-shot dump to `exile-debug.log` covering every
CHECK_TERTIARY cell on column X=227 (the user-reported pair), with the
target back-resolution. For each switch cell:

```
# switch @227,156 tile=0x3 from_map=no tertiary_idx=521
#   resolved_tile=0x8 (type=0x8) data=0x64 effect_id=12
#   target[0] data_offset=0x6a -> tile_type=0x3 source_idx=112 X=230
#       cell @230,Y1 raw=0x03 entry_idx=...
#       cell @230,Y2 raw=0x03 entry_idx=...
#   target[1] data_offset=0x8b -> tile_type=0x4 source_idx=147 X=103
#       cell @103,Y1 raw=0x04 entry_idx=...
```

That tells you, for each switch press, exactly which world tiles get
their data byte toggled. Same template can be cribbed elsewhere when a
specific switch's wiring is in question.

## Quick triage when a switch "doesn't work"

1. Read the cell's raw landscape byte (`Landscape::get_tile`). If
   `tile_type > 0x08`, it's not a CHECK_TERTIARY marker — done.
2. Read its tertiary index (`Landscape::tertiary_index_at`). If
   `NO_TERTIARY`, no source row matched on X — the cell renders as the
   feature-tile fallback and does nothing.
3. Read the entry (`Landscape::tertiary_entry(idx)`). The
   `tile_and_flip & 0x3F` byte tells you what it actually is. Type
   0x08 = switch (direct or redirect).
4. Decode `effect_id = (entry.data & 0x7f) >> 3` and look up its
   targets in `switch_effects_table`.
5. For each target, run the back-mapping (see "Worked example" above)
   to find the door's column. Walk that column for a cell of the
   matching raw type.

The `Switches` checkbox in the bottom HUD draws green wires using
exactly this resolution; if a wire goes to a clearly-wrong tile, the
back-mapping in `resolve_data_offset_to_tile` is the place to look.
