#pragma once
#include "objects/object.h"
#include "behaviours/npc_helpers.h"

namespace Behaviors {

void update_active_grenade(Object& obj, UpdateContext& ctx);
void update_icer_bullet(Object& obj, UpdateContext& ctx);
void update_tracer_bullet(Object& obj, UpdateContext& ctx);
void update_cannonball(Object& obj, UpdateContext& ctx);
void update_blue_death_ball(Object& obj, UpdateContext& ctx);
void update_red_bullet(Object& obj, UpdateContext& ctx);
void update_pistol_bullet(Object& obj, UpdateContext& ctx);
void update_plasma_ball(Object& obj, UpdateContext& ctx);
void update_lightning(Object& obj, UpdateContext& ctx);
void update_red_mushroom_ball(Object& obj, UpdateContext& ctx);
void update_invisible_debris(Object& obj, UpdateContext& ctx);
void update_red_drop(Object& obj, UpdateContext& ctx);
void update_fireball(Object& obj, UpdateContext& ctx);
void update_moving_fireball(Object& obj, UpdateContext& ctx);
void update_explosion(Object& obj, UpdateContext& ctx);

// Port of accelerate_all_objects (&343a-&34b0) — applies damage and a
// directional push to every primary within blast range of `source`.
// Used by update_explosion for EXPLOSION primaries and directly by the
// blaster discharge tick (&4a76-&4a83). Skips slot `source_slot` so the
// source itself isn't damaged or pushed. When damage_events is non-null
// every hit and the source's effective tile radius are pushed for the
// "Damage" debug overlay.
void apply_explosion_radius(ObjectManager& mgr, const Object& source,
                            int source_slot, uint8_t duration,
                            std::vector<DamageVisual>* damage_events = nullptr);

} // namespace Behaviors
