#pragma once
#include <cstdint>
#include "core/random.h"

struct Object;

// ============================================================================
// Particle system — port of &2078-&2287 (update + emit) and the per-type
// tables at &0206-&0276.
// ============================================================================
//
// Each particle stores a 16-bit position (whole+fraction), 8-bit signed
// velocity per axis, a time-to-live counter, and a colour/flags byte:
//
//   colour_and_flags bits:
//     0x80 : always set; temporarily cleared while unplotting double-height
//            halves that cross a screen edge
//     0x40 : particle is double-height (two stacked pixels)
//     0x20 : plotted on foreground (if clear, particle is removed on
//            colliding with foreground geometry)
//     0x10 : accelerates (gravity pulls down, water pushes up)
//     0x08 : cycle colour each frame
//     0x07 : current colour (0-7)
//
// Particles live in a fixed-size pool. When the pool is full, a new
// particle overwrites a random existing one (matching &2174).

struct Particle {
    int8_t  velocity_x = 0;
    int8_t  velocity_y = 0;
    uint8_t x_fraction = 0;
    uint8_t y_fraction = 0;
    uint8_t x          = 0;
    uint8_t y          = 0;
    uint8_t ttl        = 0;
    uint8_t colour_and_flags = 0;
};

namespace ParticleFlag {
    constexpr uint8_t ALWAYS_SET  = 0x80;
    constexpr uint8_t DOUBLE      = 0x40;
    constexpr uint8_t FOREGROUND  = 0x20;
    constexpr uint8_t ACCELERATE  = 0x10;
    constexpr uint8_t CYCLE       = 0x08;
    constexpr uint8_t COLOUR_MASK = 0x07;
}

// Particle type IDs — renumbered from the 6502's packed-offset IDs
// (&00, &0b, &16, …) to a simple 0..10 index.
enum class ParticleType : uint8_t {
    PLASMA            = 0,
    JETPACK           = 1,
    EXPLOSION         = 2,
    FIREBALL          = 3,
    PROJECTILE_TRAIL  = 4,
    ENGINE            = 5,
    AIM               = 6,
    STAR_OR_MUSHROOM  = 7,
    FLASK             = 8,
    WATER             = 9,
    WIND              = 10,
    COUNT             = 11,
};

class ParticleSystem {
public:
    static constexpr int MAX_PARTICLES = 32;

    void clear() { n_ = 0; }

    // Per-frame tick: apply acceleration (gravity, or water float),
    // rotate cycling colours, decrement ttl, integrate position. Particles
    // whose ttl reaches 0 are compacted out of the pool.
    void update(uint8_t waterline_y, uint8_t waterline_y_frac, Random& rng);

    // Emit `count` particles of the given type, spawning from `src`'s
    // position. Port of &218c / &218e (add_particle / add_particles).
    void emit(ParticleType type, int count, const Object& src, Random& rng);

    // Emit a single particle at an explicit world tile (whole coords only,
    // fractions = 0). Used by the star-field at &26ce-&26e3 which fills
    // &87/&89/&8b/&8d directly before calling add_particle, bypassing the
    // object-position path.
    void emit_at(ParticleType type, uint8_t wx, uint8_t wy, Random& rng);

    // Like `emit`, but the particle's base velocity comes from the
    // 6502 angle/magnitude path (&21d7-&21e1 in add_particle) instead of the
    // object's own velocity. The magnitude is drawn from the type's
    // spd_rand/spd_base table fields, and (magnitude, angle) is converted to
    // (vx, vy) via vector_from_magnitude_and_angle. Per-particle ±vx_rand /
    // ±vy_rand jitter is then added on top, matching the 6502.
    //
    // This is the path used by water-splash particles (angle=0xc0, "shoot
    // up out of the splash") at &2f6d-&2f82 and by wind-tile particles
    // (angle = direction of the wind vector) at &3f73-&3f91. The existing
    // `emit` ignores spd_rand/spd_base, which left WATER and WIND particles
    // drifting only by random jitter rather than in any directional way.
    void emit_directed(ParticleType type, uint8_t angle,
                       const Object& src, Random& rng);

    int count() const { return n_; }
    const Particle& get(int i) const { return pool_[i]; }

private:
    Particle pool_[MAX_PARTICLES];
    int n_ = 0;

    int allocate_slot(Random& rng);
};
