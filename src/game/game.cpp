#include "game/game.h"
#include "objects/physics.h"
#include "objects/collision.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include "objects/held_object.h"
#include "rendering/sprite_atlas.h"
#include "ai/behavior_dispatch.h"
#include "objects/weapon.h"
#include "world/wind.h"
#include "world/water.h"
#include "particles/particle_system.h"
#include "rendering/debug_names.h"
#include <chrono>
#include <thread>
#include <cstdio>
#include <string>

// Result of a tertiary lookup: the tile_type|flip byte to render, plus
// the offsets needed to spawn the tertiary's object if applicable.
// tertiary_index == -1 means no tertiary matched and the tile was
// filled from feature_tiles_table.
struct ResolvedTile {
    uint8_t tile_and_flip;
    int tertiary_index;
    int data_offset;
    int type_offset;
};

// Port of &1715 get_tile_and_check_for_tertiary_objects: tiles 0x00-0x08
// are "CHECK_TERTIARY_OBJECT_RANGE_N" markers. For those we scan the
// tertiary-object x list in range [tertiary_ranges[T], tertiary_ranges[T+1])
// for a matching tile_x. If found, the tertiary object's own
// tile+flip byte replaces the marker; otherwise feature_tiles_table
// supplies a default tile (keeping the landscape's original flip).
static ResolvedTile resolve_tile_with_tertiary(const Landscape& landscape,
                                               uint8_t tile_x, uint8_t tile_y) {
    ResolvedTile r{};
    r.tertiary_index = -1;
    r.data_offset = 0;
    r.type_offset = 0;

    uint8_t raw = landscape.get_tile(tile_x, tile_y);
    uint8_t tile_type = raw & TileFlip::TYPE_MASK;

    if (tile_type > 0x08) {
        r.tile_and_flip = raw;
        return r;
    }

    int range_start = tertiary_ranges[tile_type];
    int range_end   = tertiary_ranges[tile_type + 1];
    int found = -1;
    for (int i = range_start; i < range_end; i++) {
        if (tertiary_objects_x_data[i] == tile_x) { found = i; break; }
    }

    if (found < 0) {
        r.tile_and_flip = feature_tiles_table[tile_type] | (raw & TileFlip::MASK);
        return r;
    }

    r.tertiary_index = found;
    // Data/type offsets: 8-bit modular adds against signed offset tables,
    // exactly matching &1749-&1752 in the disassembly.
    r.data_offset = static_cast<uint8_t>(
        found + static_cast<int8_t>(tertiary_data_offset[tile_type]));
    r.type_offset = static_cast<uint8_t>(
        r.data_offset + static_cast<int8_t>(tertiary_type_offset[tile_type]));
    r.tile_and_flip = tertiary_objects_tile_and_flip_data[found];
    return r;
}

