#include "objects/tile_collision.h"
#include "objects/object.h"
#include "objects/object_manager.h"
#include "objects/collision.h"
#include "objects/object_data.h"
#include "world/landscape.h"
#include "world/tertiary.h"
#include "world/tile_data.h"
#include "world/obstruction.h"
#include "rendering/sprite_atlas.h"
#include "behaviours/npc_helpers.h"
#include <algorithm>
#include <array>

// Faithful byte-for-byte port of the 6502 tile-collision response chain
// at &2f8c - &30df. The control-flow structure here mirrors the original;
// most blocks lift the 6502 arithmetic verbatim (8-bit wraps, ROR/EOR
// idioms) so the values can be cross-checked against the disassembly. A
// few places use int instead of uint8_t for clarity when the 6502
// quietly relied on a flag — those spots are commented.

namespace TileCollision {

namespace {

// =============================================================================
// 6502 zp variables, packed per-resolve into a context. Field names match
// the disassembly (with `&` prefix dropped) so the trace lines up.
// =============================================================================
struct TileObstrData {
    // Address of the 8-byte pattern (one threshold per section). In the
    // 6502 this is a low/high address pair into `obstruction_patterns`;
    // here we hold a pointer to the live pattern.
    const uint8_t* pattern = nullptr;
    // 6502 &7e/&82 top/bottom_tile_obstruction_y_offset.
    uint8_t y_offset = 0;
    // 6502 &7f/&83 top/bottom_tile_sprite_and_y_flip — bit 7 set iff
    // the obstruction lives at the TOP of the tile (ceiling-like).
    bool sprite_y_flip = false;
};

struct Ctx {
    Object& obj;
    const Landscape& landscape;
    ObjectManager& mgr;
    int skip_slot;
    uint8_t prev_x_whole, prev_x_frac, prev_y_whole, prev_y_frac;

    // 6502 sprite size in zp:
    //   &3a this_object_width  = (sprite_w_pixels - 1) << 4   (clears low nibble)
    //   &3c this_object_height = (sprite_h_rows   - 1) << 3
    uint8_t width  = 0;
    uint8_t height = 0;

    // Position-derived (port of calculate_this_object_maximum_x_y at &2a48).
    uint8_t max_x_frac = 0, max_x = 0;
    uint8_t max_y_frac = 0, max_y = 0;
    bool crosses_x = false; // &29d9
    bool crosses_y = false; // &29db

    // Rounded edge fractions (round to mid-pixel).
    //   &84 top_y_fraction_rounded    = top_y_frac & 0xf8 | 0x04
    //   &85 bottom_y_fraction_rounded = max_y_frac & 0xf8 | 0x04
    uint8_t top_y_rounded = 0;
    uint8_t bot_y_rounded = 0;

    // Per-resolve obstruction-data variables (&7c..&83).
    TileObstrData top_data;
    TileObstrData bot_data;
    uint8_t tile_x = 0;
    uint8_t tile_y = 0; // top tile y; bot is tile_y + 1 if crosses_y

    // Obstruction counts/depths (&77/&78/&79/&7a).
    int8_t left_obstr   = 0;
    int8_t top_obstr    = 0;
    int8_t right_obstr  = 0;
    int8_t bottom_obstr = 0;

