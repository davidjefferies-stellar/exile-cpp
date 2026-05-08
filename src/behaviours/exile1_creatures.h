#pragma once
#include "objects/object.h"
#include "behaviours/creature.h"  // for UpdateContext

// Pre-release-only creatures recovered from EXILE1 (8-March-1988 master
// disk). Both routines are short and self-contained — see
// `exile-EXILE1-disassembly.txt` at the repo root for the full
// 6502 source they were ported from, plus
// `resources/.../sprite_extract/dog_crab_sprites.h` for the sprite
// pixel data.
//
// Wiring required to use these:
//   1. Add OBJECT_DOG and OBJECT_CRAB to `core/types.h::ObjectType`.
//   2. Add their names to `rendering/debug_names.h`.
//   3. Slot the dog/crab sprites into the atlas (extending
//      BBC_SPRITE_DATA from 128x81 to 128x96 OR allocating a new sprite
//      sheet — see FINDINGS.md for size/position info).
//   4. Add update_dog / update_crab to `behavior_dispatch.cpp` at the
//      DOG and CRAB enum slots.
//   5. Add palette + flags entries in `objects/object_data.h` so the
//      object renders with the right colour and physics.

namespace Behaviors {

// EXILE1 OBJECT_DOG (type &1a) — runtime &3cb5
//
// Original 6502 (18 bytes):
//   JSR &28ff            ; npc_setup_helper (cancel gravity, mood update)
//   BIT &be              ; check target flags
//   BPL .no_target       ; bit 7 clear → no target
//   LDY &35              ; Y = target heading
//   BNE .have_heading
// .no_target:
//   LDY &24              ; Y = default heading
// .have_heading:
//   LDA #&c0             ; A = walking speed / mood byte
//   JMP &39e3            ; tail-call shared NPC walker (sprite-flip + plot)
//
// Behaviour: simple ground-walking NPC that homes in on the player when
// it has a target, otherwise walks in the default heading.
// Closest standard-release analogue: `update_imp` (without the gift /
// climbing / jumping branches).
void update_dog(Object& obj, UpdateContext& ctx);

// EXILE1 OBJECT_CRAB (type &26) — runtime &3e67
//
// Original 6502 (52 bytes):
//   JSR &28db            ; apply 16-bit velocity → position
//   AND #&7f             ; mask sign of returned status
//   BNE .leave           ; non-zero → physics blocked, leave
//   LDA #&09
//   JSR &1b41            ; Y = count_objects_of_type(9)
//   CPY #&04
//   BCS .leave           ; >= 4 already, throttle
//   LDA #&09
//   JSR &a00e            ; sideways: find_free_slot_for_type(9)
//   BCS .leave           ; no slot
//   TXA; AND #&03; TAX   ; X = random 0..3
//   LDA spawn_table,X    ; pick one of {0x22, 0x72, 0x5a, 0x1f}
//   STA objects[Y].field_06b
//   JSR &26c5            ; copy crab's (x, y) into new object's slot
//   LDA #&40
//   STA objects[Y].field_0609   ; data byte = 0x40
//   LDA #&ff
//   BIT &26              ; N = bit 7 of zp_26 (rng_or_sign source)
//   JSR &1597            ; conditional_negate_A → +1 or -1
//   STA objects[Y].field_0632   ; initial timer / velocity
// .leave:
//   RTS
//
// Behaviour: stationary creature that periodically spawns one of 4
// projectile types (seeded with a random ±1 initial timer/velocity).
// Closest standard-release analogue: `update_gargoyle` (stationary
// emitter, throttle by spawn count).
void update_crab(Object& obj, UpdateContext& ctx);

}  // namespace Behaviors
