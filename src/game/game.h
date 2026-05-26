#pragma once
#include "core/types.h"
#include "core/damage_visual.h"
#include "core/random.h"
#include "world/landscape.h"
#include "objects/object.h"
#include "objects/object_manager.h"
#include "player/input.h"
#include "rendering/renderer.h"
#include "rendering/camera.h"
#include "particles/particle_system.h"
#include <fstream>
#include <vector>
#include <memory>
#include <string>

class Game {
public:
    Game(std::unique_ptr<IRenderer> renderer);

    bool init();
    void run();

    // One frame of the main loop, no frame-timing sleep. run() is just a
    // while-loop around tick() with a 50fps budget; tests drive tick()
    // directly so they can advance N frames without real time elapsing.
    void tick();

private:
    // Test harness reaches into private state to spawn fixtures, snapshot
    // counters, and assert on per-object fields without bloating the
    // public surface. Production code goes through run()/tick() only.
    friend class TestHarness;

    // Core systems
    std::unique_ptr<IRenderer> renderer_;
    Landscape landscape_;
    // rng_ mirrors the 6502 &d9-&dc rnd_state — used by every roll the
    // ROM also makes (AI, spawn, fire, wind magnitude, damage). cosmetic_rng_
    // is a port-only second stream for particle internals + emit gates so
    // larger pools / wider viewports don't perturb game_rng's sequence.
    Random rng_;
    Random cosmetic_rng_;
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
    // &32 player_aiming_angle_without_flip — uint8 interpreted as int8,
    // clamped to [-0x3f, +0x3f] by &30fc.
    uint8_t player_aim_angle_       = 0;
    // &33 player_aiming_angle_velocity — integrates accel toward
    // ±0x10, zeroed the frame no aim key is held.
    int8_t  player_aim_velocity_    = 0;
    uint8_t player_angle_  = 0xc0;  // &de: current body angle (0xc0 = upright head-up)
    uint8_t player_facing_ = 0x00;  // &df: facing as an x_flip byte (0x00 right, 0x80 left)
    // &ba / &bb player_immobility_timers. Set by damage_object at &24b7
    // (movement timer = raw damage, 1-in-2 chance) and consumed in
    // update_player_angle_facing_and_sprite at &3810: while non-zero,
    // movement is suppressed and player_angle rotates ("knocked spinning"
    // visual). The thrust timer (&bb) gates jetpack thrust separately.
    uint8_t player_immobility_movement_ = 0;
    uint8_t player_immobility_thrust_   = 0;
    // &b9 player_immobility_rotation_velocity. Signed (int8 in
    // disguise). Accumulated each spin frame from the collision-angle /
    // pre-collision-magnitude math, clamped to ±0x20 (&3840), decayed
    // 7/8 every 4 frames when |rv| >= 4. Added to player_angle each
    // spin tick — that's what makes the player tumble.
    uint8_t player_immobility_rotation_velocity_ = 0;
    // State-change snapshots for the walk-vs-fly debug log. We log only
    // when one of these flips, plus a one-shot "walk-blocked" line on
    // each frame the player presses left/right but the walking gate
    // refuses. Avoids spamming exile-debug.log every frame.
    bool    walk_log_supported_prev_ = false;
    uint8_t walk_log_counter_prev_   = 0xff;
    bool    walk_log_walking_prev_   = false;
    bool    walk_log_flying_prev_    = false;
    bool    inp_log_supported_prev_  = false;
    bool    inp_log_walking_prev_    = false;
    bool    inp_log_motion_prev_     = false;
    // &1c tile_collision_angle: angle of the surface the player is standing
    // on, in 8-bit units (0x00 = flat ground, 0xe0 = 45° rising right,
    // 0x20 = 45° falling right). Refreshed each frame in
    // integrate_player_motion's grounded check from the supporting tile's
    // threshold gradient. Used by the walking branch in apply_player_input
    // to point acceleration ALONG the slope instead of purely horizontal.
    uint8_t player_tile_collision_angle_ = 0;
    uint8_t held_object_slot_ = 0x80; // 0x80+ = no object held
    // Snapshot of the held primary's snap-to-player-side position
    // (port of 6502 &0a..&0d held_object_x/y/fraction). Set in
    // object_update.cpp's held-alignment step BEFORE collision runs;
    // consume_dropping_held_object compares this against the post-
    // collision obj.x/y to detect "wall pulled the held off the
    // player's side" and drops the object when the drift exceeds
    // 0x30 frac per axis (matching &1ca9-&1cc4).
    Fixed8_8 held_expected_x_;
    Fixed8_8 held_expected_y_;
    // &29d7 player_object_fired: set to the held object's slot when the
    // player presses fire while holding something; 0xff when nothing was
    // fired this frame. update_remote_control_device (&4351) and doors /
    // transporters (&4c9e / &4dc8) read it via check_if_object_fired.
    // Reset to 0xff at the end of each tick.
    uint8_t player_object_fired_ = 0xff;
    uint16_t weapon_energy_[6] = {0x0800, 0, 0, 0, 0, 0}; // Jetpack starts with energy
    bool jetpack_active_ = false;
    // &36 player_blaster_timer. Set to -5 by Weapon::fire when the blaster
    // is triggered; tick_blaster increments it each frame while negative,
    // running a duration-10 explosion centred on the player on every tick.
    // Clears to 0 once the discharge sequence finishes.
    int8_t blaster_timer_ = 0;

