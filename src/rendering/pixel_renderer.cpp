#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifdef UNICODE
#undef UNICODE
#endif
#ifdef _UNICODE
#undef _UNICODE
#endif

#include "rendering/pixel_renderer.h"
#include "rendering/sprite_atlas.h"
#include "rendering/sprite_data.h"
#include "rendering/palette.h"
#include "rendering/font8x8.h"
#include "world/tile_data.h"
#include "objects/object_data.h"
#include "fenster.h"
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

// BBC mode-1 pixels are 2:1 — each atlas pixel draws two screen pixels
// wide, one tall. Atlas tile sprites are 16 atlas-px wide × 32 tall, so
// the tile grid is 32 screen-px wide × 32 tall at 1× scale.
static constexpr int PX_SCALE_X = 2;
static constexpr int PX_SCALE_Y = 1;
static constexpr int TILE_PX_BASE_X = 16 * PX_SCALE_X;  // 32
static constexpr int TILE_PX_BASE_Y = 32 * PX_SCALE_Y;  // 32
static constexpr int INITIAL_W = 1920;
static constexpr int INITIAL_H = 1080;
static constexpr int HUD_PX = TILE_PX_BASE_Y / 2;
// Zoom is expressed as (scale / zoom_den). MIN_SCALE=1 with an
// MAX_ZOOM_DEN > 1 lets the wheel keep zooming out past 1:1.
static constexpr int MIN_SCALE      = 1;
static constexpr int MAX_SCALE      = 8;
static constexpr int MAX_ZOOM_DEN   = 4;

static constexpr uint32_t COL_RED     = 0xCC0000;
static constexpr uint32_t COL_GREEN   = 0x00CC00;
static constexpr uint32_t COL_MAGENTA = 0xCC00CC;
static constexpr uint32_t COL_CYAN    = 0x00CCCC;
static constexpr uint32_t COL_WHITE   = 0xCCCCCC;

// Floor division by 256 — rounds toward -inf rather than toward zero.
//
// world_to_screen's sub-tile offset is `(obj_frac - vp_frac) * tpx / 256`.
// The numerator can be negative (viewport-fraction > object-fraction), and
// C's default truncate-toward-zero leaves a visible 1-pixel seam between a
// static object and the landscape tile it sits on whenever the viewport's
// fraction crosses a tile-pixel boundary: the tile's offset `(0-X)*tpx/256`
// jumps at X = k·256/tpx while the object's `(F-X)*tpx/256` jumps at the
// nearest k·256/tpx above F, one pixel later. Floor division makes both
// snap on the same X so the sprite sits flush as the player walks.
static inline int floor_div_256(int v) {
    return (v >= 0) ? (v >> 8) : -((-v + 255) >> 8);
}

// --- Debug-panel layout -----------------------------------------------------
//
// Three checkboxes laid out left-to-right across the bottom HUD strip,
// replacing the earlier energy-bar + weapon-colour swatch. Each entry is
// a small box with a text label to its right. The geometry is computed
// from constants so the same layout is used by both the renderer and
// the click hit-test, keeping them in lockstep.
static constexpr int CHECKBOX_SIZE = 10;
static constexpr int CHECKBOX_PAD  = 4;
static constexpr int CHECKBOX_LABEL_GAP = 4;
static constexpr int CHECKBOX_SLOT_W = 110;   // box + gap + label room

struct DebugCheckbox {
    const char* label;
    // Pointer into Impl state — set by compute_checkboxes when laying
    // out the strip. The click-in-rect test toggles *state when hit.
    bool* state;
};

// Compute x-pixel offset of the Nth checkbox. Keeps render + hit-test
// in sync without storing a layout struct.
static int checkbox_slot_x(int idx) {
    return 10 + idx * CHECKBOX_SLOT_W;
}

static int checkbox_slot_y(int hud_y_px) {
    return hud_y_px + (16 - CHECKBOX_SIZE) / 2;
}

struct PixelRenderer::Impl {
    std::vector<uint32_t> buf;
    // Parallel per-pixel foreground mask. Bit set = a tile wrote a BBC
    // logical-colour-8..15 pixel here, which the 6502 marks by leaving
    // bit 7 set on the plotted byte. The object-plot pass BMIs past any
    // pixel that already has bit 7 set (&1066 BMI skip_byte), so objects
    // hide behind those tile pixels. We reset every begin_frame and
    // sample it in blit_sprite when drawing objects.
    std::vector<uint8_t> fg_mask;
    struct fenster f;
    bool initialized = false;
    uint8_t vp_center_x = 0;
    uint8_t vp_center_y = 0;
    uint8_t vp_frac_x   = 0;   // sub-tile fractional position of the view centre
    uint8_t vp_frac_y   = 0;   // (0-255, same units as Fixed8_8 fraction)
    // Debug-overlay toggles — exposed through IRenderer::*_enabled() so
    // the game layer reads them each frame. Driven by the checkbox
    // strip at the bottom of the window (see render_debug_panel); the
    // keyboard shortcuts that used to flip these were removed when the
    // 6502-faithful G/T/W key bindings took over those letters.
    bool tile_outline_on = false;    // "Grid" checkbox
    bool object_tiers_on = false;    // "Object tiers" checkbox
    bool map_mode_on     = false;    // "Map mode" checkbox
    // Separate "Debug" checkbox gating the debug-text overlays (selected
    // tile-info string, map-mode banner). Grid-line drawing and camera-
    // anchored map mode stay on their own switches above; this just
    // controls whether their associated text panels are drawn.
    bool debug_text_on   = false;
    bool switches_on     = false;    // "Switches" checkbox — draws
                                     // green switch→door wires.
    bool transports_on   = false;    // "Transports" checkbox — draws
                                     // cyan transporter→destination wires.
    bool collision_on    = false;    // "Collision" checkbox — shades the
                                     // solid region of every visible tile
                                     // according to its obstruction
                                     // pattern (per-x-section threshold),
                                     // so sink-through bugs are visible.
    bool aabb_overlay_on = false;    // still keyboard-toggled via 'B'.
    bool aabb_key_prev = false;
    // Highlighted tile — drawn only while the tile grid is on.
    bool has_highlight = false;
    uint8_t highlight_x = 0;
    uint8_t highlight_y = 0;
    int key_scan_idx = 0;
    bool events_processed = false;
    bool should_close = false;
    // Zoom factor = scale / zoom_den. Only one of the two is >1 at any
    // time: wheel up from 1:1 grows `scale`; wheel down from 1:1 grows
    // `zoom_den`. Keeps everything integer-arithmetic friendly.
    int scale = 1;
    int zoom_den = 1;

    // Mouse / pan / click state
    int prev_mouse_x = 0;
    int prev_mouse_y = 0;
    bool right_was_down = false;
    bool left_was_down = false;
    int pan_px_x = 0;              // sub-tile pixel offset from drag
    int pan_px_y = 0;
    int pending_pan_tiles_x = 0;   // whole-tile steps consumed by game
    int pending_pan_tiles_y = 0;
    bool has_pending_click = false;
    int pending_click_x = 0;       // screen pixel coords of the click
    int pending_click_y = 0;
    std::string overlay;

    Impl() : buf(INITIAL_W * INITIAL_H, 0),
             fg_mask(INITIAL_W * INITIAL_H, 0),
             f{.title = "Exile",
               .width = INITIAL_W,
               .height = INITIAL_H,
               .buf = nullptr} {
        f.buf = buf.data();
        std::memset(f.keys, 0, sizeof(f.keys));
        f.mod = 0; f.x = 0; f.y = 0; f.mouse = 0;
        f.wheel = 0; f.pending_w = 0; f.pending_h = 0;
    }

    int win_w() const { return f.width; }
    int win_h() const { return f.height; }
    int tile_px_x() const { return (TILE_PX_BASE_X * scale) / zoom_den; }
    int tile_px_y() const { return (TILE_PX_BASE_Y * scale) / zoom_den; }
    int vp_w_tiles() const { return win_w() / tile_px_x() + 2; }
    int vp_h_tiles() const { return (win_h() - HUD_PX) / tile_px_y() + 2; }
    int hud_y_px() const { return win_h() - HUD_PX; }

