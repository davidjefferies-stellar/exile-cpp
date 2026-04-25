#include "game/game.h"
#include "game/config.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include "objects/held_object.h"
#include "rendering/debug_names.h"
#include "rendering/sprite_atlas.h"
#include "world/tertiary.h"
#include "world/tile_data.h"
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

    // Whistle collected flags (ports of &0816 / &0817). Default true so
    // Y / U work from the start; set `whistle_one_collected = false` in
    // the ini to replay the pick-up-the-whistle discovery path.
    whistle_one_collected_ = cfg.whistle_one_collected;
    whistle_two_collected_ = cfg.whistle_two_collected;

    // Key-collected bitmask (port of &0806 player_keys_collected). Each
    // entry is 0x80 when the corresponding key has been picked up; the
    // door-unlock path (update_door's &4c9e RCD hit → consider_toggling_
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
    // promote_distance_ governs secondary → primary re-promotion; and
    // spawn_tertiary_distance_ gates render-time tertiary-to-primary
    // spawns. All live in exile.ini's [distances] section.
    object_mgr_.set_demote_distances(cfg.demote_tertiary,
                                      cfg.demote_moving,
                                      cfg.demote_settled);
    object_mgr_.set_promote_distance(cfg.promote_secondary);
    spawn_tertiary_distance_ = cfg.spawn_tertiary;

    // Pick which landscape generator runs. The two implementations are
    // intended to produce byte-identical maps; the toggle exists so the
    // C++ rewrite (landscape_cpp.cpp) can be A/B-tested against the
    // pseudo-6502 reference (landscape.cpp).
    landscape_.set_use_cpp_impl(cfg.use_cpp_landscape);

    // Cache sizes. object_manager's backing arrays are sized at compile
    // time to GameConstants::PRIMARY/SECONDARY_OBJECT_SLOTS; these
    // setters constrain the runtime "active" counts (slot search +
    // shuffle). Read from exile.ini's [caches] section.
    object_mgr_.set_active_primary_slots(cfg.primary_slots);
    object_mgr_.set_active_secondary_slots(cfg.secondary_slots);

    // Seed RNG
    rng_.seed(0x49, 0x52, 0x56, 0x49);

    // Intro sequence: Triax is pre-placed in primary slot 1 at (&99, &3b),
    // two tiles west of the player's spawn and directly adjacent to the
    // destinator tertiary at (&99, &3c). This matches the 6502's ROM
    // initial object table at &0860-&08b4: objects_type[1]=&26 (TRIAX),
    // objects_x=(&99, &64), objects_y=(&3b, &20), objects_sprite=&04
    // (SPACESUIT_VERTICAL). On frame 1 update_triax sees Triax touching
    // the destinator, absorbs it, re-arms the destinator tertiary in
    // Triax's lab (offset &9d → &80), and teleports away — producing the
    // visible "Triax teleports in, steals the destinator, teleports out"
    // beat without any scripted cutscene.
    {
        Object& triax = object_mgr_.object(1);
        object_mgr_.init_object_from_type(triax, ObjectType::TRIAX);
        triax.x = {0x99, 0x64};
        triax.y = {0x3b, 0x20};
    }

    // Truncate + open the lifecycle log. Any previous session's data is
    // discarded — we only ever want the current run's churn record. Each
    // non-paused frame flushes its events here via flush_debug_log().
    debug_log_.open("exile-debug.log",
                    std::ios::out | std::ios::trunc);
    if (debug_log_.is_open()) {
        debug_log_ << "# exile-cpp lifecycle log\n"
                   << "# cols: frame kind p<slot> TYPE @x,y anchor=ax,ay dx=DX dy=DY\n";
        debug_log_.flush();
    }

    // Seed the activation anchor to the player's spawn tile before flushing
    // so EVT_SEC_INIT entries recorded by object_mgr_.init() print with
    // sensible dx/dy relative to the player, not the default (0,0) anchor.
    // The main loop re-sets this every frame from the live player position.
    object_mgr_.set_activation_anchor(player.x.whole, player.y.whole);
    flush_debug_log();

    running_ = true;
    return true;
}

