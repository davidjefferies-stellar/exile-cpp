#pragma once
#include <cstdint>

// Camera tracks the viewport center, following the player.
// A manual pan offset (set by right-drag) is preserved across frames.
//
// The world is stored as a 256×256 tile grid and the gameplay code relies
// on uint8_t wrap for modular arithmetic, but the *camera* is clamped to
// the map extents so map-mode panning can't drift off the edge and show
// wrapped-around territory. We still store pan as int16_t so large-scale
// panning accumulates even when the clamp keeps the view on the edge —
// that way reversing direction starts moving immediately, rather than
// after the pan counter crawls back from a huge value.
struct Camera {
    uint8_t center_x = 0;
    uint8_t center_y = 0;
    int16_t pan_x = 0;
    int16_t pan_y = 0;

    // vp_w_half / vp_h_half are the half-viewport sizes in tiles. Passing
    // them lets the clamp keep the *entire viewport* inside [0, 255] so
    // you never see wrap-around tiles at the edge of the map. Renderers
    // that don't care about wrap can pass 0 to fall back to a plain
    // centre-in-[0,255] clamp.
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
