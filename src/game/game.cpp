#include "game/game.h"
#include "game/config.h"
#include "objects/object_data.h"
#include "world/water.h"
#include <chrono>
#include <thread>

// -----------------------------------------------------------------------------
// Game — lifecycle, top-level loop orchestration, timers, input polling.
// The big chunks of behaviour live in sibling translation units:
//
//   game/tertiary_spawn.cpp   — spawn_tertiary_object
//   game/player_actions.cpp   — apply_player_input
//   game/player_motion.cpp    — integrate_player_motion
//   game/player_sprite.cpp    — update_player_sprite + angle helpers
//   game/object_update.cpp    — update_objects (18-step primary loop)
//   game/render.cpp           — render
//   world/tertiary.cpp        — resolve_tile_with_tertiary + ResolvedTile
// -----------------------------------------------------------------------------

Game::Game(std::unique_ptr<IRenderer> renderer)
    : renderer_(std::move(renderer)) {
}

bool Game::init() {
    if (!renderer_->init()) return false;

    // Initialize object manager
    object_mgr_.init();

    // Load startup config (player position, energy, weapon, pockets,
    // weapon energies). Missing file → defaults reproducing the original
    // game's spawn state. See exile.ini in the project root.
    StartupConfig cfg = load_startup_config("exile.ini");

    // Initialize player in slot 0
    Object& player = object_mgr_.player();
    player.type = ObjectType::PLAYER;
    player.x = {cfg.start_x, 0x00};
    player.y = {cfg.start_y, 0x00};
    player.sprite = object_types_sprite[0];
    player.palette = object_types_palette_and_pickup[0] & 0x7f;
    player.energy = cfg.energy;
    player.flags = 0;

    // Pockets — slot 0 is the top of the stack (next to retrieve).
    // pockets_ is a C array; use sizeof to bound the copy. cfg.pockets
    // is a std::array but we still cap on the smaller of the two.
    constexpr size_t kPockets = sizeof(pockets_) / sizeof(pockets_[0]);
    for (size_t i = 0; i < kPockets && i < cfg.pockets.size(); i++) {
        pockets_[i] = cfg.pockets[i];
    }
    pockets_used_ = cfg.pockets_used;

    // Weapon energies + selected weapon. The original keeps these
    // separate from the pocket sprite — the pocket is the visible
    // grabbable, the weapon-energy counter is the loaded ammo.
    constexpr size_t kWeapons =
        sizeof(weapon_energy_) / sizeof(weapon_energy_[0]);
    for (size_t i = 0; i < kWeapons && i < cfg.weapon_energy.size(); i++) {
        weapon_energy_[i] = cfg.weapon_energy[i];
    }
    player_weapon_ = cfg.weapon;

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

        // Main game loop sequence (matching &19b6). Order inside the
        // frame is: input → toggles → anchor → world updates → render.
        // While paused the world-update block is skipped so the current
        // state snapshot can be inspected in the banner without values
        // changing every frame.
        process_input();

        // Rising-edge toggle on 'M': flip the activation-anchor mode.
        {
            bool down = input_.state().toggle_map_activation;
            if (down && !map_activation_key_prev_) {
                activation_from_camera_ = !activation_from_camera_;
            }
            map_activation_key_prev_ = down;
        }

        // Rising-edge toggle on 'P': freeze / unfreeze world updates.
        {
            bool down = input_.state().toggle_pause;
            if (down && !pause_key_prev_) {
                paused_ = !paused_;
            }
            pause_key_prev_ = down;
        }

        // Pick the anchor for this frame and hand it to ObjectManager before
        // any lifecycle decisions happen. Player-mode mirrors the 6502 and
        // the camera follows the player anyway; camera-mode lets the user
        // pan the viewport with right-drag and watch objects activate around
        // the camera centre instead. Computed before update_player so the
        // same frame's demotion / promotion / placeholder checks all see a
        // consistent anchor.
        {
            const Object& player = object_mgr_.player();
            uint8_t ax, ay;
            if (activation_from_camera_) {
                ax = static_cast<uint8_t>(player.x.whole + camera_.pan_x);
                ay = static_cast<uint8_t>(player.y.whole + camera_.pan_y);
            } else {
                ax = player.x.whole;
                ay = player.y.whole;
            }
            object_mgr_.set_activation_anchor(ax, ay);
        }

        if (!paused_) {
            object_mgr_.reset_debug_counters();
            update_timers();
            update_player();
            update_objects();

            // Decrement mushroom timers (port of &19d4-&19dd). The same loop
            // lands on &0819 (door_timer) when X reaches 0 in the 6502, so
            // we tick door_timer down here too.
            for (int i = 0; i < 2; i++) {
                if (player_mushroom_timers_[i] > 0) {
                    player_mushroom_timers_[i]--;
                }
            }
            if (object_mgr_.door_timer_ > 0) {
                object_mgr_.door_timer_--;
            }

            // Random events — currently just the star-field spawn path from
            // &2660-&26e6 (see update_events). Full event system (worms /
            // maggots / clawed robots / Triax summoning) is TODO.
            update_events();

            // Tick the particle pool (port of &207e update_particles).
            {
                const Object& p = object_mgr_.player();
                uint8_t wy = Water::get_waterline_y(p.x.whole);
                particles_.update(wy, 0, rng_);
            }
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

// Port of the star-field slice of update_events (&2660-&26e6 in the 6502).
// Each frame the original picks a random tile within ±3-4 of the player,
// resolves it through the tertiary table, and adds a PARTICLE_STAR_OR_
// MUSHROOM at that cell when the tile is above the surface line (y < 0x4e)
// and not inside a spaceship (tile_was_from_map_data clear).
//
// Stars share the particle pool (max 32). STAR_OR_MUSHROOM's TTL range
// is 8..0x23 frames, so roughly ~20 stars stay alive at once — spawning
// one per frame settles into a steady twinkle across the sky.
void Game::update_events() {
    const Object& player = object_mgr_.player();

    // get_random_tile_near_player with A = 7 (diameter). Half = 3.
    // rnd & 7 → 0..7; add player − 3 → player ± 3..4.
    uint8_t dx_rand = static_cast<uint8_t>(rng_.next() & 0x07);
    uint8_t dy_rand = static_cast<uint8_t>(rng_.next() & 0x07);
    uint8_t tx = static_cast<uint8_t>(player.x.whole + dx_rand - 3);
    uint8_t ty = static_cast<uint8_t>(player.y.whole + dy_rand - 3);

    // &26ca CMP #&4e / BCS skip: tiles at or below the surface line never
    // get stars — the sky ends at world y = 0x4e.
    if (ty >= 0x4e) return;

    // The original also suppresses stars when `tile_was_from_map_data` is
    // set (interior of spaceships) or the player is mid-teleport. We don't
    // yet expose the map-data flag from Landscape::get_tile, so for now
    // allow stars in map-data regions too — a minor cosmetic difference
    // confined to spaceship interiors, which the player exits quickly.

    particles_.emit_at(ParticleType::STAR_OR_MUSHROOM, tx, ty, rng_);
}

// Three-phase player update: read input → integrate motion → pick sprite.
// The two halves live in player_actions.cpp / player_motion.cpp so the
// input and physics concerns can evolve independently.
void Game::update_player() {
    Object& player = object_mgr_.player();
    const auto& inp = input_.state();

    int8_t accel_x = 0;
    int8_t accel_y = 0;
    apply_player_input(player, inp, accel_x, accel_y);
    integrate_player_motion(player, accel_x, accel_y);
    update_player_sprite(accel_x, accel_y);
}
