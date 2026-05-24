#include "test_harness.h"
#include "core/types.h"
#include "objects/object.h"
#include "objects/object_manager.h"
#include "objects/collision.h"
#include "rendering/sprite_atlas.h"
#include <array>
#include <memory>

// Regression coverage for the "door closes onto player and traps them"
// bug fixed by porting &2b80-&2b91 (the position-disengage limb of
// check_for_collisions) into Collision::push_out_of_overlap.
//
// Two layers of coverage:
//   1. Unit test on push_out_of_overlap itself — overlap geometry, push
//      direction (smallest axis), and the +/-2 velocity kick.
//   2. System test: spawn a weight-7 primary (HORIZONTAL_METAL_DOOR or
//      SWITCH) directly overlapping the player, tick the game, and
//      assert that within a couple of frames the player is no longer
//      inside the heavy object — i.e. the player update's collision
//      step actually invoked push_out_of_overlap.
//
// Without the fix, the player_motion.cpp branch reverted to the old
// position which was a no-op when the door moved into a stationary
// player. The system test would loop with the player stuck inside the
// door.

namespace {

// Compute AABB extent in 16-bit position units (same encoding the
// collision code uses: whole * 256 + fraction). Mirrors the static
// sprite_width_units / sprite_height_units helpers in collision.cpp.
int sprite_w(uint8_t sprite) {
    if (sprite > 0x80) return 0;
    return (sprite_atlas[sprite].w > 0 ? sprite_atlas[sprite].w - 1 : 0) * 16;
}
int sprite_h(uint8_t sprite) {
    if (sprite > 0x80) return 0;
    return (sprite_atlas[sprite].h > 0 ? sprite_atlas[sprite].h - 1 : 0) * 8;
}

bool overlaps(const Object& a, const Object& b) {
    int ax = a.x.whole * 256 + a.x.fraction;
    int ay = a.y.whole * 256 + a.y.fraction;
    int bx = b.x.whole * 256 + b.x.fraction;
    int by = b.y.whole * 256 + b.y.fraction;
    int aw = sprite_w(a.sprite), ah = sprite_h(a.sprite);
    int bw = sprite_w(b.sprite), bh = sprite_h(b.sprite);
    if (ax + aw <= bx || bx + bw <= ax) return false;
    if (ay + ah <= by || by + bh <= ay) return false;
    return true;
}

// Spawn a primary of the given type at the player's exact position and
// return its slot. Sets the bare minimum state needed for update_door /
// update_switch to run without exploding the slot: a non-zero energy,
// tertiary_slot=0 so the mirror write skips, and zero velocities.
// For doors, also seeds state with the door's home tile coord so the
// every-frame "obj.x.whole = obj.state" pin at &4c93 doesn't teleport
// the door away from the player on the first update.
int spawn_overlapping_primary(TestHarness& h, ObjectType type) {
    Object& p = h.player();
    int slot = h.objects().create_object(type, /*min_free_slots=*/4,
                                          p.x.whole, p.x.fraction,
                                          p.y.whole, p.y.fraction);
    if (slot <= 0) return -1;
    Object& o = h.objects().object(slot);
    o.energy        = 0xff;
    o.tertiary_slot = 0;
    o.velocity_x    = 0;
    o.velocity_y    = 0;
    // Horizontal doors (ty bit 1 = 0): state is the door's home tile x.
    // Vertical doors (ty bit 1 = 1): state is the home tile y.
    bool is_door = (type == ObjectType::HORIZONTAL_METAL_DOOR ||
                    type == ObjectType::HORIZONTAL_STONE_DOOR ||
                    type == ObjectType::VERTICAL_METAL_DOOR   ||
                    type == ObjectType::VERTICAL_STONE_DOOR);
    if (is_door) {
        bool vertical = (o.ty & 0x02) != 0;
        o.state = vertical ? p.y.whole : p.x.whole;
    }
    return slot;
}

}  // namespace

// ---- 1. push_out_of_overlap unit test ------------------------------------

TEST(push_out_of_overlap_kicks_obj_along_smallest_axis) {
    // Two same-sprite objects offset slightly in x and more in y. After
    // the helper's smallest-overlap pick the x-axis push moves obj's
    // position right (away from the leftward blocker) and adds +2 to
    // velocity_x. Position values use the BBC 16-bit encoding
    // (whole * 256 + fraction).
    Object obj{}, blocker{};
    obj.sprite     = 0x04;
    blocker.sprite = 0x04;
    obj.x          = {0x10, 0x10};  // 4112
    obj.y          = {0x10, 0x00};  // 4096
    blocker.x      = {0x10, 0x00};  // 4096 — obj overlaps to the right
    blocker.y      = {0x10, 0x00};  // 4096 — full y overlap
    obj.velocity_x = 0;
    obj.velocity_y = 0;

    int px_before = obj.x.whole * 256 + obj.x.fraction;
    int py_before = obj.y.whole * 256 + obj.y.fraction;
    bool pushed   = Collision::push_out_of_overlap(obj, blocker);
    EXPECT_TRUE(pushed);

    // Push moved obj on exactly one axis and added a +/-2 kick along
    // that same axis. Don't assert which one — the smallest-overlap pick
    // depends on the sprite's atlas width vs height.
    int px_after  = obj.x.whole * 256 + obj.x.fraction;
    int py_after  = obj.y.whole * 256 + obj.y.fraction;
    bool moved_x  = px_after != px_before;
    bool moved_y  = py_after != py_before;
    EXPECT_TRUE(moved_x || moved_y);
    EXPECT_TRUE(obj.velocity_x == 2 || obj.velocity_x == -2 ||
                obj.velocity_y == 2 || obj.velocity_y == -2);
}

