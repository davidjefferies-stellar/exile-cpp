#pragma once
#include <cstdint>
#include "core/random.h"

struct Object;

// Particle system — &2078-&2287 (update + emit), tables at &0206-&0276.
// colour_and_flags: 0x80 always-set, 0x40 double, 0x20 foreground,
// 0x10 accel, 0x08 cycle, 0x07 colour. Full pool evicts random (&2174).

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
    // 6502 had 32 slots; our wider viewport needs more (star-field scales
    // with width). Must stay a power of two — allocate_slot's full-pool
    // eviction does `& (MAX_PARTICLES - 1)`.
    static constexpr int MAX_PARTICLES = 256;

    void clear() { n_ = 0; }

    // Per-frame tick: apply acceleration (gravity, or water float),
    // rotate cycling colours, decrement ttl, integrate position. Particles
    // whose ttl reaches 0 are compacted out of the pool.
    void update(uint8_t waterline_y, uint8_t waterline_y_frac, Random& rng);

    // &218c/&218e add_particle(s). `angle` is the 6502 zp &b5 byte;
    // overridden from src velocity (EOR #&80) when flags use_src_vel
    // bit is set. Default 0xc0 (upward) is the common call-site value.
    // `cf_base_override` < 0 keeps the type's stock cf_base from the
    // TYPES table; >= 0 substitutes the byte directly (port of the
    // 6502's &46e4 STA particle_types_colour_and_flags_table+&2c
    // trick used by per-bullet projectile-trail colours at &46e1).
    void emit(ParticleType type, int count, const Object& src, Random& rng,
              uint8_t angle = 0xc0, int cf_base_override = -1);

    // Emit a single particle at an explicit world tile (whole coords only,
    // fractions = 0). Used by the star-field at &26ce-&26e3 which fills
    // &87/&89/&8b/&8d directly before calling add_particle, bypassing the
    // object-position path.
    void emit_at(ParticleType type, uint8_t wx, uint8_t wy, Random& rng);

    // &21d7-&21e1 angle/magnitude path — base velocity from type's
    // spd_rand/spd_base instead of source object. Used by water-splash
    // (&2f6d-&2f82) and wind (&3f73-&3f91); regular emit ignores spd.
    void emit_directed(ParticleType type, uint8_t angle,
                       const Object& src, Random& rng);

    int count() const { return n_; }
    const Particle& get(int i) const { return pool_[i]; }

    // Swap-remove (port of the 6502 free path at &213a remove_particle).
    // Caller must use a descending loop so the swap doesn't skip slots.
    void remove(int i) {
        if (i < 0 || i >= n_) return;
        pool_[i] = pool_[n_ - 1];
        n_--;
    }

    // Restore a single particle byte-for-byte. Used by save/load and the
    // rewind ring buffer — bypasses the random allocator since the saved
    // state has the slot's full velocity/position/colour already.
    bool push_raw(const Particle& p) {
        if (n_ >= MAX_PARTICLES) return false;
        pool_[n_++] = p;
        return true;
    }

private:
    Particle pool_[MAX_PARTICLES];
    int n_ = 0;

    int allocate_slot(Random& rng);
};
