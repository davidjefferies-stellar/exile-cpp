#pragma once
#include "behaviours/npc_helpers.h"

namespace NPC {

// &359a-&35e4 + &3cf6-&3da5 LOS + pathfinding. Layer 1: 16-frame LOS
// raycast (&3cf6); layer 2: directness dispatch on &3e (&3d26); layer 3
// per-type walking remains simplified — see
// move_towards_target_with_probability for the reduced form.

// target_and_flags layout (matches 6502 TARGET_FLAG_*): bit 7 DIRECTNESS_TWO
// (visible now), bit 6 DIRECTNESS_ONE (recent), bit 5 AVOIDING, bits 0-4
// slot. Combined level (bits 7,6): 00=0, 01=1, 10=2, 11=3.
inline uint8_t directness_level(const Object& obj) {
    return static_cast<uint8_t>((obj.target_and_flags >> 6) & 0x03);
}

// &359c check_for_obstruction_between_objects. Port-only: drops
// door_to_suppress and the waterline-crossing flag (update_big_fish
// approximates that separately). max_tiles is whole tiles, not &20-fracs.
bool has_line_of_sight(const Object& obj,
                       uint8_t target_slot,
                       uint8_t max_tiles,
                       const UpdateContext& ctx);

// &3cb5-&3cba find_object LOS cap = (rnd & 0x4f) XOR nearest_object_distance.
// Single-target callers pass 0xff so the cap is 22..32 tiles random;
// multi-candidate callers (future find_object port) pass the running
// nearest so later candidates get a tighter cap.
bool has_line_of_sight_randomized(
    const Object& obj,
    uint8_t target_slot,
    const UpdateContext& ctx,
    uint8_t nearest_object_distance = 0xff);

// &3cf6 consider_if_npc_can_see_target. 16-frame cadence: LOS clear ->
// directness=3; blocked -> -1 level. Updates tx/ty when target visible.
void update_target_directness(Object& obj, UpdateContext& ctx);

// &3d26 consider_updating_npc_path. Dispatches on directness level: 3/2
// -> exact target, 1 -> jittered, 0 -> wander. Combined with
// move_towards_target_with_probability to produce per-axis velocity.
void update_npc_path(Object& obj, UpdateContext& ctx);

} // namespace NPC
