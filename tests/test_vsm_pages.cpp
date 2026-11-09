#include "../player/src/vsm_pages.h"
#include "test_framework.h"

#include <algorithm>
#include <set>
#include <tuple>
#include <vector>

using namespace aether;
using namespace aether::player;

// The physical side of virtual shadow maps (player/src/vsm_pages.h): residency, eviction, what to draw, the page table.

namespace {

std::vector<VsmRequest> Requests(std::initializer_list<std::tuple<u32, i32, i32, u32>> list) {
    std::vector<VsmRequest> out;
    for (const auto& [level, x, y, hits] : list) out.push_back({{level, x, y}, hits});
    return out;
}

std::array<std::array<i32, 2>, kVsmLevels> Windows(i32 x, i32 y) {
    std::array<std::array<i32, 2>, kVsmLevels> out{};
    for (auto& origin : out) origin = {x, y};
    return out;
}

VsmPage Page(u32 level, i32 x, i32 y) { return VsmPage{level, x, y}; }

} // namespace

AETHER_TEST(VsmPages_NewPagesGetDistinctPhysicalPagesAndAreDrawnOnce) {
    VsmPageAllocator allocator(4); // 16 pages
    AETHER_CHECK(allocator.Capacity() == 16);
    allocator.BeginFrame(1);
    const auto requests = Requests({{0, 5, 5, 9}, {0, 6, 5, 4}, {1, 2, 2, 7}});
    const VsmPageStats stats = allocator.Request(requests);
    AETHER_CHECK(stats.allocated == 3 && stats.evicted == 0 && stats.dropped == 0 && stats.resident == 3 && stats.pending == 3);
    std::set<u32> physical;
    for (const VsmRequest& request : requests) physical.insert(allocator.PhysicalOf(request.page));
    AETHER_CHECK(physical.size() == 3 && !physical.count(kVsmNoPage));
    for (const u32 index : physical) AETHER_CHECK(index < 16);
    // Drawn most-looked-at first, within the budget; a page drawn is drawn once.
    const std::vector<VsmPageDraw> draws = allocator.TakeDraws(2);
    AETHER_CHECK(draws.size() == 2 && draws[0].page == requests[0].page && draws[1].page == requests[2].page);
    AETHER_CHECK(allocator.TakeDraws(8).size() == 1);
    AETHER_CHECK(allocator.TakeDraws(8).empty());
    AETHER_CHECK(allocator.Stats().pending == 0);
    // Asking again changes nothing.
    allocator.BeginFrame(2);
    const VsmPageStats again = allocator.Request(requests);
    AETHER_CHECK(again.allocated == 0 && again.resident == 3);
    AETHER_CHECK(allocator.TakeDraws(8).empty());
    // Physical origins tile the pool.
    u32 x = 0, y = 0;
    allocator.PhysicalOrigin(5, x, y);
    AETHER_CHECK(x == 1 * kVsmPageTexels && y == 1 * kVsmPageTexels);
}

