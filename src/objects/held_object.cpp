#include "objects/held_object.h"
#include "objects/object_data.h"

namespace HeldObject {

// Port of &1afd-&1b48.
// The original uses sprite width/height tables to precisely position the held
// object. Without the full sprite dimension tables extracted, we use reasonable
// defaults (player ~16px wide, ~16px tall, held objects ~8px wide/tall).
// The key behaviors are: vertical centering and horizontal offset by facing direction.
void update_position(Object& held, const Object& player) {
    // Y positioning: center held object with player vertically.
    // The original computes: (player_height - held_height) / 2, rounded to pixel,
    // then adds to player y_fraction with carry to y_whole.
    // With typical heights being similar, the offset is small.
    // For now, match player Y position directly (offset ~0 for same-height objects).
    held.y.fraction = player.y.fraction;
    held.y.whole = player.y.whole;

    // X positioning: offset to the side the player is facing.
    // The original adds player_width (in fractions, ~0x10 per pixel) to player x.
    // For facing right: held.x = player.x + (player_width + 0x10)
    // For facing left: held.x = player.x - (held_width + 0x10)
    // A pixel is 0x10 in fraction units. Player width ~16px = 0x100 = 1 tile.
    if (player.is_flipped_h()) {
        // Facing left: place held object 1 tile to the left
        held.x.fraction = player.x.fraction;
        held.x.whole = player.x.whole - 1;
    } else {
        // Facing right: place held object 1 tile to the right
        held.x.fraction = player.x.fraction;
        held.x.whole = player.x.whole + 1;
    }

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
