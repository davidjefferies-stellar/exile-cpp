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

    int range_start = tertiary_ranges[tile_type];
    int range_end   = tertiary_ranges[tile_type + 1];
    int found = -1;
    for (int i = range_start; i < range_end; i++) {
        if (tertiary_objects_x_data[i] == tile_x) { found = i; break; }
    }

    if (found < 0) {
        r.tile_and_flip = feature_tiles_table[tile_type] | (raw & TileFlip::MASK);
        return r;
    }

    r.tertiary_index = found;
    // Data/type offsets: 8-bit modular adds against signed offset tables,
    // exactly matching &1749-&1752 in the disassembly.
    r.data_offset = static_cast<uint8_t>(
        found + static_cast<int8_t>(tertiary_data_offset[tile_type]));
    r.type_offset = static_cast<uint8_t>(
        r.data_offset + static_cast<int8_t>(tertiary_type_offset[tile_type]));
    r.tile_and_flip = tertiary_objects_tile_and_flip_data[found];
    return r;
}

bool find_tertiary_tile(const Landscape& landscape,
                        int tile_type, int tertiary_index,
                        uint8_t& out_x, uint8_t& out_y) {
    if (tile_type < 0 || tile_type > 8) return false;
    if (tertiary_index < tertiary_ranges[tile_type] ||
        tertiary_index >= tertiary_ranges[tile_type + 1]) return false;
    uint8_t tx = tertiary_objects_x_data[tertiary_index];
    for (int y = 0; y < 256; y++) {
        uint8_t raw = landscape.get_tile(tx, static_cast<uint8_t>(y));
        if ((raw & TileFlip::TYPE_MASK) != tile_type) continue;
        ResolvedTile r = resolve_tile_with_tertiary(
            landscape, tx, static_cast<uint8_t>(y));
        if (r.tertiary_index != tertiary_index) continue;
        out_x = tx;
        out_y = static_cast<uint8_t>(y);
        return true;
    }
    return false;
}

bool resolve_data_offset_to_tile(const Landscape& landscape,
                                 uint8_t data_offset,
                                 uint8_t& out_x, uint8_t& out_y) {
    if (data_offset == 0) return false;
    // Walk every tile-type-with-tertiary (0..8) and its tertiary-index range.
    // For each index i, the forward map is data_offset = i + tertiary_data_
    // offset[T] (mod 256); finding the (T, i) pair whose modular sum equals
    // the target gives us tile_x from tertiary_objects_x_data. tile_y is
    // recovered by scanning the landscape column for a matching tile type.
    //
    // Validation: tertiary_objects_x_data has duplicate x's in the same
    // range — only the LOWEST matching index is ever reached by the 6502's
    // forward resolver (first-match-wins at resolve_tile_with_tertiary).
    // A column-scan without validation would attribute the real tile to
    // the higher-index duplicate too, producing spurious wires. So after
    // locating a candidate (tx, y) we run the forward resolver and keep
    // it only when its tertiary_index matches THIS i — which cleanly
    // filters out dead duplicates and unreachable entries.
    for (int T = 0; T <= 8; T++) {
        int range_start = tertiary_ranges[T];
        int range_end   = tertiary_ranges[T + 1];
        for (int i = range_start; i < range_end; i++) {
            uint8_t off = static_cast<uint8_t>(
                i + static_cast<int8_t>(tertiary_data_offset[T]));
            if (off != data_offset) continue;
            uint8_t tx = tertiary_objects_x_data[i];
            for (int y = 0; y < 256; y++) {
                uint8_t raw =
                    landscape.get_tile(tx, static_cast<uint8_t>(y));
                if ((raw & TileFlip::TYPE_MASK) != T) continue;
                ResolvedTile r = resolve_tile_with_tertiary(
                    landscape, tx, static_cast<uint8_t>(y));
                if (r.tertiary_index != i) continue;
                out_x = tx;
                out_y = static_cast<uint8_t>(y);
                return true;
            }
        }
    }
    return false;
}
