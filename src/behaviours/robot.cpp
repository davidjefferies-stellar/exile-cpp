#include "behaviours/robot.h"
#include "behaviours/mood.h"
#include "behaviours/path.h"
#include "objects/object_data.h"
#include "particles/particle_system.h"
#include "core/types.h"
#include <cstdlib>

namespace Behaviors {

// Record a flip event in the lifecycle log whenever obj's h-flip bit
// actually changed between `before_flip` and obj.flags. Skips the log if
// the flag is unchanged so the output only shows real direction flips.
// `before_flip` is the masked FLIP_HORIZONTAL bit captured *before* the
// flip-candidate call (consider_face_movement_direction, the turret's
// pivot xor, or any other site that might toggle facing). Encodes
// velocity_x into the event's x slot (for "why did it flip") and new
// facing into y (0 = right, 1 = left).
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

    // &276a-&2773 find_a_target_and_fire_at_it's RNG gate. The turret
    // fires with probability ((energy >> 3) + 2) / 256 per frame —
    // ~13% at full energy, dropping off as it takes damage. This is
    // what gives 6502 turrets their sporadic "pop … pop" cadence
    // rather than a fixed 8-frame rhythm. At 50 fps expect ~6.5
    // shots/second when fresh.
    uint8_t threshold = static_cast<uint8_t>((obj.energy >> 3) + 2);
    if (ctx.rng.next() >= threshold) return;

    // Line-of-sight gate. Turrets fire via `find_a_target_and_fire_at_it`
    // at &4f0d → `find_object` at &3c2a, which applies the randomised
    // &3cb5 cap — the same LOS path the rolling / hovering / clawed
    // robots use. Previous code here used the fixed 16-tile `_80`
    // variant, which isn't what the turret actually calls.
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

    // &27a3-&27af: if the shot would go behind the turret, don't fire —
    // flip to face the target instead and wait for next frame. This is
    // the 6502's "rotate to face player" behaviour: turrets pivot one
    // frame, shoot the next. `vector_x == 0` is treated as right-facing
    // (matches the 6502's BMI/BPL split).
    bool facing_left = obj.is_flipped_h();
    bool want_left   = (aim_vx < 0);
    if (facing_left != want_left) {
        // &3136 flip_this_object_horizontally
        uint8_t before_flip = obj.flags & ObjectFlags::FLIP_HORIZONTAL;
        obj.flags ^= ObjectFlags::FLIP_HORIZONTAL;
        log_flip_if_changed(obj, ctx, before_flip);
        return;
    }

    // &4ed8-&4edb: bullet type lives in the tertiary data byte itself.
    //   LSR A                ; carry = .......1 (inactive flag)
    //   BCS leave            ; (handled at top of routine)
    //   TAX                  ; X = data >> 1 = bullet object type
    // So data = (bullet_type << 1) | inactive_bit. Standard turret data
    // bytes are 0x26 (ICER), 0x28 (TRACER), 0x30 (PISTOL); the low bit
    // marks inactive.
    //
    // The previous port read obj.state and decoded bits (& 0x0e / & 0x06)
    // which doesn't match the 6502 at all — turrets ended up always
    // firing PISTOL_BULLET regardless of which variant the tile was.
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

    // Roll along ground: when velocity_x has decayed to 0 (either from
    // collision bounce_reflect damping or inertia decay), resume in the
    // direction the robot is currently FACING. face_movement_direction
    // updates is_flipped_h from velocity_x sign on the frame the robot
    // was last actually moving, so the facing reliably records "which
    // way was I going last". Continuing in that direction keeps a
    // stationary-post-bounce robot moving AWAY from whatever it just
    // hit, rather than immediately rolling back into it.
    //
    // Previous version had this ternary inverted — facing left mapped
    // to velocity_x = +4 — which made the robot reverse direction every
    // time its velocity decayed, producing the 1-to-5-frame tight
    // oscillation visible in the lifecycle log (every flip alternates
    // between vx=+4 and vx=-4). Matches the 6502's effect from
    // update_walking_npc_and_check_for_obstacles (&3adf), which turns
    // walking NPCs at obstacles only when they actually hit one.
    //
    // A fresh NEWLY_CREATED robot spawns with is_flipped_h == false, so
    // its first step is +4 (roll right) — deterministic initial
    // direction.
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