    void apply_pending_resize() {
        if (f.pending_w > 0 && f.pending_h > 0 &&
            (f.pending_w != f.width || f.pending_h != f.height)) {
            f.width = f.pending_w;
            f.height = f.pending_h;
            buf.assign((size_t)f.width * f.height, 0);
            fg_mask.assign((size_t)f.width * f.height, 0);
            f.buf = buf.data();
        }
        f.pending_w = 0;
        f.pending_h = 0;
    }

    void put_pixel(int x, int y, uint32_t c) {
        if ((unsigned)x >= (unsigned)f.width) return;
        if ((unsigned)y >= (unsigned)f.height) return;
        buf[(size_t)y * f.width + x] = c;
    }

    void fill_rect(int x, int y, int w, int h, uint32_t color) {
        int x0 = std::max(0, x), y0 = std::max(0, y);
        int x1 = std::min(f.width, x + w), y1 = std::min(f.height, y + h);
        for (int py = y0; py < y1; py++) {
            uint32_t* row = &buf[(size_t)py * f.width];
            for (int px = x0; px < x1; px++) row[px] = color;
        }
    }

    // 1-pixel outline rectangle, clipped to screen and HUD.
    void stroke_rect(int x, int y, int w, int h, uint32_t color) {
        int hud_y = hud_y_px();
        auto plot = [&](int px, int py) {
            if ((unsigned)px >= (unsigned)f.width) return;
            if (py < 0 || py >= hud_y) return;
            buf[(size_t)py * f.width + px] = color;
        };
        for (int i = 0; i < w; ++i) {
            plot(x + i, y);
            plot(x + i, y + h - 1);
        }
        for (int j = 0; j < h; ++j) {
            plot(x,         y + j);
            plot(x + w - 1, y + j);
        }
    }

    // Blit a sprite by reading logical colour indices (0..3) out of the
    // BBC 128x81 sheet and resolving them through `lut` per sprite pixel.
    // lut[0] is treated as transparent; only lut[1..3] are written.
    //
    // Iterates SCREEN pixels and box-samples the atlas pixels that fall
    // under each one. At 1:1 or zoom-in the box is a single atlas pixel
    // and this collapses to nearest-neighbour; at zoom-out the box covers
    // multiple atlas pixels and their RGB contributions are averaged,
    // blending against the existing frame buffer where transparent atlas
    // pixels partially cover the output pixel.
    // `fg` (per LUT slot) = 1 for pixels drawn with BBC logical-colour 8..15,
    // which the 6502 treats as foreground (see palette.cpp comments). When
    // `is_tile` is true such pixels set fg_mask. When false (object blit),
    // pixels with fg_mask[idx] already set are skipped — that's the 6502's
    // BMI past pre-existing foreground at &1066. Default behaviour (fg null)
    // leaves the mask untouched and doesn't read it, matching the old
    // blit_sprite signature.
    void blit_sprite(int dst_x, int dst_y, uint8_t sprite_id,
                     bool flip_h, bool flip_v, const uint32_t lut[4],
                     const uint8_t fg[4] = nullptr, bool is_tile = false,
                     uint8_t shrink_shift_x = 0,
                     uint8_t shrink_shift_y = 0) {
        if (sprite_id > 0x7c) return;
        const SpriteAtlasEntry& e = sprite_atlas[sprite_id];
        const int hud_y = hud_y_px();

        flip_h ^= (e.intrinsic_flip & 1) != 0;
        flip_v ^= (e.intrinsic_flip & 2) != 0;

        // Screen-pixel extents of the blit. Width/height collapse to 0
        // at extreme zoom-out; guard with max(1) so tiny sprites still
        // show at least one pixel instead of vanishing entirely.
        int w_screen = e.w * PX_SCALE_X * scale / zoom_den;
        int h_screen = e.h * PX_SCALE_Y * scale / zoom_den;
        // 6502 &0d21 teleport shrink: reduce the rendered extent so the
        // sprite fizzes down. Caller is responsible for centring via
        // dst_x / dst_y so the sprite shrinks toward its middle.
        if (shrink_shift_x) w_screen >>= shrink_shift_x;
        if (shrink_shift_y) h_screen >>= shrink_shift_y;
        if (w_screen < 1) w_screen = 1;
        if (h_screen < 1) h_screen = 1;

        // Mapping constants: atlas-pixel = screen-pixel * num / den.
        // When num > den (zoom-out) each screen pixel covers multiple
        // atlas pixels; the inner loops box-filter over that range.
        int sx_num = zoom_den;
        int sx_den = PX_SCALE_X * scale;
        int sy_num = zoom_den;
        int sy_den = PX_SCALE_Y * scale;

        for (int py = 0; py < h_screen; ++py) {
            int ppy = dst_y + py;
            if (ppy < 0 || ppy >= hud_y) continue;
            int ay0 = py * sy_num / sy_den;
            int ay1 = (py + 1) * sy_num / sy_den;
            if (ay1 <= ay0) ay1 = ay0 + 1;        // sample at least 1 row
            if (ay1 > e.h) ay1 = e.h;
            if (ay0 >= e.h) ay0 = e.h - 1;
            uint32_t* row      = &buf[(size_t)ppy * f.width];
            uint8_t*  fg_row   = &fg_mask[(size_t)ppy * f.width];
            for (int px = 0; px < w_screen; ++px) {
                int ppx = dst_x + px;
                if (ppx < 0 || ppx >= f.width) continue;

                // Object-plot skip: match the 6502's BMI past pre-existing
                // foreground at &1066. Only applies when blitting an object
                // (is_tile=false) and there's a fg lookup available.
                if (!is_tile && fg && fg_row[ppx]) continue;

                int ax0 = px * sx_num / sx_den;
                int ax1 = (px + 1) * sx_num / sx_den;
                if (ax1 <= ax0) ax1 = ax0 + 1;
                if (ax1 > e.w) ax1 = e.w;
                if (ax0 >= e.w) ax0 = e.w - 1;

                // Accumulate RGB contributions of non-transparent atlas
                // pixels in the [ax0,ax1) x [ay0,ay1) box. Track whether
                // any of the contributing pixels are foreground so tiles
                // can mark the fg_mask appropriately.
                int r_sum = 0, g_sum = 0, b_sum = 0;
                int count = 0;
                bool any_fg = false;
                for (int ay = ay0; ay < ay1; ++ay) {
                    int src_y = e.y + (flip_v ? (e.h - 1 - ay) : ay);
                    for (int ax = ax0; ax < ax1; ++ax) {
                        int src_x = e.x + (flip_h ? (e.w - 1 - ax) : ax);
                        uint8_t idx = bbc_sprite_pixel(src_x, src_y);
                        if (idx == 0) continue;   // 0 = transparent
                        uint32_t c = lut[idx];
                        r_sum += (c >> 16) & 0xff;
                        g_sum += (c >>  8) & 0xff;
                        b_sum +=  c        & 0xff;
                        if (fg && fg[idx]) any_fg = true;
                        ++count;
                    }
                }
                if (count == 0) continue;         // fully transparent box

                int total = (ay1 - ay0) * (ax1 - ax0);
                int sr = r_sum / count;
                int sg = g_sum / count;
                int sb = b_sum / count;

                if (count == total) {
                    // Fully covered — write the box average directly.
                    row[ppx] = 0xff000000u | (uint32_t(sr) << 16)
                                           | (uint32_t(sg) <<  8)
                                           |  uint32_t(sb);
                } else {
                    // Partial coverage — alpha-blend sprite-average over
                    // the existing pixel. alpha256 = 256 * count / total.
                    uint32_t existing = row[ppx];
                    int er = (existing >> 16) & 0xff;
                    int eg = (existing >>  8) & 0xff;
                    int eb =  existing        & 0xff;
                    int alpha = (count * 256) / total;
                    int inv   = 256 - alpha;
                    int or_ = (sr * alpha + er * inv) >> 8;
                    int og  = (sg * alpha + eg * inv) >> 8;
                    int ob  = (sb * alpha + eb * inv) >> 8;
                    row[ppx] = 0xff000000u | (uint32_t(or_) << 16)
                                           | (uint32_t(og)  <<  8)
                                           |  uint32_t(ob);
                }

                // Tiles mark any pixel that contributed a foreground colour
                // so subsequent object blits can skip past it. We set on
                // any_fg regardless of partial coverage — consistent with
                // the 6502's byte-granular BMI check.
                if (is_tile && any_fg) fg_row[ppx] = 1;
            }
        }
    }

