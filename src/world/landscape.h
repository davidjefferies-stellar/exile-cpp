#pragma once
#include <cstdint>
#include "core/types.h"

// Fallback tile types for CHECK_TERTIARY_OBJECT_RANGE_N tiles (0x00-0x08)
// when no tertiary object matches at a given x. Port of &117c.
static constexpr uint8_t feature_tiles_table[9] = {
    0x1b, 0x5a, 0x19, 0x19, 0x1e, 0x13, 0x24, 0x2c, 0x19,
};

// Per-cell tertiary state. Each (x, y) cell whose tile_type is a
// CHECK_TERTIARY_OBJECT_RANGE_N marker (0x00..0x08) gets its OWN entry;
// the bake duplicates source-table state when multiple cells used to
// share an entry under the 6502's x-only-in-range lookup. That makes
// stacked / shared-key doors / switches independently mutable, which
// the editor needs.
//
// Fields mirror the three pieces of state the 6502 stored in three
// parallel arrays (tertiary_objects_tile_and_flip_data,
// tertiary_objects_data_bytes, tertiary_objects_type_data):
//   tile_and_flip — the resolved tile to render at this cell (e.g.
//                   METAL_DOOR with flip bits). Was at &06ee.
//   data          — mutable runtime state byte (door open / locked /
//                   colour, switch effect-id + state, creature count,
//                   etc.) and bit 7 = "needs primary spawn". Was at
//                   &0986.
//   type          — object spawn type for FROM_DATA / FROM_TYPE
//                   tiles. Zero for entries that don't use it. Was
//                   at &0a71.
struct TertiaryEntry {
    uint8_t tile_and_flip = 0;
    uint8_t data          = 0;
    uint8_t type          = 0;
};

// 256×256 landscape backed by an in-memory tile grid. The grid is
// pre-populated at init from the procedural algorithm at &178d-&19a6;
// after that, get_tile() is a pure array read. set_tile() lets a
// future editor mutate cells without re-running the algorithm.
//
// The procedural generator is preserved as a "bake" path:
//   * pseudo-6502 (landscape.cpp): faithful Alu-emulating port. Tracks
//     A/C explicitly so each line maps 1:1 to the disassembly.
//   * native C++ (landscape_cpp.cpp): same algorithm in plain C++.
// Both run only at bake time; the toggle (`set_use_cpp_impl`) selects
// which generator to seed the grid from. They MUST produce byte-
// identical output for every (x, y); the toggle exists so the C++
// rewrite can be A/B-tested against the reference and so future tweaks
// to either path have a known-good baseline to diff against.
class Landscape {
public:
    static constexpr int WORLD_SIZE = 256;
    static constexpr int TILE_COUNT = WORLD_SIZE * WORLD_SIZE;  // 65 536 bytes

    // Capacity for the flat tertiary entry array. Each cell whose tile
    // type is CHECK_TERTIARY (0x00..0x08) and whose X matches a source
    // row gets its own copy. The procedural generator emits thousands
    // of CHECK_TERTIARY cells across the 256×256 grid; matched cells
    // observed at >5000 in practice. 16384 is a comfortable headroom
    // and still leaves plenty of room below the NO_TERTIARY sentinel
    // (0xffff) for editor-added entries.
    static constexpr int TERTIARY_CAPACITY = 16384;

    // Sentinel for "this cell has no tertiary". 0xffff because the
    // entry index is uint16_t — capacity is up to TERTIARY_CAPACITY.
    static constexpr uint16_t NO_TERTIARY = 0xffff;

    // Run the procedural algorithm over every (x, y) and store the
    // result in the in-memory grid. Must be called once at init before
    // any get_tile() call. Subsequent calls re-bake (e.g. after
    // toggling use_cpp_impl). Also rebuilds the tertiary lookup cache
    // since it derives from the tile grid.
    void bake();

    // Walk the 256×256 grid and resolve which entry in the static
    // tertiary tables (if any) each cell maps to under the 6502's x-
    // only-in-range lookup. Stored per-cell so the editor can later
    // override individual cells, and so resolve_tile_with_tertiary is
    // a cheap O(1) read instead of a per-frame range scan.
    void bake_tertiary_lookup();

    // Persist / restore the in-memory grid to/from a binary file. The
    // format is a 16-byte header (magic "EXILEMAP", version, world
    // dimensions) followed by the raw 65 536 tile bytes — no
    // compression. Returns true on success; on load failure the grid
    // is left untouched so the caller can fall back to bake().
    bool save_to_file(const char* path) const;
    bool load_from_file(const char* path);

    // Get tile at world coordinates. Returns tile_type | flip_flags.
    // Reads directly from the in-memory grid populated by bake().
    uint8_t get_tile(uint8_t tile_x, uint8_t tile_y) const {
        return tiles_[(static_cast<int>(tile_y) << 8) | tile_x];
    }

