#pragma once
#include <cstdint>
#include "core/types.h"

// Fallback tile types for CHECK_TERTIARY_OBJECT_RANGE_N tiles (0x00-0x08)
// when no tertiary object matches at a given x. Port of &117c.
static constexpr uint8_t feature_tiles_table[9] = {
    0x1b, 0x5a, 0x19, 0x19, 0x1e, 0x13, 0x24, 0x2c, 0x19,
};

// Per-cell tertiary state — diverges from 6502 shared-entry lookup so
// stacked/shared-key doors mutate independently. Fields mirror the
// 6502's three parallel arrays at &06ee (tile_and_flip),
// &0986 (data, bit 7 = needs primary spawn), &0a71 (type for FROM_*).
struct TertiaryEntry {
    uint8_t tile_and_flip = 0;
    uint8_t data          = 0;
    uint8_t type          = 0;
};

// 256x256 grid baked at init from &178d-&19a6; after that get_tile() is
// a plain array read. Two equivalent bake paths (pseudo-6502 in
// landscape.cpp, native C++ in landscape_cpp.cpp) selectable via
// set_use_cpp_impl — MUST produce byte-identical output.
class Landscape {
public:
    static constexpr int WORLD_SIZE = 256;
    static constexpr int TILE_COUNT = WORLD_SIZE * WORLD_SIZE;  // 65 536 bytes

    // Per-cell tertiary copies; >5000 matched cells observed in
    // practice. 16384 leaves room below NO_TERTIARY (0xffff).
    static constexpr int TERTIARY_CAPACITY = 16384;

    // Sentinel for "this cell has no tertiary". 0xffff because the
    // entry index is uint16_t — capacity is up to TERTIARY_CAPACITY.
    static constexpr uint16_t NO_TERTIARY = 0xffff;

    // Procedural fill of the grid. Must be called once before get_tile;
    // also rebuilds the tertiary lookup since it derives from tiles.
    void bake();

    // Resolve each cell's tertiary entry under 6502 x-in-range lookup.
    // Stored per-cell so the editor can override and resolve is O(1).
    void bake_tertiary_lookup();

    // Persist / restore the grid (16-byte header + 64K tile bytes).
    // Load failure leaves grid untouched so caller can fall back to bake().
    bool save_to_file(const char* path) const;
    bool load_from_file(const char* path);

    // Get tile at world coordinates. Returns tile_type | flip_flags.
    // Reads directly from the in-memory grid populated by bake().
    uint8_t get_tile(uint8_t tile_x, uint8_t tile_y) const {
        return tiles_[(static_cast<int>(tile_y) << 8) | tile_x];
    }

    // Permanent set + refresh attached tertiary (tile type drives which
    // entry the cell points at).
    void set_tile(uint8_t tile_x, uint8_t tile_y, uint8_t tile);

    // Per-cell tertiary entry index. NO_TERTIARY (0xffff) means no
    // tertiary attached. The index points into the flat
    // tertiary_entries_ list; see tertiary_entry() to read the entry's
    // tile_and_flip / data / type.
    uint16_t tertiary_index_at(uint8_t tile_x, uint8_t tile_y) const {
        return tertiary_idx_[(static_cast<int>(tile_y) << 8) | tile_x];
    }
    void set_tertiary_index_at(uint8_t tile_x, uint8_t tile_y, uint16_t idx) {
        tertiary_idx_[(static_cast<int>(tile_y) << 8) | tile_x] = idx;
    }

    // Port of 6502 &00 `tile_was_from_map_data`. Set by &17d6
    // get_tile_from_map_data; consumed by star-field spawn gate at &26df
    // ("is this cell inside spaceship/lab?").
    bool tile_from_map_data(uint8_t tile_x, uint8_t tile_y) const {
        return from_map_data_[(static_cast<int>(tile_y) << 8) | tile_x] != 0;
    }