    void draw_glyph(int x, int y, char ch, uint32_t fg, uint32_t bg) {
        unsigned uc = (unsigned char)ch;
        if (uc < 0x20 || uc > 0x7f) uc = ' ';
        const uint8_t* g = FONT8X8[uc - 0x20];
        for (int row = 0; row < 8; ++row) {
            uint8_t bits = g[row];
            for (int col = 0; col < 8; ++col) {
                uint32_t c = (bits & (1 << col)) ? fg : bg;
                if (c == 0xFF000000) continue; // magic: transparent bg
                put_pixel(x + col, y + row, c);
            }
        }
    }

    // Draw text; bg=0xFF000000 means transparent background.
    int draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg) {
        int cx = x;
        for (; *s; ++s) {
            if (*s == '\n') { y += 9; cx = x; continue; }
            draw_glyph(cx, y, *s, fg, bg);
            cx += 8;
        }
        return cx;
    }

    // Pan offsets in pixels; applied to world_to_screen.
    // Center tile's top-left is at (win_w/2, hud_y_px/2); other tiles offset
    // by (dx, dy) tiles from that. vp_w/h_tiles() overrenders by ±1 tile so
    // the screen edges stay covered during sub-tile pan.
    bool world_to_screen(uint8_t wx, uint8_t wy, int& sx, int& sy,
                         uint8_t wx_frac = 0, uint8_t wy_frac = 0) const {
        int dx = static_cast<int8_t>(wx - vp_center_x);
        int dy = static_cast<int8_t>(wy - vp_center_y);
        int tpx = tile_px_x();
        int tpy = tile_px_y();
        // Split the sub-tile offset into two independent floors instead of
        // one combined `floor((wx_frac - vp_frac_x) * tpx / 256)`. The
        // combined form snaps to a new pixel at different vp_frac_x values
        // depending on `wx_frac`: a tile (wx_frac=0) snaps at one set of
        // player-fraction crossings, an object (wx_frac=50) snaps at
        // shifted crossings, so as the player walks the relative offset
        // between a static object and the tile it sits on oscillates ±1px.
        // That's the single-pixel "swim" you'd see on stationary items
        // while the camera tracks the moving player.
        //
        // By computing the object's intra-tile pixel offset (`wx_frac * tpx /
        // 256`) independently of the viewport's, the offset between an
        // object and its tile becomes a constant `floor(wx_frac * tpx / 256)`
        // — both shift together when the player moves, no relative drift.
        int sub_x_view = floor_div_256(-int(vp_frac_x) * tpx);
        int sub_y_view = floor_div_256(-int(vp_frac_y) * tpy);
        int sub_x_obj  = floor_div_256( int(wx_frac)   * tpx);
        int sub_y_obj  = floor_div_256( int(wy_frac)   * tpy);
        sx = f.width / 2 + dx * tpx + sub_x_view + sub_x_obj + pan_px_x;
        sy = hud_y_px() / 2 + dy * tpy + sub_y_view + sub_y_obj + pan_px_y;
        return sx > -tpx && sx < f.width && sy > -tpy && sy < hud_y_px();
    }

    // Inverse of world_to_screen: screen pixel → world-tile offset from
    // the view center (camera center).
    //
    // world_to_screen shifts tiles by -vp_frac_*·tpx/256 so the world scrolls
    // under a fixed player sprite; the inverse must undo that shift, else
    // clicks snap to the wrong tile whenever the player is mid-cell.
    void screen_to_tile_offset(int sx, int sy, int& tdx, int& tdy) const {
        int tpx = tile_px_x();
        int tpy = tile_px_y();
        int frac_off_x = int(vp_frac_x) * tpx / 256;
        int frac_off_y = int(vp_frac_y) * tpy / 256;
        int rel_x = sx - f.width / 2 - pan_px_x + frac_off_x;
        int rel_y = sy - hud_y_px() / 2 - pan_px_y + frac_off_y;
        // Floor-division toward -inf so negative offsets map correctly.
        tdx = (rel_x >= 0) ? (rel_x / tpx) : -((-rel_x + tpx - 1) / tpx);
        tdy = (rel_y >= 0) ? (rel_y / tpy) : -((-rel_y + tpy - 1) / tpy);
    }

    void process_mouse() {
        // Right-drag panning
        bool right_down = (f.mouse & 2) != 0;
        if (right_down && right_was_down) {
            int dx = f.x - prev_mouse_x;
            int dy = f.y - prev_mouse_y;
            // Dragging right → world slides right → camera center moves LEFT.
            // Accumulate a pixel pan; when it exceeds a tile, emit tile step.
            pan_px_x += dx;
            pan_px_y += dy;
            int tpx = tile_px_x();
            int tpy = tile_px_y();
            while (pan_px_x >= tpx) { pan_px_x -= tpx; pending_pan_tiles_x -= 1; }
            while (pan_px_x <= -tpx) { pan_px_x += tpx; pending_pan_tiles_x += 1; }
            while (pan_px_y >= tpy) { pan_px_y -= tpy; pending_pan_tiles_y -= 1; }
            while (pan_px_y <= -tpy) { pan_px_y += tpy; pending_pan_tiles_y += 1; }
        }
        right_was_down = right_down;

        // Left-click edge detection. Debug-checkbox hits are intercepted
        // here so they don't get forwarded to Game::render as a world-
        // tile click. See the DebugCheckbox layout above for geometry.
        bool left_down = (f.mouse & 1) != 0;
        if (left_down && !left_was_down) {
            DebugCheckbox boxes[7] = {
                { "Grid",       &tile_outline_on },
                { "Map mode",   &map_mode_on     },
                { "Debug",      &debug_text_on   },
                { "Object lbl", &object_tiers_on },
                { "Switches",   &switches_on     },
                { "Transports", &transports_on   },
                { "Collision",  &collision_on    },
            };
            int hud_y = hud_y_px();
            int cy    = checkbox_slot_y(hud_y);
            bool consumed = false;
            for (int i = 0; i < 7; i++) {
                int cx = checkbox_slot_x(i);
                // Generous hit-area: the whole label's slot width, not
                // just the little box, so users can click the text too.
                int hx_end = cx + CHECKBOX_SLOT_W;
                int hy_end = hud_y + 16;
                if (f.x >= cx && f.x < hx_end &&
                    f.y >= hud_y && f.y < hy_end) {
                    *boxes[i].state = !*boxes[i].state;
                    consumed = true;
                    break;
                }
            }
            (void)cy;
            if (!consumed) {
                has_pending_click = true;
                pending_click_x = f.x;
                pending_click_y = f.y;
            }
        }
        left_was_down = left_down;

        prev_mouse_x = f.x;
        prev_mouse_y = f.y;
    }
};

