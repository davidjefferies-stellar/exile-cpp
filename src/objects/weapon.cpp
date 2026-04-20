#include "objects/weapon.h"
#include "objects/object_tables.h"
#include "core/types.h"
#include "rendering/sprite_atlas.h"
#include <algorithm>

namespace Weapon {

// Port of &2357 calculate_vector_from_magnitude_and_angle — the BBC's very
// approximate "diamond" trig. Each quadrant is a piecewise-linear clamp to
// magnitude 0x20, giving these anchor points (see the table in the
// disassembly comment at ~&2172):
//
//   angle 0x00 (right) → ( 0x20,  0x00)
//   angle 0x40 (down)  → ( 0x00,  0x20)
//   angle 0x80 (left)  → (-0x20,  0x00)
//   angle 0xc0 (up)    → ( 0x00, -0x20)
//
// The "diamond" name comes from the fact that |vx| + |vy| ≤ 0x40 — points
// trace a rotated square rather than a circle. Good enough for pixel-space
// gameplay and avoids trig tables entirely.
static void diamond_vector(uint8_t angle, int8_t& vx, int8_t& vy) {
    uint8_t quad = angle >> 6;       // 0..3
    uint8_t rel  = angle & 0x3f;     // 0..0x3f within the quadrant
    int a = std::min<int>(0x20, rel);
    int b = std::min<int>(0x20, 0x40 - rel);
    switch (quad) {
        case 0: vx =  static_cast<int8_t>(b); vy =  static_cast<int8_t>(a); break; // right → down
        case 1: vx = -static_cast<int8_t>(a); vy =  static_cast<int8_t>(b); break; // down  → left
        case 2: vx = -static_cast<int8_t>(b); vy = -static_cast<int8_t>(a); break; // left  → up
        case 3: vx =  static_cast<int8_t>(a); vy = -static_cast<int8_t>(b); break; // up    → right
    }
}

void get_firing_velocity(uint8_t aim_angle, bool facing_left,
                         int8_t& vel_x, int8_t& vel_y) {
    // aim_angle is a signed -0x3f..+0x3f offset from the facing direction
    // (negative = up, positive = down). Fold facing into the 8-bit angle
    // used by diamond_vector: 0x00 = right, 0x80 = left. For left-facing we
    // mirror across the vertical axis so "aim up" still maps into the upper
    // hemisphere.
    int8_t s = static_cast<int8_t>(aim_angle);
    uint8_t angle = facing_left
        ? static_cast<uint8_t>(0x80 - s)
        : static_cast<uint8_t>(s);
    diamond_vector(angle, vel_x, vel_y);
}

int fire(ObjectManager& mgr, const Object& player,
         uint8_t weapon_type, uint8_t aim_angle,
         uint16_t& weapon_energy) {
    if (weapon_type > 5) return -1;

    int8_t bullet_type = weapon_bullet_type[weapon_type];
    if (bullet_type == 0) return -1;  // No bullet (jetpack, protection suit)
    if (bullet_type < 0) return -1;   // Blaster discharge mode

    uint8_t cost = weapon_energy_cost[weapon_type];
    if (weapon_energy < static_cast<uint16_t>(cost)) return -1;

    ObjectType obj_type = static_cast<ObjectType>(bullet_type);
    int slot = mgr.create_object_at(obj_type, 4, player);
    if (slot < 0) return -1;

    weapon_energy -= cost;

    Object& bullet = mgr.object(slot);
    int8_t vel_x, vel_y;
    get_firing_velocity(aim_angle, player.is_flipped_h(), vel_x, vel_y);
    bullet.velocity_x = vel_x;
    bullet.velocity_y = vel_y;

    // Port of create_child_object (&33b8-&342f). The 6502 stores widths/
    // heights with the flip bit baked into bit 0 of the byte:
    //   width_byte  = (pixels-1)<<4 | h_flip
    //   height_byte = (rows-1)<<3   | v_flip
    // We reconstruct the full byte (flip bit included) so the arithmetic
    // matches the disassembly byte-for-byte.
    if (player.sprite <= 0x7c && bullet.sprite <= 0x7c) {
        const SpriteAtlasEntry& pe = sprite_atlas[player.sprite];
        const SpriteAtlasEntry& be = sprite_atlas[bullet.sprite];
        int parent_w_byte = (pe.w > 0 ? (pe.w - 1) : 0) * 16
                          | (pe.intrinsic_flip & 0x01);
        int child_w_byte  = (be.w > 0 ? (be.w - 1) : 0) * 16
                          | (be.intrinsic_flip & 0x01);
        int parent_h_byte = (pe.h > 0 ? (pe.h - 1) : 0) * 8
                          | ((pe.intrinsic_flip >> 1) & 0x01);
        int child_h_byte  = (be.h > 0 ? (be.h - 1) : 0) * 8
                          | ((be.intrinsic_flip >> 1) & 0x01);

        // Y: parent_y + (parent_h - child_h)/2 — &33d2-&33e2. Centres the
        // child vertically on the parent.
        {
            int dy = (parent_h_byte - child_h_byte) / 2;
            int new_y = static_cast<int>(bullet.y.whole) * 256
                      + static_cast<int>(bullet.y.fraction)
                      + dy;
            bullet.y.whole    = static_cast<uint8_t>((new_y >> 8) & 0xff);
            bullet.y.fraction = static_cast<uint8_t>(new_y & 0xff);
        }

        // X: &33e5-&342d is two stacked steps.
        //
        //  1. Primary offset (&33f5-&3404): if bullet moves right *relative
        //     to parent* (parent_vx - bullet_vx < 0), spawn past the
        //     parent's right edge; otherwise past the child's left edge.
        //  2. Extra offset (&3412-&3421): adds one frame's worth of
        //     relative motion — relative_vx in the same-direction branch,
        //     or -bullet_vx in the opposite-direction branch — so the
        //     bullet's *next* frame lands at the naive "past the edge"
        //     position.
        //
        // Without step 2 the bullet spawns `bullet_vx` fractions too far
        // forward, which can straddle a tile boundary and put the bullet
        // inside a wall on the very frame it's created.
        {
            int parent_vx = player.velocity_x;
            int bullet_vx = vel_x;
            int rel_vx    = parent_vx - bullet_vx;
            int8_t rel_s  = static_cast<int8_t>(rel_vx);
            // &33ef EOR &43: bit 7 of (rel_vx XOR parent_vx) tells us
            // whether parent and bullet are moving in the same direction
            // in x. When the 6502 comment says "negative if parent and
            // child moving in same direction", negative == bit 7 set.
            bool same_direction =
                ((static_cast<uint8_t>(rel_s) ^
                  static_cast<uint8_t>(parent_vx)) & 0x80) != 0;

            int dx_primary;
            if (rel_s < 0) {
                dx_primary =  parent_w_byte + 0x18; // past parent's right
            } else {
                dx_primary = -(child_w_byte  + 0x18); // past child's left
            }

            int extra = same_direction ? rel_s : -bullet_vx;
            int dx    = dx_primary + extra;

            int new_x = static_cast<int>(bullet.x.whole) * 256
                      + static_cast<int>(bullet.x.fraction)
                      + dx;
            bullet.x.whole    = static_cast<uint8_t>((new_x >> 8) & 0xff);
            bullet.x.fraction = static_cast<uint8_t>(new_x & 0xff);
        }
    }
    // Initial lifespan — common_bullet_update explodes the bullet the instant
    // its timer hits zero, and the icer/pistol updaters re-arm the timer
    // while the bullet is still moving. Starting at 0 (as init_object_from_type
    // leaves it) would blow the bullet up on its very first frame.
    bullet.timer = 0x30;
    return slot;
}

} // namespace Weapon
