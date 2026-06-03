#include "world/landscape.h"
#include "map_overlay.h"
#include "objects/object_tables.h"
#include "core/types.h"
#include <cstring>
#include <fstream>

// ============================================================================
// Lookup tables from the disassembly, transcribed exactly.
// ============================================================================

static constexpr uint8_t earth_tiles_rotation_table[] = {
    0x19, 0x2d, 0xed, 0x6d, 0xad, 0x2d, 0xed, 0x5e, 0x9e,
};

static constexpr uint8_t tile_rotations_table[] = { // at &1158, indexed [slope_type - 2]
    0x00, 0xc0, 0x80, 0x40,
};

static constexpr uint8_t slope_tiles_table[] = { // at &115c
    0x2e, 0x2f, 0x2e, 0x23,
};

static constexpr uint8_t sloping_passage_feature_tiles_table[] = { // at &1160
    0x06, 0x04, 0x06, 0x04, 0x07, 0x05, 0x05, 0x06,
};

// Combined table at &114f for leave_with_tile_from_table lookups.
static constexpr uint8_t tiles_table[] = {
    0x19, 0x2d, 0xed, 0x6d, 0xad, 0x2d, 0xed, 0x5e, 0x9e, // earth_tiles_rotation (0x00-0x08)
    0x00, 0xc0, 0x80, 0x40,                                 // tile_rotations (0x09-0x0c)
    0x2e, 0x2f, 0x2e, 0x23,                                 // slope_tiles (0x0d-0x10)
    0x06, 0x04, 0x06, 0x04, 0x07, 0x05, 0x05, 0x06,         // sloping_passage_features (0x11-0x18)
    0x19, 0x2c, 0x19, 0x2b,                                 // surface_features (0x19-0x1c)
    0x00, 0x01, 0x02, 0x03, 0x1a, 0x21, 0x09, 0x9b,         // horizontal_passage_features (0x1d-0x2c)
    0x12, 0x10, 0x60, 0x2b, 0x0f, 0x4f, 0x04, 0x0a,
};

// ============================================================================
// 6502 ALU helpers - track accumulator and carry flag precisely
// ============================================================================

struct Alu {
    uint8_t a = 0;
    uint8_t c = 0; // carry flag: 0 or 1

    void lda(uint8_t v) { a = v; } // LDA: doesn't affect carry
    void eor(uint8_t v) { a ^= v; } // EOR: doesn't affect carry
    void and_(uint8_t v) { a &= v; } // AND: doesn't affect carry
    void ora(uint8_t v) { a |= v; } // ORA: doesn't affect carry

    void lsr() { c = a & 1; a >>= 1; } // LSR: bit 0 -> carry
    void asl() { c = (a >> 7) & 1; a <<= 1; } // ASL: bit 7 -> carry

    void adc(uint8_t v) { // ADC: a = a + v + carry
        uint16_t s = static_cast<uint16_t>(a) + v + c;
        a = static_cast<uint8_t>(s);
        c = static_cast<uint8_t>(s >> 8);
    }

    void sbc(uint8_t v) { // SBC: a = a - v - (1 - carry)
        uint16_t s = static_cast<uint16_t>(a) - v - (1 - c);
        a = static_cast<uint8_t>(s);
        c = (s <= 0xFF) ? 1 : 0; // carry set if no borrow
    }

    void sec() { c = 1; }
    void clc() { c = 0; }

    void rol() { // ROL: shift left through carry
        uint8_t new_c = (a >> 7) & 1;
        a = (a << 1) | c;
        c = new_c;
    }

    // CMP sets carry if a >= v (borrow didn't occur)
    void cmp(uint8_t v) { c = (a >= v) ? 1 : 0; }
};

// bake — fill tiles_ from the procedural algorithm. One-shot at init;
// re-runs on use_cpp_impl_ toggle.

void Landscape::bake() {
    for (int y = 0; y < WORLD_SIZE; ++y) {
        for (int x = 0; x < WORLD_SIZE; ++x) {
            // Reset the bake helper's "took map_data path" flag for this
            // cell. The helper sets it true if it reached the &17d6
            // branch, leaves it false otherwise.
            last_bake_was_map_data_ = false;
            last_bake_map_data_offset_ = 0;
            uint8_t tile = use_cpp_impl_
                ? bake_tile_cpp(static_cast<uint8_t>(x), static_cast<uint8_t>(y))
                : bake_tile_pseudo_6502(static_cast<uint8_t>(x),
                                         static_cast<uint8_t>(y));
            tiles_[(y << 8) | x] = tile;
            from_map_data_[(y << 8) | x] = last_bake_was_map_data_ ? 1 : 0;
            map_data_offset_[(y << 8) | x] = last_bake_map_data_offset_;
        }
    }
    rebuild_map_data_offset_counts();
    bake_tertiary_lookup();
}