// Port of the body of create_primary_object_from_tertiary (&4042) combined
// with the type-selection logic from the individual tile update routines
// (update_metal_door_tile &3e98, update_stone_door_tile &3e95,
// update_switch_tile &3fcd, update_tile_with_object_from_type &3fb7,
// update_tile_with_object_from_data &3fbf, update_transporter_tile &3ee3).
//
// For tiles that spawn objects from tertiary data, picks the correct object
// type, creates a primary object at (tile_x, tile_y) with a sub-tile offset
// matching the 6502's flip-aware formula, copies the tile's flip bits onto
// the object's flags, and clears bit 7 of the tertiary data byte so the
// object doesn't respawn.
void Game::spawn_tertiary_object(uint8_t tile_type, uint8_t tile_flip,
                                 uint8_t tile_x, uint8_t tile_y,
                                 int data_offset, int type_offset) {
    // If the tile has a data byte, bit 7 must still be set to spawn.
    if (data_offset != 0 &&
        !(object_mgr_.tertiary_data_byte(data_offset) & 0x80)) {
        return;
    }

    bool vertical_door = (tile_flip == TileFlip::HORIZONTAL) ||
                         (tile_flip == TileFlip::VERTICAL);

    uint8_t obj_type = 0xff;
    switch (tile_type) {
        case 0x01:  // TILE_TRANSPORTER -> OBJECT_TRANSPORTER_BEAM
            obj_type = 0x41;
            break;
        case 0x02:  // TILE_SPACE_WITH_OBJECT_FROM_DATA
            obj_type = object_mgr_.tertiary_data_byte(data_offset) & 0x7f;
            break;
        case 0x03:  // TILE_METAL_DOOR -> OBJECT_HORIZONTAL/VERTICAL_METAL_DOOR
            obj_type = static_cast<uint8_t>(0x3c + (vertical_door ? 1 : 0));
            break;
        case 0x04:  // TILE_STONE_DOOR -> OBJECT_HORIZONTAL/VERTICAL_STONE_DOOR
            obj_type = static_cast<uint8_t>(0x3e + (vertical_door ? 1 : 0));
            break;
        case 0x05:  // TILE_STONE_HALF_WITH_OBJECT_FROM_TYPE
        case 0x06:  // TILE_SPACE_WITH_OBJECT_FROM_TYPE
        case 0x07:  // TILE_GREENERY_WITH_OBJECT_FROM_TYPE
            if (type_offset > 0 &&
                type_offset < static_cast<int>(sizeof(tertiary_objects_type_data))) {
                obj_type = tertiary_objects_type_data[type_offset];
            }
            break;
        case 0x08:  // TILE_SWITCH -> OBJECT_SWITCH
            obj_type = 0x42;
            break;
        default:
            return;  // Not an object-spawning tile type
    }

    if (obj_type >= static_cast<uint8_t>(ObjectType::COUNT)) return;

    // Compute sub-tile placement from the flip bits (matches &4069-&407e).
    // The 6502 stores (pixels-1)*16 and (rows-1)*8 in its sprite size tables;
    // we reproduce that from the atlas entry's pixel dimensions.
    uint8_t sprite_id = object_types_sprite[obj_type];
    uint8_t width_byte  = 0;
    uint8_t height_byte = 0;
    if (sprite_id <= 0x7c) {
        const SpriteAtlasEntry& e = sprite_atlas[sprite_id];
        width_byte  = static_cast<uint8_t>((e.w > 0 ? (e.w - 1) : 0) * 16);
        height_byte = static_cast<uint8_t>((e.h > 0 ? (e.h - 1) : 0) * 8);
    }
    uint8_t x_frac = (tile_flip & TileFlip::HORIZONTAL)
        ? static_cast<uint8_t>(0u - width_byte)
        : 0;
    uint8_t y_frac = (tile_flip & TileFlip::VERTICAL)
        ? 0
        : static_cast<uint8_t>(0u - height_byte);

    int slot = object_mgr_.create_object(
        static_cast<ObjectType>(obj_type), 0,
        tile_x, x_frac,
        tile_y, y_frac);

    if (slot >= 0) {
        Object& obj = object_mgr_.object(slot);
        // Copy tile flip bits to object flags.
        obj.flags = static_cast<uint8_t>(
            (obj.flags & ~(ObjectFlags::FLIP_HORIZONTAL | ObjectFlags::FLIP_VERTICAL)) |
            (tile_flip & (ObjectFlags::FLIP_HORIZONTAL | ObjectFlags::FLIP_VERTICAL)));
        // Remember the tertiary slot so return_to_tertiary can set bit 7
        // again when this object is demoted offscreen (port of &4081-&4083).
        obj.tertiary_data_offset = static_cast<uint8_t>(data_offset);
    }

    // Mark tertiary as spawned so it isn't created again every frame.
    object_mgr_.clear_tertiary_spawn_bit(data_offset);
}

Game::Game(std::unique_ptr<IRenderer> renderer)
    : renderer_(std::move(renderer)) {
}

bool Game::init() {
    if (!renderer_->init()) return false;

    // Initialize object manager
    object_mgr_.init();

    // Initialize player in slot 0
    Object& player = object_mgr_.player();
    player.type = ObjectType::PLAYER;
    player.x = {GameConstants::PLAYER_START_X, 0x00};
    player.y = {GameConstants::PLAYER_START_Y, 0x00};
    player.sprite = object_types_sprite[0];
    player.palette = object_types_palette_and_pickup[0] & 0x7f;
    player.energy = 0xff;
    player.flags = 0;

    // Seed RNG
    rng_.seed(0x49, 0x52, 0x56, 0x49);

    running_ = true;
    return true;
}

