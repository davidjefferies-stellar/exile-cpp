#include "behaviours/environment.h"
#include "behaviours/path.h"
#include "audio/audio.h"
#include "core/types.h"
#include "objects/object_data.h"
#include "particles/particle_system.h"
#include <algorithm>
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
// Faithful port of &4c83-&4d71 update_door.
//
// Per-frame door lifecycle:
//   - tx          = "open fraction" (0x00 open → 0xff closed)
//   - state       = tile x (horizontal door) or tile y (vertical door), the
//                   fixed axis coord the door slides past
//   - ty (bit 1)  = orientation, 0=horizontal (slides along x), 2=vertical
//   - data byte   = LOCKED / OPENING / MOVING / SLOW_OR_DESTROYED + colour
//
// Each frame the routine:
//   1. Masks touching back to "none" if the toucher is too light/wrong type.
//   2. Refills energy if above a colour-dependent threshold, else marks the
//      door being destroyed or sets energy=0 for the explosion.
//   3. Picks a signed "door speed" — positive when opening, negative (via
//      EOR #&ff) when closing, halved when closing, clamped to -1 if
//      something is in the way.
//   4. Runs signed SBC on (tx EOR 0x80) to get the new open fraction. The
//      6502's V flag catches crossings of the 0x80 boundary (i.e. the door
//      hitting an endpoint); at that point we clamp, and decide whether to
//      stop, toggle direction, or start the global &0819 door_timer.
//   5. Writes tx, writes axis_fraction = tx + 0x10 (carry to axis_whole),
//      sets a per-frame velocity from (new_tx - old_tx)/2, and commits the
//      data byte + palette.
void update_door(Object& obj, UpdateContext& ctx) {
    // &4c83-&4c8b: mask "touched but untriggerable" back to none. Very
    // light objects, invisible debris, clawed robots and Triax don't
    // count — check_if_object_can_trigger_switches is the same gate the
    // switch uses.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        const Object& tch = ctx.mgr.object(obj.touching);
        uint8_t w = tch.weight();
        uint8_t t = static_cast<uint8_t>(tch.type);
        bool heavy = (w >= 2);
        bool blacklisted =
            t == static_cast<uint8_t>(ObjectType::INVISIBLE_DEBRIS) ||
            t == static_cast<uint8_t>(ObjectType::MAGENTA_CLAWED_ROBOT) ||
            t == static_cast<uint8_t>(ObjectType::CYAN_CLAWED_ROBOT) ||
            t == static_cast<uint8_t>(ObjectType::GREEN_CLAWED_ROBOT) ||
            t == static_cast<uint8_t>(ObjectType::RED_CLAWED_ROBOT) ||
            t == static_cast<uint8_t>(ObjectType::TRIAX);
        if (!heavy || blacklisted) obj.touching = 0x80;
    }
    bool touched = (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS);

    // &4c8d: doors never render v-flipped.
    obj.flags &= ~ObjectFlags::FLIP_VERTICAL;

    // &4c8f-&4c97: pin axis_whole to state, seed axis_fraction to 0xff
    // (will be overwritten after the move math below).
    bool vertical = (obj.ty & 0x02) != 0;
    if (vertical) {
        obj.y.whole    = obj.state;
        obj.y.fraction = 0xff;
    } else {
        obj.x.whole    = obj.state;
        obj.x.fraction = 0xff;
    }

    // &4cab-&4cad: always re-set MOVING while the door is being ticked.
    uint8_t data = obj.tertiary_data_offset | DoorFlag::MOVING;

    // &4c9e check_if_object_hit_by_remote_control + &31ac consider_
    // toggling_lock. Port of the RCD door-unlock path.
    //
    // The 6502 hit test at &0bc5 is three-part: (a) the fired object's
    // type is REMOTE_CONTROL_DEVICE (&4e), (b) the fired object is
    // within ~3 tiles of this door with clear LOS, (c) the player's
    // aiming-angle vector points within a narrow cone at the door.
    // Our simplified check drops the angle cone and uses Chebyshev
    // distance; good enough for gameplay until the full aim-vector
    // geometry is ported. The fired object's position is the player's
    // position (RCD is held), so distance is effectively "is the
    // player within 3 tiles of the door".
    //
    // When hit, toggle DOOR_FLAG_LOCKED iff the matching key has been
    // collected, matching &31c2-&31cd exactly: clear MOVING if now
    // locked, set OPENING if now unlocked so the door starts moving
    // on the next tick.
    if (ctx.player_object_fired < GameConstants::PRIMARY_OBJECT_SLOTS &&
        ctx.player_keys_collected) {
        const Object& fired = ctx.mgr.object(ctx.player_object_fired);
        if (fired.is_active() &&
            fired.type == ObjectType::REMOTE_CONTROL_DEVICE) {
            int8_t dx = static_cast<int8_t>(fired.x.whole - obj.x.whole);
            int8_t dy = static_cast<int8_t>(fired.y.whole - obj.y.whole);
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            if (adx <= 3 && ady <= 3) {
                uint8_t door_colour = (obj.tertiary_data_offset >> 4) & 0x07;
                if (ctx.player_keys_collected[door_colour] & 0x80) {
                    data ^= DoorFlag::LOCKED;
                    if (data & DoorFlag::LOCKED) {
                        data &= ~DoorFlag::MOVING;
                    } else {
                        data |= DoorFlag::OPENING;
                    }
                    // &31d0-&31d3 lock/unlock chime — same params for
                    // both directions in the 6502.
                    static constexpr uint8_t kSoundLock[4] = { 0x94, 0x64, 0xba, 0xc4 };
                    Audio::play_at(Audio::CH_ANY, kSoundLock, obj.x.whole, obj.y.whole);
                }
            }
        }
    }

    bool opening = (data & DoorFlag::OPENING) != 0;
    bool locked  = (data & DoorFlag::LOCKED)  != 0;
    bool slow    = (data & DoorFlag::SLOW_OR_DESTROYED) != 0;
    uint8_t colour      = (data >> 4) & 0x07;
    uint8_t colour_pair = colour & 0x03;

    // &4cbe-&4cd5: energy / destruction ladder.
    if (obj.energy >= doors_energy_table[colour_pair]) {
        obj.energy = 0xff;
    } else if (slow) {
        obj.energy = 0;
    } else {
        data |= DoorFlag::SLOW_OR_DESTROYED;
        slow = true;
    }

    // &4cd7-&4cec: door speed.
    //   slow → 1
    //   opening → table[colour_pair]
    //   closing → halved-and-negated (EOR #&ff) table value
    //   closing + touching → -1 (0xff)
    uint8_t speed = slow ? 1 : doors_speed_table[colour_pair];
    if (!opening) {
        speed >>= 1;
        speed = static_cast<uint8_t>(~speed);
        if (touched) speed = 0xff;
    }

    // &4cee-&4cf5: step the fraction. tx is stored un-EOR'd (0xff closed,
    // 0x00 open); the 6502 flips it to signed via EOR #&80 so the endpoint
    // check is a V-flag crossing rather than an unsigned wrap.
    uint8_t prev_tx = obj.tx;
    int signed_start = static_cast<int8_t>(prev_tx ^ 0x80);
    int signed_speed = static_cast<int8_t>(speed);
    int wide_next    = signed_start - signed_speed;
    bool at_end      = (wide_next > 127 || wide_next < -128);

    if (at_end) {
        // &4cf7 prevent_overflow: clamp to the saturating signed endpoint.
        if (wide_next >  127) wide_next =  127;
        if (wide_next < -128) wide_next = -128;

        // &4cfb BPL stop_door: positive clamp = door reached the closed
        // end. Negative clamp = open end.
        bool closed_end = (wide_next > 0);

        if (closed_end) {
            // &4d09 stop_door: clear MOVING (the default OR at step 1
            // will re-set it next frame if something changes).
            data &= ~DoorFlag::MOVING;
        } else if (colour_pair == 0) {
            // &4cfd-&4d07: at open end for auto-cycling cyG/rmB doors.
            // If door_timer is still above 20, leave the door open
            // (skip_stopping_door); else fall through to toggle_door_opening.
            if (ctx.mgr.door_timer_ < 20) {
                data ^= DoorFlag::OPENING;   // start closing
                opening = (data & DoorFlag::OPENING) != 0;
                // &4d2a / &4d33: play opening or closing sound on the
                // direction toggle. The 6502 dispatches the same way
                // (BEQ is_closing branch on the OPENING bit).
                static constexpr uint8_t kSoundDoorOpen[4]  = { 0xc7, 0xc3, 0xc1, 0x13 };
                static constexpr uint8_t kSoundDoorClose[4] = { 0xc7, 0xc3, 0xc1, 0x03 };
                Audio::play_at(Audio::CH_ANY,
                               opening ? kSoundDoorOpen : kSoundDoorClose,
                               obj.x.whole, obj.y.whole);
            }
        }
        // Non-zero colour_pair at the open end with no touch/unlock fall
        // through to the touched-and-unlocked block below.

        // &4d0d-&4d1f: touched + unlocked reactions.
        if (touched && !locked) {
            if (colour_pair != 0) {
                // Immediately toggle direction on contact.
                data ^= DoorFlag::OPENING;
                opening = (data & DoorFlag::OPENING) != 0;
                static constexpr uint8_t kSoundDoorOpen[4]  = { 0xc7, 0xc3, 0xc1, 0x13 };
                static constexpr uint8_t kSoundDoorClose[4] = { 0xc7, 0xc3, 0xc1, 0x03 };
                Audio::play_at(Audio::CH_ANY,
                               opening ? kSoundDoorOpen : kSoundDoorClose,
                               obj.x.whole, obj.y.whole);
            } else if (ctx.mgr.door_timer_ == 0) {
                // cyG/rmB doors: arm the 60-frame hold-open timer and
                // toggle direction (fallthrough from &4d1f into &4d22).
                ctx.mgr.door_timer_ = 60;
                data ^= DoorFlag::OPENING;
                opening = (data & DoorFlag::OPENING) != 0;
            }
        }
    }

    // &4d3b-&4d46: write back the new fraction + a velocity byte derived
    // from (new - old)/2 keeping sign. velocity feeds the non-door's
    // physics when something rides / bumps the door (e.g. bullets).
    uint8_t new_tx = static_cast<uint8_t>(
        static_cast<uint8_t>(static_cast<int8_t>(wide_next)) ^ 0x80);
    obj.tx = new_tx;

    int diff = static_cast<int>(new_tx) - static_cast<int>(prev_tx);
    int8_t velocity = static_cast<int8_t>(diff / 2);

    // &4d4b-&4d54: axis_fraction = new_tx + 0x10 (1 pixel offset), axis
    // whole += carry from that addition. Lets the door sprite move past
    // the end of its home tile when fully closed.
    int fsum = static_cast<int>(new_tx) + 0x10;
    uint8_t axis_frac = static_cast<uint8_t>(fsum & 0xff);
    uint8_t axis_carry = (fsum > 0xff) ? 1 : 0;

    if (vertical) {
        obj.velocity_y = velocity;
        obj.velocity_x = 0;
        obj.y.fraction = axis_frac;
        obj.y.whole    = static_cast<uint8_t>(obj.state + axis_carry);
    } else {
        obj.velocity_x = velocity;
        obj.velocity_y = 0;
        obj.x.fraction = axis_frac;
        obj.x.whole    = static_cast<uint8_t>(obj.state + axis_carry);
    }

    // &4d56-&4d5d: if being destroyed, force MOVING so the explosion
    // animation runs; write the data byte back.
    if (obj.energy == 0) data |= DoorFlag::MOVING;
    obj.tertiary_data_offset = data;

    // Mirror the live data to tertiary storage so a later switch effect
    // reads (and XORs into) the current state rather than the original
    // initial value. Bit 7 MUST stay clear here: it's the spawn gate, and
    // setting it while the door is still primary would cause the tile-plot
    // loop to spawn duplicate primaries every render tick. The 6502 does
    // the same (&4d5f-&4d61 writes A = data without bit 7 set);
    // return_to_tertiary re-applies bit 7 only on demote.
    if (obj.tertiary_slot > 0) {
        ctx.mgr.set_tertiary_data_byte(obj.tertiary_slot,
                                        static_cast<uint8_t>(data & 0x7f));
    }

    // &4d64-&4d6f: palette from door colour. Unlocked doors strip colour 3
    // (the lock pip) by ANDing with 0x0f.
    uint8_t pal = doors_palette_table[colour];
    if (!locked) pal &= 0x0f;
    obj.palette = pal;
}

