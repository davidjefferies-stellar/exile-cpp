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
    // Tertiary bookkeeping: the offset into ObjectManager::tertiary_data_
    // that this primary was spawned from. Needed so return_to_tertiary can
    // set the spawn gate and write back any switch-toggled state. Not
    // present in the 6502 — there it's rederived from the tile position
    // each time demotion happens. Zero means "not from a tertiary slot".
    uint8_t    tertiary_slot = 0;
    // Transient collision flags, written by object-update physics at step 15
    // and read by the next frame's type-specific updater. Mirrors the 6502
    // zero-page scratch at &1b (tile_top_or_bottom_collision) and its x
    // counterpart at &1c — in our port we store them per-object because
    // our update order is (update_fn, physics) rather than (physics, update_fn),
    // so the bullet updater reads the previous frame's flag.
    bool       tile_collision = false;  // true if axis-separated move was undone
    // Max(|vx|,|vy|) at the moment of a tile collision, captured BEFORE
    // the bounce-reflect / damp pass. Mirrors the 6502's &1d
    // this_object_pre_collision_velocity_magnitude (set at &30b7).
    // update_full_flask reads it via the >= 0x14 "hit tile hard" check —
    // post-revert velocity alone is too spiky (a modest fall bounces to
    // ~0x10 which fires a smaller threshold but the unbounced velocity
    // was still tame).
    uint8_t    pre_collision_magnitude = 0;

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
