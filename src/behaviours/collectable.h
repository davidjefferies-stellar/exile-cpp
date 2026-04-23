#pragma once
#include "objects/object.h"
#include "behaviours/npc_helpers.h"

namespace Behaviors {

void update_collectable(Object& obj, UpdateContext& ctx);  // 17 types share this
void update_inactive_grenade(Object& obj, UpdateContext& ctx);
void update_power_pod(Object& obj, UpdateContext& ctx);
void update_destinator(Object& obj, UpdateContext& ctx);
void update_empty_flask(Object& obj, UpdateContext& ctx);
void update_full_flask(Object& obj, UpdateContext& ctx);
void update_control_device(Object& obj, UpdateContext& ctx);
void update_coronium_boulder(Object& obj, UpdateContext& ctx);
void update_coronium_crystal(Object& obj, UpdateContext& ctx);
void update_alien_weapon(Object& obj, UpdateContext& ctx);
void update_giant_block(Object& obj, UpdateContext& ctx);
void update_inert(Object& obj, UpdateContext& ctx);  // PIANO, BOULDER, INVISIBLE_INERT

} // namespace Behaviors