    // Set tile at world coordinates. For the upcoming editor and any
    // gameplay event that needs to permanently alter the world. Also
    // refreshes the tertiary lookup for this cell, since the cell's
    // tile type drives whether (and which) tertiary entry it points
    // at.
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

    // Was this cell sourced from the hand-authored map_overlay_data at
    // bake time? Port of the 6502's `tile_was_from_map_data` flag at
    // &00, set by `get_tile_from_map_data` at &17d6 and consumed by the
    // star-field spawn gate at &26df. The two mainline uses of this
    // signal are both about "is this cell inside the player's spaceship
    // wreck or Triax's lab?" — those interiors are placed by map data,
    // the procedural generator never produces them.
    bool tile_from_map_data(uint8_t tile_x, uint8_t tile_y) const {
        return from_map_data_[(static_cast<int>(tile_y) << 8) | tile_x] != 0;
    }

    // Index into map_overlay_data[0..1023] that the cell was read from.
    // Only meaningful when tile_from_map_data() returns true; returns 0
    // for procedural cells. Used by the grid debug overlay to label
    // each authored cell with its slot in the 1 KB ROM table.
    uint16_t map_data_offset(uint8_t tile_x, uint8_t tile_y) const {
        return map_data_offset_[(static_cast<int>(tile_y) << 8) | tile_x];
    }

    // True when the cell is from map data AND another from-map-data cell
    // shares the same map_data_offset. Drives the grid overlay's red
    // (alias) vs cyan (unique) colour split so authors can spot two
    // world tiles backed by the same ROM slot at a glance.
    bool map_data_offset_aliased(uint8_t tile_x, uint8_t tile_y) const {
        int idx = (static_cast<int>(tile_y) << 8) | tile_x;
        if (!from_map_data_[idx]) return false;
        return map_data_offset_count_[map_data_offset_[idx]] > 1;
    }

    // True when this cell is a CHECK_TERTIARY tile that resolved to a
    // source-table entry shared with at least one other cell (the 6502's
    // x-only-in-range scan would have returned the same `found` index).
    // The bake gives each cell its own TertiaryEntry copy, but the
    // shared source means edits to "the door at slot N" historically
    // affected every cell aliasing that slot — worth surfacing.
    bool tertiary_source_aliased(uint8_t tile_x, uint8_t tile_y) const {
        int idx = (static_cast<int>(tile_y) << 8) | tile_x;
        uint16_t src = tertiary_source_idx_[idx];
        if (src == NO_TERTIARY) return false;
        return tertiary_source_count_[src] > 1;
    }

    // True when this cell is a SWITCH (tile_type 0x08) AND another
    // SWITCH lives in the same world X column. Surfaces the simpler,
    // type-only flavour of switch aliasing — independent of whether the
    // tertiary x-only-in-range scan actually finds a source entry —
    // because two switches sharing an X share state at runtime even when
    // they have no tertiary attached.
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

    // Returns what the procedural generator WOULD produce for (x, y)
    // unconditionally — bypasses the region check that normally routes
    // map-overlay cells to the authored ROM table. Used by the "Algo
    // only" debug overlay so authored interiors get replaced with the
    // cavern that would have been there underneath.
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
    // Per-cell source-tertiary index — the `found` slot in the static
    // tertiary_objects_x_data table. NO_TERTIARY for cells with no
    // tertiary attached. Source indices fit in [0, 255] but we use
    // uint16_t so NO_TERTIARY (0xffff) doubles as the absent marker.
    // Initialised to 0 by the brace-init; bake_tertiary_lookup /
    // load_from_file overwrite every cell before any reader runs.
    uint16_t tertiary_source_idx_[TILE_COUNT] = {};
    // Count of cells per source index — count > 1 means two map cells
    // emit the same tertiary marker.
    uint16_t tertiary_source_count_[256] = {};
    // Count of SWITCH (tile_type 0x08) cells per X column. > 1 means two
    // switches share that X — they share runtime state regardless of
    // whether the x-only-in-range scan finds a tertiary entry. Rebuilt
    // from the tile grid in bake() / load_from_file(); kept consistent
    // by set_tile().
    uint16_t switch_x_count_[256] = {};
    // Output channels for the bake helpers. They set the flag to true
    // and write the offset on the single map-data return path; bake()
    // reads them after each cell and copies into the per-cell arrays
    // above. Mutable so the helpers can stay const-qualified.
    mutable bool last_bake_was_map_data_ = false;
    mutable uint16_t last_bake_map_data_offset_ = 0;
    // Flat list of tertiary entries. The bake sizes this from the
    // existing static ROM tables (one entry per CHECK_TERTIARY cell);
    // the editor will grow / shrink it within TERTIARY_CAPACITY.
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
