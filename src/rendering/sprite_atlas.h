#pragma once
#include <cstdint>

// ============================================================================
// Sprite Atlas: source-pixel offsets into exile_sprites.png (320x256)
// ============================================================================
// 125 sprites (indices 0x00..0x7c), laid out in 6 row-bands at
// y = 6, 38, 80, 122, 164, 206 with variable horizontal packing.

static constexpr int ATLAS_W = 320;
static constexpr int ATLAS_H = 256;

struct SpriteAtlasEntry {
    uint16_t x;
    uint16_t y;
    uint8_t  w;
    uint8_t  h;
};

// Load exile_sprites.png into the static atlas buffer (0xAARRGGBB pixels,
// row-major). Returns false if the file could not be found or decoded.
bool atlas_load(const char* png_path);

// Pointer to the decoded atlas pixels. Valid after atlas_load() returns true.
const uint32_t* atlas_pixels();

static constexpr SpriteAtlasEntry sprite_atlas[125] = {
    {   0,   6, 13,  9 },  // 0x00 SPACESUIT_HORIZONTAL
    {  17,   6, 11, 17 },  // 0x01 SPACESUIT_FORTY_FIVE_HEAD_UP
    {  32,   6,  6, 20 },  // 0x02 SPACESUIT_JUMPING
    {  44,   6, 10, 19 },  // 0x03 SPACESUIT_FORTY_FIVE_HEAD_DOWN
    {  58,   6,  5, 22 },  // 0x04 SPACESUIT_VERTICAL
    {  70,   6,  6, 21 },  // 0x05 SPACESUIT_WALKING_ONE
    {  82,   6,  7, 21 },  // 0x06 SPACESUIT_WALKING_TWO
    {  94,   6,  5, 21 },  // 0x07 SPACESUIT_WALKING_THREE
    { 106,   6,  3,  2 },  // 0x08 BULLET_HORIZONTAL
    { 118,   6,  3,  2 },  // 0x09 BULLET_TWENTY_TWO
    { 130,   6,  3,  3 },  // 0x0a BULLET_FORTY_FIVE
    { 142,   6,  3,  4 },  // 0x0b BULLET_SIXTY
    { 154,   6,  2,  4 },  // 0x0c BULLET_SEVENTY_FIVE
    { 167,   6,  2,  4 },  // 0x0d BULLET_VERTICAL
    { 178,   6,  6, 12 },  // 0x0e LEAF
    { 190,   6,  4,  4 },  // 0x0f DROP
    { 202,   6,  7, 13 },  // 0x10 FROGMAN_ONE
    { 214,   6,  6, 16 },  // 0x11 FROGMAN_TWO
    { 226,   6,  6, 17 },  // 0x12 FROGMAN_THREE
    { 238,   6,  6, 20 },  // 0x13 ROLLING_ROBOT
    { 250,   6,  9, 16 },  // 0x14 CHATTER
    { 263,   6,  6, 15 },  // 0x15 HOVERING_ROBOT
    { 275,   6,  6, 20 },  // 0x16 CLAWED_ROBOT
    { 287,   6, 12, 19 },  // 0x17 FIREBALL
    { 304,   6, 12,  6 },  // 0x18 GREENERY
    {   0,  38,  9, 10 },  // 0x19 SHORT_BUSH
    {  13,  38,  9, 16 },  // 0x1a TALL_BUSH
    {  26,  38,  6, 14 },  // 0x1b LARGE_HIVE
    {  38,  38, 10,  5 },  // 0x1c SLIME_ONE
    {  52,  38,  8,  6 },  // 0x1d SLIME_TWO
    {  64,  38,  6,  8 },  // 0x1e SLIME_THREE
    {  76,  38,  4, 10 },  // 0x1f SLIME_FOUR
    {  88,  38,  5,  8 },  // 0x20 BOULDER
    { 100,  38,  3,  5 },  // 0x21 BALL
    { 112,  38,  2,  2 },  // 0x22 CRYSTAL
    { 124,  38, 16, 32 },  // 0x23 SPACESHIP_SUPPORT
    { 144,  38, 16, 32 },  // 0x24 SPACESHIP_WALL_CORNER_PIPES_ONE
    { 164,  38,  8, 14 },  // 0x25 SPACESHIP_WALL_SMALL_CORNER
    { 176,  38,  4, 32 },  // 0x26 SPACESHIP_WALL_VERTICAL_QUARTER
    { 188,  38, 16, 32 },  // 0x27 SPACESHIP_WALL_SLOPE_TWELVE_ONE
    { 208,  38, 16, 24 },  // 0x28 SPACESHIP_WALL_SLOPE_TWELVE_TWO
    { 228,  38, 16, 16 },  // 0x29 SPACESHIP_WALL_SLOPE_TWELVE_THREE
    { 248,  38, 16,  8 },  // 0x2a SPACESHIP_WALL_SLOPE_TWELVE_FOUR
    { 268,  38, 16, 32 },  // 0x2b SPACESHIP_WALL_CORNER
    { 288,  38, 16, 16 },  // 0x2c SPACESHIP_WALL_HORIZONTAL_HALF
    { 308,  38,  4,  8 },  // 0x2d SWITCH_BOX
    {   1,  81,  7,  8 },  // 0x2e SWITCH
    {  12,  80,  4,  8 },  // 0x2f RAIL_CORNER
    {  24,  80,  4, 32 },  // 0x30 RAIL_VERTICAL
    {  36,  80, 16,  8 },  // 0x31 RAIL_HORIZONTAL
    {  56,  80, 16, 16 },  // 0x32 SPACESHIP_WALL_HORIZONTAL_HALF_PIPES
    {  76,  80, 16, 32 },  // 0x33 SPACESHIP_WALL_CORNER_PIPES_TWO
    {  96,  80, 16, 32 },  // 0x34 STONE_SLOPE_TWENTY_TWO_ONE
    { 116,  80, 16, 16 },  // 0x35 STONE_SLOPE_TWENTY_TWO_TWO
    { 136,  80, 16, 32 },  // 0x36 EARTH_SLOPE_TWENTY_TWO_ONE
    { 156,  80, 16, 16 },  // 0x37 EARTH_SLOPE_TWENTY_TWO_TWO
    { 176,  80, 16, 32 },  // 0x38 EARTH_SLOPE_FORTY_FIVE
    { 196,  80, 16, 32 },  // 0x39 STONE
    { 216,  80, 16, 26 },  // 0x3a STONE_HORIZONTAL_THREE_QUARTERS
    { 236,  80, 16, 16 },  // 0x3b STONE_HORIZONTAL_HALF
    { 256,  80, 16,  8 },  // 0x3c STONE_HORIZONTAL_QUARTER
    { 276,  80, 16, 32 },  // 0x3d EARTH
    { 296,  80, 16, 18 },  // 0x3e EARTH_HORIZONTAL_HALF_WITH_EDGE
    {   1, 122, 16,  2 },  // 0x3f EARTH_EDGE
    {  20, 122, 16, 10 },  // 0x40 EARTH_HORIZONTAL_QUARTER_WITH_EDGE
    {  40, 122,  4, 32 },  // 0x41 STONE_VERTICAL_QUARTER
    {  52, 122,  7, 32 },  // 0x42 STONE_SLOPE_SEVENTY_EIGHT
    {  64, 122, 16, 32 },  // 0x43 STONE_SLOPE_FORTY_FIVE
    {  84, 122, 16, 32 },  // 0x44 STONE_SLOPE_FORTY_FIVE_FULL
    { 104, 122,  8, 14 },  // 0x45 GARGOYLE
    { 128, 129,  1,  1 },  // 0x46 NONE
    { 129, 122, 16, 32 },  // 0x47 SPACESHIP_WALL_PIPES
    { 148, 122, 16, 16 },  // 0x48 SPACESHIP_WALL_HORIZONTAL_HALF_TWO
    { 168, 122, 16,  8 },  // 0x49 SPACESHIP_WALL_HORIZONTAL_QUARTER
    { 188, 122, 16,  8 },  // 0x4a METAL_DOOR_HORIZONTAL
    { 208, 122,  4, 32 },  // 0x4b METAL_DOOR_VERTICAL
    { 220, 122,  4, 32 },  // 0x4c SPACESHIP_LEG
    { 232, 122,  5,  6 },  // 0x4d KEY
    { 244, 122, 16, 10 },  // 0x4e TRANSPORTER
    { 264, 122,  3,  4 },  // 0x4f WASP_ONE
    { 276, 123,  5,  3 },  // 0x50 WASP_TWO
    { 288, 122,  3,  4 },  // 0x51 WASP_THREE
    { 300, 122,  6,  5 },  // 0x52 WORM_ONE
    {   0, 164,  6,  6 },  // 0x53 WORM_TWO
    {  12, 164,  4,  9 },  // 0x54 WORM_THREE
    {  24, 164,  8, 32 },  // 0x55 COLUMN
    {  36, 164, 13, 15 },  // 0x56 CANNON
    {  53, 164,  5,  7 },  // 0x57 ALIEN_WEAPON
    {  65, 164,  5,  5 },  // 0x58 REMOTE_CONTROL_DEVICE
    {  77, 164,  3,  5 },  // 0x59 BIRD_ONE
    {  89, 164,  7,  4 },  // 0x5a BIRD_TWO
    { 101, 164,  7,  4 },  // 0x5b BIRD_THREE
    { 113, 164,  5,  6 },  // 0x5c BIRD_FOUR
    { 125, 164, 16, 13 },  // 0x5d PIANO
    { 145, 164,  8, 10 },  // 0x5e TURRET
    { 157, 164,  3, 17 },  // 0x5f WALL_MOUNTED_EQUIPMENT
    { 169, 164,  8, 12 },  // 0x60 CONSOLE
    { 181, 164, 10, 12 },  // 0x61 BIG_FISH
    { 195, 164, 13,  8 },  // 0x62 MUSHROOMS
    { 212, 164,  4,  4 },  // 0x63 MUSHROOM_BALL
    { 225, 164,  5, 14 },  // 0x64 IMP_WALKING_ONE
    { 236, 164,  6, 14 },  // 0x65 IMP_WALKING_TWO
    { 248, 165,  7, 14 },  // 0x66 IMP_WALKING_THREE
    { 260, 164,  5, 12 },  // 0x67 IMP_CLIMBING_ONE
    { 272, 164,  5, 14 },  // 0x68 IMP_CLIMBING_TWO
    { 284, 164,  4, 12 },  // 0x69 IMP_JUMPING
    { 296, 164, 16,  8 },  // 0x6a PIPE
    {   0, 206,  3,  8 },  // 0x6b JETPACK_BOOSTER
    {  12, 206,  4,  6 },  // 0x6c WEAPON
    {  24, 206,  4, 10 },  // 0x6d LIGHTNING_QUARTER
    {  36, 206,  8, 10 },  // 0x6e LIGHTNING_HALF
    {  48, 206, 12, 10 },  // 0x6f LIGHTNING_THREE_QUARTERS
    {  64, 206, 16, 10 },  // 0x70 NEST
    {  84, 206,  8,  2 },  // 0x71 TRANSPORTER_BEAM
    {  96, 206,  6,  7 },  // 0x72 PIRANHA_ONE
    { 108, 206,  5,  5 },  // 0x73 PIRANHA_TWO
    { 120, 206,  6,  6 },  // 0x74 PIRANHA_THREE
    { 132, 206,  6,  8 },  // 0x75 FLUFFY
    { 144, 206,  4, 15 },  // 0x76 FLASK
    { 169, 206,  9,  8 },  // 0x77 NONE_TWO
    { 181, 206,  4,  7 },  // 0x78 HOVERING_BALL
    { 193, 206,  4,  5 },  // 0x79 PILL
    { 197, 206,  6,  5 },  // 0x7a DEVICE
    { 205, 206,  6,  5 },  // 0x7b POWER_POD
    { 217, 206,  5,  5 },  // 0x7c WHISTLE
};
