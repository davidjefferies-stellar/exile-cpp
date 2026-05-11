#include "behaviours/robot.h"
#include "behaviours/mood.h"
#include "behaviours/path.h"
#include "objects/object_data.h"
#include "particles/particle_system.h"
#include "audio/audio.h"
#include "core/types.h"
#include <cstdlib>

namespace Behaviors {

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

// &4ED8: Turret (green/white and cyan/red). Stationary emplacement that
// rotates (via h-flip) to face the player and fires angled projectiles
// whose velocity is random within the 6502's `[0x2d, 0x3c]` band.
void update_turret(Object& obj, UpdateContext& ctx) {
    // Minimum-energy backstop for the green/white turret (&4f1b = 0x14).
    // Energy regenerates toward this value every frame; firing doesn't
    // drain it, but taking damage from the player does.
    NPC::enforce_minimum_energy(obj, 0x14);

    // &4ed8-&4ed9 LSR A; BCS leave. Bit 0 of the tertiary data byte is
    // the "inactive" flag — a wired-off turret recharges but never
    // fires. Without this an inactive turret with low bit set still
    // shot at the player as soon as energy reached 0x80.
    if (obj.tertiary_data_offset & 0x01) return;

    // &4efb: don't try to fire until energy >= 0x80. Below that the
    // turret is "recharging" and silently does nothing.
    if (obj.energy < 0x80) return;

    // &276a-&2773 RNG gate: prob ((energy>>3)+2)/256 per frame. Gives
    // turrets their sporadic "pop … pop" cadence vs a fixed 8-frame rhythm.
    uint8_t threshold = static_cast<uint8_t>((obj.energy >> 3) + 2);
    if (ctx.rng.next() >= threshold) return;

    // LOS via &4f0d → &3c2a randomised cap (&3cb5), not the fixed
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

    // &27a3-&27af pivot-to-face: shot behind turret → flip and wait a
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

// &4EDE: Rolling robot (magenta and red variants)
void update_rolling_robot(Object& obj, UpdateContext& ctx) {
    // Minimum energy varies by type
    uint8_t min_energy = 0x14; // Magenta
    if (obj.type == ObjectType::RED_ROLLING_ROBOT) min_energy = 0x46;
    NPC::enforce_minimum_energy(obj, min_energy);

    // Only move if energy >= 0x80
    if (obj.energy < 0x80) return;

    // On vx==0, resume in facing direction so a post-bounce robot rolls
    // AWAY from what it hit (facing tracks last-moving direction).

    if (obj.is_supported() && obj.velocity_x == 0) {
        obj.velocity_x = obj.is_flipped_h() ? -4 : 4;
    }

    // &4ef1: 1-in-4 gated flip. Unconditional would flicker every frame
    // when velocity_x zero-crosses on wall bumps or seek overshoot.
    {
        uint8_t before_flip = obj.flags & ObjectFlags::FLIP_HORIZONTAL;
        NPC::consider_face_movement_direction(obj, ctx.rng);
        log_flip_if_changed(obj, ctx, before_flip);
    }

    // LOS-gated fire via &3c2a randomised cap (&3cb5). NOD=0xff matches
    // find_object's initial state for the player-only target pool.
    if (ctx.every_sixteen_frames && obj.energy >= 0x80 &&
        NPC::has_line_of_sight_randomized(obj, /*target_slot=*/0, ctx)) {
        const Object& player = ctx.mgr.player();
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        if (std::abs(dx) < 12) {
            ObjectType bullet = ObjectType::PISTOL_BULLET;
            if (obj.type == ObjectType::RED_ROLLING_ROBOT) bullet = ObjectType::ICER_BULLET;
            int slot = NPC::fire_projectile(obj, bullet, ctx);
            if (slot >= 0) {
                Object& b = ctx.mgr.object(slot);
                b.velocity_x = (dx > 0) ? 0x18 : -0x18;
                b.velocity_y = -4;
                NPC::offset_child_from_parent(b, obj);
                b.timer = 48;
            }
        }
    }
}

// &4EE2 update_blue_rolling_robot. Walking type 4 (max_angle=0x20,
// max_accel=4, weight=1, turn_prob/jump_prob=0) → ground-walking only;
// gravity owns velocity_y. Only set velocity_x.
void update_blue_rolling_robot(Object& obj, UpdateContext& ctx) {
    // &4ee4: check_for_npc_stimuli (mood / phobia / interest reactions).
    Mood::update_mood(obj, ctx);

    // &4ee7: consider_updating_npc_path — refresh target_and_flags via
    // the LOS-gated directness chain.
    NPC::update_npc_path(obj, ctx);

    // &4eea-&4eee update_walking_npc reduced port. Speed lowered from
    // 0x18 to 4 (port-only) so the blue isn't ~6x faster than red/magenta
    // siblings. Supported gate matches the 6502 walker's &3b10 exit.
    constexpr int8_t kSpeed = 4;
    // &3201 apply_weight_and_limit_to_acceleration. max_accel=4 is the
    // CAP, not 1 — at 1 a heavy hit takes ~25f to recover vs the 6502's
    // ~6f, making the robot read light.
    constexpr int8_t kMaxAccel = 4;
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
        int step = (diff >  kMaxAccel) ?  kMaxAccel
                 : (diff < -kMaxAccel) ? -kMaxAccel
                 : diff;
        obj.velocity_x = static_cast<int8_t>(int(obj.velocity_x) + step);
    }

    // &4ef1: 1-in-4 gated flip (shared path with magenta/red rolling robot).
    {
        uint8_t before_flip = obj.flags & ObjectFlags::FLIP_HORIZONTAL;
        NPC::consider_face_movement_direction(obj, ctx.rng);
        log_flip_if_changed(obj, ctx, before_flip);
    }

    // &4ef9 consider_firing: tracer bullets, gated on energy >= 0x80
    // (BIT this_object_energy / BPL skips). LOS-gated via the &3cb5
    // randomised cap.
    if (ctx.every_sixteen_frames && obj.energy >= 0x80 &&
        NPC::has_line_of_sight_randomized(obj, /*target_slot=*/0, ctx)) {
        const Object& player = ctx.mgr.player();
        int8_t pdx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        if (std::abs(pdx) < 16) {
            int slot = NPC::fire_projectile(obj, ObjectType::TRACER_BULLET, ctx);
            if (slot >= 0) {
                Object& b = ctx.mgr.object(slot);
                b.velocity_x = (pdx > 0) ? 0x18 : -0x18;
                b.velocity_y = 0;
                NPC::offset_child_from_parent(b, obj);
                b.timer = 96;
            }
        }
    }

    // &4f10-&4f15: minimum energy = 0x46. Re-applied last so the firing
    // gate above sees the live (un-floored) value before this clamp.
    NPC::enforce_minimum_energy(obj, 0x46);
}

// &4804: Hovering robot - flies, patrols, fires
void update_hovering_robot(Object& obj, UpdateContext& ctx) {
    NPC::cancel_gravity(obj);
    NPC::enforce_minimum_energy(obj, 0x81);

    // &480c-&4811: 1-in-128 chance per frame of the ambient hover whine.
    if ((ctx.rng.next() & 0x7f) == 0) {
        static constexpr uint8_t kSoundHover[4] = { 0x33, 0xf3, 0x63, 0xe3 };
        Audio::play_at(Audio::CH_ANY, kSoundHover, obj.x.whole, obj.y.whole);
    }

    // Patrol: hover near player
    if (ctx.every_eight_frames) {
        NPC::seek_player(obj, ctx.mgr.player(), 3);
    }

    // Random vertical jitter
    if (ctx.every_four_frames) {
        obj.velocity_y += (ctx.rng.next() & 0x03) - 1;
    }

    // &4877: 1-in-4 gated flip (hovering robots share move_hovering_npc
    // which runs the probability-gated variant).
    {
        uint8_t before_flip = obj.flags & ObjectFlags::FLIP_HORIZONTAL;
        NPC::consider_face_movement_direction(obj, ctx.rng);
        log_flip_if_changed(obj, ctx, before_flip);
    }

    // Fire at player. LOS-gated — 6502 hovering robot fires through the
    // shared find_a_target_and_fire_at_it path (&486b), which internally
    // calls find_object (&3c2a) with carry clear = consider obstructions
    // and a randomised cap (&3cb5 AND #&4f / EOR NOD).
    if (ctx.every_sixteen_frames && obj.energy >= 0x80 &&
        NPC::has_line_of_sight_randomized(obj, /*target_slot=*/0, ctx)) {
        const Object& player = ctx.mgr.player();
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        if (std::abs(dx) < 12) {
            int slot = NPC::fire_projectile(obj, ObjectType::RED_BULLET, ctx);
            if (slot >= 0) {
                Object& b = ctx.mgr.object(slot);
                b.velocity_x = (dx > 0) ? 0x20 : -0x20;
                b.velocity_y = 0;
                NPC::offset_child_from_parent(b, obj);
                b.timer = 48;
            }
        }
    }
}

// &481F: Clawed robot (4 variants)
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

