#pragma once
#include "objects/object.h"
#include "world/landscape.h"
#include <array>

class ObjectManager;

// Tile and object-object collision detection.
namespace Collision {

struct TileCollisionResult {
    bool bottom = false;     // Supported from below
    bool top = false;        // Blocked from above
    bool left = false;       // Blocked from left
    bool right = false;      // Blocked from right
    bool any = false;        // Any collision occurred
    int8_t push_x = 0;      // Fractional push-back X (for slope sliding)
    int8_t push_y = 0;      // Fractional push-back Y
    uint8_t angle = 0;       // Collision surface angle (0=right, 0x40=down, 0x80=left, 0xC0=up)
};

// Per-pixel tile collision using obstruction patterns.
// Checks the tile(s) overlapping the object's bounding box against
// obstruction patterns, determining push direction from overlap.
TileCollisionResult check_tile_collision(const Landscape& landscape, const Object& obj);

// Check if a tile position is solid (simplified check, no sub-tile precision).
bool is_tile_solid(const Landscape& landscape, uint8_t tile_x, uint8_t tile_y);

// Per-section obstruction check: is the point at (x_frac, y_frac) inside the
// current tile's obstruction pattern? Used by the line-of-sight raycast to
// walk a tile-by-tile trace and catch rays grazing slopes. Mirrors the 6502's
// LDA (&7c),Y + CMP y_fraction + EOR v-flip at &3652-&365f.
bool point_in_tile_solid(const Landscape& landscape,
                         uint8_t tile_x, uint8_t tile_y,
                         uint8_t x_frac, uint8_t y_frac);

// Same as point_in_tile_solid, but applies the 6502's update_*_door_tile
// substitution at &3ebd-&3ec2 first. Closed doors test as their substitute
// (SPACESHIP_WALL_HORIZONTAL_QUARTER for horizontal, STONE_SLOPE_78 for
// vertical); open doors test as SPACE. The 6502 routed every obstruction
// query through update_*_door_tile, so this is the door-aware variant for
// callers that want closed doors to block (NPC LOS, projectile vs tile,
// etc.). The plain point_in_tile_solid stays available for callers that
// must NOT see doors as obstacles (held-object overlap probes).
bool point_in_tile_solid_with_doors(
    const Landscape& landscape, ObjectManager& mgr,
    uint8_t tile_x, uint8_t tile_y,
    uint8_t x_frac, uint8_t y_frac);

// Same pattern check, but operates on an explicit tile_and_flip byte rather
// than reading from landscape. Useful after substitute_door_for_obstruction:
// the caller gets the live door tile (STONE_SLOPE_78 / SPACE) back and wants
// to probe that, not the raw METAL_DOOR / STONE_DOOR type.
bool tile_and_flip_obstructs_point(uint8_t tile_and_flip,
                                    uint8_t x_frac, uint8_t y_frac);

// Same, but operates on a tile type directly. Useful when you already have
// the tile byte (e.g. post-tertiary resolution) and don't want another
// landscape lookup.
bool is_tile_type_solid(uint8_t tile_type);

// Object-object collision: check one object against all others.
struct ObjectCollisionResult {
    bool collided = false;
    int other_slot = -1;     // Slot of the object we collided with
    int8_t push_x = 0;
    int8_t push_y = 0;
};

ObjectCollisionResult check_object_collision(
    const Object& obj, int slot,
    const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>& all_objects);

// AABB overlap vs weight-7 non-INTANGIBLE statics (doors, switches).
// Port-approximation of &2bb6's mass-ratio reflection; caller reverts
// position. Needed alongside substitute_door_for_obstruction because
// STONE_SLOPE_78's pattern only covers the door sprite's left quarter.
bool overlaps_solid_object(const Object& obj, int self_slot,
                           const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>& all_objects,
                           int skip_slot = -1);

// Same gate as `overlaps_solid_object` but returns the colliding slot
// instead of a bool. -1 means no overlap. Used by the post-integrate
// object-overlap revert in object_update.cpp so a bullet that bumps a
// heavier static (turret, door, cannon) gets its `obj.touching` stamped
// with the slot — the next update tick reads that and explodes the
// bullet on the target. Without it, the revert just stops the bullet
// dead and `obj.touching` from step 9b stays at 0x80.
int overlapping_solid_slot(const Object& obj, int self_slot,
                           const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>& all_objects,
                           int skip_slot = -1);

// Port of &2b80-&2b91 (the position-disengage limb of check_for_collisions):
// picks the smallest-overlap axis of `obj` vs `blocker`'s AABB, shoves
// `obj`'s position out along it, and adds ±2 to the matching velocity
// component. Used so a door closing onto a stationary player pushes the
// player aside instead of leaving them embedded in the door.
// Returns true iff there was overlap to resolve.
bool push_out_of_overlap(Object& obj, const Object& blocker);

// Port of &2bee calculate_transfer_velocities + &2bc6 apply_collision_
// to_object_velocity. Mass-ratio elastic-ish transfer:
//   transfer = ((this_v - other_v)/2) / 2^|weight_diff|
// Heavier side gets `lesser`, lighter gets `greater`, axis-doubled.
struct VelocityTransfer {
    int8_t this_v;
    int8_t other_v;
};
VelocityTransfer apply_mass_ratio_velocity(
    int8_t this_v_in, int8_t other_v_in,
    uint8_t this_weight, uint8_t other_weight,
    bool smallest_overlap_in_this_axis);

// Port of &3ebd-&3ec2 door_tiles_table substitution:
//   &3ebd ROL A                ; carry → bit 0 (open flag)
//   &3ebe TAX                  ; X = 0..3: h_closed, h_open, v_closed, v_open
//   &3ebf LDA &3e91,X ; door_tiles_table
//   &3ec2 STA &08 ; tile_type_and_flip
// Swaps METAL_DOOR / STONE_DOOR for STONE_SLOPE_78 (closed) or SPACE
// (open) based on the live DOOR_FLAG_OPENING bit; preserves flip. Reads
// live state from a linked primary if any, else the stored tertiary
// byte.
uint8_t substitute_door_for_obstruction(
    uint8_t tile_and_flip, int data_offset,
    const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>& all_objects,
    uint8_t tertiary_byte_fallback);

} // namespace Collision