    // Outputs.
    uint8_t tile_collision_y_flags = 0; // &18: top bit set on bottom collision
    bool top_or_bottom_collision   = false; // &1b
};

// =============================================================================
// Math helpers — straight ports of the named 6502 routines.
// =============================================================================

// Port of &3256 invert_if_negative:
//   &3256 CLC
//   &3257 BPL &325d ; leave            ; A positive → pass through
//   &3259 EOR #&ff
//   &325b ADC #&01                      ; two's-complement negate
//   &325d RTS
static inline uint8_t invert_if_negative(uint8_t a) {
    return (a & 0x80) ? static_cast<uint8_t>((a ^ 0xff) + 1) : a;
}
// Port of &324c invert_if_positive:
//   &324c CLC
//   &324d BMI &3253 ; leave             ; A negative → pass through
//   &324f EOR #&ff
//   &3251 ADC #&01
//   &3253 RTS
static inline uint8_t invert_if_positive(uint8_t a) {
    return (a & 0x80) ? a : static_cast<uint8_t>((a ^ 0xff) + 1);
}
// 6502 &3275 divide_by_eight (keeps sign).
static inline uint8_t divide_by_eight(uint8_t a) {
    // Three iterations of "CMP #&80; ROR A": for each pass, bit 7 of the
    // result equals bit 7 of input (sign-extension).
    for (int i = 0; i < 3; i++) {
        uint8_t bit7 = a & 0x80;
        a = static_cast<uint8_t>((a >> 1) | bit7);
    }
    return a;
}
// 6502 &3235 calculate_seven_eighths(v) = v - (|v|+7)/8 (sign-preserved).
static inline int8_t seven_eighths(int8_t v) {
    int abs_v = (v < 0) ? -static_cast<int>(v) : static_cast<int>(v);
    int eighth = (abs_v + 7) / 8;
    int signed_eighth = (v < 0) ? -eighth : eighth;
    return static_cast<int8_t>(static_cast<int>(v) - signed_eighth);
}

// 6502 &22d4 calculate_angle_from_vector. Same algorithm as
// NPC::angle_from_deltas, just renamed for the trace.
static inline uint8_t angle_from_vector(int8_t vx, int8_t vy) {
    return NPC::angle_from_deltas(vx, vy);
}

// =============================================================================
// Tile data lookup — port of set_obstruction_data_variables_for_top_tile
// (&2453) / for_bottom_tile (&2450). Resolves the tile, applies door
// substitution, and fills in pattern + y_offset + sprite_y_flip.
// =============================================================================
static void set_obstruction_data(const Landscape& landscape, ObjectManager& mgr,
                                  uint8_t tx, uint8_t ty, TileObstrData& out) {
    ResolvedTile r = resolve_tile_with_tertiary(landscape, tx, ty);
    auto& all = reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
        mgr.object(0));
    uint8_t tile = Collision::substitute_door_for_obstruction(
        r.tile_and_flip, r.data_offset, all,
        mgr.tertiary_data_byte(r.data_offset));

    uint8_t type = tile & TileFlip::TYPE_MASK;
    bool fh = (tile & TileFlip::HORIZONTAL) != 0;
    bool fv = (tile & TileFlip::VERTICAL)   != 0;

