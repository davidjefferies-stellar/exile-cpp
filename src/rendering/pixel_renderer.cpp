// Game-rendering pipeline only: tile/water/object/particle blits and
// HUD strip. Developer overlays live in pixel_renderer_debug.cpp;
// entry points go through pr_debug:: declared in pixel_renderer.h.

#include "rendering/pixel_renderer.h"
#include "rendering/sprite_atlas.h"
#include "rendering/sprite_data.h"
#include "rendering/palette.h"
#include "rendering/font8x8.h"
#include "world/tile_data.h"
#include "objects/object_data.h"
#include "sokol_app.h"
#include <unordered_map>

// Pre-decoded sprite sheet: logical colour 0..3 per pixel, flat
// BBC_SHEET_W x BBC_SHEET_H. TU-local (NOT a PixelRenderer member) so it
// can never alter the class layout. blit_sprite reads sprite_idx() — one
// array access per atlas sample instead of bbc_sprite_pixel's per-pixel
// 2-bpp bit-deinterleave (the large-window hot path). Eagerly built before
// main; BBC_SPRITE_DATA is constant-initialised so it is ready in time.
static std::array<uint8_t, BBC_SHEET_W * BBC_SHEET_H> make_sprite_index() {
    std::array<uint8_t, BBC_SHEET_W * BBC_SHEET_H> a{};
    for (int y = 0; y < BBC_SHEET_H; ++y)
        for (int x = 0; x < BBC_SHEET_W; ++x)
            a[static_cast<size_t>(y) * BBC_SHEET_W + x] = bbc_sprite_pixel(x, y);
    return a;
}
static const std::array<uint8_t, BBC_SHEET_W * BBC_SHEET_H>
    g_sprite_index = make_sprite_index();

static inline uint8_t sprite_idx(int x, int y) {
    return g_sprite_index[static_cast<size_t>(y) * BBC_SHEET_W + x];
}

// ---------------------------------------------------------------------------
// Tile cache (phase 2): per-tile pre-rasterized RGBA + fg_mask buffers,
// keyed on (tile_type, palette, flip_h, flip_v). First draw of a variant
// runs blit_sprite once to fill the entry; every subsequent draw becomes
// memcpy per row from the entry into the main framebuffer. Cleared on zoom
// change so the cached pixel size always matches the current tile_px_x/y.
//
// Key layout (16 bits):
//   bits  0..5  tile_type (0..63)
//   bits  6..13 palette   (full uint8_t)
//   bit   14    flip_h
//   bit   15    flip_v
// 64 K possible keys; in practice ~100-300 unique combos appear.
// unordered_map fits the actual working set without the 4 MB header
// overhead of a flat array indexed by key.
struct TileCacheEntry {
    std::vector<uint32_t> rgba;  // tile_px_x * tile_px_y BGRA pixels
    std::vector<uint8_t>  fg;    // matching foreground-mask bytes
    int w = 0;
    int h = 0;
};

static std::unordered_map<uint16_t, TileCacheEntry> g_tile_cache;

static inline uint16_t tile_cache_key(uint8_t tile_type, uint8_t palette,
                                      bool flip_h, bool flip_v) {
    return static_cast<uint16_t>(
        (tile_type & 0x3f) |
        (static_cast<uint16_t>(palette) << 6) |
        (flip_h ? 0x4000 : 0) |
        (flip_v ? 0x8000 : 0));
}

static void tile_cache_clear() {
    g_tile_cache.clear();
}

// Cache entries are pre-rasterized at the current tile_px_x x tile_px_y.
// Wheel zoom changes scale/zoom_den, which changes tile pixel size, so the
// cache must be flushed. Called at the top of begin_frame; first call's
// sentinel (-1, -1) just primes prev_* without clearing anything real.
static void check_tile_cache_invalidation(int scale, int zoom_den) {
    static int prev_scale = -1;
    static int prev_zoom_den = -1;
    if (scale != prev_scale || zoom_den != prev_zoom_den) {
        tile_cache_clear();
        prev_scale    = scale;
        prev_zoom_den = zoom_den;
    }
}

void PixelRenderer::apply_pending_resize() {
    if (f.pending_w > 0 && f.pending_h > 0 &&
        (f.pending_w != f.width || f.pending_h != f.height)) {
        f.width = f.pending_w;
        f.height = f.pending_h;
        buf.assign((size_t)f.width * f.height, 0);
        fg_mask.assign((size_t)f.width * f.height, 0);
    }
    f.pending_w = 0;
    f.pending_h = 0;
}

void PixelRenderer::stroke_rect(int x, int y, int w, int h, uint32_t color) {
    int hud_y = hud_y_px();
    for (int i = 0; i < w; ++i) {
        int px = x + i;
        if ((unsigned)px < (unsigned)f.width) {
            if (y >= 0 && y < hud_y)
                buf[(size_t)y * f.width + px] = color;
            int yb = y + h - 1;
            if (yb >= 0 && yb < hud_y)
                buf[(size_t)yb * f.width + px] = color;
        }
    }
    for (int j = 0; j < h; ++j) {
        int py = y + j;
        if (py < 0 || py >= hud_y) continue;
        if ((unsigned)x < (unsigned)f.width)
            buf[(size_t)py * f.width + x] = color;
        int xr = x + w - 1;
        if ((unsigned)xr < (unsigned)f.width)
            buf[(size_t)py * f.width + xr] = color;
    }
}

void PixelRenderer::blit_sprite_at_native(int cell_x, int cell_y,
                                          int cell_w, int cell_h,
                                          uint8_t sprite_id,
                                          const uint32_t lut[4],
                                          bool flip_h, bool flip_v) {
    if (sprite_id > 0x80) return;
    const SpriteAtlasEntry& e = sprite_atlas[sprite_id];
    if (e.w == 0 || e.h == 0) return;
    flip_h ^= (e.intrinsic_flip & 1) != 0;
    flip_v ^= (e.intrinsic_flip & 2) != 0;
    // Natural screen size at the default zoom (PX_SCALE_X=2, Y=1).
    int draw_w = e.w * 2;
    int draw_h = e.h;
    // Clip rather than scale — over-large sprites get cropped.
    if (draw_w > cell_w) draw_w = cell_w;
    if (draw_h > cell_h) draw_h = cell_h;
    int dst_x = cell_x + (cell_w - draw_w) / 2;
    int dst_y = cell_y + (cell_h - draw_h) / 2;
    for (int py = 0; py < draw_h; ++py) {
        int ppy = dst_y + py;
        if (ppy < 0 || ppy >= f.height) continue;
        int ay = py;
        if (ay >= e.h) ay = e.h - 1;
        int src_y = e.y + (flip_v ? (e.h - 1 - ay) : ay);
        uint32_t* row = &buf[(size_t)ppy * f.width];
        for (int px = 0; px < draw_w; ++px) {
            int ppx = dst_x + px;
            if (ppx < 0 || ppx >= f.width) continue;
            int ax = px / 2;       // 2:1 horizontal scale
            if (ax >= e.w) ax = e.w - 1;
            int src_x = e.x + (flip_h ? (e.w - 1 - ax) : ax);
            uint8_t idx = sprite_idx(src_x, src_y);
            if (idx == 0) continue;             // transparent
            row[ppx] = lut[idx];
        }
    }
}

