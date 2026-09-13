// Test runner for the LIVING CITY sim core.
//
// Sorts tests by name before running so the test run itself is deterministic —
// rule R3 applies to our own tooling, not just to the simulation.

#include "TestFramework.h"

#include <algorithm>
#include <cstring>

namespace lc::test {

namespace {
const TestCase* g_current      = nullptr;
bool            g_currentFailed = false;
int             g_failureCount  = 0;
} // namespace

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

void BeginTest(const TestCase& tc) {
    g_current       = &tc;
    g_currentFailed = false;
}

bool CurrentTestFailed() { return g_currentFailed; }

void ReportFailure(const char* file, int line, const std::string& detail) {
    g_currentFailed = true;
    ++g_failureCount;
    std::fprintf(stderr, "    FAIL %s:%d\n           %s\n",
                 file, line, detail.c_str());
}

int RunAll(const char* filter) {
    std::vector<TestCase>& all = Registry();

    // Deterministic execution order regardless of link order.
    std::sort(all.begin(), all.end(), [](const TestCase& a, const TestCase& b) {
        return std::strcmp(a.name, b.name) < 0;
    });

    int run = 0, passed = 0, failed = 0, skipped = 0;

    std::printf("\n=== LIVING CITY sim core tests ===\n");
    if (!AllocTrackingEnabled()) {
        std::printf("NOTE: allocation tracking is compiled out; I5 checks will be skipped.\n");
    }
    std::printf("\n");

    for (const TestCase& tc : all) {
        if (filter && *filter && !std::strstr(tc.name, filter)) { ++skipped; continue; }

        ++run;
        std::printf("  %-52s ", tc.name);
        std::fflush(stdout);

        const int failuresBefore = g_failureCount;
        BeginTest(tc);
        tc.fn();

        if (g_failureCount == failuresBefore) {
            ++passed;
            std::printf("ok\n");
        } else {
            ++failed;
            std::printf("FAILED\n");
        }
    }

    std::printf("\n---------------------------------------------------\n");
    std::printf("  %d run, %d passed, %d failed", run, passed, failed);
    if (skipped) std::printf(", %d skipped by filter", skipped);
    std::printf("\n---------------------------------------------------\n\n");

    return failed == 0 ? 0 : 1;
}

} // namespace lc::test

int main(int argc, char** argv) {
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            filter = argv[++i];
        } else if (std::strncmp(argv[i], "--filter=", 9) == 0) {
            filter = argv[i] + 9;
        } else if (std::strcmp(argv[i], "--list") == 0) {
            for (const auto& tc : lc::test::Registry()) std::printf("%s\n", tc.name);
            return 0;
        }
    }
    return lc::test::RunAll(filter);
}