// Port of &4958 switch_effects_table. Each 0x00 byte delimits the start of
// a group; the (effect_id+1)-th zero starts group `effect_id`. Each
// subsequent non-zero byte is an offset into tertiary_objects_data that the
// switch toggles bits in.
static constexpr uint8_t switch_effects_table[] = {
    0x00, 0xb0, 0xbb, 0x84,              // 0x00: switch at (&d5,&73)
    0x00, 0x0f, 0x29,                    // 0x01: switch at (&9d,&3b)
    0x00, 0xc5,                          // 0x02: switch at (&95,&5d)
    0x00, 0xe7, 0x8f,                    // 0x03: switch at (&29,&c8)
    0x00, 0x8a,                          // 0x04: switch at (&7c,&c0)
    0x00, 0x13,                          // 0x05: switch at (&4d,&80)
    0x00, 0x8e, 0x32,                    // 0x06: switch at (&a1,&58)
    0x00, 0xc2,                          // 0x07: switch at (&6a,&de)
    0x00, 0x11, 0xaa, 0xbd,              // 0x08: switches at (&46,&56),(&8b,&71)
    0x00, 0x58, 0xcc, 0x55, 0xbc,        // 0x09: switch at (&ab,&6b)
    0x00, 0x55,                          // 0x0a: invisible switch at (&a8,&69)
    0x00, 0x46, 0xa9,                    // 0x0b: switches at (&d4,&6f),(&d5,&73)
    0x00, 0x6a, 0x8b,                    // 0x0c: switch at (&e3,&9c)/(&e3,&bc)
    0x00, 0xe6, 0x85, 0xd8,              // 0x0d: switch at (&67,&cb)
    0x00, 0xc7, 0x88,                    // 0x0e: invisible+visible at (&b4,&c2)
    0x00, 0x68,                          // 0x0f: switch at (&c4,&c4)
    0x00, 0x14,                          // 0x10: invisible switch at (&c1,&7c)
    0x00, 0x28, 0x4c,                    // 0x11: invisible switch at (&9b,&3b)
    0x00, 0x65,                          // 0x12: invisible switch at (&c6,&7c)
    0x00, 0x89,                          // 0x13: invisible switch at (&80,&c2)
    0x00, 0x8d,                          // 0x14: invisible switch at (&67,&da)
    0x00, 0x64, 0x2a,                    // 0x15: invisible switch at (&a9,&9c)
    0x00, 0x6b,                          // 0x16: invisible switch at (&eb,&bc)
    0x00, 0xa7, 0xb9, 0x10,              // 0x17: invisible switches at (&87..)
    0x00,                                // end sentinel
};

