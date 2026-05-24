#include "behaviours/environment.h"
#include "behaviours/path.h"
#include "behaviours/npc_helpers.h"
#include "audio/audio.h"
#include "core/types.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include "objects/object_manager.h"
#include "world/landscape.h"
#include "world/tertiary.h"
#include "particles/particle_system.h"
#include <algorithm>
#include <cstdlib>

namespace Behaviors {

// Port of &0bc5/&0bc7 check_if_object_hit_by_other_control. Returns
// true when the player has just fired an object of type `control_type`
// AND that fired object is inside an aim cone centred on the player's
// aim direction passing through `target`.
//
// 6502 pipeline at &0bd2-&0be5:
//   &0bd2 LDA #&18 (3 tiles in &20-fractions) — range gate via &359c
//   &0bd9 JSR &22a0 — angle, computed via
//                    &2305 SBC: relative = other - this
//                    (this = target, other = fired), so `angle` is the
//                    direction FROM target TO fired.
//   &0bdc SBC player_aim — A = angle - player_aim
//   &0bde SBC #&80       — A = angle - player_aim - 0x80
//   &0be0 invert_if_negative — abs
//   &0be3 ADC distance        — distance in &20-fractions (8 per tile)
//   &0be5 CMP #&18            — pass if A < 0x18
//
// Direction-of-angle gotcha: despite the function name "of object X to
// this_object", &22fe builds `relative = other - this`, so the angle
// points from the target toward the fired object. For an on-axis hit
// with player aiming right (aim = 0), the RCD must be APPROACHING the
// target from the player's side — i.e. at smaller x than the target,
// so the target→fired vector points right (angle 0x80, "left" in 6502
// space). raw_diff = 0x80 - 0 - 0x80 = 0 → passes. Once the RCD
// overshoots past the target the vector flips and the test rejects.
static bool hit_by_aim_cone(const Object& target, UpdateContext& ctx,
                            ObjectType control_type) {
    if (ctx.player_object_fired >= GameConstants::PRIMARY_OBJECT_SLOTS) {
        return false;
    }
    const Object& fired = ctx.mgr.object(ctx.player_object_fired);
    if (!fired.is_active() || fired.type != control_type) return false;

    // Chebyshev range gate at 3 tiles. The 6502 raycast at &359c also
    // returns the distance in &83 in &20-fractions — we approximate
    // with the tile count × 8 below.
    int8_t fdx = static_cast<int8_t>(fired.x.whole - target.x.whole);
    int8_t fdy = static_cast<int8_t>(fired.y.whole - target.y.whole);
    int adx = fdx < 0 ? -fdx : fdx;
    int ady = fdy < 0 ? -fdy : fdy;
    int tiles = std::max(adx, ady);
    if (tiles > 3) return false;
    int distance_units = tiles * 8;     // 1 tile = 8 &20-fracs

    // Direction TARGET → FIRED (see the &2305 SBC trace above). dx/dy
    // are the same as the Chebyshev fdx/fdy above; pass them straight
    // to angle_from_deltas.
    uint8_t angle = NPC::angle_from_deltas(fdx, fdy);

    // &0bdc-&0be0: abs(angle - player_aim - 0x80).
    int raw_diff = static_cast<int8_t>(
        angle - ctx.player_aim_angle - 0x80);
    int abs_diff = raw_diff < 0 ? -raw_diff : raw_diff;

    return (abs_diff + distance_units) < 0x18;
}

// &4D72-&4D7C: per-colour-pair tables.
// speed table — how fast the door opens (halved when closing).
static constexpr uint8_t doors_speed_table[4]  = { 0x20, 0x10, 0x08, 0x20 };
// energy threshold below which the door is "being destroyed".
static constexpr uint8_t doors_energy_table[4] = { 0x80, 0x74, 0xc0, 0x80 };
// &4d79-&4d80 door palette table (low 4 bits AND'd if unlocked).
// Entries 0-3 must NOT duplicate into 4-7 — stone doors use colour 4 (rmB).
static constexpr uint8_t doors_palette_table[8] = {
    0x2b, // 0: cyG (metal)
    0x2d, // 1: ryG (metal)
    0x15, // 2: gyR (metal)
    0x1c, // 3: ywR (metal)
    0x42, // 4: rmB (stone)
    0x12, // 5: rmR (metal)
    0x26, // 6: bcG
    0x4e, // 7: mwB
};

namespace DoorFlag {
    constexpr uint8_t LOCKED            = 0x01;
    constexpr uint8_t OPENING           = 0x02;
    constexpr uint8_t MOVING            = 0x04;
    constexpr uint8_t SLOW_OR_DESTROYED = 0x08;
}

// &4C83-&4D71 update_door. tx=open fraction (0=closed, 0xff=open),
// state=per-object auto-close timer (6502 shares one global &0819),
// ty bit 1 = orientation, tertiary_data_offset = data byte
// (LOCKED/OPENING/MOVING/SLOW_OR_DESTROYED + colour).
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

