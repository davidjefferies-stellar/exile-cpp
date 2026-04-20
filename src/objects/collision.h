#pragma once
#include "objects/object.h"
#include "world/landscape.h"
#include <array>

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
    const std::array<Object, 16>& all_objects);

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
                           const std::array<Object, 16>& all_objects);

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
    const std::array<Object, 16>& all_objects,
    uint8_t tertiary_byte_fallback);

} // namespace Collision
