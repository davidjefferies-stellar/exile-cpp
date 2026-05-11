#include "behaviours/exile1_creatures.h"
#include "behaviours/mood.h"
#include "behaviours/npc_helpers.h"
#include "core/types.h"
#include <cstdlib>

// EXILE1-only dog (&3cb5, 18 bytes) and crab (&3e67, 52 bytes); see
// exile-EXILE1-disassembly.txt.

namespace Behaviors {

// EXILE1 &3cb5 update_dog. Tail-calls shared NPC walker (&39e3) with
// walk_speed/mood = 0xc0, heading from target (&35) or default (&24).
void update_dog(Object& obj, UpdateContext& ctx) {
    // 6502 &28ff npc_setup_helper. No cancel_gravity (ground walker).
    Mood::update_mood(obj, ctx);

    // Port-only: skip the 6502 &be/&35/&24 heading-select; seek player
    // unconditionally until npc_setup_helper populates target heading.
    const Object& player = ctx.mgr.player();
    NPC::seek_player(obj, player, /*speed=*/4);

    // 6502 JMP &39e3 shared NPC walker.
    NPC::consider_face_movement_direction(obj, ctx.rng);
    // Reduced &39f1 sprite_animate_helper: two-frame walk cycle, freeze
    // when stationary. Facing flip handled above.
    bool moving = (obj.velocity_x != 0) || (obj.velocity_y != 0);
    obj.sprite = moving ? (0x7d + ((ctx.frame_counter >> 3) & 0x01))
                        : 0x7d;

    // 6502 dispatch ends with RTS via stacked-jump; nothing further.
}


// EXILE1 &3e67 update_crab. Stationary emitter: throttled to <=4 type-9
// children alive; picks a random initial value from spawn_table and a
// random-sign ±1 timer/velocity.

// CRAB_SPAWN_INIT_VALUES: &3e63..&3e66 table. Stored into spawned obj's
// field-&06b as a variant selector — &72/&5a exceed the 72-entry type
// table so they can't be types. Spawned obj is always EXILE1 type &09.
static constexpr uint8_t CRAB_SPAWN_INIT_VALUES[4] = {
    0x22, 0x72, 0x5a, 0x1f,
};

// Stand-in for EXILE1 type &09 (sprite_gargoyle). FIREBALL = small
// drifting hostile projectile until the type-&09 routine is ported.
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
