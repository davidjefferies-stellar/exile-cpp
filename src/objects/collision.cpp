#include "objects/collision.h"
#include "objects/object_data.h"
#include "world/tile_data.h"
#include "world/obstruction.h"
#include "core/types.h"
#include <cstdlib>

namespace Collision {

bool is_tile_solid(const Landscape& landscape, uint8_t tile_x, uint8_t tile_y) {
    uint8_t tile = landscape.get_tile(tile_x, tile_y);
    uint8_t type = tile & TileFlip::TYPE_MASK;

    switch (type) {
        case 0x19: return false; // TILE_SPACE
        case 0x0e: return false; // TILE_VARIABLE_WIND
        case 0x0b: return false; // TILE_CONSTANT_WIND
        case 0x0d: return false; // TILE_WATER
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x04: case 0x05: case 0x06: case 0x07:
        case 0x08: return false; // Tertiary check ranges
        case 0x1b: case 0x1a: return false; // Bushes
        case 0x09: case 0x0a: return false; // Nest, pipe
        default: return true;
    }
}

// Check a test point (x_frac, y_frac) within a tile for obstruction.
static bool is_point_obstructed(uint8_t tile_type, bool flip_h, bool flip_v,
                                 uint8_t x_frac, uint8_t y_frac) {
    int pattern_idx = get_obstruction_pattern_index(tile_type, flip_h, flip_v);
    uint8_t y_offset = get_tile_y_offset(tile_type, flip_v);
    return Obstruction::is_obstructed(pattern_idx, x_frac, y_frac,
                                      y_offset, flip_v);
}

// Probe a point in the tile containing (tile_x, tile_y). The tile_y adjustment
// (and the tile-passable early-out) is handled by the caller; this just looks
// up the tile and its pattern and runs the obstruction test.
static bool tile_obstructs_point(const Landscape& landscape,
                                 uint8_t tile_x, uint8_t tile_y,
                                 uint8_t x_frac, uint8_t y_frac) {
    if (!is_tile_solid(landscape, tile_x, tile_y)) return false;
    uint8_t tile = landscape.get_tile(tile_x, tile_y);
    uint8_t type = tile & TileFlip::TYPE_MASK;
    bool fh = (tile & TileFlip::HORIZONTAL) != 0;
    bool fv = (tile & TileFlip::VERTICAL) != 0;
    return is_point_obstructed(type, fh, fv, x_frac, y_frac);
}

// Returns true if the object's point position (x, y) is inside solid geometry.
// Used by the simple undo-on-overlap resolution in update_player.
TileCollisionResult check_tile_collision(const Landscape& landscape, const Object& obj) {
    TileCollisionResult result;

    uint8_t ox = obj.x.whole;
    uint8_t oy = obj.y.whole;
    uint8_t ox_frac = obj.x.fraction;
    uint8_t oy_frac = obj.y.fraction;

    // --- Inside current tile: is the point itself obstructed? ---
    if (tile_obstructs_point(landscape, ox, oy, ox_frac, oy_frac)) {
        result.any = true;
    }

    // --- Below: foot at (ox, oy+1), top-of-tile sample. ---
    // Use a slightly-inside y so surfaces (threshold==0 for fully-solid tiles)
    // register as obstructed.
    if (tile_obstructs_point(landscape, ox,
                             static_cast<uint8_t>(oy + 1),
                             ox_frac, 0x04)) {
        result.bottom = true;
        result.any = true;
        result.push_y = -1;
    }

    // --- Above: sample bottom of tile above at ox_frac. ---
    if (tile_obstructs_point(landscape, ox,
                             static_cast<uint8_t>(oy - 1),
                             ox_frac, 0xfc)) {
        result.top = true;
        result.any = true;
        result.push_y = 1;
    }

    // --- Right: sample left edge of tile to right, at object's y_frac. ---
    if (tile_obstructs_point(landscape,
                             static_cast<uint8_t>(ox + 1), oy,
                             0x04, oy_frac)) {
        result.right = true;
        result.any = true;
        result.push_x = -1;
    }

    // --- Left: sample right edge of tile to left. ---
    if (tile_obstructs_point(landscape,
                             static_cast<uint8_t>(ox - 1), oy,
                             0xfc, oy_frac)) {
        result.left = true;
        result.any = true;
        result.push_x = 1;
    }

    if (result.any) {
        // Coarse angle from push direction; used only by some behaviors.
        if      (result.push_x > 0 && result.push_y == 0) result.angle = 0x00;
        else if (result.push_x > 0 && result.push_y > 0)  result.angle = 0x20;
        else if (result.push_y > 0)                       result.angle = 0x40;
        else if (result.push_x < 0 && result.push_y > 0)  result.angle = 0x60;
        else if (result.push_x < 0)                       result.angle = 0x80;
        else if (result.push_x < 0 && result.push_y < 0)  result.angle = 0xA0;
        else if (result.push_y < 0)                       result.angle = 0xC0;
        else                                              result.angle = 0xE0;
    }
    return result;
}

// Object-object collision: broad phase (Chebyshev distance <=2 tiles),
// narrow phase (actual overlap check at tile level).
ObjectCollisionResult check_object_collision(
    const Object& obj, int slot,
    const std::array<Object, 16>& all_objects) {

    ObjectCollisionResult result;

    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        if (i == slot) continue;
        const Object& other = all_objects[i];
        if (!other.is_active()) continue;

        // Check intangibility
        uint8_t idx = static_cast<uint8_t>(other.type);
        if (idx < static_cast<uint8_t>(ObjectType::COUNT)) {
            if (object_types_flags[idx] & ObjectTypeFlags::INTANGIBLE) continue;
        }

        // Broad phase: Chebyshev distance <= 2 tiles
        int8_t dx = static_cast<int8_t>(obj.x.whole - other.x.whole);
        int8_t dy = static_cast<int8_t>(obj.y.whole - other.y.whole);

        if (std::abs(dx) > 2 || std::abs(dy) > 2) continue;

        // Narrow phase: same tile or adjacent = collision
        // (Full pixel-precise check would use sprite dimensions;
        //  tile-level check is sufficient for gameplay)
        if (std::abs(dx) <= 1 && std::abs(dy) <= 1) {
            result.collided = true;
            result.other_slot = i;
            result.push_x = (dx > 0) ? 1 : (dx < 0) ? -1 : 0;
            result.push_y = (dy > 0) ? 1 : (dy < 0) ? -1 : 0;
            return result; // Return first collision found
        }
    }

    return result;
}

} // namespace Collision
