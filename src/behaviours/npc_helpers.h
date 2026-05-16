#pragma once
#include "core/damage_visual.h"
#include "objects/object.h"
#include "objects/object_manager.h"
#include "core/random.h"
#include "world/landscape.h"
#include <vector>

class ParticleSystem;

// Shared context passed to all update routines
struct UpdateContext {
    ObjectManager& mgr;
    const Landscape& landscape;
    Random& rng;
    uint8_t frame_counter;
    bool every_four_frames;
    bool every_eight_frames;
    bool every_sixteen_frames;
    bool every_thirty_two_frames;
    bool every_sixty_four_frames;
    // Whistle state
    bool whistle_one_active;       // Whistle one played this frame
    // &29d8 whistle_two_activating_object. Player sets this to 0 on U;
    // red/magenta bird sets it to its own slot via &2c9e. Pointer so
    // behaviours can write it. Reads compare slot < PRIMARY_SLOTS.
    uint8_t* whistle_two_activator;
    // &0816/&0817 whistle-collected flags. update_collectable sets via
    // &4b90 DEC player_collected; apply_player_input gates Y/U playback.
    bool* whistle_one_collected;
    bool* whistle_two_collected;
    // Pointer to Game::player_mushroom_timers_ [0]=red, [1]=blue. May be null.
    uint8_t* player_mushroom_timers;
    // &0806 player_keys_collected. 8 entries (0x80 = picked up); read by
    // update_door via &31ac, written by update_collectable. Nullable.
    uint8_t* player_keys_collected;
    // Particle pool. Behaviors that want to spawn particles call
    // `particles->emit(ParticleType::X, count, obj, rng)`. May be null if
    // the system isn't initialised yet (headless/tests).
    ParticleSystem* particles;
    // Index of the slot the player is currently holding, or 0x80+ if no
    // object is held. Mirrors the 6502's &dd player_object_held byte.
    // update_collectable consults this to know "the player is carrying me
    // right now -> mark me collected and remove" (port of &4b88).
    uint8_t held_object_slot;
    // &29d7 player_object_fired (0xff = nothing fired). Read by
    // &4351, &4c9e, &4dc8 via check_if_object_hit_by_remote_control.
    uint8_t player_object_fired;
    // &34 player_aiming_angle_with_flip. Already mirrored across +y when
    // facing left (&311d EOR #&7f / +1). Fed to &330f for aim-particle
    // velocity; doors/cannons read it via hit-cone tests.
    uint8_t player_aim_angle;
    // Slot index of the object whose update routine is currently running
    // (the 6502's &aa this_object). update_collectable compares against
    // held_object_slot to decide whether the player is carrying it.
    int this_slot;
    // Per-variant remaining-gift counters for fed imps that return home
    // (port of &083a..&083e). 5 entries indexed by tidx = type −
    // RED_MAGENTA_IMP. update_imp decrements when dropping a gift.
    uint8_t* imp_gifts_remaining;
    // &2a background_flash_cooldown — write 0x0b here from a behaviour
    // (e.g. coronium chain explosion at &41e5) to trigger the screen
    // flash. Game::update_background_flash decrements it each frame.
    uint8_t* background_flash_cooldown;
    // Per-frame damage event log. Push a DamageVisual every time a
    // behaviour deals damage; render.cpp draws floating numbers and
    // explosion radius rings when the "Damage" checkbox is on. Null is
    // treated as "no overlay requested" — pushes are skipped.
    std::vector<DamageVisual>* damage_events;
    // &081d explosion_timer (signed: -50..0). Read by NPC stimuli at
    // &282b for "start of explosion" detection.
    const int8_t* explosion_timer;
    // &081e flooding_state (bit 7 = endgame flood active). Read by NPC
    // stimuli at &2834 for the "world flooding" stimulus.
    const uint8_t* flooding_state;
    // Always-on transient text notifications. Behaviours push to this
    // when a rare gameplay event needs visual feedback (e.g. imp fed).
    // Renderer draws regardless of debug-overlay toggles.
    std::vector<FloatingLabel>* floating_labels;
};