void Game::run() {
    using clock = std::chrono::steady_clock;
    auto frame_duration = std::chrono::milliseconds(GameConstants::FRAME_TIME_MS);

    while (running_) {
        auto frame_start = clock::now();

        // Main game loop sequence (matching &19b6)
        update_timers();
        process_input();
        update_player();
        update_objects();

        // Decrement mushroom timers (port of &19d4-&19dd)
        for (int i = 0; i < 2; i++) {
            if (player_mushroom_timers_[i] > 0) {
                player_mushroom_timers_[i]--;
            }
        }

        // Tick the particle pool (port of &207e update_particles).
        {
            const Object& p = object_mgr_.player();
            uint8_t wy = Water::get_waterline_y(p.x.whole);
            particles_.update(wy, 0, rng_);
        }

        render();

        // Frame timing: sleep to maintain 50fps
        auto frame_end = clock::now();
        auto elapsed = frame_end - frame_start;
        if (elapsed < frame_duration) {
            std::this_thread::sleep_for(frame_duration - elapsed);
        }
    }
}

// Port of &19b6-&19c7: LSR chain sets timer negative when low bits are all zero.
// Timer fires (is "negative"/true) when frame_counter is on that boundary.
// every_two_frames fires on even frames (bit 0 = 0),
// every_four_frames fires when bits 0-1 are 0, etc.
void Game::update_timers() {
    // Clear per-frame flags (port of &19b6: LSR &27)
    whistle_one_active_ = false;

    frame_counter_++;
    uint8_t a = frame_counter_;

    every_two_frames_        = (a & 0x01) == 0;
    every_four_frames_       = (a & 0x03) == 0;
    every_eight_frames_      = (a & 0x07) == 0;
    every_sixteen_frames_    = (a & 0x0f) == 0;
    every_thirty_two_frames_ = (a & 0x1f) == 0;
    every_sixty_four_frames_ = (a & 0x3f) == 0;
}

void Game::process_input() {
    input_.clear();

    int key;
    while ((key = renderer_->get_key()) != InputKey::NONE) {
        input_.process_key(key);
    }

    if (input_.state().quit) {
        running_ = false;
    }
}

void Game::update_player() {
    Object& player = object_mgr_.player();

    int8_t accel_x = 0;
    int8_t accel_y = 0;
    const auto& inp = input_.state();

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

    // Weapon firing
    if (inp.fire && held_object_slot_ >= 0x80) {
        Weapon::fire(object_mgr_, player, player_weapon_, player_aim_angle_,
                     weapon_energy_[player_weapon_]);
    }

    // Pickup/drop
    if (inp.pickup_drop) {
        if (held_object_slot_ < 0x80) {
            // Drop held object
            HeldObject::drop(object_mgr_.object(held_object_slot_), player, held_object_slot_);
        } else {
            // Try to pick up a touching object
            if (player.touching < GameConstants::PRIMARY_OBJECT_SLOTS) {
                Object& touched = object_mgr_.object(player.touching);
                if (HeldObject::is_pickupable(touched.type)) {
                    HeldObject::pickup(touched, player, held_object_slot_, player.touching);
                }
            }
        }
    }

    // Weapon select
    if (inp.weapon_select < 6) {
        player_weapon_ = inp.weapon_select;
    }

    // Apply wind (surface only)
    Wind::apply_surface_wind(player);

    // Apply physics
    Physics::apply_acceleration(player, accel_x, accel_y, every_sixteen_frames_);

    // Integrate velocity axis-by-axis, undoing a step if it would place the
    // player's position point inside solid geometry. This keeps the player
    // pinned against surfaces instead of being teleported a whole tile when
    // the old collision code did `whole--` every frame.
    {
        Fixed8_8 old_x = player.x;
        player.x.add_velocity(player.velocity_x);
        if (Collision::is_tile_solid(landscape_, player.x.whole, player.y.whole)) {
            player.x = old_x;
            player.velocity_x = 0;
        }
    }
    {
        Fixed8_8 old_y = player.y;
        player.y.add_velocity(player.velocity_y);
        if (Collision::is_tile_solid(landscape_, player.x.whole, player.y.whole)) {
            player.y = old_y;
            if (player.velocity_y > 0) {
                // Landed on ground
                player.flags |= ObjectFlags::SUPPORTED;
            }
            player.velocity_y = 0;
        }
    }

    // Apply water effects
    Water::apply_water_effects(player, player.weight());

    // Refresh support state: is there a solid tile directly below?
    if (Collision::is_tile_solid(landscape_,
                                 player.x.whole,
                                 static_cast<uint8_t>(player.y.whole + 1))) {
        player.flags |= ObjectFlags::SUPPORTED;
    } else {
        player.flags &= ~ObjectFlags::SUPPORTED;
    }

    // Object-object collision for player
    auto obj_coll = Collision::check_object_collision(
        player, 0,
        reinterpret_cast<const std::array<Object, 16>&>(object_mgr_.object(0)));
    if (obj_coll.collided) {
        player.touching = static_cast<uint8_t>(obj_coll.other_slot);
    } else {
        player.touching = 0x80;
    }

    // Update camera
    camera_.follow_player(player.x.whole, player.y.whole);

    update_player_sprite(accel_x, accel_y);
}