AETHER_TEST(VsmPages_AFullPoolEvictsTheLeastRecentlyUsedAndNeverThisFramesPages) {
    VsmPageAllocator allocator(2); // 4 pages
    allocator.BeginFrame(1);
    allocator.Request(Requests({{0, 0, 0, 1}, {0, 1, 0, 1}}));
    allocator.BeginFrame(2);
    allocator.Request(Requests({{0, 2, 0, 1}, {0, 3, 0, 1}}));
    AETHER_CHECK(allocator.Stats().resident == 4);
    // Frame 3 wants two new pages and keeps (0,3,0): the oldest two (frame 1) go, not frame 2's.
    allocator.BeginFrame(3);
    const VsmPageStats stats = allocator.Request(Requests({{0, 3, 0, 5}, {0, 10, 0, 3}, {0, 11, 0, 2}}));
    AETHER_CHECK(stats.evicted == 2 && stats.allocated == 2 && stats.dropped == 0);
    AETHER_CHECK(!allocator.IsResident(Page(0, 0, 0)) && !allocator.IsResident(Page(0, 1, 0)));
    AETHER_CHECK(allocator.IsResident(Page(0, 2, 0)) && allocator.IsResident(Page(0, 3, 0)) && allocator.IsResident(Page(0, 10, 0)));
    // More pages asked for in one frame than the pool holds: the rest are dropped, nothing used this frame is evicted.
    allocator.BeginFrame(4);
    const VsmPageStats crowded =
        allocator.Request(Requests({{0, 20, 0, 9}, {0, 21, 0, 8}, {0, 22, 0, 7}, {0, 23, 0, 6}, {0, 24, 0, 5}, {0, 25, 0, 4}}));
    AETHER_CHECK(crowded.allocated == 4 && crowded.dropped == 2 && crowded.resident == 4);
    AETHER_CHECK(allocator.IsResident(Page(0, 20, 0)) && allocator.IsResident(Page(0, 23, 0)) && !allocator.IsResident(Page(0, 24, 0)));
    // Among pages used at the same time, the finer level goes first.
    VsmPageAllocator tie(1);
    tie.BeginFrame(1);
    tie.Request(Requests({{3, 0, 0, 1}}));
    tie.BeginFrame(2);
    tie.Request(Requests({{0, 9, 9, 1}}));
    AETHER_CHECK(!tie.IsResident(Page(3, 0, 0)) && tie.IsResident(Page(0, 9, 9)));
    // A full pool of four, all used in frame 1: one new page takes a level 1 page (the finest, the first of them by key), not the level 4 one.
    VsmPageAllocator keeps_coarse(2);
    keeps_coarse.BeginFrame(1);
    keeps_coarse.Request(Requests({{4, 0, 0, 1}, {1, 0, 0, 1}, {1, 1, 0, 1}, {1, 2, 0, 1}}));
    AETHER_CHECK(keeps_coarse.Stats().resident == 4);
    keeps_coarse.BeginFrame(2);
    const VsmPageStats taken = keeps_coarse.Request(Requests({{2, 5, 5, 1}}));
    AETHER_CHECK(taken.evicted == 1 && taken.allocated == 1);
    AETHER_CHECK(keeps_coarse.IsResident(Page(4, 0, 0)) && !keeps_coarse.IsResident(Page(1, 0, 0)));
    AETHER_CHECK(keeps_coarse.IsResident(Page(1, 1, 0)) && keeps_coarse.IsResident(Page(1, 2, 0)) && keeps_coarse.IsResident(Page(2, 5, 5)));
}

AETHER_TEST(VsmPages_AMovingCasterRedrawsTheStalePagesItReaches) {
    VsmPageAllocator allocator(4);
    const VsmLight light = VsmLightBasis(Vec3(0.3f, -1.0f, 0.2f));
    const f32 size = VsmPageSize(1);
    // Three pages of level 1 along u: x = 0, 1, 5.
    allocator.BeginFrame(1);
    allocator.Request(Requests({{1, 0, 0, 3}, {1, 1, 0, 2}, {1, 5, 0, 1}}));
    AETHER_CHECK(allocator.TakeDraws(8).size() == 3);
    // A small sphere at the middle of page x = 1 reaches only that page.
    const Vec3 inside = light.right * (size * 1.5f) + light.up * (size * 0.5f);
    AETHER_CHECK(allocator.InvalidateSphere(light, inside, size * 0.1f) == 1);
    AETHER_CHECK(allocator.IsPending(Page(1, 1, 0)) && !allocator.IsPending(Page(1, 0, 0)) && !allocator.IsPending(Page(1, 5, 0)));
    // A sphere on the border between x = 0 and 1 reaches both (the circle meets both rectangles).
    allocator.TakeDraws(8);
    AETHER_CHECK(allocator.InvalidateSphere(light, light.right * size + light.up * (size * 0.5f), size * 0.05f) == 2);
    // A sphere over pages already waiting is not counted twice, and one far away reaches nothing.
    AETHER_CHECK(allocator.InvalidateSphere(light, light.right * size + light.up * (size * 0.5f), size * 0.05f) == 0);
    allocator.TakeDraws(8);
    AETHER_CHECK(allocator.InvalidateSphere(light, light.right * (size * 50.0f), size * 0.5f) == 0);
    // A sphere is a circle in the page's space, whatever its depth along the light.
    AETHER_CHECK(allocator.InvalidateSphere(light, inside + light.direction * 500.0f, size * 0.1f) == 1);
    allocator.TakeDraws(8);
    // Stale pages are redrawn before new ones, even when the new ones were looked at more.
    allocator.BeginFrame(2);
    allocator.Request(Requests({{1, 9, 0, 100}}));
    AETHER_CHECK(allocator.InvalidateSphere(light, light.right * (size * 5.5f) + light.up * (size * 0.5f), size * 0.1f) == 1);
    const std::vector<VsmPageDraw> draws = allocator.TakeDraws(1);
    AETHER_CHECK(draws.size() == 1 && draws[0].page == Page(1, 5, 0));
    // A sun that turned redraws everything.
    allocator.TakeDraws(8);
    allocator.InvalidateAll();
    AETHER_CHECK(allocator.Stats().pending == allocator.Stats().resident);
    AETHER_CHECK(allocator.TakeDraws(100).size() == 4);
}

