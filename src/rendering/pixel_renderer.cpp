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

void PixelRenderer::apply_pending_resize() {
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
            uint8_t idx = bbc_sprite_pixel(src_x, src_y);
            if (idx == 0) continue;             // transparent
            row[ppx] = lut[idx];
        }
    }
}

// Box-filtered sprite blit — averages atlas pixels under each screen
// pixel for zoom-out. fg[] marks BBC logical colours 8..15: tiles set
// fg_mask, objects skip pixels where fg_mask is set (&1066 BMI).
void PixelRenderer::blit_sprite(int dst_x, int dst_y, uint8_t sprite_id,
                                bool flip_h, bool flip_v,
                                const uint32_t lut[4],
                                const uint8_t fg[4], bool is_tile,
                                uint8_t shrink_shift_x,
                                uint8_t shrink_shift_y) {
    if (sprite_id > 0x80) return;
    const SpriteAtlasEntry& e = sprite_atlas[sprite_id];
    const int hud_y = hud_y_px();

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

    for (int py = 0; py < h_screen; ++py) {
        int ppy = dst_y + py;
        if (ppy < 0 || ppy >= hud_y) continue;
        int ay0 = py * sy_num / sy_den + src_off_y;
        int ay1 = (py + 1) * sy_num / sy_den + src_off_y;
        if (ay1 <= ay0) ay1 = ay0 + 1;
        if (ay1 > e.h) ay1 = e.h;
        if (ay0 >= e.h) ay0 = e.h - 1;
        uint32_t* row      = &buf[(size_t)ppy * f.width];
        uint8_t*  fg_row   = &fg_mask[(size_t)ppy * f.width];
        for (int px = 0; px < w_screen; ++px) {
            int ppx = dst_x + px;
            if (ppx < 0 || ppx >= f.width) continue;

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
                int src_y = e.y + (flip_v ? (e.h - 1 - ay) : ay);
                for (int ax = ax0; ax < ax1; ++ax) {
                    int src_x = e.x + (flip_h ? (e.w - 1 - ax) : ax);
                    uint8_t idx = bbc_sprite_pixel(src_x, src_y);
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
bool PixelRenderer::world_to_screen(uint8_t wx, uint8_t wy,
                                    int& sx, int& sy,
                                    uint8_t wx_frac,
                                    uint8_t wy_frac) const {
    int dx = static_cast<int8_t>(wx - vp_center_x);
    int dy = static_cast<int8_t>(wy - vp_center_y);
    int tpx = tile_px_x();
    int tpy = tile_px_y();
    // Two independent floors, not one combined. Combined form snaps at
    // different vp_frac_x crossings depending on wx_frac, so static
    // objects oscillate +/-1px relative to their tile as the player walks.
    int sub_x_view = floor_div_256(-int(vp_frac_x) * tpx);
    int sub_y_view = floor_div_256(-int(vp_frac_y) * tpy);
    int sub_x_obj  = floor_div_256( int(wx_frac)   * tpx);
    int sub_y_obj  = floor_div_256( int(wy_frac)   * tpy);
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
    int frac_off_x = int(vp_frac_x) * tpx / 256;
    int frac_off_y = int(vp_frac_y) * tpy / 256;
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
PixelRenderer::~PixelRenderer() { shutdown(); }

bool PixelRenderer::init() {
    if (initialized) return true;
    if (fenster_open(&f) != 0) return false;
    // fenster.h's WNDCLASSEX leaves hCursor = NULL, which makes Windows
    // fall back to the "busy" cursor whenever the OS decides this is a
    // fresh app. Force the arrow on our window class so the pointer is
    // normal from the first frame.
    SetClassLongPtrA(f.hwnd, GCLP_HCURSOR,
                     reinterpret_cast<LONG_PTR>(LoadCursorA(NULL, IDC_ARROW)));
    initialized = true;
    return true;
}

void PixelRenderer::shutdown() {
    if (initialized) {
        fenster_close(&f);
        initialized = false;
    }
}

void PixelRenderer::begin_frame() {
    // Pump pending Windows messages first so mouse / size state is current.
    if (fenster_loop(&f) != 0) should_close = true;
    events_processed = true;
    key_scan_idx = 0;

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

    std::fill(buf.begin(), buf.end(), clear_colour_);
    std::fill(fg_mask.begin(), fg_mask.end(), 0u);
}

void PixelRenderer::end_frame() {
    pr_debug::render_overlay_text(*this);
    pr_debug::render_fps_text(*this);
    pr_debug::render_events_panel(*this);
    pr_debug::render_grid_panel(*this);
    InvalidateRect(f.hwnd, NULL, FALSE);
    UpdateWindow(f.hwnd);
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
        // Compute sub-tile offset so the sprite aligns to the correct
        // half of the cell when flipped — matches &2420-&243f.
        const SpriteAtlasEntry& e = sprite_atlas[sid];
        int base_y_atlas = (tiles_y_offset_and_pattern[info.tile_type & 0x3f] >> 4) * 2;

        int y_off_atlas = info.flip_v
            ? (32 - base_y_atlas - e.h)
            : base_y_atlas;
        int x_off_atlas = info.flip_h ? (16 - e.w) : 0;

        int x_off_px = x_off_atlas * PX_SCALE_X * scale / zoom_den;
        int y_off_px = y_off_atlas * PX_SCALE_Y * scale / zoom_den;

        // Tiles draw with the foreground mask (&ff) — full 16-colour
        // range. `fg` per-slot tells blit_sprite which pixels should
        // mark the foreground buffer (colours 8..15).
        uint32_t lut[4];
        uint8_t  fg[4];
        resolve_palette_with_fg(info.palette, /*is_tile=*/true, lut, fg);
        blit_sprite(sx + x_off_px, sy + y_off_px, sid,
                    info.flip_h, info.flip_v, lut, fg,
                    /*is_tile=*/true);
    }

    pr_debug::render_tile_overlay(*this, sx, sy, world_x, world_y, info);
}

// &12a6-&12d8 raster-palette swap. Framebuffer can't reprogram colour 0
// mid-scanline, so pre-fill below blits: above=black, waterline row=cyan
// (&06, 1-line delay_loop), below=blue (&04). Logical 0 blits transparent.
void PixelRenderer::render_water_column(uint8_t world_x,
                                        uint8_t waterline_y) {
    int tpx = tile_px_x();

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

    if (delta_from_top < 0) {
        // Waterline above screen top -> entire column submerged -> blue.
        fill_rect(top_sx, 0, tpx, hud_y, BLUE);
    } else if (delta_from_top < vp_h) {
        // Waterline inside viewport. Cyan on the waterline row's top
        // scanline, blue below.
        if (top_sy + 1 < hud_y) {
            int below_y0 = top_sy + 1;
            if (below_y0 < 0) below_y0 = 0;
            fill_rect(top_sx, below_y0, tpx, hud_y - below_y0, BLUE);
        }
        if (top_sy >= 0 && top_sy < hud_y) {
            fill_rect(top_sx, top_sy, tpx, 1, CYAN);
        }
    }
    // else: entire column above waterline -> leave black.
}

void PixelRenderer::render_object(Fixed8_8 world_x, Fixed8_8 world_y,
                                  const SpriteRenderInfo& info) {
    if (!info.visible) return;
    int sx, sy;
    if (!world_to_screen(world_x.whole, world_y.whole, sx, sy,
                          world_x.fraction, world_y.fraction)) return;
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

int PixelRenderer::get_key() {
    if (should_close) return InputKey::CLOSE_REQUESTED;

    // Fenster keys 0..256; f.mod bit 0 = ctrl (no L/R distinction, so
    // we probe OS). Indices 256..262 are synthetic for ctrl + arrows on
    // Windows — fenster's scancode table overruns 0x14X extended codes
    // so arrow f.keys[] never get set; macOS/Linux work via 19/20/17/18.
    while (key_scan_idx < 263) {
        int i = key_scan_idx++;

        if (i < 256) {
            if (!f.keys[i]) continue;
            switch (i) {
                case 9:  return InputKey::TAB;
                case 17: return InputKey::UP;
                case 18: return InputKey::DOWN;
                case 19: return InputKey::RIGHT;
                case 20: return InputKey::LEFT;
                case 10: return InputKey::ENTER;
                case 27: return InputKey::ESCAPE;
                default:
                    // Letters: lowercase so input.cpp's 'a'-case is hit.
                    if (i >= 'A' && i <= 'Z') return i + 32;
                    if (i >= 0x20 && i <= 0x80) return i;
                    break;
            }
            continue;
        }

        // Synthetic slots — poll OS for keys fenster's table misses on Windows.
#if defined(_WIN32)
        switch (i) {
            case 256:
                if (GetAsyncKeyState(VK_LCONTROL) & 0x8000)
                    return InputKey::CTRL_LEFT;
                break;
            case 257:
                if (GetAsyncKeyState(VK_RCONTROL) & 0x8000)
                    return InputKey::CTRL_RIGHT;
                break;
            case 258:
                if (GetAsyncKeyState(VK_LEFT)  & 0x8000)
                    return InputKey::LEFT;
                break;
            case 259:
                if (GetAsyncKeyState(VK_RIGHT) & 0x8000)
                    return InputKey::RIGHT;
                break;
            case 260:
                if (GetAsyncKeyState(VK_UP)    & 0x8000)
                    return InputKey::UP;
                break;
            case 261:
                if (GetAsyncKeyState(VK_DOWN)  & 0x8000)
                    return InputKey::DOWN;
                break;
            case 262:
                // Left Shift surfaces only via the OS poll — fenster's
                // scancode table doesn't separate LSHIFT from RSHIFT.
                if (GetAsyncKeyState(VK_LSHIFT) & 0x8000)
                    return InputKey::SHIFT_LEFT;
                break;
            default: break;
        }
#else
        if (i == 256 && (f.mod & 1)) return InputKey::CTRL_LEFT;
#endif
    }
    return InputKey::NONE;
}
