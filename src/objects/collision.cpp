#include "objects/collision.h"
#include "objects/object_data.h"
#include "world/tile_data.h"
#include "world/obstruction.h"
#include "rendering/sprite_atlas.h"
#include "core/types.h"
#include <cstdlib>

namespace Collision {

bool is_tile_type_solid(uint8_t type) {
    // Only genuinely non-obstructing tile types short-circuit to false.
    // Everything else defers to the pattern check (tile_threshold_at_x +
    // is_obstructed), which correctly produces "no obstruction" for
    // patterns that are naturally empty. Marking something like SWITCH
    // or NEST as non-solid here would bypass the pattern and let the
    // player walk straight through a tile that has a real obstruction
    // band (switch has `tiles_obstruction_y_offsets[0x08] = 0xb0` — a
    // thin surface at the top of the tile the player can stand on).
    //
    // METAL_DOOR / STONE_DOOR are listed so that callers that haven't
    // gone through substitute_door_for_obstruction still treat the raw
    // door tile as passable. Player motion always substitutes first so
    // it never sees the raw door types here.
    switch (static_cast<TileType>(type & TileFlip::TYPE_MASK)) {
        case TileType::SPACE:
        case TileType::VARIABLE_WIND:
        case TileType::CONSTANT_WIND:
        case TileType::WATER:
        case TileType::INVISIBLE_SWITCH:
        case TileType::SPACE_WITH_OBJECT_FROM_DATA:
        case TileType::SPACE_WITH_OBJECT_FROM_TYPE:
        case TileType::METAL_DOOR:
        case TileType::STONE_DOOR:
            return false;
        default:
            return true;
    }
}

bool is_tile_solid(const Landscape& landscape, uint8_t tile_x, uint8_t tile_y) {
    uint8_t tile = landscape.get_tile(tile_x, tile_y);
    return is_tile_type_solid(tile & TileFlip::TYPE_MASK);
}

// Check a test point (x_frac, y_frac) within a tile for obstruction.
// Pattern index + y-offset use the landscape's raw flip_v (&247c/&2462);
// the above/below test uses the effective collision flip — landscape
// flip_v XOR the &04ab bit-7 per-type override (&2477).
static bool is_point_obstructed(uint8_t tile_type, bool flip_h, bool flip_v,
                                 uint8_t x_frac, uint8_t y_frac) {
    int pattern_idx = get_obstruction_pattern_index(tile_type, flip_h, flip_v);
    uint8_t y_offset = get_tile_y_offset(tile_type, flip_v);
    bool coll_fv = flip_v ^ tile_obstruction_v_flip_bit(tile_type);
    return Obstruction::is_obstructed(pattern_idx, x_frac, y_frac,
                                      y_offset, coll_fv);
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

// Returns the object's extent in 16-bit position units (whole*256 + fraction).
// Matches the 6502's this_object_width / this_object_height — the raw table
// bytes from sprites_width_and_horizontal_flip_table (atlas: (w-1)*16) and
// sprites_height_and_vertical_flip_table (atlas: (h-1)*8), which live in the
// x.fraction / y.fraction space directly.
static int sprite_width_units(uint8_t sprite) {
    if (sprite > 0x7c) return 0;
    return (sprite_atlas[sprite].w > 0 ? sprite_atlas[sprite].w - 1 : 0) * 16;
}
static int sprite_height_units(uint8_t sprite) {
    if (sprite > 0x7c) return 0;
    return (sprite_atlas[sprite].h > 0 ? sprite_atlas[sprite].h - 1 : 0) * 8;
}

// Object-object collision: port of &2a64 check_for_collisions.
// Broad phase: within +/- 2 tiles in both x and y.
// Narrow phase: pixel-precise AABB overlap using sprite widths/heights.
// This matches the 6502 — a small bullet next to a large player does NOT
// register as touching unless their rectangles actually overlap.
ObjectCollisionResult check_object_collision(
    const Object& obj, int slot,
    const std::array<Object, 16>& all_objects) {

    ObjectCollisionResult result;

    int this_x  = obj.x.whole * 256 + obj.x.fraction;
    int this_y  = obj.y.whole * 256 + obj.y.fraction;
    int this_w  = sprite_width_units(obj.sprite);
    int this_h  = sprite_height_units(obj.sprite);

    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        if (i == slot) continue;
        const Object& other = all_objects[i];
        if (!other.is_active()) continue;

        // Intangibility gate (&2b2d)
        uint8_t idx = static_cast<uint8_t>(other.type);
        if (idx < static_cast<uint8_t>(ObjectType::COUNT)) {
            if (object_types_flags[idx] & ObjectTypeFlags::INTANGIBLE) continue;
        }

        // Broad phase in whole-tile units (&2a7e-&2a90): x within +/-2 tiles,
        // y within +/-2 tiles of this object.
        int8_t tdx = static_cast<int8_t>(obj.x.whole - other.x.whole);
        int8_t tdy = static_cast<int8_t>(obj.y.whole - other.y.whole);
        if (std::abs(tdx) > 2 || std::abs(tdy) > 2) continue;

        // Pixel-precise AABB overlap in 16-bit position units.
        int other_x = other.x.whole * 256 + other.x.fraction;
        int other_y = other.y.whole * 256 + other.y.fraction;
        int other_w = sprite_width_units(other.sprite);
        int other_h = sprite_height_units(other.sprite);

        // No overlap if either object is entirely left of / above the other.
        // The 6502 treats "exactly touching" as no overlap (&2ae9 BEQ skip),
        // hence the strict < comparisons.
        if (other_x + other_w <= this_x)  continue;
        if (this_x  + this_w  <= other_x) continue;
        if (other_y + other_h <= this_y)  continue;
        if (this_y  + this_h  <= other_y) continue;

        result.collided = true;
        result.other_slot = i;
        int dx = this_x - other_x;
        int dy = this_y - other_y;
        result.push_x = (dx > 0) ? 1 : (dx < 0) ? -1 : 0;
        result.push_y = (dy > 0) ? 1 : (dy < 0) ? -1 : 0;
        return result; // First collision wins, matching the 6502.
    }

    return result;
}

bool overlaps_solid_object(const Object& obj, int self_slot,
                            const std::array<Object, 16>& all_objects) {
    int this_x = obj.x.whole * 256 + obj.x.fraction;
    int this_y = obj.y.whole * 256 + obj.y.fraction;
    int this_w = sprite_width_units(obj.sprite);
    int this_h = sprite_height_units(obj.sprite);

    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; ++i) {
        if (i == self_slot) continue;
        const Object& other = all_objects[i];
        if (!other.is_active()) continue;

        // Weight-7 non-INTANGIBLE statics block — doors, switches, etc.
        // The 6502's apply_collision_to_objects_velocities (&2bb6) uses
        // the mass ratio to transfer velocity between colliders; with
        // weight diff 6 (player 1 vs door 7), the velocity transfer is
        // ~2^-6 ≈ 0 from the door's side, so the player is effectively
        // reflected. We approximate that with a hard position revert.
        uint8_t tidx = static_cast<uint8_t>(other.type);
        uint8_t tflags = (tidx < static_cast<uint8_t>(ObjectType::COUNT))
                         ? object_types_flags[tidx] : 0;
        if ((tflags & ObjectTypeFlags::WEIGHT_MASK) < 7) continue;
        if (tflags & ObjectTypeFlags::INTANGIBLE) continue;

        // Doors: OPENING set means the sprite has slid far enough out of
        // the way that the passage is functionally clear. update_door
        // ticks obj.x.fraction between 0x10 (open) and 0x0f+carry
        // (closed), so the door sprite physically moves; but the AABB
        // check here is an extra safeguard that short-circuits for
        // opening/open doors so the player can walk through.
        if (tidx >= 0x3c && tidx <= 0x3f) {
            if (other.tertiary_data_offset & 0x02) continue; // opening/open
        }

        int8_t tdx = static_cast<int8_t>(obj.x.whole - other.x.whole);
        int8_t tdy = static_cast<int8_t>(obj.y.whole - other.y.whole);
        if (std::abs(tdx) > 2 || std::abs(tdy) > 2) continue;

        int other_x = other.x.whole * 256 + other.x.fraction;
        int other_y = other.y.whole * 256 + other.y.fraction;
        int other_w = sprite_width_units(other.sprite);
        int other_h = sprite_height_units(other.sprite);

        if (other_x + other_w <= this_x)  continue;
        if (this_x  + this_w  <= other_x) continue;
        if (other_y + other_h <= this_y)  continue;
        if (this_y  + this_h  <= other_y) continue;
        return true;
    }
    return false;
}

