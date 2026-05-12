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

    // Audio. Open lazily — if the platform refuses (no device, headless
    // CI, etc.) Audio::open() returns false and every call site below
    // becomes a silent no-op rather than blocking the game from running.
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

    // Load startup config (player position, energy, weapon, pockets,
    // weapon energies). Missing file -> defaults reproducing the original
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
    if (cfg.give_protection_suit) weapon_energy_[5] = 0xffff;
    invincible_   = cfg.invincible;
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

    // Test setup: 2 red slime drops onto door at (80, 96). Each deals 100
    // dmg (&47b1 LDA #&64); same-frame total 200 drops door (255->55) below
    // pair-3 threshold 128 -> SLOW_OR_DESTROYED. Stagger x_frac to avoid
    // mutual AABB overlap before reaching the door.
    {
        constexpr uint8_t kStartTileX = 80;
        constexpr uint8_t kStartTileY = 90;
        for (int i = 0; i < 2; i++) {
            uint8_t x_frac = static_cast<uint8_t>(0x40 + i * 0x60);
            int slot = object_mgr_.create_object(
                ObjectType::RED_DROP, /*min_free_slots=*/0,
                kStartTileX, x_frac, kStartTileY, 0x40);
            if (slot > 0) {
                Object& d = object_mgr_.object(slot);
                d.velocity_x = 0;
                d.velocity_y = 4;  // matches &47f3 LDA #&04 spawn velocity
            }
        }
    }

    // [debug] stress_test — gated test rig that spawns one of every
    // animated creature type in a grid NW of the player. Off by
    // default; enable in exile.ini when benchmarking the AI / render
    // pipeline.
    if (cfg.stress_test) {
        static constexpr ObjectType kCreatures[] = {
            ObjectType::ACTIVE_CHATTER,
            ObjectType::CREW_MEMBER,
            ObjectType::FLUFFY,
            ObjectType::SMALL_HIVE,
            ObjectType::LARGE_HIVE,
            ObjectType::RED_FROGMAN,
            ObjectType::GREEN_FROGMAN,
            ObjectType::INVISIBLE_FROGMAN,
            ObjectType::RED_SLIME,
            ObjectType::GREEN_SLIME,
            ObjectType::YELLOW_SLIME,
            ObjectType::DENSE_NEST,
            ObjectType::SUCKING_NEST,
            ObjectType::BIG_FISH,
            ObjectType::WORM,
            ObjectType::PIRANHA,
            ObjectType::WASP,
            ObjectType::HOVERING_BALL,
            ObjectType::INVISIBLE_HOVERING_BALL,
            ObjectType::MAGENTA_ROLLING_ROBOT,
            ObjectType::RED_ROLLING_ROBOT,
            ObjectType::BLUE_ROLLING_ROBOT,
            ObjectType::HOVERING_ROBOT,
            ObjectType::MAGENTA_CLAWED_ROBOT,
            ObjectType::CYAN_CLAWED_ROBOT,
            ObjectType::GREEN_CLAWED_ROBOT,
            ObjectType::RED_CLAWED_ROBOT,
            ObjectType::MAGGOT,
            ObjectType::GARGOYLE,
            ObjectType::RED_MAGENTA_IMP,
            ObjectType::RED_YELLOW_IMP,
            ObjectType::BLUE_CYAN_IMP,
            ObjectType::CYAN_YELLOW_IMP,
            ObjectType::RED_CYAN_IMP,
            ObjectType::GREEN_YELLOW_BIRD,
            ObjectType::WHITE_YELLOW_BIRD,
            ObjectType::RED_MAGENTA_BIRD,
            ObjectType::INVISIBLE_BIRD,
            ObjectType::DOG,
            ObjectType::CRAB,
        };
        constexpr int kCols      = 8;
        constexpr uint8_t kBaseX = 60;  // 18 tiles left of player (78)
        constexpr uint8_t kBaseY = 85;  // 10 tiles above player (95)
        constexpr uint8_t kStepX = 3;
        constexpr uint8_t kStepY = 3;
        for (size_t i = 0; i < sizeof(kCreatures) / sizeof(kCreatures[0]); i++) {
            uint8_t tx = static_cast<uint8_t>(kBaseX +
                static_cast<int>(i % kCols) * kStepX);
            uint8_t ty = static_cast<uint8_t>(kBaseY +
                static_cast<int>(i / kCols) * kStepY);
            object_mgr_.create_object(
                kCreatures[i], /*min_free_slots=*/0,
                tx, 0x80, ty, 0x80);
        }
    }
