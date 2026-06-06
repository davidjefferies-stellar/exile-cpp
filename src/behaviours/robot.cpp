#include "behaviours/robot.h"
#include "behaviours/mood.h"
#include "behaviours/path.h"
#include "objects/object_data.h"
#include "particles/particle_system.h"
#include "audio/audio.h"
#include "core/types.h"
#include <algorithm>
#include <cstdlib>

namespace Behaviors {

// Port of &353a gain_energy_Y_and_flash_if_damaged + &4ddf
// use_damaged_palette_if_carry_clear. Three things in one call:
//   1) every 4 frames, if energy < &c0, energy += 1
//   2) floor to per-type minimum (the &4f18-&4f1d table value)
//   3) while energy < &80, strobe the damaged palette (base ^ &30) for
//      2-in-8 frames so the sprite visibly flickers
// The energy < &80 gate also drives the "stunned, doesn't fire"
// behaviour at &4efb — callers check obj.energy < 0x80 to skip firing.
static void regen_and_flash_if_damaged(Object& obj, UpdateContext& ctx,
                                        uint8_t min_energy) {
    if (ctx.every_four_frames && obj.energy != 0 && obj.energy < 0xc0) {
        obj.energy++;
    }
    NPC::enforce_minimum_energy(obj, min_energy);
    uint8_t base_palette = object_types_palette_and_pickup[
        static_cast<uint8_t>(obj.type)] & 0x7f;
    bool low_energy    = obj.energy < 0x80;
    bool damaged_phase = (ctx.frame_counter & 0x07) < 0x02;
    obj.palette = (low_energy && damaged_phase)
                      ? static_cast<uint8_t>(base_palette ^ 0x30)
                      : base_palette;
}

// Debug log helper: record actual flip transitions only. before_flip is
// the FLIP_HORIZONTAL bit captured before a flip-candidate call. Encodes
// velocity_x in event.x ("why"), new facing in event.y.
static void log_flip_if_changed(Object& obj, UpdateContext& ctx,
                                 uint8_t before_flip) {
    uint8_t after = obj.flags & ObjectFlags::FLIP_HORIZONTAL;
    if (before_flip == after) return;
    ctx.mgr.record_debug_event(
        ObjectManager::EVT_FLIP,
        static_cast<uint8_t>(ctx.this_slot),
        static_cast<uint8_t>(obj.type),
        static_cast<uint8_t>(obj.velocity_x),
        after ? 1 : 0);
}

// &4f0b LDA #&81 / JSR find_a_target_and_fire_at_it. find_object at
// &3c2a picks the nearest of (player, ACTIVE_CHATTER) with LOS;
// returns -1 if none reachable. Chebyshev nearest; LOS via the
// randomised cap (&3cb5).
static int pick_chatter_or_player_target(const Object& obj,
                                          UpdateContext& ctx) {
    int best_slot = -1;
    int best_dist = 0xff;
    const Object& player = ctx.mgr.player();
    if (player.is_active() &&
        NPC::has_line_of_sight_randomized(obj, 0, ctx)) {
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        int8_t dy = static_cast<int8_t>(player.y.whole - obj.y.whole);
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        best_dist = adx > ady ? adx : ady;
        best_slot = 0;
    }
    for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; ++i) {
        const Object& cand = ctx.mgr.object(i);
        if (!cand.is_active()) continue;
        if (cand.type != ObjectType::ACTIVE_CHATTER) continue;
        int8_t dx = static_cast<int8_t>(cand.x.whole - obj.x.whole);
        int8_t dy = static_cast<int8_t>(cand.y.whole - obj.y.whole);
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        int dist = adx > ady ? adx : ady;
        if (dist >= best_dist) continue;
        if (!NPC::has_line_of_sight_randomized(
                obj, static_cast<uint8_t>(i), ctx)) continue;
        best_dist = dist;
        best_slot = i;
    }
    return best_slot;
}