void Landscape::rebuild_map_data_offset_counts() {
    for (int i = 0; i < 1024; ++i) map_data_offset_count_[i] = 0;
    for (int i = 0; i < TILE_COUNT; ++i) {
        if (!from_map_data_[i]) continue;
        uint16_t off = map_data_offset_[i];
        if (off < 1024) map_data_offset_count_[off]++;
    }
}

// Resolve every CHECK_TERTIARY cell into its OWN entry. Diverges from
// 6502's shared x-scan so stacked/shared-key doors hold independent
// state. Source arrays: &05ef / &06ee / &0986 / &0a71.
void Landscape::bake_tertiary_lookup() {
    // Slot 0 is a sentinel so "data_offset > 0" tests still mean "has
    // tertiary"; allocate from 1. NO_TERTIARY (0xffff) flags per-cell.
    n_tertiary_entries_ = 1;
    tertiary_entries_[0] = TertiaryEntry{};
    for (int i = 0; i < 256; ++i) tertiary_source_count_[i] = 0;
    for (int i = 0; i < 256; ++i) switch_x_count_[i] = 0;
    for (int y = 0; y < WORLD_SIZE; ++y) {
        for (int x = 0; x < WORLD_SIZE; ++x) {
            uint8_t tile = tiles_[(y << 8) | x];
            uint8_t type = tile & TileFlip::TYPE_MASK;
            uint16_t resolved = NO_TERTIARY;
            uint16_t source = NO_TERTIARY;
            if (type <= static_cast<uint8_t>(TileType::SWITCH)) {
                int rs = tertiary_ranges[type];
                int re = tertiary_ranges[type + 1];
                int found = -1;
                for (int i = rs; i < re; ++i) {
                    if (tertiary_objects_x_data[i] ==
                        static_cast<uint8_t>(x)) {
                        found = i;
                        break;
                    }
                }
                if (found >= 0) {
                    // Counts both direct and redirect switches via
                    // resolved tile_and_flip == SWITCH. Gated on
                    // from_map_data so marker cells don't inflate count.
                    uint8_t resolved_type =
                        tertiary_objects_tile_and_flip_data[found]
                        & TileFlip::TYPE_MASK;
                    if (resolved_type == static_cast<uint8_t>(TileType::SWITCH) &&
                        from_map_data_[(y << 8) | x]) {
                        switch_x_count_[x]++;
                    }
                    TertiaryEntry e;
                    e.tile_and_flip = tertiary_objects_tile_and_flip_data[found];
                    // Translate source-table indices using the per-type
                    // addends (&05dd / &05e6) — same arithmetic as the
                    // pre-bake resolve_tile_with_tertiary did at runtime.
                    int data_off = static_cast<uint8_t>(
                        found + static_cast<int8_t>(tertiary_data_offset[type]));
                    int type_off = static_cast<uint8_t>(
                        data_off + static_cast<int8_t>(tertiary_type_offset[type]));
                    if (data_off > 0 &&
                        data_off < static_cast<int>(sizeof(tertiary_objects_data_bytes))) {
                        e.data = tertiary_objects_data_bytes[data_off];
                    }
                    if (type_off > 0 &&
                        type_off < static_cast<int>(sizeof(tertiary_objects_type_data))) {
                        e.type = tertiary_objects_type_data[type_off];
                    }
                    int new_idx = add_tertiary_entry(e);
                    if (new_idx >= 0) resolved = static_cast<uint16_t>(new_idx);
                    source = static_cast<uint16_t>(found);
                    if (found < 256) tertiary_source_count_[found]++;
                }
            }
            tertiary_idx_[(y << 8) | x] = resolved;
            tertiary_source_idx_[(y << 8) | x] = source;
        }
    }
}

