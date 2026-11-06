#pragma once

#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "aether/core/log.h"

namespace aether::test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

inline int& FailureCount() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        Registry().push_back({name, std::move(fn)});
    }
};

// The test running now, for the crash handler in main.cpp.
inline const char*& CurrentTest() {
    static const char* name = nullptr;
    return name;
}

// The check being evaluated right now. A check that reports can print itself, but a check that *traps*
// (an assertion inside an expression, a null dereference) ends the run with only the test name recorded,
// which says which test died and not which line of it. The macros below set this before evaluating.
struct CheckSite {
    const char* expression = nullptr;
    const char* file = nullptr;
    int line = 0;
};

inline CheckSite& CurrentCheck() {
    static CheckSite site;
    return site;
}

inline void SetCurrentCheck(const char* expression, const char* file, int line) {
    CheckSite& site = CurrentCheck();
    site.expression = expression;
    site.file = file;
    site.line = line;
}

// Whether an error line containing `needle` has been logged since the recent buffer was cleared.
//
// A validation that logs and returns has no return value to assert, and most of the RHI's input validation works
// that way -- so the log is the only observable effect. Clear the buffer before the call under test, so an older
// line cannot satisfy the check. Example:
//
//     aether::Logger::Instance().ClearRecent();
//     command_list.Dispatch(0);
//     AETHER_CHECK(LoggedErrorContaining("dispatch group counts"));
inline bool LoggedErrorContaining(const char* needle) {
    for (const auto& line : aether::Logger::Instance().Recent()) {
        if (line.level >= aether::LogLevel::Error && line.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// AETHER_TEST_FILTER=Job,Audio runs only the tests whose name contains one of the comma-separated
// pieces (used by the thread-sanitizer job to run the threaded tests); unset runs everything.
inline bool Selected(const std::string& name) {
    const char* filter = std::getenv("AETHER_TEST_FILTER");
    if (filter == nullptr || *filter == '\0') return true;
    const std::string list = filter;
    std::size_t begin = 0;
    while (begin <= list.size()) {
        const std::size_t end = list.find(',', begin) == std::string::npos ? list.size() : list.find(',', begin);
        const std::string piece = list.substr(begin, end - begin);
        if (!piece.empty() && name.find(piece) != std::string::npos) return true;
        begin = end + 1;
    }
    return false;
}

inline int RunAll() {
    int passed = 0;
    std::size_t ran = 0;
    for (auto& test : Registry()) {
        if (!Selected(test.name)) continue;
        ++ran;
        int before = FailureCount();
        CurrentTest() = test.name.c_str();
        test.fn();
        CurrentTest() = nullptr;
        if (FailureCount() == before) {
            std::printf("[PASS] %s\n", test.name.c_str());
            ++passed;
        } else {
            std::printf("[FAIL] %s\n", test.name.c_str());
        }
    }
    std::printf("\n%d/%zu tests passed\n", passed, ran);
    return FailureCount() == 0 ? 0 : 1;
}

} // namespace aether::test

#define AETHER_TEST(name)                                                                          \
    static void name();                                                                            \
    static ::aether::test::Registrar registrar_##name(#name, name);                                \
    static void name()

#define AETHER_CHECK(cond)                                                                         \
    do {                                                                                           \
        ::aether::test::SetCurrentCheck(#cond, __FILE__, __LINE__);                                \
        if (!(cond)) {                                                                             \
            std::printf("  CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                \
            ++::aether::test::FailureCount();                                                      \
        }                                                                                          \
    } while (0)

#define AETHER_CHECK_NEAR(a, b, eps)                                                               \
    do {                                                                                           \
        ::aether::test::SetCurrentCheck(#a " ~= " #b, __FILE__, __LINE__);                         \
        auto va = (a);                                                                             \
        auto vb = (b);                                                                             \
        if (std::fabs(static_cast<double>(va) - static_cast<double>(vb)) > (eps)) {                \
            std::printf("  CHECK_NEAR failed: %s ~= %s (%s:%d)\n", #a, #b, __FILE__, __LINE__);    \
            ++::aether::test::FailureCount();                                                      \
        }                                                                                          \
    } while (0)
