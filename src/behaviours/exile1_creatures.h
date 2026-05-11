#pragma once
#include "objects/object.h"
#include "behaviours/creature.h"  // for UpdateContext

// EXILE1-only creatures (8-March-1988 master disk). Wiring TODO:
// types.h ObjectType, debug_names.h, atlas sprite slots, dispatch
// table, object_data.h palette/flags. See exile-EXILE1-disassembly.txt.

namespace Behaviors {

// EXILE1 OBJECT_DOG (type &1a) — &3cb5. Ground-walking homing NPC,
// tail-calls shared NPC walker at &39e3 with walk_speed/mood = 0xc0.
void update_dog(Object& obj, UpdateContext& ctx);

// EXILE1 OBJECT_CRAB (type &26) — &3e67. Stationary emitter, throttled
// to <=4 type-9 children, random ±1 initial timer/velocity.
void update_crab(Object& obj, UpdateContext& ctx);

}  // namespace Behaviors
