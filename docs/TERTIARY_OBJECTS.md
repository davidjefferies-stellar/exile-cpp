# Tertiary objects

The 254 tertiary entries that drive every door, switch, transporter,
turret, NPC spawner and pickup in Exile's world. The 6502 keeps four
parallel byte arrays (x, tile+flip, data, type) at the addresses noted
at the bottom; each entry below is one index into that bank.

The disassembly's verbose human-readable listing of all 254 entries
lives at lines 796-2063 of `exile-standard-disassembly.txt` — this doc
is a compact column-table view of the same data, grouped by the
`TILE_CHECK_TERTIARY_OBJECT_RANGE_N` marker tile that fronts each block.
See `docs/TERTIARY.md` for how the markers redirect into this table.

## Field encodings

- **Door data byte**: `8....... ` always set when present, `.421....`
  colour (0..7), `....8...` slow flag, `.....4..` moving, `.....2..`
  opening, `......1` locked. See `behaviours/environment.cpp:DoorFlag`.
- **Switch data byte** (and `TILE_INVISIBLE_SWITCH`): `8421....` =
  switch effects index (0..0x1f, indexes `switch_effects_table` at
  `&4958`); `....84..` = toggle/set bits applied to each target
  object's data byte.
- **Sucking-nest data**: low 3 bits = type (0..7), selects target
  object + power; bit 3 = blow vs suck. See
  `gargoyles_projectile_type_table` and the sucking-nest table at
  `&4195`.
- **Hive data**: spawn target object type (low byte) + bit 6 = "less
  aggressive".
- **Pipe / nest data**: creature count `<<2`, plus `....0010` = "with
  bush" (decorative cosmetic), `....0001` = inactive.
- **Transporter data**: `..218...` destination index 0..f, `....8...`
  inactive, `8421....` required key, bits 0..1 toggle flags.
- **Engine data**: `....8...` inactive (must be cleared by a switch
  before the engine ignites).
- **Constant-wind data**: `8421....` y velocity, `....8421` x velocity
  (both signed nibbles). See `Wind::wind_vector_from_data_byte`.

## Tile-type abbreviations

| Short | Tile type | Type byte |
|---|---|---|
| `NEST` | TILE_NEST | 0x09 |
| `PIPE` | TILE_PIPE | 0x0a |
| `ISW` | TILE_INVISIBLE_SWITCH (pressure plate) | 0x00 |
| `XPORT` | TILE_TRANSPORTER | 0x01 |
| `ODATA` | TILE_SPACE_WITH_OBJECT_FROM_DATA | 0x02 |
| `MDOOR` | TILE_METAL_DOOR | 0x03 |
| `SDOOR` | TILE_STONE_DOOR | 0x04 |
| `OTYP_S` | TILE_STONE_HALF_WITH_OBJECT_FROM_TYPE | 0x05 |
| `OTYP` | TILE_SPACE_WITH_OBJECT_FROM_TYPE | 0x06 |
| `OTYP_G` | TILE_GREENERY_WITH_OBJECT_FROM_TYPE | 0x07 |
| `SW` | TILE_SWITCH | 0x08 |
| `ENG` | TILE_ENGINE | 0x0c |
| `WAT` | TILE_WATER | 0x0d |
| `CWIND` | TILE_CONSTANT_WIND | 0x0b |
| `LEAF` | TILE_POSSIBLE_LEAF | 0x11 |
| `MUSH` | TILE_MUSHROOMS | 0x0f |

Flip column reads `H` / `V` / `HV` (else `-`). Position is the first
landscape cell only when an entry covers multiple (count noted as
`+N`); the strip-style entries in range 8 list a column / row span.

## Range 0 (idx `&00-&1c`)