// &4ED8: Turret (green/white and cyan/red). Stationary emplacement that
// rotates (via h-flip) to face the player and fires angled projectiles
// whose velocity is random within the 6502's `[0x2d, 0x3c]` band.
void update_turret(Object& obj, UpdateContext& ctx) {
    // Per-type minimum from &4f1b / &4f1c: green/white 0x14, cyan/red
    // 0x7f. The regen + flash runs every frame even on inactive turrets
    // so a damaged turret visibly recovers.
    uint8_t min_energy = (obj.type == ObjectType::CYAN_RED_TURRET)
                             ? 0x7f : 0x14;
    regen_and_flash_if_damaged(obj, ctx, min_energy);

    // &4ed8-&4ed9 LSR A; BCS leave. Bit 0 of the tertiary data byte is
    // the "inactive" flag — a wired-off turret recharges but never
    // fires. Without this an inactive turret with low bit set still
    // shot at the player as soon as energy reached 0x80.
    if (obj.tertiary_data_offset & 0x01) return;

    // &4efb: don't try to fire until energy >= 0x80 ("stunned"). Below
    // that the turret is recharging — palette flashes via the helper
    // above and firing is skipped.
    if (obj.energy < 0x80) return;

    // &276a-&2773 RNG gate: prob ((energy>>3)+2)/256 per frame. Gives
    // turrets their sporadic "pop … pop" cadence vs a fixed 8-frame rhythm.
    uint8_t threshold = static_cast<uint8_t>((obj.energy >> 3) + 2);
    if (ctx.rng.next() >= threshold) return;

    // LOS via &4f0d -> &3c2a randomised cap (&3cb5), not the fixed
    // 16-tile _80 variant — same path the other robots use.
    if (!NPC::has_line_of_sight_randomized(obj, /*target_slot=*/0, ctx)) {
        return;
    }

    // Full firing chain (&3355). Returns a shot vector if the player's
    // reachable with the random firing-velocity pick, otherwise false.
    int8_t aim_vx = 0, aim_vy = 0;
    if (!NPC::fire_at_target(obj, ctx.mgr.player(), ctx.rng,
                             aim_vx, aim_vy)) {
        return; // out of range / would exceed speed cap
    }

    // &27a3-&27af pivot-to-face: shot behind turret -> flip and wait a
    // frame. vector_x==0 treated as right-facing (6502 BMI/BPL split).
    bool facing_left = obj.is_flipped_h();
    bool want_left   = (aim_vx < 0);
    if (facing_left != want_left) {
        // &3136 flip_this_object_horizontally
        uint8_t before_flip = obj.flags & ObjectFlags::FLIP_HORIZONTAL;
        obj.flags ^= ObjectFlags::FLIP_HORIZONTAL;
        log_flip_if_changed(obj, ctx, before_flip);
        return;
    }

    // &4ed8-&4edb data = (bullet_type<<1) | inactive_bit. Standard
    // turret data: 0x26 ICER, 0x28 TRACER, 0x30 PISTOL.
    ObjectType bullet = ObjectType::PISTOL_BULLET;
    {
        uint8_t data = obj.tertiary_data_offset;
        if ((data & 0x01) == 0) {
            uint8_t bullet_id = static_cast<uint8_t>(data >> 1);
            if (bullet_id < static_cast<uint8_t>(ObjectType::COUNT)) {
                bullet = static_cast<ObjectType>(bullet_id);
            }
        }
    }

    int slot = NPC::fire_projectile(obj, bullet, ctx);
    if (slot >= 0) {
        Object& b = ctx.mgr.object(slot);
        b.velocity_x = aim_vx;
        b.velocity_y = aim_vy;
        NPC::offset_child_from_parent(b, obj);
        b.timer = 64;
    }
}

// &4ede update_rolling_robot (magenta/red). The 6502 path is &4ede BIT &15
// / BPL set_turret_or_robot_energy: if energy < &80 short-circuit straight
// to the helper (no movement, no firing). Otherwise FALL THROUGH into
// &4ee2 update_blue_rolling_robot which does the full mood + NPC path +
// walking-type-4 movement + (energy>>3)+2 fire gate with the per-type
// bullet from &4f1e (magenta PISTOL, red ICER, blue TRACER).
void update_rolling_robot(Object& obj, UpdateContext& ctx) {
    if (obj.energy < 0x80) {
        uint8_t min_energy = (obj.type == ObjectType::RED_ROLLING_ROBOT)
                                 ? 0x46 : 0x14;
        regen_and_flash_if_damaged(obj, ctx, min_energy);
        return;
    }
    update_blue_rolling_robot(obj, ctx);
}