// ============================================================================
// set_tile mutator. Refreshes attached tertiary on type change; existing
// entries are NOT GC'd — editor's responsibility.
void Landscape::set_tile(uint8_t tile_x, uint8_t tile_y, uint8_t tile) {
    int idx = (static_cast<int>(tile_y) << 8) | tile_x;
    // Drop the cell's old contribution to the source-index alias count
    // before we recompute. Editor changes that detach or repoint the
    // tertiary need to leave the count consistent for the alias overlay.
    {
        uint16_t old_src = tertiary_source_idx_[idx];
        if (old_src < 256 && tertiary_source_count_[old_src] > 0) {
            tertiary_source_count_[old_src]--;
        }
    }
    // Drop the cell's old contribution to the per-X switch count.
    // Mirrors the bake's gate: counted only while raw type was SWITCH
    // and a tertiary was attached.
    {
        uint8_t old_type = tiles_[idx] & TileFlip::TYPE_MASK;
        if (old_type == static_cast<uint8_t>(TileType::SWITCH) &&
            tertiary_source_idx_[idx] != NO_TERTIARY &&
            switch_x_count_[tile_x] > 0) {
            switch_x_count_[tile_x]--;
        }
    }
    tiles_[idx] = tile;
    uint8_t type = tile & TileFlip::TYPE_MASK;
    uint16_t resolved = NO_TERTIARY;
    uint16_t source   = NO_TERTIARY;
    if (type <= static_cast<uint8_t>(TileType::SWITCH)) {
        int rs = tertiary_ranges[type];
        int re = tertiary_ranges[type + 1];
        int found = -1;
        for (int i = rs; i < re; ++i) {
            if (tertiary_objects_x_data[i] == tile_x) { found = i; break; }
        }
        if (found >= 0) {
            TertiaryEntry e;
            e.tile_and_flip = tertiary_objects_tile_and_flip_data[found];
            // Same gate as bake_tertiary_lookup: cell counts as a
            // switch when its resolved tile_and_flip type is SWITCH
            // (covers direct switches AND redirect switches).
            uint8_t resolved_type =
                e.tile_and_flip & TileFlip::TYPE_MASK;
            if (resolved_type == static_cast<uint8_t>(TileType::SWITCH) &&
                from_map_data_[idx]) {
                switch_x_count_[tile_x]++;
            }
            int data_off = static_cast<uint8_t>(
                found + static_cast<int8_t>(tertiary_data_offset[type]));
            int type_off = static_cast<uint8_t>(
                data_off + static_cast<int8_t>(tertiary_type_offset[type]));
            if (data_off > 0 &&
                data_off < static_cast<int>(sizeof(tertiary_objects_data_bytes))) {
                e.data = tertiary_objects_data_bytes[data_off];
            }
            if (type_off > 0 &&
                type_off < static_cast<int>(sizeof(tertiary_objects_type_data))) {
                e.type = tertiary_objects_type_data[type_off];
            }
            int new_idx = add_tertiary_entry(e);
            if (new_idx >= 0) resolved = static_cast<uint16_t>(new_idx);
            source = static_cast<uint16_t>(found);
            if (found < 256) tertiary_source_count_[found]++;
        }
    }
    tertiary_idx_[idx] = resolved;
    tertiary_source_idx_[idx] = source;
}

// Map file: "EXILEMAP" magic, u16 version=1, u16 N entries, u16 w/h (256),
// 64K tile bytes row-major, 128K u16 tertiary indices, N*3 byte entries.
// Mismatched magic/version/dimensions reject cleanly; caller falls back.

namespace {
constexpr char     kMagic[8]   = { 'E','X','I','L','E','M','A','P' };
constexpr uint16_t kVersion    = 1;
constexpr int      kHeaderLen  = 16;
constexpr int      kIdxBytes   = Landscape::TILE_COUNT * 2;  // uint16_t per cell
constexpr int      kEntryBytes = 3;                          // bytes per TertiaryEntry
}

bool Landscape::save_to_file(const char* path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    uint16_t n = static_cast<uint16_t>(n_tertiary_entries_);
    uint8_t header[kHeaderLen] = {};
    std::memcpy(header, kMagic, 8);
    header[8]  = static_cast<uint8_t>(kVersion & 0xff);
    header[9]  = static_cast<uint8_t>((kVersion >> 8) & 0xff);
    header[10] = static_cast<uint8_t>(n & 0xff);
    header[11] = static_cast<uint8_t>((n >> 8) & 0xff);
    header[12] = static_cast<uint8_t>(WORLD_SIZE & 0xff);
    header[13] = static_cast<uint8_t>((WORLD_SIZE >> 8) & 0xff);
    header[14] = static_cast<uint8_t>(WORLD_SIZE & 0xff);
    header[15] = static_cast<uint8_t>((WORLD_SIZE >> 8) & 0xff);
    out.write(reinterpret_cast<const char*>(header), kHeaderLen);
    out.write(reinterpret_cast<const char*>(tiles_), TILE_COUNT);
    out.write(reinterpret_cast<const char*>(tertiary_idx_), kIdxBytes);
    // Each entry is 3 bytes packed; std::ofstream treats them as bytes.
    for (int i = 0; i < n_tertiary_entries_; ++i) {
        const TertiaryEntry& e = tertiary_entries_[i];
        uint8_t buf[3] = { e.tile_and_flip, e.data, e.type };
        out.write(reinterpret_cast<const char*>(buf), kEntryBytes);
    }
    return out.good();
}