// Port of &49db process_switch_effects. Decodes (effect_id, toggle, mask)
// from the switch's data byte and applies the toggle to every tertiary
// target listed in the matching switch_effects_table group.
//
// Our primaries hold a live copy of the data byte (bit 7 stripped) and only
// write back to tertiary_data_ on demote, so the tertiary copy is stale
// while an object is onscreen. To avoid toggling a stale bit, read the
// authoritative value from the primary when one owns the slot, and write
// through to both stores when toggling.
static void process_switch_effects(ObjectManager& mgr, uint8_t effect_id,
                                    uint8_t mask, uint8_t toggle) {
    const int N = static_cast<int>(sizeof(switch_effects_table));
    int zeros_seen = 0;
    const int required = static_cast<int>(effect_id) + 1;
    for (int idx = 0; idx < N; ++idx) {
        uint8_t b = switch_effects_table[idx];
        if (b == 0) {
            ++zeros_seen;
            if (zeros_seen > required) return; // end of our group
            continue;
        }
        if (zeros_seen != required) continue;  // in an earlier group

        // Find the first primary that owns this tertiary slot (the live
        // copy of the state is in its tertiary_data_offset). If the slot
        // is currently unattached, fall back to the tertiary store.
        int first_owner = -1;
        for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; ++i) {
            const Object& p = mgr.object(i);
            if (p.is_active() && p.tertiary_slot == b) {
                first_owner = i;
                break;
            }
        }
        uint8_t prev = (first_owner >= 0)
            ? mgr.object(first_owner).tertiary_data_offset
            : static_cast<uint8_t>(mgr.tertiary_data_byte(b) & 0x7f);
        uint8_t newv = static_cast<uint8_t>((prev & mask) ^ toggle);

        if (first_owner >= 0) {
            // Update every primary tied to this slot — normally there's
            // exactly one, but if a stray duplicate ever leaked through
            // (old bug) we want both copies animating in lockstep rather
            // than one stuck at the original state. Tertiary copy gets
            // the live value without the spawn gate; return_to_tertiary
            // re-applies bit 7 when a primary is demoted.
            for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; ++i) {
                Object& p = mgr.object(i);
                if (p.is_active() && p.tertiary_slot == b) {
                    p.tertiary_data_offset = newv;
                }
            }
            mgr.set_tertiary_data_byte(b, newv);
        } else {
            // Not currently primary — preserve the spawn gate so the tile
            // still spawns the object next time it comes into view.
            mgr.set_tertiary_data_byte(b, static_cast<uint8_t>(newv | 0x80));
        }
    }
}