// Tile type (0-0x3f) → atlas sprite_id. Bit 7 XORs flip_v (matches the
// original's tiles_sprite_and_y_flip_table at &04ab). Bit 7 set means the
// tile's obstruction sits at the bottom when flip_v from the landscape
// is 0. Entries pointing at sprite 0x46 (NONE, 1×1 transparent) are
// stored here as 0xff to skip the blit entirely.
static constexpr uint8_t TILE_SPRITE_ID[64] = {
    /* 0x00 */ 0xff,         /* 0x01 */ 0xce,         /* 0x02 */ 0xff,         /* 0x03 */ 0xff,
    /* 0x04 */ 0xff,         /* 0x05 */ 0xbb,         /* 0x06 */ 0xff,         /* 0x07 */ 0x18,
    /* 0x08 */ 0x2d,         /* 0x09 */ 0x70,         /* 0x0a */ 0x6a,         /* 0x0b */ 0xff,
    /* 0x0c */ 0x23,         /* 0x0d */ 0x39,         /* 0x0e */ 0xff,         /* 0x0f */ 0x62,
    /* 0x10 */ 0xc0,         /* 0x11 */ 0x8e,         /* 0x12 */ 0x39,         /* 0x13 */ 0x44,
    /* 0x14 */ 0x47,         /* 0x15 */ 0x26,         /* 0x16 */ 0x48,         /* 0x17 */ 0x49,
    /* 0x18 */ 0xdf,         /* 0x19 */ 0xff,         /* 0x1a */ 0x99,         /* 0x1b */ 0x9a,
    /* 0x1c */ 0x25,         /* 0x1d */ 0x2b,         /* 0x1e */ 0x39,         /* 0x1f */ 0x3b,
    /* 0x20 */ 0x3c,         /* 0x21 */ 0x55,         /* 0x22 */ 0x8e,         /* 0x23 */ 0x43,
    /* 0x24 */ 0x34,         /* 0x25 */ 0x35,         /* 0x26 */ 0x27,         /* 0x27 */ 0x28,
    /* 0x28 */ 0x29,         /* 0x29 */ 0x2a,         /* 0x2a */ 0x42,         /* 0x2b */ 0xbf,
    /* 0x2c */ 0x40,         /* 0x2d */ 0x3d,         /* 0x2e */ 0x38,         /* 0x2f */ 0x36,
    /* 0x30 */ 0x37,         /* 0x31 */ 0x3e,         /* 0x32 */ 0x33,         /* 0x33 */ 0x31,
    /* 0x34 */ 0x2f,         /* 0x35 */ 0x30,         /* 0x36 */ 0x2c,         /* 0x37 */ 0x24,
    /* 0x38 */ 0x32,         /* 0x39 */ 0x41,         /* 0x3a */ 0x45,         /* 0x3b */ 0x3a,
    /* 0x3c */ 0x6a,         /* 0x3d */ 0x23,         /* 0x3e */ 0x60,         /* 0x3f */ 0xcc,
};

static uint32_t object_color(ObjectType type) {
    switch (type) {
        case ObjectType::PLAYER:        return 0xFFFF00;
        case ObjectType::TRIAX:         return 0x00FF00;
        case ObjectType::RED_FROGMAN:   return 0xFF0000;
        case ObjectType::GREEN_FROGMAN: return 0x00FF00;
        case ObjectType::RED_SLIME:     return 0xFF0000;
        case ObjectType::GREEN_SLIME:   return 0x00FF00;
        case ObjectType::YELLOW_SLIME:  return 0xFFFF00;
        case ObjectType::EXPLOSION:     return 0xFF4400;
        case ObjectType::FIREBALL:
        case ObjectType::MOVING_FIREBALL: return 0xFF2200;
        case ObjectType::DESTINATOR:    return 0xFF00FF;
        case ObjectType::POWER_POD:     return 0x00FFFF;
        default:                        return 0xFFFFFF;
    }
}

PixelRenderer::PixelRenderer() : impl_(std::make_unique<Impl>()) {}
PixelRenderer::~PixelRenderer() { shutdown(); }

bool PixelRenderer::init() {
    if (impl_->initialized) return true;
    if (fenster_open(&impl_->f) != 0) return false;
    // fenster.h's WNDCLASSEX leaves hCursor = NULL, which makes Windows
    // fall back to the "busy" cursor whenever the OS decides this is a
    // fresh app. Force the arrow on our window class so the pointer is
    // normal from the first frame.
    SetClassLongPtrA(impl_->f.hwnd, GCLP_HCURSOR,
                     reinterpret_cast<LONG_PTR>(LoadCursorA(NULL, IDC_ARROW)));
    impl_->initialized = true;
    return true;
}

void PixelRenderer::shutdown() {
    if (impl_->initialized) {
        fenster_close(&impl_->f);
        impl_->initialized = false;
    }
}

void PixelRenderer::begin_frame() {
    // Pump pending Windows messages first so mouse/size state is current.
    if (fenster_loop(&impl_->f) != 0) impl_->should_close = true;
    impl_->events_processed = true;
    impl_->key_scan_idx = 0;

    impl_->apply_pending_resize();

    if (impl_->f.wheel != 0) {
        int w = impl_->f.wheel;
        impl_->f.wheel = 0;

        // Anchor the zoom on the tile under the mouse pointer: record the
        // screen-space offset from the view centre before the zoom change,
        // then rescale it to the new tile pitch so the same world point
        // stays under the cursor. See world_to_screen for the mapping
        //   screen = centre + (wx - vp_centre) * tpx + pan_px
        // — holding world-coord fixed, pan_px_new = (mouse - centre) -
        //   (mouse - centre - pan_px_old) * tpx_new / tpx_old.
        int mx = impl_->f.x;
        int my = impl_->f.y;
        int cx = impl_->f.width / 2;
        int cy = impl_->hud_y_px() / 2;
        int tpx_old = impl_->tile_px_x();
        int tpy_old = impl_->tile_px_y();
        int off_x   = mx - cx - impl_->pan_px_x;
        int off_y   = my - cy - impl_->pan_px_y;

        // Treat the zoom ladder as a single signed axis. When zooming in
        // past 1:1 we grow `scale`; below 1:1 we grow `zoom_den` so the
        // tile/sprite pitch shrinks further. They never both exceed 1
        // simultaneously.
        while (w > 0) {
            if (impl_->zoom_den > 1) impl_->zoom_den--;
            else if (impl_->scale < MAX_SCALE) impl_->scale++;
            --w;
        }
        while (w < 0) {
            if (impl_->scale > MIN_SCALE) impl_->scale--;
            else if (impl_->zoom_den < MAX_ZOOM_DEN) impl_->zoom_den++;
            ++w;
        }

        int tpx_new = impl_->tile_px_x();
        int tpy_new = impl_->tile_px_y();
        if (tpx_old > 0 && tpy_old > 0) {
            impl_->pan_px_x = (mx - cx) - off_x * tpx_new / tpx_old;
            impl_->pan_px_y = (my - cy) - off_y * tpy_new / tpy_old;
        }
        // Normalize: any pan past a whole tile feeds back into the
        // pending-pan queue (matches the drag-pan bookkeeping), keeping
        // pan_px_* inside one tile of the new zoom.
        while (impl_->pan_px_x >=  tpx_new) { impl_->pan_px_x -= tpx_new; impl_->pending_pan_tiles_x -= 1; }
        while (impl_->pan_px_x <= -tpx_new) { impl_->pan_px_x += tpx_new; impl_->pending_pan_tiles_x += 1; }
        while (impl_->pan_px_y >=  tpy_new) { impl_->pan_px_y -= tpy_new; impl_->pending_pan_tiles_y -= 1; }
        while (impl_->pan_px_y <= -tpy_new) { impl_->pan_px_y += tpy_new; impl_->pending_pan_tiles_y += 1; }
    }

    impl_->process_mouse();

    // Debug checkboxes in the bottom HUD strip handle grid / object
    // tiers / map-mode toggles (was G / T / W key-edges). Those letters
    // are now used by the 6502-faithful player actions (G=retrieve,
    // T=teleport, W = free). See render_debug_panel + hit-test below.

    // Debug: toggle AABB overlay on rising edge 'B'. Draws pixel-precise
    // bounding boxes (sprite w × h in sub-tile units) for each primary so
    // we can see whether the player and switch/door actually overlap.
    bool b_down = impl_->f.keys['B'] != 0;
    if (b_down && !impl_->aabb_key_prev) {
        impl_->aabb_overlay_on = !impl_->aabb_overlay_on;
    }
    impl_->aabb_key_prev = b_down;

    std::fill(impl_->buf.begin(), impl_->buf.end(), 0u);
    std::fill(impl_->fg_mask.begin(), impl_->fg_mask.end(), 0u);
}

