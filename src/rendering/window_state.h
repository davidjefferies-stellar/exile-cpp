#pragma once
#include <cstdint>

// Window + input state held by PixelRenderer. Replaces the `struct fenster f`
// member during the sokol migration; field names mirror fenster's so the
// existing reader code in pixel_renderer.cpp / pixel_renderer_debug.cpp is
// unchanged. Populated by sokol_app event callbacks via the renderer's
// handle_event() — see task 5.
struct WinState {
    int     width     = 0;
    int     height    = 0;
    int     pending_w = 0;   // RESIZED event stashes new size here; consumer
    int     pending_h = 0;   // (apply_pending_resize) reallocs buf next frame
    // 1 = down. Indices 0..255 are ASCII for letter/symbol keys plus 9=TAB,
    // 10=ENTER, 27=ESC, 17..20=UP/DOWN/RIGHT/LEFT (fenster-compat). Indices
    // 256..264 are synthetic slots for L/R-distinct modifiers and Numpad
    // keys that need to be distinguishable from their ASCII twins —
    // 256=LCTRL, 257=RCTRL, 262=LSHIFT, 263=KP_STAR, 264=KP_MINUS. Array
    // MUST be sized to cover the full 0..264 range or get_key reads OOB.
    uint8_t keys[265] = {0};
    int     mod   = 0;       // bit 0 = ctrl, bit 1 = shift, bit 2 = alt, bit 3 = meta
    int     x     = 0;       // mouse position in window pixels
    int     y     = 0;
    int     mouse = 0;       // bit 0 = left, bit 1 = right, bit 2 = middle
    int     wheel = 0;       // accumulated scroll notches; drained each frame
};
