#include "game/game.h"
#include "rendering/fenster/fenster_renderer.h"
#include <memory>
#include <cstdio>

int main() {
    auto renderer = std::make_unique<FensterRenderer>();
    Game game(std::move(renderer));

    if (!game.init()) {
        std::fprintf(stderr, "Failed to initialize game\n");
        return 1;
    }

    game.run();
    return 0;
}