void Game::flush_debug_log() {
    if (!debug_log_.is_open()) return;
    if (object_mgr_.debug_events_n_ == 0) return;

    uint8_t ax = object_mgr_.activation_anchor_x();
    uint8_t ay = object_mgr_.activation_anchor_y();

    for (int i = 0; i < object_mgr_.debug_events_n_; i++) {
        const ObjectManager::DebugEvent& e = object_mgr_.debug_events_[i];
        const char* tag = "???";
        switch (e.kind) {
            case ObjectManager::EVT_CREATE:   tag = "cre"; break;
            case ObjectManager::EVT_PROMOTE:  tag = "prm"; break;
            case ObjectManager::EVT_DEMOTE:   tag = "dem"; break;
            case ObjectManager::EVT_RETURN:   tag = "ret"; break;
            case ObjectManager::EVT_REMOVE:   tag = "rem"; break;
            case ObjectManager::EVT_FLIP:     tag = "flp"; break;
            case ObjectManager::EVT_SEC_INIT: tag = "sec"; break;
        }
        const char* name =
            (e.type < static_cast<uint8_t>(ObjectType::COUNT))
                ? object_type_name(static_cast<ObjectType>(e.type))
                : "UNKNOWN";

        char line[192];
        if (e.kind == ObjectManager::EVT_FLIP) {
            // Flip event: x = velocity_x (reinterpreted signed), y = 0
            // (facing right) or 1 (facing left). Spells out vx so the
            // reason for the flip is obvious at a glance, and the
            // frame/slot/type fields line up with normal events.
            int vx = static_cast<int>(static_cast<int8_t>(e.x));
            const char* facing = (e.y & 1) ? "LEFT" : "RIGHT";
            std::snprintf(line, sizeof(line),
                          "%u %s p%u %s vx=%d -> %s\n",
                          static_cast<unsigned>(frame_counter_),
                          tag, e.slot, name, vx, facing);
        } else {
            // Chebyshev distance to anchor — helps see if events cluster
            // on the demote_settled / promote_secondary / spawn_tertiary
            // rings.
            int dx = static_cast<int>(static_cast<int8_t>(e.x - ax));
            int dy = static_cast<int>(static_cast<int8_t>(e.y - ay));
            std::snprintf(line, sizeof(line),
                          "%u %s p%u %s @%u,%u anchor=%u,%u dx=%d dy=%d\n",
                          static_cast<unsigned>(frame_counter_),
                          tag, e.slot, name, e.x, e.y, ax, ay, dx, dy);
        }
        debug_log_ << line;
    }
    debug_log_.flush();
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

        // Activation-anchor mode is now driven by the "Map mode"
        // checkbox in the bottom HUD strip. Renderer owns the flag so
        // the click-to-toggle in the renderer's mouse handler stays
        // self-contained; we just read it each frame.
        activation_from_camera_ = renderer_->map_mode_enabled();

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

            // &4a1c ROR &29d7 — clear player_object_fired at end of tick
            // so the "fire while holding" pulse only lasts one frame.
            // Any RCD / door / transporter hit-test that needed to see it
            // ran during update_objects / update_events above.
            player_object_fired_ = 0xff;
        }

        render();

        // Flush lifecycle events AFTER render() so CREATE events emitted
        // by spawn_tertiary_object (which fires from the tile-plotting
        // path inside render) are captured in the log. Flushing before
        // render silently dropped those events — the ret/cre pair for a
        // churning tertiary looked one-sided.
        if (!paused_) {
            flush_debug_log();
        }

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

    // Save / load edge detection. Holding ';' would otherwise overwrite the
    // save every frame and thrash the disk; only fire on the 0→1 transition.
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
}

