#pragma once
#include <array>
#include <cstdint>
#include "rendering/sprite_data.h"

// 125 sprites (indices 0x00..0x7c), positioned directly in the BBC 128x81
// spritesheet. Coordinates and dimensions are decoded at compile-time from
// the BBC geometry tables in sprite_data.h; no external atlas file needed.

struct SpriteAtlasEntry {
    uint8_t x;              // left column in BBC sheet (0..127)
    uint8_t y;              // top row in BBC sheet    (0..80)
    uint8_t w;              // pixel width
    uint8_t h;              // pixel height
    uint8_t intrinsic_flip; // bit 0 = h-flip, bit 1 = v-flip
};

namespace detail {
    constexpr std::array<SpriteAtlasEntry, 125> make_sprite_atlas() {
        std::array<SpriteAtlasEntry, 125> out{};
        for (int i = 0; i < 125; ++i) {
            uint8_t wf = BBC_SPRITE_WIDTH_FLIP[i];
            uint8_t hf = BBC_SPRITE_HEIGHT_FLIP[i];
            uint8_t xb = BBC_SPRITE_SHEET_X[i];
            uint8_t yb = BBC_SPRITE_SHEET_Y[i];
            out[i].w = static_cast<uint8_t>((wf >> 4) + 1);
            out[i].h = static_cast<uint8_t>((hf >> 3) + 1);
            out[i].x = static_cast<uint8_t>((xb >> 4) + ((xb & 0x07) * 16));
            out[i].y = static_cast<uint8_t>((yb >> 3) + ((yb & 0x03) * 32));
            out[i].intrinsic_flip = static_cast<uint8_t>((wf & 1) | ((hf & 1) << 1));
        }
        return out;
    }
}

inline constexpr auto sprite_atlas = detail::make_sprite_atlas();