    // &081d explosion_timer. Set to -50 by start_explosion_timer (&40e8),
    // INC'd to 0 each frame at &19df-&19e4. AI stimulus flag (read by
    // worm/maggot emergence at &267d and the stimuli responder at
    // &282b); does NOT drive the screen flash. Distinct from the
    // background-flash cooldown below.
    int8_t explosion_timer_ = 0;

    // &2a background_flash_cooldown. Set to 11 by flash_background
    // (&1f92); update_background_flash (&1f97) DECs it each frame and
    // randomises colour 0 (the sky) while > 0, restoring black on the
    // tick it reaches 0. Triggered by coronium chains (&41e5) and the
    // "no free slot — object lost" path (&0c78). The blaster does NOT
    // call this in the 6502; we add a port-only call from tick_blaster
    // so the discharge has a visible sky flash.
    uint8_t background_flash_cooldown_ = 0;

    // [player] invincible — when true, gates every player-damage path
    // (damage_player_if_touching, common_bullet_update target hit when
    // touching == player slot 0, explosion radius hits, coronium
    // radiation). Set from cfg.invincible at startup.
    bool invincible_ = false;

    // [creatures] sucking_nest_damages_player — when true, sucking-nest
    // damage-on-touch (&4e29-&4e34) is allowed to land on the player
    // slot. Default false to avoid frame-by-frame chip damage while the
    // suck/push tuning is in flight. Plumbed via UpdateContext.
    bool sucking_nest_damages_player_ = false;

    // [debug] show_fps — top-right FPS readout. Measured in Game::run
    // over a 30-frame rolling window (actual wall-clock cadence,
    // includes the per-frame sleep). Set from cfg.show_fps at startup.
    bool   show_fps_  = false;
    double fps_value_ = 0.0;

    // [debug] jetpack_boost_tint — recolours jetpack thrust particles to
    // a red/magenta combo while the booster (Right Ctrl / [) is held, so
    // the 2x acceleration path is visible at a glance. Plumbed into the
    // particle emit at player_actions.cpp's add_jetpack_thrust_particles.
    bool   jetpack_boost_tint_ = false;

    // [debug] target_fps — locked logic + render tick rate. 25 = BBC
    // original speed; 50/75/100 fast-forward. Audio sample count is
    // realigned via Audio::set_logic_rate in Game::init.
    int target_fps_ = GameConstants::TARGET_FPS_DEFAULT;

