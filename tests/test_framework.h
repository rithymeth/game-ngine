#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "aether/math/vec.h"

namespace aether {

inline bool NearEqual(f32 a, f32 b, f32 eps) {
    return std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= static_cast<double>(eps);
}

inline bool NearEqual(const Vec3& a, const Vec3& b, f32 eps) {
    return (a - b).Length() <= eps;
}

namespace test {

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

inline int RunAll() {
    int passed = 0;
    for (auto& test : Registry()) {
        int before = FailureCount();
        test.fn();
        if (FailureCount() == before) {
            std::printf("[PASS] %s\n", test.name.c_str());
            ++passed;
        } else {
            std::printf("[FAIL] %s\n", test.name.c_str());
        }
    }
    std::printf("\n%d/%zu tests passed\n", passed, Registry().size());
    return FailureCount() == 0 ? 0 : 1;
}

} // namespace test
} // namespace aether

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
