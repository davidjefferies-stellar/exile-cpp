# The Triax / destinator intro

Game start: Triax appears in the player's spaceship, grabs the
destinator, teleports out. There's no scripted cutscene — it falls out
of the regular update loop running over the ROM's pre-populated object
tables. `&xxxx` addresses refer to `exile-standard-disassembly.txt`.

## Initial state

| Slot                | Position     | Type                | 6502 source       |
|---------------------|--------------|---------------------|-------------------|
| Primary 1           | (0x99, 0x3b) | TRIAX (0x26)        | `&08b4` / `&08a3` |
| Secondary 0x10      | (0x99, 0x3c) | DESTINATOR (0x4a)   | `&0af2` / `&0b12` |
| Tertiary &9d        | (0x64, 0xd6) | DESTINATOR home     | `&0a23`           |

Player spawns at (0x9b, 0x3b). Tertiary `&9d` bit 7 = "needs primary
spawning"; setting it re-arms the destinator at Triax's lab.

## Per-frame flow

1. **Promote secondaries.** 6502 redraw sets `secondary_object_update_
   mode` bit 7 (`&15ce`); the next `consider_promoting_secondary_objects`
   (`&0be8`) takes the full-sweep path at `&0c4e`. Port equivalent:
   `Game::init` calls `promote_distance_check()` once, since the
   per-frame `promote_selective` only walks one random slot.

2. **Refresh touching.** 6502 calls `check_for_collisions` (`&2a64`)
   from per-object update at `&1b54`, *before* the type dispatch.
   `update_triax` then reads `&3b this_object_touching` (`&4704`). Port:
   Step 9b in `Game::update_objects` writes `obj.touching` ahead of the
   type dispatch.

3. **Triax falls.** ROM spawn (frac `(0x64, 0x20)`) overlaps the ship-roof
   tile. 6502's `apply_tile_collision_to_position_and_velocity` (`&306c`)
   pushes objects out of obstructions ("Always try to move out of
   obstruction" at `&308a`). Port stand-in: integration skips the
   position revert when the object **starts** the tick inside solid, so
   gravity walks Triax out a frac per frame until he clears the roof.

4. **Absorb + teleport away.** Once AABBs overlap, `update_triax`
   matches `touching` against DESTINATOR via
   `consider_absorbing_object_touched` (`&3be1`), stores `&80` to
   `tertiary_objects_data + &9d`, then jumps to `make_triax_teleport_
   away` (`&475c`): sets `ty = 0`, arms TELEPORTING. The port's
   `update_triax` (`src/behaviours/creature.cpp`) does the same plus
   marks the destinator `PENDING_REMOVAL`.

5. **Despawn.** Teleport handler counts `timer` down (6502 `&1bfd-&1c44`,
   port Step 8); at `ty = 0` Triax leaves the world. The destinator's
   primary is reaped at the PENDING_REMOVAL pass. Its tertiary `&9d`
   now has bit 7 set, so the lab respawns it the first time the player
   visits.

