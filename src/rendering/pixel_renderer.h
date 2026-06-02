#pragma once

#include "rendering/renderer.h"
#include "rendering/window_state.h"
#include <cstdint>

// sokol_app event type, declared via forward struct to avoid pulling
// sokol_app.h's win32 surface into every TU that includes this header.
// The full definition is included by pixel_renderer.cpp where handle_event
// is implemented, and by main.cpp where the callbacks live.
struct sapp_event;
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

// BBC mode-1 pixels are 2:1 — each atlas pixel draws two screen pixels
// wide, one tall. Atlas tile sprites are 16 atlas-px wide × 32 tall, so
// the tile grid is 32 screen-px wide × 32 tall at 1× scale.
inline constexpr int PX_SCALE_X     = 2;
inline constexpr int PX_SCALE_Y     = 1;
inline constexpr int TILE_PX_BASE_X = 16 * PX_SCALE_X;  // 32
inline constexpr int TILE_PX_BASE_Y = 32 * PX_SCALE_Y;  // 32
inline constexpr int INITIAL_W      = 1728;
inline constexpr int INITIAL_H      = 972;
inline constexpr int HUD_PX         = TILE_PX_BASE_Y / 2;
// Zoom factor = scale / zoom_den. MIN_SCALE=1 with MAX_ZOOM_DEN > 1
// lets the wheel keep zooming out past 1:1.
inline constexpr int MIN_SCALE      = 1;
inline constexpr int MAX_SCALE      = 8;
inline constexpr int MAX_ZOOM_DEN   = 5;

// Floor division by 256 — rounds toward -inf rather than toward zero.
//
// world_to_screen's sub-tile offset is `(obj_frac - vp_frac) * tpx / 256`.
// The numerator can be negative; C's truncate-toward-zero leaves a
// 1-pixel seam between a static object and the landscape tile it sits
// on whenever the viewport's fraction crosses a tile-pixel boundary.
inline int floor_div_256(int v) {
    return (v >= 0) ? (v >> 8) : -((-v + 255) >> 8);
}

// Tile type (0-0x3f) -> atlas sprite_id. Bit 7 XORs flip_v (matches the
// original's tiles_sprite_and_y_flip_table at &04ab). Defined in
// pixel_renderer.cpp; declared here so the debug TU can index it for
// palette previews and the sprite-viewer cross-reference panel.
extern const uint8_t TILE_SPRITE_ID[64];

class PixelRenderer : public IRenderer {
public:
    PixelRenderer();
    ~PixelRenderer() override;