// &4EE2 update_blue_rolling_robot. Walking type 4 (max_angle=0x20,
// max_accel=4, weight=1, turn_prob/jump_prob=0) -> ground-walking only;
// gravity owns velocity_y. Only set velocity_x.
void update_blue_rolling_robot(Object& obj, UpdateContext& ctx) {
    // &4f12 ldy &4efc,X: per-type min energy. Magenta 0x14, red 0x46,
    // blue 0x46. The flash + regen helper runs even when magenta/red
    // short-circuited above (they routed to set_turret_or_robot_energy).
    uint8_t min_energy = 0x46;
    if (obj.type == ObjectType::MAGENTA_ROLLING_ROBOT) min_energy = 0x14;
    regen_and_flash_if_damaged(obj, ctx, min_energy);

    // &4ee4: check_for_npc_stimuli (mood / phobia / interest reactions).
    Mood::update_mood(obj, ctx);

    // &4ee7: consider_updating_npc_path — refresh target_and_flags via
    // the LOS-gated directness chain.
    NPC::update_npc_path(obj, ctx);

    // &4eea-&4eee update_walking_npc with walking_type=4, speed=0x18.
    //   npc_walking_types_maximum_acceleration_table[4] = 0x04 (max accel cap)
    //   npc_walking_types_weight_table[4]               = 0x01 (divide by 2)
    // &3b25-&3b2d: target_vx = sign(rel_tx) * speed, then
    // &3201 apply_weight_and_limit_to_acceleration divides diff by
    // 2^weight BEFORE clamping to ±max_accel. The divide produces the
    // gentle approach to target velocity near the cap — without it the
    // robot snaps to ±0x18 in one frame (felt like a teleport).
    constexpr int8_t kSpeed    = 0x18;
    constexpr int8_t kMaxAccel = 4;
    constexpr int    kWeight   = 1;
    if (obj.is_supported()) {
        // Read obj.tx (output of update_npc_path), NOT target.x. When LOS
        // is blocked, directness decays to 0 and use_relaxed_path puts
        // tx/ty at a wander offset around the NPC — reading target.x
        // bypassed that and let sealed-room robots home through doors.
        bool avoid = (obj.target_and_flags & TargetFlags::AVOID) != 0;
        int8_t dx  = static_cast<int8_t>(obj.tx - obj.x.whole);
        if (avoid) dx = static_cast<int8_t>(-dx);
        int8_t target_vx = (dx > 0) ? kSpeed
                         : (dx < 0) ? static_cast<int8_t>(-kSpeed)
                         : 0;
        int diff = int(target_vx) - int(obj.velocity_x);
        int sign = (diff < 0) ? -1 : 1;
        int abs_diff = diff < 0 ? -diff : diff;
        abs_diff >>= kWeight;                       // &3208 LSR ×(weight)
        if (abs_diff > kMaxAccel) abs_diff = kMaxAccel;
        obj.velocity_x = static_cast<int8_t>(
            int(obj.velocity_x) + sign * abs_diff);
    }

    // &4ef1: 1-in-4 gated flip (shared path with magenta/red rolling robot).
    NPC::consider_face_movement_direction(obj, ctx.rng);

    // &4ef9-&4f0d consider_firing. Energy>=0x80 gate, then per-frame
    // probability ((energy>>3)+2)>=rnd via &276a-&2773. Per-type bullet
    // from &4f1e rolling_robots_bullet_table: magenta=PISTOL, red=ICER,
    // blue=TRACER. Targets ACTIVE_CHATTER + player (A=&81).
    if (obj.energy >= 0x80) {
        uint8_t prob = static_cast<uint8_t>((obj.energy >> 3) + 2);
        if (prob >= ctx.rng.next()) {
            int target_slot = pick_chatter_or_player_target(obj, ctx);
            if (target_slot >= 0) {
                const Object& tgt = ctx.mgr.object(target_slot);
                int8_t tdx = static_cast<int8_t>(tgt.x.whole - obj.x.whole);
                ObjectType bullet = ObjectType::TRACER_BULLET;
                if (obj.type == ObjectType::MAGENTA_ROLLING_ROBOT) {
                    bullet = ObjectType::PISTOL_BULLET;
                } else if (obj.type == ObjectType::RED_ROLLING_ROBOT) {
                    bullet = ObjectType::ICER_BULLET;
                }
                int slot = NPC::fire_projectile(obj, bullet, ctx);
                if (slot >= 0) {
                    Object& b = ctx.mgr.object(slot);
                    b.velocity_x = (tdx > 0) ? 0x18 : -0x18;
                    b.velocity_y = 0;
                    NPC::offset_child_from_parent(b, obj);
                    b.timer = 96;
                }
            }
        }
    }

    // &4f10-&4f15 JMP gain_energy_Y_and_flash_if_damaged with Y indexed
    // by type from &4efc (magenta 0x14, red 0x46, blue 0x46). Re-applied
    // last so the firing gate above sees the un-floored value.
    NPC::enforce_minimum_energy(obj, min_energy);
}

