#pragma once
#include "core/types.h"
#include <array>
#include <cstdint>
#include <string>

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

    // [distances] — activation-ring radii (in tiles) for the primary /
    // secondary / tertiary caches. Each field maps to a specific branch of
    // the 6502 lifecycle machinery; see object_manager.cpp:check_demotion
    // (&1bb7) and tertiary_spawn.cpp for details. The defaults below are
    // our port's working values — the 6502 ROM values are noted in the
    // comments but not all are faithful (KEEP_AS_TERTIARY bumped 1→12 to
    // cope with our wider viewport).
    //
    //   demote_tertiary  : primary → tertiary for statics (doors, switches).
    //                      6502: 1. Port: 12 to avoid spawn/demote churn.
    //   demote_moving    : primary → (secondary | tertiary) for moving /
    //                      airborne KEEP_AS_PRIMARY_FOR_LONGER objects.
    //                      6502: 12. Port: 12.
    //   demote_settled   : same flag but the object is slow AND supported.
    //                      6502: 4. Port: 4.
    //   promote_secondary: secondary → primary. 6502: 4. Port: 4.
    //   spawn_tertiary   : tile tertiary → primary (render-time spawn
    //                      gate in spawn_tertiary_object). Port: 4,
    //                      matched to demote_settled so settled objects
    //                      don't oscillate between the two states.
    uint8_t demote_tertiary   = 12;
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
};

// Load the config from the given path. Returns the populated struct;
// on missing / unreadable files or parse errors, returns sensible
// defaults and writes a short warning to stderr (callers can ignore —
// this is debug build tooling, not a gameplay fault).
StartupConfig load_startup_config(const std::string& path);