    bool init() override;
    void shutdown() override;
    void begin_frame() override;
    void end_frame() override;
    // sokol_app event dispatch: populates `f` (key state, mouse pos/buttons,
    // wheel, modifiers, pending resize). Called from the event_cb registered
    // in main.cpp's sapp_desc. Definition is in pixel_renderer.cpp where
    // sokol_app.h is included.
    void handle_event(const sapp_event* ev);
    void set_clear_colour(uint32_t rgb) override { clear_colour_ = rgb; }
    void set_viewport(uint8_t center_x, uint8_t center_y,
                      uint8_t frac_x = 0, uint8_t frac_y = 0) override;
    void render_tile(uint8_t world_x, uint8_t world_y,
                     const TileRenderInfo& info) override;
    void render_water_column(uint8_t world_x,
                             uint8_t waterline_y,
                             uint8_t waterline_y_frac = 0) override;
    void render_object(Fixed8_8 world_x, Fixed8_8 world_y,
                       const SpriteRenderInfo& info) override;
    void render_particle(uint8_t wx, uint8_t wx_frac,
                         uint8_t wy, uint8_t wy_frac,
                         uint8_t colour) override;
    bool query_fg_at(uint8_t wx, uint8_t wx_frac,
                     uint8_t wy, uint8_t wy_frac) const override;
    void render_hud(const PlayerState& player) override;
    int viewport_width_tiles() const override;
    int viewport_height_tiles() const override;
    void set_subpixel_mode(SubpixelMode mode) override { subpixel_mode = mode; }
    void set_zoom_den(int den) override {
        if (den < 1) den = 1;
        if (den > MAX_ZOOM_DEN) den = MAX_ZOOM_DEN;
        zoom_den = den;
    }
    void set_camera_motion(int8_t vx, int8_t vy) override {
        cam_vx = vx; cam_vy = vy;
    }
    int get_key() override;
    bool consume_pan_tiles(int& dx, int& dy) override;
    bool consume_left_click(int& tile_dx, int& tile_dy) override;
    bool consume_right_click(int& tile_dx, int& tile_dy) override;
    void set_overlay_text(const char* text) override;
    void set_menu_overlay(const char* text, int selection_index) override;
    bool capture_bmp(std::string& out) override;
    void set_fps_text(const char* text) override;
    void set_highlighted_tile(uint8_t world_x, uint8_t world_y) override;
    void render_debug_marker(uint8_t world_x, uint8_t world_y,
                             uint8_t x_frac, uint8_t y_frac,
                             uint32_t rgb, const char* label) override;
    void render_activation_overlay(uint8_t anchor_x, uint8_t anchor_y) override;
    void render_aabb(Fixed8_8 world_x, Fixed8_8 world_y,
                     int w_units, int h_units, uint32_t rgb) override;
    bool tile_grid_enabled()    const override;
    bool object_tiers_enabled() const override;
    bool map_mode_enabled()     const override;
    bool switches_enabled()     const override;
    bool transports_enabled()   const override;
    bool algo_only_enabled()    const override;
    bool collision_enabled()    const override;
    bool editor_enabled()       const override;
    bool sprite_viewer_enabled() const override;
    bool health_bars_enabled()  const override;
    bool damage_overlay_enabled() const override;
    bool mood_overlay_enabled()   const override;
    void render_damage_events(const std::vector<DamageVisual>& events) override;
    void render_world_label(uint8_t world_x, uint8_t world_y,
                            uint8_t x_frac, uint8_t y_frac,
                            int pixel_dx, int pixel_dy,
                            const char* text,
                            uint32_t fg, uint32_t bg) override;
    void render_sprite_viewer() override;
    void set_paint_tile(uint8_t tile_type) override;
    bool consume_palette_click(uint8_t& tile_type) override;
    bool events_panel_enabled() const override { return events_panel_on; }
    bool consume_event_click(int& event_id) override;
    bool saves_panel_enabled() const override { return saves_panel_on; }
    void set_save_files(const std::vector<std::string>& paths) override;
    bool consume_save_load_request(std::string& path) override;
    void set_debug_log(std::ostream* log) override { debug_log_ = log; }
    std::ostream* debug_log_ = nullptr;
    bool consume_palette_flip_click(int& which) override;
    bool consume_palette_detach_click() override;
    void set_paint_object(int idx) override;
    bool consume_object_palette_click(int& idx) override;
    uint8_t object_palette_type(int idx) const override;
    void render_wire(uint8_t x1, uint8_t y1,
                     uint8_t x2, uint8_t y2, uint32_t rgb) override;
    void render_tile_shade_rect(uint8_t world_x, uint8_t world_y,
                                uint8_t x_frac,  uint8_t y_frac,
                                uint8_t w_frac,  uint8_t h_frac,
                                uint32_t rgb) override;