    // &4c9e/&31ac RCD door-unlock: the door reacts iff the player just
    // fired an RCD that's inside the aim-cone test at &0bc5 AND the
    // matching key has been collected. player_object_fired is one-frame
    // (cleared at end of tick by game.cpp), so the LOCKED toggle only
    // fires on the firing frame, not repeatedly while the RCD coasts.
    if (ctx.player_keys_collected &&
        hit_by_aim_cone(obj, ctx, ObjectType::REMOTE_CONTROL_DEVICE)) {
        uint8_t door_colour = (obj.tertiary_data_offset >> 4) & 0x07;
        if (ctx.player_keys_collected[door_colour] & 0x80) {
            data ^= DoorFlag::LOCKED;
            if (data & DoorFlag::LOCKED) {
                data &= ~DoorFlag::MOVING;
            } else {
                data |= DoorFlag::OPENING;
            }
            // &31d0-&31d3 lock/unlock chime — same params for both
            // directions in the 6502.
            static constexpr uint8_t kSoundLock[4] = { 0x94, 0x64, 0xba, 0xc4 };
            Audio::play_at(Audio::CH_ANY, kSoundLock, obj.x.whole, obj.y.whole);
        }
    }

    bool opening = (data & DoorFlag::OPENING) != 0;
    bool locked  = (data & DoorFlag::LOCKED)  != 0;
    bool slow    = (data & DoorFlag::SLOW_OR_DESTROYED) != 0;
    uint8_t colour      = (data >> 4) & 0x07;
    uint8_t colour_pair = colour & 0x03;

    // &4cbe-&4cd5: energy refill above colour threshold, else slow->destroy
    // ladder. Door re-spawns from tertiary entry once explosion slot is reaped.
    if (obj.energy >= doors_energy_table[colour_pair]) {
        obj.energy = 0xff;
    } else if (slow) {
        obj.energy = 0;
    } else {
        data |= DoorFlag::SLOW_OR_DESTROYED;
        slow = true;
    }

    // &4cd7-&4cec: door speed. slow=1, opening=table, closing=halved &
    // negated, closing+touching=-1.
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

