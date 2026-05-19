#include "game/game.h"
#include "game/config.h"
#include "audio/audio.h"
#include "objects/collision.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include "objects/held_object.h"
#include "rendering/debug_names.h"
#include "rendering/sprite_atlas.h"
#include "world/tertiary.h"
#include "world/tile_data.h"
#include "behaviours/environment.h"
#include "behaviours/projectile.h"
#include "bridge/jsbeeb_bridge.h"
#include "objects/object_tables.h"
#include "world/water.h"
#include <algorithm>
#include <chrono>
#include <thread>

// Game — lifecycle, top-level loop orchestration, timers, input polling.
// Big chunks live in siblings: tertiary_spawn / player_* / object_update /
// render / world/tertiary.

Game::Game(std::unique_ptr<IRenderer> renderer)
    : renderer_(std::move(renderer)) {
}

bool Game::init() {
    if (!renderer_->init()) return false;

    // Load startup config first — Audio::set_log_enabled needs the
    // [logs] flag before Audio::open() emits its first log line. The
    // rest of init uses the same cfg.
    StartupConfig cfg = load_startup_config("exile.ini");

    // Audio. Open lazily — if the platform refuses (no device, headless
    // CI, etc.) Audio::open() returns false and every call site below
    // becomes a silent no-op rather than blocking the game from running.
    Audio::set_log_enabled(cfg.logs_enabled);
    Audio::open();

    // Initialize object manager. Hand it the landscape so its tertiary
    // accessors (tertiary_data_byte etc) can read/write the per-cell
    // entries owned by the landscape.
    object_mgr_.set_landscape(landscape_);
    object_mgr_.init();

    // Waterline state — reset to ROM initial values so a fresh launch
    // matches the 6502's &14d2 startup, regardless of any earlier game
    // having mutated the module-level table.
    Water::reset();

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
    if (cfg.give_protection_suit) weapon_energy_[5] = 0xffff;

    // Port deviation: stamp player_weapons_collected for any weapon slot
    // seeded with non-zero energy in [weapon_energy]. Keeps existing
    // exile.ini test setups (default icer ammo, give_protection_suit)
    // usable now that &2cef gates select on the collected bit.
    for (size_t i = 0; i < kWeapons; i++) {
        if (weapon_energy_[i] > 0) player_weapons_collected_[i] = 0x80;
    }
    invincible_   = cfg.invincible;
    sucking_nest_damages_player_ = cfg.sucking_nest_damages_player;
    show_fps_     = cfg.show_fps;
    target_fps_   = cfg.target_fps;
    Audio::set_logic_rate(target_fps_);
    player_weapon_ = cfg.weapon;

    // [audio] enabled gates Audio::play / play_at. The device is already
    // open by this point so toggling is just a flag flip.
    Audio::set_enabled(cfg.audio_enabled);

    // Whistle collected flags (ports of &0816 / &0817). Default true so
    // Y / U work from the start; set `whistle_one_collected = false` in
    // the ini to replay the pick-up-the-whistle discovery path.
    whistle_one_collected_ = cfg.whistle_one_collected;
    whistle_two_collected_ = cfg.whistle_two_collected;

    // Key-collected bitmask (port of &0806 player_keys_collected). Each
    // entry is 0x80 when the corresponding key has been picked up; the
    // door-unlock path (update_door's &4c9e RCD hit -> consider_toggling_
    // lock at &31bb) will read this array to decide whether the matching
    // coloured door can be toggled. exile.ini's [keys] section pre-sets
    // entries for testing without having to wander to each key in-world.
    constexpr size_t kKeys =
        sizeof(player_keys_collected_) / sizeof(player_keys_collected_[0]);
    for (size_t i = 0; i < kKeys && i < cfg.keys_collected.size(); i++) {
        player_keys_collected_[i] = cfg.keys_collected[i];
    }

    // Cache-range radii. object_manager.cpp's check_demotion picks one of
    // the three demote_distances_ values based on an object's type flags;
    // promote_distance_ governs secondary -> primary re-promotion; and
    // spawn_tertiary_distance_ gates render-time tertiary-to-primary
    // spawns. All live in exile.ini's [distances] section.
    object_mgr_.set_demote_distances(cfg.demote_tertiary,
                                      cfg.demote_moving,
                                      cfg.demote_settled);
    object_mgr_.set_promote_distance(cfg.promote_secondary);
    spawn_tertiary_distance_ = cfg.spawn_tertiary;

    // Pick which landscape generator runs at bake time. The two
    // implementations are intended to produce byte-identical maps; the
    // toggle exists so the C++ rewrite (landscape_cpp.cpp) can be A/B-
    // tested against the pseudo-6502 reference (landscape.cpp).
    landscape_.set_use_cpp_impl(cfg.use_cpp_landscape);

    // Try to load a hand-edited map first; fall back to baking from
    // the procedural generator when the file is missing / invalid.
    // The format is documented in landscape.cpp's save_to_file. This
    // is the hook the future editor saves through; nothing else in
    // the game cares whether the grid came from algorithm or disk.
    if (!landscape_.load_from_file("exile.map")) {
        landscape_.bake();
    }

    // Cache sizes. object_manager's backing arrays are sized at compile
    // time to GameConstants::PRIMARY/SECONDARY_OBJECT_SLOTS; these
    // setters constrain the runtime "active" counts (slot search +
    // shuffle). Read from exile.ini's [caches] section.
    object_mgr_.set_active_primary_slots(cfg.primary_slots);
    object_mgr_.set_active_secondary_slots(cfg.secondary_slots);

    // Seed RNG
    rng_.seed(0x49, 0x52, 0x56, 0x49);

    // &0860-&08b4 initial object table: Triax pre-placed at (&99, &3b)
    // adjacent to the destinator tertiary, so frame-1 update_triax fires
    // the absorb + teleport beat without any scripted cutscene. Skipped
    // when [creatures] spawn_initial_triax = false so the upper world
    // can be explored without the frame-1 grab.
    if (cfg.spawn_initial_triax) {
        Object& triax = object_mgr_.object(1);
        object_mgr_.init_object_from_type(triax, ObjectType::TRIAX);
        triax.x = {0x99, 0x64};
        triax.y = {0x3b, 0x20};
    }

    // PIPE at (198, 190) re-typed BLUE_CYAN_IMP -> CRAB when
    // [creatures] pipe_198_190_crab is true (default).
    if (cfg.pipe_198_190_crab) {
        constexpr uint8_t pipe_x = 198;
        constexpr uint8_t pipe_y = 190;
        uint16_t idx = landscape_.tertiary_index_at(pipe_x, pipe_y);
        if (idx != Landscape::NO_TERTIARY) {
            TertiaryEntry& entry = landscape_.tertiary_entry_mut(idx);
            entry.type = static_cast<uint8_t>(ObjectType::CRAB);
        }
    }

    // [startup_spawns] — one create_object per entry. min_free_slots=0
    // so an entry can fill the last free slot if the user has stacked a
    // lot of them. Skipped silently when the slot pool is exhausted —
    // create_object's "replace most distant" path already handles that
    // gracefully, but we don't want to thrash it during startup.
    for (const auto& s : cfg.startup_spawns) {
        if (s.type >= static_cast<uint8_t>(ObjectType::COUNT)) continue;
        int slot = object_mgr_.create_object(
            static_cast<ObjectType>(s.type),
            /*min_free_slots=*/0,
            s.tile_x, s.x_frac,
            s.tile_y, s.y_frac);
        (void)slot;
    }

    // Port-only startup rigs (red-slime damage demo + optional
    // creature stress grid / grenade chain / icer drop). Body lives in
    // game_debug.cpp alongside the other debug helpers.
    spawn_test_rigs(cfg.stress_test, cfg.grenade_chain, cfg.icer_drop);

    // Truncate + open the lifecycle log. 
    if (cfg.logs_enabled) {
        debug_log_.open("exile-debug.log",
                        std::ios::out | std::ios::trunc);
    }
    dump_init_diagnostics();

    // Seed the activation anchor to the player's spawn tile before flushing
    // so EVT_SEC_INIT entries recorded by object_mgr_.init() print with
    // sensible dx/dy relative to the player, not the default (0,0) anchor.
    // The main loop re-sets this every frame from the live player position.
    object_mgr_.set_activation_anchor(player.x.whole, player.y.whole);

    // Promote every in-range secondary BEFORE frame 1 — matches the &15ce
    // full-scan path at &0c4e. Without this seed, our one-per-frame
    // promote_selective misses the destinator and the Triax intro misfires.
    object_mgr_.promote_distance_check();
    flush_debug_log();

    running_ = true;
    // Start the jsbeeb bridge listener early so a browser pasting the
    // DevTools snippet on xania.org connects immediately, before any J
    // press. Failure is silent (port already in use, etc.) — J just
    // becomes a no-op in that case.
    JsbeebBridge::start();

    return true;
}

