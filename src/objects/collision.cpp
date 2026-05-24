#include "objects/collision.h"
#include "objects/object_data.h"
#include "objects/object_manager.h"
#include "world/tile_data.h"
#include "world/obstruction.h"
#include "world/tertiary.h"
#include "rendering/sprite_atlas.h"
#include "core/types.h"
#include <cstdlib>

namespace Collision {

bool is_tile_type_solid(uint8_t type) {
    // Only short-circuit genuinely empty types; everything else defers to
    // the pattern check so tiles with thin obstruction bands (switches,
    // nests) still block correctly. Raw door types listed as passable —
    // callers must run substitute_door_for_obstruction first.
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
bool point_in_tile_solid(const Landscape& landscape,
                          uint8_t tile_x, uint8_t tile_y,
                          uint8_t x_frac, uint8_t y_frac) {
    if (!is_tile_solid(landscape, tile_x, tile_y)) return false;
    uint8_t tile = landscape.get_tile(tile_x, tile_y);
    uint8_t type = tile & TileFlip::TYPE_MASK;
    bool fh = (tile & TileFlip::HORIZONTAL) != 0;
    bool fv = (tile & TileFlip::VERTICAL) != 0;
    return is_point_obstructed(type, fh, fv, x_frac, y_frac);
}

bool tile_and_flip_obstructs_point(uint8_t tile_and_flip,
                                    uint8_t x_frac, uint8_t y_frac) {
    uint8_t type = tile_and_flip & TileFlip::TYPE_MASK;
    if (!is_tile_type_solid(type)) return false;
    bool fh = (tile_and_flip & TileFlip::HORIZONTAL) != 0;
    bool fv = (tile_and_flip & TileFlip::VERTICAL) != 0;
    return is_point_obstructed(type, fh, fv, x_frac, y_frac);
}

// File-local alias kept to avoid touching the handful of call sites below
// that spell it `tile_obstructs_point`.
static bool tile_obstructs_point(const Landscape& landscape,
                                 uint8_t tile_x, uint8_t tile_y,
                                 uint8_t x_frac, uint8_t y_frac) {
    return point_in_tile_solid(landscape, tile_x, tile_y, x_frac, y_frac);
}

bool point_in_tile_solid_with_doors(
        const Landscape& landscape, ObjectManager& mgr,
        uint8_t tile_x, uint8_t tile_y,
        uint8_t x_frac, uint8_t y_frac) {
    ResolvedTile r = resolve_tile_with_tertiary(landscape, tile_x, tile_y);
    uint8_t tile = substitute_door_for_obstruction(
        r.tile_and_flip, r.data_offset,
        reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(mgr.object(0)),
        mgr.tertiary_data_byte(r.data_offset));
    if (!is_tile_type_solid(tile & TileFlip::TYPE_MASK)) return false;
    return tile_and_flip_obstructs_point(tile, x_frac, y_frac);
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
    if (sprite > 0x80) return 0;
    return (sprite_atlas[sprite].w > 0 ? sprite_atlas[sprite].w - 1 : 0) * 16;
}
static int sprite_height_units(uint8_t sprite) {
    if (sprite > 0x80) return 0;
    return (sprite_atlas[sprite].h > 0 ? sprite_atlas[sprite].h - 1 : 0) * 8;
}

// Object-object collision: port of &2a64 check_for_collisions.
// Broad phase: within +/- 2 tiles in both x and y.
// Narrow phase: pixel-precise AABB overlap using sprite widths/heights.
// This matches the 6502 — a small bullet next to a large player does NOT
// register as touching unless their rectangles actually overlap.
ObjectCollisionResult check_object_collision(
    const Object& obj, int slot,
    const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>& all_objects) {

    ObjectCollisionResult result;

    int this_x  = obj.x.whole * 256 + obj.x.fraction;
    int this_y  = obj.y.whole * 256 + obj.y.fraction;
    int this_w  = sprite_width_units(obj.sprite);
    int this_h  = sprite_height_units(obj.sprite);

    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        if (i == slot) continue;
        const Object& other = all_objects[i];
        if (!other.is_active()) continue;

        // No intangibility filter here. The 6502 sets objects_touching for
        // both sides at &2b11-&2b27 BEFORE the INTANGIBLE check at
        // &2b27-&2b2d — intangibility only skips the velocity-transfer /
        // physics path that follows. Filtering intangibles out of the
        // touching detection makes a door miss its shooter the moment the
        // bullet's slot mutates to EXPLOSION (intangible) on impact:
        // door.touching stays 0x80 and update_door's touch-toggle never
        // fires, so unlocked doors never open from gunfire.

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

bool push_out_of_overlap(Object& obj, const Object& blocker) {
    int p_x = obj.x.whole * 256 + obj.x.fraction;
    int p_y = obj.y.whole * 256 + obj.y.fraction;
    int p_w = sprite_width_units(obj.sprite);
    int p_h = sprite_height_units(obj.sprite);
    int o_x = blocker.x.whole * 256 + blocker.x.fraction;
    int o_y = blocker.y.whole * 256 + blocker.y.fraction;
    int o_w = sprite_width_units(blocker.sprite);
    int o_h = sprite_height_units(blocker.sprite);

    // Per-axis overlap depths (how far to shift `obj` to clear `blocker`
    // in each direction). Matches the &2b51 find_smallest_overlap loop
    // walking collision_plus_x / collision_minus_x / collision_plus_y /
    // collision_minus_y. <= 0 on any axis means no overlap.
    int shift_right = (o_x + o_w) - p_x;     // obj's left edge inside blocker -> shift right
    int shift_left  = (p_x + p_w) - o_x;     // obj's right edge inside blocker -> shift left
    int shift_down  = (o_y + o_h) - p_y;
    int shift_up    = (p_y + p_h) - o_y;
    if (shift_right <= 0 || shift_left <= 0 ||
        shift_down  <= 0 || shift_up   <= 0) {
        return false;
    }

    // Pick the axis with the smallest overlap — the path of least
    // resistance out of the blocker.
    int min_shift = shift_right;
    int axis = 0;  // 0=right, 1=left, 2=down, 3=up
    if (shift_left < min_shift) { min_shift = shift_left;  axis = 1; }
    if (shift_down < min_shift) { min_shift = shift_down;  axis = 2; }
    if (shift_up   < min_shift) { min_shift = shift_up;    axis = 3; }

    switch (axis) {
        case 0: p_x += min_shift; obj.velocity_x = static_cast<int8_t>(obj.velocity_x + 2); break;
        case 1: p_x -= min_shift; obj.velocity_x = static_cast<int8_t>(obj.velocity_x - 2); break;
        case 2: p_y += min_shift; obj.velocity_y = static_cast<int8_t>(obj.velocity_y + 2); break;
        case 3: p_y -= min_shift; obj.velocity_y = static_cast<int8_t>(obj.velocity_y - 2); break;
    }

    obj.x.whole    = static_cast<uint8_t>((p_x >> 8) & 0xff);
    obj.x.fraction = static_cast<uint8_t>(p_x & 0xff);
    obj.y.whole    = static_cast<uint8_t>((p_y >> 8) & 0xff);
    obj.y.fraction = static_cast<uint8_t>(p_y & 0xff);
    return true;
}

int overlapping_solid_slot(const Object& obj, int self_slot,
                            const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>& all_objects,
                            int skip_slot) {
    int this_x = obj.x.whole * 256 + obj.x.fraction;
    int this_y = obj.y.whole * 256 + obj.y.fraction;
    int this_w = sprite_width_units(obj.sprite);
    int this_h = sprite_height_units(obj.sprite);
    uint8_t self_weight = obj.weight();

    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; ++i) {
        if (i == self_slot) continue;
        if (i == skip_slot) continue;
        const Object& other = all_objects[i];
        if (!other.is_active()) continue;

        uint8_t tidx = static_cast<uint8_t>(other.type);
        uint8_t tflags = (tidx < static_cast<uint8_t>(ObjectType::COUNT))
                         ? object_types_flags[tidx] : 0;
        uint8_t other_weight = tflags & ObjectTypeFlags::WEIGHT_MASK;
        if (other_weight <= self_weight) continue;
        if (tflags & ObjectTypeFlags::INTANGIBLE) continue;

        if (tidx >= 0x3c && tidx <= 0x3f) {
            if (other.tertiary_data_offset & 0x02) continue;
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
        return i;
    }
    return -1;
}

bool overlaps_solid_object(const Object& obj, int self_slot,
                            const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>& all_objects,
                            int skip_slot) {
    int this_x = obj.x.whole * 256 + obj.x.fraction;
    int this_y = obj.y.whole * 256 + obj.y.fraction;
    int this_w = sprite_width_units(obj.sprite);
    int this_h = sprite_height_units(obj.sprite);
    uint8_t self_weight = obj.weight();

    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; ++i) {
        if (i == self_slot) continue;
        // Held-object exclusion (port of &2afd-&2b0e). The carried primary's
        // AABB overlaps the player's whenever HeldObject::update_position
        // pins it flush; without this skip, a heavier held (coronium
        // boulder, weight 5) blocks the player's own X motion.
        if (i == skip_slot) continue;
        const Object& other = all_objects[i];
        if (!other.is_active()) continue;

        // &2bb6 apply_collision_to_objects_velocities mass-ratio model:
        // strictly-heavier collider blocks the lighter side (emulated as
        // a position revert). INTANGIBLE (0x80) and open/opening doors
        // never block.
        uint8_t tidx = static_cast<uint8_t>(other.type);
        uint8_t tflags = (tidx < static_cast<uint8_t>(ObjectType::COUNT))
                         ? object_types_flags[tidx] : 0;
        uint8_t other_weight = tflags & ObjectTypeFlags::WEIGHT_MASK;
        if (other_weight <= self_weight) continue;   // lighter or equal -> no block
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

// Sign-preserving signed halve: floor toward -infinity. Matches the
// 6502's CMP #&80 / ROR A idiom (&2bca, &2c05) that brings the prior
// sign back into bit 7 after the shift.
static int asr1_floor(int x) {
    if (x >= 0) return x / 2;
    return -((-x + 1) / 2);
}

// Apply one side of the transfer (port of &2bc6-&2bed). The transfer is
// halved and, if the smallest overlap is in this axis, the original is
// added back to give a 1.5× kick (the &2bcf ADC &a2,X "doubling" path).
// Result is clamped to signed 8-bit per &327f prevent_overflow.
static int process_one_side(int transfer, bool double_it, int start_v) {
    int processed = asr1_floor(transfer);
    if (double_it) processed += transfer;
    int sum = start_v + processed;
    if (sum >  127) sum =  127;
    if (sum < -128) sum = -128;
    return sum;
}

// Port of &2bee calculate_transfer_velocities + &2bc6 apply_collision_
// to_object_velocity. `smallest_overlap_in_this_axis` mirrors the impact
// axis bits in collision_velocity_direction_flags_table (&29dc) —
// transfers on the impact axis are doubled.
VelocityTransfer apply_mass_ratio_velocity(
        int8_t this_v_in, int8_t other_v_in,
        uint8_t this_weight, uint8_t other_weight,
        bool smallest_overlap_in_this_axis) {

    int half_diff = asr1_floor(int(this_v_in) - int(other_v_in));

    int wdiff = int(this_weight) - int(other_weight);
    int wdiff_abs = wdiff < 0 ? -wdiff : wdiff;
    int shifts = wdiff_abs == 0 ? 1 : wdiff_abs;
    int lesser = half_diff;
    for (int i = 0; i < shifts; i++) lesser = asr1_floor(lesser);
    // &2c09-&2c0b CMP #&80 / ADC #0: round up by 1 if negative.
    if (lesser < 0) lesser += 1;
    int greater = lesser - half_diff;

    int this_xfer  = (wdiff > 0) ?  greater : -lesser;
    int other_xfer = (wdiff > 0) ?  lesser  : -greater;

    int this_out  = process_one_side(this_xfer,  smallest_overlap_in_this_axis, this_v_in);
    int other_out = process_one_side(other_xfer, smallest_overlap_in_this_axis, other_v_in);

    VelocityTransfer out;
    out.this_v  = static_cast<int8_t>(this_out);
    out.other_v = static_cast<int8_t>(other_out);
    return out;
}

uint8_t substitute_door_for_obstruction(
        uint8_t tile_and_flip, int data_offset,
        const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>& all_objects,
        uint8_t tertiary_byte_fallback) {
    uint8_t type = tile_and_flip & TileFlip::TYPE_MASK;
    if (type != static_cast<uint8_t>(TileType::METAL_DOOR) &&
        type != static_cast<uint8_t>(TileType::STONE_DOOR)) {
        return tile_and_flip;
    }

    // Live primary owns authoritative state; tertiary fallback keeps
    // bit 7 (needs-creating gate) — &3ead reads it BEFORE stripping.
    // EXPLOSION-typed slot here = a door that hit step 12's energy=0
    // mutation; its tertiary_data_offset has been clobbered into a
    // duration counter, so DON'T copy it forward. update_door's
    // every-frame mirror at environment.cpp:240 puts the destruction-
    // frame data (with SLOW_OR_DESTROYED + MOVING set) into the
    // tertiary store before step 12 fires, so the fallback below
    // carries the right state on its own.
    uint8_t raw_data = tertiary_byte_fallback;
    if (data_offset > 0) {
        for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; ++i) {
            const Object& obj = all_objects[i];
            if (obj.is_active() &&
                obj.tertiary_slot == static_cast<uint16_t>(data_offset)) {
                if (obj.type != ObjectType::EXPLOSION) {
                    // Primaries store the data byte with bit 7 already
                    // stripped by the spawn path (&408a in 6502).
                    raw_data = obj.tertiary_data_offset;
                }
                break;
            }
        }
    }

    // Port of &3ead-&3eb5 "is door open?" test:
    //   &3ead BMI &3eb0   ; bit 7 set → still tertiary, skip LSR
    //   &3eaf LSR A       ; primary → shift bits right by 1
    //   &3eb0 AND #&02    ; mask "OPENING slot" after shift
    // For a tertiary entry that mask reads OPENING (bit 1). For a
    // primary (bit 7 already stripped, LSR shifts MOVING into bit 1)
    // it actually reads MOVING (bit 2 of the original byte). MOVING is
    // asserted every frame update_door runs and only cleared at &4d09
    // stop_door — so a closing door reports "open" to obstruction
    // queries right up until it parks at the closed end.
    bool still_tertiary = (raw_data & 0x80) != 0;
    bool door_open = still_tertiary
        ? (raw_data & 0x02) != 0    // tertiary: read OPENING
        : (raw_data & 0x04) != 0;   // primary:  read MOVING
    bool fh = (tile_and_flip & TileFlip::HORIZONTAL) != 0;
    bool fv = (tile_and_flip & TileFlip::VERTICAL)   != 0;
    bool vertical = (fh != fv);

    // CRITICAL: preserve ORIGINAL door flip bits. The 6502 stores bare
    // type to &08 but &2462 reads &09 (original flips) for the y_offset
    // nibble + &247a collision-flip XOR — without this the substitute's
    // obstruction band lands on the wrong half of the tile.
    uint8_t flip_bits = tile_and_flip & ~TileFlip::TYPE_MASK;
    // 6502 &3eaf-&3eb0 only tests OPENING (tertiary) / MOVING (primary).
    // A destroyed door has MOVING latched on by update_door's last
    // mirror at environment.cpp:233 (`if energy==0 data |= MOVING`),
    // which is what makes the obstruction read "open" without a
    // separate SLOW_OR_DESTROYED check.
    if (door_open) {
        return static_cast<uint8_t>(TileType::SPACE) | flip_bits;
    }
    uint8_t sub_type = vertical
        ? static_cast<uint8_t>(TileType::STONE_SLOPE_78)
        : static_cast<uint8_t>(TileType::SPACESHIP_WALL_HORIZONTAL_QUARTER);
    return static_cast<uint8_t>(sub_type | flip_bits);
}

} // namespace Collision
