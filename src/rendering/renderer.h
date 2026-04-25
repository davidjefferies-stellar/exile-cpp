#pragma once
#include <cstdint>
#include "core/types.h"
#include "core/fixed_point.h"

struct TileRenderInfo {
    uint8_t tile_type;    // 0-63 (low 6 bits)
    uint8_t palette;
    bool    flip_h;
    bool    flip_v;
};

struct SpriteRenderInfo {
    uint8_t sprite_id;
    uint8_t palette;
    bool    flip_h;
    bool    flip_v;
    bool    visible;
    ObjectType type;
    // 6502 &0cfe reduce_sprite_if_teleporting. When non-zero, render shrinks
    // the sprite based on (timer & 7) — zero means no teleport effect.
    uint8_t teleport_timer = 0;
};

struct PlayerState {
    uint8_t energy;
    uint8_t weapon;
    uint8_t keys_collected;
    bool    has_jetpack_booster;
    // Port of &0848 player_pockets / &0847 player_pockets_used. Up to 5 stored
    // object types. Slot 0 is the "top" (most recently stored / next to
    // retrieve). Unused slots set to 0xff.
    uint8_t pockets[5] = {0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t pockets_used = 0;
};

namespace InputKey {
    constexpr int NONE       = -1;
    constexpr int LEFT       = 0x100;
    constexpr int RIGHT      = 0x101;
    constexpr int UP         = 0x102;
    constexpr int DOWN       = 0x103;
    constexpr int ENTER      = 0x104;
    constexpr int TAB        = 0x105;
    constexpr int CTRL_LEFT  = 0x106;
    constexpr int CTRL_RIGHT = 0x107;
}

class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual bool init() = 0;
    virtual void shutdown() = 0;

    virtual void begin_frame() = 0;
    virtual void end_frame() = 0;

    // Set viewport center in world tile coordinates. The fractional args
    // give sub-tile precision (0-255 = 0-1 tile), so the camera can
    // smoothly follow the player between tile boundaries.
    virtual void set_viewport(uint8_t center_x, uint8_t center_y,
                              uint8_t frac_x = 0, uint8_t frac_y = 0) = 0;

    // Render one tile at world coordinates
    virtual void render_tile(uint8_t world_x, uint8_t world_y,
                             const TileRenderInfo& info) = 0;

    // Paint the water backdrop for a single tile column. The 6502 does this
    // by reprogramming physical palette register 0 (VDU colour 0) mid-frame
    // via a raster timer at &12a6 / &12b8 / &12c2 / &12d8:
    //   - above the waterline:   colour 0 = black (&00)
    //   - on the waterline:      colour 0 = cyan  (&06) for one raster line
    //   - below the waterline:   colour 0 = blue  (&04)
    // Since colour-0 pixels in our blit_sprite are transparent, we emulate
    // this by pre-filling the appropriate screen cells with the water /
    // surface colours before the tile blits run. `world_x` is the column,
    // `waterline_y` is the returned get_waterline_y(world_x). Default no-op
    // for renderers that don't support it.
    virtual void render_water_column(uint8_t /*world_x*/,
                                     uint8_t /*waterline_y*/) {}

    // Render one object at world position
    virtual void render_object(Fixed8_8 world_x, Fixed8_8 world_y,
                               const SpriteRenderInfo& info) = 0;

    // Render one particle: a single pixel at world (x, y). Colour 0-7
    // (BBC mode-1 palette). Default no-op for renderers that don't support
    // particles.
    virtual void render_particle(uint8_t /*wx*/, uint8_t /*wx_frac*/,
                                 uint8_t /*wy*/, uint8_t /*wy_frac*/,
                                 uint8_t /*colour*/) {}

    // HUD
    virtual void render_hud(const PlayerState& player) = 0;

    // Viewport dimensions in tiles
    virtual int viewport_width_tiles() const = 0;
    virtual int viewport_height_tiles() const = 0;

    // Input: get last key press (non-blocking)
    virtual int get_key() = 0;

    // --- Optional mouse / overlay hooks. Default no-ops for renderers that
    //     don't support interactive input. ---