    // Per-frame damage log for the "Damage" debug overlay. Updated only
    // when the renderer's checkbox is on. Cleared at the top of every
    // tick so the overlay reflects exactly what happened this frame.
    std::vector<DamageVisual> damage_events_;

    // Always-on transient text notifications. Pushed by update routines
    // on rare game events (imp fed, key collected, …) and rendered
    // unconditionally so the feedback isn't gated by a debug toggle.
    // TTL decays each tick; entries are erased when they hit 0.
    std::vector<FloatingLabel> floating_labels_;

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

    // Port of &080e..&0813 player_weapons_collected. Index 0 is the
    // jetpack booster, 1..4 are pistol/icer/blaster/plasma gun, 5 is the
    // protection suit. 0x80 = collected, 0 = not. update_collectable's
    // &4b8e-&4b91 path stamps the flag when an item of the matching type
    // is held. &2c81 gates the Right-Ctrl boost on entry 0; &2cef gates
    // weapon select (1-5 keys) on entries 1..5.
    uint8_t player_weapons_collected_[6] = {0, 0, 0, 0, 0, 0};

    // &0814 player_fire_immunity_device_collected (&4af9 gate: fireball
    // touch damage to slot 0 zeroes out). &0818 radiation immunity (gate
    // at &41f5: coronium 8-damage hit on hold skipped).
    bool fire_immunity_collected_      = false;
    bool radiation_immunity_collected_ = false;