// Port of &4804 update_hovering_robot. The entire body is gated on
// &4807 BCC &47c2 ; leave — if set_turret_or_robot_energy returns
// carry clear (energy < &80) the robot idles for the frame, drawing
// the damaged palette but doing nothing else.
//
// set_turret_or_robot_energy (&4f10) -> gain_energy_Y_and_flash_if_
// damaged (&353a), with Y = &14 (hovering-robot minimum from &4f1d):
//   - every 4 frames, if energy < &c0, energy += 1
//   - floor to &14 (skipped while exploding, energy == 0)
//   - ASL A sets carry from energy bit 7 → carry CLEAR if low
//   - if low: 2-in-8 frames (frame_counter & 7 < 2) use damaged palette
//   - use_damaged_palette_if_carry_clear (&4ddf): base = palette table
//     entry & 0x7f, XOR #&30 when carry clear (toggles colour-3 entry)
void update_hovering_robot(Object& obj, UpdateContext& ctx) {
    // &4804 JSR &4f10 set_turret_or_robot_energy → &353a. Min &4f1d =
    // 0x14. helper handles regen + the &30-XOR palette flash; the
    // &4807 BCC leave gate (whole update skipped while energy < &80)
    // is the early return below.
    regen_and_flash_if_damaged(obj, ctx, 0x14);
    if (obj.energy < 0x80) return;

    // &480c-&4811: 1-in-128 chance per frame of the ambient hover whine.
    if ((ctx.rng.next() & 0x7f) == 0) {
        static constexpr uint8_t kSoundHover[4] = { 0x33, 0xf3, 0x63, 0xe3 };
        Audio::play_at(Audio::CH_ANY, kSoundHover, obj.x.whole, obj.y.whole);
    }

    // &4815-&481d LDA &d9 / CMP #&40 / BCS move_hovering_npc — 1-in-4
    // detour to fire, then falls through to move_hovering_npc either way.
    // &276c-&2773 inside find_a_target_and_fire_at_it applies the inner
    // energy-scaled gate ((energy>>3)+2 >= rnd) so it fires less wounded.
    if ((ctx.rng.next() & 0xc0) == 0 &&
        NPC::has_line_of_sight_randomized(obj, /*target_slot=*/0, ctx) &&
        ctx.rng.next() <= static_cast<uint8_t>((obj.energy >> 3) + 2)) {
        int8_t dx = static_cast<int8_t>(
            ctx.mgr.player().x.whole - obj.x.whole);
        int slot = NPC::fire_projectile(obj, ObjectType::PISTOL_BULLET, ctx);
        if (slot >= 0) {
            Object& b = ctx.mgr.object(slot);
            b.velocity_x = (dx > 0) ? 0x20 : -0x20;
            b.velocity_y = 0;
            NPC::offset_child_from_parent(b, obj);
            b.timer = 48;
        }
    }

    // &486e move_hovering_npc (shared with hovering balls): target the
    // player, follow the LOS-gated path waypoint, then thrust hard toward
    // it every frame. Runs unconditionally after the fire detour.
    obj.target_and_flags = (obj.target_and_flags & ~0x1fu);  // &4870 target = player
    NPC::update_npc_path(obj, ctx);                          // &4872 consider_updating_npc_path

    // &4875 LDA #&07 / consider_flipping_object_to_match_velocity_x_A:
    // (#&07 AND rnd)==0 -> 1-in-8 flip (the "1 in 32" disasm note is wrong).
    {
        uint8_t before_flip = obj.flags & ObjectFlags::FLIP_HORIZONTAL;
        if ((ctx.rng.next() & 0x07) == 0) NPC::face_movement_direction(obj);
        log_flip_if_changed(obj, ctx, before_flip);
    }

    // &487a thrust_towards_target: magnitude 0x1c, max-accel 4, 1-in-2.
    NPC::move_towards_target_with_probability(obj, ctx, 0x1c, 4, 0x80);
    NPC::cancel_gravity(obj);                       // &4883 DEC acceleration_y
    NPC::consider_hovering_over_ground(obj, ctx);   // &4885
    // &4888 add_jetpack_thrust_particles. 6502 gates on accel != 0; our
    // hover folds thrust into velocity, so approximate with "heading
    // somewhere": a confined robot parked on its own tile (tx/ty ==
    // position via the relaxed-path fallback) has no thrust and must not
    // puff in place.
    int8_t tdx = static_cast<int8_t>(obj.tx - obj.x.whole);
    int8_t tdy = static_cast<int8_t>(obj.ty - obj.y.whole);
    if (ctx.particles && (tdx != 0 || tdy != 0)) {  // &4888
        ctx.particles->emit(ParticleType::JETPACK, 1, obj, ctx.cosmetic_rng);
    }
}