    // map_overlay_data[0..1023] index (only when tile_from_map_data).
    // Used by grid overlay to label each authored cell with its ROM slot.
    uint16_t map_data_offset(uint8_t tile_x, uint8_t tile_y) const {
        return map_data_offset_[(static_cast<int>(tile_y) << 8) | tile_x];
    }

    // From-map cell shares its map_data_offset with another from-map
    // cell. Grid overlay paints red (alias) vs cyan (unique).
    bool map_data_offset_aliased(uint8_t tile_x, uint8_t tile_y) const {
        int idx = (static_cast<int>(tile_y) << 8) | tile_x;
        if (!from_map_data_[idx]) return false;
        return map_data_offset_count_[map_data_offset_[idx]] > 1;
    }

    // CHECK_TERTIARY cell sharing its source-table entry with another
    // cell (6502 x-in-range would alias). Bake gives each its own copy,
    // but the shared source historically aliased edits — worth flagging.
    bool tertiary_source_aliased(uint8_t tile_x, uint8_t tile_y) const {
        int idx = (static_cast<int>(tile_y) << 8) | tile_x;
        uint16_t src = tertiary_source_idx_[idx];
        if (src == NO_TERTIARY) return false;
        return tertiary_source_count_[src] > 1;
    }

    // ROM source-table index (0..254) for the tertiary attached to this
    // cell, or NO_TERTIARY. BBC save loaders use this to map the 6502's
    // 255-entry shared tertiary_objects_data array onto our per-cell
    // entries (multiple cells share a source).
    uint16_t tertiary_source_idx_at(uint8_t tile_x, uint8_t tile_y) const {
        return tertiary_source_idx_[(static_cast<int>(tile_y) << 8) | tile_x];
    }

    // SWITCH cell sharing X column — runtime state aliases regardless
    // of whether the tertiary x-scan finds a source entry.
    bool switch_x_aliased(uint8_t tile_x, uint8_t tile_y) const {
        int idx = (static_cast<int>(tile_y) << 8) | tile_x;
        if (!from_map_data_[idx]) return false;
        // "Switch" = the cell's tertiary resolves to a SWITCH-typed
        // tile_and_flip — covers both raw 0x08 SWITCH cells and the
        // redirect cells (e.g. raw METAL_DOOR with tile_and_flip 0x08).
        uint16_t t_idx = tertiary_idx_[idx];
        if (t_idx == NO_TERTIARY || t_idx >= n_tertiary_entries_) return false;
        uint8_t resolved = tertiary_entries_[t_idx].tile_and_flip
                           & TileFlip::TYPE_MASK;
        if (resolved != static_cast<uint8_t>(TileType::SWITCH)) return false;
        return switch_x_count_[tile_x] > 1;
    }

    // Tertiary entry table accessors. Bounds-checked; out-of-range
    // reads return a default-constructed entry (zeros), out-of-range
    // writes are no-ops, so callers don't have to guard NO_TERTIARY
    // explicitly.
    int tertiary_count() const { return n_tertiary_entries_; }
    const TertiaryEntry& tertiary_entry(int idx) const {
        static const TertiaryEntry kZero{};
        return (idx >= 0 && idx < n_tertiary_entries_)
            ? tertiary_entries_[idx] : kZero;
    }
    TertiaryEntry& tertiary_entry_mut(int idx) {
        static TertiaryEntry kZeroMut{};
        if (idx < 0 || idx >= n_tertiary_entries_) {
            kZeroMut = {};                              // reset on miss
            return kZeroMut;
        }
        return tertiary_entries_[idx];
    }
    // Allocate a new entry, returning its index, or -1 if at capacity.
    // Called by the bake and by future editor "add tertiary" actions.
    int add_tertiary_entry(const TertiaryEntry& e) {
        if (n_tertiary_entries_ >= TERTIARY_CAPACITY) return -1;
        tertiary_entries_[n_tertiary_entries_] = e;
        return n_tertiary_entries_++;
    }