void Game::run() {
    using clock = std::chrono::steady_clock;
    // Locked logic+render rate from [debug] target_fps. Logic and render
    // tick together so motion is judder-free; higher rates fast-forward
    // the game (audio aligned via Audio::set_logic_rate in init).
    auto frame_duration = std::chrono::microseconds(1'000'000 / target_fps_);

    // 30-frame rolling FPS window.
    auto fps_window_start = clock::now();
    int  fps_frame_count  = 0;

    while (running_) {
        auto frame_start = clock::now();
        tick();
        if (show_fps_) {
            fps_frame_count++;
            if (fps_frame_count >= 30) {
                double ms = std::chrono::duration<double, std::milli>(
                    clock::now() - fps_window_start).count();
                if (ms > 0.0) fps_value_ = 1000.0 * fps_frame_count / ms;
                fps_window_start = clock::now();
                fps_frame_count  = 0;
            }
        }
        auto frame_end = clock::now();
        auto elapsed = frame_end - frame_start;
        if (elapsed < frame_duration) {
            std::this_thread::sleep_for(frame_duration - elapsed);
        }
    }
}

void Game::tick() {
    {
        // Main game loop sequence (matching &19b6). Order inside the
        // frame is: input -> toggles -> anchor -> world updates -> render.
        // While paused the world-update block is skipped so the current
        // state snapshot can be inspected in the banner without values
        // changing every frame.
        process_input();

        // Activation-anchor mode is driven by the "Map mode"
        activation_from_camera_ = renderer_->map_mode_enabled();

        // Rising-edge toggle on Esc: freeze / unfreeze world updates,
        // OR commit a scrubbed frame as the new "live" state. Esc while
        // scrubbing snaps the ring buffer to the current scrubbed frame
        // (subsequent ticks overwrite the now-stale future frames) and
        // resumes the sim from there, branching the timeline.
        {
            bool down = input_.state().toggle_pause;
            if (down && !pause_key_prev_) {
                if (scrubbing_) {
                    // The state is already restored to the scrubbed
                    // frame; collapse the ring so head_ points just
                    // past the scrubbed frame and count_ reflects the
                    // truncated history.
                    size_t live_idx = (snapshot_ring_head_ +
                                       snapshot_ring_.size() - 1) %
                                      snapshot_ring_.size();
                    size_t cur_idx = (live_idx +
                                      snapshot_ring_.size() -
                                      scrub_offset_) %
                                     snapshot_ring_.size();
                    snapshot_ring_head_ = (cur_idx + 1) %
                                           snapshot_ring_.size();
                    snapshot_ring_count_ -= scrub_offset_;
                    scrub_offset_ = 0;
                    scrubbing_ = false;
                } else {
                    paused_ = !paused_;
                }
            }
            pause_key_prev_ = down;
        }

        // Rewind scrubber. Numpad '-' steps back through the snapshot
        // ring; numpad '*' steps forward. Both auto-repeat while held —
        // one frame per game tick — so the user can sweep through the
        // buffer continuously. Entering scrub mode freezes the sim;
        // pressing Esc (above) commits the current frame and resumes.
        {
            bool back = input_.state().scrub_back;
            bool fwd  = input_.state().scrub_forward;
            if (back && scrub_offset_ + 1 < snapshot_ring_count_) {
                scrubbing_ = true;
                scrub_offset_++;
                // Index of the live entry just written = head_-1.
                size_t live_idx = (snapshot_ring_head_ +
                                   snapshot_ring_.size() - 1) %
                                  snapshot_ring_.size();
                size_t cur_idx = (live_idx +
                                  snapshot_ring_.size() -
                                  scrub_offset_) %
                                 snapshot_ring_.size();
                restore_snapshot(snapshot_ring_[cur_idx]);
            }
            if (fwd && scrubbing_ && scrub_offset_ > 0) {
                scrub_offset_--;
                size_t live_idx = (snapshot_ring_head_ +
                                   snapshot_ring_.size() - 1) %
                                  snapshot_ring_.size();
                size_t cur_idx = (live_idx +
                                  snapshot_ring_.size() -
                                  scrub_offset_) %
                                 snapshot_ring_.size();
                restore_snapshot(snapshot_ring_[cur_idx]);
                if (scrub_offset_ == 0) scrubbing_ = false;
            }
        }

        // Rising-edge save-map ('\' key) — write the current 256×256
        // landscape grid to exile.map so the next launch picks it up
        // via load_from_file. Surfaces a short "Saved" / "Save FAILED"
        // banner via editor_save_msg_until_frame_; the overlay-compose
        // block in render() reads that to know whether to draw it.
        {
            bool down = input_.state().save_map;
            if (down && !save_map_key_prev_) {
                bool ok = landscape_.save_to_file("exile.map");
                editor_save_msg_until_frame_ =
                    frame_counter_ + (ok ? 100 : 200); // ~2s / 4s
                editor_save_msg_ok_ = ok;
            }
            save_map_key_prev_ = down;
        }

        // Rising-edge tertiary data-byte bumps ('[' / ']'). Always sync
        // the prev-state at the end so a held key only fires once per
        // press; gate the actual mutation on editor mode + highlight +
        // valid entry.
        {
            bool inc = input_.state().tert_data_inc;
            bool dec = input_.state().tert_data_dec;
            bool inc_edge = inc && !tert_data_inc_prev_;
            bool dec_edge = dec && !tert_data_dec_prev_;
            if ((inc_edge || dec_edge) &&
                renderer_->editor_enabled() && editor_has_highlight_) {
                uint16_t idx = landscape_.tertiary_index_at(
                    editor_highlight_x_, editor_highlight_y_);
                if (idx != Landscape::NO_TERTIARY) {
                    TertiaryEntry& e = landscape_.tertiary_entry_mut(idx);
                    if (inc_edge) e.data = static_cast<uint8_t>(e.data + 1);
                    if (dec_edge) e.data = static_cast<uint8_t>(e.data - 1);
                    // Push the change into any live primary spawned
                    // from this entry, so the in-flight door / switch /
                    // transporter sees the new state immediately.
                    // Primaries strip bit 7 from the data byte at spawn
                    // (it's the "needs spawn" gate), so we mirror that
                    // here. This is what makes the visual update — the
                    // entry's data byte alone only affects the next
                    // (re)spawn.
                    for (int i = 1;
                         i < GameConstants::PRIMARY_OBJECT_SLOTS; ++i) {
                        Object& p = object_mgr_.object(i);
                        if (p.is_active() && p.tertiary_slot == idx) {
                            p.tertiary_data_offset =
                                static_cast<uint8_t>(e.data & 0x7f);
                        }
                    }
                    editor_save_msg_until_frame_ = frame_counter_ + 50;
                    editor_save_msg_ok_ = true;
                }
            }
            tert_data_inc_prev_ = inc;
            tert_data_dec_prev_ = dec;
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

        if (!paused_ && !scrubbing_) {
            object_mgr_.reset_debug_counters();
            // [player] invincible — reset HP to full at the top of every
            // tick 
            if (invincible_) object_mgr_.player().energy = 0xff;
            update_timers();
            // &19c9 update_background_flash — tick the sky-flash cooldown
            // before any updates so renderer sees the latest clear colour.
            update_background_flash();
            // Debug overlay rings (damage numbers, floating labels) —
            // body in game_debug.cpp.
            tick_overlay_visuals();
            // Chained-grenade test rig — no-op unless the init() seed
            // block is enabled. Body lives in game_debug.cpp.
            tick_test_grenades();

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

            // Random events 
            update_events();

            // Tick the particle pool (port of &207e update_particles).
            {
                const Object& p = object_mgr_.player();
                uint8_t wy = Water::get_waterline_y(p.x.whole);
                particles_.update(wy, 0, rng_);
            }

            // &4a1c ROR &29d7 — clear player_object_fired at end of tick
            player_object_fired_ = 0xff;

            // Capture this frame into the rewind ring. Writes the
            // post-tick state so scrubbing back lands on a complete
            // frame. When the buffer is full, head_ wraps and the
            // oldest entry is overwritten in place.
            snapshot_ring_[snapshot_ring_head_] = snapshot();
            snapshot_ring_head_ =
                (snapshot_ring_head_ + 1) % snapshot_ring_.size();
            if (snapshot_ring_count_ < snapshot_ring_.size()) {
                snapshot_ring_count_++;
            }
        }

        render();

        // Update the audio listener position for distance-attenuated
        // sounds (play_at). Tracks the player tile every frame so
        // moving away from a creature or door drops its volume.
        {
            const Object& player = object_mgr_.player();
            Audio::set_listener(player.x.whole, player.y.whole);
        }

        // Push one frame of audio. Has to happen every iteration of the
        // main loop (paused or not) — the device's ring buffer drains
        // continuously, and skipping a tick produces audible underrun
        // clicks. Audio::tick is silent when no channels are active and
        // a no-op when audio failed to open.
        Audio::tick();

        // Flush lifecycle events AFTER render() so CREATE events emitted
        // by spawn_tertiary_object (which fires from the tile-plotting
        // path inside render) are captured in the log. Flushing before
        // render silently dropped those events — the ret/cre pair for a
        // churning tertiary looked one-sided.
        if (!paused_) {
            flush_debug_log();
        }
    }
}

// Port of &19b6-&19c7 update_timers:
//   &19b6 LSR &27          ; whistle_one_active = 0
//   &19b8 INC &c0          ; ++frame_counter
//   &19ba LDA &c0
//   &19bc LDY #&ff
//   &19be LDX #&05
//   &19c0 LSR A             ; loop: shift LSB out into carry
//   &19c1 BCC &19c4         ; if bit was 0, leave Y at &ff (timer "fires")
//   &19c3 INY               ; bit was 1 → Y = 0 (timer doesn't fire)
//   &19c4 STY &c1,X         ; write every_{64,32,16,8,4,2}_frames
//   &19c6 DEX / BPL &19c0   ; repeat for all 6 timers
// Net effect: every_two_frames is "true" when frame_counter bit 0 == 0,
// every_four_frames when bits 0-1 == 0, etc.
void Game::update_timers() {
    // Clear per-frame flags (port of &19b6: LSR &27 for whistle one,
    // and the &1aa4-&1aa9 ROR &29d8 chain for whistle two — the 6502
    // clears the activator whenever the activator's own slot processes,
    // so the flag survives at most one frame across all NPC updates).
    whistle_one_active_ = false;
    whistle_two_activator_ = 0xff;

    frame_counter_++;
    uint8_t a = frame_counter_;

    every_two_frames_        = (a & 0x01) == 0;
    every_four_frames_       = (a & 0x03) == 0;
    every_eight_frames_      = (a & 0x07) == 0;
    every_sixteen_frames_    = (a & 0x0f) == 0;
    every_thirty_two_frames_ = (a & 0x1f) == 0;
    every_sixty_four_frames_ = (a & 0x3f) == 0;

    // &19df-&19e4: while explosion_timer is non-zero (negative since
    // start_explosion_timer set it to -50), INC toward 0. AI uses this
    // as the "explosion in progress" flag — see start_explosion_timer.
    if (explosion_timer_ != 0) explosion_timer_++;
}

void Game::process_input() {
    input_.clear();

    int key;
    while ((key = renderer_->get_key()) != InputKey::NONE) {
        input_.process_key(key);
    }

    // Merge in the BBC's input from jsbeeb if mirror mode is on. The
    // browser-side bridge-client polls BBC's action_keys_pressed and
    // POSTs to /bridge/input each frame; JsbeebBridge::merge_into
    // OR-s those into our local InputState so either keyboard can
    // drive the port.
    if (bridge_sync_on_) {
        InputState merged = input_.state();
        JsbeebBridge::merge_into(merged);
        input_.set_state(merged);
    }

    // Window-close path: get_key returns InputKey::CLOSE_REQUESTED when
    // the user clicked the title-bar X. No key binding to this — only
    // the renderer's should_close flag drives it.
    if (input_.state().quit) {
        running_ = false;
    }

    // Save / load edge detection. Holding ';' would otherwise overwrite the
    // save every frame and thrash the disk; only fire on the 0->1 transition.
    // While scrubbing, save_game serialises the currently-restored frame —
    // the rewind path already mutated the live state to match it.
    bool save_down = input_.state().save_game;
    if (save_down && !save_key_prev_) {
        save_game("exile.sav");
    }
    save_key_prev_ = save_down;

    bool load_down = input_.state().load_game;
    if (load_down && !load_key_prev_) {
        load_game("exile.sav");
    }
    load_key_prev_ = load_down;

    // Shift+';' (== ':') dumps the entire rewind ring buffer as a
    // multi-frame trace. Useful when bisecting a bug across many frames
    // — open the file and search for the misbehaving entity by slot.
    bool dump_down = input_.state().dump_all_frames;
    if (dump_down && !dump_key_prev_) {
        dump_ring_buffer("exile-frames.txt");
    }
    dump_key_prev_ = dump_down;

    // 'J' — toggle jsbeeb mirror mode. First frame after toggling on,
    // push the full world state (objects, weapons, keys, tertiary,
    // secondary, zero-page RNG / pose / frame counter). On every
    // subsequent frame, push only the action_keys_pressed table so the
    // BBC drives its own simulation from the same input we're seeing.
    // This avoids the per-frame poke of object positions that was
    // leaving ghost pixels on the BBC framebuffer.
    bool bridge_down = input_.state().bridge_push;
    bool just_toggled_on = false;
    if (bridge_down && !bridge_key_prev_) {
        bridge_sync_on_ = !bridge_sync_on_;
        just_toggled_on = bridge_sync_on_;
    }
    bridge_key_prev_ = bridge_down;

    if (bridge_sync_on_) {
        std::vector<JsbeebBridge::Write> writes;

        if (just_toggled_on) {
            // -------- FULL INITIAL SYNC --------
            // 16 primary slots × 14 fields (bases at &0860..&0936).
            // Loop hard-capped at 16: our primary_ is 64 wide but the
            // BBC has only 16 slots' worth of RAM tables.
            for (int i = 0; i < 16; ++i) {
                const Object& o = object_mgr_.object(i);
                const uint16_t s = static_cast<uint16_t>(i);
                writes.push_back({ uint16_t(0x0860 + s),
                                   static_cast<uint8_t>(o.type) });
                writes.push_back({ uint16_t(0x0870 + s), o.sprite });
                writes.push_back({ uint16_t(0x0880 + s), o.x.fraction });
                writes.push_back({ uint16_t(0x0891 + s), o.x.whole });
                writes.push_back({ uint16_t(0x08a3 + s), o.y.fraction });
                writes.push_back({ uint16_t(0x08b4 + s), o.y.whole });
                writes.push_back({ uint16_t(0x08c6 + s), o.flags });
                writes.push_back({ uint16_t(0x08d6 + s), o.palette });
                writes.push_back({ uint16_t(0x08e6 + s),
                                   static_cast<uint8_t>(o.velocity_x) });
                writes.push_back({ uint16_t(0x08f6 + s),
                                   static_cast<uint8_t>(o.velocity_y) });
                writes.push_back({ uint16_t(0x0906 + s),
                                   o.target_and_flags });
                writes.push_back({ uint16_t(0x0916 + s), o.tx });
                writes.push_back({ uint16_t(0x0926 + s), o.energy });
                writes.push_back({ uint16_t(0x0936 + s), o.ty });
            }
            // Inventory.
            writes.push_back({ 0x084d, player_weapon_ });
            for (int i = 0; i < 6; ++i) {
                writes.push_back({ uint16_t(0x080e + i),
                                   player_weapons_collected_[i] });
            }
            for (int i = 0; i < 6; ++i) {
                uint16_t e = weapon_energy_[i];
                writes.push_back({ uint16_t(0x084e + i),
                                   static_cast<uint8_t>(e & 0xff) });
                writes.push_back({ uint16_t(0x0854 + i),
                                   static_cast<uint8_t>(e >> 8) });
            }
            static constexpr uint16_t kKeyAddrs[6] = {
                0x0806, 0x0807, 0x0808, 0x0809, 0x080b, 0x080c,
            };
            for (int i = 0; i < 6; ++i) {
                writes.push_back({ kKeyAddrs[i],
                                   player_keys_collected_[i] });
            }
            // Zero-page pose, RNG, frame counter, held-object slot.
            // &dd needs to be negative (top bit set) for the BBC's
            // &2d36 handle_firing to allow firing; if jsbeeb has a stale
            // positive value left from earlier gameplay, every fire
            // press is a silent no-op (&2d3b BPL leave).
            writes.push_back({ 0x00de, player_angle_ });
            writes.push_back({ 0x00df, player_facing_ });
            writes.push_back({ 0x00dd, held_object_slot_ });
            writes.push_back({ 0x00d9, rng_.state(0) });
            writes.push_back({ 0x00da, rng_.state(1) });
            writes.push_back({ 0x00db, rng_.state(2) });
            writes.push_back({ 0x00dc, rng_.state(3) });
            writes.push_back({ 0x00c0, frame_counter_ });
            // Tertiary linkage.
            for (int i = 0; i < 16; ++i) {
                const Object& o = object_mgr_.object(i);
                uint8_t off = (o.tertiary_slot <= 0xff)
                                  ? static_cast<uint8_t>(o.tertiary_slot)
                                  : 0u;
                writes.push_back({ uint16_t(0x0966 + i), off });
            }
            int n_tert = landscape_.tertiary_count();
            if (n_tert > 256) n_tert = 256;
            for (int i = 0; i < n_tert; ++i) {
                if (i == 0x28) continue;  // engine activation flag
                writes.push_back({ uint16_t(0x0986 + i),
                                   landscape_.tertiary_entry(i).data });
            }
            // Secondary objects (capped at BBC's 32 slots).
            for (int i = 0; i < 32; ++i) {
                const SecondaryObject& s = object_mgr_.secondary(i);
                writes.push_back({ uint16_t(0x0af2 + i), s.x });
                writes.push_back({ uint16_t(0x0b12 + i), s.y });
                writes.push_back({ uint16_t(0x0b32 + i), s.type });
                writes.push_back({ uint16_t(0x0b53 + i),
                                   s.energy_and_fractions });
            }
            JsbeebBridge::poke(writes);
        }
    }

    // Test-events panel: poll the renderer for clicks each tick.
    int event_id;
    if (renderer_->consume_event_click(event_id)) {
        trigger_event(event_id);
    }
}

// trigger_event lives in game_debug.cpp.

// Port of &25a1-&25df update_triax_lab. Maggot count refill, periodic
// maggot spawn, door drive from the lab waterline, desired_y rewrite.
void Game::update_triax_lab() {
    // Smooth-flood subframe drain for the test trigger. No-op when the
    // counter is exhausted. Body in game_debug.cpp.
    tick_test_flood();

    // &259a-&259f: when flooding_state bit 7 is set, desired_y[1] jumps
    // to 0x67 (above the upper world) and the lab work is skipped.
    if (flooding_state_ & 0x80) {
        Water::set_desired_y(1, 0x67);
        return;
    }

    // &25a1-&25a3: maggot machine never runs out while operational.
    object_mgr_.set_tertiary_data_byte(0x02, 0x60);

    // &25a6-&25c3: every 64 frames spawn one maggot at the machine's
    // transporter tile (0x61, 0xd9) with a 0x70 y-fraction.
    if (every_sixty_four_frames_) {
        int slot = object_mgr_.create_object(
            ObjectType::MAGGOT, /*min_free_slots=*/4,
            0x61, 0x61, 0xd9, 0x70);
        (void)slot;  // 6502 BCS-skips on failure; matches our no-op-on-fail.
    }

    // &25c3-&25dd: every 32 frames re-evaluate the bottom-of-lab door
    // (tertiary entry 0xc2) — open if Triax-lab waterline sits above
    // 0xe0, else close. Drives desired_y[1] to 0xe2 (drain) or 0xd2 (fill).
    uint8_t door_data = object_mgr_.tertiary_data_byte(0xc2);
    if (every_thirty_two_frames_) {
        door_data &= 0xfd;  // clear DOOR_FLAG_OPENING
        if (Water::get_y(1) < 0xe0) door_data |= 0x02;
        object_mgr_.set_tertiary_data_byte(0xc2, door_data);
    }
    Water::set_desired_y(1, (door_data & 0x02) ? 0xe2 : 0xd2);
}

// Port of update_events (&259a-&2742): stars, Triax summoning, earthquake,
// clawed-robot respawns. Now also runs Triax-lab + waterline tick.
void Game::update_events() {
    // &25a1-&25df: Triax lab door / maggot machine. Always run; this
    // sets desired_y[1] which drives the waterline integrator below.
    update_triax_lab();

    // &2626-&265b update_waterlines_loop. Steps all 4 waterline ranges
    // toward their desired_y values once per frame.
    Water::update_waterlines(frame_counter_);

    const Object& player = object_mgr_.player();

    // Random-tile star-field spawn (&26c8-&26e6). 6502 picks one
    // tile per frame via &2662 get_random_tile_near_player and runs
    // exactly one star spawn attempt. We keep the viewport-sized sample
    // box (our viewport is much wider than the BBC's ±4) but drop the
    // count to a single attempt — was up to 16/frame, which produced a
    // sky that's 16× as dense as the original.
    int vp_w = renderer_->viewport_width_tiles();
    int vp_h = renderer_->viewport_height_tiles();
    if (vp_w < 8) vp_w = 8;
    if (vp_h < 8) vp_h = 8;
    int half_w = vp_w / 2;
    int half_h = vp_h / 2;
    // Player teleport suppression — &26da reads `player_is_completely_
    // dematerialised` at &19b5, which the teleport state machine sets at
    // mid-animation while the player is briefly removed from the world.
    // Our analogue is the TELEPORTING object flag on the player; the
    // generic teleport handler arms it for the full 32-frame animation
    // (close enough to the 6502's "completely dematerialised" window for
    // the purpose of suppressing ambient stars).
    bool player_teleporting =
        (player.flags & ObjectFlags::TELEPORTING) != 0;
    {
        int dx = (rng_.next() % vp_w) - half_w;
        int dy = (rng_.next() % vp_h) - half_h;
        uint8_t tx = static_cast<uint8_t>(camera_.center_x + dx);
        uint8_t ty = static_cast<uint8_t>(camera_.center_y + dy);
        // &26ca CMP #&4e / BCS skip: sky ends at world y = 0x4e.
        // &26da-&26df: ORA player_is_completely_dematerialised /
        // ORA tile_was_from_map_data / BMI skip. Suppresses stars while
        // the player is teleporting and inside the spaceship / Triax's
        // lab (tiles from map_data rather than the algorithm).
        if (ty < 0x4e && !player_teleporting &&
            !landscape_.tile_from_map_data(tx, ty)) {
            particles_.emit_at(ParticleType::STAR_OR_MUSHROOM, tx, ty, rng_);
        }
    }
    // Mushroom-tile EVENTS branch below still needs ONE random tile near
    // the player (the 6502 uses the same picked tile for both star and
    // mushroom paths). Pick fresh ±3-4 coords in player space so mushroom
    // ball spawns stay tied to the player rather than the camera anchor.
    {
        uint8_t dx_rand = static_cast<uint8_t>(rng_.next() & 0x07);
        uint8_t dy_rand = static_cast<uint8_t>(rng_.next() & 0x07);
        uint8_t tx = static_cast<uint8_t>(player.x.whole + dx_rand - 3);
        uint8_t ty = static_cast<uint8_t>(player.y.whole + dy_rand - 3);

    // &3fd2 update_mushroom_tile EVENTS branch (&3fde-&3fe9). Red spawns at
    // y_frac=0xff (tile bottom), blue v-flipped at 0x00. &4028 CPY #&04
    // caps at 4 per type. Port inlines the dispatch (tile pick done above).
        uint8_t tile = landscape_.get_tile(tx, ty);
        uint8_t type = tile & TileFlip::TYPE_MASK;
        if (type == static_cast<uint8_t>(TileType::MUSHROOMS)) {
            bool is_blue = (tile & TileFlip::VERTICAL) != 0;
            ObjectType ball_type = is_blue
                ? ObjectType::BLUE_MUSHROOM_BALL
                : ObjectType::RED_MUSHROOM_BALL;
            int count = 0;
            for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
                const Object& o = object_mgr_.object(i);
                if (o.is_active() && o.type == ball_type) count++;
            }
            if (count < 4) {
                uint8_t x_frac = rng_.next();                  // &403c
                uint8_t y_frac = is_blue ? 0x00 : 0xff;        // &3fe4 / &3fe6
                object_mgr_.create_object(ball_type, /*min_free_slots=*/0,
                                          tx, x_frac, ty, y_frac);
            }
        }
    }

    // &3e1b update_nest_or_pipe_tile's consider_spawning branch (&3e48).
    // Live 9x9 scan via resolve_tile_with_tertiary (no canonical pipe y in
    // tertiary storage). Proximity gate emulates the 6502 only running
    // this on collision-mode tile processing.
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            uint8_t tx = static_cast<uint8_t>(player.x.whole + dx);
            uint8_t ty = static_cast<uint8_t>(player.y.whole + dy);
            ResolvedTile res = resolve_tile_with_tertiary(landscape_, tx, ty);
            if (res.data_offset <= 0) continue;
            uint8_t ttype = res.tile_and_flip & TileFlip::TYPE_MASK;
            if (ttype != static_cast<uint8_t>(TileType::NEST) &&
                ttype != static_cast<uint8_t>(TileType::PIPE)) continue;
            // 6502 &3e4b CMP #&f7 / BCC leave: 9-in-256 spawn chance per
            // event tick. The 6502 only ticks this routine during random-
            // event / tile-plot / collision passes; we run it for every
            // nearby nest tile every frame. Net rate per tile per frame
            // = ~3.5%, giving an imp ~once a second from a single nearby
            // nest. Was &ff (1-in-256, ~8 sec/spawn) — too slow to feel
            // like an active nest.
            if (rng_.next() < 0xf7) continue;

            uint8_t data = object_mgr_.tertiary_data_byte(res.data_offset);
            // &3e56 ASL; CMP #&08; BCC leave — creature-count bits sit at
            // positions 2..6; at least one remaining iff (data & 0x7c) != 0.
            bool has_creatures = (data & 0x7c) != 0;
            // &3e5b AND #&06; BNE leave — bits 1..0 are the "inactive" flag.
            bool active = (data & 0x03) == 0;
            if (!has_creatures || !active) continue;
            uint8_t ctype = object_mgr_.tertiary_type_byte(res.type_offset);
            if (ctype >= static_cast<uint8_t>(ObjectType::COUNT)) continue;

            // &3e68 create_primary_object_from_tertiary_if_Y_slots_free —
            // 5 spare slots required (X=5 in events/collision at &3e1d).
            int slot = object_mgr_.create_object(
                static_cast<ObjectType>(ctype), 5,
                tx, 0x80, ty, 0x80);
            if (slot <= 0) continue;

            Object& spawn = object_mgr_.object(slot);

            // &1edf seeds target_and_flags = slot; update_fireball reads
            // its low 5 bits as permanent / temporary. Scoped to fireball
            // types so the port's "target=0 -> player" convention used by
            // wasp / imp / slime AI keeps working.
            if (spawn.type == ObjectType::FIREBALL ||
                spawn.type == ObjectType::MOVING_FIREBALL) {
                spawn.target_and_flags = static_cast<uint8_t>(
                    (spawn.target_and_flags & ~TargetFlags::OBJECT_MASK) |
                    (slot & TargetFlags::OBJECT_MASK));
            }

            uint8_t sid = spawn.sprite;
            uint8_t sprite_h_byte = 0;
            uint8_t sprite_w_byte = 0;
            if (sid <= 0x80) {
                int w = sprite_atlas[sid].w;
                int h = sprite_atlas[sid].h;
                sprite_w_byte = static_cast<uint8_t>((w > 0 ? (w - 1) : 0) * 16);
                sprite_h_byte = static_cast<uint8_t>((h > 0 ? (h - 1) : 0) * 8);
            }

            // Port-only emergence shift for fireball in pipes
            bool v_flipped = (res.tile_and_flip & TileFlip::VERTICAL) != 0;
            spawn.y.fraction = v_flipped
                ? static_cast<uint8_t>(0x08)
                : static_cast<uint8_t>(0xf8);
            (void)sprite_h_byte;

            // &3e7d-&3e83 centre the spawn horizontally within the tile.
            // This overrides whatever &4072 set.
            spawn.x.fraction = static_cast<uint8_t>((~sprite_w_byte & 0xff) >> 1);

            // &4081-&4083 store tertiary_data_offset so return_to_tertiary
            // can credit the creature back to the nest (else nest drains
            // permanently). uint16_t — see tertiary_spawn.cpp:253.
            spawn.tertiary_slot = static_cast<uint16_t>(res.data_offset);

            // &3e6d-&3e6f SBC #&03 with carry clear = subtract 4: decrements
            // remaining creature count (bit 2 = low count bit).
            object_mgr_.set_tertiary_data_byte(
                res.data_offset, static_cast<uint8_t>(data - 4));
        }
    }

    // Earthquake progression (&25e2-&2610). negative state = running;
    // worsens more quickly early, tapers toward 0x21. Skipped: BBC R2
    // sync register writes (&2604-&260a) for raster shudder.
    if (earthquake_state_ & 0x80) {
        uint8_t a = static_cast<uint8_t>(earthquake_state_ << 1);
        // CMP rnd: carry set more often as earthquake progresses.
        bool carry = (a >= rng_.next());
        if (every_eight_frames_ && a != 0x21 &&
            ((a & 0x10) || carry)) {
            earthquake_state_++;
        }
    }

    // Triax summoning (&26e6-&2711). Trigger gate: (late earthquake OR
    // flooding OR every-32-frames) AND 1/256, AND player y>=0x94 unless
    // flooding, AND not already primary. Spawns at y=0xfe so first tick
    // teleports him up to the player.
    {
        // &26e6-&26f2 trigger: `(late_earthquake AND flooding_state) OR
        // every_32_frames` at bit 7. late_earthquake on its own never
        // contributes — it's masked by flooding. Removing the bare
        // late_earthquake OR stops the trigger firing constantly once
        // earthquake_state has wrapped (which it does within a few
        // seconds of gameplay, widely before endgame).
        bool trigger = (flooding_state_ & 0x80) || every_thirty_two_frames_;
        if (trigger && rng_.next() == 0) {                    // 1-in-256
            bool lower_world = player.y.whole >= 0x94;
            if (lower_world || (flooding_state_ & 0x80)) {
                // Check Triax not already present.
                bool present = false;
                for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
                    const Object& o = object_mgr_.object(i);
                    if (o.is_active() && o.type == ObjectType::TRIAX) {
                        present = true;
                        break;
                    }
                }
                if (!present) {
                    int slot = object_mgr_.create_object(
                        ObjectType::TRIAX, /*min_free_slots=*/4,
                        player.x.whole, 0x00, 0xfe, 0x00);
                    if (slot > 0) {
                        Object& triax = object_mgr_.object(slot);
                        // &273d: &c0 = DIRECTNESS_THREE | player-slot 0.
                        triax.target_and_flags = 0xc0;
                    }
                }
            }
        }
    }

    // -----------------------------------------------------------------
    // Clawed-robot summoning (&2714-&2742)
    // -----------------------------------------------------------------
    // Every 8 frames, pick a random robot slot (0..3). If it's dormant
    // (availability < 0) or already active (> 0) we bail. Otherwise
    // teleport-energy ticks up; once it overflows past 0x80 the robot is
    // created as OBJECT_MAGENTA_CLAWED_ROBOT + slot and marked active.
    if (every_eight_frames_) {
        uint8_t r = rng_.next() & 0x03;
        int8_t avail = static_cast<int8_t>(clawed_robot_availability_[r]);
        if (avail == 0) {
            // &2725-&2728: INC teleport_energy ; BPL leave. Only spawn
            // once the counter overflows past 0x7f (signed positive ->
            // signed negative); a still-positive byte means "recharging".
            clawed_robot_teleport_energy_[r]++;
            if (static_cast<int8_t>(clawed_robot_teleport_energy_[r]) < 0) {
                // Counter wrapped past 0x80 — robot has enough teleport
                // energy to rejoin the game.
                ObjectType t = static_cast<ObjectType>(
                    static_cast<uint8_t>(ObjectType::MAGENTA_CLAWED_ROBOT) + r);
                int slot = object_mgr_.create_object(
                    t, /*min_free_slots=*/4,
                    player.x.whole, 0x00, 0xfe, 0x00);
                if (slot > 0) {
                    Object& robot = object_mgr_.object(slot);
                    robot.target_and_flags = 0xc0;     // DIRECTNESS_THREE + player
                    clawed_robot_availability_[r] = 0x01;   // active
                }
            }
        }
    }
}