// Port of &22d4 calculate_angle_from_vector. Returns the 8-bit angle of a
// 2D vector, where 0xc0 = "head up" (negative y), 0x40 = "head down",
// 0x00 = pointing right, 0x80 = pointing left. The algorithm divides the
// smaller absolute component by the larger to get a five-bit slope, then
// XORs an octant offset from the sign/magnitude half-quadrant table at &14bf.
static uint8_t angle_from_vector(int8_t vx, int8_t vy) {
    bool y_pos = vy >= 0;
    bool x_pos = vx >= 0;
    uint8_t ay = static_cast<uint8_t>(y_pos ?  int(vy) : -int(vy));
    uint8_t ax = static_cast<uint8_t>(x_pos ?  int(vx) : -int(vx));

    bool x_ge_y = ax >= ay;
    uint8_t magnitude = x_ge_y ? ax : ay;
    uint8_t small     = x_ge_y ? ay : ax;

    // 8-bit angle starts as 0x08 and is rotated left with division bits
    // until the sentinel overflows (after 5 meaningful iterations).
    uint8_t angle = 0x08;
    if (magnitude != 0) {
        while (true) {
            bool carry_in = (small & 0x80) != 0;
            small = static_cast<uint8_t>(small << 1);
            bool ge = carry_in || small >= magnitude;
            if (ge) small = static_cast<uint8_t>(small - magnitude);
            bool sentinel_out = (angle & 0x80) != 0;
            angle = static_cast<uint8_t>((angle << 1) | (ge ? 1 : 0));
            if (sentinel_out) break;
        }
    } else {
        angle = 0;
    }

    static constexpr uint8_t HALF_QUADRANT[8] = {
        0xbf, 0x80, 0xc0, 0xff, 0x40, 0x7f, 0x3f, 0x00,
    };
    int idx = (x_ge_y ? 1 : 0) | (x_pos ? 2 : 0) | (y_pos ? 4 : 0);
    return static_cast<uint8_t>(angle ^ HALF_QUADRANT[idx]);
}

