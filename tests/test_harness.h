#pragma once
#include "game/game.h"
#include "objects/object.h"
#include "objects/object_manager.h"
#include "rendering/null_renderer.h"
#include <cstdio>
#include <cstdint>
#include <vector>

// Friend of Game (declared in game.h). Reaches through the friend
// access into Game's private state — frame counter, object manager,
// rng, renderer pointer — so fixtures can spawn entities, force
// positions, and snapshot bytes without having to add public
// accessors that would only be useful to tests.
class TestHarness {
public:
    explicit TestHarness(Game& g) : game_(g) {}

    Object& player()                   { return game_.object_mgr_.player(); }
    const Object& player() const       { return game_.object_mgr_.player(); }
    ObjectManager& objects()           { return game_.object_mgr_; }
    Random& rng()                      { return game_.rng_; }
    uint8_t frame_counter() const      { return game_.frame_counter_; }
    NullRenderer* null_renderer() {
        return dynamic_cast<NullRenderer*>(game_.renderer_.get());
    }
    void tick_n(int n) { for (int i = 0; i < n; ++i) game_.tick(); }

private:
    Game& game_;
};

// ---------------------------------------------------------------------
// Tiny assert harness. No external test framework — each TEST() emits
// a static registrar that pushes itself into TestRegistry at startup.
// tests/main.cpp walks the list, runs each, prints PASS/FAIL, and
// exits with the failure count.
// ---------------------------------------------------------------------

struct TestCase {
    const char* name;
    void (*fn)();
};

class TestRegistry {
public:
    static TestRegistry& instance() {
        static TestRegistry r;
        return r;
    }
    void add(TestCase tc) { cases_.push_back(tc); }
    const std::vector<TestCase>& cases() const { return cases_; }

private:
    std::vector<TestCase> cases_;
};

struct TestRegistrar {
    TestRegistrar(const char* name, void (*fn)()) {
        TestRegistry::instance().add({name, fn});
    }
};

// Per-test failure counter. main.cpp resets it before each test and
// reads it after to decide PASS / FAIL. Macros bump it on failure
// and print the location instead of aborting, so a single test can
// surface multiple failures in one run.
extern int g_test_failures;

#define TEST(name)                                                      \
    static void name##_impl();                                          \
    static TestRegistrar name##_reg(#name, name##_impl);                \
    static void name##_impl()

#define EXPECT_EQ(a, b) do {                                            \
    auto _va = (a); auto _vb = (b);                                     \
    if (!(_va == _vb)) {                                                \
        std::fprintf(stderr,                                            \
            "  FAIL %s:%d  %s == %s  (got %lld vs %lld)\n",             \
            __FILE__, __LINE__, #a, #b,                                 \
            (long long)_va, (long long)_vb);                            \
        ++g_test_failures;                                              \
    }                                                                   \
} while (0)

#define EXPECT_TRUE(x) do {                                             \
    if (!(x)) {                                                         \
        std::fprintf(stderr,                                            \
            "  FAIL %s:%d  expected %s\n",                              \
            __FILE__, __LINE__, #x);                                    \
        ++g_test_failures;                                              \
    }                                                                   \
} while (0)

#define EXPECT_LE(a, b) do {                                            \
    auto _va = (a); auto _vb = (b);                                     \
    if (!(_va <= _vb)) {                                                \
        std::fprintf(stderr,                                            \
            "  FAIL %s:%d  %s <= %s  (got %lld vs %lld)\n",             \
            __FILE__, __LINE__, #a, #b,                                 \
            (long long)_va, (long long)_vb);                            \
        ++g_test_failures;                                              \
    }                                                                   \
} while (0)
