#pragma once
#include "rendering/renderer.h"
#include <curses.h>

class AsciiRenderer : public IRenderer {
public:
    AsciiRenderer() = default;
    ~AsciiRenderer() override;

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

    void render_hud(const PlayerState& player) override;

    int viewport_width_tiles() const override;
    int viewport_height_tiles() const override;

    int get_key() override;

private:
    bool initialized_ = false;
    int term_width_ = 80;
    int term_height_ = 24;
    uint8_t vp_center_x_ = 0;
    uint8_t vp_center_y_ = 0;

    // Convert tile type to ASCII character
    static char tile_to_char(uint8_t tile_type, bool flip_h, bool flip_v);

    // Convert tile type to color pair index
    static int tile_to_color(uint8_t tile_type);

    // Convert object type to ASCII character
    static char object_to_char(ObjectType type);

    // Convert object type to color pair index
    static int object_to_color(ObjectType type);

    // Screen coordinate conversion
    bool world_to_screen(uint8_t wx, uint8_t wy, int& sx, int& sy) const;

    void init_color_pairs();
};
