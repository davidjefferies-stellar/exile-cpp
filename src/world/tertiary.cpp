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
