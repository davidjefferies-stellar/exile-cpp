# Exile save game format

Reverse-engineered from `exile-disk-protection-and-supervisor-disassembly.txt`.
The supervisor (the menu / options shell that wraps the main game binary)
owns saving and loading; the main game binary just reads/writes the
decrypted state in place.

## File-level

- **Size:** exactly `0x400` bytes (1024). The loader rejects anything
  else (`&2ab2-&2ac1`).
- **OSFILE block:** `start=&ffff0400, end=&ffff0800` (`save_file_block`
  at `&2b6c`). The save is a raw dump of supervisor memory at
  `&0400..&0800`, i.e. the `encrypted_game_state` region.
- **OSFILE function 0x00** to save (`save_position_screen` at `&2a65`),
  function `0x05` (probe length) + `0xff` (load) on load
  (`load_position_screen` at `&2a8f`).
- **Filename:** entered interactively at the "Save Filename?" /
  "Load Filename?" prompt (`&2b1d`, `&2b34`); pointer at `&3257`.

## Encryption

On-disk bytes are XOR-streamed by `decrypt_temporary_copy_of_game_state`
at `&2f80`. The cipher is symmetric (XOR), so the same loop runs
forwards for both encrypt and decrypt:

```
SED                           ; BCD arithmetic on the ADCs below
key = 0x6e                    ; &0b
A   = 0x92
for Y = 0..0x37d:             ; size_low/high = &fc82 = 0x10000 - 0x37e
    A   = (A + key)  BCD      ; ADC &0b
    A   = (A + 0x15) BCD      ; ADC #&15
    key = A                   ; STA &0b
    decrypted = A XOR encrypted[&0400 + Y]
    plain[&1a20 + Y] = decrypted
    A = decrypted XOR key     ; EOR &0b -- feeds next iteration
CLD
```

Only the first `0x37e` bytes (894) are run through the cipher; the
trailing `0x82` bytes of the 0x400 file are page-align padding and are
written/read as-is.

## Decrypted layout (`&1a20..&1d9d`)

This is the byte-for-byte field map of the decrypted save. Offsets are
from the start of the file.

| Offset | 6502 addr | Field                                                 | Size |
|--------|-----------|-------------------------------------------------------|------|
| 0x000  | &1a20     | `rnd_state`                                           | 4    |
| 0x004  | &1a24     | `player_object_held`                                  | 1    |
| 0x005  | &1a25     | `player_angle`                                        | 1    |
| 0x006  | &1a26     | `player_facing`                                       | 1    |
| 0x007  | &1a27     | `game_time`                                           | 4    |
| 0x00b  | &1a2b     | `player_deaths`                                       | 3    |
| 0x00e  | &1a2e     | `player_keys_collected` [8]                           | 8    |
| 0x016  | &1a36     | `player_weapons_collected` [6] (jetpack..suit)        | 6    |
| 0x01c  | &1a3c     | fire / mushroom / whistle1 / whistle2 / radiation     | 5    |
| 0x021  | &1a41     | `door_timer`                                          | 1    |
| 0x022  | &1a42     | `player_mushroom_timers` (red, blue)                  | 2    |
| 0x024  | &1a44     | `chatter_energy_reserve`                              | 1    |
| 0x025  | &1a45     | `explosion_timer`                                     | 1    |
| 0x026  | &1a46     | `flooding_state`                                      | 1    |
| 0x027  | &1a47     | `earthquake_state`                                    | 1    |
| 0x028  | &1a48     | unused (0xff)                                         | 1    |
| 0x029  | &1a49     | `player_next_teleport`                                | 1    |
| 0x02a  | &1a4a     | `player_teleports_remembered`                         | 1    |
| 0x02b  | &1a4b     | `player_teleports_x` [5]                              | 5    |
| 0x030  | &1a50     | `player_teleports_y` [5]                              | 5    |
| 0x035  | &1a55     | `copy_protection_third_byte`                          | 1    |
| 0x036  | &1a56     | `waterline_x_ranges_y_fraction` [4]                   | 4    |
| 0x03a  | &1a5a     | `waterline_x_ranges_y` [4]                            | 4    |
| 0x03e  | &1a5e     | `waterline_x_ranges_desired_y` [4]                    | 4    |
| 0x042  | &1a62     | `imp_types_gifts_remaining` [5]                       | 5    |
| 0x047  | &1a67     | `clawed_robots_availability` [4]                      | 4    |
| 0x04b  | &1a6b     | `clawed_robots_teleporting_energy` [4]                | 4    |
| 0x04f  | &1a6f     | `player_pockets_used`                                 | 1    |
| 0x050  | &1a70     | `player_pockets` [5]                                  | 5    |
| 0x055  | &1a75     | `player_weapon`                                       | 1    |
| 0x056  | &1a76     | `player_weapons_energy_low` [6]                       | 6    |
| 0x05c  | &1a7c     | `player_weapons_energy_high` [6]                      | 6    |
| 0x062  | &1a82     | `weapons_energy_cost` [6]                             | 6    |
| 0x068  | &1a88     | `objects_type` [16]                                   | 16   |
| 0x078  | &1a98     | `objects_sprite` [16]                                 | 16   |
| 0x088  | &1aa8     | `objects_x_fraction` [17]                             | 17   |
| 0x099  | &1ab9     | `objects_x` [18]                                      | 18   |
| 0x0ab  | &1acb     | `objects_y_fraction` [17]                             | 17   |
| 0x0bc  | &1adc     | `objects_y` [18]                                      | 18   |
| 0x0ce  | &1aee     | `objects_flags` [16]                                  | 16   |
| 0x0de  | &1afe     | `objects_palette` [16]                                | 16   |
| 0x0ee  | &1b0e     | `objects_velocity_x` [16]                             | 16   |
| 0x0fe  | &1b1e     | `objects_velocity_y` [16]                             | 16   |
| 0x10e  | &1b2e     | `objects_target_object_and_flags` [16]                | 16   |
| 0x11e  | &1b3e     | `objects_tx` [16]                                     | 16   |
| 0x12e  | &1b4e     | `objects_energy` [16]                                 | 16   |
| 0x13e  | &1b5e     | `objects_ty` [16]                                     | 16   |
| 0x14e  | &1b6e     | `objects_touching` [16]                               | 16   |
| 0x15e  | &1b7e     | `objects_timer` [16]                                  | 16   |
| 0x16e  | &1b8e     | `objects_tertiary_data_offset` [16]                   | 16   |
| 0x17e  | &1b9e     | `objects_state` [16]                                  | 16   |
| 0x18e  | &1bae     | `tertiary_objects_data` (235 bytes)                   | 0xeb |
| 0x279  | &1c99     | `tertiary_objects_type` (129 bytes)                   | 0x81 |
| 0x2fa  | &1d1a     | `secondary_objects_x` [32]                            | 32   |
| 0x31a  | &1d3a     | `secondary_objects_y` [32]                            | 32   |
| 0x33a  | &1d5a     | `secondary_objects_type` [32]                         | 32   |
| 0x35a  | &1d7a     | **`game_state_checksum_one`**                         | 1    |
| 0x35b  | &1d7b     | `secondary_objects_energy_and_x_y_fractions` [32]     | 32   |
| 0x37b  | &1d9b     | `secondary_object_update_next_object`                 | 1    |
| 0x37c  | &1d9c     | `secondary_object_update_random_shuffle`              | 1    |
| 0x37d  | &1d9d     | **`game_state_checksum_two`**                         | 1    |
| 0x37e..0x3ff | —   | padding (not run through the cipher)                  | 0x82 |

