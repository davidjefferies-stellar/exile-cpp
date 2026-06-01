#include "test_harness.h"
#include "core/types.h"
#include "objects/object.h"
#include "objects/object_manager.h"
#include "objects/weapon.h"
#include <memory>

// Exercises Weapon::fire for each of the three projectile-spawning
// weapons (pistol, icer, plasma), plus the blaster which discharges via
// the timer instead of spawning a primary. For each spawning weapon:
//
//   1. Fire it from a cleanly-positioned player
//   2. Confirm the bullet exists, has the right type, and a non-zero
//      velocity along the aim axis (== a trajectory)
//   3. Tick a few frames — confirm the bullet's position actually
//      changed (the trajectory is integrating)
//   4. Mark the bullet as having hit a tile and tick once more —
//      confirm it mutated into an EXPLOSION (energy=0 path in step 12)
//
// Tests are independent: each one constructs a fresh Game/init pair.
// The 6502 reference is &2d33 (fire_weapon) and &441b / &46bf / &4a88
// (the bullet update routines that consume tile_collision).

namespace {

// Stand the player on the upper-world surface and face them right.
// Pin position to the BBC default spawn (&9b, &3b) so the test is
// independent of exile.ini's debug start_x/start_y — CI ships an ini
// with start in the lower-world water area, which lands the bullet
// next to tile geometry it touches/blocks before we can assert.
// Tick a generous 60 frames after pin so the spawn fall + landing
// resolve before we start poking velocities.
void prepare_grounded_player(Game& game) {
    TestHarness h(game);
    Object& p = h.player();
    p.x.whole = 0x9b;
    p.x.fraction = 0;
    p.y.whole = 0x3b;
    p.y.fraction = 0;
    p.velocity_x = 0;
    p.velocity_y = 0;
    h.tick_n(60);
    p.velocity_x = 0;
    p.velocity_y = 0;
    p.flags &= ~ObjectFlags::FLIP_HORIZONTAL;  // facing right
}

// Find the most recently created primary of the given type. Returns -1
// if no slot matches. Walks the full active range; firing always picks
// the lowest free slot so the bullet is usually near the start, but
// boot-time spawns may occupy lower slots.
int find_primary_of_type(ObjectManager& mgr, ObjectType type) {
    for (int s = 1; s < GameConstants::PRIMARY_OBJECT_SLOTS; s++) {
        if (mgr.object(s).type == type && mgr.object(s).is_active())
            return s;
    }
    return -1;
}

// Shared body — fires the named weapon, then runs the four assertions
// described above. Lets each TEST() macro be short and readable.
void exercise_projectile_weapon(uint8_t weapon_type,
                                ObjectType expected_bullet) {
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());
    prepare_grounded_player(game);

    TestHarness h(game);
    ObjectManager& mgr = h.objects();
    Object& player = h.player();

    // Stock the weapon with plenty of energy so the cost check at &2d3e
    // doesn't gate us out. weapon_energy_cost ranges up to 0xff for the
    // blaster — 0x1000 covers every variant comfortably.
    uint16_t energy = 0x1000;
    int8_t   blaster_timer = 0;

    // aim_angle = 0 means "straight forward along facing axis" — for
    // right-facing player, this gives velocity_x > 0, velocity_y == 0.
    int slot = Weapon::fire(mgr, player, weapon_type, /*aim_angle=*/0,
                            energy, blaster_timer, h.rng());
    EXPECT_TRUE(slot > 0);
    if (slot <= 0) return;

    Object& bullet = mgr.object(slot);
    EXPECT_EQ(static_cast<int>(bullet.type),
              static_cast<int>(expected_bullet));

    // 2. Trajectory: velocity_x must be positive (we aimed right) and
    //    the magnitude should be the 0x40+jitter that fire() applies.
    EXPECT_TRUE(bullet.velocity_x > 0);
    EXPECT_TRUE(bullet.velocity_x >= 0x40);