    // Mirror live data to tertiary store; bit 7 (spawn gate) MUST stay
    // clear while primary owns the slot. &4d5f-&4d61 writes without bit 7;
    // return_to_tertiary re-applies it on demote.
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

// Find every cell sharing a 6502 data_offset so toggles fan out across
// siblings. Option B bake gives each cell its own entry; the 6502 shared
// one byte per (tile_type, X). Writes up to max_out indices.
static int find_shared_entries_for_data_offset(const Landscape& landscape,
                                                uint8_t data_offset,
                                                uint16_t* out, int max_out,
                                                ObjectManager* diag_mgr) {
    if (data_offset == 0 || max_out <= 0) return 0;
    int written = 0;
    // `range_idx` selects the per-range data-offset adjustment, not
    // the world tile type. We then match cells by their per-cell
    // tertiary_source_idx (set by bake) rather than raw tile type —
    // door / switch overlays often sit over INVISIBLE_SWITCH base tiles.
    for (int range_idx = 0; range_idx <= 8; ++range_idx) {
        uint8_t source_idx = static_cast<uint8_t>(
            data_offset - tertiary_data_offset[range_idx]);
        if (source_idx <  tertiary_ranges[range_idx]) continue;
        if (source_idx >= tertiary_ranges[range_idx + 1]) continue;
        uint8_t target_x = tertiary_objects_x_data[source_idx];
        if (diag_mgr) diag_mgr->log_diag(
            "diag find_shared data_off=0x%02x range=%d src_idx=0x%02x "
            "tgt_x=0x%02x",
            data_offset, range_idx, source_idx, target_x);
        for (int y = 0; y < 256; ++y) {
            if (landscape.tertiary_source_idx_at(
                    target_x, static_cast<uint8_t>(y)) != source_idx)
                continue;
            uint16_t cell_idx = landscape.tertiary_index_at(
                target_x, static_cast<uint8_t>(y));
            if (cell_idx == Landscape::NO_TERTIARY) continue;
            if (diag_mgr) diag_mgr->log_diag(
                "diag   cell (%d,%d) entry=%u", target_x, y, cell_idx);
            if (written >= max_out) return written;
            out[written++] = cell_idx;
        }
        // Each data_offset maps to exactly one (range, source_idx) tuple;
        // no need to keep checking other ranges.
        return written;
    }
    if (diag_mgr) diag_mgr->log_diag(
        "diag find_shared data_off=0x%02x NO MATCHING RANGE", data_offset);
    return written;
}

// &49db process_switch_effects. Fans toggles across cells sharing a
// data_offset (Option B vs 6502 shared bytes). Reads through to the
// primary's live data byte when one owns the slot; tertiary entry is
// stale until demote.
static void process_switch_effects(ObjectManager& mgr,
                                    const Landscape& landscape,
                                    uint8_t effect_id,
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

        // Resolve target data_offset -> list of per-cell entry indices
        // sharing that source row. 16 fits the worst-case "many cells of
        // the same raw type at one X" without overflow on Option-B
        // worlds.
        uint16_t entries[16];
        int n_entries = find_shared_entries_for_data_offset(
            landscape, b, entries, 16, &mgr);
        mgr.log_diag(
            "diag process_switch eff=%u byte=0x%02x mask=0x%02x "
            "toggle=0x%02x n_entries=%d",
            effect_id, b, mask, toggle, n_entries);
        if (n_entries == 0) continue;

        // Prefer a primary's live byte from any sibling entry (siblings
        // should share state, but live-vs-stale can drift).
        int sample_owner = -1;
        for (int e = 0; e < n_entries && sample_owner < 0; ++e) {
            uint16_t slot = entries[e];
            for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; ++i) {
                const Object& p = mgr.object(i);
                if (p.is_active() && p.tertiary_slot == slot) {
                    sample_owner = i;
                    break;
                }
            }
        }
        uint8_t prev = (sample_owner >= 0)
            ? mgr.object(sample_owner).tertiary_data_offset
            : static_cast<uint8_t>(
                mgr.tertiary_data_byte(entries[0]) & 0x7f);
        uint8_t newv = static_cast<uint8_t>((prev & mask) ^ toggle);
        mgr.log_diag(
            "diag   prev=0x%02x newv=0x%02x sample_owner=%d",
            prev, newv, sample_owner);

        // Apply newv to every sibling entry.
        for (int e = 0; e < n_entries; ++e) {
            uint16_t slot = entries[e];
            bool live = false;
            int live_obj = -1;
            for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; ++i) {
                Object& p = mgr.object(i);
                if (p.is_active() && p.tertiary_slot == slot) {
                    p.tertiary_data_offset = newv;
                    live = true;
                    live_obj = i;
                }
            }
            mgr.log_diag(
                "diag   apply entry=%u live=%d obj=%d",
                slot, (int)live, live_obj);
            if (live) {
                mgr.set_tertiary_data_byte(slot, newv);
            } else {
                // Port-only: force bit 7 so the next tile-plot spawns the
                // primary. Our renderer ties door visual state to its primary;
                // demote (&1d20-&1d24 ORA #&80) re-arms it anyway.
                mgr.set_tertiary_data_byte(slot,
                    static_cast<uint8_t>(newv | 0x80));
            }
        }
    }
}

