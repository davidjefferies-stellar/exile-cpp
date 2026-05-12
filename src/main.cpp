#include "game/game.h"
#include "audio/audio.h"
#include "rendering/pixel_renderer.h"
#include <memory>
#include <cstdio>

#ifdef _WIN32
// Request 1 ms scheduler resolution so std::this_thread::sleep_for in
// Game::run can actually wake up close to its requested 20 ms budget.
// Default Windows granularity is 15.625 ms, which floors the loop at
// ~32 fps (sleep_for(19 ms) rounds up to 31.25 ms = 2 scheduler ticks).
// winmm.lib is already linked via the audio backend.
#include <timeapi.h>
#endif

int main() {
    auto renderer = std::make_unique<PixelRenderer>();
    Game game(std::move(renderer));

    if (!game.init()) {
        std::fprintf(stderr, "Failed to initialize game\n");
        return 1;
    }

#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    game.run();
#ifdef _WIN32
    timeEndPeriod(1);
#endif

    Audio::close();
    return 0;
}