| # | Pos | Tile | Flip | Data | Type | Effect |
|---|---|---|---|---|---|---|
| 00 | (any) | NEST | H | 7c (31) | 0f WORM | spawner — anywhere worms emerge |
| 01 | (any) | NEST | H | 60 (24) | 27 MAGGOT | spawner — anywhere maggots emerge |
| 02 | (b0,4e) | NEST | H | 04 | 2e GREEN_YELLOW_BIRD | 1 creature, immediate |
| 03 | (ec,c0) | NEST | H | 88 | 07 GREEN_FROGMAN | 2 + bush |
| 04 | (77,54) | NEST | H | 88 | 2f WHITE_YELLOW_BIRD | 2 + bush |
| 05 | (64,94) | PIPE | H | a0 | 2d RED_CYAN_IMP | 8 + bush |
| 06 | (9a,80) | OTYP | - | a6 | 1f GREEN_WHITE_TURRET | fires ICER_BULLET |
| 07 | (af,61) | OTYP | HV | ae | 1f GREEN_WHITE_TURRET | fires RED_BULLET |
| 08 | (da,80) | OTYP | - | 83 | 0d SUCKING_NEST | type 3 (sucks coronium pwr 0x40) |
| 09 | (c6,c0) | OTYP | - | 86 | 0d SUCKING_NEST | type 6 (blows coronium pwr 0x7f) |
| 0a | (36,8c) | OTYP | V | 82 | 0d SUCKING_NEST | type 2 (sucks WASP pwr 0x7f) |
| 0b | (9f,c0) | OTYP_S | - | 80 | 0c DENSE_NEST | static |
| 0c | (2e,94) | OTYP_S | - | 80 | 60 MUSHROOM_IMMUNITY_PILL | pickup |
| 0d | (a9,9c) | ISW | - | ad (effects 0x15, set 0x02) | 2c CYAN_YELLOW_IMP | imp-triggered plate |
| 0e | (9c,3c) | MDOOR | HV | 01 | — | colour 0 (cyg), locked, h-door; toggled by SW@(9d,3b) |
| 0f | (83,77) | SDOOR | - | f7 | — | colour 7 (mwb), locked+open; toggled by ISW@(87,77),(7f,77),(83,76) |
| 10 | (88,72) | MDOOR | H | a1 | — | colour 2 (gyr), locked, v-door; toggled by SW@(46,56),(8b,71) |
| 11 | (5f,c0) | SDOOR | H | f1 | — | colour 7, locked, v-door |
| 12 | (57,94) | SDOOR | H | f7 | — | colour 7, locked, v-door; opened by SW@(4d,80) |
| 13 | (bf,80) | MDOOR | H | 81 | — | colour 0, locked, v-door; opened by ISW@(c1,7c) |
| 14 | (9d,3b) | SW | H | 0a (effects 0x01, toggle 0x01) | — | switch |
| 15 | (4d,80) | SW | V | ac (effects 0x05, toggle 0x02) | — | switch |
| 16 | (45,4e) | ODATA | - | d2 | — | RED_YELLOW_GREEN_KEY pickup |
| 17 | (81,75) | ODATA | - | df | — | FIRE_IMMUNITY_DEVICE pickup |
| 18 | (b3,80) | ODATA | V | d4 | — | YELLOW_WHITE_RED_KEY pickup |
| 19 | (3f,80) | ODATA | - | a3 | — | CYAN_CLAWED_ROBOT |
| 1a | (cb,d8) | STONE_HORIZONTAL_THREE_QUARTERS | V | — | — | scenery |
| 1b | (40,4e) | LEAF | - | — | — | scenery |
| 1c | (4c,80) | STONE_TWO | - | — | — | scenery |

## Range 1 (`&1d-&38`)