#if 0
    // Disabled — grenade chain-reaction test rig.
    {
        constexpr uint8_t kDoorTileX = 80;
        constexpr uint8_t kDoorTileY = 95;
        for (int i = 0; i < 5; i++) {
            uint8_t x_frac = static_cast<uint8_t>(0x00 + i * 0x33);
            ObjectType t = (i == 0) ? ObjectType::ACTIVE_GRENADE
                                    : ObjectType::INACTIVE_GRENADE;
            int slot = object_mgr_.create_object(
                t, /*min_free_slots=*/0,
                kDoorTileX, x_frac, kDoorTileY, 0x40);
            if (slot > 0) {
                Object& g = object_mgr_.object(slot);
                g.velocity_x = 0;
                g.velocity_y = 0;
                g.timer = 0;
                if (i > 0) {
                    g.energy = 0x3f;
                }
                if (i == 0) {
                    test_active_grenade_slot_ = slot;
                } else {
                    test_pending_grenade_slots_[i - 1] = slot;
                }
            }
        }
    }
#endif
#if 0
    // Disabled — kept around as a reference for the bullet-drop test.
    {
        constexpr uint8_t kStartTileX = 80;
        constexpr uint8_t kStartTileY = 80;
        constexpr int8_t  kFallVy     = 0x30;
        for (int i = 0; i < 7; i++) {
            uint8_t x_frac = static_cast<uint8_t>(0x20 + i * 0x20);
            int slot = object_mgr_.create_object(
                ObjectType::ICER_BULLET, /*min_free_slots=*/0,
                kStartTileX, x_frac, kStartTileY, 0x80);
            if (slot > 0) {
                Object& b = object_mgr_.object(slot);
                b.velocity_x = 0;
                b.velocity_y = kFallVy;
                b.timer = 0x30;
            }
        }
    }
