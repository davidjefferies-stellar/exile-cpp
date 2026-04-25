#pragma once
#include <cstdint>

// Fallback tile types for CHECK_TERTIARY_OBJECT_RANGE_N tiles (0x00-0x08)
// when no tertiary object matches at a given x. Port of &117c.
static constexpr uint8_t feature_tiles_table[9] = {
    0x1b, 0x5a, 0x19, 0x19, 0x1e, 0x13, 0x24, 0x2c, 0x19,
};

// Procedural landscape generation matching the original algorithm at &178d-&19a6.
// Returns tile_type_and_flip: bits 7-6 are flip flags, bits 5-0 are tile type.
//
// Two implementations live behind get_tile():
//   * pseudo-6502 (landscape.cpp): faithful Alu-emulating port, the
//     reference. Tracks A/C explicitly so each line maps 1:1 to the
//     disassembly at &178d-&19a6.
//   * native C++ (landscape_cpp.cpp): same algorithm, expressed with
//     normal arithmetic and uint8_t implicit wraparound. Easier to
//     read, no Alu struct.
// They MUST produce byte-identical output for every (x, y); the toggle
// exists so the C++ rewrite can be A/B-tested against the reference and
// so future tweaks have a known-good baseline to diff against. Default
// is pseudo-6502 (the reference); set via exile.ini's [landscape]
// section or set_use_cpp_impl().
class Landscape {
public:
    // Get tile at world coordinates. Returns tile_type | flip_flags.
    // Pure function of (x, y) plus the static map overlay data.
    uint8_t get_tile(uint8_t tile_x, uint8_t tile_y) const;

    // Toggle between the pseudo-6502 reference and the native C++
    // rewrite. The two are intended to be functionally identical.
    void set_use_cpp_impl(bool b) { use_cpp_impl_ = b; }
    bool use_cpp_impl() const { return use_cpp_impl_; }

private:
    bool use_cpp_impl_ = false;

    // ---- Pseudo-6502 reference (landscape.cpp) ----
    uint8_t get_tile_pseudo_6502(uint8_t tile_x, uint8_t tile_y) const;
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
    uint8_t get_tile_cpp(uint8_t tile_x, uint8_t tile_y) const;
};
