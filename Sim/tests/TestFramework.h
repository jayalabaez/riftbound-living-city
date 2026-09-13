// Minimal test harness for the LIVING CITY sim core.
//
// Deliberately tiny and dependency-free: the sim core must build and test with nothing
// but a C++20 compiler (R1). No gtest, no Catch2, no package manager.
//
// Usage:
//     LC_TEST(money_conserves_across_transfers) {
//         LC_CHECK_EQ(a + b, total);
//     }
#pragma once

#include "livingcity/core/Core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lc::test {

struct TestCase {
    const char* name;
    const char* file;
    int         line;
    void      (*fn)();
};

// Registry is a function-local static so registration order across translation units
// is irrelevant to correctness; the runner sorts by name before executing so the test
// run itself is deterministic (R3 applies to our own tooling too).
std::vector<TestCase>& Registry();

struct Registrar {
    Registrar(const char* name, const char* file, int line, void (*fn)()) {
        Registry().push_back(TestCase{name, file, line, fn});
    }
};

// Failure accounting for the currently running test.
void        BeginTest(const TestCase& tc);
void        ReportFailure(const char* file, int line, const std::string& detail);
bool        CurrentTestFailed();
int         RunAll(const char* filter);

} // namespace lc::test

#define LC_TEST(NAME)                                                                  \
    static void lc_test_##NAME();                                                      \
    static ::lc::test::Registrar lc_test_reg_##NAME(#NAME, __FILE__, __LINE__,         \
                                                    &lc_test_##NAME);                  \
    static void lc_test_##NAME()

#define LC_CHECK(expr)                                                                 \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            ::lc::test::ReportFailure(__FILE__, __LINE__,                              \
                std::string("expected true: ") + #expr);                               \
        }                                                                              \
    } while (false)

#define LC_CHECK_FALSE(expr)                                                           \
    do {                                                                               \
        if ((expr)) {                                                                  \
            ::lc::test::ReportFailure(__FILE__, __LINE__,                              \
                std::string("expected false: ") + #expr);                              \
        }                                                                              \
    } while (false)

#define LC_CHECK_EQ(a, b)                                                              \
    do {                                                                               \
        auto lc_a_ = (a);                                                              \
        auto lc_b_ = (b);                                                              \
        if (!(lc_a_ == lc_b_)) {                                                       \
            ::lc::test::ReportFailure(__FILE__, __LINE__,                              \
                std::string(#a) + " == " + #b + "  |  got "                            \
                + ::lc::test::Show(lc_a_) + " vs " + ::lc::test::Show(lc_b_));         \
        }                                                                              \
    } while (false)

#define LC_CHECK_NE(a, b)                                                              \
    do {                                                                               \
        auto lc_a_ = (a);                                                              \
        auto lc_b_ = (b);                                                              \
        if ((lc_a_ == lc_b_)) {                                                        \
            ::lc::test::ReportFailure(__FILE__, __LINE__,                              \
                std::string(#a) + " != " + #b + "  |  both "                           \
                + ::lc::test::Show(lc_a_));                                            \
        }                                                                              \
    } while (false)

namespace lc::test {

// Show() renders a value for failure messages. Overload it for your own types.
inline std::string Show(bool v)        { return v ? "true" : "false"; }
inline std::string Show(u8 v)          { return std::to_string(static_cast<unsigned>(v)); }
inline std::string Show(u16 v)         { return std::to_string(v); }
inline std::string Show(u32 v)         { return std::to_string(v); }
inline std::string Show(u64 v)         { return std::to_string(v); }
inline std::string Show(i8 v)          { return std::to_string(static_cast<int>(v)); }
inline std::string Show(i16 v)         { return std::to_string(v); }
inline std::string Show(i32 v)         { return std::to_string(v); }
inline std::string Show(i64 v)         { return std::to_string(v); }
// NOTE: no std::size_t overload. On 64-bit MSVC size_t IS u64, so declaring both is a
// redefinition rather than an overload. size_t values match Show(u64) already.
inline std::string Show(const char* v) { return v ? std::string(v) : std::string("(null)"); }
inline std::string Show(const std::string& v) { return v; }

template <typename Tag>
inline std::string Show(Id<Tag> v) { return "#" + std::to_string(v.value); }

} // namespace lc::test
