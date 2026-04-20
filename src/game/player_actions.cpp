#include "game/game.h"
#include "objects/held_object.h"
#include "objects/weapon.h"
#include "rendering/sprite_atlas.h"
#include <algorithm>

// Input-driven half of the original's update_player (&37xx…). Emits the
// frame's acceleration vector (which integrate_player_motion will feed
// into the physics chain) and handles all discrete actions: weapon fire,
// pickup/drop, pocket store/retrieve, aim, whistle, weapon select.
void Game::apply_player_input(Object& player, const InputState& inp,
                              int8_t& accel_x, int8_t& accel_y) {
    accel_x = 0;
    accel_y = 0;

    if (inp.move_left)  accel_x = -4;
    if (inp.move_right) accel_x = 4;

    if (inp.jetpack || inp.move_up) {
        accel_y = -6; // Thrust upward
    }
    if (inp.move_down) {
        accel_y = 2;
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

    // Weapon firing. Unlike "pickup/drop", firing isn't gated on holding an
    // object — the selected weapon fires its own bullets regardless.
    if (inp.fire) {
        Weapon::fire(object_mgr_, player, player_weapon_, player_aim_angle_,
                     weapon_energy_[player_weapon_]);
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
    auto drop_now = [&](int8_t throw_vx) {
        if (held_object_slot_ >= 0x80) return;
        Object& held = object_mgr_.object(held_object_slot_);
        HeldObject::drop(held, player, held_object_slot_);
        if (throw_vx != 0) {
            // Throw: hand off the player's velocity plus the kick. A
            // small upward bias (-3) gives the object enough air time
            // to clear the player's sprite before landing.
            int new_vx = int(player.velocity_x) + int(throw_vx);
            if (new_vx >  127) new_vx =  127;
            if (new_vx < -128) new_vx = -128;
            held.velocity_x = static_cast<int8_t>(new_vx);
            held.velocity_y = static_cast<int8_t>(
                std::max(-30, int(player.velocity_y) - 3));
            // Throwing also disturbs collectables so they don't snap
            // back to the spawn position mid-flight.
            held.energy &= 0x7f;
        }
    };

    // ENTER kept as a backwards-compat toggle — pickup if not holding,
    // drop (no throw) if holding. Same edge handling.
    bool pd_down = inp.pickup_drop;
    bool pd_edge = pd_down && !pickup_drop_key_prev_;
    pickup_drop_key_prev_ = pd_down;
    if (pd_edge) {
        if (held_object_slot_ < 0x80) drop_now(0);
        else                          pickup_now();
    }

    bool pickup_down = inp.pickup;
    bool pickup_edge = pickup_down && !pickup_key_prev_;
    pickup_key_prev_ = pickup_down;
    if (pickup_edge) pickup_now();

    bool drop_down = inp.drop;
    bool drop_edge = drop_down && !drop_key_prev_;
    drop_key_prev_ = drop_down;
    if (drop_edge) drop_now(0);

    bool throw_down = inp.throw_obj;
    bool throw_edge = throw_down && !throw_key_prev_;
    throw_key_prev_ = throw_down;
    if (throw_edge) {
        // Throw kick is signed in the player's facing direction. 0x40
        // is roughly four pixels per frame in our fixed-point velocity
        // units — fast enough to carry the object a few tiles before
        // friction / collision stop it.
        int8_t kick = player.is_flipped_h() ? -0x40 : 0x40;
        drop_now(kick);
    }

    // Pocket store/retrieve — port of &34b4 store_object and &34f8
    // handle_retrieving_object. Store: push held object type onto pockets[0],
    // shuffling existing entries down. Retrieve: store current held first
    // (so R cycles), then pull pockets[pockets_used-1] back as a new primary
    // and hold it.
    auto try_store_held = [&]() -> bool {
        if (held_object_slot_ >= 0x80) return false;
        int slot = held_object_slot_;
        Object& held = object_mgr_.object(slot);
        // &34c4: objects with sprite height >= 8 rows can't be pocketed.
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
    };

    bool store_down = inp.store;
    bool store_edge = store_down && !store_key_prev_;
    store_key_prev_ = store_down;
    if (store_edge) {
        try_store_held();
    }
    bool retrieve_down = inp.retrieve;
    bool retrieve_edge = retrieve_down && !retrieve_key_prev_;
    retrieve_key_prev_ = retrieve_down;
    if (retrieve_edge) {
        try_store_held();  // Mirror &34f8: store first so R cycles pockets.
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

    // Weapon select
    if (inp.weapon_select < 6) {
        player_weapon_ = inp.weapon_select;
    }
}
