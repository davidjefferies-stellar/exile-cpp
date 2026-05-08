#include "world/water.h"
#include "world/tile_data.h"
#include "objects/object_data.h"
#include "rendering/sprite_atlas.h"
#include "core/types.h"

namespace Water {

// Port of &2cbc-&2cdb: get_waterline_for_x.
// Searches x-ranges from high to low, returns the waterline Y for that range.
// Also clamps to the Triax lab waterline (range 1) if it's lower.
uint8_t get_waterline_y(uint8_t x) {
    // Find which x-range this position falls into
    int range = 0;
    for (int i = 3; i >= 0; i--) {
        if (x >= waterline_x_ranges_x[i]) {
            range = i;
            break;
        }
    }

    uint8_t waterline = waterline_initial_y[range];

    // Clamp: if the waterline is deeper (higher Y) than Triax's lab (range 1),
    // use Triax's lab level instead. This prevents water from being too deep
    // in areas connected to the lab.
    uint8_t lab_waterline = waterline_initial_y[1];
    if (waterline > lab_waterline) {
        waterline = lab_waterline;
    }

    return waterline;
}

bool is_underwater(const Landscape& landscape, uint8_t x, uint8_t y) {
    // 6502 at &2f03-&2f39 checks the tile (TileType::WATER) first for
    // upper-world ponds, then falls back to the global waterline.
    uint8_t tile = landscape.get_tile(x, y);
    if ((tile & TileFlip::TYPE_MASK) == static_cast<uint8_t>(TileType::WATER)) {
        return true;
    }
    return y >= get_waterline_y(x);
}

// 6502's calculate_seven_eighths at &3235: rounds |v| up to the next
// multiple of 8, then drops 1/8 — so |v| strictly decreases for any
// non-zero v. Used by &3222 dampen_this_object_velocities every four
// frames an object is in water.
static int8_t seven_eighths(int8_t v) {
    int abs_v = v < 0 ? -int(v) : int(v);
    int eighth = (abs_v + 7) >> 3;
    int new_abs = abs_v - eighth;
    return static_cast<int8_t>(v < 0 ? -new_abs : new_abs);
}

// Port of &2f01-&2f8a apply_buoyancy_loop + the &2f85 four-frame damping.
// Total velocity_y DECs when fully submerged: weight 0/1→5, 2→4, 3→3,
// 4→2, 5+→0.
void apply_water_effects(const Landscape& landscape, Object& obj,
                         uint8_t weight, bool every_four_frames) {
    int sprite_h_units = (obj.sprite <= 0x80)
        ? (sprite_atlas[obj.sprite].h > 0
            ? (sprite_atlas[obj.sprite].h - 1) * 8 : 0)
        : 0;
    int max_y_abs = static_cast<int>(obj.y.whole) * 256 +
                    static_cast<int>(obj.y.fraction) + sprite_h_units;
    int waterline_abs =
        static_cast<int>(get_waterline_y(obj.x.whole)) * 256;
    int diff = max_y_abs - waterline_abs;
    uint8_t amount_under;
    if (diff <= 0) {
        amount_under = 0;
    } else if (diff >= 0x100) {
        amount_under = 0xff;
    } else {
        amount_under = static_cast<uint8_t>(diff);
    }

    // Upper-world ponds (TILE_WATER above the global waterline) — 6502
    // OR's the water_tile flag at &01 into the buoyancy calc.
    bool in_tile_water = is_underwater(landscape, obj.x.whole, obj.y.whole);
    if (amount_under == 0 && !in_tile_water) return;
    if (amount_under == 0 && in_tile_water) amount_under = 0xff;

    int Y = (weight == 0) ? 1 : weight;  // &2f43 INY treats 0 as 1
    int h4 = sprite_h_units >> 2;
    if (h4 == 0) h4 = 1;  // guarantee progress on tiny sprites

    int amt = static_cast<int>(amount_under);
    for (int x = 0; x < 4; x++) {
        amt -= h4;
        if (amt < 0) break;
        Y--;
        if (Y < 0) {
            obj.velocity_y--;
        } else if (Y == 0) {
            obj.velocity_y -= 2;
        }
    }

    // &2f85-&2f8a: 7/8 damping every four frames.
    if (every_four_frames) {
        obj.velocity_x = seven_eighths(obj.velocity_x);
        obj.velocity_y = seven_eighths(obj.velocity_y);
    }
}

} // namespace Water
