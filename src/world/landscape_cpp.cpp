// Native C++ rewrite of the procedural landscape generator at &178d-&19a6.
// Functionally identical to the pseudo-6502 reference in landscape.cpp;
// the two paths are selectable via Landscape::set_use_cpp_impl. Running
// the game with the switch flipped should produce a byte-identical map.
//
// The original threads the 6502 carry flag through chains of LSR / ASL /
// ADC / SBC ops. The reference port mirrors that with a stateful Alu
// struct. This rewrite drops both the Alu and the 6502-mnemonic helpers
// (adc/sbc/lsr/...) entirely; everything is plain C++ arithmetic.
//
// The trick: where the 6502 chains two ADCs and the carry from the
// first feeds the second, we accumulate into an `unsigned` and keep the
// 9-bit running value in bits 0-8. Truncating to uint8_t when the value
// is consumed gives the 6502's mod-256 wrap; `>> 8` recovers the carry
// bit when the next chained add needs it.
//
// AND, EOR, ASL, LSR, ROR don't propagate carry across to a downstream
// op except through the very next LSR/ROL — which always overwrites it.
// So most expressions collapse to plain bitwise math without any
// per-step bookkeeping.
//
// References cited as `&XXXX` point at the disassembly; see landscape.cpp
// for the matching Alu-stepping line.

#include "world/landscape.h"
#include "map_overlay.h"

