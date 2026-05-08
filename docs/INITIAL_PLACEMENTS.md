# Where the world's starting objects live

Exile uses **three different ROM tables** to place objects in the world
at game start. They look similar in spirit (per-entry world coordinate +
type) but each handles a distinct case the runtime needs to treat
differently. Useful when you're trying to figure out *why* a particular
item is at a particular cell.

This doc covers what's in each table; for *how* the runtime promotes
between primary / secondary / tertiary tiers see `OBJECT_SPAWNING.md`.

## 1. Initial primary table (`&0860–&08b4`)

Single hard-wired entry: **Triax**.

```
objects_type[1]   = 0x26 (TRIAX)
objects_x[1]      = (0x99, 0x64)        // tile_x.fraction format
objects_y[1]      = (0x3b, 0x20)
objects_sprite[1] = 0x04 (SPACESUIT_VERTICAL)
```

Used when an object needs to exist as a primary on **frame 1**, even if
the player can't yet see it. Triax has to be alive immediately so its
update routine can grab the destinator tertiary, teleport away, and arm
the destinator's "stolen" state — all in the first tick — without a
scripted intro.

In `src/game/game.cpp` (in `Game::init`):

```cpp
Object& triax = object_mgr_.object(1);
object_mgr_.init_object_from_type(triax, ObjectType::TRIAX);
triax.x = {0x99, 0x64};
triax.y = {0x3b, 0x20};
```

The slot is freed naturally when Triax demotes to tertiary after
teleporting away — see `INTRO_TRIAX.md` for the full sequence.

## 2. Initial secondary table (`&0af2–&0b73`)

19 hand-authored entries describing the **objects you can pick up or
fight at game start**. Stored as four parallel 32-byte arrays (the
remaining 13 slots are zero-initialised). The matching arrays in our
port live at `src/objects/object_tables.h:162-186`:

```
initial_secondary_x[32]
initial_secondary_y[32]
initial_secondary_type[32]
initial_secondary_energy_and_fracs[32]
```

`ObjectManager::init` copies these into the live secondary pool.
Entries with `y == 0` are inactive. When the player walks close enough,
the lifecycle "promote_secondary_to_primary" pass at `&0be8` instantiates
a full primary at the stored coordinates.

### The 19 starting entries

| slot | x    | y    | type byte | what it is                                  |
| ---- | ---- | ---- | --------- | ------------------------------------------- |
| 0    | 0x9b | 0x39 | 0x64      | INVISIBLE_INERT (player spawn placeholder)  |
| 1    | 0xa3 | 0x5d | 0x43      | PIANO                                       |
| **2** | **0x98** | **0x4d** | **0x50** | **INACTIVE_GRENADE**                |
| **3** | **0x98** | **0x4d** | **0x50** | **INACTIVE_GRENADE**                |
| 4    | 0xa4 | 0x67 | 0x59      | JETPACK_BOOSTER                             |
| **5** | **0x9f** | **0x49** | **0x50** | **INACTIVE_GRENADE**                |
| 6    | 0xa0 | 0x49 | 0x46      | CANNON                                      |
| **7** | **0xc0** | **0x4e** | **0x50** | **INACTIVE_GRENADE**                |
| **8** | **0x48** | **0x56** | **0x50** | **INACTIVE_GRENADE**                |
| 9    | 0x83 | 0x78 | 0x4e      | REMOTE_CONTROL_DEVICE (RCD)                 |
| 10   | 0xc5 | 0x60 | 0x45      | BOULDER                                     |
| 11   | 0x87 | 0x59 | 0x53      | YELLOW_WHITE_RED_KEY                        |
| 12   | 0x97 | 0x5e | 0x1d      | RED_ROLLING_ROBOT                           |
| 13   | 0xe1 | 0x61 | 0x03      | FLUFFY                                      |
| **14**| **0x84** | **0x5b** | **0x50** | **INACTIVE_GRENADE**                |
| **15**| **0x98** | **0x80** | **0x50** | **INACTIVE_GRENADE**                |
| 16   | 0x99 | 0x3c | 0x4a      | DESTINATOR                                  |
| 17   | 0xe7 | 0x80 | 0x3a      | GIANT_BLOCK                                 |
| 18   | 0x7c | 0x77 | 0x4c      | EMPTY_FLASK                                 |

