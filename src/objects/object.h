#pragma once
#include "core/types.h"
#include "core/fixed_point.h"

// Primary object slot - mirrors the parallel arrays at &0860-&0976.
struct Object {
    ObjectType type       = ObjectType::PLAYER;
    Fixed8_8   x          = {};       // &0880 (fraction) + &0891 (whole)
    Fixed8_8   y          = {};       // &08a3 (fraction) + &08b4 (whole)
    uint8_t    flags      = 0;        // &08c6: ObjectFlags bits
    uint8_t    palette    = 0;        // &08d6
    int8_t     velocity_x = 0;        // &08e6
    int8_t     velocity_y = 0;        // &08f6
    uint8_t    sprite     = 0;        // &0870
    uint8_t    target_and_flags = 0;  // &0906: directness + avoid + target slot
    uint8_t    tx         = 0;        // &0916: target/teleport tile x
    uint8_t    energy     = 0xff;     // &0926: 0 = exploding
    uint8_t    ty         = 0;        // &0936: target/teleport tile y
    uint8_t    touching   = 0x80;     // &0946: slot of touching object, 0x80+ = none
    uint8_t    timer      = 0;        // &0956: type-dependent
    uint8_t    tertiary_data_offset = 0; // &0966: objects_data byte (door
                                         // locked/opening flags, switch effect
                                         // id + toggle mask, explosion
                                         // duration counter, etc.) — despite
                                         // the legacy name this is the DATA
                                         // byte, not an offset.
    uint8_t    state      = 0;        // &0976: type-dependent (NPC mood, etc.)
    // Tertiary entry index this primary spawned from. Port-only — the
    // 6502 rederives from tile position at demotion. 0 = none; entry 0
    // is a reserved sentinel. uint16_t for Option B per-cell entries.
    uint16_t   tertiary_slot = 0;
    // Transient collision flags, written by object-update physics at step 15
    // and read by the next frame's type-specific updater. Mirrors the 6502
    // zero-page scratch at &1b (tile_top_or_bottom_collision) and its x
    // counterpart at &1c — in our port we store them per-object because
    // our update order is (update_fn, physics) rather than (physics, update_fn),
    // so the bullet updater reads the previous frame's flag.
    bool       tile_collision = false;  // true if axis-separated move was undone
    // 6502 &19 this_object_any_bottom_collision = &18 | &29e5. Set when
    // this frame's tile OR object collision pushed up; read by
    // check_demotion (&1bd6) to bump stationary supported objects into
    // the tighter 4-tile demote radius.
    bool       bottom_collision = false;
    // 6502 &2b this_object_visibility. Set true at start of each object
    // update (&1ae1 STX with X=0xff); update_invisible_bird (&462f) and
    // update_invisible_frogman (&4475) LSR it to hide when undamaged.
    // Renderer skips draw if false.
    bool       visible = true;
    // Port-only "has-left-home" latch for update_imp's at-home despawn
    // gate. Stored separately from obj.timer because &2557 update_sprite_
    // offset_using_velocities overwrites timer each frame.
    bool       has_left_home = false;
    // Max(|vx|,|vy|) at the moment of a tile collision, captured BEFORE
    // the bounce-reflect / damp pass. Mirrors the 6502's &1d
    // this_object_pre_collision_velocity_magnitude (set at &30b7).
    // update_full_flask reads it via the >= 0x14 "hit tile hard" check —
    // post-revert velocity alone is too spiky (a modest fall bounces to
    // ~0x10 which fires a smaller threshold but the unbounced velocity
    // was still tame).
    uint8_t    pre_collision_magnitude = 0;
    // 6502 &1e this_object_pre_collision_velocity_angle. Captured at
    // &30b6 right before the tile-collision bounce reflects the
    // velocity. Drives the player's "knocked-spinning" rotation in
    // update_rotating_player (&3814-&3856) — the angle of incidence
    // chooses which way to tumble.
    uint8_t    pre_collision_angle = 0;
    // 6502 &50/&54/&52/&56 previous_(x_fraction|x|y_fraction|y). Captured
    // from the object table at &1a27-&1a33 at the start of each object
    // update; the per-type updater can revert position from this snapshot
    // via &28aa set_position_from_previous_position (used by placeholders
    // and disturb-pinned collectables).
    Fixed8_8   prev_x = {};
    Fixed8_8   prev_y = {};

    bool is_active() const { return y.whole != 0; }
    bool is_flipped_h() const { return flags & ObjectFlags::FLIP_HORIZONTAL; }
    bool is_flipped_v() const { return flags & ObjectFlags::FLIP_VERTICAL; }
    bool is_supported() const { return flags & ObjectFlags::SUPPORTED; }

    uint8_t weight() const;  // Determined from object_types_flags_table
};

// Compact storage for secondary objects (offscreen but remembered)
struct SecondaryObject {
    uint8_t type = 0;
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t energy_and_fractions = 0; // Packed: energy high bits + position fractions
};

// Tertiary objects: world fixtures
struct TertiaryObject {
    uint8_t x = 0;
    uint8_t tile_and_flip = 0;
    uint8_t data = 0;   // Creature count, door state, etc.
    uint8_t type = 0;   // Object type when promoted to primary
};
