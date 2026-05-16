#pragma once
#include <cstdint>
#include <iosfwd>
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
    // Cell from map_overlay_data (&00 tile_was_from_map_data, set at
    // &17d6). Drawn cyan in grid to mark authored geometry.
    bool     from_map_data = false;
    // Index into map_overlay_data[0..1023] (when from_map_data set).
    // Grid overlay prints this inside the cyan-bordered cell.
    uint16_t map_data_offset = 0;
    // True when another from-map-data cell shares this cell's
    // map_data_offset. The grid overlay swaps cyan for red on these so
    // duplicate ROM-slot references stand out.
    bool     map_data_aliased = false;
    // Cell shares its source-table tertiary entry with another cell —
    // grid overlay shades red to flag duplicate markers.
    bool     tertiary_source_aliased = false;
    // Switch shares its X column with another switch — yellow shade
    // flags pairs that share runtime state under 6502 x-only scan.
    bool     switch_x_aliased = false;
    // RAW landscape byte = SWITCH (0x08) + tertiary attached. Resolved
    // type is wind/possible_leaf because switches render as background
    // + primary; this flag is the only "IS a switch" signal at render.
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
    constexpr int CTRL_LEFT       = 0x106;
    constexpr int CTRL_RIGHT      = 0x107;
    constexpr int ESCAPE          = 0x108;
    constexpr int SHIFT_LEFT      = 0x109;
    // Synthetic — set when the window's close button was clicked;
    // surfaces to input as state_.quit so Game::run can exit.
    constexpr int CLOSE_REQUESTED = 0x10a;
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

    // &12a6-&12d8 raster palette swap: pre-fill cells black/cyan/blue
    // above/on/below waterline since blit_sprite leaves colour-0 pixels
    // transparent. Default no-op for renderers without water support.
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

    // Query whether a world-position pixel was painted by a tile's
    // foreground (BBC logical colours 8-15). Port of the 6502 test at
    // &2118 — particles without PARTICLE_FLAG_FOREGROUND that XOR onto a
    // foreground pixel are removed (&2120 BEQ remove_particle_after_
    // unplotting). Default false for renderers without an fg mask.
    virtual bool query_fg_at(uint8_t /*wx*/, uint8_t /*wx_frac*/,
                             uint8_t /*wy*/, uint8_t /*wy_frac*/) const {
        return false;
    }

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

    // Pop a pending right-click — press+release without drag motion.
    // Editor uses this as PAINT so left-click can stay as SELECT.
    virtual bool consume_right_click(int& tile_dx, int& tile_dy) {
        tile_dx = 0; tile_dy = 0; return false;
    }

    // Set overlay text drawn in the top-right corner.
    virtual void set_overlay_text(const char* /*text*/) {}

    // Optional FPS readout in the top-right corner, independent of the
    // debug-overlay toggle. Empty / null hides it.
    virtual void set_fps_text(const char* /*text*/) {}

    // Drives &1f97 update_background_flash: 6502 randomises palette
    // colour 0 during cooldown. Default 0=black; any 0xRRGGBB flashes.
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

    // Short text label at world tile + sub-tile frac (1/256 unit, same
    // as Object::x.fraction) plus screen pixel_dx/dy offset.
    // fg/bg are 0xRRGGBB.
    virtual void render_world_label(uint8_t /*world_x*/, uint8_t /*world_y*/,
                                    uint8_t /*x_frac*/, uint8_t /*y_frac*/,
                                    int /*pixel_dx*/, int /*pixel_dy*/,
                                    const char* /*text*/,
                                    uint32_t /*fg*/, uint32_t /*bg*/) {}

    // AABB overlay for object-object collision. Dimensions in 6502
    // sub-tile units (1/256 tile = 1/8 sprite pixel). Gated by the
    // "Collision" HUD checkbox.
    virtual void render_aabb(Fixed8_8 /*world_x*/, Fixed8_8 /*world_y*/,
                             int /*w_units*/, int /*h_units*/,
                             uint32_t /*rgb*/) {}

    // HUD-strip checkbox state — Game reads each frame to gate overlays.
    virtual bool tile_grid_enabled()    const { return false; }
    virtual bool object_tiers_enabled() const { return false; }
    virtual bool map_mode_enabled()     const { return false; }
    // Split wiring overlay into two toggles so users can focus on one
    // relation at a time. Game reads each gate independently.
    virtual bool switches_enabled()     const { return false; }
    virtual bool transports_enabled()   const { return false; }
    // Algo only — hides map_overlay_data cells (render as SPACE) so
    // procedural bake bugs aren't masked by authored interiors.
    virtual bool algo_only_enabled()    const { return false; }
    // Collision overlay — shades solid region via obstruction pattern.
    // Game passes each visible tile to render_collision_tile when on.
    virtual bool collision_enabled()    const { return false; }
    // "Edit" checkbox — when on, the game treats left-click as a paint
    // (write tile) instead of a select (populate info overlay). The
    // paint tile itself is whatever was last clicked while edit was OFF,
    // tracked by Game.
    virtual bool editor_enabled()       const { return false; }
    // Sprites — replaces world with sprite-viewer panels. Renderer
    // owns click handling.
    virtual bool sprite_viewer_enabled() const { return false; }
    // "Health" checkbox — when on, Game draws a small horizontal bar
    // above every active primary (filled width = obj.energy / 0xff,
    // green->red gradient) so creature HP and projectile lifespan are
    // both visually inspectable.
    virtual bool health_bars_enabled() const { return false; }
    // Damage — floats damage amounts above victims and rings each
    // explosion source at its effective radius.
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

    // "Events" panel — right-side test triggers (spawn Triax, flood,
    // earthquake, etc.). Game polls consume_event_click each tick and
    // dispatches to the relevant Game::trigger_event handler.
    virtual bool events_panel_enabled() const { return false; }
    virtual bool consume_event_click(int& event_id) {
        event_id = 0;
        return false;
    }

    // Wire Game's debug_log_ into the renderer so renderer-side
    // diagnostics land in the same exile-debug.log Game writes to.
    // Default no-op; PixelRenderer stores the pointer for use by its
    // pr_debug:: helpers. Avoids two parallel ofstreams fighting for
    // the same file on Windows (where MSVC opens it deny-write).
    virtual void set_debug_log(std::ostream* /*log*/) {}

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

    // Shade a sub-tile rectangle in 1/256-tile fraction units (same as
    // Fixed8_8 fraction / AABB). Default no-op.
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
