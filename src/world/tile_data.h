#pragma once
#include <cstdint>
#include "core/types.h"
#include "world/obstruction.h"

// Tile-to-obstruction mapping tables from the disassembly.
// Used to look up which obstruction pattern a tile uses for collision detection.

// tiles_palette_table at &052b (64 entries). Direct palette bytes unless the
// low byte is &00..&06, in which case the palette is computed procedurally
// (see resolve_tile_palette below).
static constexpr uint8_t tiles_palette_table[64] = {
    // &00-&0f
    0x80, 0x02, 0x91, 0x91, 0x91, 0x00, 0x91, 0xa8, 0xdc, 0xb8, 0x8c, 0x80, 0xc9, 0x4a, 0x80, 0x06,
    // &10-&1f
    0x88, 0x05, 0x04, 0x00, 0x02, 0x02, 0x02, 0x02, 0x02, 0x91, 0x03, 0x03, 0x02, 0x02, 0x00, 0x00,
    // &20-&2f
    0x00, 0xbc, 0xb1, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x00, 0x04, 0x04, 0x04, 0x04, 0x04,
    // &30-&3f
    0x04, 0x04, 0x02, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x82, 0x02, 0x64, 0xee,
};

// strata_palette_table at &1185 (16 entries). Stone indexes 0..9 + 15; earth
// indexes 7..14. Leaf/bush/mushroom live outside the table.
static constexpr uint8_t strata_palette_table[16] = {
    0x8d, 0x82, 0x8b, 0x8f, 0x84, 0x89, 0x8d, 0x81, // &1185-&118c (stone/earth transition)
    0x82, 0x81, 0x85, 0xb2, 0xcd, 0x90, 0x95, 0x81, // &118d-&1194 (earth / stone extra)
};

// bushes_palette_table at &1195 (4 entries).
static constexpr uint8_t bushes_palette_table[4] = {
    0xb1, 0x97, 0xfd, 0xf3,
};

// Port of &23e4-&23fe "is_leaf" logic. TILE_POSSIBLE_LEAF (0x11) passes
// through this to decide:
//   1) whether the leaf is removed entirely (tile_type becomes TILE_SPACE),
//   2) whether its vertical-flip bit should be toggled,
//   3) which palette (green / yellow / white) to render with.
// The 6502 uses a bit-mixing chain of ROR/EOR/SBC over tile_y to drive all
// three decisions at once. We reproduce the visible behaviour:
//   * leaf removed when (y & 4) != (y & 1) — matches BCC at &23eb
//   * a pseudo-random toggle of the vertical flip based on mixed y bits
//   * palette is green by default; switches to yellow/white variants when
//     the ORIGINAL tile_flip had V set (matches BVC at &23fc)
struct PossibleLeafResult {
    bool     remove;
    uint8_t  tile_flip;
    uint8_t  palette;
};

inline PossibleLeafResult process_possible_leaf(uint8_t tile_y,
                                                uint8_t tile_flip) {
    PossibleLeafResult r{};
    r.tile_flip = tile_flip;

    // 1) Leaf-removal test, bit-exact to &23e8-&23eb. After two RORs +
    //    EOR'ing with y, bit 0 of A ends up as (y bit 2) XOR (y bit 0).
    //    Faithful simplification: compare those two bits directly.
    r.remove = (((tile_y >> 2) ^ tile_y) & 1) != 0;

    // 2) Vertical-flip toggle. The 6502 runs two more RORs, SBCs y, then
    //    ANDs #&40 — which gives the derived bit 6 of a y-hashed value.
    //    We approximate with a simple bit-mix that keeps a similar "~50%
    //    of leaves flipped" distribution.
    uint8_t mix = static_cast<uint8_t>(
        (tile_y << 1) ^ (tile_y >> 3) ^ tile_y ^ 0x13);
    if (mix & 0x40) {
        r.tile_flip ^= TileFlip::VERTICAL;
    }

    // 3) Palette. The 6502 at &23f6 BITs the ORIGINAL tile_flip (before
    //    the EOR that updated it): V clear → keep 0xb1 (rgy, green);
    //    V set → ADC #&0a, giving 0xbb (cyy) or 0xbc (ywy) depending on
    //    the carry-in from the earlier SBC. We pick between those two
    //    from another bit of y.
    if (tile_flip & TileFlip::VERTICAL) {
        r.palette = (tile_y & 1) ? 0xbc : 0xbb;
    } else {
        r.palette = 0xb1;
    }
    return r;
}