    int pattern_idx = get_obstruction_pattern_index(type, fh, fv);
    if (pattern_idx < 0 ||
        pattern_idx >= static_cast<int>(Obstruction::patterns.size())) {
        pattern_idx = 0; // empty
    }
    out.pattern  = Obstruction::patterns[pattern_idx].data();
    out.y_offset = get_tile_y_offset(type, fv);
    // 6502 &2477: top_tile_sprite_and_y_flip = tile_flip ^ tiles_sprite_
    // and_y_flip[type] (bit 7). Result bit 7 = effective collision v-flip.
    out.sprite_y_flip = fv ^ tile_obstruction_v_flip_bit(type);
}

// =============================================================================
// Per-section probe — port of check_for_top_and_bottom_tile_collisions
// (&2e8a). Given an x-section index Y in [0..7] within the current tile,
// returns the COMBINED top+bottom obstruction depth for that column,
// scaled to /4 fractions and sign-flipped (negative = obstructed,
// 0 = no obstruction). Mirrors the 6502 byte exactly.
// =============================================================================
static uint8_t check_top_bot_section(const Ctx& ctx, uint8_t y_section) {
    // &2e8c-&2e8e: obstructions default to 0.
    uint8_t top_obstr = 0;
    uint8_t bot_obstr = 0;

    // Top tile probe.
    {
        // &2e90: A = (top_pattern)[Y]
        // &2e92: CLC; ADC y_offset
        uint16_t sum = static_cast<uint16_t>(ctx.top_data.pattern[y_section]) +
                       static_cast<uint16_t>(ctx.top_data.y_offset);
        uint8_t A = (sum > 0xff) ? 0xff : static_cast<uint8_t>(sum);

        // &2e99 SEC; SBC top_y_rounded
        // BCC: borrow -> object_top above obstruction -> no obstr depth (for
        // unflipped the &2ed5 path inverts; for flipped just stays 0).
        if (A >= ctx.top_y_rounded) {
            uint8_t depth = static_cast<uint8_t>(A - ctx.top_y_rounded);
            // &2e9e: CMP height; BCC skip; LDA height. Clamp to height.
            if (depth > ctx.height) depth = ctx.height;
            top_obstr = depth;
        }
    }

    // Bottom tile probe (only if AABB crosses Y boundary).
    // &2ea6: LDA height; BIT crosses_y; BPL skip_checking_bottom_tile.
    bool process_bottom = ctx.crosses_y;
    uint8_t bottom_subtract = ctx.height; // value kept for the unflipped tweak below
    if (process_bottom) {
        uint16_t sum = static_cast<uint16_t>(ctx.bot_data.pattern[y_section]) +
                       static_cast<uint16_t>(ctx.bot_data.y_offset);
        uint8_t A = (sum > 0xff) ? 0xff : static_cast<uint8_t>(sum);

        // &2eb6 CMP bot_y_rounded; BCC skip_ceiling_bottom; LDA bot_y_rounded.
        if (A >= ctx.bot_y_rounded) A = ctx.bot_y_rounded;
        bot_obstr = A; // depth-as-of-flipped; &2ec4 inverts for unflipped.

        // &2ebe BIT bot.sprite_y_flip; BMI skip_adjust.
        if (!ctx.bot_data.sprite_y_flip) {
            // 6502: A = bot_y_rounded; SEC; SBC bot_obstr.
            //       bot_obstr = A.
            uint8_t v = static_cast<uint8_t>(ctx.bot_y_rounded - bot_obstr);
            bot_obstr = v;
        }
        bottom_subtract = ctx.bot_y_rounded; // not used after this branch
        (void)bottom_subtract;
    }

    // Top-tile flipped/unflipped tweak (&2ec9-&2ed5). CRITICAL: entering
    // A depends on path — crossing-Y uses 0-top_y_rounded (&2ec9-&2ecd),
    // not-crossing keeps `height` from &2ea6. Wrong value -> multi-pixel
    // upward push every frame on flat ground.
    {
        uint8_t A = ctx.crosses_y
            ? static_cast<uint8_t>(0u - ctx.top_y_rounded)
            : ctx.height;
        if (!ctx.top_data.sprite_y_flip) {
            A = static_cast<uint8_t>(A - top_obstr);
            top_obstr = A;
        }
    }

    // &2ed7-&2ee7: combine and sign-flip.
    //   total = top_obstr + bot_obstr
    //   total = total + 6                (round)
    //   ROR A; LSR A                     (divide by 4 with sign-extend)
    //   AND #&fe; EOR #&ff; ADC #&01     (negate, clear bit 0)
    uint16_t total16 = static_cast<uint16_t>(top_obstr) +
                       static_cast<uint16_t>(bot_obstr);
    uint8_t total = static_cast<uint8_t>(total16 & 0xff);
    bool carry = total16 > 0xff;
    // ADC #&06 with the ADC's incoming carry from the sum:
    uint16_t s = static_cast<uint16_t>(total) + 6 + (carry ? 1 : 0);
    uint8_t A = static_cast<uint8_t>(s & 0xff);
    bool c1 = s > 0xff;
    // ROR A: bit 7 = old C; new C = old bit 0.
    uint8_t bit0 = A & 1;
    A = static_cast<uint8_t>((A >> 1) | (c1 ? 0x80 : 0x00));
    bool c2 = bit0 != 0;
    // LSR A: bit 7 = 0; new C = old bit 0.
    A = static_cast<uint8_t>(A >> 1);
    (void)c2;
    // AND #&fe — clear bit 0.
    A &= 0xfe;
    // EOR #&ff; CLC; ADC #&01 — two's-complement negate.
    A = static_cast<uint8_t>(((A ^ 0xff) + 1) & 0xff);
    return A;
}

// =============================================================================
// Top-bottom edge walk — port of the &2fb8 section loop. For each
// 0x20-frac wide section across the AABB width, decrement
// top_obstruction (when top tile obstructs object's top row) and
// bottom_obstruction (when bottom tile obstructs object's bottom row).
// On exit, ctx.top_obstr/bottom_obstr hold COUNTS (in sections), to be
// scaled to depth via ASL ASL ASL at &300f-&301b.
// =============================================================================
// &2fb8: A = top_y_rounded; SEC; SBC top.y_offset; BCS skip; LDA #0.
// Recomputes rounded-Y relative to each tile's obstruction y_offset.
static void refresh_relative_offsets(const Ctx& ctx,
                                     uint8_t& top_rel, uint8_t& bot_rel) {
    if (ctx.top_y_rounded >= ctx.top_data.y_offset) {
        top_rel = static_cast<uint8_t>(ctx.top_y_rounded - ctx.top_data.y_offset);
    } else {
        top_rel = 0;
    }
    if (ctx.bot_y_rounded >= ctx.bot_data.y_offset) {
        bot_rel = static_cast<uint8_t>(ctx.bot_y_rounded - ctx.bot_data.y_offset);
    } else {
        bot_rel = 0;
    }
}

static void count_top_and_bottom(Ctx& ctx) {
    uint8_t top_rel, bot_rel;
    refresh_relative_offsets(ctx, top_rel, bot_rel);

    // &2fa3 TAY: section = obj.x_frac >> 5.
    int section = static_cast<int>(ctx.obj.x.fraction) >> 5;
    // sections_to_check at &2fb6: width >> 5.
    int sections_remaining = static_cast<int>(ctx.width) >> 4; // &3a is "(w-1)<<4"; LSR ×5 ≡ >> 4 of width-byte then >> 1.
    // Actually 6502 LDA width; LSR ×5 = >>5. Width in our port is also
    // (w_pix-1) << 4, i.e. matches &3a directly. So sections = width >> 5.
    sections_remaining = static_cast<int>(ctx.width) >> 5;

    while (true) {
        // Top tile probe at section — &2fce LDA/CMP/ROR/EOR/BMI.
        // obstructed = !((pattern[section] >= top_rel) XOR sprite_y_flip).
        {
            uint8_t pat = ctx.top_data.pattern[section];
            bool ge = pat >= top_rel;
            bool not_obstr = (ge != ctx.top_data.sprite_y_flip);
            // Above is XOR of bit 7 of (CMP-result-bit-shift) vs sprite flip.
            // BMI on result==1 means "skip if bit 7 set". Bit 7 of CMP-result
            // shifted via ROR: ROR A has bit 7 = C. So CMP setting C=1 (A>=B)
            // -> ROR result bit 7 = 1 -> BMI taken when EOR with flip yields 1.
            // i.e. not obstructed iff (C XOR flip-bit) == 1 — same as `ge != flip`.
            if (!not_obstr) {
                ctx.top_obstr = static_cast<int8_t>(ctx.top_obstr - 1);
            }
        }
        // Bottom tile probe at section.
        {
            uint8_t pat = ctx.bot_data.pattern[section];
            bool ge = pat >= bot_rel;
            bool not_obstr = (ge != ctx.bot_data.sprite_y_flip);
            if (!not_obstr) {
                ctx.bottom_obstr = static_cast<int8_t>(ctx.bottom_obstr - 1);
            }
        }

        // &2fe4 DEC sections_to_check; BMI finished.
        sections_remaining--;
        if (sections_remaining < 0) break;

        // &2fe8 INY; CPY #&08; BCC loop.
        section++;
        if (section < 8) continue;

        // &2fed INC tile_x; advance to next tile horizontally.
        ctx.tile_x = static_cast<uint8_t>(ctx.tile_x + 1);
        section = 0;

        // Refresh tile data for next column. 6502 keeps tile_y on the
        // BOTTOM tile when AABB crosses Y (INC'd at &2f1e), TOP otherwise.
        // Wrong choice probes the wrong row -> player can't walk on flat
        // ground when sprite_h < tile_h.
        uint8_t bot_ty = ctx.crosses_y
            ? static_cast<uint8_t>(ctx.tile_y + 1)
            : ctx.tile_y;
        set_obstruction_data(ctx.landscape, ctx.mgr, ctx.tile_x, bot_ty,
                             ctx.bot_data);
        if (ctx.crosses_y) {
            set_obstruction_data(ctx.landscape, ctx.mgr, ctx.tile_x, ctx.tile_y,
                                 ctx.top_data);
        } else {
            ctx.top_data = ctx.bot_data;
        }

        // &300b LDY #&00; BEQ loop. Restart with refreshed relative offsets
        // (both tiles changed).
        refresh_relative_offsets(ctx, top_rel, bot_rel);
    }
}

// =============================================================================
// Single-section edge probe — used for left/right edges. Walks the
// 6502's check_for_top_and_bottom_tile_collisions for ONE x-section
// (the section at the leading or trailing edge), returns the combined
// signed depth byte. tile_x must be set to the tile containing that
// edge before the call.
// =============================================================================
static uint8_t left_or_right_edge_depth(Ctx& ctx, uint8_t x_frac_at_edge,
                                         uint8_t edge_tile_x) {
    // The 6502 calls set_obstruction_data_variables_for_top_tile here too
    // because tile_x may have advanced past where the data was set. We
    // refresh both top and bottom before the call so the section value
    // matches the current edge column.
    set_obstruction_data(ctx.landscape, ctx.mgr, edge_tile_x, ctx.tile_y,
                         ctx.top_data);
    if (ctx.crosses_y) {
        set_obstruction_data(ctx.landscape, ctx.mgr, edge_tile_x,
                             static_cast<uint8_t>(ctx.tile_y + 1),
                             ctx.bot_data);
    } else {
        ctx.bot_data = ctx.top_data;
    }
    int section = (x_frac_at_edge >> 5) & 0x07;
    return check_top_bot_section(ctx, static_cast<uint8_t>(section));
}

// CMP #&80; ROR A — preserves sign while halving.
static int8_t signed_half(int8_t v) {
    return static_cast<int8_t>((static_cast<int>(v) >= 0)
                                ? (v >> 1)
                                : ((v >> 1) | 0x80));
}

// =============================================================================
// Halve velocities and revert position — port of &3047
// halve_object_velocities_and_clear_obstructions.
// =============================================================================
static void halve_velocities_and_revert(Ctx& ctx) {
    ctx.obj.x.whole    = ctx.prev_x_whole;
    ctx.obj.x.fraction = ctx.prev_x_frac;
    ctx.obj.y.whole    = ctx.prev_y_whole;
    ctx.obj.y.fraction = ctx.prev_y_frac;
    ctx.obj.velocity_x = signed_half(ctx.obj.velocity_x);
    ctx.obj.velocity_y = signed_half(ctx.obj.velocity_y);
}

// =============================================================================
// Apply collision response — port of &306c
// apply_tile_collision_to_position_and_velocity.
//
// Inputs: ctx.{left,right,top,bottom}_obstr (already scaled to depth
// units), and the four obstructions cached in raw memory order so
// LDA &0077,Y can index them.
// =============================================================================
static void apply_tile_collision(Ctx& ctx) {
    // Build vector_y = right - left, vector_x = top - bottom.
    int8_t vector_x = static_cast<int8_t>(ctx.top_obstr   - ctx.bottom_obstr);
    int8_t vector_y = static_cast<int8_t>(ctx.right_obstr - ctx.left_obstr);

    // &306c JSR calculate_angle_from_vector -> A = tile_collision_angle.
    uint8_t tile_collision_angle = angle_from_vector(vector_x, vector_y);

    // &3071: A = angle - 0x60 (rotate -135°).
    uint8_t A = static_cast<uint8_t>(tile_collision_angle - 0x60);
    // &3074: AND #&c0 -> top->0x00, right->0x40, bottom->0x80, left->0xc0.
    A &= 0xc0;
    // &3076-&3078: ASL; ROL; ROL — shift &c0 into &03.
    bool c = (A & 0x80) != 0; A = static_cast<uint8_t>(A << 1);
    uint8_t r = static_cast<uint8_t>((A << 1) | (c ? 1 : 0));
    bool c2 = (A & 0x80) != 0;
    r = static_cast<uint8_t>((r << 1) | (c2 ? 1 : 0));
    // After the three shifts, low 2 bits hold the direction index 0..3.
    uint8_t Y = r & 0x03;
    // &307a EOR #&02 -> opposite direction in X.
    uint8_t X = Y ^ 0x02;

    // 6502 indexed access: &0077,Y where the four obstr live in order
    // [left, top, right, bottom].
    int8_t obstr[4] = { ctx.left_obstr, ctx.top_obstr,
                        ctx.right_obstr, ctx.bottom_obstr };
    // &307d LDA &0077,Y: this_object_considered_obstruction.
    int considered = obstr[Y];
    int opposite   = obstr[X];
    // &3080-&3084: BCS uses the considered if considered >= opposite (CMP),
    // else the opposite. Implements MAX of the two signed obstruction
    // counts (which are <=0; less-negative wins).
    int chosen = (considered >= opposite) ? considered : opposite;

    // &3086 CMP #&00; BNE skip_floor; LDA #&fe — minimum push of -2.
    int amount = chosen;
    if (amount == 0) amount = -2; // 0xfe as int

    // &308c-&308d ASL ASL — multiply by 4. Operates on the raw byte
    // including any 8-bit wrap (the 6502 truncates).
    uint8_t move_amt = static_cast<uint8_t>(amount & 0xff);
    move_amt = static_cast<uint8_t>(move_amt << 1);
    move_amt = static_cast<uint8_t>(move_amt << 1);

    // &308f-&3094: determine which axis. Y values: 0=top, 1=right, 2=bottom,
    // 3=left. After DEY: 0xff, 0, 1, 2. AND #&01: 1, 0, 1, 0. ASL: 2, 0, 2, 0.
    // X = 2 -> axis = Y (vertical), X = 0 -> axis = X (horizontal).
    uint8_t axis_x_byte = static_cast<uint8_t>(((Y - 1) & 0x01) << 1);
    bool axis_is_y = (axis_x_byte != 0);

    // &3095-&309e: if axis_is_y -> not_x branch; else round X up to next pixel.
    if (!axis_is_y) {
        uint16_t s = static_cast<uint16_t>(move_amt) + 0x0f;
        if (s > 0xff) {
            // BCC skip_ceiling — taken iff no carry; if carry (s>0xff) -> fe.
            move_amt = 0xfe;
        } else {
            move_amt = static_cast<uint8_t>(s);
        }
    }

    // &30a0-&30a5 CPY #&02; BCC; INY. Y=0/3 (top/left) need positive
    // push out; Y=1/2 (right/bottom) need negative. The 6502's PLP'd
    // carry is overridden by add_A_to_position's internal CLC — apparent
    // sign bug; flip explicitly for top/left.
    bool top_or_left = (Y == 0 || Y == 3);
    uint8_t signed_move = invert_if_positive(move_amt);
    int delta = static_cast<int8_t>(signed_move);
    if (top_or_left) delta = -delta;

    if (axis_is_y) {
        int combined = static_cast<int>(ctx.obj.y.whole) * 256 +
                       static_cast<int>(ctx.obj.y.fraction) + delta;
        ctx.obj.y.whole    = static_cast<uint8_t>((combined >> 8) & 0xff);
        ctx.obj.y.fraction = static_cast<uint8_t>(combined & 0xff);
    } else {
        int combined = static_cast<int>(ctx.obj.x.whole) * 256 +
                       static_cast<int>(ctx.obj.x.fraction) + delta;
        ctx.obj.x.whole    = static_cast<uint8_t>((combined >> 8) & 0xff);
        ctx.obj.x.fraction = static_cast<uint8_t>(combined & 0xff);
    }
    // &30b0-&30b9: velocity bounce. velocity_angle - tile_collision_angle.
    uint8_t velocity_angle = angle_from_vector(ctx.obj.velocity_x,
                                                ctx.obj.velocity_y);
    uint8_t magnitude = static_cast<uint8_t>(std::max(
        std::abs(static_cast<int>(ctx.obj.velocity_x)),
        std::abs(static_cast<int>(ctx.obj.velocity_y))));
    ctx.obj.pre_collision_magnitude = magnitude;
    ctx.obj.pre_collision_angle     = velocity_angle;

    uint8_t angle_rel = static_cast<uint8_t>(velocity_angle - tile_collision_angle);
    if ((angle_rel & 0x80) == 0) {
        // was_moving_towards_obstruction: bounce.
        // &30d2-&30df: SEC; SBC #&3f -> angle relative to head-on.
        //   divide_by_eight; ADC angle (the relative); EOR #&ff; CLC;
        //   ADC tile_collision_angle -> bounce angle.
        uint8_t rel = static_cast<uint8_t>(angle_rel - 0x3f);
        uint8_t reduced = divide_by_eight(rel);
        uint8_t bounce_angle = static_cast<uint8_t>(reduced + angle_rel);
        bounce_angle = static_cast<uint8_t>(bounce_angle ^ 0xff);
        bounce_angle = static_cast<uint8_t>(bounce_angle + 1 + tile_collision_angle);
        // Reduce magnitude — port of &30e3-&30ef. Carry behaviour matters:
        //   CMP #&20 (BCC skip; LDA #&20)
        //   SBC #&02
        //   BCS skip; LDA #&00
        // For mag >= 0x20: CMP sets C=1, cap to 0x20, SBC with C=1 -> 0x1e.
        // For mag <  0x20: CMP sets C=0 (borrow), SBC with C=0 -> mag - 3.
        // For mag <  3:    SBC borrows again, BCC's LDA #&00 zeros it.
        if (magnitude >= 0x20) {
            magnitude = 0x1e;
        } else if (magnitude >= 3) {
            magnitude = static_cast<uint8_t>(magnitude - 3);
        } else {
            magnitude = 0;
        }
        magnitude = static_cast<uint8_t>(seven_eighths(static_cast<int8_t>(magnitude)));
        int8_t out_vx = 0, out_vy = 0;
        NPC::vector_from_magnitude_and_angle(magnitude, bounce_angle, out_vx, out_vy);
        ctx.obj.velocity_x = out_vx;
        ctx.obj.velocity_y = out_vy;
    } else {
        // was_moving_away_from_obstruction. Halve only if grazing slowly.
        uint8_t graze = static_cast<uint8_t>(angle_rel - 0xc0);
        graze = invert_if_negative(graze);
        if (graze >= 0x2a) return; // not grazing
        if (magnitude < 0x40) return; // moving slowly -> leave
        halve_velocities_and_revert(ctx);
    }
}

} // namespace

