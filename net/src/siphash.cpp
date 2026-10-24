#include "aether/net/siphash.h"

namespace aether::net {

namespace {
inline u64 Rotl(u64 x, int b) { return (x << b) | (x >> (64 - b)); }
inline u64 Load64(const u8* p) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<u64>(p[i]) << (8 * i);
    return v;
}
inline void Round(u64& v0, u64& v1, u64& v2, u64& v3) {
    v0 += v1, v1 = Rotl(v1, 13), v1 ^= v0, v0 = Rotl(v0, 32);
    v2 += v3, v3 = Rotl(v3, 16), v3 ^= v2;
    v0 += v3, v3 = Rotl(v3, 21), v3 ^= v0;
    v2 += v1, v1 = Rotl(v1, 17), v1 ^= v2, v2 = Rotl(v2, 32);
}
} // namespace

u64 SipHash24(const SipKey& key, std::span<const u8> data) {
    const u64 k0 = Load64(key.data()), k1 = Load64(key.data() + 8);
    u64 v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1;
    u64 v2 = 0x6c7967656e657261ull ^ k0, v3 = 0x7465646279746573ull ^ k1;
    const usize n = data.size(), whole = n - n % 8;
    for (usize i = 0; i < whole; i += 8) {
        const u64 m = Load64(data.data() + i);
        v3 ^= m;
        Round(v0, v1, v2, v3), Round(v0, v1, v2, v3);
        v0 ^= m;
    }
    u64 b = static_cast<u64>(n) << 56;
    for (usize i = whole; i < n; ++i) b |= static_cast<u64>(data[i]) << (8 * (i - whole));
    v3 ^= b;
    Round(v0, v1, v2, v3), Round(v0, v1, v2, v3);
    v0 ^= b;
    v2 ^= 0xff;
    for (int i = 0; i < 4; ++i) Round(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

bool TagEqual(u64 a, u64 b) {
    volatile u64 d = a ^ b;
    return d == 0;
}

} // namespace aether::net