// &49C5 check_if_object_can_trigger_switches: heavy enough (weight >= 2)
// AND not on the per-type blacklist. The 6502 branch table at &49cc-&49d4
// allows MAGGOT and higher via BCS, takes a special path for INVISIBLE_
// DEBRIS, and only excludes the clawed-robot block (0x22-0x26) plus
// INVISIBLE_DEBRIS (0x35). Maggots DO trigger switches in the original.
static bool object_can_trigger_switches(const Object& obj) {
    uint8_t w = obj.weight();
    if (w < 2) return false;
    uint8_t t = static_cast<uint8_t>(obj.type);
    return t != static_cast<uint8_t>(ObjectType::INVISIBLE_DEBRIS) &&
           t != static_cast<uint8_t>(ObjectType::MAGENTA_CLAWED_ROBOT) &&
           t != static_cast<uint8_t>(ObjectType::CYAN_CLAWED_ROBOT) &&
           t != static_cast<uint8_t>(ObjectType::GREEN_CLAWED_ROBOT) &&
           t != static_cast<uint8_t>(ObjectType::RED_CLAWED_ROBOT) &&
           t != static_cast<uint8_t>(ObjectType::TRIAX);
}

void trigger_invisible_switch_at(Object& toucher,
                                 uint8_t tile_x, uint8_t tile_y,
                                 ObjectManager& mgr,
                                 const Landscape& landscape) {
    ResolvedTile r = resolve_tile_with_tertiary(landscape, tile_x, tile_y);
    // Dispatch on RESOLVED tile type, not raw landscape byte (6502
    // &1761/&1772 stores resolved into &08, &177c indexes routines).
    // Redirect switches (e.g. opener &89 in STONE_DOOR range with
    // tile_and_flip=&00) only fire via the resolved path.
    uint8_t resolved_type = r.tile_and_flip & TileFlip::TYPE_MASK;
    if (resolved_type != static_cast<uint8_t>(TileType::INVISIBLE_SWITCH))
        return;
    if (r.data_offset <= 0) return;

    // &3ef4-&3efb: tertiary type 0x80+ means "any object can trigger"; else
    // the type byte must equal the toucher's object type for the switch to
    // fire (e.g. the (&7f, &77) closer at idx &ae is type &4c so only an
    // OBJECT_EMPTY_FLASK can trip it).
    uint8_t type_byte = mgr.tertiary_type_byte(r.type_offset);
    if (!(type_byte & 0x80) &&
        type_byte != static_cast<uint8_t>(toucher.type)) return;

    // &3eff-&3f02 check_if_object_can_trigger_switches.
    if (!object_can_trigger_switches(toucher)) return;

    // &3f06-&3f15 decode switch data byte. Keep bit 7 — it's part of the
    // packed effect id for INVISIBLE_SWITCH (see tertiary_spawn.cpp:36-46).
    uint8_t data = mgr.tertiary_data_byte(r.data_offset);
    uint8_t mask = static_cast<uint8_t>(((data >> 1) | 0xfc) ^ 0x03);
    bool set_path = (data & 0x01) != 0;
    uint8_t effect_byte = set_path
        ? data
        : static_cast<uint8_t>(data & 0xf8);
    uint8_t toggle    = static_cast<uint8_t>((effect_byte >> 1) & 0x03);
    uint8_t effect_id = static_cast<uint8_t>(effect_byte >> 3);

    process_switch_effects(mgr, landscape, effect_id, mask, toggle);
}

