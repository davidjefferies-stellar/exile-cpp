#pragma once
#include <cstdint>

// ============================================================================
// Object Types (101 types, &00-&64)
// From disassembly at &028a (sprite table comments)
// ============================================================================
enum class ObjectType : uint8_t {
    PLAYER                    = 0x00,
    ACTIVE_CHATTER            = 0x01,
    CREW_MEMBER               = 0x02,
    FLUFFY                    = 0x03,
    SMALL_HIVE                = 0x04,
    LARGE_HIVE                = 0x05,
    RED_FROGMAN               = 0x06,
    GREEN_FROGMAN             = 0x07,
    INVISIBLE_FROGMAN         = 0x08,
    RED_SLIME                 = 0x09,
    GREEN_SLIME               = 0x0a,
    YELLOW_SLIME              = 0x0b,
    DENSE_NEST                = 0x0c,
    SUCKING_NEST              = 0x0d,
    BIG_FISH                  = 0x0e,
    WORM                      = 0x0f,
    PIRANHA                   = 0x10,
    WASP                      = 0x11,
    ACTIVE_GRENADE            = 0x12,
    ICER_BULLET               = 0x13,
    TRACER_BULLET             = 0x14,
    CANNONBALL                = 0x15,
    BLUE_DEATH_BALL           = 0x16,
    RED_BULLET                = 0x17,
    PISTOL_BULLET             = 0x18,
    PLASMA_BALL               = 0x19,
    HOVERING_BALL             = 0x1a,
    INVISIBLE_HOVERING_BALL   = 0x1b,
    MAGENTA_ROLLING_ROBOT     = 0x1c,
    RED_ROLLING_ROBOT         = 0x1d,
    BLUE_ROLLING_ROBOT        = 0x1e,
    GREEN_WHITE_TURRET        = 0x1f,
    CYAN_RED_TURRET           = 0x20,
    HOVERING_ROBOT            = 0x21,
    MAGENTA_CLAWED_ROBOT      = 0x22,
    CYAN_CLAWED_ROBOT         = 0x23,
    GREEN_CLAWED_ROBOT        = 0x24,
    RED_CLAWED_ROBOT          = 0x25,
    TRIAX                     = 0x26,
    MAGGOT                    = 0x27,
    GARGOYLE                  = 0x28,
    RED_MAGENTA_IMP           = 0x29,
    RED_YELLOW_IMP            = 0x2a,
    BLUE_CYAN_IMP             = 0x2b,
    CYAN_YELLOW_IMP           = 0x2c,
    RED_CYAN_IMP              = 0x2d,
    GREEN_YELLOW_BIRD         = 0x2e,
    WHITE_YELLOW_BIRD         = 0x2f,
    RED_MAGENTA_BIRD          = 0x30,
    INVISIBLE_BIRD            = 0x31,
    LIGHTNING                 = 0x32,
    RED_MUSHROOM_BALL         = 0x33,
    BLUE_MUSHROOM_BALL        = 0x34,
    INVISIBLE_DEBRIS          = 0x35,
    RED_DROP                  = 0x36,
    FIREBALL                  = 0x37,
    INACTIVE_CHATTER          = 0x38,
    MOVING_FIREBALL           = 0x39,
    GIANT_BLOCK               = 0x3a,
    ENGINE_FIRE               = 0x3b,
    HORIZONTAL_METAL_DOOR     = 0x3c,
    VERTICAL_METAL_DOOR       = 0x3d,
    HORIZONTAL_STONE_DOOR     = 0x3e,
    VERTICAL_STONE_DOOR       = 0x3f,
    BUSH                      = 0x40,
    TRANSPORTER_BEAM          = 0x41,
    SWITCH                    = 0x42,
    PIANO                     = 0x43,
    EXPLOSION                 = 0x44,
    BOULDER                   = 0x45,
    CANNON                    = 0x46,
    ALIEN_WEAPON              = 0x47,
    MAGGOT_MACHINE            = 0x48,
    PLACEHOLDER               = 0x49,
    DESTINATOR                = 0x4a,
    POWER_POD                 = 0x4b,
    EMPTY_FLASK               = 0x4c,
    FULL_FLASK                = 0x4d,
    REMOTE_CONTROL_DEVICE     = 0x4e,
    CANNON_CONTROL_DEVICE     = 0x4f,
    INACTIVE_GRENADE          = 0x50,
    CYAN_YELLOW_GREEN_KEY     = 0x51,
    RED_YELLOW_GREEN_KEY      = 0x52,
    GREEN_YELLOW_RED_KEY      = 0x53,
    YELLOW_WHITE_RED_KEY      = 0x54,
    CORONIUM_BOULDER          = 0x55,
    RED_MAGENTA_RED_KEY       = 0x56,
    BLUE_CYAN_GREEN_KEY       = 0x57,
    CORONIUM_CRYSTAL          = 0x58,
    JETPACK_BOOSTER           = 0x59,
    PISTOL                    = 0x5a,
    ICER                      = 0x5b,
    BLASTER                   = 0x5c,
    PLASMA_GUN                = 0x5d,
    PROTECTION_SUIT           = 0x5e,
    FIRE_IMMUNITY_DEVICE      = 0x5f,
    MUSHROOM_IMMUNITY_PILL    = 0x60,
    WHISTLE_ONE               = 0x61,
    WHISTLE_TWO               = 0x62,
    RADIATION_IMMUNITY_PILL   = 0x63,
    INVISIBLE_INERT           = 0x64,