// Three-phase player update: read input -> integrate motion -> pick sprite.
// The two halves live in player_actions.cpp / player_motion.cpp so the
// input and physics concerns can evolve independently.
void Game::update_player() {
    Object& player = object_mgr_.player();
    const auto& inp = input_.state();

    // Energy-loss teleport (&4096 consider_teleporting_damaged_player). The
    // 6502 wires this into the explosion dispatch for the &10 indestructible
    // explosion type — if the player's energy would reach zero, he teleports
    // back to a remembered position instead of exploding. Our player slot is
    // skipped by update_objects, so the object loop's energy-zero branch
    // never catches us; check it explicitly here before anything else.
    //
    // Gate on !TELEPORTING: if a teleport is already in progress (i.e.
    // a previous consider_ call this death set the flag + timer), another
    // damage tick that drives energy back to 0 must NOT re-enter
    // handle_player_teleporting. Doing so consumes a second remembered
    // slot — and once remembered hits 0, it picks the fallback (the
    // spaceship at slot 4), overwriting tx/ty so the in-progress
    // animation relocates the player to the ship instead of the
    // intended remembered position.
    if (player.energy == 0 &&
        !(player.flags & ObjectFlags::TELEPORTING)) {
        consider_teleporting_damaged_player(player);
    }

    // Drive the teleport animation if active. Skips the normal input /
    // motion chain for this frame — the player is briefly dematerialised,
    // then reappears at (tx, ty) with zero velocity.
    if (advance_player_teleport(player)) {
        update_player_sprite(0, 0);
        return;
    }

    int8_t accel_x = 0;
    int8_t accel_y = 0;

    // Snapshot pre-input state so the log shows what apply_player_input was
    // looking at, and the post-integrate values can be compared.
    int8_t pre_vx = player.velocity_x;
    int8_t pre_vy = player.velocity_y;
    uint8_t pre_xf = player.x.fraction;
    uint8_t pre_yf = player.y.fraction;

    apply_player_input(player, inp, accel_x, accel_y);
    integrate_player_motion(player, accel_x, accel_y);
    update_player_sprite(accel_x, accel_y);
    tick_blaster();

    // State-change-only walking trace — body in game_debug.cpp.
    log_walking_diagnosis(player, inp, pre_vx, pre_vy, pre_xf, pre_yf,
                          accel_x, accel_y);
}