#endif

    // Truncate + open the lifecycle log. Any previous session's data is
    // discarded — we only ever want the current run's churn record. Each
    // non-paused frame flushes its events here via flush_debug_log().
    debug_log_.open("exile-debug.log",
                    std::ios::out | std::ios::trunc);
    if (debug_log_.is_open()) {
        // Plumb the same stream into the renderer so its debug helpers
        // (events panel click trace, etc.) land in the same file rather
        // than fighting MSVC's deny-write share on a second handle.
        renderer_->set_debug_log(&debug_log_);
        debug_log_ << "# exile-cpp lifecycle log\n"
                   << "# cols: frame kind p<slot> TYPE @x,y anchor=ax,ay dx=DX dy=DY\n";

        // One-shot diagnostic for the user-reported "missing bush" at
        // (138, 120) — the SPACE_W_TYPE redirect at idx 190 (entry &be)
        // that should resolve to PIPE + bush. Logs the raw landscape
        // tile, tertiary attachment and resolved tile_and_flip / data.
        {
            uint8_t tile = landscape_.get_tile(138, 120);
            uint16_t tidx = landscape_.tertiary_index_at(138, 120);
            debug_log_ << "# (138,120) raw_tile=0x"
                       << std::hex << (int)tile << std::dec
                       << " tertiary_idx=" << tidx;
            if (tidx != Landscape::NO_TERTIARY) {
                const TertiaryEntry& te = landscape_.tertiary_entry(tidx);
                debug_log_ << " tile_and_flip=0x"
                           << std::hex << (int)te.tile_and_flip
                           << " data=0x" << (int)te.data
                           << " type=0x" << (int)te.type << std::dec;
            }
            debug_log_ << "\n";
        }

        // One-shot dump of every cell the grid overlay's yellow shade
        // would mark — i.e. SWITCH cells whose tertiary resolves to a
        // SWITCH-typed tile_and_flip and whose X column holds another
        // such cell. Helps verify whether the gate is producing zero
        // hits because no aliases exist or because it's over-tightened.
        int aliased_count = 0;
        for (int y = 0; y < 256; ++y) {
            for (int x = 0; x < 256; ++x) {
                if (landscape_.switch_x_aliased(
                        static_cast<uint8_t>(x),
                        static_cast<uint8_t>(y))) {
                    debug_log_ << "# switch_x_aliased @"
                               << x << "," << y << "\n";
                    aliased_count++;
                }
            }
        }
        debug_log_ << "# switch_x_aliased total: "
                   << aliased_count << "\n";

        // Looser diagnostic: count raw SWITCH (tile_type 0x08) cells per
        // X column, no tertiary / resolved-type gate. Tells us whether
        // the strict gate is pruning real aliases or there genuinely
        // aren't any raw-SWITCH X collisions in the loaded map.
        int raw_switch_x_count[256] = {};
        for (int y = 0; y < 256; ++y) {
            for (int x = 0; x < 256; ++x) {
                uint8_t tile = landscape_.get_tile(
                    static_cast<uint8_t>(x), static_cast<uint8_t>(y));
                uint8_t type = tile & TileFlip::TYPE_MASK;
                if (type == static_cast<uint8_t>(TileType::SWITCH)) {
                    raw_switch_x_count[x]++;
                }
            }
        }
        int loose_aliased_count = 0;
        for (int x = 0; x < 256; ++x) {
            if (raw_switch_x_count[x] > 1) {
                debug_log_ << "# raw_switch_x x=" << x
                           << " count=" << raw_switch_x_count[x] << "\n";
                loose_aliased_count += raw_switch_x_count[x];
            }
        }
        debug_log_ << "# raw_switch_x_aliased total cells: "
                   << loose_aliased_count << "\n";

        // Tertiary-entry capacity check. If close to TERTIARY_CAPACITY
        // the bake will have run out and silently dropped entries —
        // every cell after the threshold ends up NO_TERTIARY even when
        // its X matched a source row.
        debug_log_ << "# tertiary_entries used: "
                   << landscape_.tertiary_count()
                   << " / capacity " << Landscape::TERTIARY_CAPACITY
                   << "\n";

        // Catalogue of every aliased switch group in the world. Walks
        // each source row whose tile_and_flip resolves to SWITCH (0x08),
        // looks up its column X via x_data, and lists every world cell
        // with the matching raw type on that column. Groups with > 1
        // cell are switches that share state — pressing any one toggles
        // every other in the group via process_switch_effects's sibling
        // fan-out.
        debug_log_ << "# Aliased switch groups (cells sharing a source row):\n";
        int alias_groups = 0;
        for (int T = 0; T <= static_cast<int>(TileType::SWITCH); ++T) {
            int rs = tertiary_ranges[T];
            int re = tertiary_ranges[T + 1];
            for (int i = rs; i < re; ++i) {
                uint8_t tf = tertiary_objects_tile_and_flip_data[i];
                if ((tf & TileFlip::TYPE_MASK) !=
                    static_cast<uint8_t>(TileType::SWITCH)) continue;
                uint8_t source_x = tertiary_objects_x_data[i];
                uint8_t cells_y[256];
                int n_cells = 0;
                for (int y = 0; y < 256; ++y) {
                    uint8_t tile = landscape_.get_tile(
                        source_x, static_cast<uint8_t>(y));
                    if ((tile & TileFlip::TYPE_MASK) !=
                        static_cast<uint8_t>(T)) continue;
                    cells_y[n_cells++] = static_cast<uint8_t>(y);
                }
                if (n_cells <= 1) continue;
                ++alias_groups;
                int data_off = static_cast<int>(static_cast<uint8_t>(
                    i + static_cast<int8_t>(tertiary_data_offset[T])));
                uint8_t data = static_cast<uint8_t>(
                    tertiary_objects_data_bytes[data_off] & 0x7f);
                uint8_t effect_id = static_cast<uint8_t>(data >> 3);
                debug_log_ << "#   raw_type=0x"
                           << std::hex << T << std::dec
                           << " source_idx=" << i
                           << " X=" << (int)source_x
                           << " effect_id=" << (int)effect_id
                           << " cells=" << n_cells << ":\n";
                for (int c = 0; c < n_cells; ++c) {
                    debug_log_ << "#       @" << (int)source_x
                               << "," << (int)cells_y[c] << "\n";
                }
            }
        }
        debug_log_ << "# Aliased switch groups total: "
                   << alias_groups << "\n";

        // Detailed dump for X=227 — the column that holds the user-
        // reported switches at (227,188) and (227,156). Log every
        // CHECK_TERTIARY landscape cell on that column (raw types
        // 0x00..0x08), so we catch redirects: e.g. a raw METAL_DOOR
        // cell whose tertiary tile_and_flip is 0x08 SWITCH — that
        // renders as a switch even though the landscape byte is a
        // door marker.
        for (int y = 0; y < 256; ++y) {
            uint8_t tile = landscape_.get_tile(227,
                                                static_cast<uint8_t>(y));
            uint8_t type = tile & TileFlip::TYPE_MASK;
            if (type > static_cast<uint8_t>(TileType::SWITCH)) continue;
            uint16_t t_idx = landscape_.tertiary_index_at(
                227, static_cast<uint8_t>(y));
            bool from_map = landscape_.tile_from_map_data(
                227, static_cast<uint8_t>(y));
            debug_log_ << "# switch @227," << y
                       << " tile=0x" << std::hex << (int)tile << std::dec
                       << " from_map=" << (from_map ? "yes" : "no")
                       << " tertiary_idx=" << t_idx;
            if (t_idx == Landscape::NO_TERTIARY) {
                debug_log_ << " (no tertiary attached)\n";
                continue;
            }
            // The tertiary's tile_and_flip + data byte. The data offset
            // into the live runtime store is computed the same way as
            // the wiring overlay does.
            const TertiaryEntry& te = landscape_.tertiary_entry(t_idx);
            // The wiring overlay walks the SWITCH range to recover the
            // source-table index; we know it via tertiary_source_idx_
            // but that's private. Use the live data byte from the entry
            // directly — the bake stores it unchanged from the source.
            uint8_t data = te.data & 0x7f;
            uint8_t effect_id = static_cast<uint8_t>(data >> 3);
            uint8_t resolved_type = te.tile_and_flip & TileFlip::TYPE_MASK;
            debug_log_ << " resolved_tile=0x"
                       << std::hex << (int)te.tile_and_flip << std::dec
                       << " (type=0x" << std::hex << (int)resolved_type
                       << std::dec << ") data=0x"
                       << std::hex << (int)data << std::dec
                       << " effect_id=" << (int)effect_id << "\n";
            uint8_t targets[8];
            int n = Behaviors::switch_effect_targets(effect_id, targets, 8);
            for (int t = 0; t < n; t++) {
                // Decode the 6502 data_offset back to (tile_type, X) and
                // list every cell on that column that matches the raw
                // type — i.e. every door / target the press toggles in
                // lockstep under the new fan-out semantics.
                uint8_t b = targets[t];
                debug_log_ << "#   target[" << t
                           << "] data_offset=0x"
                           << std::hex << (int)b << std::dec;
                int matched_type = -1;
                int source_idx_match = -1;
                for (int tt = 0;
                     tt <= static_cast<int>(TileType::SWITCH); ++tt) {
                    uint8_t s = static_cast<uint8_t>(
                        b - tertiary_data_offset[tt]);
                    if (s <  tertiary_ranges[tt]) continue;
                    if (s >= tertiary_ranges[tt + 1]) continue;
                    matched_type = tt;
                    source_idx_match = s;
                    break;
                }
                if (matched_type < 0) {
                    debug_log_ << " (no source row matches)\n";
                    continue;
                }
                uint8_t target_x =
                    tertiary_objects_x_data[source_idx_match];
                debug_log_ << " -> tile_type=0x"
                           << std::hex << matched_type
                           << " source_idx=" << std::dec << source_idx_match
                           << " X=" << (int)target_x << "\n";
                int hits = 0;
                for (int yy = 0; yy < 256; ++yy) {
                    uint8_t raw = landscape_.get_tile(
                        target_x, static_cast<uint8_t>(yy));
                    if ((raw & TileFlip::TYPE_MASK) !=
                        static_cast<uint8_t>(matched_type)) continue;
                    uint16_t cell_idx = landscape_.tertiary_index_at(
                        target_x, static_cast<uint8_t>(yy));
                    debug_log_ << "#       cell @" << (int)target_x
                               << "," << yy
                               << " raw=0x" << std::hex << (int)raw
                               << std::dec
                               << " entry_idx=" << cell_idx << "\n";
                    hits++;
                }
                if (hits == 0) {
                    debug_log_ << "#       (no cells of that type at X)\n";
                }
            }
        }
        debug_log_.flush();
    }

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
    return true;
}

