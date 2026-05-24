#include "world/tertiary.h"
#include "objects/object_tables.h"
#include "core/types.h"

ResolvedTile resolve_tile_with_tertiary(const Landscape& landscape,
                                        uint8_t tile_x, uint8_t tile_y) {
    ResolvedTile r{};
    r.tertiary_index = -1;
    r.data_offset = 0;
    r.type_offset = 0;

    uint8_t raw = landscape.get_tile(tile_x, tile_y);
    uint8_t tile_type = raw & TileFlip::TYPE_MASK;
    r.raw_tile_type = tile_type;

    // Only tile types INVISIBLE_SWITCH..SWITCH (0x00..0x08 inclusive)
    // participate in tertiary resolution — they're the "check_tertiary"
    // markers plus the door/switch/transporter redirects.
    if (tile_type > static_cast<uint8_t>(TileType::SWITCH)) {
        r.tile_and_flip = raw;
        return r;
    }

    // Per-cell tertiary index, populated by Landscape::bake_tertiary_lookup
    // or loaded from the map file. The editor will eventually route
    // mutations through Landscape::set_tertiary_index_at, so this is
    // the single authoritative mapping from (x, y) -> entry index.
    uint16_t cell_idx = landscape.tertiary_index_at(tile_x, tile_y);
    if (cell_idx == Landscape::NO_TERTIARY) {
        r.tile_and_flip = feature_tiles_table[tile_type] | (raw & TileFlip::MASK);
        return r;
    }

    // After Option B, all three legacy fields collapse onto the same
    // entry index — there's one TertiaryEntry per cell carrying its
    // own data byte, type byte, and tile_and_flip. Callers that used
    // to read the static tertiary_objects_*_data arrays via separate
    // offsets now go through ObjectManager::tertiary_*_byte(idx) or
    // landscape.tertiary_entry(idx).tile_and_flip.
    int idx = static_cast<int>(cell_idx);
    r.tertiary_index = idx;
    r.data_offset    = idx;
    r.type_offset    = idx;
    r.tile_and_flip  = landscape.tertiary_entry(idx).tile_and_flip;
    return r;
}

// Both reverse lookups have the same shape after Option B: scan the
// per-cell tertiary index array for the cell pointing at the given
// entry. find_tertiary_tile / resolve_data_offset_to_tile take
// different "find what" arguments but the search is identical.
static bool find_cell_for_entry(const Landscape& landscape, int target_idx,
                                 uint8_t& out_x, uint8_t& out_y) {
    if (target_idx <= 0) return false;
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            uint16_t cell = landscape.tertiary_index_at(
                static_cast<uint8_t>(x), static_cast<uint8_t>(y));
            if (cell == static_cast<uint16_t>(target_idx)) {
                out_x = static_cast<uint8_t>(x);
                out_y = static_cast<uint8_t>(y);
                return true;
            }
        }
    }
    return false;
}

bool find_tertiary_tile(const Landscape& landscape,
                        int /*tile_type*/, int tertiary_index,
                        uint8_t& out_x, uint8_t& out_y) {
    return find_cell_for_entry(landscape, tertiary_index, out_x, out_y);
}

// Reverse-map a 6502 data_offset to a world cell. Match the column by
// per-cell tertiary_source_idx (set by bake) rather than raw tile type
// — door / switch overlays often sit over INVISIBLE_SWITCH base tiles.
bool resolve_data_offset_to_tile(const Landscape& landscape,
                                 uint8_t data_offset,
                                 uint8_t& out_x, uint8_t& out_y) {
    if (data_offset == 0) return false;
    for (int range_idx = 0; range_idx <= 8; ++range_idx) {
        uint8_t source_idx = static_cast<uint8_t>(
            data_offset - tertiary_data_offset[range_idx]);
        if (source_idx <  tertiary_ranges[range_idx]) continue;
        if (source_idx >= tertiary_ranges[range_idx + 1]) continue;
        uint8_t target_x = tertiary_objects_x_data[source_idx];
        for (int y = 0; y < 256; ++y) {
            if (landscape.tertiary_source_idx_at(
                    target_x, static_cast<uint8_t>(y)) == source_idx) {
                out_x = target_x;
                out_y = static_cast<uint8_t>(y);
                return true;
            }
        }
        out_x = target_x;
        out_y = 0;
        return true;
    }
    return false;
}