// &499D-&49C2 update_switch. tx is an 8-frame ROR press-history; fire
// only on bit-7-set / bits-0-6-clear (leading edge, 7-frame auto-repeat
// suppression). Toggles bit 0 of data and runs process_switch_effects.
void update_switch(Object& obj, UpdateContext& ctx) {
    // Carry-in = "triggered this frame". Touching with a trigger-capable
    // object sets it; check_if_object_can_trigger_switches filters out
    // very light objects, invisible debris, clawed robots, Triax, maggots.
    bool triggered = false;
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        triggered = object_can_trigger_switches(
            ctx.mgr.object(obj.touching));
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
        ctx.mgr.log_diag(
            "diag update_switch press at (%d,%d) slot=%u data=0x%02x "
            "effect=%u toggle=%u",
            obj.x.whole, obj.y.whole, obj.tertiary_slot, data,
            effect, toggle);
        // Mirror the change into the tertiary slot too.
        if (obj.tertiary_slot > 0) {
            ctx.mgr.set_tertiary_data_byte(obj.tertiary_slot, data);
        }
        process_switch_effects(ctx.mgr, ctx.landscape,
                                effect, /*mask=*/0xff, toggle);
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

// &4D86-&4DDE update_transporter_beam. data bit 0 = stationary, bits 1-4 =
// destination index; state = beam y-fraction sweeping 0x00..0xb0 step 0x20.
// On contact, latch tx/ty to destination and copy our velocity into target.
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
                // &4daa play_sound_for_teleporting (&440d -> JSR
                // play_sound, params 29 c2 37 f3).
                static constexpr uint8_t kSoundTeleport[4] = { 0x29, 0xc2, 0x37, 0xf3 };
                Audio::play_at(Audio::CH_ANY, kSoundTeleport, obj.x.whole, obj.y.whole);
                // Clear touching so this frame can't re-fire on the
                // just-launched target before the slot-loop OR #&80.
                obj.touching = 0x80;
            }
        }

        // &4dad-&4dbb advance beam y-fraction. Port-only: step 0x10
        // (not 6502's interlaced 0x20-wrap-at-0xb1) so non-CRT LCDs see
        // a monotonic sweep instead of half-cycle flicker.
        if (!(obj.flags & ObjectFlags::NEWLY_CREATED)) {
            uint8_t next = static_cast<uint8_t>(obj.state + 0x10);
            if (next > 0xb0) next = 0x00;
            obj.state = next;
        }
    }
    // Stationary or newly-created: state keeps its current value.

    // &4dbd-&4dc6 rendered y_fraction. 6502 BIT y_flip / invert_if_positive
    // negates when y_flip bit 7 is CLEAR — sign condition is reversed
    // vs a naive is_flipped_v() test.
    uint8_t beam = obj.state;
    uint8_t y_frac = obj.is_flipped_v() ? beam : static_cast<uint8_t>(-beam);
    obj.y.fraction = static_cast<uint8_t>(y_frac - 1);

    // &4dc8-&4dd1 / &31ac transporter path: RCD hit toggles the
    // stationary bit when the matching key is collected. Key index is
    // (data + 0x60) >> 5 → 3..6 for the four transporter colours (the
    // 6502 packs the key into data bits 5-6). 3-tile Chebyshev range
    // matches the door RCD port above.
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
                uint8_t key_idx =
                    static_cast<uint8_t>((data + 0x60) >> 5);
                if (key_idx < 8 &&
                    (ctx.player_keys_collected[key_idx] & 0x80)) {
                    data ^= 0x01;  // &31c2 toggle stationary bit
                    obj.tertiary_data_offset = data;
                    if (obj.tertiary_slot > 0) {
                        ctx.mgr.set_tertiary_data_byte(
                            obj.tertiary_slot,
                            static_cast<uint8_t>(data & 0x7f));
                    }
                    static constexpr uint8_t kSoundLock[4] =
                        { 0x94, 0x64, 0xba, 0xc4 };
                    Audio::play_at(Audio::CH_ANY, kSoundLock,
                                   obj.x.whole, obj.y.whole);
                }
            }
        }
    }

    // &4dd2-&4ddc: palette cycles via rotate_colour_from_A using the
    // *global* frame_counter (not obj's local counter).
    uint8_t idx = (ctx.frame_counter >> 2) & 0x03;
    obj.palette = transporter_palette_table[idx];
}

