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

// Pixel-precise AABB overlap check against any weight-7 non-INTANGIBLE
// static object (doors, switches, etc.). Returns true if `obj`'s AABB
// overlaps such a primary. Port-approximation of the 6502's velocity
// transfer in apply_collision_to_objects_velocities (&2bb6): when a
// light object (player) hits a heavy one (door), the mass ratio
// effectively reflects the light object's velocity. We model this as a
// position revert in the caller — same end result: player stops at the
// door boundary. Used alongside substitute_door_for_obstruction because
// STONE_SLOPE_78's pattern only covers the left quarter of the tile,
// whereas the door sprite spans ~half the tile, so tile obstruction
// alone would let the player fall through parts of the door sprite.
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

// Port of calculate_transfer_velocities (&2bee-&2c14) +
// apply_collision_to_object_velocity (&2bc6-&2bed). Given two objects'
// velocity components on one axis and their weights, returns the new
// velocities after an elastic(-ish) mass-ratio collision.
//
// The 6502 computes:
//   half_diff = (this_v - other_v) / 2
//   transfer  = half_diff / 2^|weight_diff|     (rounded toward -inf)
//   lesser    = transfer        (applied to heavier side)
//   greater   = half_diff - transfer (applied to lighter side)
// Each is then halved and, depending on whether this side was "hit
// from" that direction, doubled. Signs flip for the heavier side so
// the two objects end up moving apart.
//
// We expose it as a simple "velocity-in, velocity-out" helper so the
// player-motion block path can use it in place of "velocity = 0".
struct VelocityTransfer {
    int8_t this_v;
    int8_t other_v;
};
VelocityTransfer apply_mass_ratio_velocity(
    int8_t this_v_in, int8_t other_v_in,
    uint8_t this_weight, uint8_t other_weight,
    bool smallest_overlap_in_this_axis);

// Port of &3ebd-&3ec2 door_tiles_table substitution. Given a tile+flip
// byte and the data_offset of the tertiary entry it came from, returns
// the tile_and_flip to use for OBSTRUCTION checks. For METAL_DOOR /
// STONE_DOOR tiles this swaps in TILE_STONE_SLOPE_78 (closed door, solid)
// or TILE_SPACE (open door, passable) based on the live DOOR_FLAG_OPENING
// bit — the same mechanism the 6502 uses to make doors block at the tile
// level. All other tiles pass through unchanged. Preserves flip bits.
//
// The live state is read from any primary currently linked to the
// tertiary slot (preferred), falling back to the stored tertiary byte.
uint8_t substitute_door_for_obstruction(
    uint8_t tile_and_flip, int data_offset,
    const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>& all_objects,
    uint8_t tertiary_byte_fallback);

} // namespace Collision
