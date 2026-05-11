#include "test_harness.h"
#include "core/random.h"

// Exercises the &2587 PRNG port. Reproducibility of a seeded sequence is
// the load-bearing property: save/load relies on Random::state(i) round-
// tripping, and the audio / spawn / mood code expects identical play-
// throughs with identical seeds.

TEST(random_default_state_is_zero) {
    Random r;
    EXPECT_EQ(r.state(0), 0);
    EXPECT_EQ(r.state(1), 0);
    EXPECT_EQ(r.state(2), 0);
    EXPECT_EQ(r.state(3), 0);
}

TEST(random_seed_round_trips) {
    Random r;
    r.seed(0x12, 0x34, 0x56, 0x78);
    EXPECT_EQ(r.state(0), 0x12);
    EXPECT_EQ(r.state(1), 0x34);
    EXPECT_EQ(r.state(2), 0x56);
    EXPECT_EQ(r.state(3), 0x78);
}

TEST(random_seeded_sequence_is_deterministic) {
    // Two generators with the same seed must produce the same byte
    // stream — the 6502's ADC-through-state chain is fully determined
    // by the four state bytes and the running carry.
    Random a;
    Random b;
    a.seed(0xa1, 0xb2, 0xc3, 0xd4);
    b.seed(0xa1, 0xb2, 0xc3, 0xd4);

    for (int i = 0; i < 64; i++) {
        uint8_t va = a.next();
        uint8_t vb = b.next();
        EXPECT_EQ(va, vb);
    }
}

TEST(random_next_advances_state) {
    // First call from a fresh-zero state must produce something — and
    // must mutate state, otherwise every subsequent call is stuck.
    // (next() from all-zero state with carry=0 returns 0 on the first
    // call but the carry-driven ADC chain still bumps state[3].)
    Random r;
    r.seed(0x01, 0x00, 0x00, 0x00);
    uint8_t prev = r.next();
    bool any_different = false;
    for (int i = 0; i < 8; i++) {
        uint8_t v = r.next();
        if (v != prev) { any_different = true; break; }
        prev = v;
    }
    EXPECT_TRUE(any_different);
}

TEST(random_next_masked_respects_mask) {
    Random r;
    r.seed(0x55, 0xaa, 0x33, 0xcc);
    for (int i = 0; i < 32; i++) {
        uint8_t v = r.next_masked(0x0f);
        EXPECT_LE(static_cast<int>(v), 0x0f);
    }
}