// Port of &3906 set_spacesuit_sprite_from_angle. Quantises the current
// player angle into 8 half-quadrants, derives x/y flip bits from the
// half-quadrant, then picks one of four angled sprites (HORIZONTAL,
// FORTY_FIVE_HEAD_UP, JUMPING, FORTY_FIVE_HEAD_DOWN). For the vertical
// quadrant the sprite is chosen by walking/standing state in the usual
// way.
static void set_spacesuit_sprite_from_angle(Object& player,
                                            uint8_t angle,
                                            uint8_t x_flip_in) {
    // Five LSRs + ADC #&00 — divide by 32 with carry rounding up.
    uint8_t a = angle;
    bool carry = false;
    for (int i = 0; i < 5; ++i) {
        carry = (a & 1) != 0;
        a = static_cast<uint8_t>(a >> 1);
    }
    uint8_t hq = static_cast<uint8_t>(a + (carry ? 1 : 0));
    bool adc00_carry = (a == 0xff && carry); // effectively never; carry stays 0
    (void)adc00_carry;

    // If facing right (x_flip_in bit 7 clear), reverse the sequence and
    // add 1 (the original ADC #&01 carries the previous ADC's carry,
    // which is 0 here since hq ≤ 0x07).
    if (!(x_flip_in & 0x80)) {
        hq = static_cast<uint8_t>((hq ^ 0x07) + 1);
    }
    // hq now in 0..8; only low 3 bits matter for the quadrant lookup.

    // Derive x_flip = 0x80 when bit 2 of hq is set, y_flip = x_flip XOR x_flip_in.
    uint8_t x_flip = (hq & 0x04) ? 0x80 : 0x00;
    uint8_t y_flip = static_cast<uint8_t>(x_flip ^ x_flip_in);

    uint8_t quadrant = static_cast<uint8_t>(hq & 0x03);

    uint8_t sprite;
    if (quadrant != 2) {
        // Angled sprite — 0 HORIZONTAL, 1 45_HEAD_UP, 3 45_HEAD_DOWN.
        sprite = quadrant;
    } else {
        // Vertical: standing / jumping / walking.
        int abs_vx = player.velocity_x >= 0 ? player.velocity_x : -player.velocity_x;
        bool supported = (player.flags & ObjectFlags::SUPPORTED) != 0;
        if ((abs_vx >> 1) == 0) {
            sprite = 0x04; // SPACESUIT_VERTICAL (standing)
        } else if (!supported) {
            sprite = 0x02; // SPACESUIT_JUMPING
        } else {
            // Walking: advance timer using &2555 update_sprite_offset_using_velocities.
            int abs_vy = player.velocity_y >= 0 ? player.velocity_y : -player.velocity_y;
            int max_vel = abs_vx > abs_vy ? abs_vx : abs_vy;
            uint8_t increment = static_cast<uint8_t>(1 + (max_vel >> 4));
            player.timer = static_cast<uint8_t>((player.timer + increment) & 0x07);
            uint8_t stage = static_cast<uint8_t>(player.timer >> 1); // 0..3

            // If walking opposite to facing, reverse the animation.
            bool moving_left = player.velocity_x < 0;
            bool facing_left = (x_flip & 0x80) != 0;
            if (moving_left != facing_left) stage ^= 0x03;

            sprite = static_cast<uint8_t>(0x04 + stage);
        }
    }

    player.sprite = sprite;

    if (x_flip & 0x80) player.flags |= ObjectFlags::FLIP_HORIZONTAL;
    else               player.flags &= ~ObjectFlags::FLIP_HORIZONTAL;
    if (y_flip & 0x80) player.flags |= ObjectFlags::FLIP_VERTICAL;
    else               player.flags &= ~ObjectFlags::FLIP_VERTICAL;
}

