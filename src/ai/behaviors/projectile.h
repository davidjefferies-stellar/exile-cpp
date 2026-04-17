#pragma once
#include "objects/object.h"
#include "ai/npc_helpers.h"

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

} // namespace Behaviors
