#include "ai/behaviors/environment.h"
#include "core/types.h"
#include "particles/particle_system.h"
#include <cstdlib>

namespace Behaviors {

// &4D72-&4D7C: per-colour-pair tables.
// speed table — how fast the door opens (halved when closing).
static constexpr uint8_t doors_speed_table[4]  = { 0x20, 0x10, 0x08, 0x20 };
// energy threshold below which the door is "being destroyed".
static constexpr uint8_t doors_energy_table[4] = { 0x80, 0x74, 0xc0, 0x80 };
// palette table (low 4 bits AND'd if unlocked, hiding colour-3 = lock).
static constexpr uint8_t doors_palette_table[8] = {
    0x2b, 0x2d, 0x15, 0x1c, 0x2b, 0x2d, 0x15, 0x1c,
};

namespace DoorFlag {
    constexpr uint8_t LOCKED            = 0x01;
    constexpr uint8_t OPENING           = 0x02;
    constexpr uint8_t MOVING            = 0x04;
    constexpr uint8_t SLOW_OR_DESTROYED = 0x08;
}

// &4C83-&4D71: Door state machine. Full port of the 6502 routine. Notes:
//  - obj.tertiary_data_offset holds the `data` byte (locked/opening/moving/
//    destroyed/colour bits).
//  - obj.tx is the open fraction (0=closed, 0xff=fully open).
//  - obj.state is a per-object "auto-close timer" (the original shares one
//    global `door_timer`; per-object is a safe approximation).
//  - obj.ty (bit 1) is orientation: 0=horizontal, 2=vertical.
// TODOs not yet wired: remote-control toggle of lock, the switch-effects
// integration (&4c9e / &31ac), and door destruction via energy.
void update_door(Object& obj, UpdateContext& ctx) {
    // &4c83-&4c8b: if something is touching but it can't trigger, mask it
    // out so the "touching" checks below don't see it.
    bool touching_trigger = false;
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        const Object& tch = ctx.mgr.object(obj.touching);
        uint8_t w = tch.weight();
        touching_trigger = (w >= 2);
    }

    // &4c8d: force vertical-flip clear (doors never render flipped).
    obj.flags &= ~ObjectFlags::FLIP_VERTICAL;

    // &4cab-&4cad: re-read data and mark the door as MOVING by default.
    uint8_t data = obj.tertiary_data_offset | DoorFlag::MOVING;

    // &4cb0-&4cc3: split data into opening/locked flags and colour.
    bool opening = (data & DoorFlag::OPENING) != 0;
    bool locked  = (data & DoorFlag::LOCKED)  != 0;
    bool slow    = (data & DoorFlag::SLOW_OR_DESTROYED) != 0;
    uint8_t colour      = (data >> 4) & 0x07;
    uint8_t colour_pair = colour & 0x03;

    // &4cbe-&4cd5: destruction / energy gating. If energy >= threshold,
    // refill to 0xff. Else, if already SLOW_OR_DESTROYED, drop energy to 0
    // so the main loop will remove the door. Otherwise set the slow bit.
    if (obj.energy >= doors_energy_table[colour_pair]) {
        obj.energy = 0xff;
    } else if (slow) {
        obj.energy = 0;
    } else {
        data |= DoorFlag::SLOW_OR_DESTROYED;
        slow = true;
    }

    // &4cd7-&4cec: pick a speed. Slow/destroyed doors always use 1.
    // Closing halves the speed; a closing door touched by something
    // crawls (speed 0xff, which is -1 in signed terms → almost no move).
    uint8_t speed = slow ? 1 : doors_speed_table[colour_pair];
    if (!opening) {
        speed >>= 1;
        if (touching_trigger) speed = 0xff;
    }

    // &4cee-&4d07: advance the open fraction. Subtract "speed" from
    // fraction EOR 0x80 and watch for a signed-overflow (V flag) crossing,
    // which signals we've hit an endpoint.
    uint8_t prev = obj.tx;
    // Work in signed 8-bit: start = (prev EOR 0x80), subtract speed (a
    // signed step: positive when opening, since EOR 0x80 inverts sense).
    int signed_start = static_cast<int8_t>(prev ^ 0x80);
    int signed_step  = static_cast<int8_t>(speed);
    int signed_next  = signed_start - signed_step;
    bool at_end = signed_next < -128 || signed_next > 127;

