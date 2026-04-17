#pragma once
#include "objects/object.h"
#include "ai/npc_helpers.h"

namespace Behaviors {

void update_turret(Object& obj, UpdateContext& ctx);
void update_rolling_robot(Object& obj, UpdateContext& ctx);
void update_blue_rolling_robot(Object& obj, UpdateContext& ctx);
void update_hovering_robot(Object& obj, UpdateContext& ctx);
void update_clawed_robot(Object& obj, UpdateContext& ctx);
void update_hovering_ball(Object& obj, UpdateContext& ctx);
void update_invisible_hovering_ball(Object& obj, UpdateContext& ctx);

} // namespace Behaviors
