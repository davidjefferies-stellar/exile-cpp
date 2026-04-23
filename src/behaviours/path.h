#pragma once
#include "behaviours/npc_helpers.h"

namespace NPC {

// ============================================================================
// Line-of-sight + pathfinding — port of &359a-&35e4, &3cf6-&3da5.
// ============================================================================
//
// The 6502 runs a three-layer behaviour for NPCs that target the player
// (or any other object):
//
//   1. Once every 16 frames, `consider_if_npc_can_see_target` (&3cf6) runs
//      a tile-by-tile raycast between the NPC and its target. If a solid
//      tile obstruction blocks the line of sight, the NPC's directness
//      level decays; if the line is clear, it's restored to level 3.
//   2. Each frame, `consider_updating_npc_path` (&3d26) dispatches on the
//      directness bits of `target_object_and_flags` (&3e):
//        level 2 or 3 (bit 7 set) → head straight for the target (tx, ty).
//        level 1       (bit 6 set) → head roughly at target, randomised.
//        level 0                   → wander until the target is re-sighted.
//   3. The chosen (tx, ty) is fed to `move_towards_target` / the per-type
//      walking routine, which builds the per-axis velocity nudge.
//
// This port covers layer 1 + layer 2 (LOS raycast + directness state
// machine) and the tx/ty selection. Layer 3's full per-NPC-type walking
// state machine (slope checks, jumping, etc.) remains simplified — see
// `move_towards_target_with_probability` for the reduced form callers use.

// Directness bits on `target_and_flags`. Identical mapping to the 6502's
// `TARGET_FLAG_DIRECTNESS_TWO/ONE/AVOIDING`.
//   bit 7 : DIRECTNESS_TWO — target is currently visible
//   bit 6 : DIRECTNESS_ONE — target was visible recently
//   bit 5 : AVOIDING       — head AWAY from target, not toward
//   bits 0-4 : target object slot (0 = player)
//
// Combined directness level: bits 7,6 → 00=0, 01=1, 10=2, 11=3.
inline uint8_t directness_level(const Object& obj) {
    return static_cast<uint8_t>((obj.target_and_flags >> 6) & 0x03);
}

// Tile-by-tile line-of-sight raycast between `obj` and the object in
// `target_slot`. `max_tiles` caps the search distance in whole tiles
// (the 6502 parameter is in 1/8-tile fractions; we convert internally).
// Returns true if no solid tile obstructs the ray.
//
// Port of `check_for_obstruction_between_objects` (&359c). Simplifications
// vs the 6502:
//   * Doesn't special-case door tiles at the target's position (the 6502
//     suppresses `door_to_suppress`; ours checks the whole grid plainly).
//   * Waterline-crossing-counts-as-obstruction flag isn't wired — the
//     6502 uses it for piranha / wasp targeting of out-of-element NPCs;
//     `update_big_fish` currently approximates that separately.
bool has_line_of_sight(const Object& obj,
                       uint8_t target_slot,
                       uint8_t max_tiles,
                       const UpdateContext& ctx);

// 16-frame-cadence directness update. Port of `consider_if_npc_can_see_target`
// (&3cf6). When LOS is clear, snaps both directness bits on; when blocked,
// decays the level by one per 16-frame tick. Also nudges `obj.tx / obj.ty`
// toward the target when the target is visible.
//
// Safe to call from any NPC's update; respects `obj.frame_counter_sixteen`
// so it's O(1) most frames.
void update_target_directness(Object& obj, UpdateContext& ctx);

// Per-frame path (tx, ty) update. Port of `consider_updating_npc_path`
// (&3d26). Dispatches on the current directness level:
//   3/2 → tx, ty = target.x, target.y                 (direct chase)
//   1   → tx, ty = target.x ± random, target.y ± rand (slight jitter)
//   0   → tx, ty drifts with RNG                      (wander)
//
// Callers combine this with `move_towards_target_with_probability` (which
// already reads obj.target_and_flags to find the target slot) to get
// velocity toward the chosen (tx, ty).
void update_npc_path(Object& obj, UpdateContext& ctx);

} // namespace NPC