    // ---------------------------------------------------------------
    // Implementation state and helpers. Public so the small
    // pr_debug:: extension surface (defined in pixel_renderer_debug.cpp)
    // can render its overlays without the friend boilerplate. Only
    // main.cpp includes this header externally and it doesn't poke
    // at internals.
    // ---------------------------------------------------------------
    std::vector<uint32_t> buf;
    // Parallel per-pixel foreground mask. Bit set = a tile wrote a BBC
    // logical-colour-8..15 pixel here, which the 6502 marks by leaving
    // bit 7 set on the plotted byte. The object-plot pass BMIs past any
    // pixel that already has bit 7 set (&1066 BMI skip_byte) — objects
    // hide behind those tile pixels.
    std::vector<uint8_t> fg_mask;
    // Background-flash colour for begin_frame's clear pass. Driven by
    // Game::update_background_flash (port of &1f97). Default 0 = black.
    uint32_t clear_colour_ = 0;
    WinState f;
    bool initialized = false;
    uint8_t vp_center_x = 0;
    uint8_t vp_center_y = 0;
    uint8_t vp_frac_x   = 0;   // sub-tile fractional view-centre position
    uint8_t vp_frac_y   = 0;   // (0-255, same units as Fixed8_8 fraction)
    // Debug-overlay toggles, exposed through IRenderer::*_enabled() so
    // the game layer reads them each frame. Driven by the checkbox
    // strip at the bottom of the window.
    bool tile_outline_on = false;    // "Tiles" checkbox (also drives the text overlay that used to live behind "Debug")
    bool object_tiers_on = false;    // "Object tiers" checkbox
    bool map_mode_on     = false;    // "Map mode" checkbox
    bool switches_on     = false;    // "Switches" — green switch->door wires
    bool transports_on   = false;    // "Transports" — cyan transporter wires
    bool collision_on    = false;    // "Collision" — solid-region shading
    bool editor_on       = false;    // "Edit" — left-click paints tile
    bool sprite_viewer_on = false;   // "Sprites" — sprite-viewer overlay
    bool health_bars_on   = false;   // "Health" — HP bar over each primary
    bool damage_overlay_on = false;  // "Damage" — per-frame damage numbers + radius rings
    bool mood_overlay_on   = false;  // "Mood"   — per-NPC mood/FED status text
    int  sprite_viewer_selected = 0; // currently-selected sprite id (0..0x7c)
    int  sprite_viewer_pixel_x  = -1;// last clicked atlas pixel on the ROM
    int  sprite_viewer_pixel_y  = -1;// sheet (-1 = none)
    bool rings_on        = false;    // "Rings" — activation distance boxes
    bool algo_only_on    = false;    // "Algo only" — hide map-data cells
    bool tertiary_overlay_on = false;// "Tertiary" — magenta/yellow borders
    // "Placed Tiles" sub-toggle in the Tiles submenu. Off by default;
    // when on, draws the cyan border on map_overlay_data-sourced cells
    // (and its slot-number label, red for aliased duplicates).
    bool placed_tiles_on = false;
    // "Events" checkbox — when on, the right-side test-events panel
    // appears. Game polls consume_event_click each tick and dispatches.
    bool events_panel_on = false;
    bool has_pending_event_click = false;
    int  pending_event_id        = 0;
    // Click-feedback state. last_event_clicked = which button to flash;
    // event_flash_remaining counts down each frame (visible green flash
    // for ~30 frames so the user sees the click registered).
    int  last_event_clicked      = -1;
    int  event_flash_remaining   = 0;
    // "Saves" panel state. saves_panel_on toggles the left-side file
    // browser; saves_list_ is pushed by Game on toggle-on; saves_highlight_
    // is the keyboard / hover cursor; saves_scroll_ is the topmost visible
    // entry. has_pending_save_load / pending_save_load_path_ are popped by
    // Game::consume_save_load_request.
    bool saves_panel_on   = false;
    std::vector<std::string> saves_list_;
    int  saves_highlight_ = 0;
    int  saves_scroll_    = 0;
    bool has_pending_save_load   = false;
    std::string pending_save_load_path_;
    // Edge-detection bitmask for saves-panel nav keys (UP=1, DOWN=2, ENTER=4).
    // handle_saves_key only fires the action on the press edge; the prev/curr
    // pair is rotated in begin_frame so held keys don't auto-repeat.
    uint8_t saves_keys_held_prev_ = 0;
    uint8_t saves_keys_held_curr_ = 0;
    // Highlighted tile — drawn only while the tile grid is on.
    bool has_highlight = false;
    uint8_t highlight_x = 0;
    uint8_t highlight_y = 0;
    int key_scan_idx = 0;
    bool events_processed = false;
    bool should_close = false;
    // Zoom factor = scale / zoom_den. Only one of the two is >1 at any
    // time: wheel up from 1:1 grows `scale`; wheel down grows `zoom_den`.
    // Start two notches zoomed in (3:1) so the BBC tile reads chunkier
    // by default.
    int scale = 3;
    int zoom_den = 7;

    // [render] subpixel_rendering ini setting. Off snaps fractions to
    // the BBC pixel grid; On honours every frac unit; Adaptive snaps
    // only when the object (or camera) is exactly stationary.
    SubpixelMode subpixel_mode = SubpixelMode::Off;
    int8_t cam_vx = 0;
    int8_t cam_vy = 0;

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
    int pending_click_x = 0;
    int pending_click_y = 0;
    // Right-click vs right-drag distinguished by motion accumulated
    // while the button was held.
    int  right_press_x   = 0;
    int  right_press_y   = 0;
    int  right_motion    = 0;
    bool has_pending_right_click = false;
    int  pending_right_click_x   = 0;
    int  pending_right_click_y   = 0;
    std::string overlay;
    std::string menu_overlay_;
    int         menu_selection_ = -1;
    std::string fps_text_;
    // Edit-palette state mirrored from Game so the panel can highlight
    // the selected paint tile.
    uint8_t paint_tile_ = 0x19;
    bool    has_pending_palette_click = false;
    uint8_t pending_palette_click_type = 0;
    int     pending_palette_flip = -1;   // -1 none, 0 FlipX, 1 FlipY
    bool    pending_detach_click = false;
    int     paint_object_idx = -1;
    bool    has_pending_object_click = false;
    int     pending_object_click_idx = 0;

    // Inline accessors used hot-path enough to be worth the header.
    int win_w() const { return f.width; }
    int win_h() const { return f.height; }
    // Public read-only access to the CPU framebuffer so the sokol_gfx
    // textured-quad present (in main.cpp's frame_cb) can upload it. Stride
    // is `win_w()` uint32_t pixels.
    const uint32_t* framebuffer() const { return buf.data(); }
    int tile_px_x() const { return (TILE_PX_BASE_X * scale) / zoom_den; }
    int tile_px_y() const { return (TILE_PX_BASE_Y * scale) / zoom_den; }
    int vp_w_tiles() const { return win_w() / tile_px_x() + 2; }
    int vp_h_tiles() const { return (win_h() - HUD_PX) / tile_px_y() + 2; }
    int hud_y_px() const { return win_h() - HUD_PX; }

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

