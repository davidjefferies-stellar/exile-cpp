#include "game/game.h"
#include "objects/held_object.h"
#include "objects/weapon.h"
#include "behaviours/npc_helpers.h"
#include "rendering/sprite_atlas.h"
#include <algorithm>

// Port of &34b4 store_object. Pocket the currently-held primary; drain
// it into the jetpack instead if it's a power pod. Returns true if the
// held slot was consumed. No-op if nothing is held, the sprite is too
// tall (>= 8 rows, &34c4) to pocket, or all 5 pockets are already full.
bool Game::try_store_held(Object& player) {
    if (held_object_slot_ >= 0x80) return false;
    int slot = held_object_slot_;
    Object& held = object_mgr_.object(slot);
    uint8_t sprite_id = held.sprite;
    if (sprite_id > 0x7c || sprite_atlas[sprite_id].h >= 8) return false;
    uint8_t ot = static_cast<uint8_t>(held.type);
    if (ot == static_cast<uint8_t>(ObjectType::POWER_POD)) {
        // &34cd not_power_pod: power pods feed the jetpack, not a pocket.
        uint32_t e = static_cast<uint32_t>(weapon_energy_[0]) + 0x800u;
        weapon_energy_[0] = (e > 0xFFFFu) ? 0xFFFFu
                                          : static_cast<uint16_t>(e);
    } else if (pockets_used_ < 5) {
        for (int i = 4; i > 0; i--) pockets_[i] = pockets_[i - 1];
        pockets_[0] = ot;
        pockets_used_++;
    } else {
        return false;  // Pockets full — keep holding.
    }
    HeldObject::drop(held, player, held_object_slot_);
    object_mgr_.remove_object(slot);
    return true;
}

