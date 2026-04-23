#pragma once
#include "objects/object.h"
#include "behaviours/npc_helpers.h"

namespace AI {

// Object update function signature.
// Called once per frame for each active primary object.
using UpdateFunc = void(*)(Object& obj, UpdateContext& ctx);

// Get the update function for an object type.
// Returns nullptr for types with no active behavior (pure physics).
UpdateFunc get_update_func(ObjectType type);

} // namespace AI