// Box-filtered sprite blit — averages atlas pixels under each screen
// pixel for zoom-out. fg[] marks BBC logical colours 8..15: tiles set
// fg_mask, objects skip pixels where fg_mask is set (&1066 BMI).
// Destination-agnostic blit: writes to (dst_rgba, dst_fg) with row stride
// dst_stride and a clip box (dst_x..dst_x+w_screen) x (dst_y..dst_max_y).
// Used both by the live framebuffer path (PixelRenderer::blit_sprite below
// passes buf/fg_mask/f.width/hud_y_px) and the tile-cache fill path (passes
// a per-tile-cell scratch buffer of size tile_px_x x tile_px_y).
static void blit_sprite_impl(uint32_t* dst_rgba, uint8_t* dst_fg,
                             int dst_stride, int dst_max_y,
                             int dst_x, int dst_y,
                             uint8_t sprite_id, bool flip_h, bool flip_v,
                             const uint32_t lut[4], const uint8_t fg[4],
                             bool is_tile,
                             uint8_t shrink_shift_x, uint8_t shrink_shift_y,
                             int scale, int zoom_den) {
    if (sprite_id > 0x80) return;
    const SpriteAtlasEntry& e = sprite_atlas[sprite_id];

    flip_h ^= (e.intrinsic_flip & 1) != 0;
    flip_v ^= (e.intrinsic_flip & 2) != 0;

    int w_full = e.w * PX_SCALE_X * scale / zoom_den;
    int h_full = e.h * PX_SCALE_Y * scale / zoom_den;
    int w_screen = w_full;
    int h_screen = h_full;
    // 6502 &0d21 reduce_sprite_if_teleporting: shrink both screen
    // extent and atlas-source range by the same shift so the sprite
    // collapses around its own centre rather than its top-left.
    if (shrink_shift_x) w_screen >>= shrink_shift_x;
    if (shrink_shift_y) h_screen >>= shrink_shift_y;
    if (w_screen < 1) w_screen = 1;
    if (h_screen < 1) h_screen = 1;
    int atlas_w_shr = e.w; if (shrink_shift_x) atlas_w_shr >>= shrink_shift_x;
    int atlas_h_shr = e.h; if (shrink_shift_y) atlas_h_shr >>= shrink_shift_y;
    if (atlas_w_shr < 1) atlas_w_shr = 1;
    if (atlas_h_shr < 1) atlas_h_shr = 1;
    // 6502 &0d2c-&0d35 add (full-shrunk)/2 to both spritesheet_x and
    // x_fraction. Screen offset stays in screen pixels, source offset
    // in atlas pixels.
    dst_x += (w_full - w_screen) / 2;
    dst_y += (h_full - h_screen) / 2;
    int src_off_x = (int(e.w) - atlas_w_shr) / 2;
    int src_off_y = (int(e.h) - atlas_h_shr) / 2;

    // atlas-pixel = screen-pixel * num / den. When num > den (zoom-out)
    // each screen pixel covers multiple atlas pixels.
    int sx_num = zoom_den;
    int sx_den = PX_SCALE_X * scale;
    int sy_num = zoom_den;
    int sy_den = PX_SCALE_Y * scale;

    // Flip handled as origin + sign*a, hoisted out of the loop, rather than
    // the equivalent `base + (flip ? extent-1-a : a)` per pixel. MSVC /O2
    // miscompiles that inlined flip ternary when fused with the sprite-pixel
    // index math (Release-only: tiles render flipped / missing; Debug is
    // fine). Keeping the per-pixel source coord as plain int multiply-add
    // sidesteps it.
    const int sy_origin = flip_v ? (int(e.y) + e.h - 1) : int(e.y);
    const int sy_sign   = flip_v ? -1 : 1;
    const int sx_origin = flip_h ? (int(e.x) + e.w - 1) : int(e.x);
    const int sx_sign   = flip_h ? -1 : 1;

    for (int py = 0; py < h_screen; ++py) {
        int ppy = dst_y + py;
        if (ppy < 0 || ppy >= dst_max_y) continue;
        int ay0 = py * sy_num / sy_den + src_off_y;
        int ay1 = (py + 1) * sy_num / sy_den + src_off_y;
        if (ay1 <= ay0) ay1 = ay0 + 1;
        if (ay1 > e.h) ay1 = e.h;
        if (ay0 >= e.h) ay0 = e.h - 1;
        uint32_t* row    = dst_rgba + (size_t)ppy * dst_stride;
        uint8_t*  fg_row = dst_fg   + (size_t)ppy * dst_stride;
        for (int px = 0; px < w_screen; ++px) {
            int ppx = dst_x + px;
            if (ppx < 0 || ppx >= dst_stride) continue;

            // Object-plot skip: 6502 BMI past pre-existing foreground
            // at &1066. Only applies for object blits with an fg lookup.
            if (!is_tile && fg && fg_row[ppx]) continue;

            int ax0 = px * sx_num / sx_den + src_off_x;
            int ax1 = (px + 1) * sx_num / sx_den + src_off_x;
            if (ax1 <= ax0) ax1 = ax0 + 1;
            if (ax1 > e.w) ax1 = e.w;
            if (ax0 >= e.w) ax0 = e.w - 1;

            int r_sum = 0, g_sum = 0, b_sum = 0;
            int count = 0;
            bool any_fg = false;
            for (int ay = ay0; ay < ay1; ++ay) {
                int src_y = sy_origin + sy_sign * ay;
                for (int ax = ax0; ax < ax1; ++ax) {
                    int src_x = sx_origin + sx_sign * ax;
                    uint8_t idx = sprite_idx(src_x, src_y);
                    if (idx == 0) continue;
                    uint32_t c = lut[idx];
                    r_sum += (c >> 16) & 0xff;
                    g_sum += (c >>  8) & 0xff;
                    b_sum +=  c        & 0xff;
                    if (fg && fg[idx]) any_fg = true;
                    ++count;
                }
            }
            if (count == 0) continue;

            int total = (ay1 - ay0) * (ax1 - ax0);
            int sr = r_sum / count;
            int sg = g_sum / count;
            int sb = b_sum / count;

            if (count == total) {
                row[ppx] = 0xff000000u | (uint32_t(sr) << 16)
                                       | (uint32_t(sg) <<  8)
                                       |  uint32_t(sb);
            } else {
                // Partial coverage — alpha-blend sprite-average over
                // the existing pixel.
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

            if (is_tile && any_fg) fg_row[ppx] = 1;
        }
    }
}

void PixelRenderer::blit_sprite(int dst_x, int dst_y, uint8_t sprite_id,
                                bool flip_h, bool flip_v,
                                const uint32_t lut[4],
                                const uint8_t fg[4], bool is_tile,
                                uint8_t shrink_shift_x,
                                uint8_t shrink_shift_y) {
    blit_sprite_impl(buf.data(), fg_mask.data(), f.width, hud_y_px(),
                     dst_x, dst_y, sprite_id, flip_h, flip_v, lut, fg, is_tile,
                     shrink_shift_x, shrink_shift_y, scale, zoom_den);
}

void PixelRenderer::draw_glyph(int x, int y, char ch,
                               uint32_t fg, uint32_t bg) {
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

int PixelRenderer::draw_text(int x, int y, const char* s,
                             uint32_t fg, uint32_t bg) {
    int cx = x;
    for (; *s; ++s) {
        if (*s == '\n') { y += 9; cx = x; continue; }
        draw_glyph(cx, y, *s, fg, bg);
        cx += 8;
    }
    return cx;
}

// Centre tile's top-left is at (win_w/2, hud_y_px/2); other tiles
// offset by (dx, dy) tiles from that. vp_w/h_tiles() overrenders by
// ±1 tile so the screen edges stay covered during sub-tile pan.
// Adaptive subpixel: snap only when the object is fully stationary on
// both axes (|vx| < 1 && |vy| < 1, i.e. exactly 0). Any motion at all
// renders at sub-pixel precision — even the 1-frame gravity tick lets
// the object slide smoothly. The grounded-object judder is killed not
// by snapping but by the object spending most of its time at vel=0
// while gravity hasn't yet flipped it back to vy=1.
static constexpr int kAdaptiveSnapThreshold = 4;

static bool should_snap_for_velocity(IRenderer::SubpixelMode mode,
                                     int8_t vx, int8_t vy) {
    if (mode == IRenderer::SubpixelMode::Off) return true;
    if (mode == IRenderer::SubpixelMode::On)  return false;
    int amx = (vx < 0) ? -int(vx) : int(vx);
    int amy = (vy < 0) ? -int(vy) : int(vy);
    return amx < kAdaptiveSnapThreshold && amy < kAdaptiveSnapThreshold;
}

bool PixelRenderer::world_to_screen(uint8_t wx, uint8_t wy,
                                    int& sx, int& sy,
                                    uint8_t wx_frac,
                                    uint8_t wy_frac,
                                    int8_t  vx,
                                    int8_t  vy) const {
    int dx = static_cast<int8_t>(wx - vp_center_x);
    int dy = static_cast<int8_t>(wy - vp_center_y);
    int tpx = tile_px_x();
    int tpy = tile_px_y();
    // [render] subpixel_rendering decides whether to mask sub-tile
    // fractions to the BBC's render grid (16 frac per X-pixel, 8 per
    // Y-row). Off -> always snap (no judder, BBC-faithful chunky
    // motion). On -> never snap (smooth but exposes the gravity vs
    // tile-collision tug-of-war as 1-2 px judder on grounded objects).
    // Adaptive -> snap only when velocity is below kAdaptiveSnapThreshold;
    // near-stationary objects look stable, fast ones look smooth.
    bool snap_view = should_snap_for_velocity(subpixel_mode, cam_vx, cam_vy);
    bool snap_obj  = should_snap_for_velocity(subpixel_mode, vx, vy);
    uint8_t fvpx = vp_frac_x & (snap_view ? 0xf0 : 0xff);
    uint8_t fvpy = vp_frac_y & (snap_view ? 0xf8 : 0xff);
    uint8_t fwx  = wx_frac    & (snap_obj  ? 0xf0 : 0xff);
    uint8_t fwy  = wy_frac    & (snap_obj  ? 0xf8 : 0xff);
    // Two independent floors, not one combined. Combined form snaps at
    // different vp_frac_x crossings depending on wx_frac, so static
    // objects oscillate +/-1px relative to their tile as the player walks.
    int sub_x_view = floor_div_256(-int(fvpx) * tpx);
    int sub_y_view = floor_div_256(-int(fvpy) * tpy);
    int sub_x_obj  = floor_div_256( int(fwx)  * tpx);
    int sub_y_obj  = floor_div_256( int(fwy)  * tpy);
    sx = f.width / 2 + dx * tpx + sub_x_view + sub_x_obj + pan_px_x;
    sy = hud_y_px() / 2 + dy * tpy + sub_y_view + sub_y_obj + pan_px_y;
    return sx > -tpx && sx < f.width && sy > -tpy && sy < hud_y_px();
}

// Inverse of world_to_screen. world_to_screen shifts tiles by
// -vp_frac_*·tpx/256 so the world scrolls under a fixed player sprite;
// the inverse must undo that shift else clicks snap to the wrong tile
// whenever the player is mid-cell.
void PixelRenderer::screen_to_tile_offset(int sx, int sy,
                                          int& tdx, int& tdy) const {
    int tpx = tile_px_x();
    int tpy = tile_px_y();
    // Match world_to_screen's snap so clicks invert to the tile
    // actually drawn under the cursor. Uses the camera-motion hint so
    // Adaptive mode lines up with whatever the renderer is currently
    // showing.
    bool snap_view = should_snap_for_velocity(subpixel_mode, cam_vx, cam_vy);
    int frac_off_x = int(vp_frac_x & (snap_view ? 0xf0 : 0xff)) * tpx / 256;
    int frac_off_y = int(vp_frac_y & (snap_view ? 0xf8 : 0xff)) * tpy / 256;
    int rel_x = sx - f.width / 2 - pan_px_x + frac_off_x;
    int rel_y = sy - hud_y_px() / 2 - pan_px_y + frac_off_y;
    tdx = (rel_x >= 0) ? (rel_x / tpx) : -((-rel_x + tpx - 1) / tpx);
    tdy = (rel_y >= 0) ? (rel_y / tpy) : -((-rel_y + tpy - 1) / tpy);
}

void PixelRenderer::process_mouse() {
    constexpr int kRightClickMotionMax = 4;  // px

    // Right-drag panning + click vs drag detection.
    bool right_down = (f.mouse & 2) != 0;
    if (right_down && !right_was_down) {
        right_press_x = f.x;
        right_press_y = f.y;
        right_motion  = 0;
    }
    if (right_down && right_was_down) {
        int dx = f.x - prev_mouse_x;
        int dy = f.y - prev_mouse_y;
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        right_motion += adx + ady;
        // Dragging right -> world slides right -> camera centre LEFT.
        pan_px_x += dx;
        pan_px_y += dy;
        int tpx = tile_px_x();
        int tpy = tile_px_y();
        while (pan_px_x >= tpx) { pan_px_x -= tpx; pending_pan_tiles_x -= 1; }
        while (pan_px_x <= -tpx) { pan_px_x += tpx; pending_pan_tiles_x += 1; }
        while (pan_px_y >= tpy) { pan_px_y -= tpy; pending_pan_tiles_y -= 1; }
        while (pan_px_y <= -tpy) { pan_px_y += tpy; pending_pan_tiles_y += 1; }
    }
    if (!right_down && right_was_down) {
        // Release: short-motion press counts as a click. Use the PRESS
        // position so a tiny drift between press and release doesn't
        // move the click target.
        if (right_motion <= kRightClickMotionMax) {
            has_pending_right_click = true;
            pending_right_click_x   = right_press_x;
            pending_right_click_y   = right_press_y;
        }
    }
    right_was_down = right_down;

    // Left-click edge detection. Debug-widget hits are absorbed first;
    // unconsumed clicks fall through as a world tile click.
    bool left_down = (f.mouse & 1) != 0;
    if (left_down && !left_was_down) {
        if (!pr_debug::consume_left_click(*this)) {
            has_pending_click = true;
            pending_click_x = f.x;
            pending_click_y = f.y;
        }
    }
    left_was_down = left_down;

    // Saves panel: hover updates the keyboard highlight so users can
    // sweep the mouse over rows and confirm with Enter, or just click.
    pr_debug::saves_panel_hover(*this);

    prev_mouse_x = f.x;
    prev_mouse_y = f.y;
}

// Tile (0..0x3f) -> sprite_id. &04ab tiles_sprite_and_y_flip_table; bit 7
// XORs flip_v (obstruction-at-bottom flag for collision &2477).
// 0xff = transparent (sprite 0x46) — skip blit entirely.
const uint8_t TILE_SPRITE_ID[64] = {
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

PixelRenderer::PixelRenderer()
    : buf(INITIAL_W * INITIAL_H, 0),
      fg_mask(INITIAL_W * INITIAL_H, 0) {
    f.width  = INITIAL_W;
    f.height = INITIAL_H;
}
PixelRenderer::~PixelRenderer() { shutdown(); }

// Window creation/destruction now owned by sokol_app (sapp_run in main.cpp).
// PixelRenderer just tracks initialised state for shutdown ordering.
bool PixelRenderer::init() {
    if (initialized) return true;
    initialized = true;
    return true;
}

void PixelRenderer::shutdown() {
    initialized = false;
}

void PixelRenderer::begin_frame() {
    // sokol_app drives the window/event pump; event_cb populates `f`
    // before frame_cb invokes this. Just reset per-frame scan state.
    events_processed = true;
    key_scan_idx = 0;

    // Rotate the saves-panel debounce mask: whatever was held during the
    // previous frame's get_key sweep becomes the "prev" reference; clear
    // "curr" so this frame's sweep can record which keys are still held.
    saves_keys_held_prev_ = saves_keys_held_curr_;
    saves_keys_held_curr_ = 0;

    apply_pending_resize();

    if (f.wheel != 0) {
        int w = f.wheel;
        f.wheel = 0;

        // Anchor the zoom on the tile under the mouse pointer: record
        // the screen-space offset from the view centre before the zoom
        // change, then rescale it to the new tile pitch so the same
        // world point stays under the cursor.
        int mx = f.x;
        int my = f.y;
        int cx = f.width / 2;
        int cy = hud_y_px() / 2;
        int tpx_old = tile_px_x();
        int tpy_old = tile_px_y();
        int off_x   = mx - cx - pan_px_x;
        int off_y   = my - cy - pan_px_y;

        // Treat the zoom ladder as a single signed axis. Above 1:1 grow
        // `scale`; below grow `zoom_den`. They never both exceed 1.
        while (w > 0) {
            if (zoom_den > 1) zoom_den--;
            else if (scale < MAX_SCALE) scale++;
            --w;
        }
        while (w < 0) {
            if (scale > MIN_SCALE) scale--;
            else if (zoom_den < MAX_ZOOM_DEN) zoom_den++;
            ++w;
        }

        int tpx_new = tile_px_x();
        int tpy_new = tile_px_y();
        if (tpx_old > 0 && tpy_old > 0) {
            pan_px_x = (mx - cx) - off_x * tpx_new / tpx_old;
            pan_px_y = (my - cy) - off_y * tpy_new / tpy_old;
        }
        // Normalize: any pan past a whole tile feeds back into the
        // pending-pan queue (matches drag-pan bookkeeping).
        while (pan_px_x >=  tpx_new) { pan_px_x -= tpx_new; pending_pan_tiles_x -= 1; }
        while (pan_px_x <= -tpx_new) { pan_px_x += tpx_new; pending_pan_tiles_x += 1; }
        while (pan_px_y >=  tpy_new) { pan_px_y -= tpy_new; pending_pan_tiles_y -= 1; }
        while (pan_px_y <= -tpy_new) { pan_px_y += tpy_new; pending_pan_tiles_y += 1; }
    }

    process_mouse();

    // Cache invalidation runs AFTER the wheel-zoom block above so a same-
    // frame zoom flushes immediately and render_tile sees fresh tc.w/tc.h.
    check_tile_cache_invalidation(scale, zoom_den);

    std::fill(buf.begin(), buf.end(), clear_colour_);
    std::fill(fg_mask.begin(), fg_mask.end(), 0u);
}

void PixelRenderer::end_frame() {
    pr_debug::render_overlay_text(*this);
    pr_debug::render_fps_text(*this);
    pr_debug::render_events_panel(*this);
    pr_debug::render_grid_panel(*this);
    pr_debug::render_saves_panel(*this);
    // Present is owned by main.cpp's sokol frame_cb (tasks #6/#7 upload `buf`
    // as a texture and draw a full-screen quad). Nothing to do here.
}

// Translate a sokol_app keycode to PixelRenderer's f.keys[] index. ASCII for
// letters/digits/symbols; fenster-compat slots for TAB/ENTER/ESC/arrows
// (9/10/27/17-20); synthetic >256 slots for L/R-distinct modifiers and
// Numpad keys that need to be distinguishable from their ASCII twins.
// Returns -1 for keys we don't track (function keys, NumLock, etc.).
static int sokol_keycode_to_index(sapp_keycode code) {
    if (code >= SAPP_KEYCODE_A && code <= SAPP_KEYCODE_Z) {
        return 'A' + (code - SAPP_KEYCODE_A);
    }
    if (code >= SAPP_KEYCODE_0 && code <= SAPP_KEYCODE_9) {
        return '0' + (code - SAPP_KEYCODE_0);
    }
    switch (code) {
        case SAPP_KEYCODE_SPACE:         return ' ';
        case SAPP_KEYCODE_TAB:           return 9;
        case SAPP_KEYCODE_ENTER:         return 10;
        case SAPP_KEYCODE_ESCAPE:        return 27;
        case SAPP_KEYCODE_BACKSPACE:     return 8;
        case SAPP_KEYCODE_UP:            return 17;
        case SAPP_KEYCODE_DOWN:          return 18;
        case SAPP_KEYCODE_RIGHT:         return 19;
        case SAPP_KEYCODE_LEFT:          return 20;
        case SAPP_KEYCODE_COMMA:         return ',';
        case SAPP_KEYCODE_PERIOD:        return '.';
        case SAPP_KEYCODE_SEMICOLON:     return ';';
        case SAPP_KEYCODE_APOSTROPHE:    return '\'';
        case SAPP_KEYCODE_BACKSLASH:     return '\\';
        case SAPP_KEYCODE_LEFT_BRACKET:  return '[';
        case SAPP_KEYCODE_RIGHT_BRACKET: return ']';
        case SAPP_KEYCODE_MINUS:         return '-';
        case SAPP_KEYCODE_EQUAL:         return '=';
        case SAPP_KEYCODE_SLASH:         return '/';
        case SAPP_KEYCODE_GRAVE_ACCENT:  return '`';
        case SAPP_KEYCODE_LEFT_CONTROL:  return 256;
        case SAPP_KEYCODE_RIGHT_CONTROL: return 257;
        case SAPP_KEYCODE_LEFT_SHIFT:    return 262;
        case SAPP_KEYCODE_KP_MULTIPLY:   return 263;
        case SAPP_KEYCODE_KP_SUBTRACT:   return 264;
        default:                         return -1;
    }
}

void PixelRenderer::handle_event(const sapp_event* ev) {
    // Update fenster-compat modifier byte on every event.
    f.mod = 0;
    if (ev->modifiers & SAPP_MODIFIER_CTRL)  f.mod |= 1;
    if (ev->modifiers & SAPP_MODIFIER_SHIFT) f.mod |= 2;
    if (ev->modifiers & SAPP_MODIFIER_ALT)   f.mod |= 4;
    if (ev->modifiers & SAPP_MODIFIER_SUPER) f.mod |= 8;

    switch (ev->type) {
        case SAPP_EVENTTYPE_KEY_DOWN:
        case SAPP_EVENTTYPE_KEY_UP: {
            int idx = sokol_keycode_to_index(ev->key_code);
            if (idx >= 0 && idx < (int)sizeof(f.keys)) {
                f.keys[idx] = (ev->type == SAPP_EVENTTYPE_KEY_DOWN) ? 1 : 0;
            }
            break;
        }
        case SAPP_EVENTTYPE_MOUSE_DOWN:
        case SAPP_EVENTTYPE_MOUSE_UP: {
            int bit = 0;
            if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT)   bit = 1;
            else if (ev->mouse_button == SAPP_MOUSEBUTTON_RIGHT)  bit = 2;
            else if (ev->mouse_button == SAPP_MOUSEBUTTON_MIDDLE) bit = 4;
            if (ev->type == SAPP_EVENTTYPE_MOUSE_DOWN) f.mouse |=  bit;
            else                                       f.mouse &= ~bit;
            f.x = (int)ev->mouse_x;
            f.y = (int)ev->mouse_y;
            break;
        }
        case SAPP_EVENTTYPE_MOUSE_MOVE:
            f.x = (int)ev->mouse_x;
            f.y = (int)ev->mouse_y;
            break;
        case SAPP_EVENTTYPE_MOUSE_SCROLL:
            // Accumulator drained by begin_frame's wheel zoom path. sokol
            // reports float deltas; collapse to signed integer notches.
            if (ev->scroll_y >  0.0f) f.wheel += 1;
            else if (ev->scroll_y <  0.0f) f.wheel -= 1;
            break;
        case SAPP_EVENTTYPE_RESIZED:
            f.pending_w = ev->window_width;
            f.pending_h = ev->window_height;
            break;
        case SAPP_EVENTTYPE_QUIT_REQUESTED:
            should_close = true;
            break;
        default:
            break;
    }
}

void PixelRenderer::set_viewport(uint8_t center_x, uint8_t center_y,
                                 uint8_t frac_x, uint8_t frac_y) {
    vp_center_x = center_x;
    vp_center_y = center_y;
    vp_frac_x   = frac_x;
    vp_frac_y   = frac_y;
}

void PixelRenderer::render_tile(uint8_t world_x, uint8_t world_y,
                                const TileRenderInfo& info) {
    int sx, sy;
    if (!world_to_screen(world_x, world_y, sx, sy)) return;

    uint8_t entry = TILE_SPRITE_ID[info.tile_type & 0x3f];
    uint8_t sid = entry & 0x7f;
    // Bit 7 = &04ab obstruction-at-bottom flag (collision only, not
    // rendering). Rendering flip = landscape tile_flip XOR sprite flip.
    if (entry != 0xff && sid <= 0x80) {
        // Cache key — (tile_type, palette, flip_h, flip_v). All other
        // per-tile parameters (sprite_id, atlas offsets) are derived from
        // tile_type via TILE_SPRITE_ID + tiles_y_offset_and_pattern, so
        // the same key produces identical pixels. Cache cleared on zoom
        // change (see begin_frame's wheel-zoom path).
        const uint16_t key = tile_cache_key(info.tile_type, info.palette,
                                            info.flip_h, info.flip_v);
        auto& tc = g_tile_cache[key];
        if (tc.h == 0) {
            // Miss: render the variant once into a tile-cell-sized scratch.
            // Compute sub-tile offset so the sprite aligns to the correct
            // half of the cell when flipped — matches &2420-&243f.
            const SpriteAtlasEntry& e = sprite_atlas[sid];
            int base_y_atlas =
                (tiles_y_offset_and_pattern[info.tile_type & 0x3f] >> 4) * 2;
            int y_off_atlas = info.flip_v ? (32 - base_y_atlas - e.h)
                                          : base_y_atlas;
            int x_off_atlas = info.flip_h ? (16 - e.w) : 0;
            int x_off_px = x_off_atlas * PX_SCALE_X * scale / zoom_den;
            int y_off_px = y_off_atlas * PX_SCALE_Y * scale / zoom_den;

            int tw = tile_px_x();
            int th = tile_px_y();
            tc.w = tw;
            tc.h = th;
            tc.rgba.assign((size_t)tw * th, 0u);
            tc.fg.assign((size_t)tw * th, 0);

            // Tile fg per-slot — full 16-colour range; cf above blit path.
            uint32_t lut[4];
            uint8_t  fg[4];
            resolve_palette_with_fg(info.palette, /*is_tile=*/true, lut, fg);
            blit_sprite_impl(tc.rgba.data(), tc.fg.data(), tw, th,
                             x_off_px, y_off_px,
                             sid, info.flip_h, info.flip_v, lut, fg,
                             /*is_tile=*/true, 0, 0, scale, zoom_den);
        }

        // Copy the cached tile bitmap into the main framebuffer at (sx, sy).
        // RGBA: per-pixel skip-if-zero so cached transparent pixels don't
        // erase the waterline raster underneath. fg_mask: per-pixel OR so we
        // match the original blit's ADD-ONLY semantics (the original only
        // ever WRITES fg=1, never resets to 0). Bulk memcpy was tried and
        // empirically loses fg=1s elsewhere in the frame — bullets were
        // hidden because the object-pass's fg_mask read returned non-zero
        // at positions where OR keeps it set.
        const int tw = tc.w;
        const int th = tc.h;
        const int fw = f.width;
        const int hud_y = hud_y_px();
        for (int r = 0; r < th; ++r) {
            int dy = sy + r;
            if (dy < 0 || dy >= hud_y) continue;
            int dx_start = sx < 0 ? 0 : sx;
            int dx_end   = sx + tw;
            if (dx_end > fw) dx_end = fw;
            if (dx_end <= dx_start) continue;
            int sx_start = dx_start - sx;
            int span = dx_end - dx_start;

            uint32_t* dst_row =
                buf.data() + (size_t)dy * fw + dx_start;
            const uint32_t* src_row =
                tc.rgba.data() + (size_t)r * tw + sx_start;
            uint8_t* dst_fg_row =
                fg_mask.data() + (size_t)dy * fw + dx_start;
            const uint8_t* src_fg_row =
                tc.fg.data() + (size_t)r * tw + sx_start;

            for (int i = 0; i < span; ++i) {
                if (src_row[i]) dst_row[i] = src_row[i];
                dst_fg_row[i] |= src_fg_row[i];
            }
        }
    }

    pr_debug::render_tile_overlay(*this, sx, sy, world_x, world_y, info);
}

// &12a6-&12d8 raster-palette swap. Framebuffer can't reprogram colour 0
// mid-scanline, so pre-fill below blits: above=black, waterline row=cyan
// (&06, 1-line delay_loop), below=blue (&04). Logical 0 blits transparent.
void PixelRenderer::render_water_column(uint8_t world_x,
                                        uint8_t waterline_y,
                                        uint8_t waterline_y_frac) {
    int tpx = tile_px_x();
    int tpy = tile_px_y();

    // &16db calculate_waterline_timer tri-state. MUST use signed int,
    // not uint8_t: 6502's unsigned SBC only works because BBC camera
    // can't pan past playable; our map-mode pans freely and underflow
    // at the map top flips the compare and every column shows submerged.
    int vp_h = vp_h_tiles();
    int vp_top_y = int(vp_center_y) - vp_h / 2;
    int water    = int(waterline_y);
    int delta_from_top = water - vp_top_y;

    // Use physical BBC MODE 2 hues so the "mid-frame palette swap"
    // lands on the same RGB values as a colour-0 pixel would if the
    // palette register had been rewritten.
    const uint32_t BLUE = LOGICAL_TO_RGB[4];
    const uint32_t CYAN = LOGICAL_TO_RGB[6];

    int hud_y = hud_y_px();
    int top_sx = 0, top_sy = 0;
    (void)world_to_screen(world_x, waterline_y, top_sx, top_sy);
    if (top_sx + tpx <= 0 || top_sx >= f.width) return;

    // Sub-tile offset: y_fraction is 0..255 over one tile height. Apply
    // as a screen-pixel offset so the waterline scanline animates pixel-
    // by-pixel during fill/drain instead of jumping in whole-tile steps.
    int frac_px = (int(waterline_y_frac) * tpy) / 256;

    if (delta_from_top < 0) {
        // Waterline above screen top -> entire column submerged -> blue.
        fill_rect(top_sx, 0, tpx, hud_y, BLUE);
    } else if (delta_from_top < vp_h) {
        int waterline_sy = top_sy + frac_px;
        // Waterline inside viewport. Cyan on the waterline row's top
        // scanline, blue below.
        if (waterline_sy + 1 < hud_y) {
            int below_y0 = waterline_sy + 1;
            if (below_y0 < 0) below_y0 = 0;
            fill_rect(top_sx, below_y0, tpx, hud_y - below_y0, BLUE);
        }
        if (waterline_sy >= 0 && waterline_sy < hud_y) {
            fill_rect(top_sx, waterline_sy, tpx, 1, CYAN);
        }
    }
    // else: entire column above waterline -> leave black.
}

void PixelRenderer::render_object(Fixed8_8 world_x, Fixed8_8 world_y,
                                  const SpriteRenderInfo& info) {
    if (!info.visible) return;
    // Port-only skip for OBJECT_INVISIBLE_DEBRIS. The 6502 draws the
    // sprite EOR-style with palette &00 (kyK), which collapses to a no-op
    // against the black cavern backgrounds the debris normally spawns
    // over. Our LUT-overwrite blit instead paints the 3 logical-0/3
    // pixels as solid black, which shows up wherever the background
    // isn't pure black. Skip the blit so the design intent ("invisible")
    // holds in all contexts.
    if (info.type == ObjectType::INVISIBLE_DEBRIS) return;
    int sx, sy;
    if (!world_to_screen(world_x.whole, world_y.whole, sx, sy,
                          world_x.fraction, world_y.fraction,
                          info.velocity_x, info.velocity_y)) return;
    if (info.sprite_id > 0x80) return;

    // &0d91 object pos = sprite top-left (no sprite-height offset).
    // fg[]=0 makes blit_sprite check fg_mask (&1066 BMI hides objects
    // behind foliage).
    uint32_t lut[4];
    uint8_t  fg[4] = {0, 0, 0, 0};
    resolve_palette(info.palette, /*is_tile=*/false, lut);

    // Port of &0cfe reduce_sprite_if_teleporting. Y uses (timer & 7);
    // X uses ((t>>2) + t + bit_1(t)) & 7 — the +bit_1 is the ADC carry
    // out of the second LSR at &0d12. blit_sprite centres both screen
    // extent and atlas source around the sprite's middle.
    uint8_t shrink_x = 0, shrink_y = 0;
    if (info.teleport_timer != 0) {
        uint8_t t = info.teleport_timer;
        shrink_y = static_cast<uint8_t>(t & 0x07);
        uint8_t t_x = static_cast<uint8_t>(t + (t >> 2) + ((t >> 1) & 1));
        shrink_x = static_cast<uint8_t>(t_x & 0x07);
    }

    blit_sprite(sx, sy, info.sprite_id,
                info.flip_h, info.flip_v, lut, fg,
                /*is_tile=*/false,
                shrink_x, shrink_y);
}

void PixelRenderer::render_hud(const PlayerState& player) {
    // Bottom-strip checkboxes + editor / object palettes are owned by
    // the debug TU. The renderer skips the player-state inventory
    // strip when the sprite-viewer is on (it owns the whole window).
    if (pr_debug::render_hud_panels(*this)) return;

    // Top-left HUD strip: POCKETS | KEYS | WEAPONS panels laid out
    // horizontally. Sprites render at a fixed BBC 2:1 aspect,
    // independent of world zoom.
    static constexpr int CELL       = 28;
    static constexpr int CELL_PAD   = 4;
    static constexpr int PANEL_GAP  = 16;
    static constexpr int ORIGIN_X   = 4;
    static constexpr int ORIGIN_Y   = 4;
    static constexpr int LABEL_H    = 9;
    int cells_y = ORIGIN_Y + LABEL_H;

    // POCKETS — slot 0 ("top" of the stack, next to retrieve) leftmost.
    int px = ORIGIN_X;
    draw_text(px, ORIGIN_Y, "POCKETS", 0xFFFFFF, 0x000000);
    for (int i = 0; i < 5; i++) {
        int cx = px + i * (CELL + CELL_PAD);
        uint8_t ot = player.pockets[i];
        fill_rect(cx, cells_y, CELL, CELL, 0x000000);
        uint32_t border = (ot == 0xff) ? 0x333333 : 0x888888;
        stroke_rect(cx, cells_y, CELL, CELL, border);
        blit_obj_sprite_cell(cx, cells_y, CELL, ot, /*dim=*/false);
    }
    px += 5 * (CELL + CELL_PAD) + PANEL_GAP;

    // KEYS — six collectables (CYAN_YELLOW_GREEN_KEY=0x51 ..
    // BLUE_CYAN_GREEN_KEY=0x57, skipping 0x55).
    static constexpr uint8_t KEY_TYPES[6] = {
        0x51, 0x52, 0x53, 0x54, 0x56, 0x57,
    };
    draw_text(px, ORIGIN_Y, "KEYS", 0xFFFFFF, 0x000000);
    for (int i = 0; i < 6; i++) {
        int cx = px + i * (CELL + CELL_PAD);
        bool have = (player.keys[i] & 0x80) != 0;
        fill_rect(cx, cells_y, CELL, CELL, 0x000000);
        uint32_t border = have ? 0x888888 : 0x333333;
        stroke_rect(cx, cells_y, CELL, CELL, border);
        blit_obj_sprite_cell(cx, cells_y, CELL, KEY_TYPES[i],
                             /*dim=*/!have);
    }
    px += 6 * (CELL + CELL_PAD) + PANEL_GAP;

    // WEAPONS — slot 0 jetpack, 1 pistol, 2 icer, 3 blaster, 4 plasma.
    // Selected slot is highlighted; bottom strip shows energy as a
    // 0..0x800 scaled bar.
    static constexpr uint8_t WEAPON_TYPES[5] = {
        0x59, // JETPACK_BOOSTER
        0x5a, // PISTOL
        0x5b, // ICER
        0x5c, // BLASTER
        0x5d, // PLASMA_GUN
    };
    draw_text(px, ORIGIN_Y, "WEAPONS", 0xFFFFFF, 0x000000);
    for (int i = 0; i < 5; i++) {
        int cx = px + i * (CELL + CELL_PAD);
        bool selected = (player.weapon == i);
        uint16_t energy = player.weapon_energy[i];
        bool have = energy > 0;
        fill_rect(cx, cells_y, CELL, CELL, 0x000000);
        uint32_t border = selected ? 0xFFFF44
                                   : (have ? 0x888888 : 0x333333);
        stroke_rect(cx, cells_y, CELL, CELL, border);
        blit_obj_sprite_cell(cx, cells_y, CELL, WEAPON_TYPES[i],
                             /*dim=*/!have);
        int max_w = CELL - 4;
        int bar_w = (energy >= 0x800) ? max_w
                                      : (int(energy) * max_w) / 0x800;
        if (bar_w > 0) {
            uint32_t bar_col = selected ? 0xFFCC44 : 0x44AA44;
            fill_rect(cx + 2, cells_y + CELL - 4, bar_w, 2, bar_col);
        }
    }
}

void PixelRenderer::blit_obj_sprite_cell(int cx, int cy, int cell_size,
                                         uint8_t obj_type, bool dim) {
    if (obj_type == 0xff) return;
    uint8_t sprite_id = object_types_sprite[obj_type];
    if (sprite_id > 0x80) return;
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
            // Dim by halving each channel — palette-agnostic.
            if (dim) col = (col >> 2) & 0x3F3F3F;
            int ox = blit_x + sx * 2;
            int oy = blit_y + sy;
            put_pixel(ox,     oy, col);
            put_pixel(ox + 1, oy, col);
        }
    }
}

