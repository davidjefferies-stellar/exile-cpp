#include "game/game.h"
#include "objects/physics.h"
#include "objects/collision.h"
#include "objects/tile_collision.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include "objects/held_object.h"
#include "behaviours/behavior_dispatch.h"
#include "behaviours/environment.h"
#include "behaviours/npc_helpers.h"
#include "behaviours/projectile.h"
#include "audio/audio.h"
#include "world/tertiary.h"
#include "world/tile_data.h"
#include "world/wind.h"
#include "world/water.h"
#include "rendering/sprite_atlas.h"
#include <array>

// Port of &30e9-&30f5 apply_tile_collision_to_position_and_velocity's
// bounce math, applied per-axis (our integration is axis-separated
// whereas the 6502 uses a single vector).
//
//   magnitude = min(|v|, 0x20)           ; &30e7 cap
//   magnitude = max(magnitude - 2, 0)    ; &30e9 lose 2
//   magnitude = magnitude * 7 / 8        ; &30ef damp 7/8
//   new_v     = -sign(old_v) * magnitude ; reflect
//
// Settles within 3-4 bounces because each pass loses ~25% of magnitude
// (sub-2 gets clamped, 7/8 takes the rest).
static int8_t bounce_reflect(int8_t v_in) {
    int mag = (v_in < 0) ? -v_in : v_in;
    if (mag > 0x20) mag = 0x20;
    mag = (mag > 2) ? (mag - 2) : 0;
    mag = mag * 7 / 8;
    return static_cast<int8_t>(v_in > 0 ? -mag : mag);
}

// Companion to bounce_reflect for the *other* axis on a collision. The
// 6502's &30ef calculate_seven_eighths damps the *combined* vector
// magnitude and then splits it between vx and vy via the reflected angle.
// Our axis-separated integration would otherwise leave the non-colliding
// axis at full velocity, so a thrown grenade would slide forever along
// the floor after the first bounce. Apply the 7/8 damping-without-reflect
// on the other axis to approximate that magnitude coupling.
// Port of &3235 calculate_seven_eighths.
static int8_t damp_seven_eighths(int8_t v_in) {
    int mag = (v_in < 0) ? -v_in : v_in;
    mag = mag * 7 / 8;
    return static_cast<int8_t>(v_in > 0 ? mag : -mag);
}

// AABB-corner solid probe for the held primary's penetration check at
// step 3. Returns true if any of the four corners (top-left, top-right,
// bottom-left, bottom-right) sits inside a solid tile. Used by the
// axis-separated revert to test "if I left this axis at the new flush
// value but kept the other axis at the old held position, does the
// sprite still penetrate a wall?".
static bool held_aabb_solid(const Landscape& landscape,
                             Fixed8_8 hx, Fixed8_8 hy, int hw, int hh) {
    int right_abs = int(hx.whole) * 256 + int(hx.fraction) + hw;
    int bot_abs   = int(hy.whole) * 256 + int(hy.fraction) + hh;
    uint8_t rtx = uint8_t((right_abs >> 8) & 0xff);
    uint8_t rxf = uint8_t(right_abs & 0xff);
    uint8_t bty = uint8_t((bot_abs   >> 8) & 0xff);
    uint8_t byf = uint8_t(bot_abs   & 0xff);
    return
        Collision::point_in_tile_solid(landscape,
            hx.whole, hy.whole, hx.fraction, hy.fraction) ||
        Collision::point_in_tile_solid(landscape,
            rtx, hy.whole, rxf, hy.fraction) ||
        Collision::point_in_tile_solid(landscape,
            hx.whole, bty, hx.fraction, byf) ||
        Collision::point_in_tile_solid(landscape,
            rtx, bty, rxf, byf);
}

// Sprite AABB width in 1/256-tile fraction units (port of the 6502's
// (w-1)*16 entry in sprites_width_and_horizontal_flip_table at &5e0c).
static int spr_w_units(uint8_t sprite_id) {
    if (sprite_id > 0x80) return 0;
    int w = sprite_atlas[sprite_id].w;
    return (w > 0 ? (w - 1) : 0) * 16;
}

// Sprite AABB height in fraction units (port of (h-1)*8 from &5e89).
static int spr_h_units(uint8_t sprite_id) {
    if (sprite_id > 0x80) return 0;
    int h = sprite_atlas[sprite_id].h;
    return (h > 0 ? (h - 1) : 0) * 8;
}

// ±2 velocity kick along the smallest-overlap axis (port of &2b80-&2b8b).
// Clamps to signed 8-bit per &327f prevent_overflow.
static void nudge_velocity(int8_t& v, int sign) {
    int nv = int(v) + 2 * sign;
    if (nv >  127) nv =  127;
    if (nv < -128) nv = -128;
    v = static_cast<int8_t>(nv);
}

