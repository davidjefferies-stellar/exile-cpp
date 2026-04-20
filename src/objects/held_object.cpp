#include "objects/held_object.h"
#include "objects/object_data.h"
#include "rendering/sprite_atlas.h"
#include <cstdint>

namespace HeldObject {

// Port of &1afd-&1b48. Positions the held object flush to the player's side
// (right if facing right, left if facing left) with vertical centres aligned.
//
// Unit convention (matching Fixed8_8 / the 6502 fraction arithmetic):
//   1 screen pixel in X = 0x10 x-fraction units   (16 px per tile)
//   1 screen row    in Y = 8    y-fraction units  (32 rows per tile)
void update_position(Object& held, const Object& player) {
    uint8_t player_w_px = (player.sprite <= 0x7c) ? sprite_atlas[player.sprite].w : 5;
    uint8_t player_h_px = (player.sprite <= 0x7c) ? sprite_atlas[player.sprite].h : 22;
    uint8_t held_w_px   = (held.sprite   <= 0x7c) ? sprite_atlas[held.sprite].w   : 4;
    uint8_t held_h_px   = (held.sprite   <= 0x7c) ? sprite_atlas[held.sprite].h   : 6;

    // X: signed offset in fraction units, applied across (whole, fraction).
    int32_t x_offset;
    if (player.is_flipped_h()) {
        // Facing left: shift so the held object's right edge touches the
        // player's left edge (port of &1b2b..&1b30).
        x_offset = -static_cast<int32_t>(held_w_px) * 0x10;
    } else {
        // Facing right: shift so held left edge touches player right edge
        // (port of &1b1f..&1b24).
        x_offset = static_cast<int32_t>(player_w_px) * 0x10;
    }
    int32_t x_combined = static_cast<int32_t>(player.x.whole) * 0x100
                       + static_cast<int32_t>(player.x.fraction)
                       + x_offset;
    held.x.whole    = static_cast<uint8_t>((x_combined >> 8) & 0xff);
    held.x.fraction = static_cast<uint8_t>(x_combined & 0xff);

    // Y: align vertical centres (port of &1b00..&1b1b). Because sprites are
    // bottom-anchored, a smaller held object needs a larger world Y to
    // line its centre up with the player's.
    int16_t diff_rows = (static_cast<int16_t>(player_h_px) -
                         static_cast<int16_t>(held_h_px)) / 2;
    int32_t y_offset = static_cast<int32_t>(diff_rows) * 8;
    int32_t y_combined = static_cast<int32_t>(player.y.whole) * 0x100
                       + static_cast<int32_t>(player.y.fraction)
                       + y_offset;
    held.y.whole    = static_cast<uint8_t>((y_combined >> 8) & 0xff);
    held.y.fraction = static_cast<uint8_t>(y_combined & 0xff);

    // Sync velocity from player (port of &1b45 JSR set_this_object_velocities_from_object_Y)
    held.velocity_x = player.velocity_x;
    held.velocity_y = player.velocity_y;

    // Sync horizontal flip (port of &1b48-&1b4b)
    held.flags = (held.flags & ~ObjectFlags::FLIP_HORIZONTAL) |
                 (player.flags & ObjectFlags::FLIP_HORIZONTAL);
}

// Port of &1ca9: check if held object drifted too far from expected position.
// Original checks 3 tiles horizontally and 6 vertically (0x30 in sub-tile units
// which at 0x10/pixel = 3 pixels, but in tile units = ~3 tiles).
bool should_drop(const Object& held, const Object& player) {
    int8_t dx = static_cast<int8_t>(held.x.whole - player.x.whole);
    int8_t dy = static_cast<int8_t>(held.y.whole - player.y.whole);

    uint8_t abs_dx = (dx < 0) ? static_cast<uint8_t>(-dx) : static_cast<uint8_t>(dx);
    uint8_t abs_dy = (dy < 0) ? static_cast<uint8_t>(-dy) : static_cast<uint8_t>(dy);

    return abs_dx > 3 || abs_dy > 6;
}

void pickup(Object& held, Object& player, uint8_t& held_slot, int slot) {
    held_slot = static_cast<uint8_t>(slot);
    update_position(held, player);
}

void drop(Object& held, Object& player, uint8_t& held_slot) {
    (void)held; (void)player;
    held_slot = 0x80; // No object held (bit 7 set = negative in 6502 terms)
}

bool is_pickupable(ObjectType type) {
    uint8_t idx = static_cast<uint8_t>(type);
    if (idx >= static_cast<uint8_t>(ObjectType::COUNT)) return false;
    return (object_types_palette_and_pickup[idx] & 0x80) != 0;
}

} // namespace HeldObject