// Port of &32c8 handle_dropping_object:
//   &32c8 BIT &dd ; player_object_held   ; negative = not holding
//   &32ca BMI &32c7 ; leave
//   &32cc SEC / ROR &dd                  ; flip MSB → not_holding
//   &32cf JMP &14a5 ; play_high_beep
// Port simplified: we don't reuse the 6502's "holding-slot" register;
// just release the primary.
void Game::drop_held_object(Object& player) {
    if (held_object_slot_ >= 0x80) return;
    Object& held = object_mgr_.object(held_object_slot_);
    HeldObject::drop(held, player, held_object_slot_);
}

// &4a70 blaster discharge: while timer < 0, player acts as a duration-10
// explosion source for 5 frames (timer set to -5 by Weapon::fire).
// &1f97 update_background_flash routes the BBC palette-0 swap (1/8 white,
// 7/8 black via AND #&20; BEQ) into the renderer's clear_colour.
void Game::update_background_flash() {
    if (background_flash_cooldown_ == 0) return;
    background_flash_cooldown_--;
    uint32_t sky;
    if (background_flash_cooldown_ == 0) {
        sky = 0x000000;                  // restore black
    } else if (rng_.next() & 0x20) {
        sky = 0xCCCCCC;                  // 1-in-8: white
    } else {
        sky = 0x000000;                  // 7-in-8: black
    }
    if (renderer_) renderer_->set_clear_colour(sky);
}