Total of the cipher-covered region: `0x37e` (894) bytes.

## Field notes

- `secondary_objects_energy_and_x_y_fractions` packs three values per
  byte:
  - bits 7-4: energy high bits (`*= 0x10` to unpack)
  - bits 3-2: x fraction (`*= 0x40` to unpack)
  - bits 1-0: y fraction (`*= 0x40` to unpack)
- `objects_x` / `objects_y` are **18 entries**; the other object arrays
  are 16. The two extra bytes spill out of the 16-slot loop in the
  original — preserve them byte-for-byte if you want a bit-exact save.
- `objects_x_fraction` / `objects_y_fraction` are **17 entries**, same
  misalignment.
- `player_teleports_x[4]` / `player_teleports_y[4]` are the spawn
  fallback. The loader hard-checks them at `&2fbc-&2fc1`: if `[4]` isn't
  `0x99` / `0x3c` it rejects the position and resets to the default
  state (`copy_default_position` at `&31ed`).
- `player_weapons_energy_low/high` are split because BBC zero-page
  storage holds the 16-bit energy as two arrays of bytes. Our port
  stores `uint16_t` per weapon — pack/unpack on the boundary.
- `weapons_energy_cost` is saved even though it's effectively constant
  (`01 06 10 ff 32 00`). It's part of the snapshot the supervisor took.

## Checksums

Two are embedded inline:

- `game_state_checksum_one` (offset `0x35a`, addr `&1d7a`). Verified at
  `&2fda-&2fee`: XOR all bytes in `[&1a20, &1d79]` (the whole header up
  to but not including the checksum). If the running XOR doesn't equal
  the stored byte the load is rejected and `copy_default_position`
  restores the default.
- `game_state_checksum_two` (offset `0x37d`, addr `&1d9d`). Disassembly
  shows it stored but I haven't traced its verifier — it most likely
  covers `[&1d7b, &1d9c]` (the secondary-object tail), mirroring
  checksum_one's coverage of the header.

## Default position

`encrypted_default_game_state` at `&3b52` is a 0x400-byte template
copied into `&0400` by `copy_default_position` (`&31ed`). It is the
"starting from the surface" save the game falls back to when no file is
loaded or a load fails. Its decrypted form should match the initial
values in this disassembly's `temporary_copy_of_game_state` listing
starting at `&1a20`.

## Memory map context

- The supervisor keeps the canonical save at `&0400..&0800` in its own
  address space. This is **encrypted**.
- `decrypt_temporary_copy_of_game_state` writes the plain form to
  `&1a20..&1d9d` for in-supervisor reads (score display, position
  preview).
- At relocate-into-game time (`relocate_binary_and_saved_position` at
  `&74e0`) the encrypted save is moved around; the main game binary
  also lives at `&1a20+` (`temporary_copy_of_game_state` *is* the game
  state in the game binary's address space), so the running game reads
  and writes the same bytes the supervisor sees after decryption.