// Port of &3795 update_player_angle_facing_and_sprite (stripped down).
// Computes a target body angle from the current acceleration vector and
// slews the player's angle toward it at a quarter of the deviation per
// frame. Updates the facing direction from horizontal acceleration, then
// hands off to set_spacesuit_sprite_from_angle.
void Game::update_player_sprite(int8_t accel_x, int8_t accel_y) {
    Object& player = object_mgr_.player();
    bool supported = (player.flags & ObjectFlags::SUPPORTED) != 0;

    uint8_t target_angle;
    if ((accel_x != 0 || accel_y != 0) && !supported) {
        // Airborne thrust: rotate to align with motion. The 6502 negates
        // acceleration into a facing vector here; we use -accel directly
        // so the head leads the thrust direction.
        target_angle = angle_from_vector(static_cast<int8_t>(-accel_x),
                                         static_cast<int8_t>(-accel_y));
    } else {
        target_angle = 0xc0; // upright (head up)
    }

    // Slew: angle += deviation / 4 (signed arithmetic shift).
    int8_t deviation = static_cast<int8_t>(target_angle - player_angle_);
    int8_t delta = static_cast<int8_t>(deviation / 4);
    player_angle_ = static_cast<uint8_t>(player_angle_ + delta);

    if (accel_x != 0) {
        player_facing_ = (accel_x < 0) ? 0x80 : 0x00;
    }

    set_spacesuit_sprite_from_angle(player, player_angle_, player_facing_);
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

        // Step 3: Handle held objects
        if (slot == held_object_slot_) {
            HeldObject::update_position(obj, player);
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
                }
                obj.timer--;
                continue; // Skip physics while teleporting
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
                              player_mushroom_timers_,
                              &particles_};
            update_fn(obj, uctx);
        }

        // Step 11: Handle held object dropping
        if (slot == held_object_slot_) {
            if (HeldObject::should_drop(obj, player)) {
                HeldObject::drop(obj, object_mgr_.player(), held_object_slot_);
            }
        }

        // Step 12: Handle explosions
        if (obj.energy == 0) {
            object_mgr_.create_object_at(ObjectType::EXPLOSION, 0, obj);
            object_mgr_.remove_object(slot);
            if (slot == held_object_slot_) held_object_slot_ = 0x80;
            continue;
        }

        // Step 14: Consider tertiary return
        if (obj.flags & ObjectFlags::PENDING_REMOVAL) {
            object_mgr_.return_to_tertiary(slot);
            if (slot == held_object_slot_) held_object_slot_ = 0x80;
            continue;
        }

        // Step 9: Apply wind (only above surface)
        Wind::apply_surface_wind(obj);

        // Step 15: Apply physics (gravity + velocity)
        if (slot != held_object_slot_) {
            // Skip physics for static objects (weight 7)
            if (obj.weight() >= 7) {
                obj.velocity_x = 0;
                obj.velocity_y = 0;
            } else {
                Physics::apply_acceleration(obj, 0, 0, every_sixteen_frames_);

                // Axis-separated integrate + undo-on-overlap.
                {
                    Fixed8_8 old_x = obj.x;
                    obj.x.add_velocity(obj.velocity_x);
                    if (Collision::is_tile_solid(landscape_, obj.x.whole, obj.y.whole)) {
                        obj.x = old_x;
                        obj.velocity_x = 0;
                    }
                }
                {
                    Fixed8_8 old_y = obj.y;
                    obj.y.add_velocity(obj.velocity_y);
                    if (Collision::is_tile_solid(landscape_, obj.x.whole, obj.y.whole)) {
                        obj.y = old_y;
                        if (obj.velocity_y > 0) obj.flags |= ObjectFlags::SUPPORTED;
                        obj.velocity_y = 0;
                    }
                }

                // Apply water effects (buoyancy + damping)
                Water::apply_water_effects(obj, obj.weight());

                if (Collision::is_tile_solid(landscape_, obj.x.whole,
                                             static_cast<uint8_t>(obj.y.whole + 1))) {
                    obj.flags |= ObjectFlags::SUPPORTED;
                } else {
                    obj.flags &= ~ObjectFlags::SUPPORTED;
                }

                // Object-object collision: set touching field
                auto obj_coll = Collision::check_object_collision(
                    obj, slot,
                    reinterpret_cast<const std::array<Object, 16>&>(object_mgr_.object(0)));
                if (obj_coll.collided) {
                    obj.touching = static_cast<uint8_t>(obj_coll.other_slot);
                } else {
                    obj.touching = 0x80; // Not touching anything
                }
            }
        }

        // Step 18: Clear creation flags
        obj.flags &= ~ObjectFlags::NOT_PLOTTED;
        obj.flags &= ~ObjectFlags::NEWLY_CREATED;
    }
}

