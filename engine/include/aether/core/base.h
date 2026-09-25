#pragma once

#include <cstddef>
#include <cstdint>

namespace aether {

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using f32 = float;
using f64 = double;

using usize = std::size_t;

// Cache line size for alignment of hot data to avoid false sharing.
inline constexpr usize kCacheLineSize = 64;

inline usize AlignUp(usize value, usize alignment) {
    return (value + (alignment - 1)) & ~(alignment - 1);
}

} // namespace aether

#if defined(_MSC_VER)
#define AETHER_FORCEINLINE __forceinline
#define AETHER_NOINLINE __declspec(noinline)
#define AETHER_DEBUGBREAK() __debugbreak()
#else
#define AETHER_FORCEINLINE inline __attribute__((always_inline))
#define AETHER_NOINLINE __attribute__((noinline))
#define AETHER_DEBUGBREAK() __builtin_trap()
#endif

#define AETHER_ALIGN(n) alignas(n)

#ifdef NDEBUG
#define AETHER_ASSERT(expr) ((void)0)
#else
#define AETHER_ASSERT(expr)                                                                      \
    do {                                                                                         \
        if (!(expr)) {                                                                           \
            AETHER_DEBUGBREAK();                                                                 \
        }                                                                                        \
    } while (0)
#endif