TEST(push_out_of_overlap_returns_false_when_not_overlapping) {
    Object obj{}, blocker{};
    obj.sprite     = 0x04;
    blocker.sprite = 0x04;
    obj.x = {0x10, 0x00}; obj.y = {0x10, 0x00};
    blocker.x = {0x40, 0x00}; blocker.y = {0x40, 0x00};   // miles apart

    EXPECT_TRUE(!Collision::push_out_of_overlap(obj, blocker));
    // Untouched: no position or velocity changes.
    EXPECT_EQ(obj.x.whole,    0x10);
    EXPECT_EQ(obj.x.fraction, 0x00);
    EXPECT_EQ(static_cast<int>(obj.velocity_x), 0);
}

// ---- 2. System tests -----------------------------------------------------

// Walk the game forward a few frames once a HORIZONTAL_METAL_DOOR has been
// dropped on top of the player; assert the player ends up outside the
// door's AABB. Pre-fix this looped forever — the player_motion revert was
// a no-op when the heavy collider moved into a stationary player.
TEST(horizontal_door_pushes_stationary_player_out_of_overlap) {
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());

    TestHarness h(game);
    // Pin player to the canonical upper-world surface position used by
    // the npc_combat tests — known-open air, far from any baked door.
    Object& p = h.player();
    p.x = {0x9b, 0x80};
    p.y = {0x3b, 0x80};
    p.velocity_x = 0;
    p.velocity_y = 0;
    h.tick_n(30);  // let the spawn-fall settle

    int slot = spawn_overlapping_primary(h, ObjectType::HORIZONTAL_METAL_DOOR);
    EXPECT_TRUE(slot > 0);
    if (slot <= 0) return;

    Object& door = h.objects().object(slot);
    EXPECT_TRUE(overlaps(h.player(), door));

    // A handful of ticks is plenty — push_out_of_overlap fires on the very
    // first player update after the overlap exists. 5 frames is generous
    // headroom in case the door's update_door tweaks its own position too.
    h.tick_n(5);

    EXPECT_TRUE(!overlaps(h.player(), door));
}

// Switches share the weight-7 static profile with doors, so the same push
// regression would let a switch trap the player. Also lets the switch's
// own touch-trigger mechanic surface (update_switch's tx press history
// shifts in the trigger bit every frame the player is touching).
TEST(switch_pushes_stationary_player_out_of_overlap) {
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());

    TestHarness h(game);
    Object& p = h.player();
    p.x = {0x9b, 0x80};
    p.y = {0x3b, 0x80};
    p.velocity_x = 0;
    p.velocity_y = 0;
    h.tick_n(30);

    int slot = spawn_overlapping_primary(h, ObjectType::SWITCH);
    EXPECT_TRUE(slot > 0);
    if (slot <= 0) return;

    Object& sw = h.objects().object(slot);
    EXPECT_TRUE(overlaps(h.player(), sw));

    h.tick_n(5);

    EXPECT_TRUE(!overlaps(h.player(), sw));
}

// Once the switch has detected a player touch its tx press-history byte
// shifts a 1 in at bit 7 per frame. Crossing the &80 leading-edge value
// is what update_switch (&499d) uses to fire process_switch_effects.
// Force-stamps switch.touching with the player slot to bypass the
// frame-1 collision/push race (which separates the AABBs before step 10
// runs) and asserts the press fires.
TEST(switch_press_fires_when_touching_player) {
    Game game(std::make_unique<NullRenderer>());
    EXPECT_TRUE(game.init());

    TestHarness h(game);
    Object& p = h.player();
    p.x = {0x9b, 0x80};
    p.y = {0x3b, 0x80};
    p.velocity_x = 0;
    p.velocity_y = 0;
    h.tick_n(30);

    int slot = spawn_overlapping_primary(h, ObjectType::SWITCH);
    EXPECT_TRUE(slot > 0);
    if (slot <= 0) return;

    Object& sw = h.objects().object(slot);
    sw.tx                   = 0x00;  // empty press history
    sw.touching             = 0x00;  // stamp player slot - bypass push race
    sw.tertiary_data_offset = 0x00;  // known starting data byte

    // update_switch shifts a 1 into tx bit 7 on the touching frame, then
    // toggles tertiary_data_offset bit 0 because tx == 0x80 exactly.
    h.tick_n(1);

    // Either the press history advanced or the toggle already fired —
    // both prove the touch reached update_switch.
    EXPECT_TRUE((sw.tx & 0x80) != 0 ||
                (sw.tertiary_data_offset & 0x01) != 0);
}