    // Pop accumulated right-drag pan delta in tiles (one tile = tile_px).
    // Returns true if a non-zero delta was consumed.
    virtual bool consume_pan_tiles(int& dx_tiles, int& dy_tiles) {
        dx_tiles = 0; dy_tiles = 0; return false;
    }

    // Pop a pending left-click as a screen-relative tile offset from the
    // viewport center. Returns false if no click happened since last call.
    virtual bool consume_left_click(int& tile_dx, int& tile_dy) {
        tile_dx = 0; tile_dy = 0; return false;
    }

    // Set overlay text drawn in the top-right corner.
    virtual void set_overlay_text(const char* /*text*/) {}

    // Mark a world tile as the "selected" tile. Renderers with a visible tile
    // grid should highlight this cell while the grid is on; others can ignore.
    virtual void set_highlighted_tile(uint8_t /*world_x*/, uint8_t /*world_y*/) {}

    // Debug overlay: draw a small coloured swatch + label at a world tile.
    // Each renderer can gate it on its own toggle; implementations that don't
    // support overlays should leave the default no-op.
    virtual void render_debug_marker(uint8_t /*world_x*/, uint8_t /*world_y*/,
                                     uint32_t /*rgb*/,
                                     const char* /*label*/) {}

    // Draw the activation-distance rings around the anchor point that drives
    // demotion / promotion / placeholder conversion. Renderers that support
    // a tile-grid toggle should gate this on the same toggle so all the
    // debug overlays come and go together.
    virtual void render_activation_overlay(uint8_t /*anchor_x*/,
                                           uint8_t /*anchor_y*/) {}

    // Debug AABB overlay: draw the pixel-precise bounding box used by
    // object-object collision for a single primary. Dimensions are given in
    // the 6502's sub-tile units (1/256 tile = 1/8 sprite pixel), matching
    // sprite_atlas.w/h minus one times 16/8 respectively. Gated by the
    // renderer's own toggle key ('B' on Fenster).
    virtual bool aabb_overlay_enabled() const { return false; }
    virtual void render_aabb(Fixed8_8 /*world_x*/, Fixed8_8 /*world_y*/,
                             int /*w_units*/, int /*h_units*/,
                             uint32_t /*rgb*/) {}

    // --- Debug-overlay checkbox state, driven by the HUD-strip checkboxes.
    //     Game reads these each frame to decide whether to render the
    //     tile grid / tier labels / activation rings and whether the
    //     camera, not the player, drives the activation anchor.
    virtual bool tile_grid_enabled()    const { return false; }
    virtual bool object_tiers_enabled() const { return false; }
    virtual bool map_mode_enabled()     const { return false; }
    // Split wiring overlay into two toggles so users can focus on one
    // relation at a time. Game reads each gate independently.
    virtual bool switches_enabled()     const { return false; }
    virtual bool transports_enabled()   const { return false; }
    // Collision-debug overlay: shade the solid region of every visible
    // tile according to its obstruction pattern (per-x-section threshold
    // from tile_data.h). Makes sink-through / slope / door-substitute
    // bugs visible. Game passes each visible tile through
    // render_collision_tile below when this returns true.
    virtual bool collision_enabled()    const { return false; }

    // Shade a sub-tile rectangle in the given RGB. Coordinates are the
    // 6502's 1/256-tile fraction units — same space as Fixed8_8 fraction
    // and object AABBs. `world_x`/`world_y` select the tile; `x_frac`
    // and `y_frac` the top-left of the sub-rectangle within it.
    // Default no-op for renderers without overlay support.
    virtual void render_tile_shade_rect(uint8_t /*world_x*/, uint8_t /*world_y*/,
                                        uint8_t /*x_frac*/,  uint8_t /*y_frac*/,
                                        uint8_t /*w_frac*/,  uint8_t /*h_frac*/,
                                        uint32_t /*rgb*/) {}

    // Draw a thin line between two world-tile positions in the given RGB.
    // Coordinates are tile-whole; game code picks the tile each end sits
    // on. Default no-op so only renderers with overlay support need to
    // implement it.
    virtual void render_wire(uint8_t /*x1*/, uint8_t /*y1*/,
                             uint8_t /*x2*/, uint8_t /*y2*/,
                             uint32_t /*rgb*/) {}
};