void PixelRenderer::end_frame() {
    // Draw overlay text in top-right corner. Gated on the "Debug"
    // checkbox — Game still feeds overlay strings via set_overlay_text
    // (tile-info banner, map-mode diagnostics, etc.) but they only
    // actually get drawn when the Debug box is checked.
    if (impl_->debug_text_on && !impl_->overlay.empty()) {
        int line_count = 1;
        int max_line_w = 0;
        int cur_w = 0;
        for (char c : impl_->overlay) {
            if (c == '\n') { line_count++; max_line_w = std::max(max_line_w, cur_w); cur_w = 0; }
            else cur_w += 8;
        }
        max_line_w = std::max(max_line_w, cur_w);
        int pad = 4;
        int bx = impl_->f.width - max_line_w - pad * 2;
        int by = 2;
        int bh = line_count * 9 + pad;
        impl_->fill_rect(bx, by, max_line_w + pad * 2, bh, 0x000000);
        for (int x = bx; x < bx + max_line_w + pad * 2; ++x) {
            impl_->put_pixel(x, by, 0x666666);
            impl_->put_pixel(x, by + bh - 1, 0x666666);
        }
        for (int y = by; y < by + bh; ++y) {
            impl_->put_pixel(bx, y, 0x666666);
            impl_->put_pixel(bx + max_line_w + pad * 2 - 1, y, 0x666666);
        }
        impl_->draw_text(bx + pad, by + pad, impl_->overlay.c_str(),
                         0xFFFFFF, 0xFF000000);
    }

    InvalidateRect(impl_->f.hwnd, NULL, FALSE);
    UpdateWindow(impl_->f.hwnd);
}

void PixelRenderer::set_viewport(uint8_t center_x, uint8_t center_y,
                                    uint8_t frac_x, uint8_t frac_y) {
    impl_->vp_center_x = center_x;
    impl_->vp_center_y = center_y;
    impl_->vp_frac_x   = frac_x;
    impl_->vp_frac_y   = frac_y;
}

void PixelRenderer::render_tile(uint8_t world_x, uint8_t world_y,
                                   const TileRenderInfo& info) {
    int sx, sy;
    if (!impl_->world_to_screen(world_x, world_y, sx, sy)) return;

    uint8_t entry = TILE_SPRITE_ID[info.tile_type & 0x3f];
    uint8_t sid = entry & 0x7f;
    // Bit 7 of TILE_SPRITE_ID mirrors the &04ab flag that marks the tile's
    // obstruction as being at the bottom — it's used for COLLISION (&2477)
    // not rendering. Rendering flip comes from the landscape's tile_flip
    // XOR the sprite's intrinsic flip (applied inside blit_sprite).
    if (entry != 0xff && sid <= 0x7c) {
        // Compute sub-tile offset so the sprite aligns to the correct half
        // of the cell when flipped — matches &2420-&243f in the disassembly.
        const SpriteAtlasEntry& e = sprite_atlas[sid];
        int base_y_atlas = (tiles_y_offset_and_pattern[info.tile_type & 0x3f] >> 4) * 2;

        int y_off_atlas = info.flip_v
            ? (32 - base_y_atlas - e.h)
            : base_y_atlas;
        int x_off_atlas = info.flip_h ? (16 - e.w) : 0;

        int x_off_px = x_off_atlas * PX_SCALE_X * impl_->scale / impl_->zoom_den;
        int y_off_px = y_off_atlas * PX_SCALE_Y * impl_->scale / impl_->zoom_den;

        // Tiles draw with the foreground mask (&ff) — full 16-colour range.
        // `fg` per-slot tells blit_sprite which pixels should mark the
        // foreground buffer (colours 8..15 in the BBC logical palette).
        uint32_t lut[4];
        uint8_t  fg[4];
        resolve_palette_with_fg(info.palette, /*is_tile=*/true, lut, fg);
        impl_->blit_sprite(sx + x_off_px, sy + y_off_px, sid,
                           info.flip_h, info.flip_v, lut, fg,
                           /*is_tile=*/true);
    }

    if (impl_->tile_outline_on) {
        int tpx = impl_->tile_px_x();
        int tpy = impl_->tile_px_y();
        // Highlighted tile gets a brighter outline on top of the grey grid,
        // doubled up for visibility at high zoom.
        bool highlighted = impl_->has_highlight
                           && impl_->highlight_x == world_x
                           && impl_->highlight_y == world_y;
        impl_->stroke_rect(sx, sy, tpx, tpy, 0x404040);
        if (highlighted) {
            impl_->stroke_rect(sx,     sy,     tpx,     tpy,     0xFFEE33);
            impl_->stroke_rect(sx + 1, sy + 1, tpx - 2, tpy - 2, 0xFFEE33);
        }
    }
}

// Port of the 6502 raster-palette swap at &12a6-&12d8. Instead of
// reprogramming VDU colour 0 at scanline boundaries (not possible in a
// framebuffer renderer), we pre-fill the screen behind the tile blits:
// pixels that end up logical-colour 0 are drawn transparent in
// blit_sprite, so whatever we paint here shows through.
//
// Above the waterline: leave whatever begin_frame cleared to (black).
// On the waterline:    one pixel row of cyan (=&06), matching the 1-line
//                      delay_loop wait in the IRQ handler.
// Below the waterline: blue (=&04) all the way to the bottom of this
//                      tile row.
void PixelRenderer::render_water_column(uint8_t world_x,
                                          uint8_t waterline_y) {
    int tpx = impl_->tile_px_x();

    // Port of &16db calculate_waterline_timer's tri-state decision:
    //   &16ec BCC: waterline_y < screen_origin_y → entire screen below
    //              waterline (raster fires immediately, colour 0 is blue
    //              for the whole scan).
    //   &16f0 BCS (delta >= screen_height): entire screen above waterline
    //              (timer never fires, colour 0 stays black).
    //   Else: waterline is inside the visible area — raster fires at that
    //         scanline, cyan for one line, blue afterwards.
    //
    // Crucial: do the subtraction in signed int arithmetic, NOT uint8_t.
    // The 6502's `SBC screen_origin_y` is strictly 8-bit unsigned, but
    // that's only safe because the BBC camera can never pan outside the
    // playable range — so screen_origin_y never wraps past 0. Our map-mode
    // viewport pans freely, so computing `vp_top_y = vp_center_y − vp_h/2`
    // with a uint8_t underflows at the top of the map (0x03 → 0xF9) and
    // flips the unsigned comparison's meaning: every column shows as
    // submerged even though the waterline is far below.
    int vp_h = impl_->vp_h_tiles();
    int vp_top_y = int(impl_->vp_center_y) - vp_h / 2;   // may be negative
    int water    = int(waterline_y);
    int delta_from_top = water - vp_top_y;

    // Use physical BBC MODE 2 hues so the "mid-frame palette swap" lands on
    // the same RGB values as a colour-0 pixel would if the palette register
    // had been rewritten. LOGICAL_TO_RGB[4]=blue, [6]=cyan.
    const uint32_t BLUE = LOGICAL_TO_RGB[4];
    const uint32_t CYAN = LOGICAL_TO_RGB[6];

    int hud_y = impl_->hud_y_px();
    int top_sx = 0, top_sy = 0;
    (void)impl_->world_to_screen(world_x, waterline_y, top_sx, top_sy);
    if (top_sx + tpx <= 0 || top_sx >= impl_->f.width) return;

    if (delta_from_top < 0) {
        // Waterline above screen top → entire column submerged → blue.
        impl_->fill_rect(top_sx, 0, tpx, hud_y, BLUE);
    } else if (delta_from_top < vp_h) {
        // Waterline inside viewport. Cyan on the waterline row's top
        // scanline, blue below.
        if (top_sy + 1 < hud_y) {
            int below_y0 = top_sy + 1;
            if (below_y0 < 0) below_y0 = 0;
            impl_->fill_rect(top_sx, below_y0, tpx, hud_y - below_y0, BLUE);
        }
        if (top_sy >= 0 && top_sy < hud_y) {
            impl_->fill_rect(top_sx, top_sy, tpx, 1, CYAN);
        }
    }
    // else: entire column above waterline → leave black (begin_frame
    // cleared to 0 already).
}

