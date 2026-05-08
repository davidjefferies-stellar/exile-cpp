#pragma once
#include <cstdint>

class Landscape;
class ObjectManager;
struct Object;

// Faithful port of the 6502 tile-collision response chain at
// &2f8c - &30df. Replaces the axis-separated revert + post-frame
// grounded-snap previously living in player_motion.cpp / object_update.cpp.
//
// The 6502 walks the AABB section by section, counting obstruction
// depths on all four edges, builds a vector from the depth differences,
// converts that to an angle, pushes the object out of obstruction along
// the most-obstructed axis, and reflects velocity around the surface
// normal. Slopes / walls / floors / ceilings all fall out of the same
// pipeline.
//
// Used by both the player (Game::integrate_player_motion) and primaries
// (Game::update_objects step 15), so a single response covers both.
namespace TileCollision {

struct Result {
    // Set when at least one of the top/bottom edge sections was
    // obstructed. Mirrors the 6502's &1b this_object_tile_top_or_
    // bottom_collision; bullets read it to explode on impact.
    bool top_or_bottom_collision = false;

    // The object was entirely inside tiles (port of &3041-&3045 +
    // halve_object_velocities_and_clear_obstructions at &3047). Caller
    // may want to flag this for stuck-NPC handling.
    bool surrounded = false;

    // The object collided with any tile this frame.
    bool collided = false;

    // Pre-collision velocity magnitude (for the bullet pre_collision_
    // magnitude). Mirrors &1d this_object_pre_collision_velocity_magnitude.
    uint8_t pre_collision_magnitude = 0;

    // Bit 7 of the 6502's &18 tile_collision_y_flags — set when the
    // collision was "more to the bottom" of the AABB (i.e. the object
    // landed on something). Player code uses this to drive SUPPORTED;
    // bullets / NPCs read it via top_or_bottom_collision instead.
    bool landed_on_bottom = false;
};

// Run the 6502 collision-response chain on `obj`. Position, velocity,
// and tile_collision are mutated in-place. `prev_*` is the position
// before this frame's velocity integration (the 6502's
// &54-&52 this_object_previous_x/y); the surrounded fallback reverts to
// it.
//
// `skip_slot` is the held-object slot to exclude from door substitution
// look-ups (so a vertical door you're holding doesn't read its own data
// byte). Pass -1 if none.
Result resolve(Object& obj,
               uint8_t prev_x_whole, uint8_t prev_x_frac,
               uint8_t prev_y_whole, uint8_t prev_y_frac,
               const Landscape& landscape,
               ObjectManager& mgr,
               int skip_slot = -1);

} // namespace TileCollision