// Common NPC movement helpers
namespace NPC {

// Apply gravity cancellation for flying creatures
void cancel_gravity(Object& obj);

// Move toward a target position with given speed
void move_toward(Object& obj, uint8_t target_x, uint8_t target_y, int8_t speed);

// Set sprite based on velocity direction (8-directional)
void set_sprite_from_velocity(Object& obj, uint8_t base_sprite, int num_frames);

// Update walking animation sprite
void animate_walking(Object& obj, uint8_t base_sprite, uint8_t frame_counter);

// Apply damage to player if touching. When damage_events is non-null
// and damage actually lands, push a DamageVisual for the debug overlay.
void damage_player_if_touching(Object& obj, Object& player, uint8_t damage,
                               std::vector<DamageVisual>* damage_events = nullptr);

// Check if object has minimum energy, gain if below
void enforce_minimum_energy(Object& obj, uint8_t min_energy);

// Simple NPC targeting: set velocity toward player
void seek_player(Object& obj, const Object& player, int8_t speed);

// NPC avoidance: set velocity away from player
void flee_player(Object& obj, const Object& player, int8_t speed);

// Flip sprite to face movement direction. Port of &257e
// flip_object_to_match_velocity_x — unconditional:
//   &257e LDA &43 ; velocity_x
//   &2580 BEQ &2584 ; leave_with_this_object_x_flip
//   &2582 STA &37 ; this_object_x_flip
//   &2584 LDA &37 ; this_object_x_flip
//   &2586 RTS
// Used by NPCs whose 6502 equivalent calls &257e directly (e.g. piranha,
// wasp).
void face_movement_direction(Object& obj);

// &2578 consider_flipping_object_to_match_velocity_x. 1-in-4 rng gate
// prevents per-frame zero-crossing sprite flicker.
void consider_face_movement_direction(Object& obj, Random& rng);

// Create a child projectile from this object. Spawns at the parent's
// position; caller must set velocity then call `offset_child_from_parent`
// to push the bullet past the parent's AABB (skipping that step makes
// the bullet spawn inside the parent's tile and explode on impact).
int fire_projectile(Object& obj, ObjectType bullet_type, UpdateContext& ctx);

// &33b8-&342f create_child_object X/Y offset. Must be called AFTER
// setting child velocity. No-op for non-atlas sprites.
void offset_child_from_parent(Object& child, const Object& parent);

// Diamond-metric firing velocity (|vx|+|vy|≈speed). Approximates &2357
// via the from->target angle, cheaper than the full &3355 chain.
void aim_toward(int8_t& vel_x, int8_t& vel_y,
                const Object& from, const Object& target, uint8_t speed);

// Port of &22cc calculate_angle_from_velocities (thin shim into &22d4
// calculate_angle_from_vector):
//   &22cc LDA &43 ; velocity_x
//   &22ce STA &b4 ; vector_x
//   &22d0 LDA &45 ; velocity_y
//   &22d2 STA &b6 ; vector_y
//   &22d4 JSR &233d get_absolute_vector_components
//   …angle_from_vector body…
// Converts (dx, dy) into the 6502's 8-bit angle convention
// (0x00 = +x, 0x40 = +y, 0x80 = -x, 0xc0 = -y).
uint8_t angle_from_deltas(int8_t dx, int8_t dy);

// Port of &2357 calculate_vector_from_magnitude_and_angle. Produces a
// signed (vx, vy) pair at the given angle with diamond magnitude — i.e.
// |vx| + |vy| ranges from magnitude (at cardinals) to 2*magnitude (at
// 45° angles). Matches the 6502's 5-iteration multiply-by-angle-bits.
void vector_from_magnitude_and_angle(uint8_t magnitude, uint8_t angle,
                                     int8_t& vx, int8_t& vy);

// &3355 calculate_firing_vector_from_distance. Includes &22a0 angle,
// &3362-&338c gravity comp, &3392-&339a target leading. Returns false
// if target >=16 tiles (&335c) or final speed exceeds cap (&33a2).
// firing_velocity_times_four is kept *4 for the sanity check at &33a2.
bool compute_firing_vector(const Object& from, const Object& target,
                           uint8_t firing_velocity_times_four,
                           int8_t& vx, int8_t& vy);

// Top-level firing helper matching &278a fire_at_target: picks a random
// firing velocity in the 6502's `[0xb4, 0xf3]/4` range, then calls
// compute_firing_vector. Returns true if the shot was computed; false
// if RNG chose not to fire or the target is unreachable.
bool fire_at_target(const Object& from, const Object& target,
                    Random& rng, int8_t& vx, int8_t& vy);

// &2555 / &2557 update_sprite_offset_using_(scaled_)velocities. The
// scaled entry point lets the caller pick how aggressively to divide
// the velocity; `divide_shift` mirrors the 6502's X register + 1 (i.e.
// the number of LSRs). Default 4 (X=3, /16) matches the &2555 entry.
uint8_t update_sprite_offset_using_velocities(Object& obj, uint8_t modulus,
                                              uint8_t divide_shift = 4);

// Port of &3292 change_object_sprite_to_base_plus_A. Looks up the base
// sprite for this object's type in `object_types_sprite` and sets
// obj.sprite = base + offset. Used for directional / walk-cycle frames.
void change_object_sprite_to_base_plus_A(Object& obj, uint8_t offset);

// Port of the slime's &47fd-&4801 path (STA sprite + JMP &32aa). Sets
// the sprite as base + offset and centres only on X — the ceiling-
// mounted creatures (red slime) must keep their y-anchor pinned.
void change_object_sprite_x_only(Object& obj, uint8_t offset);

// Port of &321f dampen_this_object_velocities_twice:
//   &321f JSR &3222 dampen_this_object_velocities
//         (falls through into &3222 to dampen a second time)
//   &3222 JSR &322d dampen_this_object_velocity_x
//   &3225 LDA &45 ; velocity_y
//   &3227 JSR &3235 calculate_seven_eighths
//   &322a STA &45
//   &322c RTS
// Net effect: applies (7/8)^2 = 49/64 damping to both axes. Used by
// birds when they wander into water.
void dampen_velocities_twice(Object& obj);

// &3be1 consider_absorbing_object_touched (reduced). Plays &14ad
// low-beep. Port-only: skips the &3bd5 glancing-angle gate.
void consider_absorbing_object_touched(Object& obj, ObjectType prey_type,
                                       ObjectManager& mgr);

// &3bf8 consider_finding_target reduced (Y=0 / no range gate). Stamps
// nearest prey_type slot with DIRECTNESS_ONE; only runs every 16
// frames. Used by birds->wasps and big fish->piranhas.
void consider_finding_target(Object& obj, ObjectType prey_type,
                             UpdateContext& ctx);

// &31da move_towards_target_with_probability_X reduced. With prob
// X/256, nudges velocity toward obj.target_and_flags slot by ≤max_accel
// and ≤magnitude.
void move_towards_target_with_probability(Object& obj, UpdateContext& ctx,
                                          uint8_t magnitude,
                                          uint8_t max_accel,
                                          uint8_t prob_threshold);

// &3a1e consider_hovering_over_ground (every-4-frames). Adds upward
// thrust scaled by proximity to ground, then dampens vy by 7/8 so the
// ball settles instead of oscillating. Approximation: 6502 walks tiles
// down via check_for_space_below_object and picks one of three thrust
// magnitudes; we use is_supported() as a proxy for "near ground" and
// rng for the 2-or-3 split. Mid-air (1+ tile clear, < half-height
// clear) is treated as the "1+ tile clear" case since we lack the
// fine-grained measurement — empirically this keeps hover NPCs from
// drifting onto the ground without making them climb the ceiling.
void consider_hovering_over_ground(Object& obj, UpdateContext& ctx);

} // namespace NPC
