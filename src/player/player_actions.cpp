#include "game/game.h"
#include "objects/held_object.h"
#include "objects/weapon.h"
#include "behaviours/npc_helpers.h"
#include "audio/audio.h"
#include "rendering/sprite_atlas.h"

// Port of &34b4 store_object. Pocket the currently-held primary; drain
// it into the jetpack instead if it's a power pod. Returns true if the
// held slot was consumed. No-op if nothing is held, the sprite is too
// tall (>= 8 rows, &34c4) to pocket, or all 5 pockets are already full.
bool Game::try_store_held(Object& player, bool drain_power_pod) {
    if (held_object_slot_ >= 0x80) return false;
    int slot = held_object_slot_;
    Object& held = object_mgr_.object(slot);
    uint8_t sprite_id = held.sprite;
    if (sprite_id > 0x80 || sprite_atlas[sprite_id].h >= 8) return false;
    uint8_t ot = static_cast<uint8_t>(held.type);
    if (drain_power_pod && ot == static_cast<uint8_t>(ObjectType::POWER_POD)) {
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

// Port of the "," pickup branch. Fresh AABB scan inflated by ±0x40
// frac-units; falls back to player.touching. The 6502 reads &3b set
// earlier this frame; our port's player.touching is set at end-of-
// motion last frame — can be stale if the player passes a narrow AABB
// fast.
void Game::pickup_touching(Object& player) {
    if (held_object_slot_ < 0x80) return;                  // already holding

    int p_x = static_cast<int>(player.x.whole) * 256 +
              static_cast<int>(player.x.fraction);
    int p_y = static_cast<int>(player.y.whole) * 256 +
              static_cast<int>(player.y.fraction);
    int p_w = (player.sprite <= 0x80 && sprite_atlas[player.sprite].w > 0)
        ? (sprite_atlas[player.sprite].w - 1) * 16 : 0;
    int p_h = (player.sprite <= 0x80 && sprite_atlas[player.sprite].h > 0)
        ? (sprite_atlas[player.sprite].h - 1) * 8 : 0;
    // Inflate ~half a tile so the user can pick up an item they're
    // standing right next to without pixel-hunting.
    constexpr int kInflate = 0x40;
    int p_x_lo = p_x - kInflate;
    int p_x_hi = p_x + p_w + kInflate;
    int p_y_lo = p_y - kInflate;
    int p_y_hi = p_y + p_h + kInflate;

    int best_slot = -1;
    int best_dist = 0x7fffffff;
    for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        const Object& cand = object_mgr_.object(i);
        if (!cand.is_active()) continue;
        if (!HeldObject::is_pickupable(cand.type)) continue;
        int o_x = static_cast<int>(cand.x.whole) * 256 +
                  static_cast<int>(cand.x.fraction);
        int o_y = static_cast<int>(cand.y.whole) * 256 +
                  static_cast<int>(cand.y.fraction);
        int o_w = (cand.sprite <= 0x80 && sprite_atlas[cand.sprite].w > 0)
            ? (sprite_atlas[cand.sprite].w - 1) * 16 : 0;
        int o_h = (cand.sprite <= 0x80 && sprite_atlas[cand.sprite].h > 0)
            ? (sprite_atlas[cand.sprite].h - 1) * 8 : 0;
        if (o_x + o_w <= p_x_lo) continue;
        if (p_x_hi   <= o_x)     continue;
        if (o_y + o_h <= p_y_lo) continue;
        if (p_y_hi   <= o_y)     continue;
        // Pick the closest by centre-to-centre distance so a crowded
        // floor doesn't grab a far item over a near one.
        int o_cx = o_x + o_w / 2;
        int o_cy = o_y + o_h / 2;
        int p_cx = p_x + p_w / 2;
        int p_cy = p_y + p_h / 2;
        int dx = o_cx - p_cx;
        int dy = o_cy - p_cy;
        int d  = dx * dx + dy * dy;
        if (d < best_dist) { best_dist = d; best_slot = i; }
    }
    int target_slot = best_slot;
    if (target_slot < 0 &&
        player.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
        Object& cand = object_mgr_.object(player.touching);
        if (cand.is_active() && HeldObject::is_pickupable(cand.type)) {
            target_slot = player.touching;
        }
    }
    if (target_slot < 0) return;

    Object& touched = object_mgr_.object(target_slot);
    HeldObject::pickup(touched, player, held_object_slot_,
                       static_cast<uint8_t>(target_slot));
    // First touch clears the collectable's undisturbed pin (port of
    // the ASL/LSR at &4ba1).
    touched.energy &= 0x7f;
}

// Port of the "m" branch (and the ENTER-while-holding path). Release
// the currently-held primary straight down — no horizontal kick.
void Game::drop_in_place(Object& player) {
    if (held_object_slot_ >= 0x80) return;
    Object& held = object_mgr_.object(held_object_slot_);
    HeldObject::drop(held, player, held_object_slot_);
}

// Port of &32d9 handle_throwing_object. The throw goes in the player's
// current aim direction (with facing flip applied) at a magnitude that
// depends on the object's weight — lighter things fly further, heavy
// things barely clear the player. Plus a random 0-7 jitter on the
// magnitude. If the player is airborne, the player's own velocity is
// folded into the throw vector (both axes for vx, only when airborne
// for vy) so you can "throw while jumping" and the object inherits
// momentum.
void Game::throw_held(Object& player) {
    if (held_object_slot_ >= 0x80) return;
    Object& held = object_mgr_.object(held_object_slot_);

    // &32d9 — calculate_firing_vector is called first for side-effects
    // (setting &b5 = angle), then immediately overridden below with a
    // weight-based magnitude. We skip the side-effect call because our
    // vector_from_magnitude_and_angle is pure.

    // &311d player_aiming_angle_with_flip: mirror the aim angle across
    // the vertical axis when facing left. Bit 7 is preserved by the
    // EOR #&7f (only bits 0-6 flip), then +1 so 0x00 -> 0x80 exactly
    // (right -> left).
    uint8_t angle = player_aim_angle_;
    if (player.is_flipped_h()) {
        angle = static_cast<uint8_t>((angle ^ 0x7f) + 1);
    }

    // &32d2 throwing_velocities_by_weight_table: magnitude falls off
    // for heavier objects. Indexed by the held item's weight.
    static constexpr uint8_t THROW_MAG_BY_WEIGHT[7] = {
        0x20, 0x20, 0x20, 0x20, 0x20, 0x10, 0x08,
    };
    uint8_t w = held.weight();
    if (w > 6) w = 6;

    // &32e9-&32ee: random 0..7 + base + carry-out of the rnd routine
    // (AND #&07 preserves C between the rnd JSR and the ADC).
    uint8_t base = THROW_MAG_BY_WEIGHT[w];
    uint8_t r    = rng_.next();
    uint8_t mag  = static_cast<uint8_t>(base + (r & 0x07) + rng_.last_carry());

    // &32f1 calculate_vector_from_magnitude_and_angle.
    int8_t throw_vx = 0, throw_vy = 0;
    NPC::vector_from_magnitude_and_angle(mag, angle, throw_vx, throw_vy);

    HeldObject::drop(held, player, held_object_slot_);

    // &32f4 BIT this_object_any_bottom_collision — if the player is
    // airborne, add the player's velocity_y to the throw's y so the
    // object carries the player's vertical momentum. If supported, use
    // the throw's y alone.
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

    // Throwing disturbs collectables so they don't snap back to spawn
    // mid-flight (port of the &4ba1 ASL/LSR on energy's high bit,
    // reused here for the thrown-object case).
    held.energy &= 0x7f;
}

// Input-driven half of the original's update_player (&37xx…). Emits the
// frame's acceleration vector (which integrate_player_motion will feed
// into the physics chain) and handles all discrete actions: weapon fire,
// pickup/drop, pocket store/retrieve, aim, whistle, weapon select.
void Game::apply_player_input(Object& player, const InputState& inp_in,
                              int8_t& accel_x, int8_t& accel_y) {
    accel_x = 0;
    accel_y = 0;

    // &3810-&3856 update_rotating_player. Movement-immobility timer is
    // set by &24b7 in damage_object; while > 0, the 6502 suppresses
    // movement / thrust / jetpack AND rotates player_angle (the
    // "knocked spinning" visual). Faithful port of the rotation chain.
    bool immobile = player_immobility_movement_ > 0;
    if (immobile) {
        player_immobility_movement_--;

        // Pick the rotation base. Three cases, see &381b-&3827:
        //   tile_collision  → use the captured pre_collision_angle
        //                     (which direction we were moving when we
        //                     hit the floor / ceiling).
        //   else touching   → 0x40 (180° / 2, pivot off the object).
        //   else            → no new step, just decay the existing rv.
        bool new_step = true;
        uint8_t base  = 0x40;
        if (player.tile_collision) {
            base = player.pre_collision_angle;
        } else if (player.touching >= GameConstants::PRIMARY_OBJECT_SLOTS) {
            // Not touching anything — fall through to the every-4-frame
            // decay only, no new acceleration into rotation_velocity.
            new_step = false;
        }

        if (new_step) {
            // &3829-&382d: bit 7 of (ROR after SBC) encodes which way
            // to spin. We replicate by computing the 6502's exact
            // SBC + ROR rather than re-deriving in C — the wrap-around
            // semantics matter on the boundary.
            uint8_t base2 = static_cast<uint8_t>(base << 1);
            uint8_t pa2   = static_cast<uint8_t>(player_angle_ << 1);
            bool no_borrow = base2 >= pa2;
            uint8_t sub    = static_cast<uint8_t>(base2 - pa2);
            uint8_t rored  = static_cast<uint8_t>(
                (sub >> 1) | (no_borrow ? 0x80 : 0));
            bool sign_neg  = (rored & 0x80) != 0;

            // &382f-&3833: speed = (pre_collision_magnitude / 4) | 1.
            uint8_t speed_u = static_cast<uint8_t>(
                (player.pre_collision_magnitude >> 2) | 0x01);
            int8_t  speed   = static_cast<int8_t>(speed_u);
            if (sign_neg) speed = static_cast<int8_t>(-speed);

            // &3839-&383d: rotation_velocity += speed, clamp to ±0x20.
            int rv = static_cast<int8_t>(
                         player_immobility_rotation_velocity_) + speed;
            if (rv >  0x20) rv =  0x20;
            if (rv < -0x20) rv = -0x20;
            player_immobility_rotation_velocity_ =
                static_cast<uint8_t>(rv & 0xff);
        }

        // &3840-&384c: every 4 frames, if |rv| >= 4, decay 7/8.
        if (every_four_frames_) {
            int8_t rv_s = static_cast<int8_t>(
                player_immobility_rotation_velocity_);
            if (rv_s >= 4 || rv_s <= -4) {
                int rv = rv_s;
                rv = rv - (rv >> 3);  // calculate_seven_eighths
                player_immobility_rotation_velocity_ =
                    static_cast<uint8_t>(rv & 0xff);
            }
        }

        // &3852: player_angle += rotation_velocity (wraps mod 256).
        player_angle_ = static_cast<uint8_t>(
            player_angle_ + player_immobility_rotation_velocity_);
    } else {
        // Not immobilised: damp the residual rotation so a tiny leftover
        // doesn't drift the angle once the spin stops. The 6502 doesn't
        // need this — its skip_immobility_rotating path overwrites
        // player_angle with the upright / lying-down target.
        player_immobility_rotation_velocity_ = 0;
    }
    if (player_immobility_thrust_ > 0) player_immobility_thrust_--;

    // &3798-&37a2 every-16-frames energy regen. &37a0 BCS skip preserves
    // the value on overflow rather than wrapping past 0xff.
    if (every_sixteen_frames_) {
        int e = int(player.energy) + 4;
        if (e <= 0xff) player.energy = static_cast<uint8_t>(e);
    }

    // &37ac-&37b2 damage_immobility_set. Effective rule: if energy <
    // 0x10, force movement immobility to (0x10 - energy). Re-arms the
    // continuous wobble + jetpack cut-out below the damage threshold.
    if (player.energy < 0x10) {
        player_immobility_movement_ =
            static_cast<uint8_t>(0x10 - player.energy);
    }

    // Build a working copy of the input with movement / thrust masked
    // off while immobilised. lie_down also clears (per &24b1 LSR &31).
    InputState inp = inp_in;
    if (immobile) {
        inp.move_left = false;
        inp.move_right = false;
        inp.move_up = false;
        inp.move_down = false;
        inp.boost = false;
        inp.lie_down = false;
        player_lying_down_ = false;
    }
    // &37b6 CMP #&06 / BCS LSR &358a — at high immobility the jetpack
    // is disabled outright. Mirror by clearing the jetpack-active flag.
    if (player_immobility_movement_ >= 0x06 ||
        player_immobility_thrust_ > 0) {
        jetpack_active_ = false;
    }

    // Tab: turn around (&1e19 handle_swapping_direction). Edge-triggered.
    // Toggle player_facing_ — update_player_sprite rewrites the flag from
    // it later this frame, so a direct flag toggle gets clobbered. 6502's
    // player_object_x_flip (&38) is the same single source of truth.
    {
        bool down = inp.turn_around;
        if (down && !turn_around_key_prev_) {
            player_facing_ ^= 0x80;
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
    // &2c84 gates the boost on player_weapons_collected[0] (&080e).
    bool boost_active = inp.boost && (player_weapons_collected_[0] & 0x80);
    const int accel_scale = boost_active ? 2 : 1;

    // &2c7a set_object_jumping_or_flying triggers on jetpack/up-thrust or
    // booster+horizontal, setting state low nibble to 0x0f so &3b0b sees
    // "not walking" and skips the walk branch. Down-input also skips
    // walking — let thrust handle vertical motion.
    bool flying =
        inp.jetpack || inp.move_up || inp.move_down ||
        (boost_active && (inp.move_left || inp.move_right));

    // &2c6a-&2c73 input handlers INC/DEC accel by ±1; &3795 doubles
    // every frame; &2c81 booster fall-through doubles again → ±2 / ±4.
    if (inp.jetpack || inp.move_up) {
        accel_y = static_cast<int8_t>(-2 * accel_scale);
    }
    if (inp.move_down) {
        accel_y = static_cast<int8_t>( 2 * accel_scale);
    }

    // Set facing from input BEFORE the walking branch overwrites accel_x.
    // 6502 stores facing at &38b0-&38b7 from input-driven accel_x; the
    // &3b53 walking overwrite (slope vector / braking) lands AFTER so
    // facing isn't flipped on deceleration.
    if (inp.move_right)      player_facing_ = 0x00;
    else if (inp.move_left)  player_facing_ = 0x80;

    // Port of &3b25 walk_along_flat_or_shallow_slope:
    //   diff  = sign(dir) * 0x1f - velocity_x
    //   accel = clamp(diff >> weight, ±0x10) — converges in ~2 frames.
    // Walking gate (&3b0e-&3b10): state low nibble (NPC_WALKING_MASK)
    // must be 0; reset on bottom collision, otherwise incremented.
    uint8_t walk_counter = player.state & 0x0f;
    bool walking = (walk_counter == 0) && !flying;
    bool supported = (player.flags & ObjectFlags::SUPPORTED) != 0;
    bool wants_walk = inp.move_left || inp.move_right;

    // State-change log — only fires when one of the gate inputs flips
    // so the log shows the timeline of grounded->airborne, walk->fly etc.
    bool any_change =
        supported    != walk_log_supported_prev_ ||
        walk_counter != walk_log_counter_prev_   ||
        walking      != walk_log_walking_prev_   ||
        flying       != walk_log_flying_prev_;
    if (debug_log_.is_open() && any_change) {
        char line[260];
        std::snprintf(line, sizeof(line),
            "walk-state %u sup=%d->%d ctr=%x->%x walk=%d->%d fly=%d->%d "
            "tcA=%02x body_ang=%02x sprite=%02x flags=%02x v=(%+d,%+d) "
            "wants_walk=%d inp(L=%d R=%d U=%d D=%d J=%d B=%d)\n",
            static_cast<unsigned>(frame_counter_),
            walk_log_supported_prev_ ? 1 : 0, supported ? 1 : 0,
            walk_log_counter_prev_, walk_counter,
            walk_log_walking_prev_ ? 1 : 0, walking ? 1 : 0,
            walk_log_flying_prev_ ? 1 : 0, flying ? 1 : 0,
            player_tile_collision_angle_, player_angle_,
            player.sprite, player.flags,
            static_cast<int>(player.velocity_x),
            static_cast<int>(player.velocity_y),
            wants_walk ? 1 : 0,
            inp.move_left ? 1 : 0, inp.move_right ? 1 : 0,
            inp.move_up ? 1 : 0, inp.move_down ? 1 : 0,
            inp.jetpack ? 1 : 0, inp.boost ? 1 : 0);
        debug_log_ << line;
        debug_log_.flush();
    }

    // Walk-blocked diagnostic — fires once per frame the player presses
    // left/right but doesn't get the walk branch. Shows *why*: counter,
    // SUPPORTED, the flying-condition inputs, the tile-angle and the
    // walking gate's expectations side-by-side.
    if (debug_log_.is_open() && wants_walk && !walking) {
        const char* reason =
            !supported          ? "not-supported" :
            walk_counter != 0   ? "counter-nonzero" :
            flying              ? "flying-flag" :
                                  "unknown";
        char line[220];
        std::snprintf(line, sizeof(line),
            "walk-blocked %u reason=%s sup=%d ctr=%x fly(jp=%d up=%d "
            "dn=%d boost+lr=%d) tcA=%02x sprite=%02x v=(%+d,%+d) "
            "pos=(%02x.%02x,%02x.%02x)\n",
            static_cast<unsigned>(frame_counter_),
            reason, supported ? 1 : 0, walk_counter,
            inp.jetpack ? 1 : 0, inp.move_up ? 1 : 0,
            inp.move_down ? 1 : 0,
            (inp.boost && (inp.move_left || inp.move_right)) ? 1 : 0,
            player_tile_collision_angle_, player.sprite,
            static_cast<int>(player.velocity_x),
            static_cast<int>(player.velocity_y),
            player.x.whole, player.x.fraction,
            player.y.whole, player.y.fraction);
        debug_log_ << line;
        debug_log_.flush();
    }

    walk_log_supported_prev_ = supported;
    walk_log_counter_prev_   = walk_counter;
    walk_log_walking_prev_   = walking;
    walk_log_flying_prev_    = flying;
    if (walking) {
    // &3b25 walk: angle = tcA + (right? 0x10 : 0x6f), then &2357 emits
    // (accel_x, accel_y). On flat ground (tcA=0) this is "horizontal +
    // 22.5° down" to keep the player into the floor; on slopes the angle
    // rotates with tcA so accel runs along the slope tangent.
        // &3abd-&3ac4 max_accel for player_weight=3: LDA #&0f; LSR once;
        // ADC #&01 with C=1 from LSR -> 0x09 every frame.
        constexpr int walking_speed = 0x1f;
        constexpr int max_accel     = 0x09;
        constexpr int player_weight = 3;
        int target_vx = 0;
        if (inp.move_left)  target_vx = -walking_speed;
        if (inp.move_right) target_vx = +walking_speed;
        int diff = target_vx - static_cast<int>(player.velocity_x);
        int sign = (diff < 0) ? -1 : 1;
        int abs_diff = (diff < 0) ? -diff : diff;
        // &3208-&320c: LSR (weight+1) times, ROL once -> divide by 2^weight.
        abs_diff >>= player_weight;
        if (abs_diff > max_accel) abs_diff = max_accel;
        int8_t signed_accel = static_cast<int8_t>(sign * abs_diff);

        // &3b31-&3b37 moving-left-vs-surface flag. bit 7 of
        //   ((sign_bit(accel) XOR tcA) + 0x40)
        // tells whether the player is moving "left along the surface
        // normal frame" — flips for ceilings and steep slopes.
        uint8_t accel_sign_bit = (signed_accel < 0) ? 0x80 : 0x00;
        uint8_t mixed = static_cast<uint8_t>(accel_sign_bit ^ player_tile_collision_angle_);
        uint8_t shifted = static_cast<uint8_t>(mixed + 0x40);
        bool moving_left_vs_surface = (shifted & 0x80) != 0;

        // &3b3e-&3b44: angle_base = 0x10 (moving right) or 0x6f (left)
        //   then ADC tcA (carry from ASL A above is bit 7 of `shifted`,
        //   which is the same as moving_left_vs_surface). The ADC's
        //   carry adds 1 when moving_left_vs_surface — we replicate.
        uint8_t angle_base = moving_left_vs_surface ? 0x6f : 0x10;
        uint8_t angle = static_cast<uint8_t>(angle_base + player_tile_collision_angle_ +
                                             (moving_left_vs_surface ? 1 : 0));

        // &3b3a-&3b3c LDY #&00: when relative_tx is zero (no input),
        // the magnitude is forced to 0 — so the player still gets the
        // correct slope-aware angle but no accel applied. Slip-down-
        // slope behaviour comes from elsewhere (collision response,
        // future Step 3) — not from this branch when input is absent.
        bool no_input = !inp.move_left && !inp.move_right;
        uint8_t magnitude = no_input ? 0 : static_cast<uint8_t>(abs_diff);

        int8_t out_vx = 0, out_vy = 0;
        NPC::vector_from_magnitude_and_angle(magnitude, angle, out_vx, out_vy);
        accel_x = out_vx;
        accel_y = out_vy;

        // &3b55/&3b58 dampen_velocity_y twice — vy *= 7/8 squared. Keeps
        // slope transitions / post-jump walks from carrying stray vy.
        for (int i = 0; i < 2; i++) {
            int v = player.velocity_y;
            int abs_v = v < 0 ? -v : v;
            int eighth = (abs_v + 7) / 8;
            player.velocity_y = static_cast<int8_t>(
                v < 0 ? v + eighth : v - eighth);
        }
    } else {
        // Airborne horizontal — same ±2 / ±4 magnitudes as thrust;
        // walking branch above handles the on-the-ground case.
        if (inp.move_left)  accel_x = static_cast<int8_t>(-2 * accel_scale);
        if (inp.move_right) accel_x = static_cast<int8_t>( 2 * accel_scale);
    }

    // Lying down doesn't disable horizontal acceleration on the 6502 —
    // &37c6 clears player_is_lying_down at the top of every frame and
    // &37dc-&37e4 only re-sets it if the player isn't accelerating. So
    // pressing left/right while lying makes the player stand up and walk.
    // We mirror that here: any horizontal input cancels the toggle so the
    // sprite stands and the walking branch above stays in effect.
    if (player_lying_down_ && (inp.move_left || inp.move_right)) {
        player_lying_down_ = false;
    }

    // Port of &1f3d add_jetpack_thrust_particles: emit one jetpack
    // particle per frame while the player is accelerating. [debug]
    // jetpack_boost_tint pins colour to red/magenta during boost so the
    // 2x accel path is visible — cf_base=0xe1 (red, no CYCLE), cf_rand
    // =0x04 (only bit 2 flips, so result is colour 1 or 5).
    if (accel_x != 0 || accel_y != 0) {
        if (jetpack_boost_tint_ && boost_active) {
            particles_.emit(ParticleType::JETPACK, 1, player, cosmetic_rng_,
                            /*angle=*/0xc0, /*cf_base=*/0xe1, /*cf_rand=*/0x04);
        } else {
            particles_.emit(ParticleType::JETPACK, 1, player, cosmetic_rng_);
        }
    }

    // Whistles: &2cac whistle_one (low note only) and &2c99 whistle_two
    // (high note at &2c9e + shared low at &2cb4 -> two-tone "tweet"). Both
    // gated on "_collected" flags. Whistle two also stamps whistle_two_
    // activating_object = player slot.
    static constexpr uint8_t kSoundWhistleHigh[4] = { 0xb0, 0x24, 0xb6, 0xe2 };
    static constexpr uint8_t kSoundWhistleLow[4]  = { 0xb0, 0x24, 0xb6, 0xb3 };
    if (inp.whistle_one && whistle_one_collected_) {
        whistle_one_active_ = true;
        Audio::play(Audio::CH_ANY, kSoundWhistleLow);
        object_mgr_.log_diag("whistle1 played f=%u w1_active_=%d",
                             (unsigned)frame_counter_,
                             (int)whistle_one_active_);
    }
    if (inp.whistle_two && whistle_two_collected_) {
        whistle_two_activator_ = 0; // Player (slot 0) played whistle two
        Audio::play(Audio::CH_ANY, kSoundWhistleHigh);
        Audio::play(Audio::CH_ANY, kSoundWhistleLow);
        object_mgr_.log_diag("whistle2 played f=%u w2_activator_=0x%02x",
                             (unsigned)frame_counter_,
                             (unsigned)whistle_two_activator_);
    }

    // Aim control — port of &30fc update_player_aiming_angle + the
    // I/K/O action handlers at &3120-&3129.
    //   &3126 raise:  accel--
    //   &3129 lower:  accel++
    //   &3120 centre: angle = 0; velocity = 0
    //   &30fc per-frame: if accel==0, velocity = 0 (BEQ skip path falls
    //                    through into STA velocity with A=0); else
    //                    velocity = clamp(velocity + accel, ±0x10).
    //                    angle = clamp(angle + velocity, ±0x3f).
    // Our input model exposes K / O as single-frame bools so we
    // re-derive accel each frame and run the same integrator.
    {
        int8_t accel = 0;
        if (inp.aim_up)   accel = -1;
        if (inp.aim_down) accel = +1;

        int8_t angle = static_cast<int8_t>(player_aim_angle_);
        if (inp.aim_centre) {
            // &3120-&3126 falls through into handle_raising_aim's DEC
            // &d3, so pressing I also nudges accel to -1 this frame.
            angle = 0;
            player_aim_velocity_ = 0;
            accel = -1;
        }

        // &30fe BEQ skip_acceleration: with accel==0 the velocity gets
        // reset to 0 (A=0 falls through to STA velocity). Holding K/O
        // ramps velocity by ±1 per frame, clamped to ±0x10.
        if (accel == 0) {
            player_aim_velocity_ = 0;
        } else {
            int v = int(player_aim_velocity_) + int(accel);
            if (v >  0x10) v =  0x10;
            if (v < -0x10) v = -0x10;
            player_aim_velocity_ = static_cast<int8_t>(v);
        }

        // &310a-&3112 angle += velocity, clamp to ±0x3f.
        int a = int(angle) + int(player_aim_velocity_);
        if (a >  0x3f) a =  0x3f;
        if (a < -0x3f) a = -0x3f;
        player_aim_angle_ = static_cast<uint8_t>(static_cast<int8_t>(a));

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
            // emit_directed computes the particle's velocity FROM
            // player_aiming_angle_with_flip (port of &330f), matching
            // the 6502's create_aim_particle (&312b -> calculate_firing_
            // vector_from_aiming_angle -> add_particle) exactly.
            uint8_t angle_with_flip = player.is_flipped_h()
                ? static_cast<uint8_t>(0x80 - static_cast<int8_t>(player_aim_angle_))
                : player_aim_angle_;
            particles_.emit_directed(ParticleType::AIM, angle_with_flip,
                                     aim_src, cosmetic_rng_);
        }
    }

    // &2d33 handle_firing + &2d36-&2d3b branch: firing while holding sets
    // player_object_fired = held_slot (one-frame flag read by doors /
    // transporters / RCD) instead of launching a bullet. SPACE is
    // repeat=no in the &0d action table — edge-gate on 0->1. Port-only:
    // blaster auto-repeats while held — re-fire when blaster_timer_
    // returns to 0 so the discharge stays continuous.
    bool fire_down = inp.fire;
    bool fire_edge = fire_down && !fire_key_prev_;
    fire_key_prev_ = fire_down;
    bool blaster_repeat = fire_down && player_weapon_ == 3 &&
                          blaster_timer_ == 0 && held_object_slot_ >= 0x80;
    if (fire_edge || blaster_repeat) {
        if (held_object_slot_ < 0x80) {
            player_object_fired_ = held_object_slot_;
        } else {
            Weapon::fire(object_mgr_, player, player_weapon_, player_aim_angle_,
                         weapon_energy_[player_weapon_], blaster_timer_,
                         rng_);
        }
    }

    // Inventory keys (rising-edge gated, mirrors 6502 just-pressed at
    // &126b):
    //   ,  pickup touching   m  drop straight down   .  throw forward

    // ENTER kept as a backwards-compat toggle — pickup if not holding,
    // drop (no throw) if holding. Same edge handling.
    bool pd_down = inp.pickup_drop;
    bool pd_edge = pd_down && !pickup_drop_key_prev_;
    pickup_drop_key_prev_ = pd_down;
    if (pd_edge) {
        if (held_object_slot_ < 0x80) drop_in_place(player);
        else                          pickup_touching(player);
    }

    bool pickup_down = inp.pickup;
    bool pickup_edge = pickup_down && !pickup_key_prev_;
    pickup_key_prev_ = pickup_down;
    if (pickup_edge) pickup_touching(player);

    bool drop_down = inp.drop;
    bool drop_edge = drop_down && !drop_key_prev_;
    drop_key_prev_ = drop_down;
    if (drop_edge) drop_in_place(player);

    bool throw_down = inp.throw_obj;
    bool throw_edge = throw_down && !throw_key_prev_;
    throw_key_prev_ = throw_down;
    if (throw_edge) throw_held(player);

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
        // Mirror &34f8: store first so G cycles pockets. drain_power_pod=false
        // keeps pre-seeded power_pods in the rotation instead of refuelling
        // the jetpack every time they come around.
        try_store_held(player, /*drain_power_pod=*/false);
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
                // &351a-&351d: pickup chime — soft click as the
                // retrieved item leaves the pocket and lands in hand.
                static constexpr uint8_t kSoundRetrieve[4] = { 0x17, 0x82, 0x13, 0xc2 };
                Audio::play(Audio::CH_ANY, kSoundRetrieve);
            } else {
                // Couldn't allocate a primary slot — restore the pocket.
                pockets_[pockets_used_] = ot;
                pockets_used_++;
            }
        }
    }

    // R -> handle_remembering_position (&2c3c). Records the player's
    // current tile position into the next teleport slot and rotates the
    // cursor, so pressing R up to 4 times stores 4 recall points. Also
    // increments the remembered count (capped at 4).
    bool remember_down = inp.remember_pos;
    bool remember_edge = remember_down && !remember_key_prev_;
    remember_key_prev_ = remember_down;
    if (remember_edge) {
        handle_remembering_position(player);
    }

    // T -> handle_teleporting (&0cc1). Pops the most recent remembered
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

    // Weapon select — port of &2ce8 change_weapon. Slot 0 (jetpack) is
    // always present per &2ce9 skip; slots 1..5 read player_weapons_
    // collected[X] at &2cef and refuse the switch if the bit isn't set.
    if (inp.weapon_select < 6) {
        uint8_t w = inp.weapon_select;
        if (w == 0 || (player_weapons_collected_[w] & 0x80)) {
            player_weapon_ = w;
        }
    }

    // &37e6-&380d jetpack drain. Eligible iff jumping/flying (state low
    // nibble >= 0x0a OR forced this frame by jetpack/up/down/boost-
    // horizontal via &2c7a) AND currently accelerating AND functioning
    // jetpack. Cadence: every 2 frames on boost, every 8 otherwise.
    bool flying_now = inp.jetpack || inp.move_up || inp.move_down ||
                      (inp.boost && (inp.move_left || inp.move_right));
    bool jumping_state = (player.state & 0x0f) >= 0x0a;
    bool thrusting = (accel_x != 0 || accel_y != 0);
    bool functioning = (weapon_energy_[0] > 0) &&
                       (player_immobility_movement_ < 0x06) &&
                       (player_immobility_thrust_ == 0);
    if ((flying_now || jumping_state) && thrusting && functioning) {
        bool drain_tick = inp.boost ? every_two_frames_
                                    : every_eight_frames_;
        if (drain_tick) weapon_energy_[0]--;
    }
}