void Game::tick_blaster() {
    if (blaster_timer_ >= 0) return;
    blaster_timer_++;
    Object& player = object_mgr_.player();
    // &4a76 start_explosion_timer: arms the AI "explosion in progress"
    // flag for 50 frames (worm/maggot emergence rate, stimuli detection).
    start_explosion_timer();
    // &4a7d play_sound_on_channel_zero. Retriggers each tick on the
    // priority channel — same sound bytes used by a generic explosion
    // (&40db); 5 retriggers in 5 frames give the discharge a continuous
    // boom.
    static constexpr uint8_t kSoundBlaster[4] = { 0x17, 0x03, 0x11, 0x04 };
    Audio::play_at(Audio::CH_PRIORITY, kSoundBlaster,
                   player.x.whole, player.y.whole);

    // &4f9c update_explosion's per-frame palette cycle and particle
    // emit, applied to the player while discharging. The 8-entry mask
    // (&4fbf AND #&13) cycles {kyK,rgK,rmK,rcK,kyR,rgR,rmR,rcR}.
    // update_player_sprite ran earlier this frame so this palette
    // overwrite wins for the duration of the discharge, then the suit /
    // damage-flash logic returns once the timer reaches 0.
    player.palette = rng_.next() & 0x13;
    particles_.emit(ParticleType::EXPLOSION, 2, player, rng_);

    // accelerate_all_objects (&343a-&34b0). Duration = 9 matches the
    // 6502: &4a79-&4a7b sets tertiary_data_offset to 10, then
    // update_explosion DECs to 9 before running the damage loop.
    Behaviors::apply_explosion_radius(object_mgr_, player,
                                       /*source_slot=*/0, /*duration=*/9,
                                       renderer_ && renderer_->damage_overlay_enabled()
                                           ? &damage_events_ : nullptr);
}