| # | Pos | Tile | Flip | Data | Type | Effect |
|---|---|---|---|---|---|---|
| 1d | (ca,58)+1 | OTYP | - | 84 | 0d SUCKING_NEST | type 4 (sucks PIRANHA pwr 0x50) |
| 1e | (2f,94) | OTYP | - | 85 | 0d SUCKING_NEST | type 5 (blows all pwr 0x7f) |
| 1f | (a7,80) | OTYP | HV | ae | 1f GREEN_WHITE_TURRET | fires RED_BULLET |
| 20 | (56,94) | OTYP | - | 80 | 0d SUCKING_NEST | type 0 (sucks all pwr 0x50) |
| 21 | (34,8c) | OTYP | V | 80 | 5c BLASTER | static |
| 22 | (e3,98) | OTYP | V | 88 | 0d SUCKING_NEST | type 8 (sucks WORM pwr 0x40) |
| 23 | (3b,c0) | OTYP_S | H | ac | 20 CYAN_RED_TURRET | fires BLUE_DEATH_BALL |
| 24 | (e4,80) | OTYP_S | H | c4 | 05 LARGE_HIVE | spawns WASP (less aggro) |
| 25 | (80,c5) | OTYP_G | H | c0 | 04 SMALL_HIVE | spawns PIRANHA (less aggro) |
| 26 | (e0,98) | NEST | H | 04 | 06 RED_FROGMAN | 1, immediate |
| 27 | (64,80) | NEST | H | a8 | 31 INVISIBLE_BIRD | 10 + bush |
| 28 | (37,8c) | OTYP_G | HV | c4 | 05 LARGE_HIVE | spawns WASP (less aggro) |
| 29 | (47,c0) | PIPE | - | bc | 2a RED_YELLOW_IMP | 15 + bush |
| 2a | (9f,3a) | ENG | H | 7d | — | inactive; activated by ISW@(9b,3b) |
| 2b | (9c,3d) | MDOOR | - | 01 | — | colour 0, locked, h-door; toggled by SW@(9d,3b) |
| 2c | (aa,98) | SDOOR | H | c1 | — | colour 4 (rmb), locked, v-door; opened by ISW@(a9,9c) |
| 2d | (9b,80) | MDOOR | H | d1 | — | colour 5 (rmr), locked, v-door |
| 2e | (9a,5c) | MDOOR | V | 91 | — | colour 1 (ryg), locked, v-door |
| 2f | (5e,c0) | SDOOR | H | f1 | — | colour 7, locked, v-door |
| 30 | (c7,c0) | SDOOR | H | f1 | — | colour 7, locked, v-door |
| 31 | (8a,71) | ODATA | - | da | — | PISTOL pickup |
| 32 | (60,98) | XPORT | - | f7 | — | dest &b, inactive, key 6 (BLUE_CYAN_GREEN) |
| 33 | (9d,49) | XPORT | V | f3 | — | dest &9, inactive, key 6 |
| 34 | (a2,58) | XPORT | - | d8 | — | dest &c, key 5; toggled c↔d by SW@(a1,58) |
| 35 | (b2,80) | XPORT | - | 88 | — | dest &4, key 3 (YELLOW_WHITE_RED) |
| 36 | (98,80) | EARTH_SLOPE_45 | - | — | — | scenery |
| 37 | (a9,80) | STONE_TWO | - | — | — | scenery |
| 38 | (db,80) | STONE_HORIZONTAL_THREE_QUARTERS | - | — | — | scenery |

## Range 2 (`&39-&56`)

| # | Pos | Tile | Flip | Data | Type | Effect |
|---|---|---|---|---|---|---|
| 39 | (28,98) | OTYP | V | 80 | 09 RED_SLIME | static creature |
| 3a | (29,98) | OTYP | V | 83 | 0d SUCKING_NEST | type 3 |
| 3b | (3c,80) | OTYP | - | 83 | 0d SUCKING_NEST | type 3 |
| 3c | (98,4e) | OTYP | HV | b0 | 1f GREEN_WHITE_TURRET | fires PISTOL_BULLET |
| 3d | (63,c0) | OTYP | - | aa | 20 CYAN_RED_TURRET | fires CANNONBALL |
| 3e | (cb,dc) | OTYP | - | 80 | 55 CORONIUM_BOULDER | |
| 3f | (61,c6) | OTYP | - | 80 | 55 CORONIUM_BOULDER | |
| 40 | (a3,c0) | OTYP | V | 87 | 0d SUCKING_NEST | type 7 (blows PIRANHA pwr 0x50) |
| 41 | (ce,d8) | OTYP | - | 80 | 63 RADIATION_IMMUNITY_PILL | |
| 42 | (e9,98) | NEST | - | 30 | 0f WORM | 12 |
| 43 | (80,88) | NEST | HV | 08 | 2e GREEN_YELLOW_BIRD | 2 |
| 44 | (2e,98) | NEST | H | 10 | 0a GREEN_SLIME | 4 |
| 45 | (4f,80) | PIPE | HV | 7c | 1b INVISIBLE_HOVERING_BALL | 31 |
| 46 | (79,76) | PIPE | H | 04 | 37 FIREBALL | 1 |
| 47 | (87,bf) | PIPE | H | 10 | 29 RED_MAGENTA_IMP | 4 |
| 48 | (b6,80) | PIPE | - | a8 | 1a HOVERING_BALL | 10 + bush |
| 49 | (97,5c) | PIPE | V | 90 | 1a HOVERING_BALL | 4 + bush |
| 4a | (2d,c7) | PIPE | H | 04 | 37 FIREBALL | 1 |
| 4b | (d6,72) | SDOOR | - | c1 | — | colour 4, locked, h-door; SW@(d5,73) toggles lock, SW@(d4,6f) toggles open |
| 4c | (5c,b8)+1 | SDOOR | V | f1 | — | colour 7, locked, v-door |
| 4d | (a0,63) | XPORT | V | e1 | — | dest &0, inactive, key 6 |
| 4e | (74,54) | XPORT | - | 95 | — | dest &a, inactive, key 3 |
| 4f | (6a,de) | SW | HV | bc (eff 0x07, tog 0x02) | — | switch |
| 50 | (a1,58) | SW | V | b4 (eff 0x06, tog 0x02) | — | switch |
| 51 | (9f,3b) | ENG | HV | 7d | — | inactive; activated by ISW@(9b,3b) |
| 52 | (89,72) | ODATA | H | a1 | — | HOVERING_ROBOT |
| 53 | (85,bf) | ODATA | - | d6 | — | RED_MAGENTA_RED_KEY |
| 54 | (6b,88) | ODATA | - | dd | — | PLASMA_GUN |
| 55 | (ae,98) | ODATA | - | e2 | — | WHISTLE_TWO |
| 56 | (65,b4) | STONE_TWO | - | — | — | scenery |

