#pragma once
#include <cstdint>

// Palette decoding — port of &0ee1 convert_palette_to_sixteen_colour_pixel_values.
//
// Each object/tile carries an 8-bit `palette` byte that picks three logical
// colours (slot 0 is always transparent). The high nibble indexes colour 3,
// the low nibble indexes a *pair* (colour 1, colour 2). The low-nibble pair
// is masked against the foreground/background mask at &0b78 which is &3f for
// objects (colours 1/2 restricted to logical 0..7) and &ff for tiles.

struct ColourPair { uint8_t c1, c2; };

// Decoded &0b79 colours_1_and_2_pixel_values_table. c1 = right pixel, c2 =
// left pixel in the original byte layout.
extern const ColourPair COLOURS_1_AND_2[16];

// RGB for logical colours 0..15 (BBC mode-2 16 slot palette). Driven by the
// &11e5 palette_registers_table initial values.
extern const uint32_t LOGICAL_TO_RGB[16];

// Build a 4-entry LUT for a sprite or tile.
//   lut[0] = transparent (left as 0 for callers to test)
//   lut[1] = colour 1 RGB
//   lut[2] = colour 2 RGB
//   lut[3] = colour 3 RGB
//
// is_tile true => no mask (tile draws with foreground slots 8..15 available);
// is_tile false => low-nibble colours restricted to 0..7 (objects).
void resolve_palette(uint8_t palette_byte, bool is_tile, uint32_t lut[4]);

// Same as resolve_palette but also emits a per-slot "is foreground" flag.
// The 6502 marks pixels drawn with BBC logical colour >=8 as foreground by
// leaving bit 7 set on the plotted byte (tiles use the &ff foreground-mask,
// objects the &3f background-mask, so object pixels can never be
// foreground). The object-plot routine then BMIs past any pixel that
// already has bit 7 set on the screen, hiding behind foliage etc.
// We reproduce this with a parallel 1-bit buffer: tile pixels that come
// from colours 8..15 set the bit; object blits skip writes where the bit
// is set.
void resolve_palette_with_fg(uint8_t palette_byte, bool is_tile,
                              uint32_t lut[4], uint8_t fg[4]);