// Full object-object overlap response — port of &2b51-&2bb0 applied to
// the (this, other) pair after check_for_collisions has confirmed an
// overlap. Picks the smallest of the four edge penetration depths,
// pushes `obj` out by that depth, adds a ±2 velocity nudge along the
// same axis, and runs the &2bb6 mass-ratio velocity transfer for both
// axes (with smallest-overlap doubling on the impact axis). Symmetric
// touching stamp matches &2b1d-&2b27.
//
// Caller pre-filters: skip when either side is intangible or when this
// is a newly-created bullet (they manage their own collision response
// in common_bullet_update). The 6502 has no hard-revert; the push-out
// here is what prevents the embedded-pair state that pure velocity
// transfer can't break.
static void resolve_obj_overlap_response(Object& obj, int slot,
                                         Object& other, int other_slot) {
    int this_l = obj.x.whole * 256 + obj.x.fraction;
    int this_t = obj.y.whole * 256 + obj.y.fraction;
    int this_r = this_l + spr_w_units(obj.sprite);
    int this_b = this_t + spr_h_units(obj.sprite);
    int other_l = other.x.whole * 256 + other.x.fraction;
    int other_t = other.y.whole * 256 + other.y.fraction;
    int other_r = other_l + spr_w_units(other.sprite);
    int other_b = other_t + spr_h_units(other.sprite);

    // Edge labels:
    //   0 right (this extends past other-left → push left, sign -1)
    //   1 left  (this extends past other-right → push right, sign +1)
    //   2 bot   (this extends past other-top   → push up, sign -1)
    //   3 top   (this extends past other-bot   → push down, sign +1)
    int pen[4];
    pen[0] = this_r  - other_l;
    pen[1] = other_r - this_l;
    pen[2] = this_b  - other_t;
    pen[3] = other_b - this_t;
    int smallest = 0;
    for (int e = 1; e < 4; e++)
        if (pen[e] < pen[smallest]) smallest = e;
    bool axis_y = (smallest >= 2);
    int sign = (smallest == 0 || smallest == 2) ? -1 : +1;
    int depth = pen[smallest];

    // &2b8d-&2b94 push this out by overlap.
    if (!axis_y) {
        int nx = this_l + sign * depth;
        obj.x.whole    = static_cast<uint8_t>((nx >> 8) & 0xff);
        obj.x.fraction = static_cast<uint8_t>(nx & 0xff);
    } else {
        int ny = this_t + sign * depth;
        obj.y.whole    = static_cast<uint8_t>((ny >> 8) & 0xff);
        obj.y.fraction = static_cast<uint8_t>(ny & 0xff);
    }

    // &2b80-&2b8b ±2 velocity nudge along smallest-overlap axis.
    if (!axis_y) nudge_velocity(obj.velocity_x, sign);
    else         nudge_velocity(obj.velocity_y, sign);

    // &2bb6 per-axis mass-ratio transfer.
    {
        auto t = Collision::apply_mass_ratio_velocity(
            obj.velocity_x, other.velocity_x,
            obj.weight(), other.weight(), !axis_y);
        obj.velocity_x = t.this_v;
        other.velocity_x = t.other_v;
    }
    {
        auto t = Collision::apply_mass_ratio_velocity(
            obj.velocity_y, other.velocity_y,
            obj.weight(), other.weight(), axis_y);
        obj.velocity_y = t.this_v;
        other.velocity_y = t.other_v;
    }

    // &2b1d-&2b27 symmetric touching stamp.
    obj.touching   = static_cast<uint8_t>(other_slot);
    other.touching = static_cast<uint8_t>(slot);
}