namespace {

// ============================================================================
// Lookup tables. MUST stay byte-identical to the same arrays in
// landscape.cpp — they're 6502 ROM constants at &114f and friends.
// ============================================================================

constexpr uint8_t kTileRotations[]          = { 0x00, 0xc0, 0x80, 0x40 };
constexpr uint8_t kSlopeTiles[]             = { 0x2e, 0x2f, 0x2e, 0x23 };
constexpr uint8_t kSlopingPassageFeatures[] = {
    0x06, 0x04, 0x06, 0x04, 0x07, 0x05, 0x05, 0x06,
};
constexpr uint8_t kTilesTable[] = {
    0x19, 0x2d, 0xed, 0x6d, 0xad, 0x2d, 0xed, 0x5e, 0x9e,
    0x00, 0xc0, 0x80, 0x40,
    0x2e, 0x2f, 0x2e, 0x23,
    0x06, 0x04, 0x06, 0x04, 0x07, 0x05, 0x05, 0x06,
    0x19, 0x2c, 0x19, 0x2b,
    0x00, 0x01, 0x02, 0x03, 0x1a, 0x21, 0x09, 0x9b,
    0x12, 0x10, 0x60, 0x2b, 0x0f, 0x4f, 0x04, 0x0a,
};
constexpr uint8_t kTileSpace = 0x19;
constexpr uint8_t kTileEarth = 0x2d;

// ============================================================================
// Per-tile pseudorandom hash. f1 picks cavern variants downstream
// (vertical-shaft pattern, passage feature, surface H-flip, ...).
// Port of &178d-&179d.
//
// 6502 chain: y/2 ^ x, mask low bits, halve, +x, halve, +y. The two
// LSRs both clear their carry when consumed (the first because AND
// #&f8 cleared the low bits before the LSR; the second because its
// output feeds straight into the next ADC, which captures the carry
// inline below).
// ============================================================================
static uint8_t calc_f1(uint8_t x, uint8_t y) {
    const uint8_t step = uint8_t(((((y >> 1) ^ x) & 0xf8) >> 1) + x);
    return uint8_t((step >> 1) + (step & 1) + y);
}

// ============================================================================
// Pick from kTilesTable with bounds check. Port of &18da.
// ============================================================================
static uint8_t tile_from_table(uint8_t index) {
    return (index < sizeof(kTilesTable)) ? kTilesTable[index] : kTileSpace;
}

// ============================================================================
// Earth or stone fill. f1 == 0 always means EARTH; otherwise pick one
// of eight variants from bits 4..1 of f1. Port of &191c.
// ============================================================================
static uint8_t earth_or_stone(uint8_t f1) {
    if (f1 == 0) return kTileEarth;
    const uint8_t idx = uint8_t((((f1 >> 3) & 0x0e) >> 1) + 1);
    return kTilesTable[(idx > 8) ? 8 : idx];
}

// ============================================================================
// Slope detection. Port of &1946-&19a6.
//   y == 0       — middle of a passage (clear)
//   y in [2, 5]  — edge (slope orientation index)
// is_passage == false means "solid tile, fill normally".
// ============================================================================
struct SlopeResult { bool is_passage; uint8_t y; };

static SlopeResult slope_function(uint8_t tile_x, uint8_t tile_y) {
    // Sloping-cavern stripe: ((y/2) ^ y) bits 1-2 must both be 0.
    if ((((tile_y >> 1) ^ tile_y) & 0x06) == 0) {
        const uint8_t f1 = calc_f1(tile_x, tile_y);

        // f1 bit 5 selects the slope's fall direction. The 6502 patches
        // the ADC opcode to SBC at runtime; we just observe the choice.
        const bool open_right    = ((((f1 & 0x20) << 2) ^ 0xe5) & 0x80) == 0;
        const uint8_t edge_index = open_right ? 4 : 2;

        // Position along the slope. Two chained adds with the carry from
        // the first feeding the second (CLC+ADC vs SEC+ADC, then ADC vs
        // SBC for the second op).
        unsigned pos_raw;
        if (open_right) {
            const unsigned partial = unsigned(tile_y) + 0x16;
            pos_raw = uint8_t(partial) + tile_x + (partial >> 8);
        } else {
            const unsigned partial = unsigned(tile_y) + 0x17;        // SEC carry-in
            // 6502 SBC: A = A - tile_x - (1 - carry-in-from-previous-add)
            pos_raw = uint8_t(partial) - tile_x - (1u - (partial >> 8));
        }
        const uint8_t pos = uint8_t(pos_raw) & 0x5f;

        if (pos == 0)    return { true, uint8_t(edge_index + 1) };
        if (pos <  0x0d) return { true, 0 };
        if (pos == 0x0d) return { true, edge_index };
        // pos in [0x0e, 0x5f]: fall through to straight-passage probes.
    }

    // Bit 3 of x breaks the diagonal probes every 8 columns.
    if (tile_x & 0x08) return { false, 0 };

    // / passage probe: chained add (1 + x), then + y. The first add can
    // overflow only when x == 0xff; we forward that bit as a carry.
    const unsigned partial = 1u + tile_x;
    const uint8_t up_diag = uint8_t(uint8_t(partial) + tile_y + (partial >> 8)) & 0x8f;
    if (up_diag == 0x01) return { true, 0 };

    // \ passage probe.
    const uint8_t down_diag = uint8_t(tile_y - tile_x) & 0x2f;
    if (down_diag == 0x01) return { true, 0 };
    if (down_diag == 0x02) return { true, 2 };
    if (down_diag <  0x02) return { true, 3 };           // == 0
    if (up_diag   == 0x02) return { true, 4 };
    if (up_diag   <  0x02) return { true, 5 };           // == 0

    return { false, 0 };
}

// ============================================================================
// y == 0x4e: upper-world surface row. Either a recognisable feature
// (tree / bush / lake) or, in clear stretches, SPACE that may be H-flipped
// by f1 bit 0. Port of &1937-&1945.
// ============================================================================
static uint8_t tile_for_surface(uint8_t tile_x, uint8_t f1) {
    // First hash: ⌈x/2⌉ + x masked to 0x17. The "LSR x ; ADC x" pattern
    // is exactly ceiling-divide-by-2 plus x, since (x>>1)+(x&1) = ⌈x/2⌉.
    const unsigned phase1   = (unsigned(tile_x) + 1) / 2 + tile_x;
    const uint8_t  hash_a   = uint8_t(phase1) & 0x17;

    if (hash_a == 0) {
        // Open surface: TILE_SPACE, optionally H-flipped per f1 bit 0.
        return uint8_t(kTilesTable[0x19] | ((f1 & 1) ? 0x80 : 0));
    }

    // Surface-feature variant. The 6502 chains another ADC tile_x, then
    // ROL × 3, then AND #&02, then ADC #&19. Three left-rotates of an
    // 8-bit value with a carry chain just shuffle bits around at fixed
    // positions; tracing them shows that AND #&02 ends up keeping bit 7
    // of the pre-rotate value, and the carry into the final ADC ends up
    // being bit 5 of that same value. So the whole rotate-and-mask
    // collapses to two bit picks.
    const uint8_t step = uint8_t(unsigned(hash_a) + tile_x + (phase1 >> 8));
    const uint8_t bit1 = (step >> 6) & 0x02;             // (step >> 7) << 1
    const uint8_t c3   = (step >> 5) & 1;
    return tile_from_table(uint8_t(bit1 + 0x19 + c3));
}

// ============================================================================
// Sloping-passage tile selection. Port of &18df-&191b.
// ============================================================================
static uint8_t handle_sloping_passage(uint8_t tile_x, uint8_t tile_y, uint8_t f1) {
    const SlopeResult slope = slope_function(tile_x, tile_y);
    if (!slope.is_passage) return earth_or_stone(f1);
    if (slope.y == 0)      return kTileSpace;

    const uint8_t slope_type = slope.y;
    const uint8_t rotation   = kTileRotations[slope_type - 2];

    // The 6502 does ROL × 3 of f1 starting with C=1 (CPY #&00 left
    // carry set), then AND #&01 + ROL. Tracing the four rotates bit
    // by bit shows the final 2-bit value y_val = (f1 bits 6,5) — the
    // disassembly comment "Y = 0 or 2" is misleading; the code can
    // also produce 1 or 3 when bit 5 is set.
    const uint8_t y_val = (f1 >> 5) & 0x03;

    // Feature-vs-plain-slope test. The chain above always leaves C=0
    // coming out of its final ROL (it operates on a 0-or-1 value, so
    // bit 7 is necessarily 0). The next op is ADC tile_x, so:
    //   sum = f1 + tile_x      ; carry-in 0
    //   rol_v = (sum << 1) | carry-out-of-sum
    const unsigned add  = unsigned(f1) + tile_x;
    const uint8_t  rolv = uint8_t((uint8_t(add) << 1) | (add >> 8));

    if (((rolv ^ tile_y) & 0x1a) != 0) {
        return uint8_t(kSlopeTiles[y_val] ^ rotation);
    }

    // Feature-on-slope. (y_val ^ rotation) & 0x7f, then "CMP #&40, ROL"
    // = shift left and bring (mix >= 0x40) in as the new bit 0.
    const uint8_t mix      = (y_val ^ rotation) & 0x7f;
    const uint8_t feat_idx = uint8_t(((mix << 1) | (mix >= 0x40 ? 1 : 0)) & 0x07);
    return uint8_t(kSlopingPassageFeatures[feat_idx] ^ rotation);
}

// ============================================================================
// The procedural-cavern algorithm proper. Called when get_tile_cpp's
// region check decides "not in the map overlay". Port of &17f6-&19a6.
// ============================================================================
static uint8_t get_tile_from_algorithm_cpp(uint8_t tile_x, uint8_t tile_y, uint8_t f1) {
    // ---- &17f6: above ground / surface row ----
    if (tile_y <  0x4e) return kTileSpace;
    if (tile_y == 0x4e) return tile_for_surface(tile_x, f1);
    if (tile_y == 0x4f) {
        if (tile_x == 0x40) return 0x62;                 // TILE_LEAF | FLIP_V
        return kTileEarth;
    }

    // ---- &1814: side / bottom fill ----
    //
    // CMP #&4f at &17fc that put us past the surface succeeded, leaving
    // carry = 1, so both ADCs below pick up the +1 from carry-in.
    if (tile_y & 0x80) {
        if (uint8_t(tile_x + 0x07 + 1) < 0x2b) return earth_or_stone(f1);
    } else {
        if (uint8_t(tile_x + 0x1d + 1) < 0x5e) return earth_or_stone(f1);
    }

    // &1827: the bottom fill cap.
    if ((f1 & 0xe8) < tile_y) return earth_or_stone(f1);

    // ---- &1830: square caverns ----
    //
    // 6502: ASL y ; ADC y ; LSR ; ADC y ; AND 0xe0 ; ADC x ; AND 0xe8.
    // Each ADC chains carry from the previous. The first pair (ASL y +
    // ADC y) forms 3y + (y >> 7) — same as the 9-bit sum 2y + y + carry.
    // Then LSR + ADC y is ⌈step/2⌉ + y. Then AND + ADC x preserves the
    // carry from the second ADC.
    {
        const unsigned step1 = 3u * tile_y + (tile_y >> 7);
        const unsigned step2 = (unsigned(uint8_t(step1)) + 1) / 2 + tile_y;
        const unsigned step3 = unsigned(uint8_t(step2) & 0xe0) + tile_x + (step2 >> 8);
        const uint8_t  mask  = uint8_t(step3) & 0xe8;

        if (mask == 0) {
            if (!(tile_y & 0x80)) return kTileSpace;     // upper-world cavern
            return (tile_x >> 3) == 0x0a ? 0x8e : 0x0e;  // VARIABLE_WIND ± H-flip
        }
    }

    // ---- &1852: vertical shafts ----
    //
    // Six-step bit-mash: f1 → mask → halve → +x → halve → ^x → halve →
    // ^x → +x (with the carry from the second halve).
    {
        const uint8_t  m0   = (f1 >> 2) & 0x30;          // f1 >> 2, mask
        const uint8_t  m1   = m0 >> 1;                    // halve (carry = 0, low bit cleared)
        const uint8_t  m2   = uint8_t(m1 + tile_x);       // +x (carry-in 0)
        const uint8_t  m3   = m2 >> 1;                    // halve, carry = m2 bit 0
        const uint8_t  m4   = uint8_t(m3 ^ tile_x);
        const uint8_t  m5   = m4 >> 1;                    // halve, carry = m4 bit 0
        const uint8_t  c5   = m4 & 1;
        const uint8_t  m6   = uint8_t(m5 ^ tile_x);
        const uint8_t  m7   = uint8_t(m6 + tile_x + c5);  // +x with the chained carry
        const uint8_t  result = (uint8_t(m7 & 0xfd) ^ tile_x) & 0x07;

        if (result == 0) {
            if (tile_x & 0x80) return 0x08;
            // Secondary gate: ⌈x/2⌉ + y, then mask 0x30.
            const unsigned alt = (unsigned(tile_x) + 1) / 2 + tile_y;
            if ((uint8_t(alt) & 0x30) != 0) return 0x08;
        }
    }

    // ---- &1878: gate the rest on y >= 0x52 ----
    if (tile_y < 0x52) return earth_or_stone(f1);

    // ---- &187f: passage hash f7 ----
    //
    // SEC ; LDA f1 ; AND #&68 ; ADC y ; LSR ; ADC y ; LSR ; ADC y.
    // Each LSR's carry is captured for the next ADC; we keep the
    // running 9-bit sum in `unsigned` so it stays available.
    const unsigned f7_step1 = unsigned(f1 & 0x68) + tile_y + 1u;
    const unsigned f7_step2 = (unsigned(uint8_t(f7_step1)) + 1) / 2 + tile_y;
    const unsigned f7_step3 = (unsigned(uint8_t(f7_step2)) + 1) / 2 + tile_y;
    const uint8_t  f7_a     = uint8_t(f7_step3);
    const uint8_t  f7_carry = uint8_t(f7_step3 >> 8);

    // (f7 & 0xfc) ^ y, masked to 0x17 — non-zero diverts to a sloping
    // passage; zero falls through to horizontal-passage selection.
    if (((f7_a & 0xfc) ^ tile_y) & 0x17) {
        return handle_sloping_passage(tile_x, tile_y, f1);
    }

    // ---- &1892: horizontal-passage gate ----
    const uint8_t hp = uint8_t(unsigned(f1) + tile_x + f7_carry);
    const uint8_t gate = hp & 0x50;
    if (gate == 0) return kTileSpace;

    // ---- &1899: feature index inside a passage ----
    //
    // Two LSRs of (gate & x), then ADC y, then two more LSRs. Carry
    // matters only at the ADC y step: that picks up the carry from the
    // second LSR.
    const uint8_t s0 = gate & tile_x;
    const uint8_t s1 = s0 >> 1;                          // carry = s0 & 1
    const uint8_t s2 = s1 >> 1;                          // carry = s1 & 1
    const uint8_t s3 = uint8_t(s2 + tile_y + (s1 & 1));  // ADC y with carry from second LSR
    const uint8_t f9_a = (s3 >> 2) & 0x0f;               // two more LSRs collapsed

    uint8_t feature_a;
    if (f9_a >= 0x08) {
        feature_a = (f1 & 0x40) ? uint8_t(f9_a | 0x04) : f9_a;
    } else if (uint8_t(f9_a ^ 0x05) >= 0x06) {
        feature_a = f9_a;
    } else {
        // Fallback hash: ⌈f1/2⌉ + y, ^x, masked to 0x07.
        const unsigned h = (unsigned(f1) + 1) / 2 + tile_y;
        feature_a = (uint8_t(h) ^ tile_x) & 0x07;
    }

    // A slope might still cross this passage, opening a clear strip.
    if (slope_function(tile_x, tile_y).is_passage) return kTileSpace;

    const uint8_t offset = uint8_t(feature_a + 0x1d);
    uint8_t result = (offset < sizeof(kTilesTable)) ? kTilesTable[offset] : kTileSpace;
    if (tile_y == 0xe0) result ^= 0x40;                  // flooded-passage flip
    return result;
}

} // namespace

