#pragma once

#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

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

#define AETHER_TEST(name)                                                                         \
    static void name();                                                                           \
    static ::aether::test::Registrar registrar_##name(#name, name);                                \
    static void name()

#define AETHER_CHECK(cond)                                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("  CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                \
            ++::aether::test::FailureCount();                                                      \
        }                                                                                           \
    } while (0)

#define AETHER_CHECK_NEAR(a, b, eps)                                                               \
    do {                                                                                           \
        auto va = (a);                                                                             \
        auto vb = (b);                                                                             \
        if (std::fabs(static_cast<double>(va) - static_cast<double>(vb)) > (eps)) {                \
            std::printf("  CHECK_NEAR failed: %s ~= %s (%s:%d)\n", #a, #b, __FILE__, __LINE__);    \
            ++::aether::test::FailureCount();                                                      \
        }                                                                                           \
    } while (0)