// Port of &481f update_clawed_robot head (5-inst entry, falls through
// to the shared fire / hover-towards-target chain):
//   &481f JSR &253c check_if_object_was_damaged    ; carry = took >=8
//   &4822 ROR &11 ; this_object_state              ; bit 7 = damaged
//   &4824 LSR &11                                    ; consume the bit
//   &4826 LDX &41 ; this_object_type
//   &4828 LDY &4881,X ; clawed_robots_energy_table   ; min-energy lookup
// 4 variants share the body; the energy table at &4881 drives the
// teleport-away threshold.
void update_clawed_robot(Object& obj, UpdateContext& ctx) {
    // Min energy depends on variant
    uint8_t min_energy;
    switch (obj.type) {
        case ObjectType::MAGENTA_CLAWED_ROBOT: min_energy = 0x46; break;
        case ObjectType::CYAN_CLAWED_ROBOT:    min_energy = 0x5a; break;
        case ObjectType::GREEN_CLAWED_ROBOT:   min_energy = 0x80; break;
        case ObjectType::RED_CLAWED_ROBOT:     min_energy = 0x82; break;
        default: min_energy = 0x46; break;
    }

    // &4856-&485b: 1-in-128 chance per frame of the clawed robot's
    // ambient growl.
    if ((ctx.rng.next() & 0x7f) == 0) {
        static constexpr uint8_t kSoundClawed[4] = { 0x17, 0x03, 0x68, 0xa3 };
        Audio::play_at(Audio::CH_ANY, kSoundClawed, obj.x.whole, obj.y.whole);
    }

    // &481f-&4824: capture "damaged" state. The 6502 ROR/LSR &11 keeps
    // bit 7 of state as a sticky "recently took >=8 damage" flag for
    // one frame so the teleport-away check below can skip.
    bool recently_damaged = (obj.state & 0x80) != 0;
    obj.state = static_cast<uint8_t>(obj.state & 0x7f);

    // Gain 2 energy per update
    if (obj.energy < 0xff - 1) obj.energy += 2;
    NPC::enforce_minimum_energy(obj, min_energy);

    // &4837-&4845 teleport-away gate:
    //   (energy < 0x8c) OR ((directness bits == 0) AND (frame_counter == 0))
    //   AND NOT recently_damaged
    // 6502 uses &06 this_object_frame_counter (slot*0x11+global).
    uint8_t per_obj_counter = static_cast<uint8_t>(
        ctx.this_slot * 0x11 + ctx.frame_counter);
    bool directness_zero = (obj.target_and_flags & 0xc0) == 0;
    bool should_teleport =
        (obj.energy < 0x8c) ||
        (directness_zero && per_obj_counter == 0);
    if (should_teleport && !recently_damaged) {
        // &489e STA &16 (ty = 0) + &0ce5 set_teleporting. We don't have
        // the teleporting flag plumbed for clawed robots, so deactivate
        // by zeroing y.whole (is_active gate); the slot will respawn
        // from its tertiary entry when conditions match.
        obj.y.whole = 0;
        obj.velocity_x = 0;
        obj.velocity_y = 0;
        return;
    }

    // &4714 can_see_or_has_seen_player gate. Pursue/fire only at
    // directness>=2; otherwise wander.
    NPC::update_npc_path(obj, ctx);
    NPC::seek_player(obj, ctx.mgr.player(), 6);
    // &4877: 1-in-4 gated flip (clawed robots also share move_hovering_npc).
    {
        uint8_t before_flip = obj.flags & ObjectFlags::FLIP_HORIZONTAL;
        NPC::consider_face_movement_direction(obj, ctx.rng);
        log_flip_if_changed(obj, ctx, before_flip);
    }

    uint8_t lvl = NPC::directness_level(obj);

    // &485f-&486b find_a_target_and_fire_at_it. 6502 fire rate scales
    // with energy via &276c-&2773: fire iff (energy/8 + 2) >= rnd. At
    // max energy ~13% / frame; at min-energy floor ~3-5% / frame.
    if (lvl >= 2 &&
        NPC::has_line_of_sight_randomized(obj, /*target_slot=*/0, ctx)) {
        uint8_t threshold =
            static_cast<uint8_t>((obj.energy >> 3) + 2);
        if (ctx.rng.next() < threshold) {
            const Object& player = ctx.mgr.player();
            int8_t vx, vy;
            if (NPC::fire_at_target(obj, player, ctx.rng, vx, vy)) {
                int slot = NPC::fire_projectile(
                    obj, ObjectType::ICER_BULLET, ctx);
                if (slot >= 0) {
                    Object& b = ctx.mgr.object(slot);
                    b.velocity_x = vx;
                    b.velocity_y = vy;
                    NPC::offset_child_from_parent(b, obj);
                    b.timer = 48;
                }
            }
        }
    }

    // Melee damage on contact
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 15,
                                   ctx.damage_events, &ctx);

    // &4885 consider_hovering_over_ground — clawed robots reach the
    // shared &487a thrust_towards_target path via &4864 consider_
    // firing_at_player_and_move_robot, so the hover thrust applies.
    NPC::cancel_gravity(obj);
    NPC::consider_hovering_over_ground(obj, ctx);
}