    if (at_end) {
        // Clamp and interpret (&4cf7-&4d0d).
        if (signed_next >  127) signed_next =  127;
        if (signed_next < -128) signed_next = -128;
        bool fully_closed = signed_next > 0;
        if (fully_closed) {
            // Stop the door (&4d09): clear MOVING.
            data &= ~DoorFlag::MOVING;
        }
        // Type 0/4 (cyG metal, rmB stone): auto-toggle via obj.state timer.
        bool auto_toggle = (colour_pair == 0);
        if (auto_toggle) {
            if (obj.state == 0 && !fully_closed) {
                obj.state = 60; // prepare 60-frame auto-close
            } else if (obj.state > 0) {
                obj.state--;
                if (obj.state == 0 && !touching_trigger && !locked) {
                    // Flip opening direction, start moving again.
                    data ^= DoorFlag::OPENING;
                    data |= DoorFlag::MOVING;
                }
            }
        }
        // If touching or unlocked-by-default, honour the direction as set.
    }

    // &4d3b-&4d4e: commit the new fraction (un-EOR), compute a per-frame
    // velocity byte from (new - old), and offset the door by 1 pixel along
    // its axis so the rendered sprite lines up.
    uint8_t new_frac = static_cast<uint8_t>(
        static_cast<uint8_t>(signed_next) ^ 0x80);
    obj.tx = new_frac;

    // &4d56-&4d5d: write updated data back. If being destroyed, force
    // MOVING so the explosion animation runs.
    if (obj.energy == 0) data |= DoorFlag::MOVING;
    obj.tertiary_data_offset = data;

    // &4d64-&4d6f: palette from colour; if unlocked, mask out colour-3
    // (the lock pip) by ANDing with 0x0f.
    uint8_t pal = doors_palette_table[colour];
    if (!locked) pal &= 0x0f;
    obj.palette = pal;
}

// &499D-&49C2: Switch. `tx` is a rolling 8-frame press-history register:
// each frame we ROR the "triggered this frame" carry into its bit 7. We
// fire the toggle only when bit 7 is set AND bits 0-6 are all clear —
// i.e. the first frame of a fresh press, suppressing auto-repeat for 7
// frames afterwards. Port of:
//   CLC; BMI not_touched; JSR check_if_object_can_trigger_switches
//   ROR tx; BPL skip; LDA tx; ASL A; BNE skip; ROL A; EOR data; STA data
void update_switch(Object& obj, UpdateContext& ctx) {
    // Carry-in = "triggered this frame". Touching with a trigger-capable
    // object sets it; check_if_object_can_trigger_switches filters out
    // very light objects, invisible debris, clawed robots, Triax, maggots.
    bool triggered = false;
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        Object& toucher = ctx.mgr.object(obj.touching);
        uint8_t w = toucher.weight();
        uint8_t t = static_cast<uint8_t>(toucher.type);
        bool heavy = (w >= 2);
        bool blacklisted =
            t == static_cast<uint8_t>(ObjectType::INVISIBLE_DEBRIS) ||
            t == static_cast<uint8_t>(ObjectType::MAGGOT) ||
            t == static_cast<uint8_t>(ObjectType::MAGENTA_CLAWED_ROBOT) ||
            t == static_cast<uint8_t>(ObjectType::CYAN_CLAWED_ROBOT) ||
            t == static_cast<uint8_t>(ObjectType::GREEN_CLAWED_ROBOT) ||
            t == static_cast<uint8_t>(ObjectType::RED_CLAWED_ROBOT) ||
            t == static_cast<uint8_t>(ObjectType::TRIAX);
        triggered = heavy && !blacklisted;
    }

    // ROR tx with carry = triggered. Old tx bit 0 is shifted out and lost.
    uint8_t new_tx = static_cast<uint8_t>(obj.tx >> 1);
    if (triggered) new_tx |= 0x80;
    obj.tx = new_tx;

    // Fire only if this is the leading edge of a press: bit 7 set AND all
    // other bits clear. (bit 7 → bit 7 of new_tx, rest was old bits 7..1.)
    if (obj.tx == 0x80) {
        // ROL A with A=0, carry=1 → A=1. EOR data; STA data toggles bit 0.
        obj.state ^= 0x01;
        obj.flags ^= ObjectFlags::FLIP_HORIZONTAL;
        // TODO: process_switch_effects, play_sound (&49b3, &49b6).
    }

    // &49c2: minimum energy 30 (gain_energy_and_flash_if_damaged).
    NPC::enforce_minimum_energy(obj, 0x1e);
}