// Port of &4096 consider_teleporting_damaged_player. INCs energy to 1 then
// 1/2 roll: teleport (drop held + count death) or stay-put (drop held).
// Deviation: skip the &40ac-&40b8 pocket-retrieve-and-drop penalty —
// pocket items stay put, held-drop still fires.
void Game::consider_teleporting_damaged_player(Object& player) {
    player.energy = 1;  // &409a INC this_object_energy

    // &409f BPL &40ac: branch to the skip path when rnd is positive (bit 7
    // clear). So auto-teleport fires when bit 7 is SET — 1/2 chance.
    uint8_t r = rng_.next();
    if (r & 0x80) {
        drop_held_object(player);
        player_deaths_++;
        handle_player_teleporting(player);
        return;
    }

    drop_held_object(player);
}

// Port of &0cc1 handle_teleporting. Can't voluntarily teleport while
// holding an object (&0cc3). Consumes one remembered position, or falls
// back to slot 4. Sets tx/ty from the tables, flags TELEPORTING, and
// arms the 32-frame animation timer.
void Game::handle_player_teleporting(Object& player) {
    if (held_object_slot_ < 0x80) {
        // &0cc3 BPL leave — player is still holding something. The auto-
        // teleport path in &4096 drops first, but manual 'T' would bail
        // here. Either way, bail cleanly.
        return;
    }

    uint8_t y;
    if (player_teleports_remembered_ > 0) {
        // &0cc5 DEC remembered / &0cd1 DEC next / fix_player_next_teleport.
        player_teleports_remembered_--;
        player_next_teleport_ = (player_next_teleport_ - 1) & 0x03;
        y = player_next_teleport_;
    } else {
        // &0cca fallback path — no remembered positions, use slot 4.
        y = 4;
    }

    player.tx = player_teleports_x_[y];
    player.ty = player_teleports_y_[y];

    // &0ce2 JSR play_sound_for_teleporting. Same sound effect the
    // transporter beam plays — 4-byte block at &4410.
    static constexpr uint8_t kSoundTeleport[4] = { 0x29, 0xc2, 0x37, 0xf3 };
    Audio::play(Audio::CH_ANY, kSoundTeleport);

    player.flags |= ObjectFlags::TELEPORTING;
    player.timer = 0x20;   // 32 frames: 16 at old pos, 16 at new.
}

