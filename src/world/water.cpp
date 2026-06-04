#include "world/water.h"
#include "world/tile_data.h"
#include "world/tertiary.h"
#include "objects/object_data.h"
#include "rendering/sprite_atlas.h"
#include "core/types.h"

namespace Water {

namespace {
// Initial waterline Y values from &14d2. Range 0 main sea, 1 Triax lab,
// 2 / 3 upper reservoirs.
constexpr uint8_t kInitialY[4] = { 0xce, 0xdf, 0xc1, 0xc1 };

uint8_t g_y[4]          = { 0xce, 0xdf, 0xc1, 0xc1 };
uint8_t g_y_fraction[4] = { 0, 0, 0, 0 };
uint8_t g_desired_y[4]  = { 0xce, 0xdf, 0xc1, 0xc1 };
}  // namespace

void reset() {
    for (int i = 0; i < 4; i++) {
        g_y[i]          = kInitialY[i];
        g_y_fraction[i] = 0;
        g_desired_y[i]  = kInitialY[i];
    }
}

uint8_t get_y(int range)           { return g_y[range & 3]; }
uint8_t get_y_fraction(int range)  { return g_y_fraction[range & 3]; }
uint8_t get_desired_y(int range)   { return g_desired_y[range & 3]; }

void set_y(int range, uint8_t y, uint8_t fraction) {
    g_y[range & 3]          = y;
    g_y_fraction[range & 3] = fraction;
}

void set_desired_y(int range, uint8_t y) {
    g_desired_y[range & 3] = y;
}

// Port of &2626-&265b. For each range, compute the signed step toward
// desired_y (high byte of a 16-bit SBC of (desired, 0x18) - (y, frac)),
// add the ±2 cycle, clamp to ±2 via keep_within_range, then ADC into
// y_fraction with carry -> INC y, pre-clamp negative -> DEC y.
void update_waterlines(uint8_t frame_counter) {
    uint8_t delta = (frame_counter & 0x20) ? 0xfe : 0x02;  // signed ±2
    for (int x = 0; x < 4; x++) {
        // 6502 ALU chain: low SBC of 0x18 - y_fraction sets carry; hi
        // SBC propagates it into desired - y - borrow; ADC delta uses
        // the hi-byte carry.
        int lo_carry = (0x18 >= int(g_y_fraction[x])) ? 1 : 0;
        int hi = int(g_desired_y[x]) - int(g_y[x]) - (1 - lo_carry);
        int hi_carry = (hi >= 0) ? 1 : 0;
        int s = int(uint8_t(hi & 0xff)) + int(delta) + hi_carry;
        bool n_pre_clamp = (uint8_t(s) & 0x80) != 0;
        // keep_within_range Y=2: clamp signed A to [-2, +2].
        int8_t step = int8_t(uint8_t(s));
        if (step >  2) step =  2;
        if (step < -2) step = -2;
        // ADC y_fraction; carry -> INC y. PLP+BPL: pre-clamp N -> DEC y.
        int sum = int(uint8_t(step)) + int(g_y_fraction[x]);
        g_y_fraction[x] = uint8_t(sum & 0xff);
        if (sum > 0xff)   g_y[x]++;
        if (n_pre_clamp)  g_y[x]--;
    }
}

// Port of &2cbc-&2cdb. Range 1 (Triax lab) acts as a ceiling so the
// lab's drain/fill state pulls every connected region with it.
uint8_t get_waterline_y(uint8_t x) {
    int range = 0;
    for (int i = 3; i >= 0; i--) {
        if (x >= waterline_x_ranges_x[i]) {
            range = i;
            break;
        }
    }
    uint8_t waterline = g_y[range];
    if (waterline > g_y[1]) waterline = g_y[1];
    return waterline;
}

// Sub-tile fraction matching get_waterline_y. Picks the same range and,
// when the lab ceiling at range 1 wins the min, uses that range's fraction.
// Without this the renderer snaps the waterline at whole-tile boundaries.
uint8_t get_waterline_y_fraction(uint8_t x) {
    int range = 0;
    for (int i = 3; i >= 0; i--) {
        if (x >= waterline_x_ranges_x[i]) {
            range = i;
            break;
        }
    }
    int chosen = (g_y[range] > g_y[1]) ? 1 : range;
    return g_y_fraction[chosen];
}

bool is_underwater(const Landscape& landscape, uint8_t x, uint8_t y) {
    // 6502 at &2f03-&2f39 checks the tile (TileType::WATER) first for
    // upper-world ponds, then falls back to the global waterline. The
    // 6502's tile_update_routine resolves tertiary entries before the
    // water-tile check, so we go through resolve_tile_with_tertiary too —
    // a raw SPACE_WITH_OBJECT_FROM_TYPE cell can resolve to WATER via its
    // tertiary entry (wind.cpp does the same).
    ResolvedTile r = resolve_tile_with_tertiary(landscape, x, y);
    if ((r.tile_and_flip & TileFlip::TYPE_MASK) ==
            static_cast<uint8_t>(TileType::WATER)) {
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
// Total velocity_y DECs when fully submerged: weight 0/1->5, 2->4, 3->3,
// 4->2, 5+->0.
uint8_t submersion_depth(const Landscape& landscape, const Object& obj) {
    int sprite_h_units = (obj.sprite <= 0x80)
        ? (sprite_atlas[obj.sprite].h > 0
            ? (sprite_atlas[obj.sprite].h - 1) * 8 : 0)
        : 0;
    int max_y_abs = static_cast<int>(obj.y.whole) * 256 +
                    static_cast<int>(obj.y.fraction) + sprite_h_units;
    int waterline_abs =
        static_cast<int>(get_waterline_y(obj.x.whole)) * 256;
    int diff = max_y_abs - waterline_abs;
    uint8_t depth = (diff <= 0) ? 0
                  : (diff >= 0x100) ? 0xff
                  : static_cast<uint8_t>(diff);
    // Upper-world ponds (TILE_WATER above the global waterline) — 6502
    // OR's the water_tile flag at &01 into the buoyancy calc.
    if (depth == 0 && is_underwater(landscape, obj.x.whole, obj.y.whole))
        depth = 0xff;
    return depth;
}

bool apply_water_effects(const Landscape& landscape, Object& obj,
                         uint8_t weight, bool every_four_frames) {
    uint8_t amount_under = submersion_depth(landscape, obj);
    bool in_tile_water = is_underwater(landscape, obj.x.whole, obj.y.whole);
    if (amount_under == 0) return false;

    int sprite_h_units = (obj.sprite <= 0x80 && sprite_atlas[obj.sprite].h > 0)
        ? (sprite_atlas[obj.sprite].h - 1) * 8 : 0;
    int Y = (weight == 0) ? 1 : weight;  // &2f43 INY treats 0 as 1
    int h4 = sprite_h_units >> 2;
    if (h4 == 0) h4 = 1;  // guarantee progress on tiny sprites

    // Track whether the buoyancy loop terminated via the &2f59 BCC
    // (object's height/4 exceeded the remaining water column) vs the
    // &2f67 BEQ at X=0 (loop ran all 4 iterations -> fully submerged).
    // The 6502 emits PARTICLE_WATER only on the BCC path; we mirror by
    // returning true here and letting the caller emit.
    bool broke_early = false;
    int amt = static_cast<int>(amount_under);
    for (int x = 0; x < 4; x++) {
        amt -= h4;
        if (amt < 0) { broke_early = true; break; }
        Y--;
        if (Y < 0) {
            obj.velocity_y--;
        } else if (Y == 0) {
            obj.velocity_y -= 2;
        }
    }

    // 6502 &2f6b BMI runs the vy sign test BEFORE the &2f85 damping
    // step — so capture the decision here, then apply damping below.
    // Port-only deviation: in upper-world water tiles we short-circuit
    // the buoyancy depth math to amount_under = 0xff (the BBC's
    // &2f27-&2f2b "OR water_tile" path), which keeps buoyancy strong
    // but skips the BCC early-break -> no particle. Bubble per-frame
    // whenever the object is in tile water and not rising; otherwise
    // use the BCC path the BBC takes for the global waterline.
    const bool emit_particle = (broke_early || in_tile_water) &&
                                obj.velocity_y >= 0;

    // &2f85-&2f8a: 7/8 damping every four frames.
    if (every_four_frames) {
        obj.velocity_x = seven_eighths(obj.velocity_x);
        obj.velocity_y = seven_eighths(obj.velocity_y);
    }

    return emit_particle;
}

} // namespace Water