// Transporter destination tables from &314a and &315a (16 destinations)
static constexpr uint8_t transporter_destinations_x[] = {
    0x62, 0xad, 0x2a, 0x0b, 0x9d, 0xaf, 0x9e, 0x45,
    0x89, 0x9d, 0xb5, 0xa2, 0x72, 0xa7, 0x9f, 0xb0,
};
static constexpr uint8_t transporter_destinations_y[] = {
    0xc7, 0x62, 0xcd, 0x0b, 0x58, 0x62, 0x69, 0x57,
    0x71, 0x3c, 0x66, 0x63, 0x54, 0x80, 0x49, 0x80,
};

// Palette cycle table from &4d82
static constexpr uint8_t transporter_palette_table[] = {
    0x52, 0x63, 0x35, 0x21, // rmM, rcC, gyB, rgG
};

// &4D86-&4DDE: Transporter beam. Faithful port of the control flow.
//   data (low bit 0): 1 = stationary beam, 0 = sweeping
//   data (bits 1-4):  destination index into transporter_destinations_[xy]
//   state:            current beam y-fraction (sweeps 0x00..0xb0 in steps
//                     of 0x20 then wraps when it would reach 0xb1).
// On contact with a non-teleporting object, latch its tx/ty to the
// destination, set the teleporting flag, copy this object's velocity into
// the touched object so it follows the beam briefly, and play the sound.
// The palette cycles through transporter_palette_table using the global
// frame counter via rotate_colour_from_A.
void update_transporter_beam(Object& obj, UpdateContext& ctx) {
    // &4d86-&4d89: split data into (stationary, destination).
    uint8_t data = obj.tertiary_data_offset;
    bool stationary     = (data & 0x01) != 0;
    uint8_t destination = (data >> 1) & 0x0f;

    if (!stationary) {
        // &4d8e-&4daa: if touching an object that isn't already teleporting,
        // latch the destination and start it teleporting with velocity
        // inherited from the beam.
        if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
            Object& touched = ctx.mgr.object(obj.touching);
            if (!(touched.flags & ObjectFlags::TELEPORTING)) {
                if (destination < 16) {
                    touched.tx = transporter_destinations_x[destination];
                    touched.ty = transporter_destinations_y[destination];
                }
                touched.flags |= ObjectFlags::TELEPORTING;
                touched.timer = 0x20;
                touched.velocity_x = obj.velocity_x;
                touched.velocity_y = obj.velocity_y;
                // TODO: play teleport sound (&4daa).
            }
        }

        // &4dad-&4dbb: unless NEWLY_CREATED, step the beam y-fraction by
        // 0x20 and wrap 0xb1..0xff back to 0x01..0x4f.
        if (!(obj.flags & ObjectFlags::NEWLY_CREATED)) {
            uint8_t next = static_cast<uint8_t>(obj.state + 0x20);
            if (next >= 0xb1) next = static_cast<uint8_t>(next - 0xb0);
            obj.state = next;
        }
    }
    // Stationary or newly-created: state keeps its current value.

    // &4dbd-&4dc6: the rendered y_fraction is either the state itself
    // (beam points down from a ceiling mount) or its invert_if_positive
    // (points up from a floor mount), then minus 1.
    uint8_t beam = obj.state;
    uint8_t y_frac = obj.is_flipped_v() ? static_cast<uint8_t>(-beam) : beam;
    obj.y.fraction = static_cast<uint8_t>(y_frac - 1);

    // &4dc8-&4dd1: remote-control hit toggles the transporter lock bits.
    // TODO: port check_if_object_hit_by_remote_control + consider_toggling_lock.

    // &4dd2-&4ddc: palette cycles via rotate_colour_from_A using the
    // *global* frame_counter (not obj's local counter).
    uint8_t idx = (ctx.frame_counter >> 2) & 0x03;
    obj.palette = transporter_palette_table[idx];
}

