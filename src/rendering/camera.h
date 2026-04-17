#pragma once
#include <cstdint>

// Camera tracks the viewport center, following the player.
// A manual pan offset (set by right-drag) is preserved across frames.
struct Camera {
    uint8_t center_x = 0;
    uint8_t center_y = 0;
    int16_t pan_x = 0;
    int16_t pan_y = 0;

    void follow_player(uint8_t player_x, uint8_t player_y) {
        center_x = static_cast<uint8_t>(player_x + pan_x);
        center_y = static_cast<uint8_t>(player_y + pan_y);
    }

    void apply_pan(int dx, int dy) {
        pan_x = static_cast<int16_t>(pan_x + dx);
        pan_y = static_cast<int16_t>(pan_y + dy);
    }

    void reset_pan() { pan_x = 0; pan_y = 0; }
};