void PixelRenderer::render_particle(uint8_t wx, uint8_t wx_frac,
                                    uint8_t wy, uint8_t wy_frac,
                                    uint8_t colour) {
    // Particles use the same 16-slot logical palette as sprites/tiles,
    // keyed by their low 3 bits (BBC background colour group).
    uint32_t col = LOGICAL_TO_RGB[colour & 0x07];
    if (col == 0) return; // black -> invisible

    int sx, sy;
    if (!world_to_screen(wx, wy, sx, sy, wx_frac, wy_frac)) return;
    fill_rect(sx, sy, scale, scale, col);
}

bool PixelRenderer::query_fg_at(uint8_t wx, uint8_t wx_frac,
                                uint8_t wy, uint8_t wy_frac) const {
    // &2118 AND #&c0 / &2120 BEQ test on the post-plot screen byte. The
    // 6502 reads the live framebuffer; we read the parallel fg_mask
    // populated by tile blits. Sample the centre of the scale×scale
    // particle splat so a stretched pixel doesn't false-positive on a
    // neighbouring tile's fringe.
    int sx, sy;
    if (!world_to_screen(wx, wy, sx, sy, wx_frac, wy_frac)) return false;
    int px = sx + scale / 2;
    int py = sy + scale / 2;
    if (px < 0 || px >= f.width || py < 0 || py >= f.height) return false;
    return fg_mask[static_cast<size_t>(py) * f.width + px] != 0;
}