// &4BAF: Hive update (small and large)
void update_hive(Object& obj, UpdateContext& ctx) {
    NPC::enforce_minimum_energy(obj, 0x46);

    // Spawn creatures every N frames
    if (!ctx.every_four_frames) return;

    // Determine spawn type from data
    ObjectType spawn_type = ObjectType::WASP;
    if (obj.type == ObjectType::SMALL_HIVE) {
        spawn_type = ObjectType::PIRANHA;
    }

    // Random chance to spawn
    if (ctx.rng.next() > 0x20) return;

    // Create spawn
    int slot = ctx.mgr.create_object_at(spawn_type, 4, obj);
    if (slot >= 0) {
        Object& spawn = ctx.mgr.object(slot);
        // Set initial velocity away from hive
        spawn.velocity_x = obj.is_flipped_h() ? -4 : 4;
        spawn.velocity_y = -2;
    }
}

// &4789: Dense nest - spawns creatures from nest
void update_dense_nest(Object& obj, UpdateContext& ctx) {
    NPC::enforce_minimum_energy(obj, 0x7f);
    // Dense nests absorb objects that touch them
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS && obj.touching != 0) {
        Object& other = ctx.mgr.object(obj.touching);
        // Absorb non-player objects
        if (other.weight() <= 4) {
            ctx.mgr.remove_object(obj.touching);
        }
    }
}

// &4DED: Sucking nest - pulls objects toward it
void update_sucking_nest(Object& obj, UpdateContext& ctx) {
    NPC::enforce_minimum_energy(obj, 0x7f);

    // Pull nearby objects toward nest
    for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        Object& other = ctx.mgr.object(i);
        if (!other.is_active()) continue;

        int8_t dx = static_cast<int8_t>(obj.x.whole - other.x.whole);
        int8_t dy = static_cast<int8_t>(obj.y.whole - other.y.whole);
        uint8_t dist = static_cast<uint8_t>(std::max(std::abs(dx), std::abs(dy)));

        if (dist < 6) {
            // Pull toward nest
            if (dx > 0 && other.velocity_x < 4) other.velocity_x++;
            if (dx < 0 && other.velocity_x > -4) other.velocity_x--;
            if (dy > 0 && other.velocity_y < 4) other.velocity_y++;
            if (dy < 0 && other.velocity_y > -4) other.velocity_y--;
        }
    }

    // Absorb objects that reach the nest
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS && obj.touching != 0) {
        ctx.mgr.remove_object(obj.touching);
    }
}

// &4BA9: Bush - grows back, can be destroyed
void update_bush(Object& obj, UpdateContext& ctx) {
    // Bushes slowly regenerate
    if (ctx.every_sixty_four_frames) {
        if (obj.energy < 0xff) obj.energy++;
    }
}

// &40EE: Cannon - fires cannonballs periodically
void update_cannon(Object& obj, UpdateContext& ctx) {
    NPC::enforce_minimum_energy(obj, 0x3f);

    if (ctx.every_thirty_two_frames) {
        int slot = NPC::fire_projectile(obj, ObjectType::CANNONBALL, ctx);
        if (slot >= 0) {
            Object& ball = ctx.mgr.object(slot);
            ball.velocity_x = obj.is_flipped_h() ? -0x18 : 0x18;
            ball.velocity_y = -0x08;
            ball.timer = 64;
        }
    }
}

// &419F: Maggot machine - spawns maggots
void update_maggot_machine(Object& obj, UpdateContext& ctx) {
    if (ctx.every_sixty_four_frames) {
        int slot = ctx.mgr.create_object_at(ObjectType::MAGGOT, 4, obj);
        if (slot >= 0) {
            Object& maggot = ctx.mgr.object(slot);
            maggot.velocity_x = (ctx.rng.next() & 0x07) - 3;
            maggot.velocity_y = -4;
        }
    }
}