The seven `INACTIVE_GRENADE` entries are the only thing in the ROM that
puts a grenade in the world at game start. Slots 2, 3 and 5 cluster
around `x ≈ 0x98–0x9f`, `y ≈ 0x49–0x4d` — the spaceship interior — and
account for the "three grenades near the beginning" the player sees.

### Packed energy/fractions byte (`&0b53`)

Each `initial_secondary_energy_and_fracs[i]` packs three sub-fields:

```
bits 7-4 : energy high nibble  (× 0x10 to unpack into the full byte)
bits 3-2 : x.fraction high     (× 0x40)
bits 1-0 : y.fraction high     (× 0x40)
```

So slot 7's `0x47` decodes to `energy=0x40, x_frac=0x40, y_frac=0xc0` —
that's a grenade with mid-energy at sub-tile offset (0x40, 0xc0) inside
its (0xc0, 0x4e) cell. The compact format trades sub-tile precision
(quarter-tile granularity) for a 4× space saving over storing four
separate bytes.

## 3. Tertiary tables (`&05ef–&0a71`)

The bulk of the world's content. 254 entries spread across 4 parallel
arrays: tile-x, tile-and-flip, mutable data byte, and (for ~half the
entries) a spawn type byte.

Tile placement is *implicit* — the procedural landscape generator
emits one of the nine `CHECK_TERTIARY_OBJECT_RANGE_N` marker tile types
(0x00-0x08) at the desired positions, and the resolver walks the
matching tertiary sub-range looking for the first entry whose `x` matches
the cell's `x`. This is how every door, switch, transporter, fixed
crystal, nest, and most of the static decoration exists.

See `TERTIARY.md` for the full lookup mechanics, and `LANDSCAPE.md` for
how the procedural generator emits markers.

### What's NOT in the tertiary tables

Useful negatives, since "is X tertiary?" comes up a lot:

* The 19 starting items above (grenades, piano, etc.).
* Triax (initial primary).
* Anything spawned at runtime by another object (drops from slimes,
  bullets fired, mushroom balls thrown, fireballs from explosions).
* The PISTOL — `0x5a` doesn't appear in either `tertiary_objects_type_data`
  or as a `0xda` data byte under a `FROM_DATA` entry. If you see one
  near spawn, it's probably the BLASTER (`0x5c`, which IS in the
  tertiary type table) or has been picked up from a drop.

## Quick decision tree: "where would I put a new X at game start?"

```
is X a unique scripted thing that needs to exist on frame 1
even when offscreen?
   yes → initial primary table       (rare; only Triax today)
   no  ↓

is X just one item / pickup / creature at a specific (x, y)?
   yes → initial secondary table     (32-slot budget; 19 used)
   no  ↓

is X tied to a tile-marker emitted by the landscape (door,
switch, transporter, nest, FROM_TYPE/FROM_DATA spawn)?
   yes → tertiary tables             (254-entry budget)
   no  → won't be in the world at start; spawn it dynamically
         from another object's update routine
```

## Editor implications

The Option-B refactor (see `OBJECT_SPAWNING.md` § Tertiary data) makes
the tertiary table editable per-(x, y) in the in-game editor. The
initial primary and initial secondary tables stay as compile-time
constants in `src/objects/object_tables.h` — there's no runtime mutator
for them yet. If you want to relocate a starting grenade, the cleanest
path is currently:

1. Edit `initial_secondary_x[]` / `_y[]` for the slot you care about.
2. Or place a *new* tertiary entry at the new location via the editor's
   object palette + right-click.

A future "starting state" editor would expose the initial secondary
table similarly — it's a TODO, but the data layout is already small
enough to hand-edit if needed.
