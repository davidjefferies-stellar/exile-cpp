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
};

// Load the config from the given path. Returns the populated struct;
// on missing / unreadable files or parse errors, returns sensible
// defaults and writes a short warning to stderr (callers can ignore —
// this is debug build tooling, not a gameplay fault).
StartupConfig load_startup_config(const std::string& path);