AETHER_TEST(VsmPages_ThePageTableHoldsOnlyDrawnPagesInsideTheWindow) {
    VsmPageAllocator allocator(4);
    allocator.BeginFrame(1);
    // Level 2 pages: (40, -3) inside a window whose origin is (30, -10); (70, 0) outside it; (31, -10) the window's first row and column.
    allocator.Request(Requests({{2, 40, -3, 5}, {2, 70, 0, 4}, {2, 31, -10, 3}, {0, 7, 7, 2}}));
    const auto windows = Windows(30, -10);
    // Nothing is drawn yet: nothing in the table.
    std::vector<u32> table = allocator.BuildPageTable(windows);
    AETHER_CHECK(table.size() == VsmPageAllocator::kTableEntries);
    AETHER_CHECK(std::count_if(table.begin(), table.end(), [](u32 e) { return e != 0; }) == 0);
    allocator.TakeDraws(8);
    table = allocator.BuildPageTable(windows);
    const auto at = [&](u32 level, i32 x, i32 y) {
        return table[(static_cast<usize>(level) * 32 + (static_cast<u32>(y) & 31)) * 32 + (static_cast<u32>(x) & 31)];
    };
    AETHER_CHECK(at(2, 40, -3) == allocator.PhysicalOf(Page(2, 40, -3)) + 1);
    AETHER_CHECK(at(2, 31, -10) == allocator.PhysicalOf(Page(2, 31, -10)) + 1);
    AETHER_CHECK(at(2, 70, 0) == 0); // outside the window: its wrapped slot is not claimed
    AETHER_CHECK(std::count_if(table.begin(), table.end(), [](u32 e) { return e != 0; }) == 2); // (0, 7, 7) is outside the window too
    // A page waiting for a redraw drops out until it is drawn; the rest stay.
    const VsmLight light = VsmLightBasis(Vec3(0, -1, 0));
    const f32 size = VsmPageSize(2);
    allocator.InvalidateSphere(light, light.right * (size * 40.5f) + light.up * (size * -2.5f), size * 0.1f);
    table = allocator.BuildPageTable(windows);
    AETHER_CHECK(at(2, 40, -3) == 0 && at(2, 31, -10) != 0);
    allocator.TakeDraws(8);
    table = allocator.BuildPageTable(windows);
    AETHER_CHECK(at(2, 40, -3) != 0);
    // A window that has moved on drops the pages it no longer covers.
    table = allocator.BuildPageTable(Windows(100, 100));
    AETHER_CHECK(std::count_if(table.begin(), table.end(), [](u32 e) { return e != 0; }) == 0);
}

AETHER_TEST(VsmPages_TheSameRequestsGiveTheSameAllocation) {
    const auto run = [] {
        VsmPageAllocator allocator(3);
        std::vector<u32> physical;
        for (u64 frame = 1; frame <= 6; ++frame) {
            allocator.BeginFrame(frame);
            std::vector<VsmRequest> requests;
            for (i32 i = 0; i < 5; ++i) {
                requests.push_back({{static_cast<u32>(i % 3), static_cast<i32>(frame) + i, i}, static_cast<u32>(10 - i)});
            }
            allocator.Request(requests);
            for (const VsmPageDraw& draw : allocator.TakeDraws(3)) physical.push_back(draw.physical);
        }
        return physical;
    };
    AETHER_CHECK(run() == run());
}
