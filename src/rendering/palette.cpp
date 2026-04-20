#include "rendering/palette.h"

// Decoded &0b79 colours_1_and_2_pixel_values_table.
// Raw bytes: ca c9 e3 e9 eb ce f8 e6 cc ee 30 de ef cb fb fe.
// Each byte interleaves two 4-bit logical colours in BBC MODE 2 layout:
//   c1 (right pixel) = bits 0,2,4,6 → colour-value bits 0..3
//   c2 (left pixel)  = bits 1,3,5,7 → colour-value bits 0..3
// Values below are the raw logical colours 0..15; resolve_palette applies the
// &0b78 foreground/background mask when building the final LUT.
const ColourPair COLOURS_1_AND_2[16] = {
    { 8,  11}, { 9,  10}, { 9,  13}, { 9,  14},
    { 9,  15}, {10,  11}, {12,  14}, {10,  13},
    {10,  10}, {10,  15}, { 4,   4}, {14,  11},
    {11,  15}, { 9,  11}, {13,  15}, {14,  15},
};

// Initial physical palette from &11e5 palette_registers_table:
//   K R G Y B M C W k r g y b m c w
// All 16 slots are initialised to the same 8 BBC hues (0..7 repeated for
// 8..15); gameplay may later reprogram slots at runtime (VDU 19). We use
// shade 0xCC to match the rest of exile-cpp's rendering.
const uint32_t LOGICAL_TO_RGB[16] = {
    0x000000, // 0  K black
    0xCC0000, // 1  R red
    0x00CC00, // 2  G green
    0xCCCC00, // 3  Y yellow
    0x2244CC, // 4  B blue
    0xCC00CC, // 5  M magenta
    0x00CCCC, // 6  C cyan
    0xCCCCCC, // 7  W white
    0x000000, // 8  k black
    0xCC0000, // 9  r red
    0x00CC00, // a  g green
    0xCCCC00, // b  y yellow
    0x2244CC, // c  b blue
    0xCC00CC, // d  m magenta
    0x00CCCC, // e  c cyan
    0xCCCCCC, // f  w white
};

void resolve_palette(uint8_t palette_byte, bool is_tile, uint32_t lut[4]) {
    uint8_t fg[4];
    resolve_palette_with_fg(palette_byte, is_tile, lut, fg);
}

void resolve_palette_with_fg(uint8_t palette_byte, bool is_tile,
                              uint32_t lut[4], uint8_t fg[4]) {
    // Colour 0 is always transparent (matches &0ec0 "Colour 0 is always
    // black"). Callers treat a zero entry as skip.
    lut[0] = 0;
    fg[0]  = 0;

    // Colour 3 — high nibble indexes colour_3_pixel_values_table, which is a
    // trivial identity (nibble == logical colour). No mask applied at &0ee8.
    uint8_t c3 = (palette_byte >> 4) & 0x0f;
    lut[3] = LOGICAL_TO_RGB[c3];
    fg[3]  = (c3 >= 8) ? 1 : 0;

    // Colours 1 and 2 — low nibble indexes the pair table. For objects
    // (is_tile == false) the 6502 stores the pixel byte AND'd with &3f, which
    // clears the logical-colour bit-3 in both pixels. The equivalent in
    // logical space is masking each colour with 0x07.
    ColourPair p = COLOURS_1_AND_2[palette_byte & 0x0f];
    if (!is_tile) {
        p.c1 &= 0x07;
        p.c2 &= 0x07;
    }
    lut[1] = LOGICAL_TO_RGB[p.c1];
    lut[2] = LOGICAL_TO_RGB[p.c2];
    fg[1]  = (p.c1 >= 8) ? 1 : 0;
    fg[2]  = (p.c2 >= 8) ? 1 : 0;
}