## Range 3 (`&57-&79`)

| # | Pos | Tile | Flip | Data | Type | Effect |
|---|---|---|---|---|---|---|
| 57 | (e2,c0) | NEST | H | 04 | 37 FIREBALL | 1 |
| 58 | (ed,bc) | NEST | - | 0c | 0a GREEN_SLIME | 3 |
| 59 | (80,54) | PIPE | - | 04 | 37 FIREBALL | 1 |
| 5a | (cd,7c) | PIPE | V | 20 | 4b POWER_POD | 8 |
| 5b | (a8,68) | PIPE | HV | 21 | 4b POWER_POD | 8, inactive; SW@(ab,6b) toggles, ISW@(a8,69) deactivates |
| 5c | (2b,80) | PIPE | V | a0 | 2d RED_CYAN_IMP | 8 + bush |
| 5d | (ab,80) | OTYP | H | b0 | 1f GREEN_WHITE_TURRET | fires PISTOL_BULLET |
| 5e | (9d,6f) | OTYP | V | ac | 20 CYAN_RED_TURRET | fires BLUE_DEATH; SW@(ab,6b) toggles |
| 5f | (62,c0) | OTYP | V | 83 | 0d SUCKING_NEST | type 3 |
| 60 | (e5,bc) | OTYP | V | 81 | 0d SUCKING_NEST | type 1 (blows H_STONE_DOOR pwr 0x30) |
| 61 | (70,88) | OTYP | H | 84 | 28 GARGOYLE | type 4 (LIGHTNING every 4 frames) |
| 62 | (ec,bc) | OTYP_S | V | 80 | 55 CORONIUM_BOULDER | |
| 63 | (83,5c) | OTYP_G | V | c4 | 05 LARGE_HIVE | spawns WASP (more aggro) |
| 64 | (c1,7c) | ISW | - | 85 (eff 0x10, set 0x02) | 80 ANY | pressure plate |
| 65 | (c6,7c) | ISW | - | 95 (eff 0x12, set 0x02) | 00 PLAYER | player-only plate |
| 66 | (67,da) | ISW | - | a3 (eff 0x14, set 0x01) | 80 ANY | |
| 67 | (eb,bc) | ISW | - | b5 (eff 0x16, set 0x02) | 80 ANY | |
| 68 | (2d,94) | SDOOR | H | f1 | — | colour 7, locked, v-door |
| 69 | (98,54) | MDOOR | HV | ad | — | colour 2, locked + slow, h-door |
| 6a | (aa,9c) | SDOOR | V | c1 | — | colour 4, locked, v-door; ISW@(a9,9c) opens |
| 6b | (cc,7c) | MDOOR | V | 81 | — | colour 0, locked, v-door; ISW@(c6,7c) opens |
| 6c | (a5,80) | MDOOR | V | 89 | — | colour 0, locked + slow, v-door |
| 6d | (9e,6b) | MDOOR | V | a0 | — | colour 2, unlocked, v-door |
| 6e | (a2,c0) | SDOOR | V | c1 | — | colour 4, locked, v-door; SW@(c4,c4) toggles lock |
| 6f | (d7,c0) | SDOOR | H | f1 | — | colour 7, locked, v-door |
| 70 | (e6,bc) | SDOOR | V | f1 | — | colour 7, locked, v-door; SW@(e3,9c) or (e3,bc) toggles open |
| 71 | (e7,bc) | SDOOR | - | c1 | — | colour 4, locked, h-door; ISW@(eb,bc) opens |
| 72 | (94,5c) | XPORT | - | 8c | — | dest &6, key 3 |
| 73 | (7c,c0) | SW | - | a4 (eff 0x04, tog 0x02) | — | switch |
| 74 | (e3,9c)+1 | SW | - | e4 (eff 0x0c, tog 0x02) | — | switch (two cells) |
| 75 | (45,c0) | ODATA | - | d7 | — | BLUE_CYAN_GREEN_KEY |
| 76 | (9b,66) | ODATA | - | 9d | — | RED_ROLLING_ROBOT |
| 77 | (9f,73) | ODATA | H | e1 | — | WHISTLE_ONE |
| 78 | (c2,7c)+1 | STONE_TWO | - | — | — | scenery |
| 79 | (71,88) | EARTH | - | — | — | scenery |

