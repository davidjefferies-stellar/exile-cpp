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

} // namespace Collision