bool Landscape::load_from_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;

    uint8_t header[kHeaderLen] = {};
    in.read(reinterpret_cast<char*>(header), kHeaderLen);
    if (in.gcount() != kHeaderLen) return false;
    if (std::memcmp(header, kMagic, 8) != 0) return false;

    uint16_t version = static_cast<uint16_t>(header[8] | (header[9] << 8));
    if (version != kVersion) return false;
    uint16_t n_entries = static_cast<uint16_t>(header[10] | (header[11] << 8));
    if (n_entries > TERTIARY_CAPACITY) return false;
    uint16_t w = static_cast<uint16_t>(header[12] | (header[13] << 8));
    uint16_t h = static_cast<uint16_t>(header[14] | (header[15] << 8));
    if (w != WORLD_SIZE || h != WORLD_SIZE) return false;

    // Scratch buffers — load nothing into the live state until every
    // section has been read successfully.
    uint8_t  tile_scratch[TILE_COUNT];
    uint16_t idx_scratch [TILE_COUNT];
    in.read(reinterpret_cast<char*>(tile_scratch), TILE_COUNT);
    if (in.gcount() != TILE_COUNT) return false;
    in.read(reinterpret_cast<char*>(idx_scratch), kIdxBytes);
    if (in.gcount() != kIdxBytes) return false;

    TertiaryEntry entry_scratch[TERTIARY_CAPACITY];
    for (int i = 0; i < n_entries; ++i) {
        uint8_t buf[3];
        in.read(reinterpret_cast<char*>(buf), kEntryBytes);
        if (in.gcount() != kEntryBytes) return false;
        entry_scratch[i].tile_and_flip = buf[0];
        entry_scratch[i].data          = buf[1];
        entry_scratch[i].type          = buf[2];
    }

    std::memcpy(tiles_,         tile_scratch, TILE_COUNT);
    std::memcpy(tertiary_idx_,  idx_scratch,  kIdxBytes);
    for (int i = 0; i < n_entries; ++i) tertiary_entries_[i] = entry_scratch[i];
    n_tertiary_entries_ = n_entries;

    // Recompute from_map_data_ by re-running bake (deterministic for
    // (x,y)). Hand-edited cells get the procedural answer — fine for
    // star-spawn suppression, avoids bumping format version.
    for (int y = 0; y < WORLD_SIZE; ++y) {
        for (int x = 0; x < WORLD_SIZE; ++x) {
            last_bake_was_map_data_ = false;
            last_bake_map_data_offset_ = 0;
            if (use_cpp_impl_) {
                (void)bake_tile_cpp(static_cast<uint8_t>(x),
                                     static_cast<uint8_t>(y));
            } else {
                (void)bake_tile_pseudo_6502(static_cast<uint8_t>(x),
                                             static_cast<uint8_t>(y));
            }
            from_map_data_[(y << 8) | x] = last_bake_was_map_data_ ? 1 : 0;
            map_data_offset_[(y << 8) | x] = last_bake_map_data_offset_;
        }
    }
    rebuild_map_data_offset_counts();

    // Re-derive tertiary_source_idx_ + count table from the loaded tiles.
    // The save format omits the source index (it's deterministic from
    // tile_type + x), so we replay the same x-only-in-range scan the bake
    // does — without recreating the live entries that we just loaded.
    for (int i = 0; i < 256; ++i) tertiary_source_count_[i] = 0;
    for (int y = 0; y < WORLD_SIZE; ++y) {
        for (int x = 0; x < WORLD_SIZE; ++x) {
            uint8_t tile = tiles_[(y << 8) | x];
            uint8_t type = tile & TileFlip::TYPE_MASK;
            uint16_t source = NO_TERTIARY;
            if (type <= static_cast<uint8_t>(TileType::SWITCH)) {
                int rs = tertiary_ranges[type];
                int re = tertiary_ranges[type + 1];
                for (int i = rs; i < re; ++i) {
                    if (tertiary_objects_x_data[i] == static_cast<uint8_t>(x)) {
                        source = static_cast<uint16_t>(i);
                        if (i < 256) tertiary_source_count_[i]++;
                        break;
                    }
                }
            }
            tertiary_source_idx_[(y << 8) | x] = source;
        }
    }
    return true;
}