int PixelRenderer::viewport_width_tiles() const { return vp_w_tiles(); }
int PixelRenderer::viewport_height_tiles() const { return vp_h_tiles(); }

bool PixelRenderer::consume_pan_tiles(int& dx, int& dy) {
    dx = pending_pan_tiles_x;
    dy = pending_pan_tiles_y;
    pending_pan_tiles_x = 0;
    pending_pan_tiles_y = 0;
    return dx != 0 || dy != 0;
}

bool PixelRenderer::consume_left_click(int& tile_dx, int& tile_dy) {
    if (!has_pending_click) { tile_dx = 0; tile_dy = 0; return false; }
    has_pending_click = false;
    screen_to_tile_offset(pending_click_x, pending_click_y, tile_dx, tile_dy);
    return true;
}

bool PixelRenderer::consume_right_click(int& tile_dx, int& tile_dy) {
    if (!has_pending_right_click) {
        tile_dx = 0; tile_dy = 0; return false;
    }
    has_pending_right_click = false;
    screen_to_tile_offset(pending_right_click_x, pending_right_click_y,
                          tile_dx, tile_dy);
    return true;
}

// Saves-panel keyboard nav. Eats UP/DOWN/ENTER while the panel is open
// so the keys don't double-fire into the game (jetpack thrust etc.).
// Debounced via saves_keys_held_prev_ / saves_keys_held_curr_ so holding
// a cursor key advances the highlight one row, not one row per frame.
// Returns true iff the key was consumed.
static bool handle_saves_key(PixelRenderer& r, int key) {
    if (!r.saves_panel_on) return false;
    uint8_t bit = 0;
    if      (key == InputKey::UP)    bit = 0x01;
    else if (key == InputKey::DOWN)  bit = 0x02;
    else if (key == InputKey::ENTER) bit = 0x04;
    else return false;

    bool was_held = (r.saves_keys_held_prev_ & bit) != 0;
    r.saves_keys_held_curr_ |= bit;
    if (was_held) return true;   // swallow the held key, no auto-repeat

    int total = static_cast<int>(r.saves_list_.size());
    if (key == InputKey::UP && total > 0) {
        r.saves_highlight_ = (r.saves_highlight_ - 1 + total) % total;
    } else if (key == InputKey::DOWN && total > 0) {
        r.saves_highlight_ = (r.saves_highlight_ + 1) % total;
    } else if (key == InputKey::ENTER &&
               total > 0 && r.saves_highlight_ < total) {
        r.has_pending_save_load   = true;
        r.pending_save_load_path_ = r.saves_list_[r.saves_highlight_];
    }
    return true;
}

