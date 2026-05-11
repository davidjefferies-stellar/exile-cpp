#pragma once
#include <cstdint>
#include "world/landscape.h"

// Result of a tertiary lookup: the tile_type|flip byte to render, plus the
// offsets needed to spawn the tertiary's object if applicable.
// tertiary_index == -1 means no tertiary matched and the tile was filled
// from feature_tiles_table.
struct ResolvedTile {
    uint8_t tile_and_flip;
    int tertiary_index;
    int data_offset;
    int type_offset;
    // Pre-redirect landscape type. INVISIBLE_SWITCH redirected to a
    // metal-door graphic reuses bit 7 of data for switch-effects high
    // bit, NOT the door's "needs creating" gate.
    uint8_t raw_tile_type;
};

// &1715 get_tile_and_check_for_tertiary_objects. Tiles 0x00-0x08 scan
// tertiary x-data for matching tile_x; miss → feature_tiles_table fallback.
ResolvedTile resolve_tile_with_tertiary(const Landscape& landscape,
                                        uint8_t tile_x, uint8_t tile_y);

// Reverse: data_offset byte → (tile_x, tile_y). Inverts 6502 arithmetic
// data_offset = (tertiary_index + tertiary_data_offset[type]) mod 256,
// scans column for matching tile_type. Used by Wiring overlay.
bool resolve_data_offset_to_tile(const Landscape& landscape,
                                 uint8_t data_offset,
                                 uint8_t& out_x, uint8_t& out_y);

// (tile_type, tertiary_index) → world (x,y). Validates via the forward
// resolver so duplicate-x dead entries return false. Cheaper than
// resolve_data_offset_to_tile when the pair is already known.
bool find_tertiary_tile(const Landscape& landscape,
                        int tile_type, int tertiary_index,
                        uint8_t& out_x, uint8_t& out_y);
