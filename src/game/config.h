#pragma once
#include "core/types.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Startup configuration loaded from exile.ini. Everything is optional —
// missing fields fall back to the hard-coded defaults (which reproduce
// the original game's spawn state). See exile.ini in the project root
// for the format and writable keys.
//
// This header intentionally has no dependencies on the game's systems:
// Game::init reads these fields and wires them into the player / pocket
// state, so the loader doesn't need to know about ObjectManager etc.
struct StartupConfig {
    // [player]
    uint8_t start_x        = GameConstants::PLAYER_START_X;
    uint8_t start_y        = GameConstants::PLAYER_START_Y;
    uint8_t energy         = 0xff;
    uint8_t weapon         = 2;      // selected weapon slot (0=jetpack,
                                     // 1=pistol, 2=icer, 3=blaster,
                                     // 4=plasma, 5=suit)
    // Force-fill weapon_energy[5] (the suit slot) to 0xffff at init —
    // shorthand for "wear the suit from the start" so [weapon_energy]'s
    // numeric `suit` doesn't have to be touched.
    bool    give_protection_suit = false;

    // Player invincibility — when true, every damage path that targets
    // the player (slot 0) silently no-ops. Useful for level-walking and
    // testing AI without dying. Defaults off so normal play is unchanged.
    bool    invincible = false;

    // [player] bbc_save — optional path to a BBC-format save file
    // (1024 bytes, XOR-streamed). When set, Game::init loads it after
    // the landscape bake and overwrites the [player] / [weapon_energy]
    // / [pockets] / [keys] starting state. Empty = no BBC save load.
    // Extract these via tools/extract_ssd_saves.py.
    std::string bbc_save_path;

    // [weapon_energy] — one 16-bit counter per weapon slot.
    std::array<uint16_t, 6> weapon_energy = { 0x0800, 0, 0x0800, 0, 0, 0 };