int PixelRenderer::get_key() {
    // One-shot: must clear before returning, or the while-loop in
    // Game::step that drains get_key() spins forever and starves the
    // sokol_app message pump that would otherwise process the quit.
    if (should_close) {
        should_close = false;
        return InputKey::CLOSE_REQUESTED;
    }

    // f.keys[0..255]:  ASCII for letter/symbol keys, plus 9=TAB, 10=ENTER,
    //                  17..20 = UP/DOWN/RIGHT/LEFT, 27=ESCAPE.
    // f.keys[256..264]: synthetic slots for modifier/arrow/numpad keys that
    //                  the event handler (sokol_app) populates with the
    //                  L/R-distinct keycodes — see PixelRenderer::handle_event.
    while (key_scan_idx < 265) {
        int i = key_scan_idx++;
        if (!f.keys[i]) continue;

        int decoded = InputKey::NONE;
        if (i < 256) {
            switch (i) {
                case 9:  decoded = InputKey::TAB; break;
                case 17: decoded = InputKey::UP; break;
                case 18: decoded = InputKey::DOWN; break;
                case 19: decoded = InputKey::RIGHT; break;
                case 20: decoded = InputKey::LEFT; break;
                case 10: decoded = InputKey::ENTER; break;
                case 27: decoded = InputKey::ESCAPE; break;
                default:
                    if (i >= 'A' && i <= 'Z') decoded = i + 32;
                    else if (i >= 0x20 && i <= 0x80) decoded = i;
                    break;
            }
        } else {
            switch (i) {
                case 256: decoded = InputKey::CTRL_LEFT; break;
                case 257: decoded = InputKey::CTRL_RIGHT; break;
                case 258: decoded = InputKey::LEFT; break;
                case 259: decoded = InputKey::RIGHT; break;
                case 260: decoded = InputKey::UP; break;
                case 261: decoded = InputKey::DOWN; break;
                case 262: decoded = InputKey::SHIFT_LEFT; break;
                case 263: decoded = InputKey::KEYPAD_STAR; break;
                case 264: decoded = InputKey::KEYPAD_MINUS; break;
                default: break;
            }
        }
        if (decoded == InputKey::NONE) continue;
        if (handle_saves_key(*this, decoded)) continue;
        return decoded;
    }
    return InputKey::NONE;
}