// &499D-&49C2: Switch. `tx` is a rolling 8-frame press-history register:
// each frame we ROR the "triggered this frame" carry into its bit 7. We
// fire the toggle only when bit 7 is set AND bits 0-6 are all clear —
// i.e. the first frame of a fresh press, suppressing auto-repeat for 7
// frames afterwards. On the leading edge we toggle bit 0 of the switch's
// data byte (switch state) and call process_switch_effects, which XORs
// bits 1-2 (the toggle mask) into every target listed in the switch's
// effect-id group.
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

    // Leading-edge of press: bit 7 set AND all other bits clear.
    if (obj.tx == 0x80) {
        // &49ac-&49af: toggle bit 0 of data (switch state), then call
        // process_switch_effects with the NEW data value.
        obj.tertiary_data_offset ^= 0x01;
        uint8_t data   = obj.tertiary_data_offset;
        uint8_t toggle = static_cast<uint8_t>((data >> 1) & 0x03);
        uint8_t effect = static_cast<uint8_t>(data >> 3);
        // Mirror the change into the tertiary slot too.
        if (obj.tertiary_slot > 0) {
            ctx.mgr.set_tertiary_data_byte(obj.tertiary_slot, data);
        }
        process_switch_effects(ctx.mgr, effect, /*mask=*/0xff, toggle);
        // &49b6-&49b9: switch click.
        static constexpr uint8_t kSoundSwitch[4] = { 0x3d, 0x04, 0x11, 0xd4 };
        Audio::play_at(Audio::CH_ANY, kSoundSwitch, obj.x.whole, obj.y.whole);

        ctx.mgr.debug_switch_presses_++;
    }

    // &49bd-&49c1: sprite flips horizontally based on bit 0 of the data.
    if (obj.tertiary_data_offset & 0x01) obj.flags |=  ObjectFlags::FLIP_HORIZONTAL;
    else                                 obj.flags &= ~ObjectFlags::FLIP_HORIZONTAL;

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
                // &4daa play_sound_for_teleporting (&440d → JSR
                // play_sound, params 29 c2 37 f3).
                static constexpr uint8_t kSoundTeleport[4] = { 0x29, 0xc2, 0x37, 0xf3 };
                Audio::play_at(Audio::CH_ANY, kSoundTeleport, obj.x.whole, obj.y.whole);
            }
        }

        // &4dad-&4dbb: unless NEWLY_CREATED, advance the beam y-fraction.
        // The original adds 0x20 and wraps at 0xb1 which produces two
        // interlaced sweeps (0x20,0x40,…,0xa0,0x10,0x30,…,0xb0); BBC CRT
        // persistence + the 2-px sprite blend these into one pulse, but on
        // a progressive LCD it reads as a flick every half-cycle. Step by
        // 0x10 and wrap at 0xb0 so the beam visits all 11 positions in
        // monotonic order — same range, same 11-frame cycle, smooth visual.
        if (!(obj.flags & ObjectFlags::NEWLY_CREATED)) {
            uint8_t next = static_cast<uint8_t>(obj.state + 0x10);
            if (next > 0xb0) next = 0x00;
            obj.state = next;
        }
    }
    // Stationary or newly-created: state keeps its current value.

    // &4dbd-&4dc6: the rendered y_fraction is either the state itself
    // (ceiling-mounted base — beam extends downward) or its negation
    // (floor-mounted base — beam extends upward), minus one. The 6502's
    // `BIT y_flip / invert_if_positive` pair negates when y_flip's bit 7 is
    // CLEAR (not flipped), so the sign condition is the opposite of a
    // naive is_flipped_v() test.
    uint8_t beam = obj.state;
    uint8_t y_frac = obj.is_flipped_v() ? beam : static_cast<uint8_t>(-beam);
    obj.y.fraction = static_cast<uint8_t>(y_frac - 1);

    // &4dc8-&4dd1: remote-control hit toggles the transporter lock bits.
    // TODO: port check_if_object_hit_by_remote_control + consider_toggling_lock.

    // &4dd2-&4ddc: palette cycles via rotate_colour_from_A using the
    // *global* frame_counter (not obj's local counter).
    uint8_t idx = (ctx.frame_counter >> 2) & 0x03;
    obj.palette = transporter_palette_table[idx];
}

