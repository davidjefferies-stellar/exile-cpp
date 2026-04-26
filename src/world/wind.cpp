#include "world/wind.h"
#include "behaviours/npc_helpers.h"
#include "core/types.h"
#include "objects/object_data.h"
#include "objects/object_manager.h"
#include "particles/particle_system.h"
#include "world/landscape.h"
#include "world/tertiary.h"
#include "world/tile_data.h"
#include "world/water.h"
#include <cstdlib>

namespace Wind {

// Port of &1c47-&1c92: apply_surface_wind.
//
// Wind is centred at (x=0x9B, y=0x4E) and blows INWARD. The original
// routine computes a desired velocity from distance, then at &1c84
// hands off to `add_weighted_vector_component_to_this_object_velocity`
// (&3f94) which:
//   delta = desired - current_velocity
//   delta >>= Y                                   (weight factor)
//   |delta| clamped to max_acceleration (0x0c)    (&3213)
//   current_velocity += delta                     (&31fc)
//
// The previous port was missing this "accelerate toward desired"
// step — it added `desired >> Y` to velocity every frame, so wind
// never saturated and felt much stronger than the 6502. It also had
// the base shift off by one and didn't implement the overflow-ceiling
// case at dist >= 0x48 (&1c72 BPL/DEY DEY).
void apply_surface_wind(Object& obj) {
    // Only above surface (y < 0x4f, &1c49 CMP #&4f / BCS skip)
    if (obj.y.whole >= 0x4f) return;

    uint8_t type_idx = static_cast<uint8_t>(obj.type);
    uint8_t weight = (type_idx < static_cast<uint8_t>(ObjectType::COUNT))
        ? (object_types_flags[type_idx] & ObjectTypeFlags::WEIGHT_MASK)
        : 3;
    if (weight >= 7) return;

    struct { uint8_t center; int8_t* velocity; uint8_t pos; } axes[2] = {
        {0x4e, &obj.velocity_y, obj.y.whole},
        {GameConstants::WIND_CENTER_X, &obj.velocity_x, obj.x.whole},
    };

    for (int i = 0; i < 2; i++) {
        int16_t dist_signed = static_cast<int16_t>(axes[i].pos) -
                              static_cast<int16_t>(axes[i].center);
        bool negative = dist_signed < 0;
        uint8_t dist = negative ? static_cast<uint8_t>(-dist_signed)
                                : static_cast<uint8_t>(dist_signed);

        // &1c61: no wind if |dist| < 0x1e
        if (dist < 0x1e) continue;

        // Weight factor:
        //   base = weight + 2  (LDY weight / INY at &1c5b then INY at &1c78)
        //   -1 at dist >= 0x32 (&1c69 DEY)
        //   -1 at dist >= 0x3c (&1c6e DEY)
        //   -2 at dist >= 0x48 (&1c74 DEY DEY — strength ceiling hit)
        int shift_count = weight + 2;
        if (dist >= 0x32) shift_count--;
        if (dist >= 0x3c) shift_count--;

        // Strength = 2 * (dist - 8), clamped to 0x7f (&1c6f-&1c76)
        int strength = 2 * (static_cast<int>(dist) - 0x08);
        if (strength > 0x7f) {
            strength = 0x7f;
            shift_count -= 2;
        }
        if (shift_count < 0) shift_count = 0; // &1c79-&1c7b: floor Y at 0

        // Sign: wind pushes TOWARD centre, so desired velocity is
        // opposite sign of (pos - centre). Port of &1c5c ROR + &1c7f
        // invert_if_negative.
        int desired = negative ? strength : -strength;

        // &3f94 → &31f6: delta = desired - current; |delta| >>= Y;
        // clamp to ±max_acceleration (0x0c); current += delta.
        int current = *axes[i].velocity;
        int delta = desired - current;
        bool delta_neg = delta < 0;
        int mag = delta_neg ? -delta : delta;
        mag >>= shift_count;
        if (mag > 0x0c) mag = 0x0c; // maximum_acceleration at &9c
        int accel = delta_neg ? -mag : mag;
        int new_vel = current + accel;
        if (new_vel > 127) new_vel = 127;
        if (new_vel < -128) new_vel = -128;
        *axes[i].velocity = static_cast<int8_t>(new_vel);
    }
}

// Compute the bigger of the two axis strengths from the same distance
// table apply_surface_wind uses. Callers use this to gate side effects
// like particle emission. Returning the max (not sum) mirrors the
// 6502, which only produces one particle per frame regardless of how
// many axes of wind are active.
uint8_t surface_wind_magnitude(const Object& obj) {
    if (obj.y.whole >= 0x4f) return 0;

    struct { uint8_t center; uint8_t pos; } axes[2] = {
        {0x4e, obj.y.whole},
        {GameConstants::WIND_CENTER_X, obj.x.whole},
    };

    uint8_t best = 0;
    for (int i = 0; i < 2; i++) {
        int16_t dist_signed = static_cast<int16_t>(axes[i].pos) -
                              static_cast<int16_t>(axes[i].center);
        uint8_t dist = dist_signed < 0
            ? static_cast<uint8_t>(-dist_signed)
            : static_cast<uint8_t>(dist_signed);
        if (dist < 0x1e) continue;
        int strength = 2 * (static_cast<int>(dist) - 0x08);
        if (strength > 0x7f) strength = 0x7f;
        if (strength > best) best = static_cast<uint8_t>(strength);
    }
    return best;
}

// 6502's &1e44 water_velocities_table: tile flip bits (after the 3-rotation
// shuffle at &3faa-&3fac) index into a 4-entry table. Top nibble of the
// looked-up byte is y velocity; bottom nibble (after 4 ASLs) is x velocity.
static constexpr uint8_t kWaterVelocitiesTable[4] = { 0x00, 0x80, 0x07, 0x70 };

// Port of the constant-wind / water-tile data-byte unpack at &3f47-&3f4d:
// vector_y = whole byte (top nibble dominates as a signed velocity);
// vector_x = byte << 4 (bottom nibble shifted into high bits).
static void wind_vector_from_data_byte(uint8_t data, int8_t& vx, int8_t& vy) {
    vy = static_cast<int8_t>(data);
    vx = static_cast<int8_t>(data << 4);
}

// Port of the variable-wind formula at &3f24-&3f3e. Angle cycles 360° every
// 64 frames (frame_counter << 2). Magnitude is a function of rnd, tile_y and
// tile_x (left/right halves of the world get different scaling so the
// canonical "windy caverns" produce the right intensity range).
static void variable_wind_vector(uint8_t tile_x, uint8_t tile_y,
                                 uint8_t frame_counter, Random& rng,
                                 int8_t& vx, int8_t& vy) {
    uint8_t angle = static_cast<uint8_t>(frame_counter << 2);
    uint8_t mag   = (rng.next() & 0x1f) ^ tile_y;
    mag = static_cast<uint8_t>((mag << 1) & 0x7f);
    if ((tile_x & 0x80) != 0) {
        mag = static_cast<uint8_t>((mag & 0x3f) + 0x28);
    }
    NPC::vector_from_magnitude_and_angle(mag, angle, vx, vy);
}

// Port of &3f56-&3f72 apply_wind_velocity_loop. Returns the magnitude
// (max-axis) of the wind vector for downstream particle gating; 0 means
// no force was applied (frame check skipped it).
static uint8_t apply_wind_velocity_to_object(Object& obj,
                                             int8_t vector_x, int8_t vector_y,
                                             bool in_water, bool fully_under,
                                             uint8_t frame_counter) {
    // &3f64-&3f6a frame check: airborne objects only feel wind 16/32 frames;
    // anything in water gets the current every frame.
    if (!in_water && (frame_counter & 0x10) == 0) return 0;

    uint8_t weight = obj.weight();
    int Y = (weight < 4) ? (weight + 1) : weight;
    if (fully_under) Y++;

    // &31f6 apply_weighted_acceleration: |delta| >>= Y, clamp to ±0x0c.
    constexpr int kMaxAccel = 0x0c;
    int dx = int(vector_x) - int(obj.velocity_x);
    int neg_x = dx < 0;
    int mag_x = neg_x ? -dx : dx;
    mag_x >>= Y;
    if (mag_x > kMaxAccel) mag_x = kMaxAccel;
    int new_vx = int(obj.velocity_x) + (neg_x ? -mag_x : mag_x);
    if (new_vx >  127) new_vx =  127;
    if (new_vx < -128) new_vx = -128;
    obj.velocity_x = static_cast<int8_t>(new_vx);

    int dy = int(vector_y) - int(obj.velocity_y);
    int neg_y = dy < 0;
    int mag_y = neg_y ? -dy : dy;
    mag_y >>= Y;
    if (mag_y > kMaxAccel) mag_y = kMaxAccel;
    int new_vy = int(obj.velocity_y) + (neg_y ? -mag_y : mag_y);
    if (new_vy >  127) new_vy =  127;
    if (new_vy < -128) new_vy = -128;
    obj.velocity_y = static_cast<int8_t>(new_vy);

    int absx = vector_x < 0 ? -int(vector_x) : int(vector_x);
    int absy = vector_y < 0 ? -int(vector_y) : int(vector_y);
    return static_cast<uint8_t>(absx > absy ? absx : absy);
}

void surface_wind_vector(const Object& obj, int8_t& vx, int8_t& vy) {
    vx = 0;
    vy = 0;
    if (obj.y.whole >= 0x4f) return;

    struct { uint8_t center; uint8_t pos; int8_t* out; } axes[2] = {
        {0x4e,                          obj.y.whole, &vy},
        {GameConstants::WIND_CENTER_X,  obj.x.whole, &vx},
    };

    for (int i = 0; i < 2; i++) {
        int16_t dist_signed = static_cast<int16_t>(axes[i].pos) -
                              static_cast<int16_t>(axes[i].center);
        bool negative = dist_signed < 0;
        uint8_t dist = negative ? static_cast<uint8_t>(-dist_signed)
                                : static_cast<uint8_t>(dist_signed);
        if (dist < 0x1e) continue;
        int strength = 2 * (static_cast<int>(dist) - 0x08);
        if (strength > 0x7f) strength = 0x7f;
        // 6502 &1c7d-&1c82: invert_if_negative on the wind_velocity_sign
        // bit, then store at &b4,X (vector_x or vector_y). The sign
        // points TOWARD the centre, so it's opposite the "pos - centre"
        // distance sign.
        *axes[i].out = static_cast<int8_t>(negative ? strength : -strength);
    }
}

void apply_tile_environment(Object& obj,
                            const Landscape& landscape,
                            const ObjectManager& mgr,
                            uint8_t frame_counter,
                            Random& rng,
                            ParticleSystem& particles) {
    // &1f29-&1f33 set tile_x/tile_y from the object's centre (this_object_x /
    // this_object_y); we do the same with the whole-tile coords. The exact
    // 6502 path checks the top tile and optionally the bottom tile when the
    // sprite straddles a tile boundary. Sampling a single tile is close
    // enough for visible effects; doing both adds bookkeeping for a small
    // win.
    uint8_t tx = obj.x.whole;
    uint8_t ty = obj.y.whole;

    ResolvedTile res = resolve_tile_with_tertiary(landscape, tx, ty);
    uint8_t tile_type = res.tile_and_flip & TileFlip::TYPE_MASK;
    uint8_t tile_flip = res.tile_and_flip & TileFlip::MASK;
    bool h_flipped = (tile_flip & TileFlip::HORIZONTAL) != 0;

    int8_t vx = 0, vy = 0;
    bool active = false;

    if (tile_type == static_cast<uint8_t>(TileType::VARIABLE_WIND)) {
        // &3f18 underwater early-out, &3f1c-&3f22 h-flipped variant.
        if (Water::is_underwater(landscape, tx, ty)) return;
        if (h_flipped) {
            // &3f20 LDA #&70: constant downdraft for the two square caverns
            // south of the west stone door (h-flipped variable-wind tiles).
            wind_vector_from_data_byte(0x70, vx, vy);
        } else {
            variable_wind_vector(tx, ty, frame_counter, rng, vx, vy);
        }
        active = true;
    } else if (tile_type == static_cast<uint8_t>(TileType::CONSTANT_WIND)) {
        // &3f41-&3f4d: read the tertiary data byte, unpack to (vx, vy).
        // If the tile has no tertiary entry the 6502 falls through to
        // update_variable_water_tile (a 50%-frame still-water path that
        // doesn't apply velocity); we do the same by leaving `active` false.
        if (res.data_offset > 0) {
            uint8_t data = mgr.tertiary_data_byte(res.data_offset);
            wind_vector_from_data_byte(data, vx, vy);
            active = (vx != 0) || (vy != 0);
        }
    } else if (tile_type == static_cast<uint8_t>(TileType::WATER)) {
        // &3fa8-&3fb1: tile flip bits select the water-current vector.
        // The 6502 rolls the flip pair into the low 2 bits via ASL/ROL/ROL.
        // Equivalent direct mapping: HORIZONTAL=0x80→2, VERTICAL=0x40→1, both→3.
        int flip_idx = ((tile_flip & TileFlip::HORIZONTAL) ? 2 : 0) |
                       ((tile_flip & TileFlip::VERTICAL)   ? 1 : 0);
        uint8_t data = kWaterVelocitiesTable[flip_idx];
        if (data == 0) return;     // still water — no current to apply
        wind_vector_from_data_byte(data, vx, vy);
        active = true;
    }

    if (!active) return;

    // is_underwater treats partially-submerged objects the same as fully
    // submerged. The 6502 distinguishes via this_object_waterline (&20):
    // 0xff means "completely underwater". As a rough approximation, treat
    // anything 4+ tiles below the waterline as fully submerged, matching
    // the +1 weight modifier the 6502 applies at &3f61.
    uint8_t wy = Water::get_waterline_y(obj.x.whole);
    bool in_water    = obj.y.whole >= wy;
    bool fully_under = obj.y.whole >= static_cast<uint8_t>(wy + 4);

    uint8_t mag = apply_wind_velocity_to_object(obj, vx, vy,
                                                in_water, fully_under,
                                                frame_counter);

    // &3f73-&3f7b add_wind_particle_using_velocities: emit one PARTICLE_WIND
    // with probability ~ magnitude / 0x80, oriented along the wind vector.
    if (mag > 0 && (rng.next() >> 1) < mag) {
        uint8_t angle = NPC::angle_from_deltas(vx, vy);
        particles.emit_directed(ParticleType::WIND, angle, obj, rng);
    }
}

} // namespace Wind