void Game::flush_debug_log() {
    if (!debug_log_.is_open()) return;
    if (object_mgr_.debug_events_n_ == 0 &&
        object_mgr_.diag_lines_.empty()) return;

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
    for (const std::string& s : object_mgr_.diag_lines_) {
        debug_log_ << static_cast<unsigned>(frame_counter_) << " diag "
                   << s;
        if (s.empty() || s.back() != '\n') debug_log_ << '\n';
    }
    debug_log_.flush();
}

void Game::run() {
    using clock = std::chrono::steady_clock;
    // Locked logic+render rate from [debug] target_fps. Logic and render
    // tick together so motion is judder-free; higher rates fast-forward
    // the game (audio aligned via Audio::set_logic_rate in init).
    auto frame_duration = std::chrono::microseconds(1'000'000 / target_fps_);

    // 30-frame rolling FPS window — fed by actual frame_start deltas so
    // the per-frame sleep counts. Game::render reads fps_value_.
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

        if (!paused_) {
            object_mgr_.reset_debug_counters();
            // [player] invincible — reset HP to full at the top of every
            // tick so any damage applied during this frame is undone
            // before the next visual update. Cheap, doesn't need to
            // touch any per-source damage path.
            if (invincible_) object_mgr_.player().energy = 0xff;
            update_timers();
            // &19c9 update_background_flash — tick the sky-flash cooldown
            // before any updates so renderer sees the latest clear colour.
            update_background_flash();
            // Damage debug overlay: tick TTLs and drop expired entries
            // at the start of every frame so numbers linger long enough
            // to read (~30 frames / 0.6s) before being recycled.
            for (auto& ev : damage_events_) {
                if (ev.ttl > 0) ev.ttl--;
            }
            damage_events_.erase(
                std::remove_if(damage_events_.begin(), damage_events_.end(),
                               [](const DamageVisual& e){ return e.ttl == 0; }),
                damage_events_.end());

            // Always-on floating labels (e.g. "FED!"): same TTL pattern
            // as damage_events_ but rendered unconditionally.
            for (auto& f : floating_labels_) {
                if (f.ttl > 0) f.ttl--;
            }
            floating_labels_.erase(
                std::remove_if(floating_labels_.begin(), floating_labels_.end(),
                               [](const FloatingLabel& f){ return f.ttl == 0; }),
                floating_labels_.end());
            // Test rig: when the original active grenade's fuse hits
            // halfway (timer >= 0x30), convert the 3 pending inactive
            // slots to ACTIVE_GRENADE with their fuses fresh
            // (timer = 0). The 3 newly-activated grenades take a full
            // 96 frames to reach 0x60, so they detonate ~48 frames
            // after the original — a clean test that multi-frame
            // damage cannot destroy the door while the refill is on.
            if (!test_grenades_activated_ && test_active_grenade_slot_ > 0) {
                Object& src = object_mgr_.object(test_active_grenade_slot_);
                if (src.is_active() &&
                    src.type == ObjectType::ACTIVE_GRENADE &&
                    src.timer >= 0x30) {
                    for (int i = 0; i < 4; i++) {
                        int s = test_pending_grenade_slots_[i];
                        if (s <= 0) continue;
                        Object& g = object_mgr_.object(s);
                        if (!g.is_active()) continue;
                        if (g.type != ObjectType::INACTIVE_GRENADE) continue;
                        g.type = ObjectType::ACTIVE_GRENADE;
                        g.sprite = object_types_sprite[
                            static_cast<uint8_t>(ObjectType::ACTIVE_GRENADE)];
                        g.palette = object_types_palette_and_pickup[
                            static_cast<uint8_t>(ObjectType::ACTIVE_GRENADE)] & 0x7f;
                        g.timer = 0;
                    }
                    test_grenades_activated_ = true;
                }
            }

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

// Port of &19b6-&19c7: LSR chain sets timer negative when low bits are all zero.
// Timer fires (is "negative"/true) when frame_counter is on that boundary.
// every_two_frames fires on even frames (bit 0 = 0),
// every_four_frames fires when bits 0-1 are 0, etc.
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

    // Window-close path: get_key returns InputKey::CLOSE_REQUESTED when
    // the user clicked the title-bar X. No key binding to this — only
    // the renderer's should_close flag drives it.
    if (input_.state().quit) {
        running_ = false;
    }

    // Save / load edge detection. Holding ';' would otherwise overwrite the
    // save every frame and thrash the disk; only fire on the 0->1 transition.
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

    // Test-events panel: poll the renderer for clicks each tick.
    int event_id;
    if (renderer_->consume_event_click(event_id)) {
        trigger_event(event_id);
    }
}

// Port-only test triggers. Wired into the right-side Events panel —
// each branch corresponds to one button in kEventButtons. Effects are
// deliberately over-driven beyond the 6502's per-frame slope so the
// dev sees a clear visual response within a couple of frames.
void Game::trigger_event(int event_id) {
    Object& player = object_mgr_.player();
    // Spawn 4 tiles above the player — visible immediately. The 6502's
    // y=0xfe / "teleport-to-player on first tick" recipe relies on the
    // per-type teleport step which isn't fully ported for these types.
    uint8_t spawn_x = player.x.whole;
    uint8_t spawn_y = (player.y.whole > 4) ? (player.y.whole - 4) : 0;
    switch (static_cast<EventId>(event_id)) {
        case EventId::SPAWN_TRIAX: {
            int slot = object_mgr_.create_object(
                ObjectType::TRIAX, /*min_free_slots=*/4,
                spawn_x, 0x80, spawn_y, 0x80);
            if (slot > 0) object_mgr_.object(slot).target_and_flags = 0xc0;
            break;
        }
        case EventId::SPAWN_MAGGOT:
            // Spawn near the player so it's immediately visible. The
            // 6502 spawns at the Triax-lab maggot machine (0x61, 0xd9);
            // for a test trigger we want the maggot to be where the dev
            // is looking, not 80 tiles away.
            object_mgr_.create_object(ObjectType::MAGGOT, 4,
                                       spawn_x, 0x80, spawn_y, 0x80);
            break;
        case EventId::SPAWN_CLAWED_ROBOT: {
            int slot = object_mgr_.create_object(
                ObjectType::MAGENTA_CLAWED_ROBOT, /*min_free_slots=*/4,
                spawn_x, 0x80, spawn_y, 0x80);
            if (slot > 0) object_mgr_.object(slot).target_and_flags = 0xc0;
            break;
        }
        case EventId::TOGGLE_FLOOD: {
            // Flip the flooding bit and arm the test fast-flood
            // counter so the waterline moves at 1 tile/frame for the
            // next 0x78 tiles. The 6502's slow integrator runs in
            // parallel — once test progression finishes, the integrator
            // takes over (and is a no-op since we're at desired_y).
            flooding_state_ ^= 0x80;
            test_flood_steps_remaining_ = 0x78;
            test_flood_direction_ = (flooding_state_ & 0x80) ? -1 : +1;
            break;
        }
        case EventId::TOGGLE_EARTHQUAKE:
            // Bit 7 flip drives the existing state-advance code. Also
            // arm the port-only camera-shake counter for a visible
            // 60-frame jitter (CRTC R2 writes the 6502 used aren't
            // available on a framebuffer renderer).
            earthquake_state_ ^= 0x80;
            test_shake_frames_ = (earthquake_state_ & 0x80) ? 60 : 0;
            break;
        case EventId::DAMAGE_PLAYER:
            player.energy = (player.energy > 10) ? (player.energy - 10) : 0;
            break;
        case EventId::HEAL_PLAYER:
            player.energy = (player.energy < 0xff - 0x40)
                            ? (player.energy + 0x40) : 0xff;
            break;
    }
}

// Port of &25a1-&25df update_triax_lab. Maggot count refill, periodic
// maggot spawn, door drive from the lab waterline, desired_y rewrite.
// Also drains the port-only test fast-flood counter (1 tile/frame).
void Game::update_triax_lab() {
    if (test_flood_steps_remaining_ > 0) {
        // 1 tile / 50 frames at 50fps == 1 tile / second.
        if (++test_flood_subframe_ >= 50) {
            test_flood_subframe_ = 0;
            uint8_t y = Water::get_y(1);
            int ny = int(y) + int(test_flood_direction_);
            if (ny < 0)    ny = 0;
            if (ny > 0xff) ny = 0xff;
            Water::set_y(1, static_cast<uint8_t>(ny), 0);
            test_flood_steps_remaining_--;
        }
    }

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

    // Random-tile star-field spawn (&26c8-&26e6). Port-only: sample across
    // the actual viewport extent (not BBC's ±4 box) and scale spawn count
    // so star density matches the original on our wider viewport.
    int vp_w = renderer_->viewport_width_tiles();
    int vp_h = renderer_->viewport_height_tiles();
    if (vp_w < 8) vp_w = 8;
    if (vp_h < 8) vp_h = 8;
    int half_w = vp_w / 2;
    int half_h = vp_h / 2;
    // Scale spawn count to viewport area vs the 6502's ~7×7 = 49 tiles.
    // Cap at 16/frame so a maximised window doesn't burst the pool.
    int spawn_count = (vp_w * vp_h) / 64;
    if (spawn_count < 1)  spawn_count = 1;
    if (spawn_count > 16) spawn_count = 16;
    // Player teleport suppression — &26da reads `player_is_completely_
    // dematerialised` at &19b5, which the teleport state machine sets at
    // mid-animation while the player is briefly removed from the world.
    // Our analogue is the TELEPORTING object flag on the player; the
    // generic teleport handler arms it for the full 32-frame animation
    // (close enough to the 6502's "completely dematerialised" window for
    // the purpose of suppressing ambient stars).
    bool player_teleporting =
        (player.flags & ObjectFlags::TELEPORTING) != 0;
    for (int i = 0; i < spawn_count; i++) {
        int dx = (rng_.next() % vp_w) - half_w;
        int dy = (rng_.next() % vp_h) - half_h;
        uint8_t tx = static_cast<uint8_t>(camera_.center_x + dx);
        uint8_t ty = static_cast<uint8_t>(camera_.center_y + dy);
        // &26ca CMP #&4e / BCS skip: sky ends at world y = 0x4e.
        if (ty >= 0x4e) continue;
        // &26da-&26df: ORA player_is_completely_dematerialised /
        // ORA tile_was_from_map_data / BMI skip. Suppresses stars while
        // the player is teleporting  and inside the player's spaceship and Triax's lab
        if (player_teleporting) continue;
        if (landscape_.tile_from_map_data(tx, ty)) continue;
        particles_.emit_at(ParticleType::STAR_OR_MUSHROOM, tx, ty, rng_);
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
            // One spawn roll per tile per frame: 1-in-256 -> ~0.2 spawns/sec
            // per nearby nest/pipe, or roughly one every five seconds.
            if (rng_.next() < 0xff) continue;

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
            uint8_t sid = spawn.sprite;
            uint8_t sprite_h_byte = 0;
            uint8_t sprite_w_byte = 0;
            if (sid <= 0x80) {
                int w = sprite_atlas[sid].w;
                int h = sprite_atlas[sid].h;
                sprite_w_byte = static_cast<uint8_t>((w > 0 ? (w - 1) : 0) * 16);
                sprite_h_byte = static_cast<uint8_t>((h > 0 ? (h - 1) : 0) * 8);
            }

            // Place the spawn so its sprite CENTRE sits on the tile's
            // boundary opposite the pipe / nest opening — i.e. half of
            // the sprite is in the pipe tile, half emerges into the next
            // tile down (or up if v-flipped). The 6502 default at
            // &4075-&407e is bottom-flush (sprite bottom at tile bottom);
            // the visual we want is the creature partially poking out of
            // the opening, so we shift by an extra h/2.
            bool v_flipped = (res.tile_and_flip & TileFlip::VERTICAL) != 0;
            uint8_t half_h = static_cast<uint8_t>(sprite_h_byte >> 1);
            spawn.y.fraction = v_flipped
                ? static_cast<uint8_t>(0u - half_h)            // centred on tile TOP edge
                : static_cast<uint8_t>(0xff - half_h);          // centred on tile BOTTOM edge

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
    // sync register writes (&2604-&260a) for raster shudder — TODO.
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

    // Per-frame walking-diagnosis log. Only emits when motion-relevant input
    // is held or the player is still moving — silent when stationary so the
    // log doesn't flood. Goes to the same stream as flush_debug_log so the
    // lifecycle events and the input frames interleave by frame number.
    bool motion_input = inp.move_left || inp.move_right ||
                        inp.move_up   || inp.move_down  ||
                        inp.jetpack   || inp.boost;
    // State-change only: emit when SUPPORTED, walking, or the
    // motion_input mask flips. Suppresses per-frame spam when nothing
    // interesting changes — pair with walk-state / walk-blocked /
    // plr-tcr to get a focused timeline.
    bool inp_supported_now = (player.flags & ObjectFlags::SUPPORTED) != 0;
    bool inp_walking_now = inp_supported_now &&
        !(inp.jetpack || inp.move_up || inp.move_down ||
          (inp.boost && (inp.move_left || inp.move_right)));
    bool inp_state_changed =
        inp_supported_now != inp_log_supported_prev_ ||
        inp_walking_now   != inp_log_walking_prev_   ||
        motion_input      != inp_log_motion_prev_;
    inp_log_supported_prev_ = inp_supported_now;
    inp_log_walking_prev_   = inp_walking_now;
    inp_log_motion_prev_    = motion_input;
    if (debug_log_.is_open() && inp_state_changed &&
        (motion_input || pre_vx != 0 || pre_vy != 0 ||
         player.velocity_x != 0 || player.velocity_y != 0)) {
        bool supported = (player.flags & ObjectFlags::SUPPORTED) != 0;
        bool newly    = (player.flags & ObjectFlags::NEWLY_CREATED) != 0;
        bool fliph    = (player.flags & ObjectFlags::FLIP_HORIZONTAL) != 0;
        bool walking  = supported && !(inp.jetpack || inp.move_up || inp.move_down ||
                                       (inp.boost && (inp.move_left || inp.move_right)));

        // Re-resolve the tile under the player's feet so the log shows the
        // exact inputs the grounded-check inside integrate_player_motion was
        // looking at. Lets us see WHY sup=0 when the player is visually
        // standing on a floor.
        int sprite_h = (player.sprite <= 0x80)
                       ? sprite_atlas[player.sprite].h : 22;
        int sprite_h_frac = (sprite_h > 0 ? sprite_h - 1 : 0) * 8;
        int feet_abs_dbg = static_cast<int>(player.y.whole) * 256 +
                           static_cast<int>(player.y.fraction) + sprite_h_frac;
        uint8_t feet_ty  = static_cast<uint8_t>((feet_abs_dbg >> 8) & 0xff);
        uint8_t feet_yf  = static_cast<uint8_t>(feet_abs_dbg & 0xff);
        ResolvedTile fres_dbg = resolve_tile_with_tertiary(
            landscape_, player.x.whole, feet_ty);
        uint8_t raw_tile = Collision::substitute_door_for_obstruction(
            fres_dbg.tile_and_flip, fres_dbg.data_offset,
            reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                object_mgr_.object(0)),
            object_mgr_.tertiary_data_byte(fres_dbg.data_offset));
        uint8_t ftype = raw_tile & TileFlip::TYPE_MASK;
        bool ffh = (raw_tile & TileFlip::HORIZONTAL) != 0;
        bool ffv = (raw_tile & TileFlip::VERTICAL)   != 0;
        bool ftype_solid = Collision::is_tile_type_solid(ftype);
        bool fcoll_fv = ffv ^ tile_obstruction_v_flip_bit(ftype);
        uint8_t fthresh = tile_threshold_at_x(ftype, ffh, ffv, player.x.fraction);
        bool feet_in_obstr = fcoll_fv ? (feet_yf <= fthresh)
                                      : (feet_yf >= fthresh);

        char line[320];
        std::snprintf(line, sizeof(line),
                      "%u inp keys=%c%c%c%c%c%c facing=%s sup=%d new=%d fh=%d "
                      "walk=%d pre_vel=%d,%d pre_frac=%u,%u accel=%d,%d "
                      "post_vel=%d,%d post_frac=%u,%u pos=%u,%u "
                      "feet=ty%u,yf%u tile=%02x type=%02x sol=%d cfv=%d "
                      "thr=%u in_obs=%d tcA=%02x\n",
                      static_cast<unsigned>(frame_counter_),
                      inp.move_left  ? 'L' : '-',
                      inp.move_right ? 'R' : '-',
                      inp.move_up    ? 'U' : '-',
                      inp.move_down  ? 'D' : '-',
                      inp.jetpack    ? 'J' : '-',
                      inp.boost      ? 'B' : '-',
                      (player_facing_ & 0x80) ? "L" : "R",
                      supported ? 1 : 0, newly ? 1 : 0, fliph ? 1 : 0,
                      walking ? 1 : 0,
                      pre_vx, pre_vy, pre_xf, pre_yf,
                      accel_x, accel_y,
                      player.velocity_x, player.velocity_y,
                      player.x.fraction, player.y.fraction,
                      player.x.whole, player.y.whole,
                      feet_ty, feet_yf, raw_tile, ftype,
                      ftype_solid ? 1 : 0, fcoll_fv ? 1 : 0,
                      fthresh, feet_in_obstr ? 1 : 0,
                      player_tile_collision_angle_);
        debug_log_ << line;
    }
}

// Port of &32c8 handle_dropping_object (simplified — we don't need the full
// &dd player_object_held protocol, just release the primary).
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
    // Port-only: the 6502's blaster path doesn't call flash_background,
    // but without it the discharge is invisible at the screen-flash
    // level. Coronium chains and the "no slot" path (the original
    // callers of &1f92) still trigger it via their own routines.
    flash_background();

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