    // Fire at player. LOS-gated — 6502 routes through find_a_target_and_
    // fire_at_it → find_object (&3c2a, carry clear) which runs LOS per
    // fire attempt with a randomised cap (&3cb5 AND #&4f / EOR NOD).
    // has_line_of_sight_randomized reproduces that cap faithfully; NOD
    // defaults to 0xff to match find_object's initial state for a
    // single-candidate (player-only) target pool.
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

// &4EE2: Blue rolling robot - more aggressive, uses NPC walking
void update_blue_rolling_robot(Object& obj, UpdateContext& ctx) {
    NPC::enforce_minimum_energy(obj, 0x46);

    Mood::update_mood(obj, ctx);

    // Blue rolling robot actively seeks player
    if (obj.energy >= 0x80) {
        NPC::seek_player(obj, ctx.mgr.player(), 4);
    }

    // &4ef1: 1-in-4 gated flip (shared path with magenta/red rolling robot).
    {
        uint8_t before_flip = obj.flags & ObjectFlags::FLIP_HORIZONTAL;
        NPC::consider_face_movement_direction(obj, ctx.rng);
        log_flip_if_changed(obj, ctx, before_flip);
    }

    // Fire tracer bullets. LOS-gated — see rolling-robot comment above;
    // same 6502 find_object-driven randomised cap.
    if (ctx.every_sixteen_frames && obj.energy >= 0x80 &&
        NPC::has_line_of_sight_randomized(obj, /*target_slot=*/0, ctx)) {
        const Object& player = ctx.mgr.player();
        int8_t dx = static_cast<int8_t>(player.x.whole - obj.x.whole);
        if (std::abs(dx) < 16) {
            int slot = NPC::fire_projectile(obj, ObjectType::TRACER_BULLET, ctx);
            if (slot >= 0) {
                Object& b = ctx.mgr.object(slot);
                b.velocity_x = (dx > 0) ? 0x18 : -0x18;
                b.velocity_y = 0;
                NPC::offset_child_from_parent(b, obj);
                b.timer = 96;
            }
        }
    }
}

// &4804: Hovering robot - flies, patrols, fires
void update_hovering_robot(Object& obj, UpdateContext& ctx) {
    NPC::cancel_gravity(obj);
    NPC::enforce_minimum_energy(obj, 0x81);

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

    // LOS-aware targeting. Once every 16 frames update_target_directness
    // raycasts to the player and updates obj.target_and_flags + obj.tx/ty.
    // Clawed robots now only pursue / fire when the player is in sight
    // (directness >= 2); otherwise they wander — port of the &4714
    // `can_see_or_has_seen_player` gate.
    NPC::update_npc_path(obj, ctx);
    NPC::seek_player(obj, ctx.mgr.player(), 6);
    // &4877: 1-in-4 gated flip (clawed robots also share move_hovering_npc).
    {
        uint8_t before_flip = obj.flags & ObjectFlags::FLIP_HORIZONTAL;
        NPC::consider_face_movement_direction(obj, ctx.rng);
        log_flip_if_changed(obj, ctx, before_flip);
    }

    uint8_t lvl = NPC::directness_level(obj);

    // Attack: fire icer bullets. Directness level gates path choice but
    // NOT firing — in the 6502, firing goes through find_a_target_and_
    // fire_at_it (&4868) which calls find_object (&3c2a) with carry
    // clear, so every fire attempt runs its own LOS raycast. Relying on
    // directness alone bakes in a stale LOS: is_unable_to_see_target
    // (&3d1d) only drops 3→2 and then latches, so the robot would keep
    // firing through a door that closed after it first saw the player.
    // Match the 6502 by LOS-gating the fire with the randomised-cap
    // variant (&3cb5 AND #&4f / EOR NOD), not the turret's fixed
    // 16-tile direct-call variant at &359a.
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
    NPC::damage_player_if_touching(obj, ctx.mgr.player(), 15);
}

// &43E7 update_hovering_ball / &43EB update_invisible_hovering_ball.
//
// Structure:
//   1. rotate_colour_from_frame_counter (&4dd2) — cycle palette by the
//      global counter. Visible ball only; invisible skips this and
//      enters at &43EB directly.
//   2. If touching a primary that isn't another hovering ball, deal
//      3 damage (&43f4-&43f6) and play the "zap" sound.
//   3. `energy = energy & 4` (&4400-&4404). Bit 2 is the "alive"
//      marker. Any damage that clears bit 2 kills the ball next frame
//      via the main loop's energy-zero path. Keeping the AND on its
//      own means that healing paths have to explicitly set bit 2.
//   4. `DEC timer` (&4406). When the timer reaches zero, the ball
//      teleports away via set_object_as_far_away + teleport sound
//      (~256 frames lifespan).
//   5. Otherwise: move_hovering_npc (path update + 1-in-8 flip) then
//      thrust_towards_target (magnitude 0x1c, max-accel 4, 1-in-2
//      probability). Gravity cancelled, hovering-over-ground clamp
//      applied, one PARTICLE_JETPACK emitted.
//
// We port most of this faithfully; the set_object_as_far_away path
// becomes a simple "flag PENDING_REMOVAL" for now since we don't have
// the teleport-to-nest mechanism wired up yet (TODO).
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
            NPC::damage_player_if_touching(obj, ctx.mgr.player(), 3);
            // TODO: sound &13fa 33 03 85 02.
        }
    }

    // &4400-&4404: energy &= 4 — any damage that clears bit 2 leaves
    // energy zero, so the main loop's step 12 explodes it next frame.
    obj.energy = static_cast<uint8_t>(obj.energy & 0x04);

    // &4406-&4408: ttl countdown. The 6502 does `DEC timer; BNE move`
    // unconditionally — so the first decrement of a freshly-spawned ball
    // with timer=0 wraps to 0xff (non-zero, BNE branches, ball moves) and
    // the ball lives for a full 256 frames before the timer returns to 0
    // and the teleport-to-nest path fires. We can't fully reproduce
    // set_object_as_far_away yet, so tag the object PENDING_REMOVAL
    // instead (&2516 set_object_for_removal); SPAWNED_FROM_NEST on type
    // 0x1a means return_to_tertiary would bump the nest's creature count
    // back up if we had the full path.
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

// &43EB. The invisible variant enters update_hovering_ball's body
// AFTER the palette cycle (the 6502 JMPs past the rotate at &43ea),
// so it shares behaviour but never gets visibly animated. We implement
// the visibility suppression by clearing the palette bit 7 — the
// object is still there, still solid, just drawn as SPRITE_NONE.
void update_invisible_hovering_ball(Object& obj, UpdateContext& ctx) {
    update_hovering_ball(obj, ctx);
    // Skip the palette cycle that the visible variant does — keep the
    // palette at whatever init_object_from_type gave it so
    // this-object-visibility bit 7 stays clear.
    obj.palette = object_types_palette_and_pickup[
        static_cast<uint8_t>(ObjectType::INVISIBLE_HOVERING_BALL)] & 0x7f;
}

} // namespace Behaviors