// &4BAF: Hive update (small and large). Port of update_hive.
//
void update_hive(Object& obj, UpdateContext& ctx) {
    // &4baf-&4bb1: extract bits 6-2 of the data byte as spawn type, cache
    // in obj.state. The 6502 caches it as this_object_state (hive spawn
    // type) so later logic and the spawned creature's target setup can
    // read it cheaply.
    uint8_t spawn_type_id = (obj.tertiary_data_offset >> 2) & 0x1f;
    obj.state = spawn_type_id;

    // &4bb3: consider_absorbing_object_touched — hives absorb any of
    // their own spawn they touch. TODO: full port.

    // &4bb6: minimum energy 0x46 (70).
    NPC::enforce_minimum_energy(obj, 0x46);

    // &4bbb: gate spawn on every-four-frames.
    if (!ctx.every_four_frames) return;

    // &4bbf-&4bc3: bits 1-0 of the data byte mean "inactive"; if set,
    // the hive shouldn't spawn.
    if ((obj.tertiary_data_offset & 0x03) != 0) return;

    // &4bc5-&4bd5: probability depends on how many of this spawn type
    // already exist in primaries — more alive → less likely to spawn.
    //   rnd & rnd & rnd & 7  gives a value biased toward 0.
    //   BCC leave if value < existing_count.
    // We approximate existing_count with a linear scan over primaries.
    int count = 0;
    for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        const Object& p = ctx.mgr.object(i);
        if (p.is_active() &&
            static_cast<uint8_t>(p.type) == spawn_type_id) {
            count++;
        }
    }
    uint8_t roll = ctx.rng.next() & ctx.rng.next() & ctx.rng.next() & 0x07;
    if (roll < count) return;

    // &4bd7-&4bde: don't spawn if a BIG_FISH or any OBJECT_RANGE_FLYING_
    // ENEMIES is already present. TODO: full port (needs find_object and
    // the object-range tables at &3c2a).

    // Range-check the spawn type before we commit.
    if (spawn_type_id >= static_cast<uint8_t>(ObjectType::COUNT)) return;

    // &4be0-&4bf4: create the spawn, emit it left/right depending on
    // the hive's x_flip. In the 6502 this calls create_child_object which
    // also copies the hive's position; create_object_at mirrors that.
    int slot = ctx.mgr.create_object_at(
        static_cast<ObjectType>(spawn_type_id), 4, obj);
    if (slot < 0) return;

    // &4be0-&4be3: hive birth squelch sound, played as soon as the
    // child slot has been allocated successfully.
    static constexpr uint8_t kSoundHiveSpawn[4] = { 0x33, 0xf3, 0x4f, 0x35 };
    Audio::play_at(Audio::CH_ANY, kSoundHiveSpawn, obj.x.whole, obj.y.whole);

    Object& spawn = ctx.mgr.object(slot);
    // angle &80 = leftward, &00 = rightward. Magnitude 0x20 → translate
    // to velocity_x ≈ ±0x20 with no y component (calculate_vector_from_
    // magnitude_and_angle returns a vector whose x-axis component is
    // +magnitude for angle 0 and -magnitude for angle 0x80).
    spawn.velocity_x = obj.is_flipped_h() ? -0x20 : 0x20;
    spawn.velocity_y = 0;

    // Shift the wasp out of the hive's AABB, on the side matching its
    // emerge velocity. create_object_at spawned the child at the hive's
    // exact origin, and the hive is weight-7 — overlaps_solid_object
    // fires on every subsequent X-integration and bounces the wasp's
    // velocity back, so before this call each wasp sat stuck on top of
    // the hive (velocity decaying to near zero via bounce_reflect)
    // until the hive itself demoted. Same pre-compensation the 6502's
    // create_child_object at &33e5-&342d does for bullets; wasps
    // should use it too.
    NPC::offset_child_from_parent(spawn, obj);

    // &4bf9-&4c06: aggressiveness lives in the spawn's state. Flipped
    // hives produce less aggressive spawns (0x20 / 1-in-8 target the
    // player), non-flipped hives produce more aggressive ones (0xa0 /
    // 5-in-8 target the player).
    uint8_t aggressiveness = obj.is_flipped_h() ? 0x20 : 0xa0;
    spawn.state            = aggressiveness;
    // Target the hive so the spawn returns home when wandering.
    // target_and_flags low bits = target slot index.
    int hive_slot = -1;
    for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        if (&ctx.mgr.object(i) == &obj) { hive_slot = i; break; }
    }
    if (hive_slot > 0) {
        spawn.target_and_flags =
            static_cast<uint8_t>((spawn.target_and_flags & 0xe0) | hive_slot);
    }

    // &4c0c-&4c11: more-aggressive spawns get an alternate palette
    // (XOR their palette with 0x3b). Yellow-white wasps and green-cyan
    // piranhas are the visible tells.
    if (aggressiveness & 0x80) {
        spawn.palette = static_cast<uint8_t>(spawn.palette ^ 0x3b);
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

// &4E37 sucking_nests_trigger_table — object type that activates each
// variant. 0xff means "activates on any object" (bit 7 → always active).
// The nest variant is stored in the tertiary data byte.
static constexpr uint8_t sucking_nests_trigger[9] = {
    0xff, // 0: all
    0x3e, // 1: HORIZONTAL_STONE_DOOR
    0x11, // 2: WASP
    0x55, // 3: CORONIUM_BOULDER (also YELLOW_SLIME via &3c2a secondary)
    0x10, // 4: PIRANHA
    0xff, // 5: all
    0x55, // 6: CORONIUM_BOULDER
    0x10, // 7: PIRANHA
    0x0f, // 8: WORM
};

// &4E40 sucking_nests_power_table — suction radius / force, in &20
// fractions (8 per tile). This is both the `acceleration_power` argument
// passed to `accelerate_all_objects` and the per-candidate LOS cap
// inside it (&344a `check_for_obstruction_between_objects_A`).
static constexpr uint8_t sucking_nests_power[9] = {
    0x50, // 0: 10 tiles
    0x30, // 1:  6 tiles
    0x7f, // 2: ~16 tiles
    0x40, // 3:  8 tiles
    0x50, // 4: 10 tiles
    0x7f, // 5: ~16 tiles
    0x7f, // 6: ~16 tiles
    0x50, // 7: 10 tiles
    0x40, // 8:  8 tiles
};

// &4E49 sucking_nests_palette_direction_table — top 7 bits are palette
// (shifted down into obj.palette); bit 0 is direction (1 = attract,
// 0 = repel).
static constexpr uint8_t sucking_nests_direction[9] = {
    0x5f, 0xac, 0xbf, 0x3d, 0xf9, 0x58, 0xa2, 0xd8, 0x4b,
};

// &4DED update_sucking_nest — port of &4ded-&4e34.
//
// Every 16 frames, `find_object` (6502 &3c2a) scans for an object of
// the nest's trigger type; if one is found (or trigger == 0xff = "all")
// the nest enters "active" state. While active, `accelerate_all_objects`
// applies variant-dependent suction — each candidate is LOS-raycast out
// to `power` &20-fractions, and the effective acceleration is
// `power - (weight*2 + 8 + distance)`, so heavy or far objects are
// skipped automatically.
//
// Simplifications vs the 6502:
//   * The coronium-boulder → secondary-type-yellow-slime special case at
//     &4dfd-&4e01 isn't wired up. Variants 3 and 6 still activate on
//     coronium boulders directly; they just don't also activate on
//     yellow slime.
//   * `find_object`'s random per-attempt probability gate at &3c99
//     isn't ported — we accept any matching candidate. In practice
//     activation is still correct because the 6502 also accepts any
//     matching candidate eventually; we just converge on frame 1 rather
//     than over ~dozen 16-frame ticks.
//   * The random-flip (&4e1f STA this_object_x_flip) and 1/256 high-
//     damage roll (&4e23 CMP #&50) are preserved faithfully.
void update_sucking_nest(Object& obj, UpdateContext& ctx) {
    NPC::enforce_minimum_energy(obj, 0x7f);

    // Variant from tertiary data byte (our port's this_object_data
    // equivalent). Clamp to table size so unwired variants don't crash.
    uint8_t variant = obj.tertiary_data_offset & 0x0f;
    if (variant >= 9) variant = 0;

    // &4ded: set palette from top 7 bits of direction byte.
    obj.palette = static_cast<uint8_t>(sucking_nests_direction[variant] >> 1);

    // &4df3-&4e09: detection. Every 16 frames scan for a target of the
    // trigger type; 0xff trigger means "always active". Active state
    // persists between detection ticks via Object::state.
    if (ctx.every_sixteen_frames) {
        uint8_t trigger = sucking_nests_trigger[variant];
        bool active = false;
        if (trigger == 0xff) {
            active = true;
        } else {
            for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
                const Object& other = ctx.mgr.object(i);
                if (!other.is_active()) continue;
                if (static_cast<uint8_t>(other.type) != trigger) continue;
                // 6502 find_object carry-clear LOS: randomised cap per
                // call (see has_line_of_sight_randomized). One slot from
                // self to candidate here stands in for find_object's
                // slot loop — close enough for activation gating.
                if (NPC::has_line_of_sight_randomized(
                        obj, static_cast<uint8_t>(i), ctx)) {
                    active = true;
                    break;
                }
            }
        }
        obj.state = active ? 0x80 : 0x00;
    }

    bool active = (obj.state & 0x80) != 0;

    // &4e0f-&4e1c: accelerate_all_objects. Iterate every primary
    // (skip self), LOS-raycast out to `power`, compute weight/distance-
    // attenuated acceleration, nudge velocity toward (attract) or away
    // (repel) from the nest.
    uint8_t damage_amount = 2;
    if (active) {
        uint8_t power = sucking_nests_power[variant];
        bool attract = (sucking_nests_direction[variant] & 0x01) != 0;
        // power/8 converts &20 fractions to whole-tile cap for our
        // has_line_of_sight (which takes tiles, not fractions). Matches
        // the 6502's `&344a JSR check_for_obstruction_between_objects_A`
        // where A = acceleration_power (in &20 fractions).
        uint8_t max_tiles = static_cast<uint8_t>(power / 8);
        for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            Object& other = ctx.mgr.object(i);
            if (!other.is_active()) continue;
            int8_t dx = static_cast<int8_t>(obj.x.whole - other.x.whole);
            int8_t dy = static_cast<int8_t>(obj.y.whole - other.y.whole);
            uint8_t adx = static_cast<uint8_t>(std::abs(dx));
            uint8_t ady = static_cast<uint8_t>(std::abs(dy));
            uint8_t dist_tiles = adx > ady ? adx : ady;
            if (dist_tiles > max_tiles) continue;
            if (!NPC::has_line_of_sight(obj, static_cast<uint8_t>(i),
                                        max_tiles, ctx)) continue;

            // &3461-&3476: acceleration = power - (weight*2 + 8 + distance).
            // Distance is in &20 fractions, so convert our tile count
            // back. Weight of 7 means "static" (the 6502 sets its static
            // bit at &346b and skips the velocity write at &348c).
            uint8_t w = other.weight();
            bool is_static = (w == 7);
            int dist_fracs = static_cast<int>(dist_tiles) * 8;
            int eff = static_cast<int>(power) - (w * 2 + 8 + dist_fracs);
            if (eff <= 0) continue;
            if (is_static) continue;

            // Velocity nudge toward / away from nest. `dx > 0` means the
            // nest is east of the candidate → attract pulls the
            // candidate east (velocity_x++). Repel flips the sign.
            int8_t step_x = 0, step_y = 0;
            if (dx > 0) step_x = attract ?  1 : -1;
            if (dx < 0) step_x = attract ? -1 :  1;
            if (dy > 0) step_y = attract ?  1 : -1;
            if (dy < 0) step_y = attract ? -1 :  1;
            if (step_x > 0 && other.velocity_x <  4) other.velocity_x++;
            if (step_x < 0 && other.velocity_x > -4) other.velocity_x--;
            if (step_y > 0 && other.velocity_y <  4) other.velocity_y++;
            if (step_y < 0 && other.velocity_y > -4) other.velocity_y--;
        }

        // &4e1f-&4e25: random h-flip per frame + 1-in-256 chance of
        // dealing 80 damage instead of 2.
        uint8_t r = ctx.rng.next();
        if (r & 0x80) obj.flags |=  ObjectFlags::FLIP_HORIZONTAL;
        else           obj.flags &= ~ObjectFlags::FLIP_HORIZONTAL;
        if (r == 0x50) damage_amount = 80;
    }

    // &4e29-&4e34: damage the touched object (2 normally, 80 on the
    // rare roll above). remove_object approximates the full damage
    // chain — a cannonball-strength hit is enough to kill most things
    // and lighter victims end up deleted by the nest's touch regardless.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS && obj.touching != 0) {
        Object& victim = ctx.mgr.object(obj.touching);
        if (victim.energy > damage_amount) {
            victim.energy = static_cast<uint8_t>(victim.energy - damage_amount);
        } else {
            ctx.mgr.remove_object(obj.touching);
        }
        // &4e2d-&4e30: nest digestion crunch when something gets
        // absorbed.
        static constexpr uint8_t kSoundNestEat[4] = { 0x57, 0x07, 0x57, 0x97 };
        Audio::play_at(Audio::CH_ANY, kSoundNestEat, obj.x.whole, obj.y.whole);
    }
}