// compute_algo_tile — procedural output ignoring map-data gate. Used by
// the "Algo only" debug overlay.

uint8_t Landscape::compute_algo_tile(uint8_t tile_x, uint8_t tile_y) const {
    Alu s;
    s.lda(tile_y);
    s.lsr();
    s.eor(tile_x);
    s.and_(0xf8);
    s.lsr();
    s.adc(tile_x);
    s.lsr();
    s.adc(tile_y);
    uint8_t f1 = s.a;
    return get_tile_from_algorithm(tile_x, tile_y, f1);
}

// ============================================================================
// bake_tile_pseudo_6502 - faithful port of &178d-&19a6 with explicit
// Alu carry tracking. Each block 1:1-maps to the disassembly.
// ============================================================================

uint8_t Landscape::bake_tile_pseudo_6502(uint8_t tile_x, uint8_t tile_y) const {
    Alu s;

    // &178d: Calculate f1 - triangular function of x and y
    s.lda(tile_y);
    s.lsr();                    // A = tile_y >> 1, carry = tile_y bit 0
    s.eor(tile_x);
    s.and_(0xf8);
    s.lsr();                    // carry = bit 0 (always 0 after AND &f8, so carry=0)
    s.adc(tile_x);              // a + tile_x + 0
    s.lsr();                    // carry = bit 0 of result
    s.adc(tile_y);              // a + tile_y + carry
    uint8_t f1 = s.a;

    // &179d: Determine region (mapped data vs algorithm)
    s.lda(tile_y);
    s.cmp(0x79);
    if (tile_y >= 0x79) {
        if (tile_y < 0xbf) {
            return get_tile_from_algorithm(tile_x, tile_y, f1);
        }
        s.sec();
        s.lda(tile_y);
        s.sbc(0x46);
    }
    // skip_subtraction
    s.cmp(0x48);
    if (s.a < 0x48) {
        s.cmp(0x3e);
        if (s.a >= 0x3e) {
            return get_tile_from_algorithm(tile_x, tile_y, f1);
        }
        s.adc(0x0a);            // carry is 0 from failed CMP
    }

    // &17b2: Calculate f2, f3, f4 - carry is tracked through Alu
    uint8_t f2 = s.a;
    s.and_(0xa8);
    s.eor(0x6f);
    s.lsr();                    // carry = bit 0 of (f2 & 0xa8) ^ 0x6f
    s.adc(tile_x);
    s.eor(0x60);
    s.adc(0x28);
    uint8_t f3 = s.a;

    s.and_(0x38);
    s.eor(0xa4);
    s.adc(f2);
    f2 = s.a;                   // f2 updated

    uint8_t y_reg = s.a;
    s.eor(0x2c);
    s.adc(f3);
    uint8_t f4 = s.a;

    // &17ce: Check if should use mapped data
    if (y_reg >= 0x20) {
        return get_tile_from_algorithm(tile_x, tile_y, f1);
    }
    if (f4 >= 0x20) {
        if (f4 < 0x3d) {
            return tiles_table[0]; // TILE_SPACE
        }
        return get_tile_from_algorithm(tile_x, tile_y, f1);
    }

    // &17d6: get_tile_from_map_data — every return on this branch is
    // a map-data tile (the 6502 sets &00 here too, regardless of
    // whether the offset ends up valid).
    last_bake_was_map_data_ = true;

    // Carry state from last ADC (f4 computation) is tracked in s.c
    Alu m;
    m.lda(f4);
    m.asl(); m.asl(); m.asl(); // f4 << 3, carry from 3rd ASL
    m.eor(f2);
    uint8_t addr_low = m.a;
    // carry unchanged by EOR
    m.lda(f4);
    m.and_(0x03);               // AND doesn't change carry
    m.adc(0x4f);                // uses carry from the 3rd ASL
    uint8_t addr_high = m.a;

    uint16_t map_offset = static_cast<uint16_t>(addr_high - 0x4f) * 256 + addr_low;
    // LDA (&a4),Y with Y=&ec; map_data starts at &4fec.
    // offset = (addr_high * 256 + addr_low + 0xEC) - 0x4FEC.
    uint16_t effective = static_cast<uint16_t>(addr_high) * 256 + addr_low + 0xEC;
    uint16_t offset = effective - 0x4FEC;
    last_bake_map_data_offset_ = offset;
    if (offset < 1024) {
        return map_overlay_data[offset];
    }
    return 0x19;
}