    // Toggle between the pseudo-6502 reference and the native C++
    // rewrite at bake time. The two are intended to be functionally
    // identical; see set_use_cpp_impl notes above.
    void set_use_cpp_impl(bool b) { use_cpp_impl_ = b; }
    bool use_cpp_impl() const { return use_cpp_impl_; }

    // Procedural output ignoring the map-overlay gate. "Algo only"
    // overlay uses this to show the cavern under authored interiors.
    uint8_t compute_algo_tile(uint8_t tile_x, uint8_t tile_y) const;

private:
    uint8_t  tiles_[TILE_COUNT]        = {};
    // Per-cell tertiary index. Values 0..n_tertiary_entries_-1 point
    // into tertiary_entries_; NO_TERTIARY means "no tertiary at this
    // cell". Populated by bake_tertiary_lookup or load_from_file.
    uint16_t tertiary_idx_[TILE_COUNT] = {};
    // 1 = bake reached the get_tile_from_map_data branch (&17d6) for
    // this cell. Set during bake() via the mutable flag below.
    uint8_t  from_map_data_[TILE_COUNT] = {};
    // Index into map_overlay_data[] for cells where from_map_data_ == 1;
    // 0 for procedural cells. Populated alongside the flag at bake time.
    uint16_t map_data_offset_[TILE_COUNT] = {};
    // How many cells reference each of the 1024 ROM slots. Rebuilt at the
    // end of bake() / load_from_file(); count > 1 marks an alias for the
    // grid overlay's red highlight.
    uint16_t map_data_offset_count_[1024] = {};
    // Per-cell source-tertiary index (`found` in tertiary_objects_x_data).
    // uint16_t lets NO_TERTIARY (0xffff) double as absent marker.
    uint16_t tertiary_source_idx_[TILE_COUNT] = {};
    // Count of cells per source index — count > 1 means two map cells
    // emit the same tertiary marker.
    uint16_t tertiary_source_count_[256] = {};
    // SWITCH count per X column. >1 means switches share runtime state
    // regardless of the tertiary scan. Rebuilt at bake / load.
    uint16_t switch_x_count_[256] = {};
    // Output channels for the bake helpers (one map-data return path);
    // bake reads after each cell. Mutable to keep helpers const.
    mutable bool last_bake_was_map_data_ = false;
    mutable uint16_t last_bake_map_data_offset_ = 0;
    // Flat tertiary entries (one per CHECK_TERTIARY cell after bake;
    // editor may grow/shrink within TERTIARY_CAPACITY).
    TertiaryEntry tertiary_entries_[TERTIARY_CAPACITY] = {};
    int           n_tertiary_entries_ = 0;
    bool use_cpp_impl_ = false;

    // Recompute map_data_offset_count_ from the per-cell tables. Cheap
    // (65 536 cell scan), called once per bake / load.
    void rebuild_map_data_offset_counts();

    // ---- Pseudo-6502 reference (landscape.cpp) ----
    uint8_t bake_tile_pseudo_6502(uint8_t tile_x, uint8_t tile_y) const;
    uint8_t get_tile_from_algorithm(uint8_t tile_x, uint8_t tile_y, uint8_t f1) const;
    uint8_t get_tile_for_surface(uint8_t tile_x, uint8_t f1) const;
    uint8_t handle_sloping_passage(uint8_t tile_x, uint8_t tile_y, uint8_t f1) const;
    uint8_t leave_with_earth_or_stone(uint8_t f1) const;
    uint8_t leave_with_tile_from_table(uint8_t index) const;

    // Sloping passage detection. Returns is_passage=true if passage found.
    // y = 0 if middle, 2-5 if edge (slope type for rotation lookup).
    struct SlopeResult {
        bool is_passage;
        uint8_t y;
    };
    SlopeResult calculate_slope_function(uint8_t tile_x, uint8_t tile_y) const;
    static uint8_t recalc_f1(uint8_t tile_x, uint8_t tile_y);

    // ---- Native C++ rewrite (landscape_cpp.cpp) ----
    uint8_t bake_tile_cpp(uint8_t tile_x, uint8_t tile_y) const;
};