// &4BA9: Bush - grows back, can be destroyed
void update_bush(Object& obj, UpdateContext& ctx) {
    // Bushes slowly regenerate
    if (ctx.every_sixty_four_frames) {
        if (obj.energy < 0xff) obj.energy++;
    }
}

// &40EE update_cannon. The 6502 cannon has no internal timer: it fires a
// cannonball iff the player has just fired a CANNON_CONTROL_DEVICE within
// range and pointed at it (check_if_object_hit_by_other_control at &0bc7,
// invoked with A=&4f). Without that gate the cannon never fires.
void update_cannon(Object& obj, UpdateContext& ctx) {
    // &40ee-&40f3 check_if_object_hit_by_other_control(CANNON_CONTROL_DEVICE).
    // The full 6502 test is three-part: (a) fired object is type 0x4f,
    // (b) within ~3 tiles with clear LOS, (c) player aim angle points
    // within a narrow cone at the cannon. We approximate (b) with
    // Chebyshev distance and drop (c), matching the door RCD path above.
    bool triggered = false;
    if (ctx.player_object_fired < GameConstants::PRIMARY_OBJECT_SLOTS) {
        const Object& fired = ctx.mgr.object(ctx.player_object_fired);
        if (fired.is_active() &&
            fired.type == ObjectType::CANNON_CONTROL_DEVICE) {
            int8_t dx = static_cast<int8_t>(fired.x.whole - obj.x.whole);
            int8_t dy = static_cast<int8_t>(fired.y.whole - obj.y.whole);
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            if (adx <= 3 && ady <= 3) triggered = true;
        }
    }

    if (triggered) {
        // &40f5-&40f9 create_projectile_with_zero_velocity_y(CANNONBALL,
        // x velocity = 0x40). The fire_projectile helper inverts vx for a
        // left-facing parent via this_object_x_flip, matching &33ad.
        int slot = NPC::fire_projectile(obj, ObjectType::CANNONBALL, ctx);
        if (slot >= 0) {
            Object& ball = ctx.mgr.object(slot);
            ball.velocity_x = obj.is_flipped_h() ? -0x40 : 0x40;
            ball.velocity_y = 0;
            NPC::offset_child_from_parent(ball, obj);
        }
    }
    // &40fc-&40fe consider_flipping_object_to_match_velocity_x(0x0f) — 1-in-16
    // chance per frame of snapping facing to the cannon's drift direction.
    // Skipped here: the cannon is mounted (no x velocity) so the flip would
    // never trigger anyway.
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
        // &4c61-&4c64 play_sound_on_channel_zero (priority): engine
        // fire roar. Channel 0 is the SEC entry point so this always
        // plays even if the regular pool is busy.
        static constexpr uint8_t kSoundEngineFire[4] = { 0x70, 0xc2, 0x6e, 0xa3 };
        Audio::play_at(Audio::CH_PRIORITY, kSoundEngineFire, obj.x.whole, obj.y.whole);
    }

    // &4c68-&4c80: palette rwY + random x_fraction in [0x90, 0xcf].
    obj.palette = 0x34;
    uint8_t fc = ctx.frame_counter;
    // ROL x4 then ADC frame_counter (simulate the original's bit rotation).
    uint8_t rot = static_cast<uint8_t>((fc << 4) | (fc >> 4));
    uint8_t xf  = static_cast<uint8_t>(rot + fc) & 0x3f;
    obj.x.fraction = static_cast<uint8_t>(xf + 0x90);
}

