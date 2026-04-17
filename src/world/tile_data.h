#pragma once
#include <cstdint>

// Tile-to-obstruction mapping tables from the disassembly.
// Used to look up which obstruction pattern a tile uses for collision detection.

// tiles_y_offset_and_pattern_table at &04eb (64 entries)
// High nibble: Y start offset of tile content (in 0x10 fractions)
// Low nibble: index into the set-of-4 patterns in obstruction_pattern_addresses[]
static constexpr uint8_t tiles_y_offset_and_pattern[] = {
    // &00-&0f
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd0, 0xc5, 0xb0, 0xc7, 0x00, 0x06, 0x00, 0x00, 0xc0,
    // &10-&1f
    0xb0, 0xa0, 0x07, 0x08, 0x00, 0x04, 0x80, 0xc0, 0x70, 0x00, 0xb0, 0x80, 0x99, 0x08, 0x00, 0x80,
    // &20-&2f
    0xc0, 0x00, 0xa0, 0x03, 0x02, 0x82, 0x01, 0x41, 0x81, 0xc1, 0x04, 0xf0, 0xb0, 0x00, 0x03, 0x02,
    // &30-&3f
    0x82, 0x70, 0x06, 0xc0, 0xc5, 0x04, 0x80, 0x06, 0x80, 0x04, 0x99, 0x30, 0xc7, 0x06, 0xa9, 0x00,
};

// tiles_obstruction_y_offsets_table at &056b (64 entries)
// High nibble: y offset if NOT flipped vertically
// Low nibble: y offset if flipped vertically
static constexpr uint8_t tiles_obstruction_y_offsets[] = {
    // &00-&0f
    0x0f, 0x3a, 0x0f, 0x0f, 0x0f, 0x77, 0x0f, 0xf0, 0xb0, 0xf0, 0xc0, 0x0f, 0x00, 0xf0, 0x0f, 0xf0,
    // &10-&1f
    0x0f, 0x0f, 0x00, 0x06, 0x0f, 0x00, 0x77, 0xb3, 0x0f, 0x0f, 0x0f, 0x0f, 0x90, 0x06, 0x0f, 0x77,
    // &20-&2f
    0xb3, 0xf0, 0x0f, 0x10, 0x07, 0x70, 0x0b, 0x37, 0x73, 0xb0, 0x00, 0x0f, 0xb3, 0x0f, 0x10, 0x07,
    // &30-&3f
    0x70, 0x77, 0x00, 0xb3, 0xb0, 0x00, 0x77, 0x00, 0x77, 0x00, 0x90, 0x2c, 0xc0, 0x00, 0x90, 0x0f,
};

// obstruction_pattern_low_addresses_table at &05ab
// 10 groups of 4 entries each: [normal, v-flip, h-flip, vh-flip]
// Each value is a byte offset into the obstruction_patterns at &0100.
// Divide by 8 to get the pattern index into Obstruction::patterns[].
static constexpr uint8_t obstruction_pattern_addresses[] = {
    0x00, 0x00, 0x00, 0x00, // group 0: empty
    0x08, 0x40, 0x40, 0x08, // group 1: gentle slope
    0x10, 0x48, 0x48, 0x10, // group 2: medium slope
    0x18, 0x50, 0x50, 0x18, // group 3: steep slope
    0x38, 0x20, 0x70, 0x58, // group 4: quarter solid
    0x38, 0x78, 0x70, 0x80, // group 5: curved
    0x30, 0x60, 0x68, 0x28, // group 6: curved alt
    0x88, 0x90, 0x88, 0x90, // group 7: middle
    0xa0, 0x50, 0x98, 0x18, // group 8: partial slope
    0x00, 0x00, 0x00, 0x00, // group 9: empty (padding)
};

// Get the obstruction pattern index for a tile type with given flip flags.
// Returns an index into Obstruction::patterns[].
inline int get_obstruction_pattern_index(uint8_t tile_type, bool flip_h, bool flip_v) {
    if (tile_type >= 64) return 0; // out of range = empty

    // Get the pattern group from the low nibble of the y_offset_and_pattern table
    uint8_t group = tiles_y_offset_and_pattern[tile_type] & 0x0f;
    if (group >= 9) return 0; // clamp

    // Select variant based on flip flags: [normal, v, h, vh]
    int variant = (flip_v ? 1 : 0) | (flip_h ? 2 : 0);

    // Look up the byte offset into obstruction_patterns (&0100)
    uint8_t addr = obstruction_pattern_addresses[group * 4 + variant];

    // Convert byte offset to pattern index (each pattern is 8 bytes)
    return addr / 8;
}

// Get the Y offset for a tile's obstruction. Port of &245f-&246f:
// pick high nibble (non-flipped) or low nibble (flipped), shift to 0xN0; if
// non-zero, OR with 0x0f to round to end of pixel (original &246e ORA #&0f).
inline uint8_t get_tile_y_offset(uint8_t tile_type, bool flip_v) {
    if (tile_type >= 64) return 0;
    uint8_t offsets = tiles_obstruction_y_offsets[tile_type];
    uint8_t hi = flip_v ? static_cast<uint8_t>((offsets & 0x0f) << 4)
                        : static_cast<uint8_t>(offsets & 0xf0);
    return hi == 0 ? 0 : static_cast<uint8_t>(hi | 0x0f);
}

// Waterline X-range boundaries from &14d2
static constexpr uint8_t waterline_x_ranges_x[] = {
    0x00, 0x54, 0x74, 0xa0,
};

// Initial waterline Y values from &0832
static constexpr uint8_t waterline_initial_y[] = {
    0xce, 0xdf, 0xc1, 0xc1,
};