## Range 4 (`&7a-&9d`)

| # | Pos | Tile | Flip | Data | Type | Effect |
|---|---|---|---|---|---|---|
| 7a | (67,c8) | OTYP | V | a6 | 20 CYAN_RED_TURRET | fires ICER_BULLET |
| 7b | (4f,68)+1 | OTYP | V | 81 | 28 GARGOYLE | type 1 (PLASMA_BALL ev 8 frames) |
| 7c | (cf,b8) | OTYP_S | V | 85 | 0d SUCKING_NEST | type 5 |
| 7d | (d2,9d) | OTYP | V | 83 | 0d SUCKING_NEST | type 3 |
| 7e | (e2,a2)+1 | OTYP | HV | 83 | 28 GARGOYLE | type 3 (PLASMA_BALL ev 8 frames) |
| 7f | (7a,94)+1 | NEST | H | d0 | 27 MAGGOT | 20 + bush |
| 80 | (62,72) | NEST | HV | a8 | 31 INVISIBLE_BIRD | 10 + bush |
| 81 | (da,d8) | NEST | V | 04 | 0e BIG_FISH | 1 |
| 82 | (76,94)+1 | NEST | H | 04 | 08 INVISIBLE_FROGMAN | 1 |
| 83 | (b2,8d) | PIPE | HV | d0 | 11 WASP | 20 + bush |
| 84 | (66,66) | PIPE | HV | 88 | 39 MOVING_FIREBALL | 2 + bush |
| 85 | (d7,6e) | PIPE | H | 04 | 37 FIREBALL | 1 |
| 86 | (83,78) | PIPE | H | 04 | 37 FIREBALL | 1 |
| 87 | (84,6c) | PIPE | H | 04 | 37 FIREBALL | 1 |
| 88 | (80,75) | PIPE | - | 08 | 2a RED_YELLOW_IMP | 2 |
| 89 | (87,77) | ISW | - | bd (eff 0x17, set 0x02) | 80 ANY | |
| 8a | (9b,3b) | ISW | - | 8a (eff 0x11, clr 0x01) | 4a DESTINATOR | destinator-triggered (engine activator) |
| 8b | (50,60) | SDOOR | HV | f1 | — | colour 7, locked, h-door |
| 8c | (ae,62) | MDOOR | V | d1 | — | colour 5, locked, v-door; SW@(d5,73) toggles open |
| 8d | (64,c8) | SDOOR | - | f1 | — | colour 7, locked, h-door; SW@(67,cb) toggles open |
| 8e | (a3,69) | MDOOR | V | b1 | — | colour 3 (ywr), locked, v-door |
| 8f | (63,cc) | SDOOR | H | f1 | — | colour 7, locked, v-door |
| 90 | (b8,c5) | SDOOR | V | c1 | — | colour 4, locked, v-door; SW@(b8,c3) toggles, ISW@(b4,c2) opens |
| 91 | (7f,94)+1 | SDOOR | - | c1 | — | colour 4, locked, h-door; ISW@(80,c2) opens |
| 92 | (82,c3) | SDOOR | V | c1 | — | colour 4, locked, v-door; SW@(7c,c0) toggles open |
| 93 | (e0,b8) | SDOOR | H | c1 | — | colour 4, locked, v-door; SW@(e3,9c) or (e3,bc) toggles open |
| 94 | (9c,66) | XPORT | V | e2 | — | dest &1, key 6 |
| 95 | (61,d9) | XPORT | - | e4 | — | dest &2, key 6; ISW@(67,da) deactivates |
| 96 | (9d,58) | XPORT | - | dc | — | dest &e, key 5; toggled e↔f by SW@(a1,58) |
| 97 | (29,c6) | XPORT | - | a0 | — | dest &0, key 4 (no key); toggled 0↔1 + on/off by SW@(29,c8) |
| 98 | (46,56) | SW | HV | c2 (eff 0x08, tog 0x01) | — | switch |
| 99 | (9f,6b) | ODATA | V | cb | — | POWER_POD |
| 9a | (9a,66) | ODATA | H | b8 | — | INACTIVE_CHATTER |
| 9b | (74,94) | STONE_HORIZONTAL_THREE_QUARTERS | - | — | — | scenery |
| 9c | (75,94)+1 | LEAF | - | — | — | scenery |
| 9d | (77,94) | STONE_HORIZONTAL_THREE_QUARTERS | - | — | — | scenery |