// Port of &239c calculate_palette_for_tile. The table values 0..6 trigger
// procedural palettes that depend on tile position and flip; any other value
// is used as-is.
//
// Leaf processing (case 0x05) mutates both tile_type (TILE_POSSIBLE_LEAF can
// flip to TILE_SPACE when removed) and tile_flip (V toggle), so both are
// passed by reference.
inline uint8_t resolve_tile_palette(uint8_t& tile_type, uint8_t tile_x,
                                    uint8_t tile_y, uint8_t& tile_flip) {
    uint8_t val = tiles_palette_table[tile_type & 0x3f];
    if (val >= 0x07) return val;

    switch (val) {
        case 0x00: {
            // Stone — strata changes every 16 tile rows starting at y=&54.
            int i = static_cast<int>(tile_y) - 0x54;
            if (i < 0) i = 0;
            i >>= 4;
            if (i > 15) i = 15;
            return strata_palette_table[i];
        }
        case 0x01:
        case 0x02: {
            // Spacecraft / Triax machinery — &b2/&b3 in top half, &f5/&f7 in
            // bottom (approximate port of &23b2..&23b9: ADC #&b1 then optional
            // ASL+ADC #&90 with carry set).
            uint8_t pal = static_cast<uint8_t>(0xb1 + val);
            if (tile_y & 0x80) {
                pal = static_cast<uint8_t>((pal << 1) + 0x91);
            }
            return pal;
        }
        case 0x03: {
            // Bush — depends on tile_flip, tile_y, tile_x. Not bit-exact but
            // gives the same 4-way cycling the original produces.
            uint8_t a = static_cast<uint8_t>(tile_flip << 3);
            a = static_cast<uint8_t>(a - tile_y);
            a = static_cast<uint8_t>(a >> 1);
            a = static_cast<uint8_t>(a + tile_x);
            return bushes_palette_table[a & 0x03];
        }
        case 0x04: {
            // Earth — strata[7..14] indexed by top 3 bits of tile_y.
            int i = 7 + ((tile_y >> 5) & 0x07);
            return strata_palette_table[i];
        }
        case 0x05: {
            // TILE_POSSIBLE_LEAF — some leaves disappear, some flip, and the
            // palette varies between green / yellow / white. See
            // process_possible_leaf above.
            auto leaf = process_possible_leaf(tile_y, tile_flip);
            if (leaf.remove) {
                tile_type = 0x19;   // TILE_SPACE — caller's sprite lookup
                                    // will yield "no sprite", skipping the
                                    // blit entirely.
            }
            tile_flip = leaf.tile_flip;
            return leaf.palette;
        }
        case 0x06:
            // Mushroom — red on the floor, blue on the ceiling.
            return (tile_flip & TileFlip::VERTICAL) ? 0xcf : 0x9c;
    }
    return 0;
}

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

// Bit 7 of tiles_sprite_and_y_flip_table at &04ab. "1" means the tile's
// obstruction is at the TOP of the unflipped cell (ceiling-like); "0"
// means at the BOTTOM (ground-like). Must be XOR'd with the landscape's
// flip_v to get the effective collision flip the 6502 hands to its
// obstruction-threshold test (&2477).
inline bool tile_obstruction_v_flip_bit(uint8_t tile_type) {
    static constexpr uint8_t tiles_sprite_and_y_flip[64] = {
        0xc6, 0xce, 0xc6, 0xc6, 0xc6, 0xbb, 0xc6, 0x18,
        0x2d, 0x70, 0x6a, 0xc6, 0x23, 0x39, 0xc6, 0x62,
        0xc0, 0x8e, 0x39, 0x44, 0x47, 0x26, 0x48, 0x49,
        0xdf, 0xc6, 0x99, 0x9a, 0x25, 0x2b, 0x39, 0x3b,
        0x3c, 0x55, 0x8e, 0x43, 0x34, 0x35, 0x27, 0x28,
        0x29, 0x2a, 0x42, 0xbf, 0x40, 0x3d, 0x38, 0x36,
        0x37, 0x3e, 0x33, 0x31, 0x2f, 0x30, 0x2c, 0x24,
        0x32, 0x41, 0x45, 0x3a, 0x6a, 0x23, 0x60, 0xcc,
    };
    return (tiles_sprite_and_y_flip[tile_type & 0x3f] & 0x80) != 0;
}

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
    0x18, 0x98, 0x50, 0xa0, // group 9: partial slope (mirror of 8)
};

// Get the obstruction pattern index for a tile type with given flip flags.
// Returns an index into Obstruction::patterns[].
inline int get_obstruction_pattern_index(uint8_t tile_type, bool flip_h, bool flip_v) {
    if (tile_type >= 64) return 0; // out of range = empty

    // Get the pattern group from the low nibble of the y_offset_and_pattern table
    uint8_t group = tiles_y_offset_and_pattern[tile_type] & 0x0f;
    if (group > 9) return 0; // clamp — only groups 0-9 exist in the 6502 table

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

// Pattern-aware obstruction threshold for a specific x-section of a
// tile. Slope tiles (non-zero pattern group) have a threshold that
// varies with x; flat tiles collapse to the same value everywhere.
inline uint8_t tile_threshold_at_x(uint8_t tile_type, bool flip_h,
                                    bool flip_v, uint8_t x_frac) {
    if (tile_type >= 64) return 0;
    int pattern_idx = get_obstruction_pattern_index(tile_type, flip_h, flip_v);
    uint8_t y_offset = get_tile_y_offset(tile_type, flip_v);
    if (pattern_idx < 0 ||
        pattern_idx >= static_cast<int>(Obstruction::patterns.size())) {
        return y_offset;
    }
    int x_section = (x_frac >> 5) & 0x07;
    int t = static_cast<int>(Obstruction::patterns[pattern_idx][x_section]) +
            static_cast<int>(y_offset);
    return (t > 0xff) ? 0xff : static_cast<uint8_t>(t);
}

// Waterline X-range boundaries from &14d2
static constexpr uint8_t waterline_x_ranges_x[] = {
    0x00, 0x54, 0x74, 0xa0,
};

// Initial waterline Y values from &0832
static constexpr uint8_t waterline_initial_y[] = {
    0xce, 0xdf, 0xc1, 0xc1,
};