// Port of update_events (&259a-&2742). Runs every frame to animate the
// ambient world: stars, Triax summoning, earthquake progression, and
// clawed-robot respawns. Waterline movement and the Triax-lab maggot
// machinery live in update_water / WaterlineManager (TODO — not yet
// ported). The star-field slice lives at the end of the routine
// (&26c8-&26e6) and is also the first thing the 6502 does after
// picking a random nearby tile.
//
// Each sub-event is gated on its own timer flag so we stay broadly
// faithful without carrying the 6502's zero-page state layout.
void Game::update_events() {
    const Object& player = object_mgr_.player();

    // -----------------------------------------------------------------
    // Random-tile star-field spawn (&26c8-&26e6)
    // -----------------------------------------------------------------
    // The 6502 picks a random tile ±3-4 of the player and, if it's above
    // the surface line and not inside a spaceship, drops a STAR_OR_
    // MUSHROOM particle. STAR TTL range is 8..0x23 frames, so one spawn
    // per frame settles into ~20 stars visible at once.
    {
        uint8_t dx_rand = static_cast<uint8_t>(rng_.next() & 0x07);
        uint8_t dy_rand = static_cast<uint8_t>(rng_.next() & 0x07);
        uint8_t tx = static_cast<uint8_t>(player.x.whole + dx_rand - 3);
        uint8_t ty = static_cast<uint8_t>(player.y.whole + dy_rand - 3);
        // &26ca CMP #&4e / BCS skip: sky ends at world y = 0x4e. The
        // 6502 also suppresses inside spaceships (tile_was_from_map_data)
        // and during player teleport; we don't expose those yet.
        if (ty < 0x4e) {
            particles_.emit_at(ParticleType::STAR_OR_MUSHROOM, tx, ty, rng_);
        }

        // &3fd2 update_mushroom_tile's EVENTS branch (&3fde-&3fe9). The
        // 6502 reaches this via get_random_tile_near_player calling each
        // tile's update routine with TILE_PROCESSING_FLAG_EVENTS set; our
        // port inlines the dispatch right here since the random-tile pick
        // has already been done for the star spawn above.
        //
        // Red (not v-flipped) mushroom balls spawn at y_fraction = 0xff
        // (tile bottom), blue (v-flipped) at 0x00 (tile top). Cap total
        // at 4 balls per type via the &4028 CPY #&04 check, so mushrooms
        // can't carpet the world. Random x_fraction inside the tile
        // matches the &403c rnd_state+1 store.
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

    // -----------------------------------------------------------------
    // NEST / PIPE creature spawn (port of &3e1b update_nest_or_pipe_
    // tile's consider_spawning branch at &3e48). Scan the 9x9 tile
    // window around the player — for each resolved NEST or PIPE tile
    // with creatures remaining and the active bit clear, roll a per-
    // frame chance to spawn one creature of the tertiary's type.
    //
    // Live scan (rather than a pre-computed list): the 6502 doesn't
    // store a y per tertiary entry, so there's no canonical "pipe
    // position" to pre-compute. resolve_tile_with_tertiary walks the
    // landscape and tells us what each nearby tile actually resolves
    // to — including any that the procedural terrain lands on.
    //
    // 81 landscape lookups per frame is trivial; gating spawning on
    // player proximity emulates the 6502's collision-triggered behaviour
    // (consider_spawning only runs when tile_processing_mode has no
    // plot/events flag, i.e. collision processing).
    // -----------------------------------------------------------------
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            uint8_t tx = static_cast<uint8_t>(player.x.whole + dx);
            uint8_t ty = static_cast<uint8_t>(player.y.whole + dy);
            ResolvedTile res = resolve_tile_with_tertiary(landscape_, tx, ty);
            if (res.data_offset <= 0) continue;
            uint8_t ttype = res.tile_and_flip & TileFlip::TYPE_MASK;
            if (ttype != static_cast<uint8_t>(TileType::NEST) &&
                ttype != static_cast<uint8_t>(TileType::PIPE)) continue;
            // One spawn roll per tile per frame: 1-in-256 → ~0.2 spawns/sec
            // per nearby nest/pipe, or roughly one every five seconds.
            if (rng_.next() < 0xff) continue;

            uint8_t data = object_mgr_.tertiary_data_byte(res.data_offset);
            // &3e56 ASL; CMP #&08; BCC leave — creature-count bits sit at
            // positions 2..6; at least one remaining iff (data & 0x7c) != 0.
            bool has_creatures = (data & 0x7c) != 0;
            // &3e5b AND #&06; BNE leave — bits 1..0 are the "inactive" flag.
            bool active = (data & 0x03) == 0;
            if (!has_creatures || !active) continue;
            if (res.type_offset <= 0 ||
                res.type_offset >=
                    static_cast<int>(sizeof(tertiary_objects_type_data))) continue;
            uint8_t ctype = tertiary_objects_type_data[res.type_offset];
            if (ctype >= static_cast<uint8_t>(ObjectType::COUNT)) continue;

            // &3e68 create_primary_object_from_tertiary_if_Y_slots_free —
            // 5 spare slots required (X=5 in events/collision at &3e1d).
            int slot = object_mgr_.create_object(
                static_cast<ObjectType>(ctype), 5,
                tx, 0x80, ty, 0x80);
            if (slot <= 0) continue;

            Object& spawn = object_mgr_.object(slot);
            uint8_t sid = spawn.sprite;
            uint8_t sprite_h_byte = 0;
            uint8_t sprite_w_byte = 0;
            if (sid <= 0x7c) {
                int w = sprite_atlas[sid].w;
                int h = sprite_atlas[sid].h;
                sprite_w_byte = static_cast<uint8_t>((w > 0 ? (w - 1) : 0) * 16);
                sprite_h_byte = static_cast<uint8_t>((h > 0 ? (h - 1) : 0) * 8);
            }

            // &4075-&407e set y_fraction so the spawn sits at the EMPTY
            // edge of the nest tile, away from the solid region:
            //   v-flipped tile  -> y_frac = 0       (spawn at top)
            //   not v-flipped   -> y_frac = ~height (spawn at bottom)
            // create_object left y_frac at 0x80 (tile centre) which
            // drops the bird straight into the tile's solid pattern
            // (for nest-adjacent tiles like leaves / branches) — its
            // velocity never builds past the bounce-reflect threshold
            // so the bird sits at the spawn point forever.
            //
            // Note: &3e72-&3e74 then overwrite flags back to 0x05 after
            // create_primary_object_from_tertiary sets flip bits — birds
            // from nests deliberately do NOT inherit the tile's flip.
            bool v_flipped = (res.tile_and_flip & TileFlip::VERTICAL) != 0;
            spawn.y.fraction = v_flipped
                ? 0x00
                : static_cast<uint8_t>(~sprite_h_byte & 0xff);

            // &3e7d-&3e83 centre the spawn horizontally within the tile.
            // This overrides whatever &4072 set.
            spawn.x.fraction = static_cast<uint8_t>((~sprite_w_byte & 0xff) >> 1);

            // &4081-&4083 store this_object_tertiary_data_offset into the
            // primary so return_to_tertiary can credit the creature back
            // to the nest. Without this the nest drains permanently —
            // birds never come back, and after four spawns the tile is
            // silent forever.
            spawn.tertiary_slot = static_cast<uint8_t>(res.data_offset);

            // &3e6d-&3e6f SBC #&03 with carry clear = subtract four: clears
            // bit 2 of the data byte (the lowest creature-count bit),
            // decrementing remaining creatures by one.
            object_mgr_.set_tertiary_data_byte(
                res.data_offset, static_cast<uint8_t>(data - 4));
        }
    }

    // -----------------------------------------------------------------
    // Earthquake progression (&25e2-&2610)
    // -----------------------------------------------------------------
    // earthquake_state_ is negative while an earthquake is running. Each
    // tick it may increment (worsening) based on the timer AND'd with
    // rnd, with the effect that it worsens more quickly early and tapers
    // toward 0x21 (comment at &25f3: "Then decreasingly frequently").
    //
    // We skip the screen-shudder hardware register writes (&2604-&260a)
    // — they poke the BBC video chip's R2 sync position to visibly
    // wobble the raster. A framebuffer renderer would need an
    // equivalent offset hook; TODO.
    if (earthquake_state_ & 0x80) {
        uint8_t a = static_cast<uint8_t>(earthquake_state_ << 1);
        // CMP rnd: carry set more often as earthquake progresses.
        bool carry = (a >= rng_.next());
        if (every_eight_frames_ && a != 0x21 &&
            ((a & 0x10) || carry)) {
            earthquake_state_++;
        }
    }

    // -----------------------------------------------------------------
    // Triax summoning (&26e6-&2711)
    // -----------------------------------------------------------------
    // Conditions for considering a summon:
    //   * late earthquake OR world flooding OR every-32-frames flag,
    //     then 1-in-256 chance per frame.
    //   * Player in the lower world (y >= 0x94) unless flooding — Triax
    //     doesn't wander up to the surface until endgame.
    //   * Not already present as a primary.
    // On success, OBJECT_TRIAX is spawned at y=0xfe (bottom of world) so
    // it teleports up toward the player on its first update tick.
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
            // &2725-&2728: INC teleport_energy ; BPL leave.
            // Only spawn once the counter overflows past 0x7f (signed
            // positive → signed negative). Previously the conditional
            // was inverted — a freshly-incremented counter (0 → 1) is
            // positive, which is the "still recharging, don't spawn"
            // case in the 6502, but my code ran the spawn branch there
            // and a clawed robot appeared within 8 frames of game start.
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