    COUNT                     = 0x65
};

// ============================================================================
// Object Flags (stored in objects_flags at &08c6)
// ============================================================================
namespace ObjectFlags {
    constexpr uint8_t FLIP_HORIZONTAL = 0x80;
    constexpr uint8_t FLIP_VERTICAL   = 0x40;
    constexpr uint8_t PENDING_REMOVAL = 0x20;
    constexpr uint8_t TELEPORTING     = 0x10;
    constexpr uint8_t WAS_DAMAGED     = 0x08;
    constexpr uint8_t NEWLY_CREATED   = 0x04;
    constexpr uint8_t SUPPORTED       = 0x02;
    constexpr uint8_t NOT_PLOTTED     = 0x01;
}

// ============================================================================
// Object Type Flags (from &0354)
// ============================================================================
namespace ObjectTypeFlags {
    constexpr uint8_t INTANGIBLE                    = 0x80;
    constexpr uint8_t DO_NOT_KEEP_AS_SECONDARY      = 0x40;
    constexpr uint8_t KEEP_AS_PRIMARY_FOR_LONGER    = 0x20;
    constexpr uint8_t KEEP_AS_TERTIARY              = 0x10;
    constexpr uint8_t SPAWNED_FROM_NEST             = 0x08;
    constexpr uint8_t WEIGHT_MASK                   = 0x07;
}

// ============================================================================
// Target/Directness flags (stored in objects_target_object_and_flags at &0906)
// ============================================================================
namespace TargetFlags {
    constexpr uint8_t DIRECTNESS_MASK  = 0xC0;
    constexpr uint8_t DIRECTNESS_THREE = 0xC0;
    constexpr uint8_t DIRECTNESS_TWO   = 0x80;
    constexpr uint8_t DIRECTNESS_ONE   = 0x40;
    constexpr uint8_t DIRECTNESS_ZERO  = 0x00;
    constexpr uint8_t AVOID            = 0x20;
    constexpr uint8_t OBJECT_MASK      = 0x1F;
}

// ============================================================================
// Tile flip flags (returned by landscape generation in high bits)
// ============================================================================
namespace TileFlip {
    constexpr uint8_t HORIZONTAL = 0x80;
    constexpr uint8_t VERTICAL   = 0x40;
    constexpr uint8_t MASK       = 0xC0;
    constexpr uint8_t TYPE_MASK  = 0x3F;
}