// ============================================================================
// get_tile_cpp — public entry point for the C++ rewrite. Same algorithm
// as get_tile_pseudo_6502; switch via Landscape::set_use_cpp_impl.
// ============================================================================
uint8_t Landscape::get_tile_cpp(uint8_t tile_x, uint8_t tile_y) const {
    const uint8_t f1 = calc_f1(tile_x, tile_y);

    // ---- &179d: region selection ----
    //
    // Two slices of y feed the 1024-byte map_overlay; the rest runs the
    // procedural algorithm. The y range [0xbf, 0xff] gets folded down
    // by 0x46 onto the same overlay address space as [0x79, 0xb8].
    if (tile_y >= 0x79 && tile_y < 0xbf) {
        return get_tile_from_algorithm_cpp(tile_x, tile_y, f1);
    }
    const uint8_t y_in_map = (tile_y >= 0xbf)
        ? uint8_t(tile_y - 0x46)
        : tile_y;

    uint8_t f2;
    if (y_in_map >= 0x48) {
        f2 = y_in_map;
    } else {
        if (y_in_map >= 0x3e) return get_tile_from_algorithm_cpp(tile_x, tile_y, f1);
        f2 = uint8_t(y_in_map + 0x0a);                   // ADC #&0a, carry-in 0
    }

    // ---- &17b2: f3 / updated f2 / f4 ----
    //
    // Each chained ADC's carry-out feeds the next ADC's carry-in. The
    // running 9-bit value sits in an `unsigned`; uint8_t(...) extracts
    // the byte, (... >> 8) extracts the carry.
    const uint8_t pre = uint8_t((f2 & 0xa8) ^ 0x6f);
    const unsigned a1 = unsigned(pre >> 1) + tile_x + (pre & 1);
    const unsigned a2 = unsigned(uint8_t(uint8_t(a1) ^ 0x60)) + 0x28 + (a1 >> 8);
    const uint8_t  f3 = uint8_t(a2);

    const uint8_t mix_f3 = uint8_t((f3 & 0x38) ^ 0xa4);
    const unsigned a3 = unsigned(mix_f3) + f2 + (a2 >> 8);
    const uint8_t  new_f2 = uint8_t(a3);
    const uint8_t  y_reg  = new_f2;                       // gate

    const unsigned a4 = unsigned(uint8_t(new_f2 ^ 0x2c)) + f3 + (a3 >> 8);
    const uint8_t  f4 = uint8_t(a4);

    // ---- &17ce: gate to overlay or fall back to algorithm ----
    if (y_reg >= 0x20) return get_tile_from_algorithm_cpp(tile_x, tile_y, f1);
    if (f4 >= 0x20) {
        if (f4 < 0x3d) return kTileSpace;
        return get_tile_from_algorithm_cpp(tile_x, tile_y, f1);
    }

    // ---- &17d6: map-overlay address ----
    //
    // Three ASLs of f4 land bit 5 of f4 into the carry. Then EOR new_f2
    // forms the low byte; (f4 & 0x03) + 0x4f + (carry) forms the high
    // byte. The 6502 indexes via LDA (&a4),Y with Y = 0xec, which lands
    // on 0x4fec — exactly the start of map_overlay_data[].
    const uint8_t addr_low  = uint8_t((f4 << 3) ^ new_f2);
    const uint8_t carry5    = (f4 >> 5) & 1;
    const uint8_t addr_high = uint8_t((f4 & 0x03) + 0x4f + carry5);

    const unsigned effective = unsigned(addr_high) * 256u + addr_low + 0xec;
    const unsigned offset    = effective - 0x4fec;
    if (offset < 1024) return map_overlay_data[offset];
    return kTileSpace;
}