// Three-phase player update: read input → integrate motion → pick sprite.
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
    if (player.energy == 0) {
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
    apply_player_input(player, inp, accel_x, accel_y);
    integrate_player_motion(player, accel_x, accel_y);
    update_player_sprite(accel_x, accel_y);
}

// Port of &32c8 handle_dropping_object (simplified — we don't need the full
// &dd player_object_held protocol, just release the primary).
void Game::drop_held_object(Object& player) {
    if (held_object_slot_ >= 0x80) return;
    Object& held = object_mgr_.object(held_object_slot_);
    HeldObject::drop(held, player, held_object_slot_);
}

// Port of &4096 consider_teleporting_damaged_player. Runs only when the
// player's energy would hit zero. INCs energy back to 1 so the rest of
// the frame doesn't treat the player as dead, then splits on a 1/2 roll:
//   teleport      drop held + count the death + handle_teleporting.
//   stay-put      drop held.
//
// Deviation from 6502: the original's skip-teleport branch at &40ac-&40b8
// pulls an item out of the pocket stack (1/4 chance if hands empty) and
// then unconditionally drops it on the ground as a near-death penalty.
// We suppress that retrieve step — pocket items stay in the pocket. The
// held-object drop still fires so your hands are empty either way.
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
    player.flags |= ObjectFlags::TELEPORTING;
    player.timer = 0x20;   // 32 frames: 16 at old pos, 16 at new.
}

