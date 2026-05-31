#include "game/game.h"
#include "behaviours/environment.h"
#include "objects/collision.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include "rendering/debug_names.h"
#include "rendering/sprite_atlas.h"
#include "world/tertiary.h"
#include "world/tile_data.h"
#include "world/water.h"
#include <algorithm>
#include <cstdio>

// game_debug.cpp — implementations of the debug-log helpers and the
// port-only test rigs (event-panel triggers, smooth-flood subframe drain,
// chained-grenade demo). Pulled out of game.cpp so the main per-frame
// chain stays readable. All entry points are members of Game; nothing
// here is reachable from a release-only path.

// ---------- Frame profiler report ---------------------------------------
//
// Called from tick() once the [debug] profile window fills (~1s). Writes
// the mean ms/frame per phase to exile-debug.log, then resets the window.
// Resets even when the log is closed so the accumulators don't grow
// unbounded. Render sub-phases are listed under render; "other" is the
// render time not covered by begin/tiles/objects/end (overlays, HUD-text).
void Game::emit_profile_report() {
    using S = Profile::Section;
    int frames = profiler_.frames();
    if (debug_log_.is_open() && frames > 0) {
        int vp_w = renderer_->viewport_width_tiles();
        int vp_h = renderer_->viewport_height_tiles();
        char hdr[96];
        std::snprintf(hdr, sizeof(hdr),
                      "[profile] %d frames @ %dx%d tiles, avg ms/frame:\n",
                      frames, vp_w, vp_h);
        debug_log_ << hdr;

        const S order[] = { S::Frame, S::Input, S::Player, S::Objects,
                            S::Events, S::Particles, S::Render,
                            S::RenderBegin, S::RenderTiles,
                            S::RenderWater, S::RenderTileResolve,
                            S::RenderTileInfo, S::RenderTileBlit,
                            S::RenderObjects, S::RenderEnd };
        for (S s : order) {
            char line[64];
            std::snprintf(line, sizeof(line), "  %-10s %7.3f\n",
                          Profile::name(s), profiler_.avg_ms(s));
            debug_log_ << line;
        }
        // Render time not attributed to a sub-phase (overlays + HUD text).
        double other = profiler_.avg_ms(S::Render)
                     - profiler_.avg_ms(S::RenderBegin)
                     - profiler_.avg_ms(S::RenderTiles)
                     - profiler_.avg_ms(S::RenderObjects)
                     - profiler_.avg_ms(S::RenderEnd);
        char line[64];
        std::snprintf(line, sizeof(line), "  %-10s %7.3f\n", "  other", other);
        debug_log_ << line;
        debug_log_.flush();
    }
    profiler_.reset();
}

// ---------- One-shot startup diagnostics --------------------------------
//
// Writes a census of the freshly-baked landscape into exile-debug.log so
// post-mortem grep can answer "is the switch at (227,156) actually wired
// up?" without re-running the game under a debugger. Bails out silently
// if [logs] is disabled (debug_log_ never opened).
void Game::dump_init_diagnostics() {
    if (!debug_log_.is_open()) return;

    // Plumb the same stream into the renderer so its debug helpers
    // (events panel click trace, etc.) land in the same file rather
    // than fighting MSVC's deny-write share on a second handle.
    renderer_->set_debug_log(&debug_log_);
    debug_log_ << "# exile-cpp lifecycle log\n"
               << "# cols: frame kind p<slot> TYPE @x,y anchor=ax,ay dx=DX dy=DY\n";

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
            // Decode data_offset back to (source_idx, X) and list
            // every column cell whose tile type matches the source
            // entry's own tile_and_flip. `range_idx` is just the
            // offset-table partition, not the world tile type.
            uint8_t b = targets[t];
            debug_log_ << "#   target[" << t
                       << "] data_offset=0x"
                       << std::hex << (int)b << std::dec;
            int matched_range = -1;
            int source_idx_match = -1;
            for (int range_idx = 0; range_idx <= 8; ++range_idx) {
                uint8_t s = static_cast<uint8_t>(
                    b - tertiary_data_offset[range_idx]);
                if (s <  tertiary_ranges[range_idx]) continue;
                if (s >= tertiary_ranges[range_idx + 1]) continue;
                matched_range = range_idx;
                source_idx_match = s;
                break;
            }
            if (matched_range < 0) {
                debug_log_ << " (no source row matches)\n";
                continue;
            }
            uint8_t target_x =
                tertiary_objects_x_data[source_idx_match];
            debug_log_ << " -> source_idx=" << std::dec << source_idx_match
                       << " X=" << (int)target_x << "\n";
            int hits = 0;
            for (int yy = 0; yy < 256; ++yy) {
                if (landscape_.tertiary_source_idx_at(
                        target_x, static_cast<uint8_t>(yy)) !=
                    source_idx_match) continue;
                uint16_t cell_idx = landscape_.tertiary_index_at(
                    target_x, static_cast<uint8_t>(yy));
                debug_log_ << "#       cell @" << (int)target_x
                           << "," << yy
                           << " entry_idx=" << cell_idx << "\n";
                hits++;
            }
            if (hits == 0) {
                debug_log_ << "#       (no cells share that source)\n";
            }
        }
    }
    debug_log_.flush();
}