// Full 18-step update loop - port of &1a0b-&1e18
void Game::update_objects() {
    const Object& player = object_mgr_.player();

    // Secondary object promotion
    object_mgr_.promote_selective(rng_);

    // Main loop over slots 1-15
    for (int slot = 1; slot < GameConstants::PRIMARY_OBJECT_SLOTS; slot++) {
        Object& obj = object_mgr_.object(slot);
        if (!obj.is_active()) continue;

        // Step 3: Handle held objects. 6502 &1afd-&1b54 snaps the held
        // primary flush to the player's facing side (height-centred,
        // x_flip-mirrored, velocity copied from player), then falls
        // through into the SAME check_for_collisions routine every
        // other primary uses. The drift between the snap-set position
        // (saved in held_expected_*) and the post-resolve position is
        // what feeds &1ca9 consider_dropping_held_object below.
        if (slot == held_object_slot_) {
            Fixed8_8 prev_held_x = obj.x;
            Fixed8_8 prev_held_y = obj.y;
            HeldObject::update_position(obj, player);
            held_expected_x_ = obj.x;
            held_expected_y_ = obj.y;
            // Step 15 normally resets pre_collision_magnitude to 0 at
            // the top of each frame, but we skip step 15 for held
            // primaries. Without an explicit reset here, a high
            // magnitude captured BEFORE pickup (e.g. the impact frame
            // where the player walked into the flask to grab it)
            // persists forever, and update_full_flask's >= 0x14
            // trigger fires every frame → continuous water bleed
            // (re-arms timer = 0x10 → 8 particles/frame indefinitely).
            // Resetting matches step 15 so TileCollision::resolve
            // below only re-stamps the value if the held actually
            // penetrates solid geometry this frame (slap into a wall).
            obj.pre_collision_magnitude = 0;
            TileCollision::Result tcr = TileCollision::resolve(
                obj, prev_held_x.whole, prev_held_x.fraction,
                prev_held_y.whole, prev_held_y.fraction,
                landscape_, object_mgr_, /*skip_slot=*/-1);
            obj.tile_collision = tcr.top_or_bottom_collision;
        }

        // Step 7: Check demotion
        if (object_mgr_.check_demotion(slot, frame_counter_)) {
            if (slot == held_object_slot_) {
                held_object_slot_ = 0x80;
            }
            continue;
        }

        // Step 8: Handle teleporting (port of &1bfd-&1c44)
        if (obj.flags & ObjectFlags::TELEPORTING) {
            if (obj.timer == 0) {
                // Finished teleporting: clear flag
                obj.flags &= ~ObjectFlags::TELEPORTING;
                if (obj.energy < 0xff) obj.energy++;
            } else {
                if (obj.timer == 0x11) {
                    // Brief removal at midpoint (object disappears)
                }
                if (obj.timer == 0x10) {
                    // Change position to teleport destination
                    obj.x.whole = obj.tx;
                    obj.y.whole = obj.ty;
                    obj.x.fraction = 0x80; // Center in tile
                    obj.y.fraction = 0x80;
                    obj.velocity_x = 0;
                    obj.velocity_y = 0;
                    // &1c32-&1c35 play sound for object changing position
                    // in teleport. play_at so distance attenuates for
                    // off-screen primaries that teleport (clawed robots,
                    // hovering balls returning to nest).
                    static constexpr uint8_t kSoundTeleportArrive[4] = {
                        0x33, 0xf3, 0x63, 0xf3 };
                    Audio::play_at(Audio::CH_ANY, kSoundTeleportArrive,
                                   obj.x.whole, obj.y.whole);
                }
                obj.timer--;
                continue; // Skip physics while teleporting
            }
        }

        // Step 9b: Refresh `touching` BEFORE the type-specific update reads
        // it. The 6502 calls check_for_collisions at &1b54 ahead of the
        // per-type dispatch so update routines see a touching field that
        // reflects the current overlap (most notably &4704 update_triax,
        // which absorbs the destinator on its first frame). The end-of-
        // update OR #&80 at &1dd6-&1dda handles cross-frame staleness;
        // here we just detect this frame's overlap and stamp both sides.
        //
        // The 6502 SKIPS this call for static objects at &1b50 (BIT &2c
        // / BMI &1bb7), relying solely on the symmetric stamp from other
        // slots' check_for_collisions to set their touching. We run it
        // for all weights — overlap detection is symmetric, so the answer
        // matches, and it gives static objects a fresh stamp on the same
        // frame an overlap starts rather than waiting for a later slot.
        // The frame-fresh clear on no-overlap only fires for non-static
        // because static touching is owned by the end-of-update OR and
        // by other slots' symmetric stamps, not by self-checks.
        {
            auto early_coll = Collision::check_object_collision(
                obj, slot,
                reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                    object_mgr_.object(0)));
            if (early_coll.collided) {
                obj.touching = static_cast<uint8_t>(early_coll.other_slot);
                Object& other = object_mgr_.object(early_coll.other_slot);
                other.touching = static_cast<uint8_t>(slot);
            } else if (obj.weight() < 7) {
                obj.touching = 0x80;
            }
        }

        // Step 10: Call type-specific update routine
        auto update_fn = AI::get_update_func(obj.type);
        if (update_fn) {
            UpdateContext uctx{object_mgr_, landscape_, rng_, frame_counter_,
                              every_four_frames_, every_eight_frames_,
                              every_sixteen_frames_, every_thirty_two_frames_,
                              every_sixty_four_frames_,
                              whistle_one_active_, whistle_two_activator_,
                              &whistle_one_collected_, &whistle_two_collected_,
                              player_mushroom_timers_,
                              player_keys_collected_,
                              &particles_,
                              held_object_slot_, player_object_fired_,
                              // &311d player_aiming_angle_with_flip:
                              // mirror across the vertical axis when
                              // player is facing left. Match Weapon::
                              // get_firing_velocity's (0x80 - s) form
                              // exactly so AIM particles and bullets
                              // always agree on direction — the EOR/+1
                              // form is algebraically the same but
                              // routing both through the same explicit
                              // signed subtract keeps any future tweak
                              // synchronised across the two paths.
                              static_cast<uint8_t>(player.is_flipped_h()
                                  ? (0x80 - static_cast<int8_t>(player_aim_angle_))
                                  : static_cast<int8_t>(player_aim_angle_)),
                              slot,
                              imp_gifts_remaining_,
                              &background_flash_cooldown_,
                              renderer_ && renderer_->damage_overlay_enabled()
                                  ? &damage_events_ : nullptr,
                              &explosion_timer_,
                              &flooding_state_,
                              &floating_labels_};
            update_fn(obj, uctx);
        }

        // Step 11: 6502 &1ca9-&1cc4 consider_dropping_held_object.
        // The original computes a 16-bit signed drift between the
        // pre-collision "snap-to-player-side" position (saved in
        // held_expected_* during step 6/7) and the current obj.x/y
        // (which TileCollision::resolve may have pushed back if the
        // snap penetrated a wall). Drop if drift on either axis
        // leaves the [-0x30, +0x30) frac window — that's the 6502's
        // `(diff + 0x30) >= 0x60 OR high-byte ≠ 0` test expressed
        // as a signed bound.
        if (slot == held_object_slot_) {
            int drift_x =
                (static_cast<int>(held_expected_x_.whole) * 256 +
                 static_cast<int>(held_expected_x_.fraction)) -
                (static_cast<int>(obj.x.whole) * 256 +
                 static_cast<int>(obj.x.fraction));
            int drift_y =
                (static_cast<int>(held_expected_y_.whole) * 256 +
                 static_cast<int>(held_expected_y_.fraction)) -
                (static_cast<int>(obj.y.whole) * 256 +
                 static_cast<int>(obj.y.fraction));
            if (drift_x >= 0x30 || drift_x < -0x30 ||
                drift_y >= 0x30 || drift_y < -0x30) {
                HeldObject::drop(obj, object_mgr_.player(),
                                 held_object_slot_);
            }
        }

        // Step 12: Handle explosions. Port of &1ce3-&1cf3: when energy
        // hits zero, mutate the slot IN-PLACE into an EXPLOSION via
        // explode_object_with_duration_A (&40db). The 6502 routes
        // through the per-type explosion routine — for "explode with
        // squeal" types (doors, hives, grenades, bullets) duration =
        // (initial_energy / 32) + 3, which is what we use here so the
        // boom is visible.
        //
        // Mutating IN-PLACE is critical for tertiary-backed primaries
        // like doors: keeping the same slot keeps obj.tertiary_slot
        // pointing at the door's entry, and tertiary_spawn.cpp's
        // dedup scan blocks re-spawning the door while the explosion
        // still occupies the slot.
        if (obj.energy == 0 && obj.type != ObjectType::EXPLOSION &&
            !object_type_is_indestructible(static_cast<uint8_t>(obj.type))) {
            // 6502 &1ce3-&1cf3 dispatches energy=0 through
            // update_routine_addresses_high_table[type]'s top two bits.
            // OBJECT_EXPLOSION (0x44) → 00 indestructible → no-op;
            // EMPTY_FLASK / FULL_FLASK / keys / weapons / pickups / Triax
            // / static cosmetics → also 00, also no-op (they sit at
            // energy 0 instead of transmuting). Without the indestructible
            // guard, the explosion radius from a grenade would convert a
            // nearby flask into an EXPLOSION sprite. Without the
            // EXPLOSION guard, step 12 would re-fire every tick on the
            // mutated slot, restoring tertiary_data_offset to its full
            // duration and producing an infinite damage well.
            uint8_t init_e = get_initial_energy(static_cast<uint8_t>(obj.type));
            uint8_t duration = static_cast<uint8_t>((init_e >> 5) + 3);

            // Port-only deviation: swap small projectile sprites for
            // SPRITE_FIREBALL so their explosions are visible on our
            // wider viewport. The 6502 keeps the original sprite for
            // every explosion (a bullet's 3x2 sprite flickers under
            // the cycling palette), which on the BBC's ~10-tile-wide
            // screen reads fine but on our 40-tile viewport vanishes
            // into nothing. Doors and other large sprites keep their
            // original sprite so the palette flicker plays out on
            // the source shape (matches 6502 visually).
            //
            // Range &12..&1b is the 6502's OBJECT_RANGE_PROJECTILES
            // (active grenade through hovering balls). Those are the
            // types whose 6502 sprites are 3x2..6x4 — too small for
            // our viewport to pick up the palette cycle on its own.
            uint8_t t = static_cast<uint8_t>(obj.type);
            bool is_projectile = (t >= 0x12 && t <= 0x1b);
            if (is_projectile) {
                int old_w_frac = 0, old_h_frac = 0;
                if (obj.sprite <= 0x80) {
                    const SpriteAtlasEntry& e = sprite_atlas[obj.sprite];
                    old_w_frac = (e.w > 0 ? (e.w - 1) : 0) * 16;
                    old_h_frac = (e.h > 0 ? (e.h - 1) : 0) * 8;
                }
                obj.sprite = object_types_sprite[
                    static_cast<uint8_t>(ObjectType::EXPLOSION)];
                obj.palette = object_types_palette_and_pickup[
                    static_cast<uint8_t>(ObjectType::EXPLOSION)] & 0x7f;
                int new_w_frac = 0, new_h_frac = 0;
                if (obj.sprite <= 0x80) {
                    const SpriteAtlasEntry& e = sprite_atlas[obj.sprite];
                    new_w_frac = (e.w > 0 ? (e.w - 1) : 0) * 16;
                    new_h_frac = (e.h > 0 ? (e.h - 1) : 0) * 8;
                }
                auto shift_axis = [](uint8_t& whole, uint8_t& frac, int delta) {
                    int sum = int(whole) * 0x100 + int(frac) + delta;
                    sum &= 0xffff;
                    whole = static_cast<uint8_t>((sum >> 8) & 0xff);
                    frac  = static_cast<uint8_t>(sum & 0xff);
                };
                shift_axis(obj.x.whole, obj.x.fraction, (old_w_frac - new_w_frac) / 2);
                shift_axis(obj.y.whole, obj.y.fraction, (old_h_frac - new_h_frac) / 2);
            }

            Behaviors::explode_object_with_duration(obj, duration);
            static constexpr uint8_t kSoundExplosion[4] = { 0x17, 0x03, 0x11, 0x04 };
            Audio::play_at(Audio::CH_PRIORITY, kSoundExplosion,
                           obj.x.whole, obj.y.whole);
            // The slot is now an EXPLOSION primary — let it run through
            // the normal update_explosion path on subsequent frames so
            // the duration counts down, particles fire, and the radius
            // damage applies.
            if (slot == held_object_slot_) held_object_slot_ = 0x80;
            continue;
        }

        // Step 14: Reap PENDING_REMOVAL objects.
        //
        // The 6502's set_object_for_removal at &2516 just sets the flag;
        // the main loop later zeroes the slot. It does NOT bounce the
        // object back to tertiary — PENDING_REMOVAL means "this thing is
        // GONE" (collected, exploded, despawned) and reviving it would
        // undo whatever effect set the flag, and would re-arm bit 7 of
        // the data byte so collected items respawn a few frames later.
        if (obj.flags & ObjectFlags::PENDING_REMOVAL) {
            object_mgr_.remove_object(slot);
            if (slot == held_object_slot_) held_object_slot_ = 0x80;
            continue;
        }

        // Step 9: Apply wind (only above surface)
        Wind::apply_surface_wind(obj);

        // Wind particle emission — port of &3f73 add_wind_particle_
        // using_velocities. One PARTICLE_WIND per frame with probability
        // `magnitude / 0x7f`: stronger wind → visibly more drift trails.
        // The 6502's flow at &3f73-&3f91 is: (1) calculate_angle_from_vector
        // turns the active wind (vector_x, vector_y) into &b5; (2) the
        // probability gate at &3f76-&3f7b checks that &b7 magnitude exceeds
        // a random byte; (3) add_particle reads the angle and the type's
        // spd_rand/spd_base to pick a base velocity in the wind direction.
        // emit_directed reproduces step 3, so wind particles now drift in
        // the actual wind direction rather than a random one.
        {
            int8_t wvx = 0, wvy = 0;
            Wind::surface_wind_vector(obj, wvx, wvy);
            uint8_t mag = Wind::surface_wind_magnitude(obj);
            if (mag > 0 && (rng_.next() & 0x7f) < mag) {
                uint8_t angle = NPC::angle_from_deltas(wvx, wvy);
                particles_.emit_directed(ParticleType::WIND, angle, obj, rng_);
            }
        }

        // Step 15: Apply physics (gravity + velocity)
        if (slot != held_object_slot_) {
            // Per-type physics gate:
            //   weight 7           -> fully static; pin position, zero velocity.
            //   "undisturbed pin"  -> energy bit 7 set on a type whose update
            //                         routine pins via consider_disturbing_object
            //                         (collectables, inactive grenade). The
            //                         6502 keeps these still by simply NOT
            //                         calling add_A_to_position from inside
            //                         the type's update — there's no global
            //                         "integrate velocity into position" step
            //                         like ours has. Without an equivalent
            //                         skip here, gravity adds +1 to vy each
            //                         frame and our generic integration drifts
            //                         the object downward even after the
            //                         type's pin zeroed velocity. Treat it
            //                         like fully_static.
            //   INTANGIBLE (0x80)  -> keeps its velocity but skips gravity
            //                         (explosions, lightning, transporter beams,
            //                         moving fireballs, invisible inert — these
            //                         are "not physical" in the 6502's sense).
            //   otherwise          -> gravity + wind + velocity integration as normal.
            uint8_t tidx = static_cast<uint8_t>(obj.type);
            uint8_t tflags = (tidx < static_cast<uint8_t>(ObjectType::COUNT))
                             ? object_types_flags[tidx] : 0;
            bool fully_static = obj.weight() >= 7;
            bool gravity_exempt = (tflags & ObjectTypeFlags::INTANGIBLE) != 0;
            // "undisturbed" pin via energy bit 7 — see comment above.
            // Active for the types whose update_fn runs the consider_
            // disturbing_object pin (collectables 0x4a..0x64 and the
            // inactive grenade dispatch chain). The bit gets cleared on
            // touch in update_collectable, after which physics resumes.
            bool pin_undisturbed = (obj.energy & 0x80) != 0 &&
                                    static_cast<uint8_t>(obj.type) >= 0x4a;
            // BUSH pins unconditionally. &4ba9 update_bush is just JSR
            // set_this_object_velocities_to_zero + JMP set_this_object_
            // position_from_previous_position — the bush freezes in
            // place every frame regardless of touch. Bushes are weight 6
            // (flags 0xd6 & 0x07), so they fall outside fully_static; pin
            // them explicitly here or a wasp/projectile leaves residual
            // velocity that gravity then integrates.
            bool pin_bush = obj.type == ObjectType::BUSH;

            if (fully_static || pin_undisturbed || pin_bush) {
                obj.velocity_x = 0;
                obj.velocity_y = 0;
                // Static objects still need to notice when something is
                // touching them — switches fire on touching==player, doors
                // self-close on touching!=none, etc. The 6502's &2a64
                // check_for_collisions runs for every object, regardless of
                // weight; skipping it for weight-7 statics meant the switch
                // never saw the player even with overlapping AABBs.
                auto obj_coll = Collision::check_object_collision(
                    obj, slot,
                    reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(object_mgr_.object(0)));
                // Only stamp on overlap. Cross-frame staleness is handled
                // by the end-of-update OR #&80 at the bottom of the slot
                // loop (port of &1dd6-&1dda); a no-clear here just means
                // step 9b's stamp earlier this frame survives until that
                // OR runs.
                if (obj_coll.collided) {
                    obj.touching = static_cast<uint8_t>(obj_coll.other_slot);
                    object_mgr_.object(obj_coll.other_slot).touching =
                        static_cast<uint8_t>(slot);
                }
            } else {
                Physics::apply_acceleration(obj, 0, 0, every_sixteen_frames_);
                if (gravity_exempt && obj.velocity_y > 0) {
                    // Undo the gravity +1 that apply_acceleration's y-axis
                    // hard-codes. Leaves any real upward/downward velocity
                    // the object gave itself intact.
                    obj.velocity_y--;
                }

                // ====================================================
                // Tile collision — TileCollision::resolve port of the
                // 6502 &2f8c-&30df chain. Walks AABB edges, builds an
                // obstruction vector, pushes the object out perpendicular
                // to the surface, and reflects velocity at reduced angle.
                // Same module the player uses; covers slopes, walls,
                // floors, ceilings under one pipeline.
                //
                // Object-object overlap is handled below by the &2bb6
                // mass-ratio velocity transfer in both the heavier-
                // blocker and lighter-target branches.
                // ====================================================
                Fixed8_8 ou_old_x = obj.x;
                Fixed8_8 ou_old_y = obj.y;
                obj.x.add_velocity(obj.velocity_x);
                obj.y.add_velocity(obj.velocity_y);

                // Clear SUPPORTED before the resolve; it'll be re-set
                // below if landed_on_bottom comes back true. The 6502
                // doesn't have a SUPPORTED flag per se — it derives the
                // "grounded" state from tile_collision_y_flags bit 7
                // (set when the collision was "more to the bottom" of
                // the AABB, i.e. the object landed on something).
                obj.flags &= ~ObjectFlags::SUPPORTED;

                TileCollision::Result tcr = TileCollision::resolve(
                    obj, ou_old_x.whole, ou_old_x.fraction,
                    ou_old_y.whole, ou_old_y.fraction,
                    landscape_, object_mgr_,
                    /*skip_slot=*/-1);
                obj.tile_collision = tcr.top_or_bottom_collision;
                if (tcr.landed_on_bottom) {
                    obj.flags |= ObjectFlags::SUPPORTED;
                }

                // Object-object overlap response. Port of &2b51-&2bb0
                // (smallest-overlap push-out + ±2 velocity nudge + per-
                // axis &2bb6 mass-ratio transfer). Same response runs
                // for both heavier-blocker and lighter-target overlaps —
                // the 6502 doesn't distinguish at &2b97. We split the
                // detection only to keep overlapping_solid_slot's
                // weight filter for the door/turret/cannon case, where
                // it picks the heavier overlap when multiple are present;
                // the response itself is shared.
                //
                // Newly-spawned bullets are exempt because their initial
                // position deliberately overlaps the firer's AABB (port
                // of &2afd-&2b0e). Projectile types (ICER_BULLET 0x13 ..
                // PLASMA_BALL 0x19) also skip: bullets run their own
                // velocity transfer + explosion inside the type-specific
                // update routine (see projectile.cpp around line 198) so
                // adding it here would double-apply.
                {
                    uint8_t self_idx = static_cast<uint8_t>(obj.type);
                    bool is_projectile = (self_idx >= 0x13 && self_idx <= 0x19);
                    auto& all_primaries =
                        reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                            object_mgr_.object(0));
                    bool eligible =
                        !is_projectile &&
                        !(obj.flags & ObjectFlags::NEWLY_CREATED) &&
                        !(tflags & ObjectTypeFlags::INTANGIBLE);

                    int blocker = Collision::overlapping_solid_slot(
                        obj, slot, all_primaries);
                    if (eligible && blocker >= 0) {
                        // Heavier-blocker overlap: same response as the
                        // lighter case (the 6502 doesn't distinguish at
                        // &2b97 — push out + nudge + transfer for every
                        // overlap). For very fast objects this can let
                        // the moving primary clip through a static door
                        // when the smallest-overlap edge is on the far
                        // side, but the BBC's per-frame velocities are
                        // small enough that the near-side overlap stays
                        // smallest in practice. Same behaviour the 6502
                        // exhibits.
                        Object& other = object_mgr_.object(blocker);
                        resolve_obj_overlap_response(obj, slot, other, blocker);
                    } else if (eligible) {
                        // Lighter target (or any other overlap). Without
                        // this, a grenade (weight 4) thrown at a flask
                        // (weight 2) flies straight through — the
                        // overlapping_solid_slot filter above only fires
                        // for HEAVIER targets, so light pickups are
                        // invisible to the moving primary unless we
                        // explicitly handle them here.
                        auto coll = Collision::check_object_collision(
                            obj, slot, all_primaries);
                        if (coll.collided) {
                            Object& other = object_mgr_.object(coll.other_slot);
                            uint8_t oi = static_cast<uint8_t>(other.type);
                            uint8_t of = (oi < static_cast<uint8_t>(ObjectType::COUNT))
                                         ? object_types_flags[oi] : 0;
                            if (!(of & ObjectTypeFlags::INTANGIBLE)) {
                                resolve_obj_overlap_response(
                                    obj, slot, other, coll.other_slot);
                            }
                        }
                    }
                }

                // Water splash — port of &2f69-&2f82. Emit one upward
                // PARTICLE_WATER if the object just dropped across the
                // waterline this frame.
                {
                    uint8_t wy = Water::get_waterline_y(obj.x.whole);
                    bool was_above = ou_old_y.whole < wy;
                    bool now_at    = obj.y.whole >= wy;
                    if (was_above && now_at && obj.velocity_y > 0) {
                        particles_.emit_directed(ParticleType::WATER,
                                                 0xc0, obj, rng_);
                    }
                }
#if 0
                // Sprite AABB in 16-bit fraction-unit space. The 6502
                // stores (pixels-1)*16 in its width table and (rows-1)*8
                // in its height table (see &5e89 / the port at
                // tertiary_spawn.cpp:186); derive the same here so the
                // probe matches what the renderer actually draws.
                int obj_h_units = (obj.sprite <= 0x80)
                    ? (sprite_atlas[obj.sprite].h > 0
                        ? (sprite_atlas[obj.sprite].h - 1) * 8 : 0)
                    : 0;
                int obj_w_units = (obj.sprite <= 0x80)
                    ? (sprite_atlas[obj.sprite].w > 0
                        ? (sprite_atlas[obj.sprite].w - 1) * 16 : 0)
                    : 0;

                // Probe a single (tile_x, tile_y) cell at an explicit
                // sub-tile (x_frac, y_frac). Door tiles are swapped for
                // their live closed/open substitute so primaries collide
                // with closed doors like the player does, and the pattern-
                // based `tile_and_flip_obstructs_point` handles slopes
                // and spaceship-wall tiles where only a sub-section is
                // solid — same per-section resolution as the 6502's
                // &2fce LDA (&7c),Y / CMP y_fraction check.
                auto probe_tile = [&](uint8_t ttx, uint8_t tty,
                                      uint8_t ttx_frac,
                                      uint8_t tty_frac)->bool {
                    ResolvedTile res =
                        resolve_tile_with_tertiary(landscape_, ttx, tty);
                    uint8_t subst = Collision::substitute_door_for_obstruction(
                        res.tile_and_flip, res.data_offset,
                        reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                            object_mgr_.object(0)),
                        object_mgr_.tertiary_data_byte(res.data_offset));
                    return Collision::tile_and_flip_obstructs_point(
                        subst, ttx_frac, tty_frac);
                };

                // Sample all four corners of the sprite's AABB
                // unconditionally. Even when corners share a tile, their
                // (x_frac, y_frac) differ and the obstruction threshold
                // is a function of x_section / y_frac, so "same tile"
                // doesn't mean "same obstruction answer" — required for
                // slopes and partial-tile patterns. The probes are
                // idempotent when corners collapse onto the same point.
                auto any_tile_solid = [&](uint8_t tx, uint8_t tx_frac,
                                          uint8_t ty, uint8_t ty_frac)->bool {
                    int right_abs  = static_cast<int>(tx) * 256 +
                                     static_cast<int>(tx_frac) + obj_w_units;
                    uint8_t r_tx   = static_cast<uint8_t>((right_abs >> 8) & 0xff);
                    uint8_t r_frac = static_cast<uint8_t>(right_abs & 0xff);
                    int bot_abs    = static_cast<int>(ty) * 256 +
                                     static_cast<int>(ty_frac) + obj_h_units;
                    uint8_t b_ty   = static_cast<uint8_t>((bot_abs >> 8) & 0xff);
                    uint8_t b_frac = static_cast<uint8_t>(bot_abs & 0xff);

                    if (probe_tile(tx,   ty,   tx_frac, ty_frac)) return true;
                    if (probe_tile(r_tx, ty,   r_frac,  ty_frac)) return true;
                    if (probe_tile(tx,   b_ty, tx_frac, b_frac))  return true;
                    if (probe_tile(r_tx, b_ty, r_frac,  b_frac))  return true;
                    return false;
                };

                // 6502 &1b `tile_top_or_bottom_collision` only flags
                // top/bottom (Y-axis) collisions — side (X-axis) hits go
                // into `left_obstruction` / `right_obstruction` which
                // bullets and piranha/wasp behaviour don't read. Mirror
                // that here: tile_collision is set *only* on the Y revert.
                //
                // The tile-pattern probe isn't enough on its own around
                // closed doors: STONE_SLOPE_78 (the door substitute) is
                // only solid in the tile's left quarter, while the door
                // sprite spans ~half the tile. For the remaining band we
                // fall back to overlaps_solid_object — same mechanism
                // player_motion.cpp uses. Without this a grenade dropped
                // near tile-centre passes through a closed door.
                auto& all_primaries =
                    reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                        object_mgr_.object(0));
                obj.tile_collision = false;
                obj.pre_collision_magnitude = 0;

                // Per-axis "escape" relaxation — partial port of &306c
                // apply_tile_collision_to_position_and_velocity. Allow
                // the move only if the start position has its OPPOSITE
                // side blocked (motion in the escape direction).
                auto side_corners = [&](uint8_t tx, uint8_t tx_frac,
                                        uint8_t ty, uint8_t ty_frac,
                                        bool& top, bool& bot,
                                        bool& left, bool& right) {
                    int right_abs = static_cast<int>(tx) * 256 +
                                    static_cast<int>(tx_frac) + obj_w_units;
                    uint8_t r_tx   = static_cast<uint8_t>((right_abs >> 8) & 0xff);
                    uint8_t r_frac = static_cast<uint8_t>(right_abs & 0xff);
                    int bot_abs   = static_cast<int>(ty) * 256 +
                                    static_cast<int>(ty_frac) + obj_h_units;
                    uint8_t b_ty   = static_cast<uint8_t>((bot_abs >> 8) & 0xff);
                    uint8_t b_frac = static_cast<uint8_t>(bot_abs & 0xff);
                    bool tl = probe_tile(tx,   ty,   tx_frac, ty_frac);
                    bool tr = probe_tile(r_tx, ty,   r_frac,  ty_frac);
                    bool bl = probe_tile(tx,   b_ty, tx_frac, b_frac);
                    bool br = probe_tile(r_tx, b_ty, r_frac,  b_frac);
                    top   = tl || tr;
                    bot   = bl || br;
                    left  = tl || bl;
                    right = tr || br;
                };
                bool start_top = false, start_bot = false;
                bool start_left = false, start_right = false;
                side_corners(obj.x.whole, obj.x.fraction,
                             obj.y.whole, obj.y.fraction,
                             start_top, start_bot, start_left, start_right);

                // Object-object overlap at start relaxes for newly-spawned
                // bullets and for intangible objects (explosions, lightning,
                // transporter beams, fireballs). The relax is needed because
                // a bullet's spawn position can overlap its firer's AABB on
                // frame 1 — without an escape, the next frame's revert
                // would pin it inside the firer. Restricted to NEWLY_CREATED
                // + INTANGIBLE so an established primary stuck inside a
                // closed door's AABB stays trapped instead of phasing out.
                bool start_obj_overlap = false;
                if ((obj.flags & ObjectFlags::NEWLY_CREATED) ||
                    (tflags & ObjectTypeFlags::INTANGIBLE)) {
                    start_obj_overlap =
                        Collision::overlaps_solid_object(obj, slot, all_primaries);
                }
                {
                    Fixed8_8 old_x = obj.x;
                    obj.x.add_velocity(obj.velocity_x);
                    bool tile_blocked =
                        any_tile_solid(obj.x.whole, obj.x.fraction,
                                       obj.y.whole, obj.y.fraction);
                    int  obj_blocker = Collision::overlapping_solid_slot(
                        obj, slot, all_primaries);
                    bool obj_blocked = (obj_blocker >= 0);
                    // Object-overlap relax only escapes another object's AABB,
                    // never punches through tiles — otherwise a player pushing
                    // a resting collectable would walk it through walls/floor.
                    //
                    // Side-relax (clauses 2/3) is tile-pattern-only and gated
                    // on !obj_blocked so heavier-primary collisions (door,
                    // cannon, hive) outrank the tile-pattern escape — without
                    // the gate, substitute_door_for_obstruction's sub-tile
                    // pattern (e.g. STONE_SLOPE_78, solid only in the left
                    // quarter) lets an object straddling the door's AABB
                    // phase out through the empty band. Clause 1 stays
                    // unconditional because bullets must leave their firer's
                    // AABB on frame 1.
                    bool relax_x = (start_obj_overlap && !tile_blocked) ||
                        (obj.velocity_x > 0 && start_left  && !start_right && !obj_blocked) ||
                        (obj.velocity_x < 0 && start_right && !start_left  && !obj_blocked);
                    if ((tile_blocked || obj_blocked) && !relax_x) {
                        // Port of &30b7: capture the max-axis velocity
                        // BEFORE the reflect/damp so update_full_flask and
                        // friends can tell a hard collision from a scrape.
                        uint8_t pre = static_cast<uint8_t>(std::max(
                            std::abs(static_cast<int>(obj.velocity_x)),
                            std::abs(static_cast<int>(obj.velocity_y))));
                        obj.pre_collision_magnitude = pre;
                        obj.x = old_x;
                        obj.velocity_x = bounce_reflect(obj.velocity_x);
                        obj.velocity_y = damp_seven_eighths(obj.velocity_y);
                        // Port of &2b1d-&2b27: when a primary bumps a heavier
                        // primary, BOTH sides' touching get stamped. Without
                        // this, the heavier side (door, hive, cannon) never
                        // sees the toucher because its own step 9b runs
                        // post-revert when the AABBs no longer overlap, so
                        // update_door's touched-and-unlocked toggle at &4d0d
                        // never fires from contact.
                        if (obj_blocked) {
                            obj.touching = static_cast<uint8_t>(obj_blocker);
                            object_mgr_.object(obj_blocker).touching =
                                static_cast<uint8_t>(slot);
                        }
                    }
                }
                {
                    Fixed8_8 old_y = obj.y;
                    obj.y.add_velocity(obj.velocity_y);
                    bool tile_blocked =
                        any_tile_solid(obj.x.whole, obj.x.fraction,
                                       obj.y.whole, obj.y.fraction);
                    int  obj_blocker = Collision::overlapping_solid_slot(
                        obj, slot, all_primaries);
                    bool obj_blocked = (obj_blocker >= 0);
                    // Same side-relax gating as the X-axis above: a heavier
                    // primary (closed horizontal door, etc.) must outrank
                    // the tile-pattern escape. Without !obj_blocked here, a
                    // jumping imp whose top edge clipped the door tile's
                    // sub-tile-thin SPACESHIP_WALL_HORIZONTAL_QUARTER pattern
                    // could escape upward through the door's primary AABB.
                    bool relax_y = (start_obj_overlap && !tile_blocked) ||
                        (obj.velocity_y > 0 && start_top && !start_bot && !obj_blocked) ||
                        (obj.velocity_y < 0 && start_bot && !start_top && !obj_blocked);
                    if ((tile_blocked || obj_blocked) && !relax_y) {
                        uint8_t pre = static_cast<uint8_t>(std::max(
                            std::abs(static_cast<int>(obj.velocity_x)),
                            std::abs(static_cast<int>(obj.velocity_y))));
                        if (pre > obj.pre_collision_magnitude) {
                            obj.pre_collision_magnitude = pre;
                        }
                        obj.y = old_y;
                        if (obj.velocity_y > 0) obj.flags |= ObjectFlags::SUPPORTED;
                        obj.velocity_y = bounce_reflect(obj.velocity_y);
                        obj.velocity_x = damp_seven_eighths(obj.velocity_x);
                        obj.tile_collision = true;
                        // Symmetric touching stamp — same as the X-axis
                        // revert above. A flask landing on top of a
                        // closed door triggers the Y-axis branch (gravity
                        // pulled it onto the door's AABB), so this is
                        // where the door learns about the flask.
                        if (obj_blocked) {
                            obj.touching = static_cast<uint8_t>(obj_blocker);
                            object_mgr_.object(obj_blocker).touching =
                                static_cast<uint8_t>(slot);
                        }
                    }

                    // Water splash — port of &2f69-&2f82 add_water_
                    // particles_for_splash. If the object just crossed
                    // the waterline this frame moving downward, emit one
                    // PARTICLE_WATER at the crossing point with angle
                    // &c0 (straight up): emit_directed feeds the angle +
                    // the type's spd_rand/spd_base through &2357 so the
                    // droplet starts with vy ≈ -(spd_base..spd_base+spd_rand)
                    // and visibly leaps out of the water.
                    uint8_t wy = Water::get_waterline_y(obj.x.whole);
                    bool was_above = old_y.whole < wy;
                    bool now_at    = obj.y.whole >= wy;
                    if (was_above && now_at && obj.velocity_y > 0) {
                        particles_.emit_directed(ParticleType::WATER,
                                                 0xc0, obj, rng_);
                    }
                }
