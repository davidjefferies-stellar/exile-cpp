#include "test_harness.h"
#include "core/fixed_point.h"

// Fixed8_8::add_velocity is on the physics hot path — every primary's
// per-frame x and y integration runs through it. Bugs here cascade
// silently into "objects drift a sub-pixel per frame" or "negative
// velocity bumps whole the wrong way at the fraction boundary".
// 6502 reference: &2a36-&2a47.

TEST(fixed_point_add_zero_is_noop) {
    Fixed8_8 p(0x10, 0x80);
    p.add_velocity(0);
    EXPECT_EQ(p.whole, 0x10);
    EXPECT_EQ(p.fraction, 0x80);
}

TEST(fixed_point_positive_velocity_within_fraction) {
    Fixed8_8 p(0x40, 0x10);
    p.add_velocity(0x20);
    EXPECT_EQ(p.whole, 0x40);
    EXPECT_EQ(p.fraction, 0x30);
}

TEST(fixed_point_positive_velocity_carries_into_whole) {
    // fraction overflows from 0xf0 + 0x20 = 0x110 → carry bumps whole.
    Fixed8_8 p(0x40, 0xf0);
    p.add_velocity(0x20);
    EXPECT_EQ(p.whole, 0x41);
    EXPECT_EQ(p.fraction, 0x10);
}

TEST(fixed_point_small_negative_velocity_stays_within_whole) {
    // -0x10 from fraction 0x80 doesn't cross a tile boundary. The 6502's
    // pessimistic pre-DEC is cancelled by the post-INC (the unsigned
    // ADC carries because 0x80 + 0xf0 > 0xff). Net effect: whole
    // unchanged, fraction drops by 0x10 to 0x70.
    Fixed8_8 p(0x40, 0x80);
    p.add_velocity(static_cast<int8_t>(-0x10));
    EXPECT_EQ(p.whole, 0x40);
    EXPECT_EQ(p.fraction, 0x70);
}

TEST(fixed_point_negative_velocity_borrows_when_underflow) {
    // -0x80 from fraction 0x10 does cross a boundary. ADC of 0x10+0x80
    // = 0x90 doesn't carry, so the pre-DEC sticks — whole drops by 1.
    Fixed8_8 p(0x40, 0x10);
    p.add_velocity(static_cast<int8_t>(-0x80));
    EXPECT_EQ(p.whole, 0x3f);
    EXPECT_EQ(p.fraction, 0x90);
}

TEST(clamp_velocity_within_range_is_pass_through) {
    EXPECT_EQ(static_cast<int>(clamp_velocity(0)),     0);
    EXPECT_EQ(static_cast<int>(clamp_velocity(0x40)),  0x40);
    EXPECT_EQ(static_cast<int>(clamp_velocity(-0x40)), -0x40);
}

TEST(clamp_velocity_saturates_at_engine_caps) {
    EXPECT_EQ(static_cast<int>(clamp_velocity(0x7f)),    0x40);
    EXPECT_EQ(static_cast<int>(clamp_velocity(-0x7f)),  -0x40);
    EXPECT_EQ(static_cast<int>(clamp_velocity(10000)),   0x40);
    EXPECT_EQ(static_cast<int>(clamp_velocity(-10000)), -0x40);
}