// &4C15-&4C82: Engine fire.
//   state   = 8-bit timer (free-runs while burning; once it hits 0x80 we set
//             the "inactive" bit in data and stop burning).
//   tertiary_data_offset bit 0/1 = "engine inactive" (we approximate the
//   original's full tertiary-data field with just a bool flag).
// Each live frame: random flip, heat-push the touching object (INC velocity_x),
// emit a particle, 1-in-4 accelerate nearby objects with damage, then set
// palette rwY (0x34) and a random x_fraction 0x90..0xcf. While inactive the
// fire is hidden (palette 0, x_fraction 0x40) and the timer is reset to 0.
void update_engine_fire(Object& obj, UpdateContext& ctx) {
    // &4c15: if engine is off, go straight to reset-and-hide.
    bool inactive = (obj.tertiary_data_offset & 0x03) != 0;
    if (!inactive) {
        // &4c19: advance timer. When it flips to 0x80, mark the engine off.
        obj.state++;
        if (obj.state & 0x80) {
            obj.tertiary_data_offset |= 0x02;
            inactive = true;
        }
    }

    if (inactive) {
        // reset_and_hide_fire (&4c78) / hide_fire (&4c7a)
        obj.state = 0;
        obj.palette = 0x00;        // kyK
        obj.x.fraction = 0x40;     // hide behind foreground
        return;
    }

    // &4c21-&4c25: fire more likely to hide later in its burn.
    uint8_t r = ctx.rng.next();
    if (r < obj.state) {
        // hide_fire path — palette=0, x_fraction=0x40.
        obj.palette = 0x00;
        obj.x.fraction = 0x40;
        return;
    }

    // &4c27-&4c2b: random flip bits driven by (r<<1, r<<2).
    uint8_t flip = static_cast<uint8_t>(r << 1);
    if (flip & 0x80) obj.flags |= ObjectFlags::FLIP_HORIZONTAL;
    else             obj.flags &= ~ObjectFlags::FLIP_HORIZONTAL;
    uint8_t vf = static_cast<uint8_t>(flip << 1);
    if (vf & 0x80)   obj.flags |= ObjectFlags::FLIP_VERTICAL;
    else             obj.flags &= ~ObjectFlags::FLIP_VERTICAL;

    // &4c2d-&4c32: push any touching object outward by bumping velocity_x.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        ctx.mgr.object(obj.touching).velocity_x++;
    }

    // &4c34-&4c49: emit a single engine-fire particle.
    if (ctx.particles) ctx.particles->emit(ParticleType::ENGINE, 1, obj, ctx.rng);

    // &4c4c-&4c64: 1-in-4 frames, accelerate nearby objects with damage.
    if (((ctx.frame_counter + obj.y.whole) & 0x03) == 0) {
        // accelerate_all_objects_within_angle power=0x50, range=+/-28° (0x14),
        // damage_targets=true. Approximation: knock back any object within
        // Chebyshev distance 10 on the fire's right/left.
        for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            Object& o = ctx.mgr.object(i);
            if (!o.is_active()) continue;
            int8_t dx = static_cast<int8_t>(o.x.whole - obj.x.whole);
            int8_t dy = static_cast<int8_t>(o.y.whole - obj.y.whole);
            if (std::abs(dx) > 10 || std::abs(dy) > 4) continue;
            int push = (dx >= 0) ? +4 : -4;
            int nv = int(o.velocity_x) + push;
            if (nv >  127) nv =  127;
            if (nv < -128) nv = -128;
            o.velocity_x = static_cast<int8_t>(nv);
        }
        // TODO: play engine-fire sound (&4c61).
    }

    // &4c68-&4c80: palette rwY + random x_fraction in [0x90, 0xcf].
    obj.palette = 0x34;
    uint8_t fc = ctx.frame_counter;
    // ROL x4 then ADC frame_counter (simulate the original's bit rotation).
    uint8_t rot = static_cast<uint8_t>((fc << 4) | (fc >> 4));
    uint8_t xf  = static_cast<uint8_t>(rot + fc) & 0x3f;
    obj.x.fraction = static_cast<uint8_t>(xf + 0x90);
}

// &4B64: Placeholder - does nothing
void update_placeholder(Object& obj, UpdateContext& ctx) {
    // Placeholder objects have no behavior
}

} // namespace Behaviors
