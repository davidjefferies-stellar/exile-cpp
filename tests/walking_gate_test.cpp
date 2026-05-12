#include "test_harness.h"
#include "core/types.h"
#include "objects/object.h"
#include <memory>

// Pinned to commit b56ab6e — "Faithful port of 6502 walking gate via
// state low-nibble counter". Verifies the exact mechanism documented in
// player_motion.cpp:502-516 (port of &3a4c-&3a8a update_walking_state):
//
//   counter = player.state & 0x0f
//   if SUPPORTED:        counter = 0
//   else if counter<0x0f counter++
//
// If a future change reverts to the old binary SUPPORTED-driven walking
// decision, walking_gate_caps_at_0x0f will fail loudly.

namespace {
constexpr uint8_t kCounterMask = 0x0f;
}

TEST(walking_gate_zero_when_supported) {
    // Tick long enough for the default-spawn player to fall to the
    // ground and SUPPORTED to latch. At 0x0f-cap the counter would
    // otherwise climb during the fall; we want to confirm it clamps
    // back to 0 once the player is grounded (the if-branch in
    // player_motion.cpp:511). 60 frames ≈ 1.2 s in-game — plenty.
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());

    TestHarness h(game);
    h.tick_n(60);
    EXPECT_TRUE(h.player().is_supported());
    EXPECT_EQ(static_cast<int>(h.player().state & kCounterMask), 0);
}

TEST(walking_gate_increments_and_caps_in_air) {
    // Force the player into clear sky so no bottom collision can fire.
    // The counter should tick up by 1 each frame (else-if branch in
    // player_motion.cpp:513) and cap at 0x0f instead of wrapping.
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());

    TestHarness h(game);
    Object& p = h.player(); 

    // Reset to a controlled airborne state. y=0x10 sits in the upper
    // sky band; clearing velocity prevents the very first tick from
    // covering enough ground to hit anything.
    p.x.whole = 0x80; p.x.fraction = 0x00;
    p.y.whole = 0x10; p.y.fraction = 0x00;
    p.velocity_x = 0;
    p.velocity_y = 0;
    p.flags = 0;
    p.state = 0;

    // First tick: SUPPORTED clears, counter goes 0 -> 1.
    h.tick_n(1);
    EXPECT_EQ(static_cast<int>(h.player().state & kCounterMask), 1);

    // After 14 more frames the counter has reached 0x0f.
    h.tick_n(14);
    EXPECT_EQ(static_cast<int>(h.player().state & kCounterMask), 0x0f);

    // 5 more frames — still capped (no wrap to 0).
    h.tick_n(5);
    EXPECT_EQ(static_cast<int>(h.player().state & kCounterMask), 0x0f);
}
