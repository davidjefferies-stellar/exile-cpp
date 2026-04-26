#include "particles/particle_system.h"
#include "behaviours/npc_helpers.h"
#include "objects/object.h"
#include "objects/object_data.h"
#include "rendering/sprite_atlas.h"
#include "core/types.h"

// ===== Per-type configuration (from &0206-&0276 in the disassembly) =========
//
// Source rows in the original (ttl_r, ttl, spd_r, spd, cf, cf_r, flags,
// x_r, y_r, vx_r, vy_r):
//
//   PLASMA            : 0f 1e 0f 0a 91 02 a0 1f 1f 03 03
//   JETPACK           : 0f 03 0f 18 86 01 ed 00 00 03 03
//   EXPLOSION         : ff 00 00 00 91 46 2a 00 00 2f 2f
//   FIREBALL          : 07 05 07 0a 81 02 20 7f 3f 00 00
//   PROJECTILE_TRAIL  : 07 02 0f 03 82 01 2a 00 00 03 03
//   ENGINE            : 0f 14 0f 1e 81 42 00 00 00 0f 03
//   AIM               : 03 10 01 3f a8 07 2d 00 00 00 01
//   STAR_OR_MUSHROOM  : 1c 08 00 00 88 47 00 ff ff 00 00
//   FLASK             : 0f 14 07 0a 97 41 22 00 00 03 03
//   WATER             : 07 0a 03 06 97 41 01 00 00 0f 03
//   WIND              : 07 0a 0f 28 97 41 00 ff ff 03 03

struct TypeData {
    uint8_t ttl_rand, ttl_base;
    uint8_t spd_rand, spd_base;
    uint8_t cf_base, cf_rand;
    uint8_t flags;
    uint8_t x_rand, y_rand;
    uint8_t vx_rand, vy_rand;
};

static constexpr TypeData TYPES[static_cast<int>(ParticleType::COUNT)] = {
    /* PLASMA           */ { 0x0f, 0x1e, 0x0f, 0x0a, 0x91, 0x02, 0xa0, 0x1f, 0x1f, 0x03, 0x03 },
    /* JETPACK          */ { 0x0f, 0x03, 0x0f, 0x18, 0x86, 0x01, 0xed, 0x00, 0x00, 0x03, 0x03 },
    /* EXPLOSION        */ { 0xff, 0x00, 0x00, 0x00, 0x91, 0x46, 0x2a, 0x00, 0x00, 0x2f, 0x2f },
    /* FIREBALL         */ { 0x07, 0x05, 0x07, 0x0a, 0x81, 0x02, 0x20, 0x7f, 0x3f, 0x00, 0x00 },
    /* PROJECTILE_TRAIL */ { 0x07, 0x02, 0x0f, 0x03, 0x82, 0x01, 0x2a, 0x00, 0x00, 0x03, 0x03 },
    /* ENGINE           */ { 0x0f, 0x14, 0x0f, 0x1e, 0x81, 0x42, 0x00, 0x00, 0x00, 0x0f, 0x03 },
    /* AIM              */ { 0x03, 0x10, 0x01, 0x3f, 0xa8, 0x07, 0x2d, 0x00, 0x00, 0x00, 0x01 },
    /* STAR_OR_MUSHROOM */ { 0x1c, 0x08, 0x00, 0x00, 0x88, 0x47, 0x00, 0xff, 0xff, 0x00, 0x00 },
    /* FLASK            */ { 0x0f, 0x14, 0x07, 0x0a, 0x97, 0x41, 0x22, 0x00, 0x00, 0x03, 0x03 },
    /* WATER            */ { 0x07, 0x0a, 0x03, 0x06, 0x97, 0x41, 0x01, 0x00, 0x00, 0x0f, 0x03 },
    /* WIND             */ { 0x07, 0x0a, 0x0f, 0x28, 0x97, 0x41, 0x00, 0xff, 0xff, 0x03, 0x03 },
};

// Signed clamp to [-128, 127] (port of &327f prevent_overflow).
static int8_t clamp_signed(int v) {
    if (v >  127) return  127;
    if (v < -128) return -128;
    return static_cast<int8_t>(v);
}

int ParticleSystem::allocate_slot(Random& rng) {
    if (n_ < MAX_PARTICLES) return n_++;
    // Pool full — replace a random existing particle.
    return static_cast<int>(rng.next()) & (MAX_PARTICLES - 1);
}