// ============================================================================
// get_tile_from_algorithm - port of &17f6-&19a6
// ============================================================================

uint8_t Landscape::get_tile_from_algorithm(uint8_t tile_x, uint8_t tile_y, uint8_t f1) const {
    // &17f6
    if (tile_y < 0x4e) return tiles_table[0]; // TILE_SPACE
    if (tile_y == 0x4e) return get_tile_for_surface(tile_x, f1);
    if (tile_y == 0x4f) {
        if (tile_x == 0x40) return 0x62; // TILE_LEAF | FLIP_V
        return tiles_table[1]; // TILE_EARTH
    }

    // &180e: is_below_surface
    uint8_t y_save = f1;
    // Zero f1 for side/bottom fill checks
    // &1814: check sides
    Alu s;
    s.sec(); // carry=1 from CMP #&4f at &17fc which succeeded (tile_y > 0x4f)
    s.lda(tile_x);
    if (tile_y & 0x80) {
        // bottom half: ADC #&07 with carry=1
        s.adc(0x07);
        if (s.a < 0x2b) return leave_with_earth_or_stone(y_save);
    } else {
        // top half: ADC #&1d with carry=1
        s.adc(0x1d);
        if (s.a < 0x5e) return leave_with_earth_or_stone(y_save);
    }

    // &1827: bottom fill pattern
    if ((y_save & 0xe8) < tile_y) {
        return leave_with_earth_or_stone(y_save);
    }

    // &1830: Calculate f5 (square caverns)
    s.lda(tile_y);
    s.asl();                    // carry = tile_y bit 7
    s.adc(tile_y);
    s.lsr();                    // carry = bit 0
    s.adc(tile_y);
    s.and_(0xe0);
    s.adc(tile_x);
    s.and_(0xe8);

    if (s.a == 0) {
        // is_square_cavern
        if (!(tile_y & 0x80)) return tiles_table[0]; // TILE_SPACE
        // is_windy_square_cavern
        uint8_t x_div8 = tile_x >> 3;
        return (x_div8 == 0x0a) ? 0x8e : 0x0e; // VARIABLE_WIND with optional H-flip
    }

    // &1852: Calculate f6 (vertical shafts)
    s.lda(f1);
    s.lsr(); s.lsr();
    s.and_(0x30);
    s.lsr();
    s.adc(tile_x);
    s.lsr();
    s.eor(tile_x);
    s.lsr();
    s.eor(tile_x);
    s.adc(tile_x);
    s.and_(0xfd);
    s.eor(tile_x);
    s.and_(0x07);

    if (s.a == 0) {
        if (tile_x & 0x80) return 0x08; // vertical shaft
        // Additional check
        s.lda(tile_x);
        s.lsr();
        s.adc(tile_y);
        s.and_(0x30);
        if (s.a != 0) return 0x08; // vertical shaft
    }

    // &1878: not_vertical_shaft
    // CMP #&52 on tile_y (actually X register = tile_y from &1830 TXA)
    if (tile_y < 0x52) return leave_with_earth_or_stone(f1);

    // &187f: Calculate f7 (passage function)
    // carry=1 from CMP #&52 succeeding (tile_y >= 0x52, so BCS not taken = carry WAS set)
    s.sec();
    s.lda(f1);
    s.and_(0x68);
    s.adc(tile_y);              // with carry=1
    s.lsr();
    s.adc(tile_y);
    s.lsr();
    s.adc(tile_y);
    uint8_t c_from_f7 = s.c;   // Save carry for horizontal passage
    s.and_(0xfc);
    s.eor(tile_y);
    s.and_(0x17);

    if (s.a != 0) {
        // &18df: consider_sloping_passage
        return handle_sloping_passage(tile_x, tile_y, f1);
    }

    // &1892: consider_horizontal_passage
    // Carry state from f7: AND/EOR/AND don't affect carry, so use c_from_f7
    s.c = c_from_f7;
    s.lda(f1);
    s.adc(tile_x);
    s.and_(0x50);

    if (s.a == 0) return tiles_table[0]; // TILE_SPACE

    // &1899: f9 calculation
    s.and_(tile_x);
    s.lsr(); s.lsr();
    s.adc(tile_y);
    s.lsr(); s.lsr();
    s.and_(0x0f);

    uint8_t feature_a;
    if (s.a >= 0x08) {
        // is_horizontal_passage_type_two
        feature_a = s.a;
        if (!(f1 & 0x40)) {
            // BVC: bit 6 clear, use feature_a as-is
        } else {
            feature_a |= 0x04;
        }
    } else {
        // is_horizontal_passage_type_one
        uint8_t special = s.a;
        if ((special ^ 0x05) >= 0x06) {
            feature_a = special;
        } else {
            // Calculate f10
            s.lda(f1);
            s.lsr();
            s.adc(tile_y);
            s.eor(tile_x);
            s.and_(0x07);
            feature_a = s.a;
        }
    }

    // &18c1: use_passage_feature
    uint8_t tile_offset = feature_a + 0x1d; // CLC; ADC #&1d

    // Check if crossed by sloping passage
    auto slope = calculate_slope_function(tile_x, tile_y);
    if (slope.is_passage) {
        return tiles_table[0]; // TILE_SPACE
    }

    // Look up feature from combined table
    uint8_t result = (tile_offset < sizeof(tiles_table)) ? tiles_table[tile_offset] : 0x19;

    // Flip mushrooms in flooded passage at y=0xE0
    if (tile_y == 0xe0) result ^= 0x40;

    return result;
}

