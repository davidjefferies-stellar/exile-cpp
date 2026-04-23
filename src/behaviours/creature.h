#pragma once
#include "objects/object.h"
#include "behaviours/npc_helpers.h"

namespace Behaviors {

void update_player(Object& obj, UpdateContext& ctx);
void update_active_chatter(Object& obj, UpdateContext& ctx);
void update_inactive_chatter(Object& obj, UpdateContext& ctx);
void update_crew_member(Object& obj, UpdateContext& ctx);
void update_fluffy(Object& obj, UpdateContext& ctx);
void update_imp(Object& obj, UpdateContext& ctx);
void update_red_frogman(Object& obj, UpdateContext& ctx);
void update_green_frogman(Object& obj, UpdateContext& ctx);
void update_invisible_frogman(Object& obj, UpdateContext& ctx);
void update_red_slime(Object& obj, UpdateContext& ctx);
void update_green_slime(Object& obj, UpdateContext& ctx);
void update_yellow_slime(Object& obj, UpdateContext& ctx);
void update_big_fish(Object& obj, UpdateContext& ctx);
void update_worm(Object& obj, UpdateContext& ctx);
void update_maggot(Object& obj, UpdateContext& ctx);
void update_piranha_or_wasp(Object& obj, UpdateContext& ctx);
void update_bird(Object& obj, UpdateContext& ctx);
void update_red_magenta_bird(Object& obj, UpdateContext& ctx);
void update_invisible_bird(Object& obj, UpdateContext& ctx);
void update_gargoyle(Object& obj, UpdateContext& ctx);
void update_triax(Object& obj, UpdateContext& ctx);

} // namespace Behaviors