// Drive the OBJECT_FLAG_TELEPORTING animation for the player. Port of the
// main-loop teleport section at &1bfd-&1c44 (which the object loop runs
// for every primary but skips for slot 0).
//
// Timeline, counting down from 0x20:
//   0x11  this_object_y := 0x11 → briefly remove object; mark player
//         completely dematerialised (&1c10).
//   0x10  position := (tx, ty), fraction centered (&1c1e), velocities
//         zeroed, player no longer dematerialised (&1c1b).
//   0x00  clear TELEPORTING, +1 energy (&1c3e-&1c44).
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
        int sw = (player.sprite <= 0x7c) ? sprite_atlas[player.sprite].w : 1;
        int sh = (player.sprite <= 0x7c) ? sprite_atlas[player.sprite].h : 1;
        int wfrac = (sw > 0 ? sw - 1 : 0) * 16;
        int hfrac = (sh > 0 ? sh - 1 : 0) * 8;
        player.x.fraction = static_cast<uint8_t>((~wfrac & 0xff) >> 1);
        player.y.fraction = static_cast<uint8_t>((~hfrac & 0xff) >> 1);
        player.velocity_x = 0;
        player.velocity_y = 0;

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
    //
    // The earlier port added the half-width in *pixels* directly to the
    // whole byte, which shifted the remembered tile two tiles east on the
    // player's 5-pixel sprite — so teleport landed ~2 tiles off.
    int sw = (player.sprite <= 0x7c) ? sprite_atlas[player.sprite].w : 1;
    int sh = (player.sprite <= 0x7c) ? sprite_atlas[player.sprite].h : 1;
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
}
