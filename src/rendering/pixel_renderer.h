#pragma once
#include "rendering/renderer.h"
#include <cstdint>
#include <memory>

class PixelRenderer : public IRenderer {
public:
    PixelRenderer();
    ~PixelRenderer() override;

    bool init() override;
    void shutdown() override;
    void begin_frame() override;
    void end_frame() override;
    void set_viewport(uint8_t center_x, uint8_t center_y,
                      uint8_t frac_x = 0, uint8_t frac_y = 0) override;
    void render_tile(uint8_t world_x, uint8_t world_y,
                     const TileRenderInfo& info) override;
    void render_water_column(uint8_t world_x,
                             uint8_t waterline_y) override;
    void render_object(Fixed8_8 world_x, Fixed8_8 world_y,
                       const SpriteRenderInfo& info) override;
    void render_particle(uint8_t wx, uint8_t wx_frac,
                         uint8_t wy, uint8_t wy_frac,
                         uint8_t colour) override;
    void render_hud(const PlayerState& player) override;
    int viewport_width_tiles() const override;
    int viewport_height_tiles() const override;
    int get_key() override;
    bool consume_pan_tiles(int& dx, int& dy) override;
    bool consume_left_click(int& tile_dx, int& tile_dy) override;
    void set_overlay_text(const char* text) override;
    void set_highlighted_tile(uint8_t world_x, uint8_t world_y) override;
    void render_debug_marker(uint8_t world_x, uint8_t world_y,
                             uint32_t rgb, const char* label) override;
    void render_activation_overlay(uint8_t anchor_x, uint8_t anchor_y) override;
    bool aabb_overlay_enabled() const override;
    void render_aabb(Fixed8_8 world_x, Fixed8_8 world_y,
                     int w_units, int h_units, uint32_t rgb) override;
    bool tile_grid_enabled()    const override;
    bool object_tiers_enabled() const override;
    bool map_mode_enabled()     const override;
    bool switches_enabled()     const override;
    bool transports_enabled()   const override;
    void render_wire(uint8_t x1, uint8_t y1,
                     uint8_t x2, uint8_t y2, uint32_t rgb) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