// Input-driven half of the original's update_player (&37xx…). Emits the
// frame's acceleration vector (which integrate_player_motion will feed
// into the physics chain) and handles all discrete actions: weapon fire,
// pickup/drop, pocket store/retrieve, aim, whistle, weapon select.
void Game::apply_player_input(Object& player, const InputState& inp,
                              int8_t& accel_x, int8_t& accel_y) {
    accel_x = 0;
    accel_y = 0;

    // Tab: turn the player around (port of &1e19 handle_swapping_
    // direction — the 6502 action table's &17 entry). Edge-triggered so
    // holding Tab doesn't spin the facing every frame.
    {
        bool down = inp.turn_around;
        if (down && !turn_around_key_prev_) {
            player.flags ^= ObjectFlags::FLIP_HORIZONTAL;
        }
        turn_around_key_prev_ = down;
    }

    // Left Ctrl: toggle lying down (port of &2c7a handle_lying_down, the
    // 6502's &16 action). Edge-triggered.
    {
        bool down = inp.lie_down;
        if (down && !lie_down_prev_) {
            player_lying_down_ = !player_lying_down_;
        }
        lie_down_prev_ = down;
    }

    // Right Ctrl: jetpack booster (port of &2c81 handle_using_booster,
    // the 6502's &15 action). Held-key — while down, acceleration gets
    // a multiplier; the 6502 uses it in the jumping and jetpack-thrust
    // paths to increase max velocity (&3ba1 LDA #&f0 / &3ba3 ADC weight).
    const int accel_scale = inp.boost ? 2 : 1;

    if (inp.move_left)  accel_x = static_cast<int8_t>(-4 * accel_scale);
    if (inp.move_right) accel_x = static_cast<int8_t>( 4 * accel_scale);

    if (inp.jetpack || inp.move_up) {
        accel_y = static_cast<int8_t>(-6 * accel_scale); // Thrust upward
    }
    if (inp.move_down) {
        accel_y = static_cast<int8_t>(2 * accel_scale);
    }

    // Lying down disables normal walking acceleration (the 6502 clears
    // the walking state and lets gravity take over).
    if (player_lying_down_) {
        accel_x = 0;
        if (accel_y < 0) accel_y = 0;  // can't stand up mid-jump
    }

    // Port of &1f3d add_jetpack_thrust_particles: emit one jetpack
    // particle per frame while the player is accelerating.
    if (accel_x != 0 || accel_y != 0) {
        particles_.emit(ParticleType::JETPACK, 1, player, rng_);
    }

    // Whistle playing (port of &2c99 and &2cac)
    // whistle_one_active_ is cleared at start of each frame in update_timers()
    if (inp.whistle_one && whistle_one_collected_) {
        whistle_one_active_ = true;
    }
    if (inp.whistle_two && whistle_two_collected_) {
        whistle_two_activator_ = 0; // Player (slot 0) played whistle two
    }

    // Aim control — port of &30fc update_player_aiming_angle + the I/K/O
    // handlers at &3120..&3129. The 6502 runs an accel → velocity → angle
    // chain; key presses nudge the acceleration. We use a simpler one-step
    // model that feels close enough at 50 fps: each key frame moves the
    // angle by a fixed step, clamped to the -0x3f..+0x3f range (±~90°).
    {
        constexpr int AIM_STEP = 2;
        int8_t angle = static_cast<int8_t>(player_aim_angle_);
        if (inp.aim_centre) {
            angle = 0;
        } else {
            // Raising aim = angle becomes more negative (points up); lowering
            // = more positive (points down). Matches the sign convention of
            // &3126 (DEC accel) / &3129 (INC accel).
            if (inp.aim_up)   angle -= AIM_STEP;
            if (inp.aim_down) angle += AIM_STEP;
        }
        if (angle >  0x3f) angle =  0x3f;
        if (angle < -0x3f) angle = -0x3f;
        player_aim_angle_ = static_cast<uint8_t>(angle);

        // &312b create_aim_particle: every aim key press emits a PARTICLE_AIM
        // travelling along the current aim vector. Spawn from the held
        // object's position when the player is carrying something (e.g. the
        // icer) so the particles trail from the weapon rather than the
        // player's head. Fall back to the player when hands are empty.
        if (inp.aim_up || inp.aim_down || inp.aim_centre) {
            const bool has_held = held_object_slot_ > 0 &&
                                  held_object_slot_ < 0x80 &&
                                  held_object_slot_ < GameConstants::PRIMARY_OBJECT_SLOTS;
            Object aim_src = has_held
                ? object_mgr_.object(held_object_slot_)
                : player;
            Weapon::get_firing_velocity(player_aim_angle_, player.is_flipped_h(),
                                        aim_src.velocity_x, aim_src.velocity_y);
            particles_.emit(ParticleType::AIM, 1, aim_src, rng_);
        }
    }

    // &2d33 handle_firing with the &2d36-&2d3b "BPL leave" branch
    // faithfully applied: firing while holding an object doesn't launch a
    // bullet — it sets `player_object_fired = held_slot` instead. Doors,
    // transporters and the RCD itself read that flag to detect "player
    // aimed the RCD at me". The flag lives for one frame; Game::run
    // clears it back to 0xff at the end of each tick.
    //
    // SPACE is `repeat = no` in the 6502 action table at &0d (line 3572
    // of the disassembly) — one press fires one bullet. Gate on the
    // 0→1 edge so holding the key doesn't spam bullets every frame
    // and drain the weapon ammo in a tenth of a second.
    bool fire_down = inp.fire;
    bool fire_edge = fire_down && !fire_key_prev_;
    fire_key_prev_ = fire_down;
    if (fire_edge) {
        if (held_object_slot_ < 0x80) {
            player_object_fired_ = held_object_slot_;
        } else {
            Weapon::fire(object_mgr_, player, player_weapon_, player_aim_angle_,
                         weapon_energy_[player_weapon_]);
        }
    }

    // Inventory actions are split across three keys:
    //   ,  pickup the touching object (no-op if nothing in reach or
    //      we're already holding something).
    //   m  drop the held object straight down (no horizontal velocity
    //      added — gravity takes it from the player's hand).
    //   .  throw the held object: same as drop but with a horizontal
    //      kick in the player's facing direction so it sails away.
    //
    // Each is rising-edge gated. Mirrors the 6502's per-key "just-
    // pressed" register at &126b — without this, holding a key would
    // trigger every frame.

    auto pickup_now = [&](void) {
        if (held_object_slot_ < 0x80) return;                  // already holding
        if (player.touching >= GameConstants::PRIMARY_OBJECT_SLOTS) return;
        Object& touched = object_mgr_.object(player.touching);
        if (!HeldObject::is_pickupable(touched.type)) return;
        HeldObject::pickup(touched, player, held_object_slot_,
                           player.touching);
        // First touch clears the collectable's undisturbed pin (port
        // of the ASL/LSR at &4ba1).
        touched.energy &= 0x7f;
    };
    auto drop_now = [&]() {
        if (held_object_slot_ >= 0x80) return;
        Object& held = object_mgr_.object(held_object_slot_);
        HeldObject::drop(held, player, held_object_slot_);
    };

    // Port of &32d9 handle_throwing_object. The throw goes in the
    // player's current aim direction (with facing flip applied) at a
    // magnitude that depends on the object's weight — lighter things
    // fly further, heavy things barely clear the player. Plus a random
    // 0-7 jitter on the magnitude. If the player is airborne, the
    // player's own velocity is folded into the throw vector (both axes
    // for vx, only when airborne for vy) so you can "throw while
    // jumping" and the object inherits momentum.
    auto throw_now = [&]() {
        if (held_object_slot_ >= 0x80) return;
        Object& held = object_mgr_.object(held_object_slot_);

        // &32d9 — calculate_firing_vector is called first for side-
        // effects (setting &b5 = angle), then immediately overridden
        // below with a weight-based magnitude. We skip the side-effect
        // call because our vector_from_magnitude_and_angle is pure.

        // &311d player_aiming_angle_with_flip: mirror the aim angle
        // across the vertical axis when facing left. Bit 7 is preserved
        // by the EOR #&7f (only bits 0-6 flip), then +1 so 0x00 → 0x80
        // exactly (right → left).
        uint8_t angle = player_aim_angle_;
        if (player.is_flipped_h()) {
            angle = static_cast<uint8_t>((angle ^ 0x7f) + 1);
        }

        // &32d2 throwing_velocities_by_weight_table: magnitude falls
        // off for heavier objects. Indexed by the held item's weight.
        static constexpr uint8_t THROW_MAG_BY_WEIGHT[7] = {
            0x20, 0x20, 0x20, 0x20, 0x20, 0x10, 0x08,
        };
        uint8_t w = held.weight();
        if (w > 6) w = 6;

        // &32e9-&32ee: random 0..7 added to base magnitude.
        uint8_t base = THROW_MAG_BY_WEIGHT[w];
        uint8_t mag  = static_cast<uint8_t>(base + (rng_.next() & 0x07));

        // &32f1 calculate_vector_from_magnitude_and_angle.
        int8_t throw_vx = 0, throw_vy = 0;
        NPC::vector_from_magnitude_and_angle(mag, angle, throw_vx, throw_vy);

        HeldObject::drop(held, player, held_object_slot_);

        // &32f4 BIT this_object_any_bottom_collision — if the player is
        // airborne, add the player's velocity_y to the throw's y so the
        // object carries the player's vertical momentum. If supported,
        // use the throw's y alone.
        int new_vy = int(throw_vy);
        if (!(player.flags & ObjectFlags::SUPPORTED)) {
            new_vy += int(player.velocity_y);
        }
        if (new_vy >  127) new_vy =  127;
        if (new_vy < -128) new_vy = -128;
        held.velocity_y = static_cast<int8_t>(new_vy);

        // &3303 — always add the player's velocity_x to the throw's vx.
        int new_vx = int(throw_vx) + int(player.velocity_x);
        if (new_vx >  127) new_vx =  127;
        if (new_vx < -128) new_vx = -128;
        held.velocity_x = static_cast<int8_t>(new_vx);

        // Throwing disturbs collectables so they don't snap back to
        // spawn mid-flight (port of the &4ba1 ASL/LSR on energy's high
        // bit, reused here for the thrown-object case).
        held.energy &= 0x7f;
    };

    // ENTER kept as a backwards-compat toggle — pickup if not holding,
    // drop (no throw) if holding. Same edge handling.
    bool pd_down = inp.pickup_drop;
    bool pd_edge = pd_down && !pickup_drop_key_prev_;
    pickup_drop_key_prev_ = pd_down;
    if (pd_edge) {
        if (held_object_slot_ < 0x80) drop_now();
        else                          pickup_now();
    }

    bool pickup_down = inp.pickup;
    bool pickup_edge = pickup_down && !pickup_key_prev_;
    pickup_key_prev_ = pickup_down;
    if (pickup_edge) pickup_now();

    bool drop_down = inp.drop;
    bool drop_edge = drop_down && !drop_key_prev_;
    drop_key_prev_ = drop_down;
    if (drop_edge) drop_now();

    bool throw_down = inp.throw_obj;
    bool throw_edge = throw_down && !throw_key_prev_;
    throw_key_prev_ = throw_down;
    if (throw_edge) throw_now();

    // Pocket store/retrieve — port of &34b4 store_object and &34f8
    // handle_retrieving_object. Store: push held object type onto pockets[0],
    // shuffling existing entries down. Retrieve: store current held first
    // (so R cycles), then pull pockets[pockets_used-1] back as a new primary
    // and hold it.
    bool store_down = inp.store;
    bool store_edge = store_down && !store_key_prev_;
    store_key_prev_ = store_down;
    if (store_edge) {
        try_store_held(player);
    }
    bool retrieve_down = inp.retrieve;
    bool retrieve_edge = retrieve_down && !retrieve_key_prev_;
    retrieve_key_prev_ = retrieve_down;
    if (retrieve_edge) {
        try_store_held(player);  // Mirror &34f8: store first so G cycles pockets.
        if (held_object_slot_ >= 0x80 && pockets_used_ > 0) {
            uint8_t ot = pockets_[pockets_used_ - 1];
            pockets_[pockets_used_ - 1] = 0xff;
            pockets_used_--;
            // Spawn the retrieved object in front of the player and grab it.
            int8_t facing_dx = player.is_flipped_h() ? -1 : 1;
            uint8_t spawn_x = static_cast<uint8_t>(player.x.whole + facing_dx);
            int new_slot = object_mgr_.create_object(
                static_cast<ObjectType>(ot), /*min_free_slots=*/1,
                spawn_x, 0, player.y.whole, 0);
            if (new_slot > 0) {
                HeldObject::pickup(object_mgr_.object(new_slot),
                                   player, held_object_slot_, new_slot);
            } else {
                // Couldn't allocate a primary slot — restore the pocket.
                pockets_[pockets_used_] = ot;
                pockets_used_++;
            }
        }
    }

    // R → handle_remembering_position (&2c3c). Records the player's
    // current tile position into the next teleport slot and rotates the
    // cursor, so pressing R up to 4 times stores 4 recall points. Also
    // increments the remembered count (capped at 4).
    bool remember_down = inp.remember_pos;
    bool remember_edge = remember_down && !remember_key_prev_;
    remember_key_prev_ = remember_down;
    if (remember_edge) {
        handle_remembering_position(player);
    }

    // T → handle_teleporting (&0cc1). Pops the most recent remembered
    // position (or the fallback at slot 4) and starts the 32-frame
    // teleport animation that step-8 of update_objects drives. The
    // method early-outs if the player is currently holding an object
    // — the 6502 forbids voluntary teleporting while holding at &0cc3.
    bool teleport_down = inp.teleport;
    bool teleport_edge = teleport_down && !teleport_key_prev_;
    teleport_key_prev_ = teleport_down;
    if (teleport_edge) {
        handle_player_teleporting(player);
    }

    // Weapon select
    if (inp.weapon_select < 6) {
        player_weapon_ = inp.weapon_select;
    }
}