// ============================================================================
// handle_sloping_passage - port of &18df-&191b
// ============================================================================

uint8_t Landscape::handle_sloping_passage(uint8_t tile_x, uint8_t tile_y, uint8_t f1) const {
    auto slope = calculate_slope_function(tile_x, tile_y);
    if (!slope.is_passage) {
        return leave_with_earth_or_stone(f1);
    }
    if (slope.y == 0) {
        return tiles_table[0]; // TILE_SPACE (middle of passage)
    }

    // is_sloping_passage_edge (&18e8)
    uint8_t slope_type = slope.y;

    // Calculate which slope variant using f1 bits
    // &18ee-&18f3: three ROLs starting with carry=1 (from CPY #&00, Y != 0)
    Alu s;
    s.lda(f1);
    s.c = 1; // carry from CPY #&00 (Y is non-zero, so carry set)
    s.rol(); s.rol(); s.rol();
    s.and_(0x01);
    s.rol();
    uint8_t y_val = s.a; // 0 or 2

    // &18f4-&18fc: check if should use feature or plain slope
    s.lda(f1);
    s.adc(tile_x);     // carry from the ROL above
    s.rol();
    s.eor(tile_y);
    s.and_(0x1a);

    if (s.a != 0) {
        // use_slope (&1913)
        uint8_t tile = slope_tiles_table[y_val];
        tile ^= tile_rotations_table[slope_type - 2];
        return tile;
    }

    // use_sloping_passage_feature (&18fe)
    s.lda(y_val);
    s.eor(tile_rotations_table[slope_type - 2]);
    s.and_(0x7f);
    s.cmp(0x40);
    s.rol();
    s.and_(0x07);
    uint8_t idx = s.a;
    uint8_t tile = sloping_passage_feature_tiles_table[idx];
    tile ^= tile_rotations_table[slope_type - 2];
    return tile;
}

// ============================================================================
// leave_with_earth_or_stone - port of &191c
// ============================================================================

uint8_t Landscape::leave_with_earth_or_stone(uint8_t f1) const {
    if (f1 == 0) return tiles_table[1]; // TILE_EARTH for side/bottom fill

    Alu s;
    s.lda(f1);
    s.lsr(); s.lsr(); s.lsr();
    s.and_(0x0e);
    s.lsr();
    s.adc(0x01);
    uint8_t idx = s.a;
    if (idx > 8) idx = 8;
    return tiles_table[idx];
}

// ============================================================================
// leave_with_tile_from_table - port of &18da
// ============================================================================

uint8_t Landscape::leave_with_tile_from_table(uint8_t index) const {
    if (index < sizeof(tiles_table)) return tiles_table[index];
    return 0x19;
}

// ============================================================================
// get_tile_for_surface - port of &1937-&1945
// ============================================================================

