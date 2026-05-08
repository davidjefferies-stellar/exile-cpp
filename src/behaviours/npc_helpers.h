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
    uint8_t whistle_two_activator; // Slot of object that played whistle two (0xff = none)
    // Per-whistle "collected" flags (ports of &0816 / &0817). update_
    // collectable sets these to true when the corresponding WHISTLE_ONE /
    // WHISTLE_TWO primary ends up in the player's held slot — mirrors the
    // 6502's DEC player_collected[whistle_type] at &4b90. Game::apply_
    // player_input then gates Y / U playback on these flags so an
    // uncollected whistle is silent.
    bool* whistle_one_collected;
    bool* whistle_two_collected;
    // Pointer to Game::player_mushroom_timers_ [0]=red, [1]=blue. May be null.
    uint8_t* player_mushroom_timers;
    // Pointer to Game::player_keys_collected_ (port of &0806). 8 entries;
    // 0x80 in entry N means key N has been picked up. update_door reads
    // this via consider_toggling_lock (&31ac) to decide whether an RCD
    // shot can toggle the door's LOCKED flag; update_collectable writes
    // it when a key primary is collected. May be null for headless/tests.
    uint8_t* player_keys_collected;
    // Particle pool. Behaviors that want to spawn particles call
    // `particles->emit(ParticleType::X, count, obj, rng)`. May be null if
    // the system isn't initialised yet (headless/tests).
    ParticleSystem* particles;
    // Index of the slot the player is currently holding, or 0x80+ if no
    // object is held. Mirrors the 6502's &dd player_object_held byte.
    // update_collectable consults this to know "the player is carrying me
    // right now → mark me collected and remove" (port of &4b88).
    uint8_t held_object_slot;
    // &29d7 player_object_fired — slot the player fired (or 0xff when no
    // object fired this frame). update_remote_control_device (&4351)
    // reads this to emit aim particles; doors and transporters (&4c9e /
    // &4dc8) compare against it via check_if_object_hit_by_remote_control.
    uint8_t player_object_fired;
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

// Flip sprite to face movement direction. Port of &257e flip_object_to_match_
// velocity_x — unconditional. Used by NPCs whose 6502 equivalent calls &257e
// directly (e.g. piranha, wasp).
void face_movement_direction(Object& obj);

// Probability-gated flip. Port of &2578 consider_flipping_object_to_match_
// velocity_x — same as above but only flips 1-in-4 frames on average. Used
// by NPCs whose 6502 path runs through the &2578 entry point (rolling robots,
// hovering robots, clawed robots, green slime, frogmen, imps, big fish,
// worms, maggots). Without this gate, a velocity_x that crosses zero each
// frame (from seek jitter or collision-zeroing) flicks the sprite 180°
// every frame instead of settling.
void consider_face_movement_direction(Object& obj, Random& rng);

// Create a child projectile from this object. Spawns at the parent's
// position; caller must set velocity then call `offset_child_from_parent`
// to push the bullet past the parent's AABB (skipping that step makes
// the bullet spawn inside the parent's tile and explode on impact).
int fire_projectile(Object& obj, ObjectType bullet_type, UpdateContext& ctx);

// Port of create_child_object (&33b8-&342f) X/Y offset. Shifts `child`
// from the parent's origin to the correct spawn point on the firing side
// of the parent, with a relative-velocity pre-compensation so the next
// frame's position is "past the edge". Must be called **after** setting
// child velocity. No-op for non-atlas sprites.
void offset_child_from_parent(Object& child, const Object& parent);

// Produce a firing velocity toward `target` using the 6502's "diamond"
// metric (|vx| + |vy| ≈ speed), so the direction stays proportional to
// the delta rather than snapping to the 8 cardinals. Approximates
// calculate_vector_from_magnitude_and_angle (&2357) via the angle from
// `from` to `target` — good enough for turrets / robots aiming at the
// player without pulling in the full firing-vector chain (&3355).
void aim_toward(int8_t& vel_x, int8_t& vel_y,
                const Object& from, const Object& target, uint8_t speed);

// Port of &22cc calculate_angle_from_velocities. Converts a signed
// (dx, dy) byte pair into the 6502's 8-bit angle convention
// (0x00 = +x, 0x40 = +y, 0x80 = -x, 0xc0 = -y).
uint8_t angle_from_deltas(int8_t dx, int8_t dy);