void ParticleSystem::update(uint8_t waterline_y, uint8_t waterline_y_frac, Random& rng) {
    // Walk backwards so that swap-remove during compaction is safe.
    int i = n_ - 1;
    while (i >= 0) {
        Particle& p = pool_[i];

        // &20a7: if ACCELERATE flag, apply gravity (+1) or water float (-3).
        if (p.colour_and_flags & ParticleFlag::ACCELERATE) {
            int dy = 1;
            bool in_water =
                (p.y > waterline_y) ||
                (p.y == waterline_y && p.y_fraction >= waterline_y_frac);
            if (in_water) {
                dy = -3;
                // &20c6: water particles are cyan (6) or white (7).
                uint8_t c = (rng.next() & 0x07) | 0x06;
                p.colour_and_flags = (p.colour_and_flags & 0xf8) | (c & 0x07);
            }
            int nv = int(p.velocity_y) + dy;
            // Original guards against signed overflow; same effect here.
            if (nv >=  127 || nv <= -128) {
                // Skip the acceleration this frame (BVS skip).
            } else {
                p.velocity_y = static_cast<int8_t>(nv);
            }
        }

        // &20d3: cycle colour if CYCLE flag set.
        if (p.colour_and_flags & ParticleFlag::CYCLE) {
            uint8_t c = (p.colour_and_flags & 0x07) + 1;
            p.colour_and_flags = (p.colour_and_flags & 0xf8) | (c & 0x07);
        }

        // &20e1: ttl decrement; remove at 0.
        if (p.ttl == 0 || --p.ttl == 0) {
            // Compact: swap with last.
            pool_[i] = pool_[n_ - 1];
            n_--;
            i--;
            continue;
        }

        // &20e6-&2100: integrate position by velocity (signed add with
        // fraction carry / underflow into whole byte).
        auto step_axis = [](uint8_t& whole, uint8_t& frac, int8_t vel) {
            uint8_t uv = static_cast<uint8_t>(vel);
            int sum = int(frac) + int(uv);
            frac = static_cast<uint8_t>(sum);
            if (sum > 0xff) whole++;
            if (vel < 0)    whole--;   // pre-decrement for negative velocity
        };
        step_axis(p.x, p.x_fraction, p.velocity_x);
        step_axis(p.y, p.y_fraction, p.velocity_y);

        i--;
    }
}

// ---------- Emission (port of &218c / &218e / associated helpers) ----------

