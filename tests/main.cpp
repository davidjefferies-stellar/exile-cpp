#include "test_harness.h"
#include "audio/audio.h"
#include <cstdio>

int g_test_failures = 0;

int main() {
    // GitHub Actions' pwsh invocation eats native exe stdout if it's
    // block-buffered; force unbuffered so each line lands in the log
    // as it's written.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    const auto& cases = TestRegistry::instance().cases();
    int total = static_cast<int>(cases.size());
    int passed = 0;
    int failed = 0;

    std::fprintf(stdout, "Running %d test(s)\n", total);

    for (const auto& tc : cases) {
        g_test_failures = 0;
        std::fprintf(stdout, "[ RUN  ] %s\n", tc.name);
        try {
            tc.fn();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  FAIL exception: %s\n", e.what());
            ++g_test_failures;
        } catch (...) {
            std::fprintf(stderr, "  FAIL unknown exception\n");
            ++g_test_failures;
        }
        if (g_test_failures == 0) {
            std::fprintf(stdout, "[ PASS ] %s\n", tc.name);
            ++passed;
        } else {
            std::fprintf(stdout, "[ FAIL ] %s (%d failures)\n",
                         tc.name, g_test_failures);
            ++failed;
        }
    }

    Audio::close();

    std::fprintf(stdout, "\n%d passed, %d failed (of %d)\n",
                 passed, failed, total);
    return failed == 0 ? 0 : 1;
}
