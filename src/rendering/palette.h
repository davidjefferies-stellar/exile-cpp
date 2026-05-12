#pragma once
#include <cstdint>

// &0ee1 palette decode. Byte: hi nibble = colour 3, lo nibble = pair
// (c1,c2). lo masked at &0b78 to &3f for objects (lc 0..7) or &ff for
// tiles (full 16). Slot 0 always transparent.

struct ColourPair { uint8_t c1, c2; };

// Decoded &0b79 colours_1_and_2_pixel_values_table. c1 = right pixel, c2 =
// left pixel in the original byte layout.
extern const ColourPair COLOURS_1_AND_2[16];

// RGB for logical colours 0..15 (BBC mode-2 16 slot palette). Driven by the
// &11e5 palette_registers_table initial values.
extern const uint32_t LOGICAL_TO_RGB[16];

// 4-entry LUT: lut[0]=transparent (0), lut[1..3] = c1/c2/c3 RGB.
// is_tile true -> full 16-colour range; false -> low nibble masked to 0..7.
void resolve_palette(uint8_t palette_byte, bool is_tile, uint32_t lut[4]);

// fg flag = "pixel drawn with BBC logical colour >=8". 6502 leaves bit 7
// set on the plotted byte (tile mask &ff, object mask &3f); object plot
// BMIs past set pixels so objects hide behind foliage.
void resolve_palette_with_fg(uint8_t palette_byte, bool is_tile,
                              uint32_t lut[4], uint8_t fg[4]);