// =============================================================================
// Public entry. Mirrors the 6502 entry at &2f8c "check_for_collision_
// with_tiles" (drops the water-buoyancy preamble at &2ee8 — buoyancy
// stays in Water::apply_water_effects on our side).
// =============================================================================
Result resolve(Object& obj,
               uint8_t prev_x_whole, uint8_t prev_x_frac,
               uint8_t prev_y_whole, uint8_t prev_y_frac,
               const Landscape& landscape, ObjectManager& mgr,
               int skip_slot) {
    Result result{};

    // Set up sprite size in 6502 byte conventions:
    //   width-byte  = (w_pixels - 1) << 4
    //   height-byte = (h_rows   - 1) << 3
    if (obj.sprite > 0x80) return result;
    const SpriteAtlasEntry& sprite = sprite_atlas[obj.sprite];
    uint8_t w_pix = sprite.w;
    uint8_t h_pix = sprite.h;
    if (w_pix == 0 || h_pix == 0) return result;

    Ctx ctx{obj, landscape, mgr, skip_slot,
            prev_x_whole, prev_x_frac, prev_y_whole, prev_y_frac};
    ctx.width  = static_cast<uint8_t>((w_pix - 1) << 4);
    ctx.height = static_cast<uint8_t>((h_pix - 1) << 3);

    // calculate_this_object_maximum_x_y (&2a48). max = pos + width/height,
    // with crosses-tile bit if the addition overflowed.
    {
        uint16_t s = static_cast<uint16_t>(obj.x.fraction) + ctx.width;
        ctx.max_x_frac = static_cast<uint8_t>(s & 0xff);
        ctx.max_x      = static_cast<uint8_t>(obj.x.whole + (s > 0xff ? 1 : 0));
        ctx.crosses_x  = (s > 0xff);
    }
    {
        uint16_t s = static_cast<uint16_t>(obj.y.fraction) + ctx.height;
        ctx.max_y_frac = static_cast<uint8_t>(s & 0xff);
        ctx.max_y      = static_cast<uint8_t>(obj.y.whole + (s > 0xff ? 1 : 0));
        ctx.crosses_y  = (s > 0xff);
    }

    // &2f8c-&2f9a: round top/bottom to mid-pixel.
    ctx.top_y_rounded = static_cast<uint8_t>((obj.y.fraction & 0xf8) | 0x04);
    ctx.bot_y_rounded = static_cast<uint8_t>((ctx.max_y_frac  & 0xf8) | 0x04);

    // &2f9c-&2fa3: section index = obj.x_frac >> 5 (already used inside
    // count_top_and_bottom; we just need tile_x/tile_y here).
    ctx.tile_x = obj.x.whole;
    ctx.tile_y = obj.y.whole;

    // Set up obstruction data for the top and (if crossing) bottom tiles
    // at the LEFT-EDGE column (matches the 6502's
    // check_for_collision_with_water_and_tiles preamble).
    set_obstruction_data(landscape, mgr, ctx.tile_x, ctx.tile_y, ctx.top_data);
    if (ctx.crosses_y) {
        set_obstruction_data(landscape, mgr, ctx.tile_x,
                             static_cast<uint8_t>(ctx.tile_y + 1),
                             ctx.bot_data);
    } else {
        ctx.bot_data = ctx.top_data;
    }

    // &2fa4 first call: left-edge depth.
    ctx.left_obstr = static_cast<int8_t>(
        left_or_right_edge_depth(ctx, obj.x.fraction, ctx.tile_x));

    // Restore tile_x to the left edge so the count_top_and_bottom walk
    // starts at the correct column.
    ctx.tile_x = obj.x.whole;
    set_obstruction_data(landscape, mgr, ctx.tile_x, ctx.tile_y, ctx.top_data);
    if (ctx.crosses_y) {
        set_obstruction_data(landscape, mgr, ctx.tile_x,
                             static_cast<uint8_t>(ctx.tile_y + 1),
                             ctx.bot_data);
    } else {
        ctx.bot_data = ctx.top_data;
    }

    // &2fa9-&3007: walk top and bottom edges, accumulating section counts.
    ctx.top_obstr    = 0;
    ctx.bottom_obstr = 0;
    count_top_and_bottom(ctx);

    // &300f-&301b: scale counts to depth-equivalent units (×8).
    ctx.bottom_obstr = static_cast<int8_t>(ctx.bottom_obstr * 8);
    ctx.top_obstr    = static_cast<int8_t>(ctx.top_obstr    * 8);

    // &301d-&3031: tile_collision_y_flags.
    {
        uint8_t v = static_cast<uint8_t>(ctx.top_obstr - ctx.bottom_obstr);
        ctx.tile_collision_y_flags = static_cast<uint8_t>((v ^ 0xff) + 1);
        // &302b-&3031: top_or_bottom_collision = (top|bottom) ≥ 1 -> bit 7.
        uint8_t any_tb = static_cast<uint8_t>(static_cast<uint8_t>(ctx.top_obstr) |
                                                static_cast<uint8_t>(ctx.bottom_obstr));
        ctx.top_or_bottom_collision = (any_tb != 0); // ROR turns ≥1 into bit-7 set
    }

    // &3033 second call: right-edge depth at the right-edge column.
    {
        uint8_t right_edge_x = ctx.max_x;
        uint8_t right_edge_xf = ctx.max_x_frac;
        ctx.right_obstr = static_cast<int8_t>(
            left_or_right_edge_depth(ctx, right_edge_xf, right_edge_x));
    }

    // &303d-&303f: vector_x | vector_y; if any nonzero -> apply collision.
    int8_t vec_x = static_cast<int8_t>(ctx.top_obstr   - ctx.bottom_obstr);
    int8_t vec_y = static_cast<int8_t>(ctx.right_obstr - ctx.left_obstr);

    if (vec_x != 0 || vec_y != 0) {
        apply_tile_collision(ctx);
        result.collided = true;
    } else if (ctx.left_obstr != 0 || ctx.top_obstr != 0) {
        // &3041-&3045: surrounded fallback.
        halve_velocities_and_revert(ctx);
        result.surrounded = true;
        result.collided = true;
    }
    result.top_or_bottom_collision = ctx.top_or_bottom_collision;
    result.pre_collision_magnitude = obj.pre_collision_magnitude;
    result.landed_on_bottom = (ctx.tile_collision_y_flags & 0x80) != 0;
    return result;
}

} // namespace TileCollision