## Range 5 (`&9e-&bb`)

| # | Pos | Tile | Flip | Data | Type | Effect |
|---|---|---|---|---|---|---|
| 9e | (b2,c2) | NEST | H | a8 | 10 PIRANHA | 10 + bush |
| 9f | (e4,b4) | NEST | HV | 10 | 2f WHITE_YELLOW_BIRD | 4 |
| a0 | (62,a2) | PIPE | HV | 98 | 30 RED_MAGENTA_BIRD | 6 + bush |
| a1 | (63,b5) | PIPE | H | a0 | 30 RED_MAGENTA_BIRD | 8 + bush |
| a2 | (82,bf) | OTYP | HV | 80 | 09 RED_SLIME | static creature |
| a3 | (61,c7) | OTYP | - | 83 | 0d SUCKING_NEST | type 3 |
| a4 | (d4,bf) | OTYP | HV | 80 | 09 RED_SLIME | |
| a5 | (d3,be) | OTYP | HV | 80 | 09 RED_SLIME | |
| a6 | (77,aa) | OTYP | - | 80 | 4f CANNON_CONTROL_DEVICE | static |
| a7 | (2e,d6) | OTYP | - | 80 | 24 GREEN_CLAWED_ROBOT | static |
| a8 | (64,d6) | OTYP_S | H | 00 | 4a DESTINATOR | the destinator's home in Triax's lab |
| a9 | (86,56) | OTYP_G | V | c4 | 04 SMALL_HIVE | spawns WASP (more aggro) |
| aa | (a5,6b)+1 | PIPE | V | 40 | 1a HOVERING_BALL | 16 |
| ab | (a0,bf) | PIPE | HV | 84 | 39 MOVING_FIREBALL | 1 + bush |
| ac | (d1,d3) | PIPE | H | 28 | 10 PIRANHA | 10 |
| ad | (b4,c2) | ISW | - | 75 (eff 0x0e, set 0x02) | 00 PLAYER | |
| ae | (7f,77) | ISW | - | bc (eff 0x17, clr 0x02) | 4c EMPTY_FLASK | |
| af | (a3,63) | SDOOR | V | f1 | — | colour 7, locked, v-door |
| b0 | (9f,71) | MDOOR | V | d1 | — | colour 5, locked, v-door |
| b1 | (99,4c) | MDOOR | HV | a9 | — | colour 2, locked + slow, h-door |
| b2 | (80,77) | SDOOR | V | f1 | — | colour 7, locked, v-door; ISW@(87,77) opens, ISW@(7f,77),(83,76) close |
| b3 | (67,ce) | SDOOR | V | c0 | — | colour 4, unlocked, v-door |
| b4 | (da,6d) | SDOOR | - | c1 | — | colour 4, locked, h-door; SW@(d5,73) tog lock, SW@(d4,6f) tog open |
| b5 | (89,71) | XPORT | V | 8f | — | dest &7, inactive, key 3; SW@(46,56) or (8b,71) toggles |
| b6 | (95,5d) | SW | HV | 94 (eff 0x02, tog 0x02) | — | switch |
| b7 | (8b,71) | SW | H | c2 (eff 0x08, tog 0x01) | — | switch |
| b8 | (ab,6b) | SW | HV | ca (eff 0x09, tog 0x01) | — | switch |
| b9 | (c4,c4) | SW | HV | fa (eff 0x0f, tog 0x01) | — | switch |
| ba | (9d,5d) | ODATA | - | 9c | — | MAGENTA_ROLLING_ROBOT |
| bb | (aa,61) | ENG | H | fe | — | inactive; SW@(d5,73) toggles |

## Range 6 (`&bc-&d7`)