// &4BAF: Hive update (small and large). Port of update_hive.
//
void update_hive(Object& obj, UpdateContext& ctx) {
    // &4baf-&4bb1 cache spawn type in obj.state (this_object_state) so
    // child target setup can read it cheaply.
    uint8_t spawn_type_id = (obj.tertiary_data_offset >> 2) & 0x1f;
    obj.state = spawn_type_id;

    // &4bb3 / &3be1 consider_absorbing_object_touched: hives absorb
    // their own spawn type on contact, BUT only on the hive's back side.
    // The 6502 gates with check_object_touching_angle (&3bd5):
    //   angle = angle_of_other_to_this; A = angle + &40; A EOR x_flip;
    //   bit 7 set -> not viable, leave (don't absorb).
    // Without this gate the just-spawned wasp's frame-1 self-touch makes
    // the hive eat it before it can move clear.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS &&
        obj.touching != 0) {
        Object& touched = ctx.mgr.object(obj.touching);
        if (touched.is_active() &&
            static_cast<uint8_t>(touched.type) == spawn_type_id) {
            // &3bd5 check_object_touching_angle. The 6502 uses sprite
            // centres; we use the high byte of the (a - b) delta across
            // (whole, fraction) so that two objects in the same tile
            // still get a meaningful direction. Saturating clamp keeps
            // dx/dy in int8_t range — the absorb decision only needs the
            // half-plane (front vs back), not a precise angle.
            int dx_abs = (static_cast<int>(obj.x.whole) * 256 +
                          static_cast<int>(obj.x.fraction)) -
                         (static_cast<int>(touched.x.whole) * 256 +
                          static_cast<int>(touched.x.fraction));
            int dy_abs = (static_cast<int>(obj.y.whole) * 256 +
                          static_cast<int>(obj.y.fraction)) -
                         (static_cast<int>(touched.y.whole) * 256 +
                          static_cast<int>(touched.y.fraction));
            if (dx_abs >  127) dx_abs =  127;
            if (dx_abs < -128) dx_abs = -128;
            if (dy_abs >  127) dy_abs =  127;
            if (dy_abs < -128) dy_abs = -128;
            uint8_t angle =
                NPC::angle_from_deltas(static_cast<int8_t>(dx_abs),
                                       static_cast<int8_t>(dy_abs));
            uint8_t a = static_cast<uint8_t>(angle + 0x40);
            uint8_t x_flip = obj.is_flipped_h() ? 0x80 : 0x00;
            bool viable = ((a ^ x_flip) & 0x80) == 0;

            if (viable) {
                ctx.mgr.remove_object(obj.touching);
                // &3bf2 play_low_beep — 4-byte sound block at &14b0.
                static constexpr uint8_t kSoundLowBeep[4] =
                    { 0x5d, 0x04, 0xff, 0x05 };
                Audio::play_at(Audio::CH_ANY, kSoundLowBeep,
                               obj.x.whole, obj.y.whole);
            }
        }
    }

    // &4bb6: minimum energy 0x46 (70).
    NPC::enforce_minimum_energy(obj, 0x46);

    // &4bbb: gate spawn on every-four-frames.
    if (!ctx.every_four_frames) return;

    // &4bbf-&4bc3: bits 1-0 of the data byte mean "inactive"; if set,
    // the hive shouldn't spawn.
    if ((obj.tertiary_data_offset & 0x03) != 0) return;

    // &4bc5-&4bd5 spawn prob ∝ 1/existing_count via (rnd&rnd&rnd&7) gate.
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

    // &4bd7-&4bde find_object A=0x0e Y=0x86: skip spawning if any
    // BIG_FISH or any OBJECT_RANGE_FLYING_ENEMIES (range 6) primary is
    // present. Cheaper than the full find_object — we only care if any
    // matches, not which is nearest.
    constexpr int kFlyingEnemiesRange = 6;
    for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        const Object& p = ctx.mgr.object(i);
        if (!p.is_active()) continue;
        uint8_t pt = static_cast<uint8_t>(p.type);
        if (pt == 0x0e ||
            get_range_for_object_type(pt) == kFlyingEnemiesRange) {
            return;
        }
    }

    // Range-check the spawn type before we commit.
    if (spawn_type_id >= static_cast<uint8_t>(ObjectType::COUNT)) return;

    // &4be0-&4bf4: create child via &33b8 create_child_object (mirrored
    // by create_object_at), emit L/R per hive x_flip.
    int slot = ctx.mgr.create_object_at(
        static_cast<ObjectType>(spawn_type_id), 4, obj);
    if (slot < 0) return;

    // &4be0-&4be3: hive birth squelch sound, played as soon as the
    // child slot has been allocated successfully.
    static constexpr uint8_t kSoundHiveSpawn[4] = { 0x33, 0xf3, 0x4f, 0x35 };
    Audio::play_at(Audio::CH_ANY, kSoundHiveSpawn, obj.x.whole, obj.y.whole);

    Object& spawn = ctx.mgr.object(slot);
    // angle &80 = leftward, &00 = rightward. Magnitude 0x20 -> translate
    // to velocity_x ≈ ±0x20 with no y component (calculate_vector_from_
    // magnitude_and_angle returns a vector whose x-axis component is
    // +magnitude for angle 0 and -magnitude for angle 0x80).
    spawn.velocity_x = obj.is_flipped_h() ? -0x20 : 0x20;
    spawn.velocity_y = 0;

    // Shift wasp out of hive AABB (hive is weight-7, would bounce-stick
    // the child otherwise). Mirrors &33e5-&342d create_child_object.
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