    // Port of &0806 player_keys_collected. Eight entries, 0x80 = key
    // collected, 0 = not. Index 0..5 are the six visible key object
    // types (CYAN_YELLOW_GREEN_KEY ... BLUE_CYAN_GREEN_KEY); 6..7 are
    // the 6502's transporter-beam slots shared with the higher door
    // colours via consider_toggling_lock's shift math at &31bb. The
    // RCD's door-unlock path (update_door's &4c9e hook) reads this
    // array — keys don't occupy pockets, they just stamp the bitmask.
    uint8_t player_keys_collected_[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    // Mushroom timers (port of &081a)
    // [0] = red mushroom exposure (makes invisible objects visible)
    // [1] = blue mushroom exposure (immobilizes player)
    // Decremented each frame, added to when player contacts mushroom balls/tiles.
    uint8_t player_mushroom_timers_[2] = {0, 0};
    bool mushroom_immunity_collected_ = false;  // &0815

    // Global event state (&081e-&0846). flooding_state_ &081e, earthquake_
    // state_ &081f (neg = running, worsens via &25e7). clawed_robot_
    // availability_[4] &083f: ff dormant, 00 ready, 01 primary. _teleport_
    // energy_[4] &0843 ticks up while dormant until past 0x80 (&2725).
    uint8_t flooding_state_   = 0;
    uint8_t earthquake_state_ = 0;
    uint8_t clawed_robot_availability_[4]   = {0, 0, 0, 0};
    uint8_t clawed_robot_teleport_energy_[4] = {0, 0, 0, 0};

    // Per-variant remaining-gift counters for the imps' fed-then-home
    // gift drop (port of &083a..&083e). Decremented when a fed imp
    // returns to its pipe. Initial counts from the 6502 are:
    //   red/magenta  -> 4 power pods
    //   red/yellow   -> 10 active grenades
    //   blue/cyan    -> 1 alien weapon
    //   cyan/yellow  -> 1 alien weapon
    //   red/cyan     -> 10 active grenades
    uint8_t imp_gifts_remaining_[5] = {4, 10, 1, 1, 10};

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

    // Activation anchor mode: false -> player position drives distance-based
    // lifecycle checks (matches the 6502). true -> camera centre drives them
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

    // Test rig — door-destruction setup at startup spawns 1 active
    // grenade plus 3 inactive ones; halfway through the active's fuse
    // (timer >= 0x30), each inactive slot here is converted to active
    // by setting type=ACTIVE_GRENADE and timer=0x30 so all four detonate
    // on the same frame (active + 3 newly-activated each at fuse 0x30,
    // counting up to 0x60 in 48 more frames).
    int test_active_grenade_slot_ = -1;
    int test_pending_grenade_slots_[4] = { -1, -1, -1, -1 };
    bool test_grenades_activated_ = false;

    // Rising-edge state for inventory keys. Without these, holding down
    // ENTER / S / R for more than one frame causes a pickup -> drop ->
    // pickup oscillation that locks the player out of grabbing anything.
    // The 6502 polls a per-key "just-pressed" register at &126b which is
    // implicitly edge-triggered; we replicate by remembering last frame's
    // raw key state and only acting on a 0->1 transition.
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
    //   R -> handle_remembering_position (&2c3c)
    //   T -> handle_teleporting          (&0cc1)
    bool remember_key_prev_    = false;
    bool teleport_key_prev_    = false;
    bool save_map_key_prev_    = false;

    // Tile-editor state. The "Edit" HUD checkbox (read via
    // IRenderer::editor_enabled) flips left-click between SELECT and
    // PAINT. SELECT writes the clicked tile into editor_paint_tile_ and
    // populates the info overlay; PAINT writes editor_paint_tile_ into
    // landscape_ at the clicked cell. Default 0x19 = TILE_SPACE so the
    // first paint with no prior selection erases.
    uint8_t editor_paint_tile_ = 0x19;
    // Right-click action selector: 0 = paint tile, 1 = place object.
    // Toggled by the user clicking either palette panel.
    int     editor_paint_kind_ = 0;
    int     editor_paint_object_idx_ = -1;
    // Last cell the user left-clicked, in world coordinates. Used by
    // the editor's tertiary controls (Detach button, [ / ] data-byte
    // bump keys) so they know which cell to act on.
    uint8_t  editor_highlight_x_ = 0;
    uint8_t  editor_highlight_y_ = 0;
    bool     editor_has_highlight_ = false;
    bool     tert_data_inc_prev_ = false;
    bool     tert_data_dec_prev_ = false;
    // Frame at which the last "Saved" feedback overlay should disappear.
    // Zero means no message active.
    uint64_t editor_save_msg_until_frame_ = 0;
    // Whether the last save attempt succeeded (true) or failed (false);
    // controls "Saved" vs "Save FAILED" text in the banner.
    bool     editor_save_msg_ok_           = false;

    void refresh_selected_tile_info(uint8_t tx, uint8_t ty);

    // Spawn diagnostics — incremented from spawn_tertiary_object so the
    // map-mode HUD banner can show whether spawns are actually firing.
    uint32_t spawn_attempts_ = 0;
    uint32_t spawn_created_  = 0;

    // Append-only lifecycle log. Opened once in init(); each non-paused
    // frame flushes any events recorded in ObjectManager::debug_events_
    // (cre/prm/dem/ret/rem) as a line so churn patterns can be inspected
    // offline (grep / tail -f). File lives next to exile.ini.
    //
    // The implementation of these debug-only helpers lives in
    // game_debug.cpp so the main game.cpp stays focused on the per-frame
    // chain. dump_init_diagnostics writes the one-shot landscape /
    // switch / tertiary census at startup; log_walking_diagnosis is the
    // state-change-only walking trace emitted from update_player.
    std::ofstream debug_log_;
    void flush_debug_log();
    void dump_init_diagnostics();
    void log_walking_diagnosis(const Object& player, const InputState& inp,
                               int8_t pre_vx, int8_t pre_vy,
                               uint8_t pre_xf, uint8_t pre_yf,
                               int8_t accel_x, int8_t accel_y);

    // Port-only test rigs (event panel buttons, chained-grenade demo,
    // smooth-flood subframe drain). All bodies live in game_debug.cpp so
    // the production update path stays clean.
    void tick_test_grenades();
    void tick_test_flood();

    // Startup test spawns: red-slime-on-door rig (always) plus the
    // [debug] gated rigs — stress_test creature grid, grenade_chain,
    // icer_drop. Called once from Game::init after the world is baked.
    void spawn_test_rigs(bool stress_test, bool grenade_chain, bool icer_drop);

    // Per-frame TTL tick + erase pass for the damage / floating-label
    // overlay rings. Both are debug-only visual feedback (no gameplay
    // effect) so the body lives in game_debug.cpp alongside the rest of
    // the debug helpers.
    void tick_overlay_visuals();

    // Rewind ring management — debug-only feature. Bodies in game_debug.cpp.
    // commit_scrub_if_active returns true if it consumed the Esc press
    // (collapsed the ring to the scrubbed frame); the caller toggles pause
    // only when it returns false. tick_scrub_keys handles numpad +/-.
    // capture_rewind_snapshot writes the current state into the ring.
    // scrubbed_ring_index resolves the current scrub_offset_ to a ring slot.
    bool   commit_scrub_if_active();
    void   tick_scrub_keys();
    void   capture_rewind_snapshot();
    size_t scrubbed_ring_index() const;

    // Tertiary -> primary spawn-gate radius (in tiles). Set from
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
    // &4a70-&4a87 blaster discharge tick. While blaster_timer_ < 0,
    // INC the timer and run update_explosion centred on the player at
    // duration 10 — same damage radius as a duration-10 EXPLOSION primary.
    void tick_blaster();

    // &40e8 start_explosion_timer. Sets explosion_timer_ = -50 so AI
    // stimuli code can read "explosion in progress" for the next 50
    // frames (&267d / &282b). Does not drive any visual.
    void start_explosion_timer() { explosion_timer_ = -50; }

    // &1f92 flash_background. Sets background_flash_cooldown_ = 11 so
    // update_background_flash randomises the sky for the next 11 frames.
    void flash_background() { background_flash_cooldown_ = 0x0b; }

    // &1f97 update_background_flash. Per-frame DEC + colour-0 swap.
    // Called once per game tick. While the cooldown is > 0, picks a
    // random sky colour (1-in-8 chance of white-ish, otherwise random
    // logical colour); restores black on the tick the cooldown hits 0.
    void update_background_flash();

    // Port of &34b4 store_object: pocket held primary (or drain if power
    // pod). drain_power_pod=false lets the retrieve path cycle a pre-seeded
    // exile.ini [pockets] power_pod instead of consuming it each revolution
    // (a state the 6502 couldn't reach).
    bool try_store_held(Object& player, bool drain_power_pod = true);

    // Port of &32c8 handle_dropping_object: release the currently-held
    // primary (if any) back into the world.
    void drop_held_object(Object& player);

    // Three input-edge actions called from apply_player_input. They share
    // member state (held_object_slot_, object_mgr_, rng_, aim) so they live
    // as methods rather than free functions.
    void pickup_touching(Object& player);
    void drop_in_place(Object& player);
    void throw_held(Object& player);

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

    // Port of &25a1-&25df update_triax_lab. Feeds maggot machine,
    // opens/closes lab bottom door based on waterline range 1, and
    // rewrites Water::desired_y[1] to drain/fill accordingly. Pauses
    // when flooding_state_'s bit 7 is set (lab fully floods to 0x67).
    void update_triax_lab();

    // Test-events panel triggers (port-only). IDs match the kEventButtons
    // table in src/rendering/pixel_renderer_debug.cpp; keep both in sync.
    enum class EventId : int {
        SPAWN_TRIAX         = 0,
        SPAWN_MAGGOT        = 1,
        SPAWN_CLAWED_ROBOT  = 2,
        TOGGLE_FLOOD        = 3,
        TOGGLE_EARTHQUAKE   = 4,
        DAMAGE_PLAYER       = 5,
        HEAL_PLAYER         = 6,
    };
    void trigger_event(int event_id);

    // Port-only camera shake — armed by the Earthquake test trigger
    // (6502 CRTC R2 line-shudder isn't available on a framebuffer).
    // Decrements each frame; when > 0, render() perturbs the camera
    // sub-tile fraction by a random ±0x40 in x/y.
    uint8_t test_shake_frames_ = 0;

    // Port-only smooth flood — runs alongside the 6502 integrator so
    // the visible waterline moves at 1 tile / second. The 6502's
    // integrator advances g_y[1] by ~1 tile every 128 frames; we use
    // a 50-frame subframe counter to drive 1 tile / 50 frames (50fps =
    // 1 tile / second) for the test event.
    // direction is -1 to flood (y decreasing = water rising on screen),
    // +1 to drain.
    int     test_flood_steps_remaining_ = 0;
    int8_t  test_flood_direction_       = 0;
    uint8_t test_flood_subframe_        = 0;

    // Save / restore. Human-readable text format — see save_load.cpp for
    // the schema. The landscape is regenerated from seed on load, so the
    // save only has to capture mutable state (player, objects, events,
    // rng, tertiary data bytes). Both return true on success.
    bool save_game(const std::string& path) const;
    bool load_game(const std::string& path);

    // Load a BBC-format Exile save (1024 bytes, XOR-streamed). The file
    // is the supervisor's raw &0400..&0800 dump — extracted from an .ssd
    // via tools/extract_ssd_saves.py or written by the original game on
    // a real BBC. Format documented in docs/save_game_format.md.
    bool load_bbc_save(const std::string& path);

    // Recursively enumerate save files under `root` (typically
    // "save_disks"). Returns paths suitable for load_game / load_bbc_save.
    // Used by the renderer's Saves panel.
    static std::vector<std::string> scan_save_files(const std::string& root);

    // Stream-based variants used by the rewind ring buffer. snapshot()
    // serialises the same payload save_game writes to disk;
    // restore_snapshot() is the inverse. dump_ring_buffer() concatenates
    // every captured frame to a single file with `=== frame N ===`
    // delimiters — useful when bisecting a bug across many frames.
    void        write_state(std::ostream& f) const;
    bool        read_state(std::istream& f);
    std::string snapshot() const;
    bool        restore_snapshot(const std::string& s);
    bool        dump_ring_buffer(const std::string& path) const;

    // One-shot full world-state push to jsbeeb (objects, inventory, RNG,
    // pose, tertiary linkage, secondaries). Fired from process_input on
    // the J rising edge; after the snapshot jsbeeb runs free.
    void        push_jsbeeb_snapshot();

    // Rising-edge state for save/load keys. Without these, holding down
    // ';' would overwrite the save every frame. Scrub keys deliberately
    // do NOT use edge detection — holding them auto-repeats one frame
    // per tick so the user can sweep through the rewind buffer.
    bool save_key_prev_     = false;
    bool load_key_prev_     = false;
    bool dump_key_prev_     = false;
    // 'J' is a one-shot — edge-detect only, no mirror-mode flag.
    bool bridge_key_prev_   = false;
    // Saves-panel toggle edge detection — rescan save_disks/ on rising edge.
    bool saves_panel_was_on_ = false;

    // Rewind ring buffer. capacity == 600 frames (~10 sec at 60fps);
    // head_ is the next write slot, count_ is the number of valid entries
    // (capped at capacity). scrub_offset_ counts frames back from the
    // most recently written entry; 0 means "live", non-zero freezes the
    // sim until the user presses Esc.
    static constexpr size_t SNAPSHOT_RING_CAPACITY = 600;
    std::vector<std::string> snapshot_ring_{SNAPSHOT_RING_CAPACITY};
    size_t snapshot_ring_head_  = 0;
    size_t snapshot_ring_count_ = 0;
    bool   scrubbing_           = false;
    size_t scrub_offset_        = 0;

    // Spawn a primary object from a tertiary when its tile comes into view.
    // Port of create_primary_object_from_tertiary (&4042) plus the per-tile
    // update routines that call it (metal door / stone door / switch /
    // tile_with_object_from_type / tile_with_object_from_data).
    void spawn_tertiary_object(uint8_t tile_type, uint8_t tile_flip,
                               uint8_t tile_x, uint8_t tile_y,
                               int data_offset, int type_offset,
                               uint8_t raw_tile_type);
};