// Port of &1bfd-&1c44 teleport animation (main loop runs it for every
// primary but skips slot 0). Timeline from 0x20: 0x11 dematerialise,
// 0x10 reposition + zero velocity, 0x00 clear flag + +1 energy.
bool Game::advance_player_teleport(Object& player) {
    if (!(player.flags & ObjectFlags::TELEPORTING)) return false;

    if (player.timer == 0) {
        player.flags &= ~ObjectFlags::TELEPORTING;
        if (player.energy < 0xff) player.energy++;
        return false;   // don't consume this frame's motion
    }

    if (player.timer == 0x11) {
        // Brief visual removal — flag so renderer can hide the sprite and
        // so nest-spawned objects know to pause despawn checks (&1bed).
        player_is_completely_dematerialised_ = true;
    }
    if (player.timer == 0x10) {
        player_is_completely_dematerialised_ = false;

        // Centre in destination tile (&1c1e-&1c2d): x_fraction = (-width)/2,
        // y_fraction likewise for the sprite height. For the spacesuit the
        // pattern lands on ~0x80 which is tile centre.
        player.x.whole = player.tx;
        player.y.whole = player.ty;
        int sw = (player.sprite <= 0x80) ? sprite_atlas[player.sprite].w : 1;
        int sh = (player.sprite <= 0x80) ? sprite_atlas[player.sprite].h : 1;
        int wfrac = (sw > 0 ? sw - 1 : 0) * 16;
        int hfrac = (sh > 0 ? sh - 1 : 0) * 8;
        player.x.fraction = static_cast<uint8_t>((~wfrac & 0xff) >> 1);
        player.y.fraction = static_cast<uint8_t>((~hfrac & 0xff) >> 1);
        player.velocity_x = 0;
        player.velocity_y = 0;

        // &1c32-&1c35: play sound for object changing position in teleport.
        // Distinct from kSoundTeleport (the &4410 "teleport activation"
        // chime) — this fires once at the rematerialise frame.
        static constexpr uint8_t kSoundTeleportArrive[4] = { 0x33, 0xf3, 0x63, 0xf3 };
        Audio::play(Audio::CH_ANY, kSoundTeleportArrive);

        // Re-anchor the camera so the view snaps to the teleport target
        // immediately — otherwise the player emerges off-screen and the
        // camera lazily scrolls there.
        camera_.follow_player(player.x.whole, player.y.whole);
    }
    player.timer--;
    return true;
}