// Port of &43e7 update_hovering_ball (3-inst head) + &43eb
// update_invisible_hovering_ball (fall-through entry):
//   &43e7 JSR &4dd2 rotate_colour_from_frame_counter ; visible variant only
//   &43ea TYA
//   &43eb BMI &4400 ; not_touching_other_object       ; invisible entry
// Shared body factored to take a "is visible" flag matching the 6502's
// fall-through (visible variant runs &4dd2 colour rotate then drops
// into the invisible entry at &43eb).
static void update_hovering_ball_common(Object& obj, UpdateContext& ctx,
                                        bool visible) {
    // &1a8d-&1a97 per-object frame counter: (slot<<4 | slot) + global.
    // Desyncs hover-ball palette cycles + timers so a swarm doesn't
    // strobe in unison.
    uint8_t this_fc = static_cast<uint8_t>(
        ((ctx.this_slot << 4) | (ctx.this_slot & 0x0f)) + ctx.frame_counter);

    // &43e7-&43ea visible-only colour rotation through &4dd2's 4-entry
    // transporter_beams_palette_table at &4d82.
    if (visible) {
        static constexpr uint8_t kPaletteTable[4] = { 0x52, 0x63, 0x35, 0x21 };
        obj.palette = kPaletteTable[(this_fc >> 2) & 0x03];
    }

    // &43eb-&43f6 damage touched object by 3 iff its type differs (EOR
    // / BEQ skip). Visible vs invisible balls have different types so
    // they DO damage each other; only same-variant pairs skip. Port of
    // damage_object inlined since the helper only targets the player.
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        Object& other = ctx.mgr.object(obj.touching);
        if (other.type != obj.type) {
            constexpr uint8_t kDamage = 3;
            if (obj.touching == 0) {
                Object& player = ctx.mgr.player();
                uint16_t hurt = std::min<uint16_t>(kDamage, player.energy);
                player.energy = (player.energy > kDamage)
                              ? static_cast<uint8_t>(player.energy - kDamage)
                              : 0;
                if (ctx.damage_events && hurt > 0) {
                    DamageVisual ev;
                    ev.src_x = obj.x.whole;       ev.src_y = obj.y.whole;
                    ev.src_x_frac = obj.x.fraction; ev.src_y_frac = obj.y.fraction;
                    ev.tgt_x = player.x.whole;    ev.tgt_y = player.y.whole;
                    ev.tgt_x_frac = player.x.fraction; ev.tgt_y_frac = player.y.fraction;
                    ev.tgt_slot = 0;
                    ev.amount  = static_cast<uint8_t>(hurt);
                    ctx.damage_events->push_back(ev);
                }
            } else {
                other.energy = (other.energy > kDamage)
                             ? static_cast<uint8_t>(other.energy - kDamage)
                             : 0;
                // &24ed-&24f6 sets WAS_DAMAGED only when damage>=8; 3 < 8 so skip.
            }
            // &43f9-&43fc impact sound.
            static constexpr uint8_t kSoundBallZap[4] = { 0x33, 0x03, 0x85, 0x02 };
            Audio::play_at(Audio::CH_ANY, kSoundBallZap, obj.x.whole, obj.y.whole);
        }
    }

    // &4400-&4404 energy &= 4 — any damage that clears bit 2 leaves
    // energy zero, so step 12 explodes the ball next frame.
    obj.energy = static_cast<uint8_t>(obj.energy & 0x04);

    // &4406-&440d DEC timer; on zero call set_object_as_far_away (return
    // to nest) and play the teleport sound. Port simplification: mark
    // PENDING_REMOVAL — the nest creature-count restore at &1d20-&1d21
    // isn't wired up yet, but the &440d teleport sound IS faithful.
    obj.timer--;
    if (obj.timer == 0) {
        static constexpr uint8_t kSoundTeleport[4] = { 0x29, 0xc2, 0x37, 0xf3 };
        Audio::play_at(Audio::CH_ANY, kSoundTeleport,
                       obj.x.whole, obj.y.whole);
        obj.flags |= ObjectFlags::PENDING_REMOVAL;
        return;
    }

    // &486e move_hovering_npc: target = player; &4872 consider_updating_
    // npc_path sets obj.tx/ty from the target via LOS-gated directness.
    // Without it tx/ty stay 0 from init and the ball thrusts toward
    // world (0,0) -- looks like it's fleeing the player.
    obj.target_and_flags = (obj.target_and_flags & ~0x1fu);
    NPC::update_npc_path(obj, ctx);
    if ((ctx.rng.next() & 0x07) == 0) {
        NPC::face_movement_direction(obj);
    }

    // &487a thrust_towards_target: magnitude 0x1c, max-accel 4, 1-in-2
    // probability. Comment in the 6502 calls hover balls out as moving
    // "twice as quickly as other flying NPCs" — the 0x1c/4 combo.
    NPC::move_towards_target_with_probability(obj, ctx, 0x1c, 4, 0x80);

    // &4883 DEC acceleration_y — cancel gravity for hovering NPCs.
    NPC::cancel_gravity(obj);

    // &4885 consider_hovering_over_ground — every-4-frame upward thrust
    // scaled by ground proximity, then dampens vy. Without this the
    // ball would gradually settle onto tiles instead of hovering.
    NPC::consider_hovering_over_ground(obj, ctx);

    // &4888 add_jetpack_thrust_particles — visible exhaust puff.
    if (ctx.particles) {
        ctx.particles->emit(ParticleType::JETPACK, 1, obj, ctx.cosmetic_rng);
    }
}

void update_hovering_ball(Object& obj, UpdateContext& ctx) {
    update_hovering_ball_common(obj, ctx, /*visible=*/true);
}

// &43EB entry point — invisible variant skips the &43e7 colour rotate
// (JSR is at &43e7, the BMI gate sits at &43eb so the invisible entry
// falls in past the rotate). Visibility bit 7 of palette stays clear
// from init_object_from_type.
void update_invisible_hovering_ball(Object& obj, UpdateContext& ctx) {
    update_hovering_ball_common(obj, ctx, /*visible=*/false);
}

} // namespace Behaviors