// ============================================================================
// Tile types — the low 6 bits of a landscape byte. Names from the 6502
// disassembly catalogue (#675-#738). Use `static_cast<uint8_t>(TileType::X)`
// when comparing against raw tile bytes already masked with TileFlip::TYPE_MASK.
// ============================================================================
enum class TileType : uint8_t {
    INVISIBLE_SWITCH                 = 0x00,
    TRANSPORTER                      = 0x01,
    SPACE_WITH_OBJECT_FROM_DATA      = 0x02,
    METAL_DOOR                       = 0x03,
    STONE_DOOR                       = 0x04,
    STONE_HALF_WITH_OBJECT_FROM_TYPE = 0x05,
    SPACE_WITH_OBJECT_FROM_TYPE      = 0x06,
    GREENERY_WITH_OBJECT_FROM_TYPE   = 0x07,
    SWITCH                           = 0x08,
    NEST                             = 0x09,
    PIPE                             = 0x0a,
    CONSTANT_WIND                    = 0x0b,
    ENGINE                           = 0x0c,
    WATER                            = 0x0d,
    VARIABLE_WIND                    = 0x0e,
    MUSHROOMS                        = 0x0f,
    GREEN_HORIZONTAL_QUARTER         = 0x10,
    POSSIBLE_LEAF                    = 0x11,
    STONE_ONE                        = 0x12,
    STONE_SLOPE_45_FULL              = 0x13,
    SPACESHIP_WALL_PIPES             = 0x14,
    SPACESHIP_WALL_VERTICAL_QUARTER  = 0x15,
    SPACESHIP_WALL_HORIZONTAL_HALF_TWO = 0x16,
    SPACESHIP_WALL_HORIZONTAL_QUARTER  = 0x17,
    WALL_MOUNTED_EQUIPMENT           = 0x18,
    SPACE                            = 0x19,
    SHORT_BUSH                       = 0x1a,
    TALL_BUSH                        = 0x1b,
    SPACESHIP_WALL_SMALL_CORNER      = 0x1c,
    SPACESHIP_WALL_CORNER            = 0x1d,
    STONE_TWO                        = 0x1e,
    STONE_HORIZONTAL_HALF            = 0x1f,
    STONE_HORIZONTAL_QUARTER         = 0x20,
    COLUMN                           = 0x21,
    LEAF                             = 0x22,
    STONE_SLOPE_45                   = 0x23,
    STONE_SLOPE_22_ONE               = 0x24,
    STONE_SLOPE_22_TWO               = 0x25,
    SPACESHIP_WALL_SLOPE_12_ONE      = 0x26,
    SPACESHIP_WALL_SLOPE_12_TWO      = 0x27,
    SPACESHIP_WALL_SLOPE_12_THREE    = 0x28,
    SPACESHIP_WALL_SLOPE_12_FOUR     = 0x29,
    STONE_SLOPE_78                   = 0x2a,
    EARTH_EDGE                       = 0x2b,
    EARTH_HORIZ_QUARTER_EDGE         = 0x2c,
    EARTH                            = 0x2d,
    EARTH_SLOPE_45                   = 0x2e,
    EARTH_SLOPE_22_ONE               = 0x2f,
    EARTH_SLOPE_22_TWO               = 0x30,
    EARTH_HORIZ_HALF_WITH_EDGE       = 0x31,
    SPACESHIP_WALL_CORNER_PIPES_TWO  = 0x32,
    RAIL_HORIZONTAL                  = 0x33,
    RAIL_CORNER                      = 0x34,
    RAIL_VERTICAL                    = 0x35,
    SPACESHIP_WALL_HORIZONTAL_HALF   = 0x36,
    SPACESHIP_WALL_CORNER_PIPES      = 0x37,
    SPACESHIP_WALL_HORIZONTAL_HALF_PIPES = 0x38,
    STONE_VERTICAL_QUARTER           = 0x39,
    GARGOYLE_TILE                    = 0x3a,
    STONE_HORIZ_THREE_QUARTERS       = 0x3b,
    RED_PIPE                         = 0x3c,
    SPACESHIP_SUPPORT                = 0x3d,
    CONSOLE                          = 0x3e,
    SPACESHIP_LEG                    = 0x3f,
};

// Helper: compare a raw byte's type bits against a TileType.
inline bool tile_is(uint8_t raw, TileType t) {
    return (raw & TileFlip::TYPE_MASK) == static_cast<uint8_t>(t);
}

// ============================================================================
// Game Colors (8 BBC Micro logical colors)
// ============================================================================
enum class GameColor : uint8_t {
    BLACK   = 0,
    RED     = 1,
    GREEN   = 2,
    YELLOW  = 3,
    BLUE    = 4,
    MAGENTA = 5,
    CYAN    = 6,
    WHITE   = 7
};

// ============================================================================
// Game Constants
// ============================================================================
namespace GameConstants {
    constexpr int PRIMARY_OBJECT_SLOTS   = 16;
    constexpr int SECONDARY_OBJECT_SLOTS = 32;
    constexpr int TERTIARY_OBJECT_SLOTS  = 254;

    constexpr int WORLD_WIDTH  = 256;
    constexpr int WORLD_HEIGHT = 256;

    // Player start position (from &19b6 area, initial objects_x/y for player)
    constexpr uint8_t PLAYER_START_X = 0x9B;
    constexpr uint8_t PLAYER_START_Y = 0x3B;

    // Surface level
    constexpr uint8_t SURFACE_Y = 0x4E;

    // Wind center
    constexpr uint8_t WIND_CENTER_X = 0x9B;

    // Frame timing (original runs at 50Hz PAL)
    constexpr int TARGET_FPS = 50;
    constexpr int FRAME_TIME_MS = 1000 / TARGET_FPS; // 20ms
}

// ============================================================================
// NPC Mood (top 2 bits of objects_state)
// ============================================================================
namespace NPCMood {
    constexpr uint8_t MINUS_TWO = 0x80;
    constexpr uint8_t MINUS_ONE = 0xC0;
    constexpr uint8_t ZERO      = 0x00;
    constexpr uint8_t PLUS_ONE  = 0x40;
    constexpr uint8_t MASK      = 0xC0;
}
