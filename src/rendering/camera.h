#pragma once
#include <cstdint>

// Camera follows player with right-drag pan offset. Clamped to map
// extents to prevent showing wrapped tiles. pan stays int16_t so reversing
// direction after a long pan moves immediately, not after a slow crawl back.
struct Camera {
    uint8_t center_x = 0;
    uint8_t center_y = 0;
    int16_t pan_x = 0;
    int16_t pan_y = 0;

    // vp_w_half/vp_h_half keep the entire viewport in [0,255] (no wrap
    // at map edge). Pass 0 to fall back to plain centre clamp.
    void follow_player(uint8_t player_x, uint8_t player_y,
                       int vp_w_half = 0, int vp_h_half = 0) {
        int cx = int(player_x) + int(pan_x);
        int cy = int(player_y) + int(pan_y);
        int min_x = vp_w_half;
        int max_x = 255 - vp_w_half;
        int min_y = vp_h_half;
        int max_y = 255 - vp_h_half;
        if (min_x > max_x) { min_x = 0; max_x = 255; } // viewport > map
        if (min_y > max_y) { min_y = 0; max_y = 255; }
        if (cx < min_x) cx = min_x;
        if (cx > max_x) cx = max_x;
        if (cy < min_y) cy = min_y;
        if (cy > max_y) cy = max_y;
        center_x = static_cast<uint8_t>(cx);
        center_y = static_cast<uint8_t>(cy);
    }

    void apply_pan(int dx, int dy) {
        pan_x = static_cast<int16_t>(pan_x + dx);
        pan_y = static_cast<int16_t>(pan_y + dy);
    }

    void reset_pan() { pan_x = 0; pan_y = 0; }
};