    // Gain 2 energy per update
    if (obj.energy < 0xff - 1) obj.energy += 2;
    NPC::enforce_minimum_energy(obj, min_energy);

    // Teleport away if low energy or can't reach player
    if (obj.energy < 0x8c) {
        if (ctx.rng.next() == 0) {
            // Teleport to bottom of world
            obj.y.whole = 0xfe;
            obj.velocity_x = 0;
            obj.velocity_y = 0;
            return;
        }
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

    // Fire LOS-gated via &3cb5 randomised-cap variant. Directness alone
    // latches (3→2 only via &3d1d), letting robots shoot through a door
    // that closed after first sighting — fire path needs its own raycast
    // (matches 6502 find_a_target_and_fire_at_it at &4868).
    if (ctx.every_eight_frames && lvl >= 2 &&
        NPC::has_line_of_sight_randomized(obj, /*target_slot=*/0, ctx)) {
        const Object& player = ctx.mgr.player();
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        int8_t dy = static_cast<int8_t>(player.y.whole - obj.y.whole);
        if (std::abs(dx) < 10 && std::abs(dy) < 10) {
            int slot = NPC::fire_projectile(obj, ObjectType::ICER_BULLET, ctx);
            if (slot >= 0) {
                Object& b = ctx.mgr.object(slot);
                b.velocity_x = (dx > 0) ? 0x20 : -0x20;
                b.velocity_y = (dy > 0) ? 0x10 : -0x10;
                NPC::offset_child_from_parent(b, obj);
                b.timer = 48;
            }
        }
    }

    // Melee damage on contact
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 15, ctx.damage_events);
}

