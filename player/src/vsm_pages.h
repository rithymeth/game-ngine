#pragma once

#include "virtual_shadow.h"

#include <array>
#include <span>
#include <unordered_map>
#include <vector>

// The physical side of virtual shadow maps: which virtual pages (virtual_shadow.h) are resident in the pool of physical pages, which one each
// occupies, what to evict when the pool is full, which pages must be drawn (new ones, and ones a moving caster has made stale), and the page
// table the scene shader reads. CPU only; the pass that draws the pages and the shader are the next stage.
//
// The pool is a square of pool_pages_per_side^2 pages of kVsmPageTexels^2 texels, one depth atlas; physical page i sits at column i % side, row
// i / side. Rules:
//  - A page asked for this frame is never evicted this frame; when the pool holds nothing else to evict, the request is dropped (counted).
//  - Eviction takes the page used longest ago, a finer level before a coarser one among equals, so the pool keeps the coarse pages that cover
//    a lot of ground when it is short.
//  - A page is drawn once, when it gets its physical page, and again when something it contains moves (Invalidate*). A page that is waiting to
//    be drawn is not in the page table, so the shader falls back to a coarser level and then the cascades rather than read stale or empty depth.

namespace aether::player {

constexpr u32 kVsmNoPage = 0xFFFFFFFFu;

struct VsmPageDraw {
    VsmPage page;
    u32 physical = 0;
};

struct VsmPageStats {
    u32 resident = 0;   // pages holding a physical page
    u32 pending = 0;    // of those, waiting to be drawn
    u32 allocated = 0;  // given a physical page by the last Request
    u32 evicted = 0;    // taken from another page by the last Request
    u32 dropped = 0;    // asked for, but the pool had nothing left to evict
};

class VsmPageAllocator {
public:
    explicit VsmPageAllocator(u32 pool_pages_per_side);
    u32 PoolPagesPerSide() const { return side_; }
    u32 Capacity() const { return side_ * side_; }

    // Starts a frame: pages asked for from now on are protected from eviction until the next BeginFrame.
    void BeginFrame(u64 frame);
    // Marks the pages in use this frame and gives the new ones physical pages, most looked at first. Returns what happened.
    VsmPageStats Request(std::span<const VsmRequest> requests);

    // Marks a resident page as in use this frame without a request from the feedback (a page the caller wants kept).
    bool Touch(const VsmPage& page);

    // Pages that must be drawn this frame: pending ones, those a moving caster stopped being valid for first, then new ones, at most `budget`.
    // They count as drawn from now on (the caller draws them this frame, before the scene pass reads the table).
    std::vector<VsmPageDraw> TakeDraws(u32 budget);

    // Marks resident pages whose area the world-space sphere reaches as needing a redraw. Returns how many.
    u32 InvalidateSphere(const VsmLight& light, const Vec3& center, f32 radius);
    // Every resident page needs a redraw (the sun turned, so every page's contents mean something else).
    void InvalidateAll();
    // Forgets every page.
    void Clear();

    bool IsResident(const VsmPage& page) const { return pages_.count(Key(page)) != 0; }
    u32 PhysicalOf(const VsmPage& page) const;                 // kVsmNoPage when not resident
    bool IsPending(const VsmPage& page) const;
    VsmPageStats Stats() const;

    // The page table for a camera with these window origins: kVsmLevels * 32 * 32 entries, [level][y & 31][x & 31], physical index + 1 for a
    // resident page inside the level's window that is not waiting to be drawn, else 0. Pages a world-space sphere of `suppress` reaches are
    // left out too (the cascades shadow them), and counted in `suppressed` when given.
    struct Sphere {
        Vec3 center;
        f32 radius = 0.0f;
    };
    std::vector<u32> BuildPageTable(const std::array<std::array<i32, 2>, kVsmLevels>& window_origins, const VsmLight& light = {},
                                    std::span<const Sphere> suppress = {}, u32* suppressed = nullptr) const;
    static constexpr usize kTableEntries = static_cast<usize>(kVsmLevels) * kVsmWindowPages * kVsmWindowPages;

    // A physical page's origin in the pool, in texels.
    void PhysicalOrigin(u32 physical, u32& x, u32& y) const;

private:
    struct Resident {
        VsmPage page;
        u32 physical = 0;
        u64 last_used = 0;
        bool pending = true;  // needs drawing
        bool stale = false;   // was drawn, and has been invalidated since: drawn before the new ones
        u32 hits = 0;         // of the last request, for ordering the draws
    };
    static u64 Key(const VsmPage& page);
    bool Evict(u32& physical);

    u32 side_ = 0;
    u64 frame_ = 0;
    std::unordered_map<u64, Resident> pages_;
    std::vector<u32> free_;
    VsmPageStats last_;
};

} // namespace aether::player