// Port of &2357 calculate_vector_from_magnitude_and_angle. Produces a
// signed (vx, vy) pair at the given angle with diamond magnitude — i.e.
// |vx| + |vy| ranges from magnitude (at cardinals) to 2*magnitude (at
// 45° angles). Matches the 6502's 5-iteration multiply-by-angle-bits.
void vector_from_magnitude_and_angle(uint8_t magnitude, uint8_t angle,
                                     int8_t& vx, int8_t& vy);

// Port of &3355 calculate_firing_vector_from_distance. The full firing
// chain — produces (vx, vy) aimed at `target` from `from`, with:
//   * angle computed from source/target centres (&22a0)
//   * gravity compensation proportional to flight time (&3362-&338c)
//   * target-velocity leading (&3392-&339a)
//
// `firing_velocity_times_four` is the 6502's packed parameter; the
// effective firing magnitude is velocity/4 and the *4 form is kept for
// the final "not too fast" sanity check (&33a2).
//
// Returns true if the shot is viable. Returns false (leaving vx/vy
// untouched) if the target is >=16 tiles away (&335c out-of-range) or
// the final speed exceeds the cap (&33a2).
bool compute_firing_vector(const Object& from, const Object& target,
                           uint8_t firing_velocity_times_four,
                           int8_t& vx, int8_t& vy);

// Top-level firing helper matching &278a fire_at_target: picks a random
// firing velocity in the 6502's `[0xb4, 0xf3]/4` range, then calls
// compute_firing_vector. Returns true if the shot was computed; false
// if RNG chose not to fire or the target is unreachable.
bool fire_at_target(const Object& from, const Object& target,
                    Random& rng, int8_t& vx, int8_t& vy);

// Port of &2555 update_sprite_offset_using_velocities. Picks the larger
// of |velocity_x|/|velocity_y|, divides by 16, adds 1 + obj.timer, and
// reduces modulo `modulus`. The result replaces obj.timer and is returned
// — makes animation frames advance faster when the object is moving
// faster, which birds / wasps / piranhas all rely on.
uint8_t update_sprite_offset_using_velocities(Object& obj, uint8_t modulus);

// Port of &3292 change_object_sprite_to_base_plus_A. Looks up the base
// sprite for this object's type in `object_types_sprite` and sets
// obj.sprite = base + offset. Used for directional / walk-cycle frames.
void change_object_sprite_to_base_plus_A(Object& obj, uint8_t offset);

// Port of the slime's &47fd-&4801 path (STA sprite + JMP &32aa). Sets
// the sprite as base + offset and centres only on X — the ceiling-
// mounted creatures (red slime) must keep their y-anchor pinned.
void change_object_sprite_x_only(Object& obj, uint8_t offset);

// Port of &321f dampen_this_object_velocities_twice. Halves both axes
// of velocity via arithmetic shift right, twice. Used by birds when
// they wander into water.
void dampen_velocities_twice(Object& obj);

// Reduced port of consider_absorbing_object_touched (&3be1). If `obj` is
// touching another primary of `prey_type`, mark that primary
// PENDING_REMOVAL and play the low-beep "absorbed" sound (&14ad,
// 5d 04 ff 05) at `obj`'s position. We skip the &3bd5
// check_object_touching_angle gate the 6502 applies — fine for tight-
// AABB predators (bird/fish) where any touching pair is a viable bite.
// Used by birds eating wasps and big fish eating piranhas.
void consider_absorbing_object_touched(Object& obj, ObjectType prey_type,
                                       ObjectManager& mgr);

// Reduced port of consider_finding_target (&3bf8) for the simplest
// "nearest object of type X" case (Y=0 / no range gate). Walks the
// primary slots, picks the active object of `prey_type` with the
// smallest Chebyshev distance, and stamps its slot into
// obj.target_and_flags with DIRECTNESS_ONE. No-op when
// ctx.every_sixteen_frames is false. update_npc_path picks up the new
// target on the next path tick. Used by birds → wasps and big fish →
// piranhas.
void consider_finding_target(Object& obj, ObjectType prey_type,
                             UpdateContext& ctx);

// Port of &31da move_towards_target_with_probability_X in a reduced form.
// Each frame with probability X/256 (X is the threshold byte), nudges
// velocity towards the object's current target (obj.target_and_flags
// slot) by up to `max_accel` and at most `magnitude` apart. Called with
// fixed X, magnitude, max_accel so different creatures get different
// responsiveness.
void move_towards_target_with_probability(Object& obj, UpdateContext& ctx,
                                          uint8_t magnitude,
                                          uint8_t max_accel,
                                          uint8_t prob_threshold);

} // namespace NPC