// Port of &2c3c handle_remembering_position. Press-to-save: the current
// player centre becomes the next teleport destination. Refuses to save
// when energy < 8 (the 6502 "too damaged" check at &2c3e).
void Game::handle_remembering_position(Object& player) {
    if (player.energy < 8) return;

    // &2c42-&2c49: bump the remembered count, capped at 4.
    if (player_teleports_remembered_ < 4) {
        player_teleports_remembered_++;
    }

    // &2288 get_this_object_centre: add half the sprite width / height in
    // fraction units (width is stored as (pixels-1)*16 fractions, height
    // as (rows-1)*8) to the object's (x_fraction | x_whole << 8). Only
    // the carry into the whole byte matters for the stored tile — the
    // fraction is discarded by player_teleports_x_ being uint8_t. Most of
    // the time the centre's whole byte is simply player.x.whole; it ticks
    // up by 1 only when the half-width addition crosses a tile boundary.
    int sw = (player.sprite <= 0x80) ? sprite_atlas[player.sprite].w : 1;
    int sh = (player.sprite <= 0x80) ? sprite_atlas[player.sprite].h : 1;
    int half_w_frac = ((sw > 0 ? sw - 1 : 0) * 16) / 2;
    int half_h_frac = ((sh > 0 ? sh - 1 : 0) *  8) / 2;
    int cx_16 = static_cast<int>(player.x.whole) * 256 +
                static_cast<int>(player.x.fraction) + half_w_frac;
    int cy_16 = static_cast<int>(player.y.whole) * 256 +
                static_cast<int>(player.y.fraction) + half_h_frac;
    uint8_t centre_x = static_cast<uint8_t>((cx_16 >> 8) & 0xff);
    uint8_t centre_y = static_cast<uint8_t>((cy_16 >> 8) & 0xff);

    uint8_t y = player_next_teleport_;
    player_teleports_x_[y] = centre_x;
    player_teleports_y_[y] = centre_y;
    // &2c5e INC next / &2c61 fix_player_next_teleport AND #&03.
    player_next_teleport_ = (player_next_teleport_ + 1) & 0x03;

    // &2c5b JSR play_middle_beep (&14a0 sound block).
    static constexpr uint8_t kSoundMiddleBeep[4] = { 0x17, 0xe3, 0x2f, 0x72 };
    Audio::play(Audio::CH_ANY, kSoundMiddleBeep);
}