// Port of &4789 update_dense_nest (called with Y = touching slot, or
// negative if none):
//   &4789 TYA
//   &478a ORA &dc ; rnd_state+3       ; 1-in-2 chance of acting
//   &478c BMI &47c2 ; leave            ; nothing touching, or roll miss
//   &478e JMP &0ba9 set_object_Y_velocities_from_this_object
// Net: half the time, the dense nest zeroes the touched object's
// velocities by copying its own (zero) vels onto the toucher. Min-energy
// 0x7f keeps the nest at its range-2 floor (not in the 6502 routine
// itself but matches its long-term steady state).
void update_dense_nest(Object& obj, UpdateContext& ctx) {
    NPC::enforce_minimum_energy(obj, 0x7f);
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS &&
        (ctx.rng.next() & 0x80) == 0) {
        Object& other = ctx.mgr.object(obj.touching);
        other.velocity_x = obj.velocity_x;
        other.velocity_y = obj.velocity_y;
    }
}

// &4E37 sucking_nests_trigger_table — object type that activates each
// variant. 0xff means "activates on any object" (bit 7 -> always active).
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

// &4DED-&4E34 update_sucking_nest. Port-only simplifications: drops the
// &4dfd-&4e01 coronium-boulder -> yellow-slime alias, and the &3c99 random
// per-attempt probability gate (we accept any matching candidate).
// Random-flip (&4e1f) and 1/256 high-damage roll (&4e23) preserved.
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

    // &4e0f-&4e1c: accelerate_all_objects. Iterate every primary slot
    // including the player, LOS-raycast out to `power`, then add a
    // magnitude-(power - weight*2 - 8 - distance)/2 velocity vector
    // aimed away from (repel) or toward (attract) the nest.
    uint8_t damage_amount = 2;
    if (active) {
        uint8_t power = sucking_nests_power[variant];
        bool attract = (sucking_nests_direction[variant] & 0x01) != 0;
        uint8_t max_tiles = static_cast<uint8_t>(power / 8);
        for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            if (i == ctx.this_slot) continue;
            Object& other = ctx.mgr.object(i);
            if (!other.is_active()) continue;

            // Distance in &20 fractions = Chebyshev(dfx,dfy)/0x20, the
            // 6502's `&83 distance` from check_for_obstruction_between_
            // objects_A at &35bb-&35cc.
            int sfx = int(obj.x.whole) * 256 + int(obj.x.fraction);
            int sfy = int(obj.y.whole) * 256 + int(obj.y.fraction);
            int ofx = int(other.x.whole) * 256 + int(other.x.fraction);
            int ofy = int(other.y.whole) * 256 + int(other.y.fraction);
            int dfx = ofx - sfx;
            int dfy = ofy - sfy;
            int distance = std::max(std::abs(dfx), std::abs(dfy)) / 0x20;
            if (distance > power) continue;
            if (!NPC::has_line_of_sight(obj, static_cast<uint8_t>(i),
                                        max_tiles, ctx)) continue;

            // &3461-&3476: weight_factor = weight*2 + 8 + distance.
            // weight == 7 marks static targets (&346b sets the static
            // bit) which never get a velocity write.
            uint8_t w = other.weight();
            if (w == 7) continue;
            int weight_factor = w * 2 + 8 + distance;
            if (weight_factor > power) continue;
            uint8_t magnitude = static_cast<uint8_t>((power - weight_factor) / 2);

            // &344f-&3453: angle from source to target, XOR with the
            // sign byte to attract (target moves opposite to that
            // vector). int8 whole-tile deltas are enough for angle_from_
            // deltas — it only cares about ratio + signs.
            int8_t dx_t = static_cast<int8_t>(other.x.whole - obj.x.whole);
            int8_t dy_t = static_cast<int8_t>(other.y.whole - obj.y.whole);
            uint8_t angle = NPC::angle_from_deltas(dx_t, dy_t);
            if (attract) angle ^= 0x80;

            int8_t vx = 0, vy = 0;
            NPC::vector_from_magnitude_and_angle(magnitude, angle, vx, vy);

            int new_vx = int(other.velocity_x) + int(vx);
            int new_vy = int(other.velocity_y) + int(vy);
            if (new_vx >  127) new_vx =  127;
            if (new_vx < -128) new_vx = -128;
            if (new_vy >  127) new_vy =  127;
            if (new_vy < -128) new_vy = -128;
            other.velocity_x = static_cast<int8_t>(new_vx);
            other.velocity_y = static_cast<int8_t>(new_vy);
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
    // Player (slot 0) gated by [creatures] sucking_nest_damages_player.
    bool victim_is_player = (obj.touching == 0);
    bool victim_allowed = obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS &&
                          (!victim_is_player || ctx.sucking_nest_damages_player);
    if (victim_allowed) {
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

// Port of &4ba9 update_bush:
//   &4ba9 JSR &28a3 set_this_object_velocities_to_zero
//   &4bac JMP &28aa set_this_object_position_from_previous_position
// We pin BUSH like a weight-7 static in object_update.cpp (search
// `pin_bush`), so this stub is a no-op. Bushes are indestructible
// per &040d.
void update_bush(Object& obj, UpdateContext& ctx) {
    (void)obj; (void)ctx;
}

// &40EE update_cannon. The 6502 cannon has no internal timer: it fires a
// cannonball iff the player has just fired a CANNON_CONTROL_DEVICE within
// range and pointed at it (check_if_object_hit_by_other_control at &0bc7,
// invoked with A=&4f). Without that gate the cannon never fires.
void update_cannon(Object& obj, UpdateContext& ctx) {
    // &40ee-&40f3 check_if_object_hit_by_other_control(CANNON_CONTROL):
    // the cannon fires iff the player aimed at it within the aim cone
    // and a CANNON_CONTROL_DEVICE is in flight. 3-tile range, tolerance
    // narrows with distance — see hit_by_aim_cone at the top of this
    // TU. The flight is one-frame, so the trigger fires exactly once.
    if (hit_by_aim_cone(obj, ctx, ObjectType::CANNON_CONTROL_DEVICE)) {
        // &40f5-&40f9 create_projectile_with_zero_velocity_y(CANNONBALL,
        // vx=0x40). fire_projectile inverts vx via x_flip (matches &33ad).
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

// &4C15-&4C82 update_engine_fire. state = burn timer (hits 0x80 -> inactive);
// tertiary_data_offset bits 0/1 = engine-inactive flag (port simplification
// of the 6502's full data field).
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
    if (ctx.particles) ctx.particles->emit(ParticleType::ENGINE, 1, obj, ctx.cosmetic_rng);

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

// &4B64 update_placeholder_object + &4B7F convert_placeholder_object.
// data byte low 7 bits = real object type; placeholder converts on LOS or
// touch. Port-only: LOS approximated as Chebyshev distance (no raycast).
void update_placeholder(Object& obj, UpdateContext& ctx) {
    // Keep the placeholder pinned — zero velocity every frame so an errant
    // stream from physics / wind / water can't drift it off its tile.
    obj.velocity_x = 0;
    obj.velocity_y = 0;

    // Convert on physical contact (touching != 0x80 means something's there).
    bool touched = obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS;

    // &359a anchor-proximity gate. 6502 uses A=&80 (16 tiles); viewport
    // is ~20 wide so anything tighter leaves placeholders visible.
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

// Debug-only "Wiring" overlay. Re-walks switch_effects_table without
// mutating state.
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
