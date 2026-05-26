#pragma once
#include <cstdint>
#include <cstdlib>

// Camera follows player with right-drag pan offset. Clamped to map
// extents to prevent showing wrapped tiles. pan stays int16_t so reversing
// direction after a long pan moves immediately, not after a slow crawl back.
struct Camera {
    uint8_t center_x = 0;
    uint8_t center_y = 0;
    int16_t pan_x = 0;
    int16_t pan_y = 0;
    // Hysteresis state for sub-tile fraction tracking. Holds the last
    // accepted player fraction; updated only when the player makes a
    // sustained move in one direction. Prevents the gravity / tile-
    // collision +1/-1 oscillation on a grounded player from juddering
    // the world. See pick_display_frac() below.
    uint8_t display_frac_x = 0;
    uint8_t display_frac_y = 0;
    int8_t  last_dx = 0;
    int8_t  last_dy = 0;

    // Update display_frac_* with one-frame lag hysteresis: a single ±1
    // step holds, the same direction repeated (or any |delta|>1) accepts.
    // Net effect: a steady ±1/±1 oscillation never advances; sustained
    // walking advances with one frame of lag. Wrap-aware: uint8 input is
    // treated as 256-modular signed delta.
    static int8_t wrap_delta(uint8_t cur, uint8_t prev) {
        int d = int(cur) - int(prev);
        if (d >  128) d -= 256;
        if (d < -128) d += 256;
        return static_cast<int8_t>(d);
    }
    static void apply_axis(uint8_t  cur,
                           uint8_t& display,
                           int8_t&  last_d) {
        int8_t d = wrap_delta(cur, display);
        if (std::abs(int(d)) > 1) {
            display = cur;
            last_d = d;
        } else if (d > 0 && last_d > 0) {
            display = cur;
            last_d = d;
        } else if (d < 0 && last_d < 0) {
            display = cur;
            last_d = d;
        } else if (d == 0) {
            last_d = 0;
        } else {
            // Single ±1 step against (or after) zero — hold display,
            // record direction so a second hit in the same direction
            // is accepted as sustained motion.
            last_d = d;
        }
    }
    void update_display_frac(uint8_t fx, uint8_t fy) {
        apply_axis(fx, display_frac_x, last_dx);
        apply_axis(fy, display_frac_y, last_dy);
    }

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
