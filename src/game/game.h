#pragma once
#include "core/types.h"
#include "core/random.h"
#include "world/landscape.h"
#include "objects/object.h"
#include "objects/object_manager.h"
#include "player/input.h"
#include "rendering/renderer.h"
#include "rendering/camera.h"
#include "particles/particle_system.h"
#include <memory>
#include <string>

class Game {
public:
    Game(std::unique_ptr<IRenderer> renderer);

    bool init();
    void run();

private:
    // Core systems
    std::unique_ptr<IRenderer> renderer_;
    Landscape landscape_;
    Random rng_;
    Camera camera_;
    InputHandler input_;
    ObjectManager object_mgr_;

    // Game state
    uint8_t frame_counter_ = 0;
    bool running_ = false;

    // Timer flags (set negative every N frames, matching &19b6-&19c7)
    bool every_two_frames_ = false;
    bool every_four_frames_ = false;
    bool every_eight_frames_ = false;
    bool every_sixteen_frames_ = false;
    bool every_thirty_two_frames_ = false;
    bool every_sixty_four_frames_ = false;

    // Player-specific state
    uint8_t player_weapon_ = 0;   // 0=jetpack, 1=pistol, etc.
    uint8_t player_aim_angle_ = 0;
    uint8_t player_angle_  = 0xc0;  // &de: current body angle (0xc0 = upright head-up)
    uint8_t player_facing_ = 0x00;  // &df: facing as an x_flip byte (0x00 right, 0x80 left)
    uint8_t held_object_slot_ = 0x80; // 0x80+ = no object held
    uint16_t weapon_energy_[6] = {0x0800, 0, 0, 0, 0, 0}; // Jetpack starts with energy
    bool jetpack_active_ = false;

    // Player pockets (&0848-&084c): up to 5 stored object types, slot 0 is
    // the "top" of the stack (next to retrieve). Unused slots = 0xff.
    uint8_t pockets_[5] = {0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t pockets_used_ = 0;

    // Whistle state (port of &27 and &29d8)
    bool whistle_one_active_ = false;    // &27 bit 7: set for one frame when whistle one played
    uint8_t whistle_two_activator_ = 0xff; // &29d8: slot of object that played whistle two (0xff=none)
    bool whistle_one_collected_ = false;  // &0816
    bool whistle_two_collected_ = false;  // &0817
    uint8_t chatter_energy_reserve_ = 0;  // &081c

    // Mushroom timers (port of &081a)
    // [0] = red mushroom exposure (makes invisible objects visible)
    // [1] = blue mushroom exposure (immobilizes player)
    // Decremented each frame, added to when player contacts mushroom balls/tiles.
    uint8_t player_mushroom_timers_[2] = {0, 0};
    bool mushroom_immunity_collected_ = false;  // &0815

    // Debug overlay: text displayed in top-right, set when left-click picks a tile.
    std::string selected_tile_info_;

    // Activation anchor mode: false → player position drives distance-based
    // lifecycle checks (matches the 6502). true → camera centre drives them
    // so scrolling the viewport activates objects near the view, not the
    // player. Toggled by 'M' with rising-edge detection.
    bool activation_from_camera_ = false;
    bool map_activation_key_prev_ = false;

    // Pause toggle. While paused the main loop still runs input + render
    // (so the user can pan, click, toggle overlays, and unpause), but
    // skips timers / update_player / update_objects / particle tick — so
    // the world state freezes and the diagnostic banner becomes readable.
    bool paused_ = false;
    bool pause_key_prev_ = false;

    // Rising-edge state for inventory keys. Without these, holding down
    // ENTER / S / R for more than one frame causes a pickup → drop →
    // pickup oscillation that locks the player out of grabbing anything.
    // The 6502 polls a per-key "just-pressed" register at &126b which is
    // implicitly edge-triggered; we replicate by remembering last frame's
    // raw key state and only acting on a 0→1 transition.
    bool pickup_drop_key_prev_ = false;
    bool pickup_key_prev_      = false;
    bool drop_key_prev_        = false;
    bool throw_key_prev_       = false;
    bool store_key_prev_       = false;
    bool retrieve_key_prev_    = false;

    // Spawn diagnostics — incremented from spawn_tertiary_object so the
    // map-mode HUD banner can show whether spawns are actually firing.
    uint32_t spawn_attempts_ = 0;
    uint32_t spawn_created_  = 0;

    // Particle pool (max 32). Updated each frame; rendered in Game::render.
    ParticleSystem particles_;

    // Main loop phases
    void update_timers();
    void process_input();
    void update_player();
    void update_objects();
    void update_events();
    void render();

    // update_player is split into three phases, all Game members so they
    // share access to state through `this`:
    //   apply_player_input      — reads input, fires weapons/particles,
    //                             emits (accel_x, accel_y).
    //   integrate_player_motion — wind, physics, tile collision, water,
    //                             object-object touching, camera follow.
    //   update_player_sprite    — picks the spacesuit frame from the
    //                             current body angle / walk state.
    void apply_player_input(Object& player, const InputState& inp,
                            int8_t& accel_x, int8_t& accel_y);
    void integrate_player_motion(Object& player,
                                 int8_t accel_x, int8_t accel_y);
    void update_player_sprite(int8_t accel_x, int8_t accel_y);

    // Spawn a primary object from a tertiary when its tile comes into view.
    // Port of create_primary_object_from_tertiary (&4042) plus the per-tile
    // update routines that call it (metal door / stone door / switch /
    // tile_with_object_from_type / tile_with_object_from_data).
    void spawn_tertiary_object(uint8_t tile_type, uint8_t tile_flip,
                               uint8_t tile_x, uint8_t tile_y,
                               int data_offset, int type_offset,
                               uint8_t raw_tile_type);
};