    // 3. Motion: tick a handful of frames and confirm the bullet's x
    //    actually changed. We can't predict the exact destination
    //    (gravity, drag, collisions all interact) but a moving bullet
    //    must shift at least one fraction unit.
    int start_abs_x = int(bullet.x.whole) * 256 + int(bullet.x.fraction);
    h.tick_n(3);
    // Bullet may have exploded mid-flight if it spawned next to a tile.
    // Only validate motion if it's still alive as its original type.
    if (bullet.type == expected_bullet) {
        int after_abs_x = int(bullet.x.whole) * 256 + int(bullet.x.fraction);
        EXPECT_TRUE(after_abs_x != start_abs_x);
    }

    // 4. Impact: force the bullet's terminal condition and confirm it
    //    mutates to EXPLOSION within a couple of ticks. The trigger
    //    differs by type:
    //      - PISTOL_BULLET / ICER_BULLET share common_bullet_update.
    //        &443d takes the BCS explode_bullet path only when timer
    //        (post-decrement) >= 0x3e, so bump timer to 0x3f before
    //        stamping tile_collision; otherwise the bullet ricochets
    //        (timer -= 0x15) instead of exploding.
    //      - PLASMA_BALL (&4a88) doesn't consume tile_collision at all
    //        — its terminal path is the per-frame energy decrement
    //        running out, so force energy directly. Object-touch would
    //        mutate it to FIREBALL instead, which is plasma's special
    //        non-explosive impact.
    //    Either path lands in object_update.cpp step 12 (energy=0 ->
    //    explode_object_with_duration), which is what we assert.
    if (bullet.type == expected_bullet) {
        if (expected_bullet == ObjectType::PLASMA_BALL) {
            bullet.energy = 0;
        } else {
            bullet.timer = 0x3f;
            bullet.tile_collision = true;
        }
        h.tick_n(2);
        EXPECT_EQ(static_cast<int>(bullet.type),
                  static_cast<int>(ObjectType::EXPLOSION));
    }
}

}  // namespace

TEST(weapons_pistol_fires_moves_and_explodes) {
    exercise_projectile_weapon(/*weapon_type=*/1, ObjectType::PISTOL_BULLET);
}

TEST(weapons_icer_fires_moves_and_explodes) {
    exercise_projectile_weapon(/*weapon_type=*/2, ObjectType::ICER_BULLET);
}

TEST(weapons_plasma_fires_moves_and_explodes) {
    exercise_projectile_weapon(/*weapon_type=*/4, ObjectType::PLASMA_BALL);
}

TEST(weapons_blaster_arms_timer_without_spawning_bullet) {
    // Blaster takes a different path (&2d4a-&2d51): no primary spawned,
    // just blaster_timer = -5 and the full 0xff energy cost paid. The
    // test exe doesn't run Game's tick_blaster code that converts the
    // timer into a per-frame discharge radius, but we can still verify
    // the arming side effects.
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());
    prepare_grounded_player(game);

    TestHarness h(game);
    uint16_t energy = 0x1000;
    int8_t   blaster_timer = 0;

    int slot = Weapon::fire(h.objects(), h.player(), /*weapon_type=*/3,
                            /*aim_angle=*/0, energy, blaster_timer,
                            h.rng());
    EXPECT_EQ(slot, -1);            // no bullet primary spawned
    EXPECT_EQ(static_cast<int>(blaster_timer), -5);
    EXPECT_EQ(static_cast<int>(energy), 0x1000 - 0xff);
}

TEST(weapons_low_energy_refuses_to_fire) {
    // Cost gate: weapon_energy_cost[icer] = 0x10. With energy < cost the
    // fire() call must return -1 without spawning anything and without
    // touching `energy`. Guards against an off-by-one in the threshold
    // check at &2d3e.
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());
    prepare_grounded_player(game);

    TestHarness h(game);
    uint16_t energy = 0x05;
    int8_t   blaster_timer = 0;
    int slot = Weapon::fire(h.objects(), h.player(), /*weapon_type=*/2,
                            /*aim_angle=*/0, energy, blaster_timer,
                            h.rng());
    EXPECT_EQ(slot, -1);
    EXPECT_EQ(static_cast<int>(energy), 0x05);
    EXPECT_EQ(static_cast<int>(blaster_timer), 0);
}