| # | Pos | Tile | Flip | Data | Type | Effect |
|---|---|---|---|---|---|---|
| bc | (bb,c3) | NEST | V | 10 | 0a GREEN_SLIME | 4 |
| bd | (47,59) | NEST | H | 14 | 2f WHITE_YELLOW_BIRD | 5 |
| be | (8a,78) | PIPE | - | 90 | 29 RED_MAGENTA_IMP | 4 + bush |
| bf | (a7,9a) | PIPE | - | 98 | 2c CYAN_YELLOW_IMP | 6 + bush |
| c0 | (61,d7) | PIPE | H | 04 | 37 FIREBALL | 1 |
| c1 | (9e,51) | OTYP | HV | a4 | 20 CYAN_RED_TURRET | fires ACTIVE_GRENADE |
| c2 | (2e,c8) | OTYP | - | 80 | 3a GIANT_BLOCK | static |
| c3 | (d6,a1) | OTYP | - | 83 | 0d SUCKING_NEST | type 3 |
| c4 | (7e,76) | OTYP_G | V | c6 | 05 LARGE_HIVE | spawns WASP, inactive; ISW@(87,77) deactivates, (7f,77),(83,76) activate |
| c5 | (da,6e) | OTYP_G | H | c4 | 05 LARGE_HIVE | spawns WASP (less aggro) |
| c6 | (aa,62) | ENG | HV | fe | — | inactive; SW@(d5,73) toggles |
| c7 | (ab,69) | XPORT | V | aa | — | dest &5, key 4 (no key); SW@(ab,6b) toggles 5↔4 |
| c8 | (45,57) | XPORT | - | 90 | — | dest &8, key 3; SW@(46,56) or (8b,71) toggles 8↔9 |
| c9 | (67,cb) | SW | - | ec (eff 0x0d, tog 0x02) | — | switch |
| ca | (d4,6f) | SW | - | dc (eff 0x0b, tog 0x02) | — | switch |
| cb | (29,c8) | SW | - | 9e (eff 0x03, tog 0x03) | — | switch |
| cc | (b8,c3) | SW | - | f4 (eff 0x0e, tog 0x02) | — | switch |
| cd | (6b,e1) | SDOOR | HV | f7 | — | colour 7, locked + open, h-door; SW@(6a,de) toggles |
| ce | (69,de) | SDOOR | H | f1 | — | colour 7, locked, v-door |
| cf | (9d,56) | MDOOR | V | f1 | — | colour 7, locked, v-door |
| d0 | (94,5f) | MDOOR | V | 81 | — | colour 0, locked, v-door; SW@(95,5d) toggles open |
| d1 | (63,ca) | SDOOR | H | f1 | — | colour 7, locked, v-door |
| d2 | (b4,c3) | SDOOR | - | f1 | — | colour 7, locked, h-door; SW@(b8,c3) toggles, ISW@(b4,c2) opens |
| d3 | (a1,6b) | MDOOR | H | b1 | — | colour 3, locked, v-door |
| d4 | (9f,57) | ODATA | H | db | — | ICER |
| d5 | (a0,6b) | ODATA | H | 9e | — | BLUE_ROLLING_ROBOT |
| d6 | (57,69) | WAT | - | — | — | water (top-of-pond override) |
| d7 | (e1,73) | WAT | - | — | — | water |

## Range 7 (`&d8-&f5`)

