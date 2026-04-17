#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifdef UNICODE
#undef UNICODE
#endif
#ifdef _UNICODE
#undef _UNICODE
#endif

#include "rendering/fenster/fenster_renderer.h"
#include "rendering/sprite_atlas.h"
#include "rendering/font8x8.h"
#include "world/tile_data.h"
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
static constexpr int INITIAL_W = 640;
static constexpr int INITIAL_H = 480;
static constexpr int HUD_PX = TILE_PX_BASE_Y / 2;
static constexpr int MIN_SCALE = 1;
static constexpr int MAX_SCALE = 8;

static const char* const ATLAS_PATHS[] = {
    "exile_sprites.png",
    "../exile_sprites.png",
    "../../exile_sprites.png",
    "../../../exile_sprites.png",
};

static constexpr uint32_t COL_BLACK   = 0x000000;
static constexpr uint32_t COL_RED     = 0xCC0000;
static constexpr uint32_t COL_GREEN   = 0x00CC00;
static constexpr uint32_t COL_YELLOW  = 0xCCCC00;
static constexpr uint32_t COL_BLUE    = 0x2244CC;
static constexpr uint32_t COL_MAGENTA = 0xCC00CC;
static constexpr uint32_t COL_CYAN    = 0x00CCCC;
static constexpr uint32_t COL_WHITE   = 0xCCCCCC;
static constexpr uint32_t COL_BROWN   = 0x8B6914;
static constexpr uint32_t COL_GRAY    = 0x808080;
static constexpr uint32_t COL_DARK    = 0x333333;
static constexpr uint32_t COL_ORANGE  = 0xFF6600;

// Per-sprite intrinsic flip bits — bit 0 = h-flip, bit 1 = v-flip.
// Taken from the low bits of sprites_width_and_horizontal_flip_table
// (&5e0c) and sprites_height_and_vertical_flip_table (&5e89). The atlas
// PNG stores sprites in "raw ROM" orientation; these bits say how each
// sprite must be flipped to reach its display orientation.
static constexpr uint8_t SPRITE_INTRINSIC_FLIP[125] = {
    /* 0x00 */ 0, /* 0x01 */ 0, /* 0x02 */ 0, /* 0x03 */ 2,
    /* 0x04 */ 0, /* 0x05 */ 0, /* 0x06 */ 0, /* 0x07 */ 0,
    /* 0x08 */ 3, /* 0x09 */ 0, /* 0x0a */ 0, /* 0x0b */ 3,
    /* 0x0c */ 3, /* 0x0d */ 1, /* 0x0e */ 0, /* 0x0f */ 0,
    /* 0x10 */ 0, /* 0x11 */ 1, /* 0x12 */ 2, /* 0x13 */ 0,
    /* 0x14 */ 0, /* 0x15 */ 0, /* 0x16 */ 0, /* 0x17 */ 0,
    /* 0x18 */ 0, /* 0x19 */ 0, /* 0x1a */ 0, /* 0x1b */ 2,
    /* 0x1c */ 0, /* 0x1d */ 0, /* 0x1e */ 0, /* 0x1f */ 0,
    /* 0x20 */ 0, /* 0x21 */ 0, /* 0x22 */ 0, /* 0x23 */ 1,
    /* 0x24 */ 1, /* 0x25 */ 1, /* 0x26 */ 2, /* 0x27 */ 2,
    /* 0x28 */ 2, /* 0x29 */ 2, /* 0x2a */ 2, /* 0x2b */ 1,
    /* 0x2c */ 0, /* 0x2d */ 0, /* 0x2e */ 0, /* 0x2f */ 0,
    /* 0x30 */ 0, /* 0x31 */ 0, /* 0x32 */ 0, /* 0x33 */ 1,
    /* 0x34 */ 0, /* 0x35 */ 0, /* 0x36 */ 0, /* 0x37 */ 0,
    /* 0x38 */ 1, /* 0x39 */ 0, /* 0x3a */ 2, /* 0x3b */ 0,
    /* 0x3c */ 0, /* 0x3d */ 0, /* 0x3e */ 2, /* 0x3f */ 2,
    /* 0x40 */ 2, /* 0x41 */ 0, /* 0x42 */ 0, /* 0x43 */ 2,
    /* 0x44 */ 2, /* 0x45 */ 0, /* 0x46 */ 0, /* 0x47 */ 0,
    /* 0x48 */ 0, /* 0x49 */ 0, /* 0x4a */ 2, /* 0x4b */ 2,
    /* 0x4c */ 3, /* 0x4d */ 1, /* 0x4e */ 0, /* 0x4f */ 0,
    /* 0x50 */ 2, /* 0x51 */ 2, /* 0x52 */ 0, /* 0x53 */ 2,
    /* 0x54 */ 2, /* 0x55 */ 0, /* 0x56 */ 0, /* 0x57 */ 0,
    /* 0x58 */ 0, /* 0x59 */ 0, /* 0x5a */ 2, /* 0x5b */ 0,
    /* 0x5c */ 0, /* 0x5d */ 0, /* 0x5e */ 0, /* 0x5f */ 0,
    /* 0x60 */ 0, /* 0x61 */ 0, /* 0x62 */ 2, /* 0x63 */ 2,
    /* 0x64 */ 0, /* 0x65 */ 0, /* 0x66 */ 0, /* 0x67 */ 0,
    /* 0x68 */ 0, /* 0x69 */ 0, /* 0x6a */ 0, /* 0x6b */ 0,
    /* 0x6c */ 0, /* 0x6d */ 1, /* 0x6e */ 0, /* 0x6f */ 1,
    /* 0x70 */ 0, /* 0x71 */ 0, /* 0x72 */ 1, /* 0x73 */ 1,
    /* 0x74 */ 0, /* 0x75 */ 3, /* 0x76 */ 0, /* 0x77 */ 0,
    /* 0x78 */ 0, /* 0x79 */ 0, /* 0x7a */ 0, /* 0x7b */ 0,
    /* 0x7c */ 0,
};