uint8_t Landscape::get_tile_for_surface(uint8_t tile_x, uint8_t f1) const {
    Alu s;
    s.lda(tile_x);
    s.lsr();
    s.adc(tile_x);
    s.and_(0x17);

    if (s.a != 0) {
        // consider_adding_surface_feature (&192a)
        s.adc(tile_x); // carry from AND is unchanged from previous ADC
        s.rol(); s.rol(); s.rol();
        s.and_(0x02);
        s.adc(0x19);
        uint8_t idx = s.a;
        return leave_with_tile_from_table(idx);
    }

    // &1942-&1945: surface fall-through. The 6502 does NOT look up the
    // table here — it RTSes with A directly. A was 0 after the AND #&17
    // at &193e, then &1942 ROR &9d (carry = f1 bit 0), &1944 ROR A
    // (rotates that carry into A's bit 7). So the returned byte is 0x00
    // or 0x80 — tile type 0x00 is TILE_INVISIBLE_SWITCH, which checks
    // the tertiary range-zero table at runtime to spawn birds, frogmen,
    // imps, and any other nest/pipe creatures whose tertiary entry x
    // matches this cell. Returning TILE_SPACE instead leaves the cell
    // inert and kills every surface-level nest in the world.
    return (f1 & 1) ? 0x80 : 0x00;
}

// ============================================================================
// calculate_slope_function - port of &1946-&19a6
// ============================================================================

Landscape::SlopeResult Landscape::calculate_slope_function(uint8_t tile_x, uint8_t tile_y) const {
    Alu s;

    // &1946: check for sloping cavern
    s.lda(tile_y);
    s.lsr();
    s.eor(tile_y);
    s.and_(0x06);

    if (s.a == 0) {
        // consider_adding_sloping_cavern (&194e)
        uint8_t f1_local = recalc_f1(tile_x, tile_y);
        s.lda(f1_local);
        uint8_t y_out = 2;
        s.and_(0x20);
        s.asl(); s.asl();
        s.eor(0xe5); // either ADC (0x65) or SBC (0xe5) opcode
        bool use_adc = (s.a & 0x80) == 0; // BPL = positive = ADC
        if (use_adc) y_out = 4;

        // &195e slope bounds. Both branches enter ADC #&16 with C clear
        // (the &1953/&1954 ASLs always clear it). SMC at &1961 inherits
        // carry from ADC #&16.
        s.lda(tile_y);
        s.clc();
        s.adc(0x16);
        if (use_adc) {
            s.adc(tile_x);
        } else {
            s.sbc(tile_x);
        }
        s.and_(0x5f);
        uint8_t val = s.a;
        // uint8_t underflow at 0 -> 0xff, matching DEX wrap on the 6502.
        val--;

        if (val < 0x0c) return {true, 0}; // middle of sloping cavern
        if (val == 0x0c) return {true, y_out}; // edge
        y_out++;
        val++;
        if (val == 0) return {true, y_out}; // edge (wrapped from 0xff)
        // Fall through to straight passage checks
    }

    // &1971: check for / and \ sloping passages
    // LSR tile_x four times
    s.lda(tile_x);
    s.lsr(); s.lsr(); s.lsr(); s.lsr();
    if (s.c) return {false, 0}; // break every 8 tiles

    // Check / passage
    s.lda(0x01);
    s.clc();
    s.adc(tile_x);
    s.adc(tile_y);
    s.and_(0x8f);
    if (s.a == 0x01) return {true, 0}; // middle of / passage

    // Check \ passage
    uint8_t x_save = s.a;
    s.sec();
    s.lda(tile_y);
    s.sbc(tile_x);
    s.and_(0x2f);

    if (s.a == 0x01) return {true, 0};     // middle of \ passage
    if (s.a == 0x02) return {true, 2};     // edge
    if (s.a < 0x02) return {true, 3};      // narrower edge (a==0)
    if (x_save == 0x02) return {true, 4};  // edge from / check
    if (x_save < 0x02) return {true, 5};   // edge from / check

    return {false, 0}; // solid tile
}

// ============================================================================
// recalc_f1 - same as get_tile's f1 calculation
// ============================================================================

uint8_t Landscape::recalc_f1(uint8_t tile_x, uint8_t tile_y) {
    Alu s;
    s.lda(tile_y);
    s.lsr();
    s.eor(tile_x);
    s.and_(0xf8);
    s.lsr();
    s.adc(tile_x);
    s.lsr();
    s.adc(tile_y);
    return s.a;
}