// &4B64: Placeholder — invisible proxy for tertiary objects spawned from
// TILE_SPACE_WITH_OBJECT_FROM_DATA (tile type 0x02). The data byte's low 7
// bits hold the ACTUAL object type (rolling robot, inactive chatter, boulder,
// collectable, …). The placeholder sits still (INTANGIBLE flag blocks
// gravity, update zeroes any velocity that snuck in) until the player gets
// a clear line of sight or physically touches it — at which point the
// placeholder's type is overwritten with the real type and it takes over.
//
// Port of &4B64 update_placeholder_object + &4B7F convert_placeholder_object.
// The "obstruction-free line of sight within 0x80 range" check is approximated
// with a simple Chebyshev-distance test for now (no raycast through tiles).
void update_placeholder(Object& obj, UpdateContext& ctx) {
    // Keep the placeholder pinned — zero velocity every frame so an errant
    // stream from physics / wind / water can't drift it off its tile.
    obj.velocity_x = 0;
    obj.velocity_y = 0;

    // Convert on physical contact (touching != 0x80 means something's there).
    bool touched = obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS;

    // Or when the anchor (normally the player, optionally the camera in
    // map-scroll mode) is close enough. The 6502 at &359a uses
    // `A = &80 (128 &20 fractions = 16 tiles)` for this exact test, so
    // the box needs to be ~16 tiles wide — the viewport is ~20 wide, so
    // anything less leaves the edges of the screen uncovered and
    // placeholders sitting in plain sight refuse to convert.
    uint8_t anchor_x = ctx.mgr.activation_anchor_x();
    uint8_t anchor_y = ctx.mgr.activation_anchor_y();
    int8_t dx = static_cast<int8_t>(anchor_x - obj.x.whole);
    int8_t dy = static_cast<int8_t>(anchor_y - obj.y.whole);
    uint8_t adx = static_cast<uint8_t>(dx < 0 ? -dx : dx);
    uint8_t ady = static_cast<uint8_t>(dy < 0 ? -dy : dy);
    bool visible = (adx <= 16 && ady <= 16);

    if (!touched && !visible) return;

    // Pull the real object type out of the tertiary data byte that
    // spawn_tertiary_object copied into obj.tertiary_data_offset (with the
    // spawn bit already stripped).
    uint8_t real_type = obj.tertiary_data_offset & 0x7f;
    if (real_type == 0 ||
        real_type >= static_cast<uint8_t>(ObjectType::COUNT)) {
        return;  // Nothing to convert to; stay a placeholder.
    }
    obj.type = static_cast<ObjectType>(real_type);
    obj.sprite = object_types_sprite[real_type];
    obj.palette = object_types_palette_and_pickup[real_type] & 0x7f;
    obj.energy = 0xff;  // matches &4b83 LDA #&ff / STA energy
}

// Debug-only exports for the "Wiring" overlay. Re-walks the same
// switch_effects_table group scan process_switch_effects does, but only
// records the tertiary_data_offset targets without mutating state.
int switch_effect_targets(uint8_t effect_id, uint8_t* out, int max_out) {
    const int N = static_cast<int>(sizeof(switch_effects_table));
    int zeros_seen = 0;
    const int required = static_cast<int>(effect_id) + 1;
    int written = 0;
    for (int idx = 0; idx < N && written < max_out; ++idx) {
        uint8_t b = switch_effects_table[idx];
        if (b == 0) {
            ++zeros_seen;
            if (zeros_seen > required) break;
            continue;
        }
        if (zeros_seen != required) continue;
        out[written++] = b;
    }
    return written;
}

bool transporter_destination(uint8_t destination,
                             uint8_t& out_x, uint8_t& out_y) {
    if (destination >= 16) return false;
    out_x = transporter_destinations_x[destination];
    out_y = transporter_destinations_y[destination];
    return true;
}

} // namespace Behaviors
