#include "behaviours/exile1_creatures.h"
#include "behaviours/mood.h"
#include "behaviours/npc_helpers.h"
#include "core/types.h"
#include <cstdlib>

// Pre-release-only creature behaviours recovered from EXILE1's
// disassembly (see `exile-EXILE1-disassembly.txt`, sections "Dog
// update routine" and "Crab update routine"). Both routines were short
// in 6502 (18 / 52 bytes) and use existing engine helpers; the C++
// ports below mirror that structure using the equivalent NPC:: helpers
// already in this codebase.

namespace Behaviors {

// =============================================================================
// EXILE1 update_dog                                            (runtime &3cb5)
//
// 6502 source — eight instructions:
//   JSR &28ff           ; npc_setup_helper (mood + globals)
//   BIT &be             ; check target flags (sign bit)
//   BPL .no_target      ; no target → use default
//   LDY &35             ; Y = target heading
//   BNE .have_heading   ; heading non-zero → use it
// .no_target:
//   LDY &24             ; Y = default heading
// .have_heading:
//   LDA #&c0            ; A = walking_speed | mood byte
//   JMP &39e3           ; tail-call shared NPC walker
//
// In our engine the equivalent is: update mood, pick a target (player
// when available), seek with a moderate walk speed. Closest analogue
// already in the project is `update_imp` (ground walker that homes the
// player) — copying the simple parts of it verbatim.
// =============================================================================
void update_dog(Object& obj, UpdateContext& ctx) {
    // 6502 &28ff "npc_setup_helper" expanded — mood update + every-N-
    // frames stimulus check. Our Mood::update_mood is the C++-side
    // equivalent of the EXILE1 helper; gravity behaves naturally for a
    // ground walker so no cancel_gravity call is needed (unlike the
    // chatter / wasp).
    Mood::update_mood(obj, ctx);

    // 6502 BIT &be / BPL .no_target picks between the target heading
    // (zp &35) or default heading (zp &24), but EITHER WAY the JMP
    // &39e3 below is unconditional — the dog always walks. The C++
    // engine doesn't yet have a "default heading" for un-targeted
    // creatures, so seek the player every frame regardless of
    // target_and_flags. (The 6502's `npc_setup_helper` at &28ff is
    // what populates the target heading; until that helper is ported,
    // bypassing the gate keeps the dog mobile.)
    const Object& player = ctx.mgr.player();
    NPC::seek_player(obj, player, /*speed=*/4);

    // 6502 JMP &39e3 — shared NPC walker. The walker plots the sprite
    // and handles flip-to-match-velocity. Two helper calls cover that:
    NPC::consider_face_movement_direction(obj, ctx.rng);
    // 6502 sprite_animate_helper @ &39f1 derives the frame from the
    // input A (= &c0 for dog), the facing-flag at zp &91, AND the
    // walking-state at zp &35 — the full computation involves several
    // helpers (&2922, &1597) and writes results back into zp &24/&26/&90.
    // For our port: cycle the two walk frames while moving (every 8
    // frames), freeze on frame 0 when stationary. Sprite-flip for
    // direction is handled separately by consider_face_movement_direction
    // above.
    bool moving = (obj.velocity_x != 0) || (obj.velocity_y != 0);
    obj.sprite = moving ? (0x7d + ((ctx.frame_counter >> 3) & 0x01))
                        : 0x7d;

    // 6502 dispatch ends with RTS via stacked-jump; nothing further.
}


// =============================================================================
// EXILE1 update_crab                                           (runtime &3e67)
//
// 6502 source — 17 instructions, 52 bytes:
//   JSR &28db           ; apply velocity → position (16-bit add)
//   AND #&7f            ; mask sign of returned status
//   BNE .leave          ; movement blocked → leave
//   LDA #&09
//   JSR &1b41           ; Y = count_objects_of_type(9)
//   CPY #&04
//   BCS .leave          ; >= 4 already, throttle
//   LDA #&09
//   JSR &a00e           ; sideways: find_free_slot_for_type(9)
//   BCS .leave          ; no slot
//   TXA; AND #&03; TAX  ; X = random 0..3
//   LDA spawn_table,X   ; pick one of {0x22, 0x72, 0x5a, 0x1f}
//   STA objects[Y].field_06b
//   JSR &26c5           ; copy crab's pos into new object
//   LDA #&40
//   STA objects[Y].field_0609   ; data byte = &40
//   LDA #&ff
//   BIT &26
//   JSR &1597           ; conditional negate: ±1 from random sign
//   STA objects[Y].field_0632   ; initial timer / velocity
// .leave: RTS
//
// Behaviour: stationary; periodically spawns one of 4 projectile types
// at its position with a random ±1 initial timer/velocity. Throttled
// to keep at most 4 of "type 9" alive. Closest analogue: `update_
// gargoyle` (15 lines, also stationary + emit-on-timer).
// =============================================================================

// CRAB_SPAWN_INIT_VALUES: 4-entry table read by the 6502 at &3e63..&3e66.
// The crab picks one at random and stores it in the spawned object's
// field-&06b. These are NOT 4 different object types — the spawned
// object is always EXILE1 type &09 (which maps to sprite &45 = GARGOYLE
// in the standard atlas via EXILE1's table at &1d15). Two of the values
// (&72, &5a) are >= &48 so they CAN'T be object types in EXILE1's
// 72-entry table — they're likely a "variant selector" / sprite-tweak
// the spawned object's own update routine consumes.
static constexpr uint8_t CRAB_SPAWN_INIT_VALUES[4] = {
    0x22, 0x72, 0x5a, 0x1f,
};

// The spawned object's type. EXILE1 obj-type &09 → sprite &45 (which is
// SPRITE_GARGOYLE in the standard release's atlas). Until the EXILE1
// type-&09 update routine is ported, FIREBALL is the stand-in: a
// small, drifting, hostile projectile — same kind of thing the crab
// would emit.
static constexpr ObjectType CRAB_SPAWN_TYPE = ObjectType::FIREBALL;

void update_crab(Object& obj, UpdateContext& ctx) {
    // 6502 &3e67: faithful update is JSR &28db (apply velocity); the
    // crab never sets velocity itself. Everything below is port-only.

    // Override get_initial_energy's range-9 default (0x7d) — too high to
    // deplete in a play session, hence the perceived invulnerability.
    if (obj.flags & ObjectFlags::NEWLY_CREATED) {
        obj.energy = 0x18;
    }

    // Horizontal-only seek. seek_player(1) cancels with the +1 gravity
    // the shared physics step adds, leaving vy=0 and trapping the crab.
    const Object& player = ctx.mgr.player();
    int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
    if (dx > 0)      obj.velocity_x =  4;
    else if (dx < 0) obj.velocity_x = -4;
    else             obj.velocity_x =  0;

    bool moving = (obj.velocity_x != 0) || (obj.velocity_y != 0);
    obj.sprite = moving ? (0x7f + ((ctx.frame_counter >> 3) & 0x01))
                        : 0x7f;
    NPC::consider_face_movement_direction(obj, ctx.rng);

    // One-frame damage flash. Clearing WAS_DAMAGED here is what limits
    // the tint to a single frame (nothing else clears it).
    constexpr uint8_t kCrabBasePalette = 0x35;
    if (obj.flags & ObjectFlags::WAS_DAMAGED) {
        obj.palette = kCrabBasePalette ^ 0x30;
        obj.flags &= ~ObjectFlags::WAS_DAMAGED;
    } else {
        obj.palette = kCrabBasePalette;
    }


}

}  // namespace Behaviors
