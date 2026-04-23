#include "behaviours/behavior_dispatch.h"
#include "behaviours/projectile.h"
#include "behaviours/creature.h"
#include "behaviours/robot.h"
#include "behaviours/environment.h"
#include "behaviours/collectable.h"

namespace AI {

// Master dispatch table: maps each ObjectType to its update function.
// Derived from the 6502 update routine address table at &03cd/&0446.
// Types sharing the same address in the original share the same function here.
static const UpdateFunc dispatch_table[] = {
    // 0x00-0x0F
    Behaviors::update_player,               // 0x00 PLAYER          (&4A11)
    Behaviors::update_active_chatter,       // 0x01 ACTIVE_CHATTER  (&48D7)
    Behaviors::update_crew_member,          // 0x02 CREW_MEMBER     (&46F0)
    Behaviors::update_fluffy,               // 0x03 FLUFFY          (&4288)
    Behaviors::update_hive,                 // 0x04 SMALL_HIVE      (&4BAF)
    Behaviors::update_hive,                 // 0x05 LARGE_HIVE      (&4BAF)
    Behaviors::update_red_frogman,          // 0x06 RED_FROGMAN     (&4463)
    Behaviors::update_green_frogman,        // 0x07 GREEN_FROGMAN   (&4477)
    Behaviors::update_invisible_frogman,    // 0x08 INVISIBLE_FROGMAN (&4475)
    Behaviors::update_red_slime,            // 0x09 RED_SLIME       (&47C9)
    Behaviors::update_green_slime,          // 0x0A GREEN_SLIME     (&422A)
    Behaviors::update_yellow_slime,         // 0x0B YELLOW_SLIME    (&4266)
    Behaviors::update_dense_nest,           // 0x0C DENSE_NEST      (&4789)
    Behaviors::update_sucking_nest,         // 0x0D SUCKING_NEST    (&4DED)
    Behaviors::update_big_fish,             // 0x0E BIG_FISH        (&4761)
    Behaviors::update_worm,                 // 0x0F WORM            (&420A)

    // 0x10-0x1F
    Behaviors::update_piranha_or_wasp,      // 0x10 PIRANHA         (&4F21)
    Behaviors::update_piranha_or_wasp,      // 0x11 WASP            (&4F21)
    Behaviors::update_active_grenade,       // 0x12 ACTIVE_GRENADE  (&42F7)
    Behaviors::update_icer_bullet,          // 0x13 ICER_BULLET     (&46BF)
    Behaviors::update_tracer_bullet,        // 0x14 TRACER_BULLET   (&4614)
    Behaviors::update_cannonball,           // 0x15 CANNONBALL      (&4326)
    Behaviors::update_blue_death_ball,      // 0x16 BLUE_DEATH_BALL (&4332)
    Behaviors::update_red_bullet,           // 0x17 RED_BULLET      (&434A)
    Behaviors::update_pistol_bullet,        // 0x18 PISTOL_BULLET   (&441B)
    Behaviors::update_plasma_ball,          // 0x19 PLASMA_BALL     (&4A88)
    Behaviors::update_hovering_ball,        // 0x1A HOVERING_BALL   (&43E7)
    Behaviors::update_invisible_hovering_ball, // 0x1B INVISIBLE_HOVERING_BALL (&43EB)
    Behaviors::update_rolling_robot,        // 0x1C MAGENTA_ROLLING_ROBOT (&4EDE)
    Behaviors::update_rolling_robot,        // 0x1D RED_ROLLING_ROBOT (&4EDE)
    Behaviors::update_blue_rolling_robot,   // 0x1E BLUE_ROLLING_ROBOT (&4EE2)
    Behaviors::update_turret,               // 0x1F GREEN_WHITE_TURRET (&4ED8)

    // 0x20-0x2F
    Behaviors::update_turret,               // 0x20 CYAN_RED_TURRET (&4ED8)
    Behaviors::update_hovering_robot,       // 0x21 HOVERING_ROBOT  (&4804)
    Behaviors::update_clawed_robot,         // 0x22 MAGENTA_CLAWED_ROBOT (&481F)
    Behaviors::update_clawed_robot,         // 0x23 CYAN_CLAWED_ROBOT (&481F)
    Behaviors::update_clawed_robot,         // 0x24 GREEN_CLAWED_ROBOT (&481F)
    Behaviors::update_clawed_robot,         // 0x25 RED_CLAWED_ROBOT (&481F)
    Behaviors::update_triax,                // 0x26 TRIAX           (&4704)
    Behaviors::update_maggot,               // 0x27 MAGGOT          (&4E52)
    Behaviors::update_gargoyle,             // 0x28 GARGOYLE        (&4170)
    Behaviors::update_imp,                  // 0x29 RED_MAGENTA_IMP (&44EF)
    Behaviors::update_imp,                  // 0x2A RED_YELLOW_IMP  (&44EF)
    Behaviors::update_imp,                  // 0x2B BLUE_CYAN_IMP   (&44EF)
    Behaviors::update_imp,                  // 0x2C CYAN_YELLOW_IMP (&44EF)
    Behaviors::update_imp,                  // 0x2D RED_CYAN_IMP    (&44EF)
    Behaviors::update_bird,                 // 0x2E GREEN_YELLOW_BIRD (&4631)
    Behaviors::update_bird,                 // 0x2F WHITE_YELLOW_BIRD (&4631)

    // 0x30-0x3F
    Behaviors::update_red_magenta_bird,     // 0x30 RED_MAGENTA_BIRD (&4621)
    Behaviors::update_invisible_bird,       // 0x31 INVISIBLE_BIRD  (&462B)
    Behaviors::update_lightning,            // 0x32 LIGHTNING        (&4101)
    Behaviors::update_red_mushroom_ball,    // 0x33 RED_MUSHROOM_BALL (&4698)
    Behaviors::update_red_mushroom_ball,    // 0x34 BLUE_MUSHROOM_BALL (&4698)
    Behaviors::update_invisible_debris,     // 0x35 INVISIBLE_DEBRIS (&4791)
    Behaviors::update_red_drop,             // 0x36 RED_DROP         (&4799)
    Behaviors::update_fireball,             // 0x37 FIREBALL         (&4AD6)
    Behaviors::update_inactive_chatter,     // 0x38 INACTIVE_CHATTER (&48C1)
    Behaviors::update_moving_fireball,      // 0x39 MOVING_FIREBALL  (&4B26)
    Behaviors::update_giant_block,          // 0x3A GIANT_BLOCK      (&439C)
    Behaviors::update_engine_fire,          // 0x3B ENGINE_FIRE      (&4C15)
    Behaviors::update_door,                 // 0x3C HORIZONTAL_METAL_DOOR (&4C83)
    Behaviors::update_door,                 // 0x3D VERTICAL_METAL_DOOR (&4C83)
    Behaviors::update_door,                 // 0x3E HORIZONTAL_STONE_DOOR (&4C83)
    Behaviors::update_door,                 // 0x3F VERTICAL_STONE_DOOR (&4C83)

    // 0x40-0x4F
    Behaviors::update_bush,                 // 0x40 BUSH             (&4BA9)
    Behaviors::update_transporter_beam,     // 0x41 TRANSPORTER_BEAM (&4D86)
    Behaviors::update_switch,               // 0x42 SWITCH           (&499D)
    Behaviors::update_inert,                // 0x43 PIANO            (&43AD)
    Behaviors::update_explosion,            // 0x44 EXPLOSION        (&4F9C)
    Behaviors::update_inert,                // 0x45 BOULDER          (&43AD)
    Behaviors::update_cannon,               // 0x46 CANNON           (&40EE)
    Behaviors::update_alien_weapon,         // 0x47 ALIEN_WEAPON     (&4216)
    Behaviors::update_maggot_machine,       // 0x48 MAGGOT_MACHINE   (&419F)
    Behaviors::update_placeholder,          // 0x49 PLACEHOLDER      (&4B64)
    Behaviors::update_destinator,           // 0x4A DESTINATOR       (&4374)
    Behaviors::update_power_pod,            // 0x4B POWER_POD        (&4360)
    Behaviors::update_empty_flask,          // 0x4C EMPTY_FLASK      (&43A7)
    Behaviors::update_full_flask,           // 0x4D FULL_FLASK       (&43AE)
    Behaviors::update_control_device,       // 0x4E REMOTE_CONTROL_DEVICE (&4351)
    Behaviors::update_control_device,       // 0x4F CANNON_CONTROL_DEVICE (&4351)

    // 0x50-0x5F
    Behaviors::update_inactive_grenade,     // 0x50 INACTIVE_GRENADE (&4158)
    Behaviors::update_collectable,          // 0x51 CYAN_YELLOW_GREEN_KEY (&4B88)
    Behaviors::update_collectable,          // 0x52 RED_YELLOW_GREEN_KEY (&4B88)
    Behaviors::update_collectable,          // 0x53 GREEN_YELLOW_RED_KEY (&4B88)
    Behaviors::update_collectable,          // 0x54 YELLOW_WHITE_RED_KEY (&4B88)
    Behaviors::update_coronium_boulder,     // 0x55 CORONIUM_BOULDER (&41CA)
    Behaviors::update_collectable,          // 0x56 RED_MAGENTA_RED_KEY (&4B88)
    Behaviors::update_collectable,          // 0x57 BLUE_CYAN_GREEN_KEY (&4B88)
    Behaviors::update_coronium_crystal,     // 0x58 CORONIUM_CRYSTAL (&41C2)
    Behaviors::update_collectable,          // 0x59 JETPACK_BOOSTER  (&4B88)
    Behaviors::update_collectable,          // 0x5A PISTOL           (&4B88)
    Behaviors::update_collectable,          // 0x5B ICER             (&4B88)
    Behaviors::update_collectable,          // 0x5C BLASTER          (&4B88)
    Behaviors::update_collectable,          // 0x5D PLASMA_GUN       (&4B88)
    Behaviors::update_collectable,          // 0x5E PROTECTION_SUIT  (&4B88)
    Behaviors::update_collectable,          // 0x5F FIRE_IMMUNITY    (&4B88)

    // 0x60-0x64
    Behaviors::update_collectable,          // 0x60 MUSHROOM_IMMUNITY (&4B88)
    Behaviors::update_collectable,          // 0x61 WHISTLE_ONE      (&4B88)
    Behaviors::update_collectable,          // 0x62 WHISTLE_TWO      (&4B88)
    Behaviors::update_collectable,          // 0x63 RADIATION_IMMUNITY (&4B88)
    Behaviors::update_inert,                // 0x64 INVISIBLE_INERT  (&43AD)
};

static_assert(sizeof(dispatch_table) / sizeof(dispatch_table[0]) ==
              static_cast<int>(ObjectType::COUNT),
              "Dispatch table must have exactly one entry per ObjectType");

UpdateFunc get_update_func(ObjectType type) {
    uint8_t idx = static_cast<uint8_t>(type);
    if (idx >= static_cast<uint8_t>(ObjectType::COUNT)) return nullptr;
    return dispatch_table[idx];
}

} // namespace AI
