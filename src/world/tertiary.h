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

// Reverse lookup: given a tertiary `data_offset` byte (as used in
// switch_effects_table entries and stored in Object::tertiary_slot),
// find the world (tile_x, tile_y) of the tertiary entry it addresses.
// Matches the 6502 index arithmetic in reverse — data_offset =
// (tertiary_index + tertiary_data_offset[tile_type]) mod 256 — and
// resolves tile_y by scanning the landscape column for a matching tile
// type. Returns false if no matching tertiary / landscape tile is
// found. Used by the Wiring debug overlay so switches can point at
// doors still sitting in tertiary storage.
bool resolve_data_offset_to_tile(const Landscape& landscape,
                                 uint8_t data_offset,
                                 uint8_t& out_x, uint8_t& out_y);

// Given a known (tile_type, tertiary_index) pair, find the landscape
// tile that that entry is placed at. Scans the x-column from
// tertiary_objects_x_data[tertiary_index] and validates via the forward
// resolver so dead-duplicate entries (same x as an earlier index in the
// range) correctly return false. Cheaper than resolve_data_offset_to_tile
// when you already have the pair.
bool find_tertiary_tile(const Landscape& landscape,
                        int tile_type, int tertiary_index,
                        uint8_t& out_x, uint8_t& out_y);