uint8_t substitute_door_for_obstruction(
        uint8_t tile_and_flip, int data_offset,
        const std::array<Object, 16>& all_objects,
        uint8_t tertiary_byte_fallback) {
    uint8_t type = tile_and_flip & TileFlip::TYPE_MASK;
    if (type != static_cast<uint8_t>(TileType::METAL_DOOR) &&
        type != static_cast<uint8_t>(TileType::STONE_DOOR)) {
        return tile_and_flip;
    }

    // Read the live door data byte. If a primary owns this tertiary
    // slot, that's authoritative (update_door mutates it each frame).
    // Otherwise fall back to the tertiary store (with the spawn gate
    // stripped — bit 7 is for spawning, not door state).
    uint8_t data = static_cast<uint8_t>(tertiary_byte_fallback & 0x7f);
    if (data_offset > 0) {
        for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; ++i) {
            const Object& obj = all_objects[i];
            if (obj.is_active() &&
                obj.tertiary_slot == static_cast<uint8_t>(data_offset)) {
                data = obj.tertiary_data_offset;
                break;
            }
        }
    }

    // &3e91-&3e94 door_tiles_table: closed → STONE_SLOPE_78, open → SPACE.
    // The 6502's OPENING bit is bit 1 of the door's data byte (&4cdf),
    // and it's set while the door is opening or fully open.
    bool opening = (data & 0x02) != 0;
    // The 6502 writes the raw door_tiles_table byte directly into
    // tile_type_and_flip (&3ec2 STA &08). There is NO flip-bit inheritance
    // from the original door tile — and that's load-bearing: a v-flipped
    // STONE_SLOPE_78 has an inverted threshold that collapses its solid
    // region, so a closed v-flipped door would be walked through. Match
    // the original: return the raw type with no flip bits.
    return opening
        ? static_cast<uint8_t>(TileType::SPACE)
        : static_cast<uint8_t>(TileType::STONE_SLOPE_78);
}

} // namespace Collision