void PixelRenderer::render_object(Fixed8_8 world_x, Fixed8_8 world_y,
                                     const SpriteRenderInfo& info) {
    if (!info.visible) return;
    int sx, sy;
    if (!impl_->world_to_screen(world_x.whole, world_y.whole, sx, sy,
                                world_x.fraction, world_y.fraction)) return;

    if (info.sprite_id <= 0x7c) {
        // Object position (x, y) is the sprite's TOP-left in world
        // coordinates, matching the 6502 at &0d91: screen_x = object_x -
        // screen_start_x with no sprite-height offset. Tertiary spawns
        // pre-compute an x_frac / y_frac that place the sprite in the
        // intended half of the tile when flipped, so anchoring at the top
        // is the one that keeps their half-of-cell layout correct.
        //
        // Pass an `fg` array of all-zero entries so blit_sprite performs
        // the foreground-skip check against fg_mask: where a tile has
        // already marked a pixel foreground (BBC logical-colour 8..15)
        // the object pixel is skipped — the 6502 "BMI skip_byte" at
        // &1066 that hides objects behind foliage / spaceship overlays.
        uint32_t lut[4];
        uint8_t  fg[4] = {0, 0, 0, 0};
        resolve_palette(info.palette, /*is_tile=*/false, lut);

        // Port of &0cfe reduce_sprite_if_teleporting. Y uses (timer & 7) as
        // shift count; X uses ((timer * 5/4) & 7). With timer counting down
        // from 0x20 across 32 frames this strobes the sprite between full
        // size (shift=0) and nearly invisible (shift=7), which on the BBC
        // reads as a "fizzing out" effect. Centre the shrunk sprite on the
        // original footprint so it doesn't drift.
        uint8_t shrink_x = 0, shrink_y = 0;
        int dx_shrink = 0, dy_shrink = 0;
        if (info.teleport_timer != 0 && info.sprite_id <= 0x7c) {
            uint8_t t = info.teleport_timer;
            shrink_y = static_cast<uint8_t>(t & 0x07);
            uint8_t t_x = static_cast<uint8_t>(t + (t >> 2));
            shrink_x = static_cast<uint8_t>(t_x & 0x07);
            const SpriteAtlasEntry& e = sprite_atlas[info.sprite_id];
            int tpx = impl_->tile_px_x();
            int tpy = impl_->tile_px_y();
            int w_full = e.w * tpx / 16;  // 1 tile = 16 atlas-px wide
            int h_full = e.h * tpy / 8;   // 1 tile = 8  atlas-px tall
            int w_shr  = w_full >> shrink_x; if (w_shr < 1) w_shr = 1;
            int h_shr  = h_full >> shrink_y; if (h_shr < 1) h_shr = 1;
            dx_shrink = (w_full - w_shr) / 2;
            dy_shrink = (h_full - h_shr) / 2;
        }

        impl_->blit_sprite(sx + dx_shrink, sy + dy_shrink, info.sprite_id,
                           info.flip_h, info.flip_v, lut, fg,
                           /*is_tile=*/false,
                           shrink_x, shrink_y);
    } else {
        uint32_t col = object_color(info.type);
        int tx = impl_->tile_px_x();
        int ty = impl_->tile_px_y();
        impl_->fill_rect(sx + 1, sy + 1, tx - 2, ty - 2, col);
    }
}

void PixelRenderer::render_hud(const PlayerState& player) {
    int hud_y = impl_->hud_y_px();

    // Debug-panel strip across the bottom — replaces the old energy bar /
    // weapon swatch with three click-to-toggle checkboxes. Geometry lives
    // in the CHECKBOX_* constants + checkbox_slot_x/y so the click hit-
    // test in process_mouse uses the same layout.
    impl_->fill_rect(0, hud_y, impl_->f.width, 16, 0x151515);
    DebugCheckbox boxes[7] = {
        { "Grid",       &impl_->tile_outline_on },
        { "Map mode",   &impl_->map_mode_on     },
        { "Debug",      &impl_->debug_text_on   },
        { "Object lbl", &impl_->object_tiers_on },
        { "Switches",   &impl_->switches_on     },
        { "Transports", &impl_->transports_on   },
        { "Collision",  &impl_->collision_on    },
    };
    int cy = checkbox_slot_y(hud_y);
    for (int i = 0; i < 7; i++) {
        int cx = checkbox_slot_x(i);
        uint32_t border = *boxes[i].state ? 0xffffff : 0x666666;
        impl_->stroke_rect(cx, cy, CHECKBOX_SIZE, CHECKBOX_SIZE, border);
        if (*boxes[i].state) {
            impl_->fill_rect(cx + 2, cy + 2,
                             CHECKBOX_SIZE - 4, CHECKBOX_SIZE - 4,
                             0x44CC44);
        }
        impl_->draw_text(cx + CHECKBOX_SIZE + CHECKBOX_LABEL_GAP,
                         cy + (CHECKBOX_SIZE - 8) / 2,
                         boxes[i].label, 0xdddddd, 0x000000);
    }

    // Player-state HUD fields the user still wants visible (energy +
    // selected weapon) are now suppressed in the debug-panel build.
    // Reinstate here later if gameplay needs the energy bar back.
    (void)player;

    // Top-left HUD strip: POCKETS | KEYS | WEAPONS panels laid out
    // horizontally. Sprites render at a fixed BBC 2:1 aspect, independent
    // of world zoom.
    static constexpr int CELL       = 28;
    static constexpr int CELL_PAD   = 4;
    static constexpr int PANEL_GAP  = 16;
    static constexpr int ORIGIN_X   = 4;
    static constexpr int ORIGIN_Y   = 4;
    static constexpr int LABEL_H    = 9;
    int cells_y = ORIGIN_Y + LABEL_H;

    // POCKETS — slot 0 ("top" of the stack, next to retrieve) drawn leftmost.
    int px = ORIGIN_X;
    impl_->draw_text(px, ORIGIN_Y, "POCKETS", 0xFFFFFF, 0x000000);
    for (int i = 0; i < 5; i++) {
        int cx = px + i * (CELL + CELL_PAD);
        uint8_t ot = player.pockets[i];
        impl_->fill_rect(cx, cells_y, CELL, CELL, 0x000000);
        uint32_t border = (ot == 0xff) ? 0x333333 : 0x888888;
        impl_->stroke_rect(cx, cells_y, CELL, CELL, border);
        blit_obj_sprite_cell(cx, cells_y, CELL, ot, /*dim=*/false);
    }
    px += 5 * (CELL + CELL_PAD) + PANEL_GAP;

    // KEYS — six collectable keys, indices match player_keys_collected_[0..5]
    // (CYAN_YELLOW_GREEN_KEY=0x51 .. BLUE_CYAN_GREEN_KEY=0x57, skipping 0x55).
    static constexpr uint8_t KEY_TYPES[6] = {
        0x51, 0x52, 0x53, 0x54, 0x56, 0x57,
    };
    impl_->draw_text(px, ORIGIN_Y, "KEYS", 0xFFFFFF, 0x000000);
    for (int i = 0; i < 6; i++) {
        int cx = px + i * (CELL + CELL_PAD);
        bool have = (player.keys[i] & 0x80) != 0;
        impl_->fill_rect(cx, cells_y, CELL, CELL, 0x000000);
        uint32_t border = have ? 0x888888 : 0x333333;
        impl_->stroke_rect(cx, cells_y, CELL, CELL, border);
        blit_obj_sprite_cell(cx, cells_y, CELL, KEY_TYPES[i],
                             /*dim=*/!have);
    }
    px += 6 * (CELL + CELL_PAD) + PANEL_GAP;

    // WEAPONS — slot 0 jetpack, 1 pistol, 2 icer, 3 blaster, 4 plasma. The
    // 6502 weapon table also has slot 5 (suit) but it's never user-selectable
    // so we leave it off the HUD. Selected slot is highlighted; the bottom
    // strip shows energy as a 0..0x800 scaled bar (0x800 = full pip).
    static constexpr uint8_t WEAPON_TYPES[5] = {
        0x59, // JETPACK_BOOSTER
        0x5a, // PISTOL
        0x5b, // ICER
        0x5c, // BLASTER
        0x5d, // PLASMA_GUN
    };
    impl_->draw_text(px, ORIGIN_Y, "WEAPONS", 0xFFFFFF, 0x000000);
    for (int i = 0; i < 5; i++) {
        int cx = px + i * (CELL + CELL_PAD);
        bool selected = (player.weapon == i);
        uint16_t energy = player.weapon_energy[i];
        bool have = energy > 0;
        impl_->fill_rect(cx, cells_y, CELL, CELL, 0x000000);
        uint32_t border = selected ? 0xFFFF44
                                   : (have ? 0x888888 : 0x333333);
        impl_->stroke_rect(cx, cells_y, CELL, CELL, border);
        blit_obj_sprite_cell(cx, cells_y, CELL, WEAPON_TYPES[i],
                             /*dim=*/!have);
        // Energy bar along the bottom edge — full width at 0x800, clamped.
        int max_w = CELL - 4;
        int bar_w = (energy >= 0x800) ? max_w
                                      : (int(energy) * max_w) / 0x800;
        if (bar_w > 0) {
            uint32_t bar_col = selected ? 0xFFCC44 : 0x44AA44;
            impl_->fill_rect(cx + 2, cells_y + CELL - 4, bar_w, 2, bar_col);
        }
    }
}

