#pragma once

// Minimal test harness so the core has no third-party dependencies.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    std::function<void()> body;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

inline void fail(const char* file, int line, const std::string& message) {
    std::printf("    FAIL %s:%d: %s\n", file, line, message.c_str());
    ++failures();
}

inline int runAll() {
    int failedCases = 0;
    for (const auto& c : registry()) {
        const int before = failures();
        c.body();
        const bool passed = failures() == before;
        std::printf("%s %s\n", passed ? "  ok  " : "  FAIL", c.name);
        if (!passed) ++failedCases;
    }
    std::printf("\n%zu tests, %d failed\n", registry().size(), failedCases);
    return failedCases == 0 ? 0 : 1;
}

}  // namespace test

#define TEST_CONCAT_INNER(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_INNER(a, b)

#define TEST(name)                                                                   \
    static void TEST_CONCAT(test_fn_, __LINE__)();                                   \
    static test::Registrar TEST_CONCAT(test_reg_, __LINE__)(name, TEST_CONCAT(test_fn_, __LINE__)); \
    static void TEST_CONCAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) test::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(actual, expected)                                                             \
    do {                                                                                       \
        const auto a_ = (actual);                                                              \
        const auto e_ = (expected);                                                            \
        if (!(a_ == e_)) {                                                                     \
            test::fail(__FILE__, __LINE__,                                                     \
                       std::string(#actual " == " #expected ": got ") + std::to_string(a_) +   \
                           ", expected " + std::to_string(e_));                                \
        }                                                                                      \
    } while (0)