// ---------- Per-frame lifecycle flush -----------------------------------
//
// Drains the ObjectManager's per-frame ring of cre/prm/dem/ret/rem/flp
// events (plus any diag_lines_) into exile-debug.log, then clears the
// buffer. Called once per non-paused tick from Game::tick after render().
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

// ---------- Event-panel triggers (port-only) ----------------------------
//
// Wired into the right-side Events panel — each branch corresponds to
// one button in kEventButtons. Effects are deliberately over-driven
// beyond the 6502's per-frame slope so the dev sees a clear visual
// response within a couple of frames.
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

// ---------- Chained-grenade test rig ------------------------------------
//
// Halfway through the seed grenade's fuse (timer >= 0x30), convert the
// three pending inactive slots to ACTIVE_GRENADE with their fuses fresh
// so the four detonate in a chain ~48 frames after the seed. The seed +
// slot table are populated by a #if 0-gated rig in init(); when the rig
// is disabled `test_active_grenade_slot_` stays -1 and this is a no-op.
void Game::tick_test_grenades() {
    if (test_grenades_activated_ || test_active_grenade_slot_ <= 0) return;
    Object& src = object_mgr_.object(test_active_grenade_slot_);
    if (!src.is_active() ||
        src.type != ObjectType::ACTIVE_GRENADE ||
        src.timer < 0x30) return;

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

// ---------- Overlay-visual TTL tick -------------------------------------
//
// damage_events_ and floating_labels_ are both transient visual rings
// (damage numbers floating up from hit objects, "FED!" / "KEY!" pop-ups
// on rare events). Their TTLs decay every frame and expired entries get
// erased; the renderer reads the live vectors. No gameplay effect, so
// the maintenance pass lives here instead of crowding tick().
static bool damage_visual_expired(const DamageVisual& e) { return e.ttl == 0; }
static bool floating_label_expired(const FloatingLabel& f) { return f.ttl == 0; }

void Game::tick_overlay_visuals() {
    for (auto& ev : damage_events_) {
        if (ev.ttl > 0) ev.ttl--;
    }
    damage_events_.erase(
        std::remove_if(damage_events_.begin(), damage_events_.end(),
                       damage_visual_expired),
        damage_events_.end());

    for (auto& f : floating_labels_) {
        if (f.ttl > 0) f.ttl--;
    }
    floating_labels_.erase(
        std::remove_if(floating_labels_.begin(), floating_labels_.end(),
                       floating_label_expired),
        floating_labels_.end());
}

// ---------- Smooth-flood subframe drain ---------------------------------
//
// Runs alongside the 6502's slow integrator so the visible waterline
// moves at 1 tile / second under the test trigger. direction is -1 to
// flood (y decreasing = water rising on screen), +1 to drain.
void Game::tick_test_flood() {
    if (test_flood_steps_remaining_ <= 0) return;
    if (++test_flood_subframe_ < 50) return;
    test_flood_subframe_ = 0;
    uint8_t y = Water::get_y(1);
    int ny = int(y) + int(test_flood_direction_);
    if (ny < 0)    ny = 0;
    if (ny > 0xff) ny = 0xff;
    Water::set_y(1, static_cast<uint8_t>(ny), 0);
    test_flood_steps_remaining_--;
}

// ---------- Startup test spawns ----------------------------------------
//
// Drops a pair of red slimes onto the door at (80, 96) so a fresh game
// has a visible damage event in view, and optionally scatters one of
// every animated NPC type in a grid NW of the player when [debug]
// stress_test is on. Both rigs are port-only debug aids — the 6502 ROM
// has no equivalent — but they're useful enough during AI / damage
// regression work to keep around behind the existing config gate.
void Game::spawn_test_rigs(bool stress_test, bool grenade_chain, bool icer_drop) {
    // Two red slime drops onto door at (80, 96). Each deals 100 dmg
    // (&47b1 LDA #&64); same-frame total 200 drops door (255->55) below
    // pair-3 threshold 128 -> SLOW_OR_DESTROYED. Stagger x_frac so the
    // pair doesn't overlap before reaching the door.
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

    // [debug] grenade_chain — one ACTIVE_GRENADE + four INACTIVE_GRENADEs
    // on door (80, 95). tick_test_grenades() flips the inactives to
    // ACTIVE mid-fuse so the four detonate in a chain ~48 frames after
    // the seed. Records the spawned slots in test_*_grenade_slots_.
    if (grenade_chain) {
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
                if (i > 0) g.energy = 0x3f;
                if (i == 0) {
                    test_active_grenade_slot_ = slot;
                } else {
                    test_pending_grenade_slots_[i - 1] = slot;
                }
            }
        }
    }

    // [debug] icer_drop — seven ICER_BULLETs falling at vy=0x30 onto
    // (80, 80). Reference rig for bullet-vs-tile collision tuning.
    if (icer_drop) {
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

    // [debug] stress_test — gated rig that spawns one of every animated
    // creature type in an 8-wide grid NW of the player. Off by default;
    // enable in exile.ini when benchmarking the AI / render pipeline.
    if (!stress_test) return;

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

// ---------- Rewind ring scrubbing ---------------------------------------

// Resolve scrub_offset_ frames back from the most recently captured entry
// (head_-1 mod size). Shared between the back / forward / commit paths so
// the index arithmetic lives in one place.
size_t Game::scrubbed_ring_index() const {
    size_t cap     = snapshot_ring_.size();
    size_t live    = (snapshot_ring_head_ + cap - 1) % cap;
    return (live + cap - scrub_offset_) % cap;
}

// Esc while scrubbing snaps the ring to the currently-restored frame
// (subsequent ticks overwrite the now-stale future entries) and resumes
// the sim from there, branching the timeline. Returns true when it has
// taken the press; the caller falls back to a pause toggle otherwise.
bool Game::commit_scrub_if_active() {
    if (!scrubbing_) return false;
    size_t cap = snapshot_ring_.size();
    snapshot_ring_head_   = (scrubbed_ring_index() + 1) % cap;
    snapshot_ring_count_ -= scrub_offset_;
    scrub_offset_         = 0;
    scrubbing_            = false;
    return true;
}

// Numpad '-' steps back through the ring, numpad '*' steps forward. Both
// auto-repeat (one frame per tick) so the user can sweep continuously.
// Entering scrub mode freezes the sim until commit_scrub_if_active runs.
void Game::tick_scrub_keys() {
    bool back = input_.state().scrub_back;
    bool fwd  = input_.state().scrub_forward;
    if (back && scrub_offset_ + 1 < snapshot_ring_count_) {
        scrubbing_ = true;
        scrub_offset_++;
        restore_snapshot(snapshot_ring_[scrubbed_ring_index()]);
    }
    if (fwd && scrubbing_ && scrub_offset_ > 0) {
        scrub_offset_--;
        restore_snapshot(snapshot_ring_[scrubbed_ring_index()]);
        if (scrub_offset_ == 0) scrubbing_ = false;
    }
}

// Capture post-tick state into the ring. When the buffer is full, head_
// wraps and the oldest entry is overwritten in place.
void Game::capture_rewind_snapshot() {
    snapshot_ring_[snapshot_ring_head_] = snapshot();
    snapshot_ring_head_ =
        (snapshot_ring_head_ + 1) % snapshot_ring_.size();
    if (snapshot_ring_count_ < snapshot_ring_.size()) {
        snapshot_ring_count_++;
    }
}