    // [pockets] — slot 0 is the top of stack (next to retrieve). Values
    // are ObjectType enum values (0..0x64). 0xff means "empty".
    std::array<uint8_t, 5> pockets = { 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t                pockets_used = 0;   // number of filled slots

    // [whistles] — collected flags for the two whistles (ports of &0816 /
    // &0817 player_whistle_one_collected / player_whistle_two_collected).
    // When true, Y / U trigger whistle_one_active / whistle_two_activator
    // immediately without requiring the player to pick up the primary
    // whistle object. Default false (matches the 6502 ROM state — both
    // whistles start uncollected); exile.ini overrides to true for
    // convenience during development.
    bool whistle_one_collected = false;
    bool whistle_two_collected = false;

    // [distances] activation-ring radii (tiles). See &1bb7 check_demotion +
    // tertiary_spawn.cpp. Internally there are five rings; exile.ini
    // exposes just two collapsed keys (radius_static / radius_moving)
    // that fan out — the four static-related rings track together so
    // the port's wider viewport doesn't trigger spawn/demote churn.
    // Defaults are 6502 ROM (4 static, 12 moving).
    uint8_t demote_tertiary   = 4;
    uint8_t demote_moving     = 12;
    uint8_t demote_settled    = 4;
    uint8_t promote_secondary = 4;
    uint8_t spawn_tertiary    = 4;

    // [caches] — how many primary and secondary slots the world is
    // allowed to use at once. Defaults match the 6502 ROM (16 / 32);
    // exile.ini can raise them up to the compile-time ceiling in
    // GameConstants::PRIMARY_OBJECT_SLOTS / SECONDARY_OBJECT_SLOTS.
    // Setting these lower than their defaults is legal but of course
    // restricts how much stuff can be live simultaneously.
    int primary_slots   = 16;
    int secondary_slots = 32;

    // [keys] &0806 player_keys_collected. 8 entries; 0..5 = the six key
    // object types (CYAN_YELLOW_GREEN..BLUE_CYAN_GREEN), 6..7 are
    // transporter-beam slots (&31bb shift math). 0x80 = collected. Read
    // by RCD door-unlock — keys never occupy pockets.
    std::array<uint8_t, 8> keys_collected = {0, 0, 0, 0, 0, 0, 0, 0};

    // [landscape] — pick which procedural-landscape implementation runs.
    // The pseudo-6502 reference (landscape.cpp) and the native C++
    // rewrite (landscape_cpp.cpp) are intended to produce byte-identical
    // maps; this toggle exists for A/B testing and as a safety net while
    // the rewrite settles. Default false -> reference implementation.
    bool use_cpp_landscape = false;

    // [creatures] — pre-release crab swap. Default true re-types the
    // PIPE tertiary at (198, 190) from BLUE_CYAN_IMP to CRAB so the
    // EXILE1 sprite is reachable in-game. Set false to keep the original
    // imp spawn.
    bool pipe_198_190_crab = true;

    // [creatures] — pre-place Triax at (&99, &3b) in slot 1 so the frame-1
    // intro (absorb + teleport via the adjacent destinator) plays out as
    // it does in the 6502 ROM (&0860 initial object table). Set false to
    // start with no Triax in the world — useful when level-walking the
    // upper world without the intro snatching the player on frame 1.
    bool spawn_initial_triax = true;

    // [creatures] — sucking nest damage-on-touch covers the player. The
    // 6502's &4e29-&4e34 path damages anything in this_object_touching
    // including slot 0; default false here so a sucked-in player isn't
    // chipped to death frame-by-frame while sucking/pushing is tuned.
    bool sucking_nest_damages_player = false;

    // [debug] stress_test — when true, scatter one of every animated
    // creature type in a grid around the player's spawn at startup. The
    // grid covers a ~24×15 tile area NW of the player and stresses the
    // primary slot pool, AI dispatch, and rendering with ~40 active NPCs
    // simultaneously. Defaults to false; intended only for benchmarking
    // and visual regression testing.
    bool stress_test = false;

    // [debug] grenade_chain — when true, drop one ACTIVE_GRENADE and
    // four INACTIVE_GRENADEs on door (80, 95) at startup. tick_test_
    // grenades() flips the inactives to active mid-fuse so the four
    // detonate in a chain ~48 frames after the seed. Default false.
    bool grenade_chain = false;

    // [debug] icer_drop — when true, drop seven ICER_BULLETs falling at
    // vy=0x30 onto (80, 80) at startup. Used during the bullet-vs-tile
    // collision tuning work; left as a reference. Default false.
    bool icer_drop = false;

    // [debug] show_fps — when true, render a measured frames-per-second
    // value in the top-right corner of the window. Sampled over a 30-
    // frame rolling window in Game::run, so the number reflects actual
    // wall-clock cadence (including the per-frame sleep), not just work
    // time. Defaults off; orthogonal to the bottom-HUD Debug checkbox.
    bool show_fps = false;

    // [debug] target_fps — game tick rate. BBC original was 25 Hz; this
    // sets both the logic update and render rate to the same value, so
    // gameplay speed scales linearly: 50 = 2x, 75 = 3x, 100 = 4x. Audio
    // samples-per-tick is realigned automatically. Allowed: 25/50/75/100.
    int target_fps = GameConstants::TARGET_FPS_DEFAULT;

    // [render] subpixel_rendering — three modes:
    //   off      : always snap fractions to the BBC pixel grid (16
    //              frac per X-pixel, 8 frac per Y-row). No judder,
    //              chunky motion. Matches the original BBC display.
    //   on       : never snap; every frac unit advances the screen
    //              position. Smooth motion but surfaces the 6502's
    //              gravity-vs-tile-collision oscillation as 1-2 px
    //              judder on grounded objects.
    //   adaptive : per-object — snap when velocity is small (near-
    //              stationary objects don't judder) and don't snap
    //              when velocity is large (walking / falling renders
    //              smoothly). Default.
    // Stored as the renderer's enum so the parser can fail loudly on
    // unknown spellings.
    enum class SubpixelMode {
        Off       = 0,
        On        = 1,
        Adaptive  = 2,
    };
    SubpixelMode subpixel_mode = SubpixelMode::Adaptive;

    // [audio] — master enable for the synthesised sound. When false,
    // Audio::play / play_at become no-ops; the device still opens so
    // toggling at runtime stays cheap. Defaults to true to match the
    // existing "no [audio] section means full audio" behaviour.
    bool audio_enabled = true;

    // [logs] enabled — master switch for the two diagnostic log files
    // (exile-debug.log and exile-audio.log). When false the files are
    // never opened, so every log call site becomes a no-op (they all
    // gate on the stream being open). Default depends on build: ON in
    // Debug (so devs always have a log when chasing a bug) and OFF in
    // Release (so an end-user run leaves the cwd clean). The ini key
    // overrides either default, so Release users can flip logs on with
    // `[logs] enabled = true` when filing a bug report.
#ifdef NDEBUG
    bool logs_enabled = false;
#else
    bool logs_enabled = true;
#endif

    // [startup_spawns] — list of primary objects to drop into the world
    // during Game::init, after the landscape is baked. Each entry maps a
    // unique key (slot0..slotN, or any other distinct name) to a tuple
    // of <type, tile_x, tile_y[, x_frac, y_frac]>. Used for testing
    // specific creatures / pickups in specific locations without having
    // to author a new tertiary entry.
    struct StartupSpawn {
        uint8_t type;     // ObjectType value (0..0x66)
        uint8_t tile_x;
        uint8_t tile_y;
        uint8_t x_frac;   // 0..255 within tile; default 0x80 (centred)
        uint8_t y_frac;
    };
    std::vector<StartupSpawn> startup_spawns;
};

// Load the config from the given path. Returns the populated struct;
// on missing / unreadable files or parse errors, returns sensible
// defaults and writes a short warning to stderr (callers can ignore —
// this is debug build tooling, not a gameplay fault).
StartupConfig load_startup_config(const std::string& path);
