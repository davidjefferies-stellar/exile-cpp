#include "test_harness.h"
#include "core/types.h"
#include "objects/object.h"
#include "objects/object_manager.h"
#include <memory>

// Exercises NPC combat behaviour for the two boss-ish enemy types,
// MAGENTA_CLAWED_ROBOT and TRIAX. Both share the 6502 &4861
// consider_firing_at_player_and_move_triax / move_hovering_npc /
// thrust_towards_target chain, so the invariants tested here apply
// to both:
//
//   - Gravity is cancelled each frame (DEC acceleration_y at &4883)
//   - Fire rate scales with energy (&276c-&2773 gate)
//   - Fired bullets get a non-zero lifespan timer (else they
//     self-destruct on tick 1 via common_bullet_update's
//     `if (timer == 0) { energy = 0; }` branch)
//   - Bullet velocity aims toward the player (compute_firing_vector
//     centre-to-centre math, not the legacy fixed-diagonal hack)
//   - Triax eventually teleports away on its own (&474f-&4752 always
//     1-in-256 random teleport regardless of state)

namespace {

int find_primary_of_type(ObjectManager& mgr, ObjectType type) {
    for (int s = 1; s < GameConstants::PRIMARY_OBJECT_SLOTS; s++) {
        if (mgr.object(s).type == type && mgr.object(s).is_active())
            return s;
    }
    return -1;
}

// Spawn the named type 3 tiles east of the player and zero out
// velocities so the test starts from a clean slate. Returns the
// primary slot or -1 on failure.
int spawn_npc_near_player(Game& game, ObjectType type) {
    TestHarness h(game);
    h.tick_n(30);
    Object& p = h.player();
    int slot = h.objects().create_object(
        type, /*min_free_slots=*/4,
        static_cast<uint8_t>(p.x.whole + 3), 0x80,
        p.y.whole, 0x80);
    if (slot > 0) {
        Object& n = h.objects().object(slot);
        n.target_and_flags = 0xc0;   // DIRECTNESS_THREE — can-see-player
        n.velocity_x = 0;
        n.velocity_y = 0;
    }
    return slot;
}

}  // namespace

TEST(triax_hovers_without_falling) {
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());
    int slot = spawn_npc_near_player(game, ObjectType::TRIAX);
    EXPECT_TRUE(slot > 0);
    if (slot <= 0) return;

    TestHarness h(game);
    Object& triax = h.objects().object(slot);
    triax.energy = 0xff;                 // max energy → max gate, full update
    uint8_t y0 = triax.y.whole;
    uint8_t yf0 = triax.y.fraction;

    // Run a short burst — without cancel_gravity the +1/frame gravity
    // would accelerate Triax downward by ~0x14 in 20 frames, dropping
    // his y_whole by at least one tile.
    h.tick_n(20);
    if (!triax.is_active()) return;       // teleported away — accept

    int drift_y = (int(triax.y.whole) * 256 + int(triax.y.fraction)) -
                  (int(y0)            * 256 + int(yf0));
    int abs_drift = drift_y < 0 ? -drift_y : drift_y;
    EXPECT_LE(abs_drift, 0x80);           // less than half a tile
}

TEST(clawed_robot_hovers_without_falling) {
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());
    int slot = spawn_npc_near_player(game, ObjectType::MAGENTA_CLAWED_ROBOT);
    EXPECT_TRUE(slot > 0);
    if (slot <= 0) return;

    TestHarness h(game);
    Object& bot = h.objects().object(slot);
    bot.energy = 0xff;
    uint8_t y0 = bot.y.whole;
    uint8_t yf0 = bot.y.fraction;

    h.tick_n(20);
    if (!bot.is_active()) return;

    int drift_y = (int(bot.y.whole) * 256 + int(bot.y.fraction)) -
                  (int(y0)          * 256 + int(yf0));
    int abs_drift = drift_y < 0 ? -drift_y : drift_y;
    EXPECT_LE(abs_drift, 0x80);
}

TEST(triax_fires_bullet_with_lifespan) {
    // Triax fires ICER_BULLETs (and occasionally ACTIVE_GRENADE). The
    // bullet must have timer > 0 — common_bullet_update zeroes its
    // energy on the first tick with timer == 0.
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());
    int slot = spawn_npc_near_player(game, ObjectType::TRIAX);
    EXPECT_TRUE(slot > 0);
    if (slot <= 0) return;

    TestHarness h(game);
    h.objects().object(slot).energy = 0xff;

    // Tick up to 200 frames waiting for the fire RNG to roll under
    // 0x21 (~13%/frame at max energy). Probability of zero shots in
    // 200 frames is (0.87)^200 ≈ 10^-12.
    int bullet_slot = -1;
    for (int i = 0; i < 200 && bullet_slot < 0; i++) {
        h.tick_n(1);
        if (!h.objects().object(slot).is_active()) return;  // teleported
        bullet_slot = find_primary_of_type(h.objects(),
                                           ObjectType::ICER_BULLET);
        if (bullet_slot < 0) {
            bullet_slot = find_primary_of_type(h.objects(),
                                                ObjectType::ACTIVE_GRENADE);
        }
    }
    EXPECT_TRUE(bullet_slot > 0);
    if (bullet_slot <= 0) return;

    EXPECT_TRUE(h.objects().object(bullet_slot).timer > 0);
}

TEST(clawed_robot_bullet_aims_toward_player) {
    // Set the clawed robot to the east of the player and force can-see.
    // The bullet's velocity_x should be NEGATIVE (firing west toward
    // the player). The pre-fix port used a fixed ±0x20 sign-of-dx
    // which was right, but the magnitude was hardcoded — now velocity
    // comes from compute_firing_vector (centre-to-centre with leading).
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());
    int slot = spawn_npc_near_player(game,
                                     ObjectType::MAGENTA_CLAWED_ROBOT);
    EXPECT_TRUE(slot > 0);
    if (slot <= 0) return;

    TestHarness h(game);
    h.objects().object(slot).energy = 0xff;

    int bullet_slot = -1;
    for (int i = 0; i < 200 && bullet_slot < 0; i++) {
        h.tick_n(1);
        if (!h.objects().object(slot).is_active()) return;
        bullet_slot = find_primary_of_type(h.objects(),
                                            ObjectType::ICER_BULLET);
    }
    EXPECT_TRUE(bullet_slot > 0);
    if (bullet_slot <= 0) return;

    // Robot is to the east of player → bullet should head west (vx < 0).
    EXPECT_TRUE(h.objects().object(bullet_slot).velocity_x < 0);
}

TEST(triax_teleports_away_eventually) {
    // The always-1-in-256 random teleport at &474f-&4752 alone gives
    // an expected lifetime of ~256 frames. Run 2000 frames — the
    // probability that Triax is still on the world after 2000 random
    // rolls is (255/256)^2000 ≈ 0.04%; combined with the other escape
    // paths it's essentially zero.
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());
    int slot = spawn_npc_near_player(game, ObjectType::TRIAX);
    EXPECT_TRUE(slot > 0);
    if (slot <= 0) return;

    TestHarness h(game);
    h.objects().object(slot).energy = 0xff;
    h.tick_n(2000);

    Object& triax = h.objects().object(slot);
    bool gone = !triax.is_active() ||
                (triax.flags & ObjectFlags::TELEPORTING) != 0 ||
                triax.ty == 0;
    EXPECT_TRUE(gone);
}
