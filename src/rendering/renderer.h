#pragma once
#include <cstdint>
#include "core/types.h"
#include "core/damage_visual.h"
#include <vector>
#include "core/fixed_point.h"

struct TileRenderInfo {
    uint8_t tile_type;    // 0-63 (low 6 bits)
    uint8_t palette;
    bool    flip_h;
    bool    flip_v;
    // True when the cell has a tertiary entry attached. Used by the
    // grid overlay (Grid checkbox) to draw an alternate-coloured cell
    // border so map authors can see at a glance where doors / switches
    // / transporters / spawnable objects live.
    bool    has_tertiary = false;
    // True when the cell was sourced from the hand-authored
    // map_overlay_data at bake time (port of &00 tile_was_from_map_data,
    // set in get_tile_from_map_data at &17d6). Drawn cyan in the grid
    // overlay so authored geometry — the player's spaceship interior,
    // Triax's lab, set-piece corridors — stands out from procedurally
    // generated cavern.
    bool     from_map_data = false;
    // Index into map_overlay_data[0..1023] for cells where
    // from_map_data is true. The grid overlay prints this number inside
    // the cyan-bordered cell so authors can match a world cell to its
    // ROM table slot at a glance.
    uint16_t map_data_offset = 0;
    // True when another from-map-data cell shares this cell's
    // map_data_offset. The grid overlay swaps cyan for red on these so
    // duplicate ROM-slot references stand out.
    bool     map_data_aliased = false;
    // True when this cell resolves to the same source-table tertiary
    // entry as at least one other cell. The grid overlay shades the
    // cell red so authors can spot two map tiles emitting the same
    // tertiary marker.
    bool     tertiary_source_aliased = false;
    // True when this cell is a SWITCH (tile_type 0x08) AND another
    // SWITCH lives in the same world X column. The grid overlay
    // shades the cell yellow so authors can spot switches that share
    // an X (and therefore share runtime state under the 6502's
    // x-only-in-range scan).
    bool     switch_x_aliased = false;
    // True when this cell's RAW landscape byte is SWITCH (0x08) AND a
    // tertiary is attached. The resolved tile_type is something else
    // (wind / possible_leaf) because switches render as a background
    // tile with a primary object on top — so this flag is the only
    // reliable "this cell IS a switch" signal at render time.
    bool     is_switch = false;
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
    bool    has_jetpack_booster;
    // Port of &0848 player_pockets / &0847 player_pockets_used. Up to 5 stored
    // object types. Slot 0 is the "top" (most recently stored / next to
    // retrieve). Unused slots set to 0xff.
    uint8_t pockets[5] = {0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t pockets_used = 0;
    // Port of &0806 player_keys_collected. Indices 0..5 map to the six
    // collectable key types (CYAN_YELLOW_GREEN_KEY ... BLUE_CYAN_GREEN_KEY);
    // 6..7 are unused. Each byte is 0x80 when collected, 0 otherwise.
    uint8_t keys[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    // Port of &08?? weapon_energy table. One 16-bit counter per weapon slot:
    // 0=jetpack, 1=pistol, 2=icer, 3=blaster, 4=plasma, 5=suit (see
    // weapon.h). The HUD highlights `weapon` and shows non-zero energies.
    uint16_t weapon_energy[6] = {0, 0, 0, 0, 0, 0};
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

    // Pop a pending right-click. Distinguished from a right-drag (camera
    // pan) by a small motion threshold; a press+release without
    // significant movement queues a click here and skips the pan path.
    // Used by the editor as the PAINT action so left-click can stay as
    // SELECT.
    virtual bool consume_right_click(int& tile_dx, int& tile_dy) {
        tile_dx = 0; tile_dy = 0; return false;
    }

    // Set overlay text drawn in the top-right corner.
    virtual void set_overlay_text(const char* /*text*/) {}

    // Override the colour begin_frame clears the framebuffer to. Drives
    // the &1f97 update_background_flash effect: while the cooldown is
    // active the 6502 randomises palette colour 0 (the sky/black slot)
    // each frame. Default 0 = black; pass any 0xRRGGBB to flash.
    virtual void set_clear_colour(uint32_t /*rgb*/) {}

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

    // Render a short text label anchored at a world tile. Used for the
    // health-bar mood badge and any other small per-object debug text.
    // x_frac/y_frac give the sub-tile position (1/256 of a tile, same
    // unit as Object::x.fraction) so the label tracks moving NPCs
    // smoothly; pixel_dx/pixel_dy are an extra screen-pixel offset
    // applied after the tile-to-screen projection. fg/bg are 0xRRGGBB.
    virtual void render_world_label(uint8_t /*world_x*/, uint8_t /*world_y*/,
                                    uint8_t /*x_frac*/, uint8_t /*y_frac*/,
                                    int /*pixel_dx*/, int /*pixel_dy*/,
                                    const char* /*text*/,
                                    uint32_t /*fg*/, uint32_t /*bg*/) {}

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
    // "Algo only" — when on, cells whose landscape byte was sourced from
    // map_overlay_data are rendered as SPACE so only the procedural
    // generator's output is visible. Lets us inspect bake bugs without
    // the hand-authored interiors confusing the picture.
    virtual bool algo_only_enabled()    const { return false; }
    // Collision-debug overlay: shade the solid region of every visible
    // tile according to its obstruction pattern (per-x-section threshold
    // from tile_data.h). Makes sink-through / slope / door-substitute
    // bugs visible. Game passes each visible tile through
    // render_collision_tile below when this returns true.
    virtual bool collision_enabled()    const { return false; }
    // "Edit" checkbox — when on, the game treats left-click as a paint
    // (write tile) instead of a select (populate info overlay). The
    // paint tile itself is whatever was last clicked while edit was OFF,
    // tracked by Game.
    virtual bool editor_enabled()       const { return false; }
    // "Sprites" checkbox — when on, Game skips the world render and the
    // renderer paints a sprite-viewer overlay (palette grid, selected
    // sprite at high zoom, full BBC sheet with the selection highlighted)
    // instead. A debug aid for verifying which atlas region each
    // sprite_id resolves to. Click handling lives in the renderer.
    virtual bool sprite_viewer_enabled() const { return false; }
    // "Health" checkbox — when on, Game draws a small horizontal bar
    // above every active primary (filled width = obj.energy / 0xff,
    // green→red gradient) so creature HP and projectile lifespan are
    // both visually inspectable.
    virtual bool health_bars_enabled() const { return false; }
    // "Damage" checkbox — when on, Game records every damage event of the
    // current frame (bullets, explosion radius, contact damage, …) and
    // draws a floating amount above each victim plus a circle outline at
    // each explosion source showing its effective radius.
    virtual bool damage_overlay_enabled() const { return false; }
    // "Mood" checkbox — when on, Game draws the per-NPC mood badge
    // (HAPPY/CALM/ANGRY/FURY) and the WAS_FED status text above each
    // stimuli-eligible primary. Independent of the Health bar.
    virtual bool mood_overlay_enabled() const { return false; }
    // Paint the per-frame damage log. Renderers that don't support it
    // can no-op; the pixel renderer draws a number at each victim and a
    // ring around each explosion source.
    virtual void render_damage_events(const std::vector<DamageVisual>& /*events*/) {}
    // Renderer paints the sprite-viewer panels. Game calls this when
    // sprite_viewer_enabled() returns true, in place of the world render.
    virtual void render_sprite_viewer() {}

    // Editor palette: tell the renderer which tile type Game's "paint
    // source" currently is, so the palette panel can highlight it.
    virtual void set_paint_tile(uint8_t /*tile_type*/) {}

    // Pop a pending palette click. Returns true and sets `tile_type`
    // (0..0x3f) when the user clicked a palette cell since the last
    // call; false otherwise.
    virtual bool consume_palette_click(uint8_t& tile_type) {
        tile_type = 0;
        return false;
    }

    // Pop a pending FlipX / FlipY toggle click from the palette panel.
    // `which` = 0 for FlipX (bit 7), 1 for FlipY (bit 6). Game XORs the
    // matching bit on its own paint state.
    virtual bool consume_palette_flip_click(int& which) {
        which = -1;
        return false;
    }

    // Pop a pending Detach-tertiary button click. Game responds by
    // setting the highlighted cell's tertiary index to NO_TERTIARY.
    virtual bool consume_palette_detach_click() { return false; }

    // Object palette: highlights the currently-selected object (so the
    // panel can show which one right-click will place), and pops a
    // pending click on a palette cell.
    virtual void set_paint_object(int /*idx*/) {}
    virtual bool consume_object_palette_click(int& idx) {
        idx = -1;
        return false;
    }
    // Map an object-palette index back to its ObjectType byte. Used by
    // Game so the renderer owns the palette layout while Game owns the
    // placement semantics. Returns 0 if idx out of range.
    virtual uint8_t object_palette_type(int /*idx*/) const { return 0; }

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
