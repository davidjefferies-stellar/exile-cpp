#include "test_harness.h"
#include "objects/object.h"
#include <memory>

// Headless boot smoke: proves Game instantiates, init() succeeds with the
// NullRenderer, and tick() advances frame_counter by exactly one. If this
// fails everything else is moot — most other test files start by booting a
// Game, so a regression here is upstream of half the suite.

TEST(boot_headless_init_succeeds) {
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());
}

TEST(boot_tick_advances_frame_counter) {
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());

    TestHarness h(game);
    EXPECT_EQ(h.frame_counter(), 0);
    h.tick_n(10);
    EXPECT_EQ(h.frame_counter(), 10);
}

TEST(boot_player_starts_with_energy) {
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());

    TestHarness h(game);
    h.tick_n(1);
    EXPECT_TRUE(h.player().energy > 0);
}