void ParticleSystem::emit(ParticleType type, int count, const Object& src, Random& rng) {
    if (type >= ParticleType::COUNT || count <= 0) return;
    const TypeData& t = TYPES[static_cast<int>(type)];

    // Build base position from source object (&2197-&2209).
    //
    // After the three ASLs at &21e4-&21e8, the flags byte is walked MSB-first
    // and each axis goes through two flag bits:
    //   - "consider sprite flip": if set AND the object is flipped on this
    //     axis, start the offset at the sprite's (width-1)*16 / (height-1)*8
    //     edge; otherwise start at 0.
    //   - "use centre": if set, add half the sprite's width/height in the
    //     6502's sub-tile units (1 sprite-pixel horizontally = 16, 1 row
    //     vertically = 8).
    // Both offsets are applied to the object's position fraction, carrying
    // into the whole-tile byte on overflow. Jetpack particles use the "use
    // vertical centre" bit (0x08) so they come out of the middle of the
    // spacesuit instead of its head.
    uint8_t base_x         = src.x.whole;
    uint8_t base_x_frac    = src.x.fraction;
    uint8_t base_y         = src.y.whole;
    uint8_t base_y_frac    = src.y.fraction;

    // Look up the source sprite so we know how big it is in sub-tile units.
    // object_types_sprite[type] picks the default spritesheet index; fall
    // back to the object's overridden sprite (used by direction animations).
    uint8_t sprite_id = src.sprite;
    if (sprite_id > 0x7c) {
        uint8_t tidx = static_cast<uint8_t>(src.type);
        if (tidx < static_cast<uint8_t>(ObjectType::COUNT)) {
            sprite_id = object_types_sprite[tidx];
        }
    }
    int sw_units = 0, sh_units = 0;
    if (sprite_id <= 0x7c) {
        const SpriteAtlasEntry& e = sprite_atlas[sprite_id];
        sw_units = (e.w > 0 ? (e.w - 1) : 0) * 16;   // 1 sprite-px = 16 frac
        sh_units = (e.h > 0 ? (e.h - 1) : 0) * 8;    // 1 sprite-row = 8 frac
    }

    // Helper applies the 6502's per-axis "flip-aware edge + optional centre"
    // offset. `size` is the sub-tile extent on this axis, `flipped` is whether
    // the object's flip bit is set for this axis, `consider_flip` and
    // `use_centre` come out of the flags byte.
    auto apply_base_offset = [&](uint8_t& whole, uint8_t& frac,
                                 int size, bool flipped,
                                 bool consider_flip, bool use_centre) {
        int offset = 0;
        if (consider_flip && flipped) offset = size;    // start at far edge
        if (use_centre) offset += size / 2;              // plus half-extent
        int sum = int(frac) + offset;
        frac = static_cast<uint8_t>(sum);
        whole = static_cast<uint8_t>(whole + (sum >> 8));
    };

    // After 3 ASLs the flags byte has been consumed down to bits 4..0; the
    // per-axis pair order the 6502 uses is (y first — X=2 in its loop, then
    // x — X=0). Bits walked: vflip, vcentre, hflip, hcentre.
    bool consider_vflip  = (t.flags & 0x10) != 0;
    bool use_vcentre     = (t.flags & 0x08) != 0;
    bool consider_hflip  = (t.flags & 0x04) != 0;
    bool use_hcentre     = (t.flags & 0x02) != 0;

    bool y_flipped = (src.flags & ObjectFlags::FLIP_VERTICAL)   != 0;
    bool x_flipped = (src.flags & ObjectFlags::FLIP_HORIZONTAL) != 0;

    apply_base_offset(base_y, base_y_frac, sh_units, y_flipped,
                      consider_vflip, use_vcentre);
    apply_base_offset(base_x, base_x_frac, sw_units, x_flipped,
                      consider_hflip, use_hcentre);

    // Base velocity from the source object's velocity or acceleration
    // (bits 7-6 of flags). Acceleration source isn't wired yet; treat any
    // non-zero value as "use object velocity" and zero as "no base vel".
    int8_t base_vx = 0, base_vy = 0;
    bool use_src_vel   = (t.flags & 0x80) != 0 && (t.flags & 0x40) == 0;
    bool use_src_accel = (t.flags & 0xc0) == 0xc0;
    bool add_obj_vel   = (t.flags & 0x01) != 0;
    if (use_src_vel || use_src_accel) {
        base_vx = src.velocity_x;
        base_vy = src.velocity_y;
        // Original negates (EOR #&80 on computed angle); approximate by
        // flipping the signed sign of each axis so particles leave the
        // source opposite to its motion.
        base_vx = clamp_signed(-int(base_vx));
        base_vy = clamp_signed(-int(base_vy));
    }

    for (int k = 0; k < count; k++) {
        int slot = allocate_slot(rng);
        Particle& p = pool_[slot];

        // &2220-&222a: ttl.
        p.ttl = static_cast<uint8_t>((rng.next() & t.ttl_rand) + t.ttl_base);

        // &220d-&2215: colour and flags.
        uint8_t cf = (rng.next() & t.cf_rand) ^ t.cf_base;
        p.colour_and_flags = cf;

        // &222e-&2263: velocity (signed random + base) and position (fraction
        // jittered by *_rand). Done per-axis.
        auto axis = [&](uint8_t v_rand, int8_t base_v,
                        uint8_t pos_rand, uint8_t base_pos, uint8_t base_frac,
                        uint8_t& out_v_unsigned,
                        uint8_t& out_whole, uint8_t& out_frac) {
            uint8_t r = rng.next();
            bool negate = (r & 0x80) != 0;
            int8_t mag = static_cast<int8_t>((r >> 1) & v_rand);
            int v = int(base_v) + (negate ? -int(mag) : int(mag));
            out_v_unsigned = static_cast<uint8_t>(clamp_signed(v));

            uint8_t jitter = rng.next() & pos_rand;
            int sum = int(base_frac) + int(jitter);
            out_frac  = static_cast<uint8_t>(sum);
            out_whole = base_pos + (sum > 0xff ? 1 : 0);
        };

        uint8_t vxu = 0, vyu = 0;
        axis(t.vx_rand, base_vx, t.x_rand, base_x, base_x_frac,
             vxu, p.x, p.x_fraction);
        axis(t.vy_rand, base_vy, t.y_rand, base_y, base_y_frac,
             vyu, p.y, p.y_fraction);
        p.velocity_x = static_cast<int8_t>(vxu);
        p.velocity_y = static_cast<int8_t>(vyu);

        // &226a-&2281: optionally add the object's velocity on top.
        if (add_obj_vel) {
            p.velocity_x = clamp_signed(int(p.velocity_x) + int(src.velocity_x));
            p.velocity_y = clamp_signed(int(p.velocity_y) + int(src.velocity_y));
        }
    }
}

