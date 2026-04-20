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
    // Landscape tile-type before tertiary redirection. Needed because the
    // data-byte semantics depend on it: e.g. a tile whose landscape type is
    // TILE_INVISIBLE_SWITCH (0x00) but whose tertiary tile_and_flip resolves
    // to a metal-door graphic reuses bit 7 for the switch-effects-number
    // high bit, NOT the door's "needs creating" gate. Spawn/gate logic keys
    // off this to avoid rejecting those doors at game start.
    uint8_t raw_tile_type;
};

// Port of &1715 get_tile_and_check_for_tertiary_objects. Tiles 0x00-0x08 are
// "CHECK_TERTIARY_OBJECT_RANGE_N" markers: for those we scan the tertiary
// x-data range for a matching tile_x and, if found, replace the marker with
// the tertiary's tile_and_flip byte. Otherwise feature_tiles_table supplies
// a default tile (keeping the landscape's original flip bits).
ResolvedTile resolve_tile_with_tertiary(const Landscape& landscape,
                                        uint8_t tile_x, uint8_t tile_y);