#endif

                // Apply water effects (buoyancy + damping)
                Water::apply_water_effects(landscape_, obj, obj.weight(),
                                            every_four_frames_);

                // Tile-based wind / water-current (port of &3f18 / &3f41 /
                // &3fa3). Pushes the object toward the tile's wind/flow
                // vector. Runs after the global surface wind so a windy
                // cavern's local force can override the surface drift.
                Wind::apply_tile_environment(obj, landscape_, object_mgr_,
                                             frame_counter_, rng_, particles_);

                // SUPPORTED is driven by TileCollision::resolve's
                // landed_on_bottom flag (see above).

                // Object-object collision: stamp BOTH sides on overlap
                // (port of &2b14-&2b27 in check_for_collisions). Late
                // re-check after physics so a velocity-driven move into
                // another primary is registered this frame; the
                // end-of-update OR #&80 at the bottom of the slot loop
                // handles cross-frame staleness.
                auto obj_coll = Collision::check_object_collision(
                    obj, slot,
                    reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(object_mgr_.object(0)));
                if (obj_coll.collided) {
                    obj.touching = static_cast<uint8_t>(obj_coll.other_slot);
                    object_mgr_.object(obj_coll.other_slot).touching =
                        static_cast<uint8_t>(slot);
                } else {
                    obj.touching = 0x80;
                }
            }
        }

        // &3ef2 update_invisible_switch_tile — collision branch. Per-
        // primary scan covers type-filtered switches the player path
        // can't trigger: the (&7f, &77) closer at idx &ae is type &4c
        // (OBJECT_EMPTY_FLASK) so only a flask resting on / thrown
        // across the cell will fire it. Same head→feet AABB walk
        // integrate_player_motion uses; the helper itself bails fast
        // when the resolved tile isn't INVISIBLE_SWITCH.
        {
            uint8_t sprite_id = obj.sprite;
            uint8_t h_frac = 0;
            if (sprite_id <= 0x80) {
                int h = sprite_atlas[sprite_id].h;
                h_frac = static_cast<uint8_t>((h > 0 ? (h - 1) : 0) * 8);
            }
            int head_abs = static_cast<int>(obj.y.whole) * 256 +
                           static_cast<int>(obj.y.fraction);
            int feet_abs = head_abs + h_frac;
            uint8_t head_tile_y = static_cast<uint8_t>((head_abs >> 8) & 0xff);
            uint8_t feet_tile_y = static_cast<uint8_t>((feet_abs >> 8) & 0xff);
            for (int ty = head_tile_y;
                 ty != static_cast<uint8_t>(feet_tile_y + 1);
                 ty = static_cast<uint8_t>(ty + 1)) {
                Behaviors::trigger_invisible_switch_at(
                    obj, obj.x.whole, static_cast<uint8_t>(ty),
                    object_mgr_, landscape_);
            }
        }

        // &1dd6-&1dda end-of-update touching clear. The 6502 ORAs
        // #&80 into objects_touching before STA-ing it back to
        // persistent storage every slot, every frame — so the next
        // frame's load starts from "not touching" UNLESS a later
        // slot's check_for_collisions symmetrically stamps it
        // (&2b22-&2b24). Static slots (doors, switches, transporters)
        // skip the 6502's own check_for_collisions entirely; their
        // touching reflects only those symmetric stamps. Without this
        // clear in our port, an object that briefly overlapped a
        // static door left its slot pinned there forever — door.
        // touching stayed positive, update_door's "closing + touched
        // → speed = 0xff" clamp at &4cea fired every frame, and the
        // door closed at -1 (≈5s) instead of the table value (≈0.3s).
        obj.touching |= 0x80;

        // Step 18: Clear creation flags
        obj.flags &= ~ObjectFlags::NOT_PLOTTED;
        obj.flags &= ~ObjectFlags::NEWLY_CREATED;
    }
}
