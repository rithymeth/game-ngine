#include "aether/memory/frame_allocator.h"
#include "aether/memory/linear_allocator.h"
#include "aether/memory/pool_allocator.h"
#include "test_framework.h"

#include <vector>

using namespace aether;

AETHER_TEST(LinearAllocator_AllocatesSequentially) {
    alignas(16) u8 buffer[256];
    LinearAllocator arena(buffer, sizeof(buffer));

    void* a = arena.Allocate(16, 16);
    void* b = arena.Allocate(32, 16);

    AETHER_CHECK(a != nullptr);
    AETHER_CHECK(b != nullptr);
    AETHER_CHECK(a != b);
    AETHER_CHECK(arena.Used() >= 48);
}

AETHER_TEST(LinearAllocator_FailsWhenOutOfSpace) {
    alignas(16) u8 buffer[32];
    LinearAllocator arena(buffer, sizeof(buffer));

    void* a = arena.Allocate(24, 8);
    void* b = arena.Allocate(24, 8);

    AETHER_CHECK(a != nullptr);
    AETHER_CHECK(b == nullptr);
}

AETHER_TEST(LinearAllocator_ResetReclaimsSpace) {
    alignas(16) u8 buffer[64];
    LinearAllocator arena(buffer, sizeof(buffer));

    arena.Allocate(64, 8);
    AETHER_CHECK(arena.Remaining() == 0);

    arena.Reset();
    AETHER_CHECK(arena.Remaining() == 64);
}

AETHER_TEST(PoolAllocator_AllocatesFixedBlocks) {
    struct Block { u64 data[2]; };
    alignas(16) u8 buffer[sizeof(Block) * 4];

    PoolAllocator pool(buffer, sizeof(buffer), sizeof(Block), alignof(Block));
    AETHER_CHECK(pool.BlockCount() == 4);
    AETHER_CHECK(pool.FreeCount() == 4);

    void* a = pool.Allocate();
    void* b = pool.Allocate();
    AETHER_CHECK(a != nullptr);
    AETHER_CHECK(b != nullptr);
    AETHER_CHECK(a != b);
    AETHER_CHECK(pool.FreeCount() == 2);

    pool.Free(a);
    AETHER_CHECK(pool.FreeCount() == 3);

    void* c = pool.Allocate();
    AETHER_CHECK(c == a); // freed block reused (LIFO free list)
}

AETHER_TEST(PoolAllocator_ExhaustsCleanly) {
    struct Block { void* padding; u32 v; }; // must be >= sizeof(void*): pool threads its free list through unused blocks
    alignas(8) u8 buffer[sizeof(Block) * 2];
    PoolAllocator pool(buffer, sizeof(buffer), sizeof(Block), alignof(Block));

    std::vector<void*> allocs;
    for (usize i = 0; i < pool.BlockCount(); ++i) {
        void* p = pool.Allocate();
        AETHER_CHECK(p != nullptr);
        allocs.push_back(p);
    }
    AETHER_CHECK(pool.Allocate() == nullptr);
}

AETHER_TEST(FrameAllocator_SwapResetsOldestBuffer) {
    FrameAllocator frame(1024);

    void* a = frame.Allocate(64, 16);
    AETHER_CHECK(a != nullptr);
    usize used_before_swap = frame.Current().Used();
    AETHER_CHECK(used_before_swap >= 64);

    frame.Swap(); // now on buffer 1, buffer 0 still holds `a`'s data
    AETHER_CHECK(frame.Current().Used() == 0);

    frame.Swap(); // back to buffer 0, which should now be reset
    AETHER_CHECK(frame.Current().Used() == 0);
}
