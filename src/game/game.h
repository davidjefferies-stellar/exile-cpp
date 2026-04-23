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
    // &29d7 player_object_fired: set to the held object's slot when the
    // player presses fire while holding something; 0xff when nothing was
    // fired this frame. update_remote_control_device (&4351) and doors /
    // transporters (&4c9e / &4dc8) read it via check_if_object_fired.
    // Reset to 0xff at the end of each tick.
    uint8_t player_object_fired_ = 0xff;
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

    // Global event state (all ports of 6502 bytes at &081e-&0846).
    //   flooding_state_      &081e  negative → endgame flood in progress
    //   earthquake_state_    &081f  negative → earthquake running; gradually
    //                               worsens via update_events (&25e7).
    //   clawed_robot_availability_[4]        &083f
    //       ff (negative) = dormant
    //       00            = ready to respawn (teleport-energy building)
    //       01 (positive) = already primary in the world
    //   clawed_robot_teleport_energy_[4]     &0843
    //       Counter that ticks up while the robot is dormant; once it
    //       overflows past 0x80 the robot can rejoin the game (&2725).
    uint8_t flooding_state_   = 0;
    uint8_t earthquake_state_ = 0;
    uint8_t clawed_robot_availability_[4]   = {0, 0, 0, 0};
    uint8_t clawed_robot_teleport_energy_[4] = {0, 0, 0, 0};

    // Player teleport tables (&0821-&082c). Slots 0-3 are rewritten by
    // handle_remembering_position; slot 4 is the fallback used when no
    // positions are remembered. Initial values match the 6502 ROM state:
    // slot 4 = (&99, &3c), the player's spawn tile.
    uint8_t player_next_teleport_        = 0;
    uint8_t player_teleports_remembered_ = 0;
    uint8_t player_teleports_x_[5] = {0x32, 0x8e, 0xd2, 0x63, 0x99};
    uint8_t player_teleports_y_[5] = {0x98, 0xc0, 0xc0, 0xc7, 0x3c};
    uint16_t player_deaths_ = 0;  // port of &080d game_time deaths counter
    bool player_is_completely_dematerialised_ = false;  // &19b5

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
    bool turn_around_key_prev_ = false;
    bool lie_down_prev_        = false;
    // SPACE (fire) is `repeat = no` in the 6502 action table (&0d at
    // line 3572 of the disassembly), i.e. one press = one bullet. Without
    // the edge gate, holding space empties the weapon in a few frames.
    bool fire_key_prev_        = false;

    // Lying-down state (&05 bit 6-ish): true while the player is prone.
    // Tab flips facing; Left Ctrl toggles lying. Right Ctrl feeds into
    // the jetpack booster path so motion accelerates faster.
    bool player_lying_down_ = false;
    bool retrieve_key_prev_    = false;
    // Edge triggers for the 6502 teleport + remember keys.
    //   R → handle_remembering_position (&2c3c)
    //   T → handle_teleporting          (&0cc1)
    bool remember_key_prev_    = false;
    bool teleport_key_prev_    = false;

    // Spawn diagnostics — incremented from spawn_tertiary_object so the
    // map-mode HUD banner can show whether spawns are actually firing.
    uint32_t spawn_attempts_ = 0;
    uint32_t spawn_created_  = 0;

    // Tertiary → primary spawn-gate radius (in tiles). Set from
    // exile.ini [distances] spawn_tertiary during Game::init; default
    // 4 matches the KEEP_AS_PRIMARY_FOR_LONGER slow+supported demote
    // ring so settled tertiary objects don't oscillate between
    // "spawned" and "demoted" each frame.
    uint8_t spawn_tertiary_distance_ = 4;

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

    // Port of &34b4 store_object: pocket the currently-held primary (or
    // drain it into the jetpack if it's a power pod). Returns true if the
    // object was consumed. No-op + false if nothing is held, the object
    // is too tall (>= 8 rows) to fit a pocket, or pockets are full.
    bool try_store_held(Object& player);

    // Port of &32c8 handle_dropping_object: release the currently-held
    // primary (if any) back into the world.
    void drop_held_object(Object& player);

    // Port of &4096 consider_teleporting_damaged_player. Called when the
    // player's energy would hit zero; short-circuits the explosion path
    // by bumping energy back to 1 and either auto-teleporting the player
    // to a remembered position, or (1/4 chance) retrieving a pocket item.
    void consider_teleporting_damaged_player(Object& player);

    // Port of &0cc1 handle_teleporting. Sets OBJECT_FLAG_TELEPORTING +
    // timer and picks the target tile from the teleport tables.
    void handle_player_teleporting(Object& player);

    // Drive the 32-frame teleport animation for the player. Called from
    // update_player before the input / motion chain. Mirrors the section
    // of the 6502 object loop at &1bfd-&1c44 that the player slot skips.
    // Returns true if the teleport animation consumed this frame (so the
    // caller should skip the normal input / motion update).
    bool advance_player_teleport(Object& player);

    // Port of &2c3c handle_remembering_position. Record the player's
    // current centre into the teleport tables (if energy >= 8).
    void handle_remembering_position(Object& player);

    // Save / restore. Human-readable text format — see save_load.cpp for
    // the schema. The landscape is regenerated from seed on load, so the
    // save only has to capture mutable state (player, objects, events,
    // rng, tertiary data bytes). Both return true on success.
    bool save_game(const std::string& path) const;
    bool load_game(const std::string& path);

    // Rising-edge state for save/load keys. Without these, holding down
    // ';' would overwrite the save every frame.
    bool save_key_prev_ = false;
    bool load_key_prev_ = false;

    // Spawn a primary object from a tertiary when its tile comes into view.
    // Port of create_primary_object_from_tertiary (&4042) plus the per-tile
    // update routines that call it (metal door / stone door / switch /
    // tile_with_object_from_type / tile_with_object_from_data).
    void spawn_tertiary_object(uint8_t tile_type, uint8_t tile_flip,
                               uint8_t tile_x, uint8_t tile_y,
                               int data_offset, int type_offset,
                               uint8_t raw_tile_type);
};