void ParticleSystem::emit_at(ParticleType type, uint8_t wx, uint8_t wy,
                              Random& rng) {
    if (type >= ParticleType::COUNT) return;
    const TypeData& t = TYPES[static_cast<int>(type)];

    int slot = allocate_slot(rng);
    Particle& p = pool_[slot];

    p.ttl = static_cast<uint8_t>((rng.next() & t.ttl_rand) + t.ttl_base);
    p.colour_and_flags = (rng.next() & t.cf_rand) ^ t.cf_base;

    // For star placement the 6502 pre-zeroes both fractions and both
    // velocity randomness fields are 0x00 (see STAR_OR_MUSHROOM row), so
    // the per-axis random loop collapses to "keep the base whole cell".
    auto axis = [&](uint8_t v_rand, uint8_t pos_rand, uint8_t base_pos,
                    uint8_t& out_v_unsigned, uint8_t& out_whole,
                    uint8_t& out_frac) {
        uint8_t r = rng.next();
        bool negate = (r & 0x80) != 0;
        int8_t mag = static_cast<int8_t>((r >> 1) & v_rand);
        int v = negate ? -int(mag) : int(mag);
        out_v_unsigned = static_cast<uint8_t>(clamp_signed(v));

        uint8_t jitter = rng.next() & pos_rand;
        out_frac  = jitter;
        out_whole = base_pos + (jitter > 0 ? 0 : 0); // no carry from 0
    };

    uint8_t vxu = 0, vyu = 0;
    axis(t.vx_rand, t.x_rand, wx, vxu, p.x, p.x_fraction);
    axis(t.vy_rand, t.y_rand, wy, vyu, p.y, p.y_fraction);
    p.velocity_x = static_cast<int8_t>(vxu);
    p.velocity_y = static_cast<int8_t>(vyu);
}

// Per-axis particle position+velocity randomisation used by emit_directed.
// Mirrors the inner add_particles_loop body at &2230-&2263: the velocity is
// the caller's base ± (rnd >> 1) & v_rand, the position is src + (rnd &
// pos_rand). Extracted to a static helper to avoid lambdas per CLAUDE.md.
static void emit_directed_axis(Random& rng,
                               int8_t base_v, uint8_t v_rand,
                               uint8_t base_pos, uint8_t base_frac, uint8_t pos_rand,
                               int8_t& out_v,
                               uint8_t& out_whole, uint8_t& out_frac) {
    uint8_t r = rng.next();
    bool negate = (r & 0x80) != 0;
    int8_t mag = static_cast<int8_t>((r >> 1) & v_rand);
    int v = int(base_v) + (negate ? -int(mag) : int(mag));
    out_v = static_cast<int8_t>(clamp_signed(v));

    uint8_t jitter = rng.next() & pos_rand;
    int sum = int(base_frac) + int(jitter);
    out_frac  = static_cast<uint8_t>(sum);
    out_whole = base_pos + (sum > 0xff ? 1 : 0);
}

void ParticleSystem::emit_directed(ParticleType type, uint8_t angle,
                                   const Object& src, Random& rng) {
    if (type >= ParticleType::COUNT) return;
    const TypeData& t = TYPES[static_cast<int>(type)];

    // &21d7-&21e1 in add_particle: magnitude = (rnd & spd_rand) + spd_base,
    // then (magnitude, angle) -> (vector_x, vector_y) via &2357. The 6502
    // doesn't bound-check the addition; let it wrap mod 256 like the original.
    uint8_t magnitude = static_cast<uint8_t>((rng.next() & t.spd_rand) + t.spd_base);
    int8_t base_vx = 0, base_vy = 0;
    NPC::vector_from_magnitude_and_angle(magnitude, angle, base_vx, base_vy);

    int slot = allocate_slot(rng);
    Particle& p = pool_[slot];

    p.ttl = static_cast<uint8_t>((rng.next() & t.ttl_rand) + t.ttl_base);
    p.colour_and_flags = (rng.next() & t.cf_rand) ^ t.cf_base;

    emit_directed_axis(rng, base_vx, t.vx_rand,
                       src.x.whole, src.x.fraction, t.x_rand,
                       p.velocity_x, p.x, p.x_fraction);
    emit_directed_axis(rng, base_vy, t.vy_rand,
                       src.y.whole, src.y.fraction, t.y_rand,
                       p.velocity_y, p.y, p.y_fraction);
}
