#pragma once
#include "objects/object.h"
#include "objects/object_manager.h"

// Held object system - port of &1afd-&1b48 and &1ca9-&1cdc
namespace HeldObject {

// Update the position of a held object to follow the player.
// Port of &1afd-&1b48: centers vertically, offsets horizontally by facing direction.
void update_position(Object& held, const Object& player);

// Check if the held object should be dropped.
// Port of &1ca9: drops if distance > 0x30 from expected position.
bool should_drop(const Object& held, const Object& player);

// Pick up an object: set it as held.
void pickup(Object& held, Object& player, uint8_t& held_slot, int slot);

// Drop the held object.
void drop(Object& held, Object& player, uint8_t& held_slot);

// Check if an object type can be picked up (bit 7 of palette_and_pickup table)
bool is_pickupable(ObjectType type);

} // namespace HeldObject
