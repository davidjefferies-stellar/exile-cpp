#pragma once
#include "objects/object.h"
#include "ai/npc_helpers.h"

namespace Behaviors {

void update_door(Object& obj, UpdateContext& ctx);
void update_switch(Object& obj, UpdateContext& ctx);
void update_transporter_beam(Object& obj, UpdateContext& ctx);
void update_hive(Object& obj, UpdateContext& ctx);
void update_dense_nest(Object& obj, UpdateContext& ctx);
void update_sucking_nest(Object& obj, UpdateContext& ctx);
void update_bush(Object& obj, UpdateContext& ctx);
void update_cannon(Object& obj, UpdateContext& ctx);
void update_maggot_machine(Object& obj, UpdateContext& ctx);
void update_engine_fire(Object& obj, UpdateContext& ctx);
void update_placeholder(Object& obj, UpdateContext& ctx);

} // namespace Behaviors
