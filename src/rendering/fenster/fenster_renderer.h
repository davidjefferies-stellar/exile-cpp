#pragma once
#include "rendering/renderer.h"
#include <cstdint>
#include <memory>

class FensterRenderer : public IRenderer {
public:
    FensterRenderer();
    ~FensterRenderer() override;

    bool init() override;
    void shutdown() override;
    void begin_frame() override;
    void end_frame() override;
    void set_viewport(uint8_t center_x, uint8_t center_y,
                      uint8_t frac_x = 0, uint8_t frac_y = 0) override;
    void render_tile(uint8_t world_x, uint8_t world_y,
                     const TileRenderInfo& info) override;
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