void Game::render() {
    renderer_->begin_frame();

    // Apply right-drag pan from the renderer (if any).
    int pan_dx = 0, pan_dy = 0;
    if (renderer_->consume_pan_tiles(pan_dx, pan_dy)) {
        camera_.apply_pan(pan_dx, pan_dy);
    }
    // Re-derive view center (player pos + pan).
    const Object& player_obj = object_mgr_.player();
    camera_.follow_player(player_obj.x.whole, player_obj.y.whole);

    renderer_->set_viewport(camera_.center_x, camera_.center_y,
                            player_obj.x.fraction, player_obj.y.fraction);

    // Handle left-click to select a tile and build info overlay.
    int click_dx = 0, click_dy = 0;
    if (renderer_->consume_left_click(click_dx, click_dy)) {
        uint8_t tx = static_cast<uint8_t>(camera_.center_x + click_dx);
        uint8_t ty = static_cast<uint8_t>(camera_.center_y + click_dy);
        uint8_t tile = resolve_tile_with_tertiary(landscape_, tx, ty).tile_and_flip;
        uint8_t ttype = tile & 0x3f;

        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "Tile (%u,%u)\n%s (0x%02x)",
                      tx, ty, tile_type_name(ttype), ttype);
        std::string text(buf);

        int object_count = 0;
        for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            const Object& o = object_mgr_.object(i);
            if (!o.is_active()) continue;
            if (o.x.whole != tx || o.y.whole != ty) continue;
            if (object_count == 0) text += "\nObjects:";
            text += "\n  ";
            text += object_type_name(o.type);
            object_count++;
            if (object_count >= 6) { text += "\n  ..."; break; }
        }
        selected_tile_info_ = text;
    }
    renderer_->set_overlay_text(selected_tile_info_.c_str());

    int vp_w = renderer_->viewport_width_tiles();
    int vp_h = renderer_->viewport_height_tiles();

    // Render visible tiles
    uint8_t start_x = camera_.center_x - static_cast<uint8_t>(vp_w / 2);
    uint8_t start_y = camera_.center_y - static_cast<uint8_t>(vp_h / 2);

    for (int dy = 0; dy < vp_h; dy++) {
        uint8_t wy = static_cast<uint8_t>(start_y + dy);
        for (int dx = 0; dx < vp_w; dx++) {
            uint8_t wx = static_cast<uint8_t>(start_x + dx);
            ResolvedTile res = resolve_tile_with_tertiary(landscape_, wx, wy);
            uint8_t tile      = res.tile_and_flip;
            uint8_t tile_type = tile & TileFlip::TYPE_MASK;
            uint8_t tile_flip = tile & TileFlip::MASK;

            // For tiles that spawn objects from tertiary data, create the
            // primary object on first render (the original does this
            // during tile plotting). The door tile additionally swaps in
            // an equivalent wall tile for the underlying geometry.
            if (res.tertiary_index >= 0) {
                spawn_tertiary_object(tile_type, tile_flip,
                                      wx, wy,
                                      res.data_offset, res.type_offset);
            }

            // Door tile → draw the equivalent wall beneath the door object.
            if (tile_type == 0x03 || tile_type == 0x04) {
                bool vertical = (tile_flip == TileFlip::HORIZONTAL) ||
                                (tile_flip == TileFlip::VERTICAL);
                uint8_t equivalent = vertical ? 0x2a : 0x17;
                tile_type = equivalent;
                // keep flip bits for the wall sprite as stored.
            }

            TileRenderInfo info;
            info.tile_type = tile_type;
            info.flip_h = (tile_flip & TileFlip::HORIZONTAL) != 0;
            info.flip_v = (tile_flip & TileFlip::VERTICAL) != 0;
            info.palette = 0;

            renderer_->render_tile(wx, wy, info);
        }
    }

    // Render active objects
    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        const Object& obj = object_mgr_.object(i);
        if (!obj.is_active()) continue;

        SpriteRenderInfo info;
        info.sprite_id = obj.sprite;
        info.palette = obj.palette;
        info.flip_h = obj.is_flipped_h();
        info.flip_v = obj.is_flipped_v();
        info.visible = true;
        info.type = obj.type;

        renderer_->render_object(obj.x, obj.y, info);
    }

    // Render particles (on top of tiles/objects, below HUD).
    for (int i = 0; i < particles_.count(); i++) {
        const Particle& p = particles_.get(i);
        renderer_->render_particle(p.x, p.x_fraction,
                                   p.y, p.y_fraction,
                                   p.colour_and_flags & 0x07);
    }

    // Render HUD
    PlayerState ps;
    ps.energy = object_mgr_.player().energy;
    ps.weapon = player_weapon_;
    ps.keys_collected = 0;
    ps.has_jetpack_booster = false;
    renderer_->render_hud(ps);

    renderer_->end_frame();
}