void PixelRenderer::blit_obj_sprite_cell(int cx, int cy, int cell_size,
                                         uint8_t obj_type, bool dim) {
    if (obj_type == 0xff) return;
    uint8_t sprite_id = object_types_sprite[obj_type];
    if (sprite_id > 0x7c) return;
    const SpriteAtlasEntry& e = sprite_atlas[sprite_id];
    uint32_t lut[4];
    resolve_palette(object_types_palette_and_pickup[obj_type] & 0x7f,
                    /*is_tile=*/false, lut);
    int sprite_w_px = e.w * 2;   // BBC 2:1 horizontal aspect
    int sprite_h_px = e.h;
    int blit_x = cx + (cell_size - sprite_w_px) / 2;
    int blit_y = cy + (cell_size - sprite_h_px) / 2;
    bool flip_h = (e.intrinsic_flip & 1) != 0;
    bool flip_v = (e.intrinsic_flip & 2) != 0;
    for (int sy = 0; sy < e.h; sy++) {
        int src_y = e.y + (flip_v ? (e.h - 1 - sy) : sy);
        for (int sx = 0; sx < e.w; sx++) {
            int src_x = e.x + (flip_h ? (e.w - 1 - sx) : sx);
            uint8_t idx = bbc_sprite_pixel(src_x, src_y);
            if (idx == 0) continue;
            uint32_t col = lut[idx];
            // Dim by halving each channel — simple and palette-agnostic.
            if (dim) col = (col >> 2) & 0x3F3F3F;
            int ox = blit_x + sx * 2;
            int oy = blit_y + sy;
            impl_->put_pixel(ox,     oy, col);
            impl_->put_pixel(ox + 1, oy, col);
        }
    }
}

void PixelRenderer::render_particle(uint8_t wx, uint8_t wx_frac,
                                       uint8_t wy, uint8_t wy_frac,
                                       uint8_t colour) {
    // Particles use the same 16-slot logical palette as sprites/tiles, keyed
    // by their low 3 bits (BBC background colour group).
    uint32_t col = LOGICAL_TO_RGB[colour & 0x07];
    if (col == 0) return; // black → invisible

    int sx, sy;
    if (!impl_->world_to_screen(wx, wy, sx, sy, wx_frac, wy_frac)) return;
    int s = impl_->scale;
    impl_->fill_rect(sx, sy, s, s, col);
}

int PixelRenderer::viewport_width_tiles() const { return impl_->vp_w_tiles(); }
int PixelRenderer::viewport_height_tiles() const { return impl_->vp_h_tiles(); }

bool PixelRenderer::consume_pan_tiles(int& dx, int& dy) {
    dx = impl_->pending_pan_tiles_x;
    dy = impl_->pending_pan_tiles_y;
    impl_->pending_pan_tiles_x = 0;
    impl_->pending_pan_tiles_y = 0;
    return dx != 0 || dy != 0;
}

bool PixelRenderer::consume_left_click(int& tile_dx, int& tile_dy) {
    if (!impl_->has_pending_click) { tile_dx = 0; tile_dy = 0; return false; }
    impl_->has_pending_click = false;
    impl_->screen_to_tile_offset(impl_->pending_click_x,
                                 impl_->pending_click_y,
                                 tile_dx, tile_dy);
    return true;
}

void PixelRenderer::set_overlay_text(const char* text) {
    impl_->overlay = text ? text : "";
}

void PixelRenderer::set_highlighted_tile(uint8_t world_x, uint8_t world_y) {
    impl_->has_highlight = true;
    impl_->highlight_x = world_x;
    impl_->highlight_y = world_y;
}

void PixelRenderer::render_activation_overlay(uint8_t anchor_x,
                                                 uint8_t anchor_y) {
    // Piggyback on the tile-grid toggle (G) so all the debug overlays share
    // one key. Nothing to draw otherwise.
    if (!impl_->tile_outline_on) return;

    // Ring radii taken from ObjectManager's lifecycle decisions:
    //   r = 1  — KEEP_AS_TERTIARY without KEEP_AS_PRIMARY_FOR_LONGER (flags
    //            &50): objects return to tertiary the moment they leave
    //            this box.
    //   r = 4  — standard demotion / promotion distance. Secondary objects
    //            within this get promoted back to primary.
    //   r = 12 — KEEP_AS_PRIMARY_FOR_LONGER extended range.
    struct Ring { int r; uint32_t rgb; const char* label; };
    static constexpr Ring RINGS[] = {
        { 1,  0xFF3333, "1" },
        { 4,  0xFFDD33, "4" },
        { 12, 0x33DD55, "12" },
    };

    int tpx = impl_->tile_px_x();
    int tpy = impl_->tile_px_y();
    int sx_anchor, sy_anchor;
    // Render the anchor cell itself as a small cross so you can always find
    // it even at high zoom-out. world_to_screen clips offscreen points to
    // false, but we still want to draw the rings if the anchor is *near*
    // the edge — recompute geometrically.
    (void)impl_->world_to_screen(anchor_x, anchor_y, sx_anchor, sy_anchor);

    for (const Ring& ring : RINGS) {
        int w = (2 * ring.r + 1) * tpx;
        int h = (2 * ring.r + 1) * tpy;
        int x = sx_anchor - ring.r * tpx;
        int y = sy_anchor - ring.r * tpy;
        impl_->stroke_rect(x, y, w, h, ring.rgb);
        // Double up for visibility at high zoom.
        impl_->stroke_rect(x + 1, y + 1, w - 2, h - 2, ring.rgb);
        // Label the ring at its top-left corner.
        impl_->draw_text(x + 2, y + 2, ring.label, ring.rgb, 0x000000);
    }

    // Crosshair on the anchor cell itself.
    impl_->fill_rect(sx_anchor + tpx / 2 - 1, sy_anchor, 2, tpy, 0xFFFFFF);
    impl_->fill_rect(sx_anchor, sy_anchor + tpy / 2 - 1, tpx, 2, 0xFFFFFF);
}

bool PixelRenderer::aabb_overlay_enabled() const {
    return impl_->aabb_overlay_on;
}

