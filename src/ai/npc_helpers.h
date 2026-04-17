#pragma once
#include "objects/object.h"
#include "objects/object_manager.h"
#include "core/random.h"
#include "world/landscape.h"

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
    // Pointer to Game::player_mushroom_timers_ [0]=red, [1]=blue. May be null.
    uint8_t* player_mushroom_timers;
    // Particle pool. Behaviors that want to spawn particles call
    // `particles->emit(ParticleType::X, count, obj, rng)`. May be null if
    // the system isn't initialised yet (headless/tests).
    ParticleSystem* particles;
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

// Check if object is underwater
bool is_underwater(const Object& obj);

// Apply damage to player if touching
void damage_player_if_touching(Object& obj, Object& player, uint8_t damage);

// Check if object has minimum energy, gain if below
void enforce_minimum_energy(Object& obj, uint8_t min_energy);

// Simple NPC targeting: set velocity toward player
void seek_player(Object& obj, const Object& player, int8_t speed);

// NPC avoidance: set velocity away from player
void flee_player(Object& obj, const Object& player, int8_t speed);

// Flip sprite to face movement direction
void face_movement_direction(Object& obj);

// Create a child projectile from this object
int fire_projectile(Object& obj, ObjectType bullet_type, UpdateContext& ctx);

} // namespace NPC
