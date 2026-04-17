#include "world/water.h"
#include "world/tile_data.h"
#include "objects/object_data.h"
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

bool is_underwater(uint8_t x, uint8_t y) {
    return y >= get_waterline_y(x);
}

// Apply water physics: buoyancy and velocity damping.
// Port of the water physics at &2f01-&2f6e.
// Buoyancy pushes objects upward, damping reduces velocity.
void apply_water_effects(Object& obj, uint8_t weight) {
    uint8_t waterline = get_waterline_y(obj.x.whole);

    if (obj.y.whole < waterline) return; // Not underwater

    // Buoyancy: reduce downward velocity based on weight
    // Lighter objects are more buoyant
    if (obj.velocity_y > 0) {
        // Object is sinking: apply upward force
        if (weight <= 2) {
            obj.velocity_y -= 2; // Light: strong buoyancy
        } else if (weight <= 4) {
            obj.velocity_y -= 1; // Medium: moderate buoyancy
        }
        // Heavy objects (weight 5-6) get minimal buoyancy
    } else if (obj.velocity_y < -2) {
        // Object is rising fast: dampen upward motion
        obj.velocity_y++;
    }

    // Velocity damping: friction from water slows all motion
    // Applied every frame - water is viscous
    if (obj.velocity_x > 1) obj.velocity_x--;
    else if (obj.velocity_x < -1) obj.velocity_x++;

    if (obj.velocity_y > 1) obj.velocity_y--;
    else if (obj.velocity_y < -1) obj.velocity_y++;
}

} // namespace Water
