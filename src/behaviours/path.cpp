#include "behaviours/path.h"
#include "objects/collision.h"
#include "core/types.h"
#include <cstdlib>

namespace NPC {

// One raycast step represents 0x20 fractions = 1/8 tile, matching the 6502's
// `use_vector_between_object_centres` with magnitude = 0x20. After 8 steps
// we've advanced one tile along the dominant axis.
static constexpr int STEP_FRACTIONS = 0x20;

bool has_line_of_sight(const Object& obj,
                       uint8_t target_slot,
                       uint8_t max_tiles,
                       const UpdateContext& ctx) {
    if (target_slot >= GameConstants::PRIMARY_OBJECT_SLOTS) return false;
    const Object& target = ctx.mgr.object(target_slot);
    if (!target.is_active()) return false;

    // 16-bit centres so we can step along the ray without worrying about the
    // 256-fraction / whole-tile boundary on every iteration. The centre is
    // "object origin + sprite half-width", matching the 6502's vector
    // between centres (&35e5 LDA this_object_width; LSR; ADC x_fraction).
    auto centre16 = [](const Object& o, bool is_x) -> int {
        // Without the sprite atlas here we approximate the centre as a
        // half-tile offset. The 6502 uses the sprite's actual width/height
        // but for LOS the error (<0.5 tile) is below the raycast step.
        int whole = is_x ? o.x.whole    : o.y.whole;
        int frac  = is_x ? o.x.fraction : o.y.fraction;
        return whole * 256 + frac + 0x80;
    };

    int sx = centre16(obj,    true);
    int sy = centre16(obj,    false);
    int tx = centre16(target, true);
    int ty = centre16(target, false);

    int dx = tx - sx;
    int dy = ty - sy;

    int adx = std::abs(dx);
    int ady = std::abs(dy);
    int max_axis = (adx > ady) ? adx : ady;
    if (max_axis == 0) return true; // coincident

    // Early distance gate: if even the Chebyshev distance exceeds the cap,
    // skip the raycast. `max_tiles` × 256 is the cap in fraction-units.
    int cap_fracs = static_cast<int>(max_tiles) * 256;
    if (max_axis > cap_fracs) return false;

    // Normalise to a step of STEP_FRACTIONS along the dominant axis. The
    // number of steps is max_axis / STEP_FRACTIONS; other axis advances
    // proportionally. Matches the 6502's `calculate_vector_from_magnitude_
    // and_angle` called with magnitude = 0x20 (&35b4).
    int steps = max_axis / STEP_FRACTIONS;
    if (steps == 0) steps = 1;
    int vx = dx / steps;
    int vy = dy / steps;

    int px = sx;
    int py = sy;
    for (int i = 0; i < steps; ++i) {
        px += vx;
        py += vy;
        uint8_t tile_x = static_cast<uint8_t>((px >> 8) & 0xff);
        uint8_t tile_y = static_cast<uint8_t>((py >> 8) & 0xff);
        uint8_t x_frac = static_cast<uint8_t>(px & 0xff);
        // 6502 rounds y_fraction to the middle of the pixel (AND #&f8 /
        // ORA #&04 at &3630) before comparing against the threshold. Same
        // trick here so slope tiles match byte-for-byte.
        uint8_t y_frac = static_cast<uint8_t>((py & 0xf8) | 0x04);
        if (Collision::point_in_tile_solid(ctx.landscape, tile_x, tile_y,
                                           x_frac, y_frac)) {
            return false;
        }
    }
    return true;
}

void update_target_directness(Object& obj, UpdateContext& ctx) {
    // 6502 &3cf6: `LDA this_object_frame_counter_sixteen / BNE leave`.
    // frame_counter_sixteen is a per-object 0..15 counter, not the global
    // frame counter. We approximate with slot-staggered every-16-frames
    // ticks so 16 NPCs share the load across frames — (frame + slot) & 15.
    uint8_t t = static_cast<uint8_t>(ctx.frame_counter + ctx.this_slot);
    if ((t & 0x0f) != 0) return;

    uint8_t target_slot = obj.target_and_flags & TargetFlags::OBJECT_MASK;
    const Object& target = (target_slot < GameConstants::PRIMARY_OBJECT_SLOTS)
                         ? ctx.mgr.object(target_slot)
                         : ctx.mgr.player();
    if (!target.is_active() || target_slot >= GameConstants::PRIMARY_OBJECT_SLOTS) {
        // No target: decay directness by one level. Port of &3d36
        // reduce_targeting_directness.
        uint8_t flags = obj.target_and_flags;
        uint8_t lvl   = (flags & TargetFlags::DIRECTNESS_MASK);
        if (lvl >= TargetFlags::DIRECTNESS_ONE) {
            flags -= TargetFlags::DIRECTNESS_ONE;
            obj.target_and_flags = flags;
        }
        return;
    }

    bool los = has_line_of_sight(obj, target_slot, /*max_tiles=*/16, ctx);
    if (los) {
        // &3d04-&3d08: target visible → snap to DIRECTNESS_THREE.
        obj.target_and_flags = static_cast<uint8_t>(
            (obj.target_and_flags & ~TargetFlags::DIRECTNESS_MASK)
            | TargetFlags::DIRECTNESS_THREE);
        // &3d0e: set this_object_tx/ty from target (unless AVOIDING, in
        // which case head in the opposite direction). Wrap-around
        // arithmetic in uint8_t matches the 6502's 256-wrap world.
        if (obj.target_and_flags & TargetFlags::AVOID) {
            obj.tx = static_cast<uint8_t>(obj.x.whole * 2 - target.x.whole);
            obj.ty = static_cast<uint8_t>(obj.y.whole * 2 - target.y.whole);
        } else {
            obj.tx = target.x.whole;
            obj.ty = target.y.whole;
        }
    } else {
        // &3d1d-&3d23: LOS blocked → if DIRECTNESS_TWO was set, drop
        // DIRECTNESS_ONE (level 3 → 2). Otherwise no change this tick.
        if (obj.target_and_flags & TargetFlags::DIRECTNESS_TWO) {
            obj.target_and_flags = static_cast<uint8_t>(
                obj.target_and_flags & ~TargetFlags::DIRECTNESS_ONE);
        }
    }
}

void update_npc_path(Object& obj, UpdateContext& ctx) {
    // First, refresh LOS-derived directness once every 16 frames.
    update_target_directness(obj, ctx);

    uint8_t lvl = directness_level(obj);
    uint8_t target_slot = obj.target_and_flags & TargetFlags::OBJECT_MASK;
    const Object& target = (target_slot < GameConstants::PRIMARY_OBJECT_SLOTS)
                         ? ctx.mgr.object(target_slot)
                         : ctx.mgr.player();

    // Path-update cadence: the 6502 only rewrites (tx, ty) every 8 or 64
    // frames depending on directness (`check_if_npc_path_update_needed`
    // at &3d94). We collapse the two cadences into every-8-frame updates.
    if (!ctx.every_eight_frames) return;

    switch (lvl) {
    case 3:
    case 2: {
        // use_direct_path (&3d31): head straight for the target's current
        // position. update_target_directness already did this when LOS
        // was clear; re-assert here for level-2 (target was seen recently
        // but is now hidden — keep heading to where it last was).
        if (obj.target_and_flags & TargetFlags::AVOID) {
            obj.tx = static_cast<uint8_t>(obj.x.whole * 2 - target.x.whole);
            obj.ty = static_cast<uint8_t>(obj.y.whole * 2 - target.y.whole);
        } else {
            obj.tx = target.x.whole;
            obj.ty = target.y.whole;
        }
        break;
    }
    case 1: {
        // use_slightly_relaxed_path (&3d40): head roughly toward target
        // but jitter the position by ±~2 tiles so the NPC doesn't line
        // up perfectly. Mirrors the 6502's &3d51 use_vector_between_
        // object_centres + random magnitude fiddling in a reduced form.
        int8_t jx = static_cast<int8_t>((ctx.rng.next() & 0x07) - 4);
        int8_t jy = static_cast<int8_t>((ctx.rng.next() & 0x07) - 4);
        obj.tx = static_cast<uint8_t>(target.x.whole + jx);
        obj.ty = static_cast<uint8_t>(target.y.whole + jy);
        break;
    }
    default: {
        // use_relaxed_path (&3d68): drift tx, ty around the NPC's own
        // position. The 6502 picks a random nearby tile; we pick a
        // random offset in [-8, +7] on each axis so the NPC wanders
        // aimlessly until the target is re-sighted.
        int8_t dx = static_cast<int8_t>((ctx.rng.next() & 0x0f) - 8);
        int8_t dy = static_cast<int8_t>((ctx.rng.next() & 0x0f) - 8);
        obj.tx = static_cast<uint8_t>(obj.x.whole + dx);
        obj.ty = static_cast<uint8_t>(obj.y.whole + dy);
        break;
    }
    }
}

} // namespace NPC