bool PixelRenderer::tile_grid_enabled()    const { return impl_->tile_outline_on; }
bool PixelRenderer::object_tiers_enabled() const { return impl_->object_tiers_on; }
bool PixelRenderer::map_mode_enabled()     const { return impl_->map_mode_on;     }
bool PixelRenderer::switches_enabled()     const { return impl_->switches_on;     }
bool PixelRenderer::transports_enabled()   const { return impl_->transports_on;   }
bool PixelRenderer::collision_enabled()    const { return impl_->collision_on;    }

void PixelRenderer::render_tile_shade_rect(uint8_t world_x, uint8_t world_y,
                                             uint8_t x_frac, uint8_t y_frac,
                                             uint8_t w_frac, uint8_t h_frac,
                                             uint32_t rgb) {
    int sx0, sy0;
    if (!impl_->world_to_screen(world_x, world_y, sx0, sy0,
                                 x_frac, y_frac)) return;
    int tpx = impl_->tile_px_x();
    int tpy = impl_->tile_px_y();
    // Fraction → screen-pixel conversion. Integer-round so adjacent
    // sub-sections tile without gaps, and floor-to-1 so one-frac bars
    // stay visible at low zoom.
    int w_px = (static_cast<int>(w_frac) * tpx + 128) / 256;
    int h_px = (static_cast<int>(h_frac) * tpy + 128) / 256;
    if (w_px < 1) w_px = 1;
    if (h_px < 1) h_px = 1;
    impl_->fill_rect(sx0, sy0, w_px, h_px, rgb);
}

// Bresenham between the centres of two world tiles, plus a small
// arrowhead at (x2, y2). Tile-whole inputs only — game layer decides
// which tile each endpoint sits on.
void PixelRenderer::render_wire(uint8_t x1, uint8_t y1,
                                uint8_t x2, uint8_t y2, uint32_t rgb) {
    int tpx = impl_->tile_px_x();
    int tpy = impl_->tile_px_y();
    int sx1, sy1, sx2, sy2;
    // world_to_screen returns false when the point is off-screen, but we
    // still want to draw lines whose endpoints are off-screen as long as
    // the line crosses the visible area. Use the raw screen mapping and
    // rely on per-pixel clipping in put_pixel.
    (void)impl_->world_to_screen(x1, y1, sx1, sy1);
    (void)impl_->world_to_screen(x2, y2, sx2, sy2);
    int cx1 = sx1 + tpx / 2;
    int cy1 = sy1 + tpy / 2;
    int cx2 = sx2 + tpx / 2;
    int cy2 = sy2 + tpy / 2;

    int hud_y = impl_->hud_y_px();
    int dx = std::abs(cx2 - cx1);
    int dy = -std::abs(cy2 - cy1);
    int sx_step = (cx1 < cx2) ? 1 : -1;
    int sy_step = (cy1 < cy2) ? 1 : -1;
    int err = dx + dy;
    int x = cx1, y = cy1;
    // Safety ceiling so a badly scaled wire can't spin here forever.
    int budget = impl_->f.width + hud_y + 4;
    while (budget-- > 0) {
        if (x >= 0 && x < impl_->f.width && y >= 0 && y < hud_y) {
            impl_->put_pixel(x,     y,     rgb);
            impl_->put_pixel(x + 1, y,     rgb);
            impl_->put_pixel(x,     y + 1, rgb);
        }
        if (x == cx2 && y == cy2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx_step; }
        if (e2 <= dx) { err += dx; y += sy_step; }
    }

    // Small square at the destination makes switch → door pairs easy to
    // read when several wires overlap.
    int hs = 3;
    impl_->fill_rect(cx2 - hs, cy2 - hs, 2 * hs + 1, 2 * hs + 1, rgb);
}

void PixelRenderer::render_aabb(Fixed8_8 world_x, Fixed8_8 world_y,
                                  int w_units, int h_units, uint32_t rgb) {
    // w_units / h_units are in 1/256 of a tile (matches x.fraction /
    // y.fraction arithmetic). The top-left lives at (world_x, world_y);
    // bottom-right at (world_x + w_units, world_y + h_units).
    int sx0, sy0;
    if (!impl_->world_to_screen(world_x.whole, world_y.whole, sx0, sy0,
                                 world_x.fraction, world_y.fraction)) return;
    int tpx = impl_->tile_px_x();
    int tpy = impl_->tile_px_y();
    int w_px = w_units * tpx / 256;
    int h_px = h_units * tpy / 256;
    if (w_px < 1) w_px = 1;
    if (h_px < 1) h_px = 1;
    impl_->stroke_rect(sx0, sy0, w_px, h_px, rgb);
    // Double up the rectangle so it's visible at low zoom.
    impl_->stroke_rect(sx0 + 1, sy0 + 1, w_px - 2, h_px - 2, rgb);
}

void PixelRenderer::render_debug_marker(uint8_t world_x, uint8_t world_y,
                                          uint32_t rgb, const char* label) {
    if (!impl_->object_tiers_on) return;
    int sx, sy;
    if (!impl_->world_to_screen(world_x, world_y, sx, sy)) return;
    // Anchor the swatch at the top-left corner of the cell so multiple
    // markers on the same tile stack predictably.
    int sz = 6 + impl_->scale;                   // swatch size grows with zoom
    impl_->fill_rect(sx + 2, sy + 2, sz, sz, rgb);
    impl_->stroke_rect(sx + 2, sy + 2, sz, sz, 0x000000);
    if (label && *label) {
        // Black background behind text keeps it readable against sprites.
        impl_->draw_text(sx + 2 + sz + 2, sy + 2, label, 0xFFFFFF, 0x000000);
    }
}

int PixelRenderer::get_key() {
    if (impl_->should_close) return 'q';

    // Fenster's key scan uses indices 0..256 where keys[9] is Tab and
    // modifiers land in f.mod (bit 0 = ctrl). The mod bitmask doesn't
    // distinguish left vs right ctrl, so when ctrl is flagged we probe
    // the OS directly to emit CTRL_LEFT or CTRL_RIGHT separately.
    //
    // Indices 256..258 are synthetic sentinels for those three keys so
    // the caller's polling loop can receive each independently in a
    // single frame without colliding with a real keys[] entry.
    while (impl_->key_scan_idx < 259) {
        int i = impl_->key_scan_idx++;

        if (i < 256) {
            if (!impl_->f.keys[i]) continue;
            switch (i) {
                case 9:  return InputKey::TAB;
                case 17: return InputKey::UP;
                case 18: return InputKey::DOWN;
                case 19: return InputKey::RIGHT;
                case 20: return InputKey::LEFT;
                case 10: return InputKey::ENTER;
                case 27: return 'q';
                default:
                    // Letters: lowercase so input.cpp's 'a'-case is hit.
                    if (i >= 'A' && i <= 'Z') return i + 32;
                    // Every other printable ASCII character passes
                    // through unchanged. input.cpp handles specific
                    // punctuation (`,` `.` `'` `;` `\` etc.); dropping
                    // them here silently loses drop/throw/save/load/
                    // map-toggle keys.
                    if (i >= 0x20 && i <= 0x7e) return i;
                    break;
            }
            continue;
        }

        // Synthetic slots — poll OS for left/right ctrl separately.
        // GetAsyncKeyState's high bit is set while the key is down.
#if defined(_WIN32)
        if (i == 256) {
            if (GetAsyncKeyState(VK_LCONTROL) & 0x8000)
                return InputKey::CTRL_LEFT;
        } else if (i == 257) {
            if (GetAsyncKeyState(VK_RCONTROL) & 0x8000)
                return InputKey::CTRL_RIGHT;
        }
        // Slot 258 reserved for future modifier.
#else
        // Non-Windows: fall back to the generic ctrl bit via fenster's
        // mod mask (emit CTRL_LEFT for either ctrl; right-ctrl boost
        // only works on Windows for now).
        if (i == 256 && (impl_->f.mod & 1)) return InputKey::CTRL_LEFT;
#endif
    }
    return InputKey::NONE;
}