    void apply_pending_resize();

    // Sparse-checker shade. Plots `color` on pixels where (px+py) %
    // step == 0 — used by the tertiary-source-alias overlay.
    void hatch_rect(int x, int y, int w, int h, uint32_t color, int step);

    // 1-pixel outline rectangle, clipped to screen and HUD.
    void stroke_rect(int x, int y, int w, int h, uint32_t color);

    // Blit a sprite at its NATURAL 2:1 BBC pixel scale, centred inside
    // (cell_x, cell_y, cell_w, cell_h). Used by the palette panels.
    void blit_sprite_at_native(int cell_x, int cell_y,
                               int cell_w, int cell_h,
                               uint8_t sprite_id,
                               const uint32_t lut[4],
                               bool flip_h, bool flip_v);

    // Edit-palette tile preview.
    void blit_tile_preview(int dst_x, int dst_y,
                           int dst_w, int dst_h,
                           uint8_t tile_type,
                           bool flip_h = false, bool flip_v = false);

    // Box-filtered sprite blit.
    void blit_sprite(int dst_x, int dst_y, uint8_t sprite_id,
                     bool flip_h, bool flip_v, const uint32_t lut[4],
                     const uint8_t fg[4] = nullptr, bool is_tile = false,
                     uint8_t shrink_shift_x = 0,
                     uint8_t shrink_shift_y = 0);

    void draw_glyph(int x, int y, char ch, uint32_t fg, uint32_t bg);
    int draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg);

    // vx / vy are the object's velocity. Adaptive subpixel mode snaps
    // the rendered position to the BBC pixel grid only when both axes
    // are exactly 0; any motion stays at full frac precision. Tiles
    // and other stationary draws can omit the velocity (default 0).
    bool world_to_screen(uint8_t wx, uint8_t wy, int& sx, int& sy,
                         uint8_t wx_frac = 0, uint8_t wy_frac = 0,
                         int8_t vx = 0, int8_t vy = 0) const;

    void screen_to_tile_offset(int sx, int sy, int& tdx, int& tdy) const;

    // Right-drag / left-click edge detection. Debug-widget hits are
    // delegated to pr_debug::consume_left_click.
    void process_mouse();

private:
    // Inventory-HUD helper. Renders an object-type sprite centred in a
    // HUD cell at BBC 2:1; obj_type 0xff leaves the cell empty. `dim`
    // halves the palette so unselected weapons / un-collected keys read
    // as inactive.
    void blit_obj_sprite_cell(int cx, int cy, int cell_size,
                              uint8_t obj_type, bool dim);
};

// Debug-overlay extension surface implemented in
// pixel_renderer_debug.cpp. The faithful 6502 TU calls into these so
// it doesn't need to know about editor checkboxes / palettes / the
// sprite-viewer.
namespace pr_debug {
    // Returns true iff the click at (r.f.x, r.f.y) was absorbed by a
    // debug widget; caller forwards unconsumed clicks as a world click.
    bool consume_left_click(PixelRenderer& r);

    // Top-right text overlay drawn from end_frame.
    void render_overlay_text(PixelRenderer& r);

    // Centered modal text (Esc menu). Hidden when r.menu_overlay_ is empty.
    void render_menu_overlay(PixelRenderer& r);

    // Optional FPS readout drawn at the top-right, independent of the
    // debug overlay. Shifts render_overlay_text down by its box height
    // when both are active.
    void render_fps_text(PixelRenderer& r);

    // Right-side test-events panel — visible while the "Events" checkbox
    // is on. Each button posts a pending event id that Game consumes.
    void render_events_panel(PixelRenderer& r);

    // Left-side scrollable saves browser — visible while the "Saves"
    // checkbox is on. Game populates the file list via set_save_files.
    void render_saves_panel(PixelRenderer& r);

    // Hover hook — when the cursor is inside the saves panel, track
    // which row it's over and move the highlight there.
    void saves_panel_hover(PixelRenderer& r);

    // Right-side "Tiles" submenu — visible while the main-strip Tiles
    // box is on. Holds Map mode / Rings / Object lbl / Switches /
    // Transports / Tertiary / Placed Tiles toggles that were lifted
    // out of the bottom HUD strip.
    void render_grid_panel(PixelRenderer& r);

    // Per-tile grid + tertiary border / hatch overlays drawn from
    // render_tile when the Grid checkbox is on.
    void render_tile_overlay(PixelRenderer& r,
                             int sx, int sy,
                             uint8_t world_x, uint8_t world_y,
                             const TileRenderInfo& info);

    // Bottom-strip checkboxes + edit / object palettes drawn from
    // render_hud. Returns true iff the sprite-viewer is active so the
    // caller skips the rest of the inventory HUD.
    bool render_hud_panels(PixelRenderer& r);
}