| # | Pos | Tile | Flip | Data | Type | Effect |
|---|---|---|---|---|---|---|
| d8 | (7f,c1) | OTYP | V | 84 | 0d SUCKING_NEST | type 4 |
| d9 | (a6,69) | OTYP | - | ac | 20 CYAN_RED_TURRET | fires BLUE_DEATH; SW@(ab,6b) toggles |
| da | (b4,c5) | OTYP | - | 80 | 0d SUCKING_NEST | type 0 |
| db | (53,95)+1 | OTYP | - | 80 | 0d SUCKING_NEST | type 0 |
| dc | (61,d8) | OTYP | - | 80 | 48 MAGGOT_MACHINE | |
| dd | (d4,73) | OTYP_S | V | 80 | 51 CYAN_YELLOW_GREEN_KEY | |
| de | (82,c5) | OTYP_S | V | 80 | 0c DENSE_NEST | |
| df | (e3,b5) | OTYP_S | V | 80 | 55 CORONIUM_BOULDER | |
| e0 | (75,87) | OTYP | - | 80 | 22 MAGENTA_CLAWED_ROBOT | static |
| e1 | (c3,c5) | OTYP_G | - | c0 | 04 SMALL_HIVE | spawns PIRANHA (more aggro) |
| e2 | (84,70) | NEST | H | 04 | 2e GREEN_YELLOW_BIRD | 1 |
| e3 | (9e,69) | NEST | - | 08 | 2f WHITE_YELLOW_BIRD | 2 |
| e4 | (c6,be) | PIPE | H | 90 | 2b BLUE_CYAN_IMP | 4 + bush |
| e5 | (64,c6) | PIPE | V | a2 | 2a RED_YELLOW_IMP | 8 + bush, inactive; SW@(67,cb) toggles |
| e6 | (a2,5b) | PIPE | V | 04 | 21 HOVERING_ROBOT | 1 |
| e7 | (28,d8) | PIPE | HV | 04 | 02 CREW_MEMBER | 1 |
| e8 | (29,d8) | PIPE | HV | 04 | 02 CREW_MEMBER | 1 |
| e9 | (9d,5b) | PIPE | V | 20 | 1a HOVERING_BALL | 8 |
| ea | (83,76) | ISW | - | bc (eff 0x17, clr 0x02) | 80 ANY | |
| eb | (a8,69) | ISW | - | 53 (eff 0x0a, set 0x01) | 4b POWER_POD | power-pod plate |
| ec | (80,c2) | ISW | - | 9d (eff 0x13, set 0x02) | 80 ANY | |
| ed | (aa,63) | SW | V | 84 (eff 0x00, tog 0x02) | — | switch |
| ee | (d5,73) | SW | - | da (eff 0x0b, tog 0x01) | — | switch |
| ef | (a0,67) | ODATA | H | cb | — | POWER_POD |
| f0 | (9f,51) | ODATA | H | de | — | PROTECTION_SUIT |
| f1 | (d6,73) | ODATA | H | c5 | — | BOULDER |
| f2 | (62,cc) | ODATA | - | a5 | — | RED_CLAWED_ROBOT |
| f3 | (69,d1) | SDOOR | HV | c1 | — | colour 4, locked, h-door; SW@(67,cb) toggles open |
| f4 | (2c,d6) | SDOOR | HV | f1 | — | colour 7, locked, h-door; SW@(29,c8) toggles lock+open |
| f5 | (a5,64)+1 | CWIND | - | 70 | — | constant wind: down @ 0x70 |

## Range 8 (`&f6-&fd`) — strip-style entries

These entries cover **multiple cells** (column-strips of decoration / wind), not single positions.

| # | Strip | Tile | Data | Effect | Cell count |
|---|---|---|---|---|---|
| f6 | x=b8, y=60..7f, aa..c0, c6..c8, ce..e1 | CWIND | d0 | wind up @ 0x20 | 78 |
| f7 | x=b9, y=60..c0, c6..c8, cd..e0 | CWIND | 80 | wind up @ 0x80 | 121 |
| f8 | x=d9, y=57..6b, 70..80, 85..88, 8d..a0, b3..e8 | LEAF (HV) | — | yellow / white leaves | 116 |
| f9 | x=59, y=52, 63..6f, 72..92, a3..c4 | LEAF (H) | — | green leaves | 81 |
| fa | x=79, y=53..73, 78..82, 93..ba | LEAF (HV) | — | yellow / white leaves | 84 |
| fb | x=39, y=80..a2, b3..c5, ca..cc | LEAF (HV) | — | yellow / white leaves | 57 |
| fc | x=48, y=50..52, 58..5b, 6c..9b | LEAF (H) | — | green leaves | 55 |
| fd | x=e8, y=80..9f | LEAF (H) | — | green leaves | 32 |

## ROM byte arrays

The four parallel arrays the disassembly walks. Address-of-entry-N is
the listed base + N.

| Array | Base | Notes |
|---|---|---|
| `tertiary_objects_x` | `&05ef` | tile-x for each entry; for strip entries this is the column |
| `tertiary_objects_tile_and_flip` | `&06ee` | tile type byte + flip flags |
| `tertiary_objects_data` | `&0986` | data byte (door / switch / spawner state) |
| `tertiary_objects_type` | `&0a71` | object type byte (only meaningful for `OTYP*`/`ODATA`) |

The data and type arrays are smaller than the x/tile arrays — they
only cover the entries where the field is meaningful. The `&05dd`
(`tertiary_objects_data_offset`) and `&05e6` (`tertiary_objects_type_offset`)
9-entry tables give the per-range bias added to the entry index to get
the array index. Our port replicates this with `tertiary_data_offset[]`
and `tertiary_type_offset[]` in `src/objects/object_tables.h`.