struct FensterRenderer::Impl {
    std::vector<uint32_t> buf;
    struct fenster f;
    bool initialized = false;
    uint8_t vp_center_x = 0;
    uint8_t vp_center_y = 0;
    uint8_t vp_frac_x   = 0;   // sub-tile fractional position of the view centre
    uint8_t vp_frac_y   = 0;   // (0-255, same units as Fixed8_8 fraction)
    bool tile_outline_on = false;    // debug: draw tile cell borders
    bool tile_outline_key_prev = false;
    int key_scan_idx = 0;
    bool events_processed = false;
    bool should_close = false;
    int scale = 1;
    bool atlas_ready = false;

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
    int tile_px_x() const { return TILE_PX_BASE_X * scale; }
    int tile_px_y() const { return TILE_PX_BASE_Y * scale; }
    int vp_w_tiles() const { return win_w() / tile_px_x() + 2; }
    int vp_h_tiles() const { return (win_h() - HUD_PX) / tile_px_y() + 2; }
    int hud_y_px() const { return win_h() - HUD_PX; }

    void apply_pending_resize() {
        if (f.pending_w > 0 && f.pending_h > 0 &&
            (f.pending_w != f.width || f.pending_h != f.height)) {
            f.width = f.pending_w;
            f.height = f.pending_h;
            buf.assign((size_t)f.width * f.height, 0);
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

    void blit_sprite(int dst_x, int dst_y, uint8_t sprite_id,
                     bool flip_h, bool flip_v) {
        if (!atlas_ready || sprite_id > 0x7c) return;
        const SpriteAtlasEntry& e = sprite_atlas[sprite_id];
        const uint32_t* atlas = atlas_pixels();
        const int sxm = PX_SCALE_X * scale;
        const int sym = PX_SCALE_Y * scale;
        const int hud_y = hud_y_px();

        uint8_t intrinsic = SPRITE_INTRINSIC_FLIP[sprite_id];
        flip_h ^= (intrinsic & 1) != 0;
        flip_v ^= (intrinsic & 2) != 0;

        for (int sy = 0; sy < e.h; ++sy) {
            int src_y = flip_v ? (e.h - 1 - sy) : sy;
            const uint32_t* src_row = &atlas[(e.y + src_y) * ATLAS_W + e.x];
            for (int sx = 0; sx < e.w; ++sx) {
                int src_x = flip_h ? (e.w - 1 - sx) : sx;
                uint32_t px = src_row[src_x] & 0x00FFFFFF;
                if (px == 0) continue; // black = transparent

                int ox = dst_x + sx * sxm;
                int oy = dst_y + sy * sym;
                for (int dy = 0; dy < sym; ++dy) {
                    int py = oy + dy;
                    if (py < 0 || py >= hud_y) continue;
                    for (int dx = 0; dx < sxm; ++dx) {
                        int ppx = ox + dx;
                        if (ppx < 0 || ppx >= f.width) continue;
                        buf[(size_t)py * f.width + ppx] = px;
                    }
                }
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
        // Sub-tile offset: (wx_frac - vp_frac_x) is a signed 8.8 delta, scaled
        // to pixels. Same for y.
        int sub_x = (int(wx_frac) - int(vp_frac_x)) * tpx / 256;
        int sub_y = (int(wy_frac) - int(vp_frac_y)) * tpy / 256;
        sx = f.width / 2 + dx * tpx + sub_x + pan_px_x;
        sy = hud_y_px() / 2 + dy * tpy + sub_y + pan_px_y;
        return sx > -tpx && sx < f.width && sy > -tpy && sy < hud_y_px();
    }

    // Inverse of world_to_screen: screen pixel → world-tile offset from
    // the view center (camera center).
    void screen_to_tile_offset(int sx, int sy, int& tdx, int& tdy) const {
        int tpx = tile_px_x();
        int tpy = tile_px_y();
        int rel_x = sx - f.width / 2 - pan_px_x;
        int rel_y = sy - hud_y_px() / 2 - pan_px_y;
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

        // Left-click edge detection
        bool left_down = (f.mouse & 1) != 0;
        if (left_down && !left_was_down) {
            has_pending_click = true;
            pending_click_x = f.x;
            pending_click_y = f.y;
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

static uint32_t tile_color(uint8_t tile_type) {
    uint8_t t = tile_type & 0x3f;
    switch (t) {
        case 0x19: return COL_BLACK;
        case 0x2d: return COL_BROWN;
        case 0x1e: case 0x12: return COL_GRAY;
        case 0x2e: case 0x2f: return COL_BROWN;
        case 0x23: case 0x13: case 0x24: return COL_GRAY;
        case 0x0f: return COL_RED;
        case 0x1b: case 0x1a: return COL_GREEN;
        case 0x09: return COL_CYAN;
        case 0x0a: return COL_CYAN;
        case 0x0e: case 0x0b: return 0x4488CC;
        case 0x0d: return COL_BLUE;
        case 0x0c: return COL_ORANGE;
        case 0x21: return COL_WHITE;
        case 0x08: return COL_DARK;
        case 0x2b: case 0x2c: return COL_BROWN;
        case 0x10: return COL_GREEN;
        default: return (t == 0x00) ? COL_DARK : 0x444444;
    }
}

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

FensterRenderer::FensterRenderer() : impl_(std::make_unique<Impl>()) {}
FensterRenderer::~FensterRenderer() { shutdown(); }

bool FensterRenderer::init() {
    if (impl_->initialized) return true;
    if (fenster_open(&impl_->f) != 0) return false;
    for (const char* p : ATLAS_PATHS) {
        if (atlas_load(p)) { impl_->atlas_ready = true; break; }
    }
    impl_->initialized = true;
    return true;
}

void FensterRenderer::shutdown() {
    if (impl_->initialized) {
        fenster_close(&impl_->f);
        impl_->initialized = false;
    }
}

void FensterRenderer::begin_frame() {
    // Pump pending Windows messages first so mouse/size state is current.
    if (fenster_loop(&impl_->f) != 0) impl_->should_close = true;
    impl_->events_processed = true;
    impl_->key_scan_idx = 0;

    impl_->apply_pending_resize();

    if (impl_->f.wheel != 0) {
        impl_->scale = std::clamp(impl_->scale + impl_->f.wheel,
                                  MIN_SCALE, MAX_SCALE);
        impl_->f.wheel = 0;
        // Normalize pan after scale change so the view doesn't jump wildly.
        impl_->pan_px_x %= impl_->tile_px_x();
        impl_->pan_px_y %= impl_->tile_px_y();
    }

    impl_->process_mouse();

    // Debug: toggle tile-outline overlay on rising edge of 'G'.
    bool g_down = impl_->f.keys['G'] != 0;
    if (g_down && !impl_->tile_outline_key_prev) {
        impl_->tile_outline_on = !impl_->tile_outline_on;
    }
    impl_->tile_outline_key_prev = g_down;

    std::fill(impl_->buf.begin(), impl_->buf.end(), 0u);
}

void FensterRenderer::end_frame() {
    // Draw overlay text in top-right corner.
    if (!impl_->overlay.empty()) {
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

void FensterRenderer::set_viewport(uint8_t center_x, uint8_t center_y,
                                    uint8_t frac_x, uint8_t frac_y) {
    impl_->vp_center_x = center_x;
    impl_->vp_center_y = center_y;
    impl_->vp_frac_x   = frac_x;
    impl_->vp_frac_y   = frac_y;
}

void FensterRenderer::render_tile(uint8_t world_x, uint8_t world_y,
                                   const TileRenderInfo& info) {
    int sx, sy;
    if (!impl_->world_to_screen(world_x, world_y, sx, sy)) return;

    if (impl_->atlas_ready) {
        uint8_t entry = TILE_SPRITE_ID[info.tile_type & 0x3f];
        uint8_t sid = entry & 0x7f;
        bool override_flip_v = (entry & 0x80) != 0;
        if (entry != 0xff && sid <= 0x7c) {
            // Compute sub-tile offset so the sprite aligns to the
            // correct half of the cell when flipped — matches the
            // fraction-shifting at &2420-&243f in the disassembly.
            // Base y-offset comes from tiles_y_offset_and_pattern;
            // horizontally the base is always 0.
            const SpriteAtlasEntry& e = sprite_atlas[sid];
            int base_y_atlas = (tiles_y_offset_and_pattern[info.tile_type & 0x3f] >> 4) * 2;
            // upper nibble * 16 fractions / 8 fractions-per-atlas-row = upper nibble * 2

            bool final_v = info.flip_v ^ override_flip_v;
            int y_off_atlas = final_v
                ? (32 - base_y_atlas - e.h)   // tile_h - base - sprite_h
                : base_y_atlas;
            int x_off_atlas = info.flip_h
                ? (16 - e.w)                   // tile_w - sprite_w
                : 0;

            int scale = impl_->scale;
            int x_off_px = x_off_atlas * PX_SCALE_X * scale;
            int y_off_px = y_off_atlas * PX_SCALE_Y * scale;

            impl_->blit_sprite(sx + x_off_px, sy + y_off_px, sid,
                               info.flip_h, final_v);
            if (impl_->tile_outline_on) {
                impl_->stroke_rect(sx, sy,
                                   impl_->tile_px_x(), impl_->tile_px_y(),
                                   0x404040);
            }
            return;
        }
    }

    uint32_t col = tile_color(info.tile_type);
    if (col != COL_BLACK) {
        impl_->fill_rect(sx, sy, impl_->tile_px_x(), impl_->tile_px_y(), col);
    }

    if (impl_->tile_outline_on) {
        impl_->stroke_rect(sx, sy, impl_->tile_px_x(), impl_->tile_px_y(),
                           0x404040);
    }
}

void FensterRenderer::render_object(Fixed8_8 world_x, Fixed8_8 world_y,
                                     const SpriteRenderInfo& info) {
    if (!info.visible) return;
    int sx, sy;
    if (!impl_->world_to_screen(world_x.whole, world_y.whole, sx, sy,
                                world_x.fraction, world_y.fraction)) return;

    if (impl_->atlas_ready && info.sprite_id <= 0x7c) {
        // Anchor the sprite so its bottom sits at the world position — the
        // game's physics treats (x.whole, y.whole) as the feet tile, so
        // feet-at-position keeps visuals and collision consistent.
        const SpriteAtlasEntry& e = sprite_atlas[info.sprite_id];
        int sprite_h_px = e.h * PX_SCALE_Y * impl_->scale;
        impl_->blit_sprite(sx, sy - sprite_h_px, info.sprite_id,
                           info.flip_h, info.flip_v);
    } else {
        uint32_t col = object_color(info.type);
        int tx = impl_->tile_px_x();
        int ty = impl_->tile_px_y();
        impl_->fill_rect(sx + 1, sy - ty + 1, tx - 2, ty - 2, col);
    }
}

void FensterRenderer::render_hud(const PlayerState& player) {
    int hud_y = impl_->hud_y_px();
    int bar_width = (player.energy * (impl_->f.width - 20)) / 255;
    impl_->fill_rect(10, hud_y + 2, bar_width, 6, COL_GREEN);

    uint32_t weapon_colors[] = {COL_CYAN, COL_WHITE, 0x88CCFF, COL_MAGENTA, 0xFF00FF, COL_RED};
    int wi = player.weapon;
    if (wi > 5) wi = 0;
    impl_->fill_rect(impl_->f.width - 40, hud_y + 2, 30, 6, weapon_colors[wi]);
}

void FensterRenderer::render_particle(uint8_t wx, uint8_t wx_frac,
                                       uint8_t wy, uint8_t wy_frac,
                                       uint8_t colour) {
    // Map BBC mode-1 style 3-bit colour (0-7) to our palette.
    static constexpr uint32_t COLOURS[8] = {
        0x000000, // 0 black
        0xCC0000, // 1 red
        0x00CC00, // 2 green
        0xCCCC00, // 3 yellow
        0x2244CC, // 4 blue
        0xCC00CC, // 5 magenta
        0x00CCCC, // 6 cyan
        0xCCCCCC, // 7 white
    };
    uint32_t col = COLOURS[colour & 0x07];
    if (col == 0) return; // black → invisible

    int sx, sy;
    if (!impl_->world_to_screen(wx, wy, sx, sy, wx_frac, wy_frac)) return;
    // Draw as a `scale` x `scale` square so particles are visible at zoom.
    int s = impl_->scale;
    impl_->fill_rect(sx, sy, s, s, col);
}

int FensterRenderer::viewport_width_tiles() const { return impl_->vp_w_tiles(); }
int FensterRenderer::viewport_height_tiles() const { return impl_->vp_h_tiles(); }

bool FensterRenderer::consume_pan_tiles(int& dx, int& dy) {
    dx = impl_->pending_pan_tiles_x;
    dy = impl_->pending_pan_tiles_y;
    impl_->pending_pan_tiles_x = 0;
    impl_->pending_pan_tiles_y = 0;
    return dx != 0 || dy != 0;
}

bool FensterRenderer::consume_left_click(int& tile_dx, int& tile_dy) {
    if (!impl_->has_pending_click) { tile_dx = 0; tile_dy = 0; return false; }
    impl_->has_pending_click = false;
    impl_->screen_to_tile_offset(impl_->pending_click_x,
                                 impl_->pending_click_y,
                                 tile_dx, tile_dy);
    return true;
}

void FensterRenderer::set_overlay_text(const char* text) {
    impl_->overlay = text ? text : "";
}

int FensterRenderer::get_key() {
    if (impl_->should_close) return 'q';
    // begin_frame already pumped messages; just scan key state.
    while (impl_->key_scan_idx < 256) {
        int i = impl_->key_scan_idx++;
        if (!impl_->f.keys[i]) continue;

        switch (i) {
            case 17: return InputKey::UP;
            case 18: return InputKey::DOWN;
            case 19: return InputKey::RIGHT;
            case 20: return InputKey::LEFT;
            case 10: return InputKey::ENTER;
            case 27: return 'q';
            default:
                if (i >= 'A' && i <= 'Z') return i + 32;
                if (i >= '0' && i <= '9') return i;
                break;
        }
    }
    return InputKey::NONE;
}