// &43E7 update_hovering_ball / &43EB update_invisible_hovering_ball.
// energy &= 4 keeps bit 2 as alive-marker; timer 0 means teleport-home.
// Port-only: set_object_as_far_away → PENDING_REMOVAL (no nest teleport
// wired yet).
void update_hovering_ball(Object& obj, UpdateContext& ctx) {
    // &43e7: rotate colour by frame counter. The 6502's routine uses
    // a 4-colour table indexed by `frame_counter >> 2 & 0x03`; we
    // approximate with a palette cycle on the low nibble.
    obj.palette = (obj.palette & 0xf0) | ((ctx.frame_counter >> 2) & 0x07);

    // &43eb-&43f6: damage non-hovering-ball colliders for 3. A ball
    // touching another ball no-ops (so swarms don't self-destruct).
    if (obj.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        const Object& other = ctx.mgr.object(obj.touching);
        bool other_is_ball =
            other.type == ObjectType::HOVERING_BALL ||
            other.type == ObjectType::INVISIBLE_HOVERING_BALL;
        if (!other_is_ball) {
            NPC::damage_player_if_touching(obj, ctx.mgr.player(), 3, ctx.damage_events);
            // Hovering-ball impact zap. Bytes follow JSR play_sound at &13fa.
            static constexpr uint8_t kSoundBallZap[4] = { 0x33, 0x03, 0x85, 0x02 };
            Audio::play_at(Audio::CH_ANY, kSoundBallZap, obj.x.whole, obj.y.whole);
        }
    }

    // &4400-&4404: energy &= 4 — any damage that clears bit 2 leaves
    // energy zero, so the main loop's step 12 explodes it next frame.
    obj.energy = static_cast<uint8_t>(obj.energy & 0x04);

    // &4406-&4408: DEC timer; BNE move (unconditional). First decrement
    // wraps 0→0xff, giving a ~256 frame lifespan. Port-only: replace
    // set_object_as_far_away with PENDING_REMOVAL (no nest return path).
    obj.timer--;
    if (obj.timer == 0) {
        obj.flags |= ObjectFlags::PENDING_REMOVAL;
        return;
    }

    // &4415 move_hovering_npc → target the player, run NPC path, with
    // a 1-in-8 chance of flipping to face movement.
    if ((ctx.rng.next() & 0x07) == 0) {
        NPC::face_movement_direction(obj);
    }

    // &487a thrust_towards_target: magnitude 0x1c, max-accel 4,
    // 1-in-2 probability (threshold 0x80). Hovering balls are called
    // out in the 6502 as moving "twice as quickly as other flying
    // NPCs" — that's what the 0x1c / 4 combo produces.
    NPC::move_towards_target_with_probability(obj, ctx, 0x1c, 4, 0x80);

    // &4883: cancel gravity for hovering NPCs.
    NPC::cancel_gravity(obj);

    // &4888: emit a jetpack thrust particle under the ball so it looks
    // like it's hovering on a puff of exhaust.
    if (ctx.particles) {
        ctx.particles->emit(ParticleType::JETPACK, 1, obj, ctx.rng);
    }
}

// &43EB invisible variant. 6502 JMPs past &43ea rotate; we suppress
// visibility by clearing palette bit 7 — still solid, drawn as SPRITE_NONE.
void update_invisible_hovering_ball(Object& obj, UpdateContext& ctx) {
    update_hovering_ball(obj, ctx);
    // Skip the palette cycle that the visible variant does — keep the
    // palette at whatever init_object_from_type gave it so
    // this-object-visibility bit 7 stays clear.
    obj.palette = object_types_palette_and_pickup[
        static_cast<uint8_t>(ObjectType::INVISIBLE_HOVERING_BALL)] & 0x7f;
}

} // namespace Behaviors
