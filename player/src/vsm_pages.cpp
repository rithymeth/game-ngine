#include "vsm_pages.h"

#include <algorithm>
#include <cmath>

namespace aether::player {

VsmPageAllocator::VsmPageAllocator(u32 pool_pages_per_side) : side_(std::clamp(pool_pages_per_side, 1u, 64u)) {
    Clear();
}

void VsmPageAllocator::Clear() {
    pages_.clear();
    free_.clear();
    for (u32 i = Capacity(); i-- > 0;) free_.push_back(i); // the first page handed out is physical page 0
    last_ = {};
}

u64 VsmPageAllocator::Key(const VsmPage& page) {
    return (static_cast<u64>(page.level & 7u) << 56) | (static_cast<u64>(static_cast<u32>(page.x) & 0x0FFFFFFFu) << 28) |
           static_cast<u64>(static_cast<u32>(page.y) & 0x0FFFFFFFu);
}

void VsmPageAllocator::BeginFrame(u64 frame) { frame_ = frame; }

bool VsmPageAllocator::Evict(u32& physical) {
    Resident* victim = nullptr;
    for (auto& [key, page] : pages_) {
        if (page.last_used == frame_) continue; // asked for this frame
        if (victim == nullptr || page.last_used < victim->last_used ||
            (page.last_used == victim->last_used &&
             (page.page.level < victim->page.level || (page.page.level == victim->page.level && Key(page.page) < Key(victim->page))))) {
            victim = &page;
        }
    }
    if (victim == nullptr) return false;
    physical = victim->physical;
    pages_.erase(Key(victim->page));
    return true;
}

VsmPageStats VsmPageAllocator::Request(std::span<const VsmRequest> requests) {
    VsmPageStats result;
    for (const VsmRequest& request : requests) {
        if (request.page.level >= kVsmLevels) continue;
        const u64 key = Key(request.page);
        if (const auto found = pages_.find(key); found != pages_.end()) {
            found->second.last_used = frame_;
            found->second.hits = request.hits;
            continue;
        }
        u32 physical = 0;
        if (!free_.empty()) {
            physical = free_.back();
            free_.pop_back();
        } else if (Evict(physical)) {
            ++result.evicted;
        } else {
            ++result.dropped;
            continue;
        }
        Resident resident;
        resident.page = request.page;
        resident.physical = physical;
        resident.last_used = frame_;
        resident.hits = request.hits;
        pages_.emplace(key, resident);
        ++result.allocated;
    }
    result.resident = static_cast<u32>(pages_.size());
    for (const auto& [key, page] : pages_) result.pending += page.pending ? 1u : 0u;
    last_ = result;
    return result;
}

bool VsmPageAllocator::Touch(const VsmPage& page) {
    const auto found = pages_.find(Key(page));
    if (found == pages_.end()) return false;
    found->second.last_used = frame_;
    return true;
}

std::vector<VsmPageDraw> VsmPageAllocator::TakeDraws(u32 budget) {
    std::vector<Resident*> waiting;
    for (auto& [key, page] : pages_) {
        if (page.pending) waiting.push_back(&page);
    }
    std::sort(waiting.begin(), waiting.end(), [](const Resident* a, const Resident* b) {
        if (a->stale != b->stale) return a->stale; // pages that were right and no longer are, first
        if (a->hits != b->hits) return a->hits > b->hits;
        return Key(a->page) < Key(b->page);
    });
    std::vector<VsmPageDraw> draws;
    for (Resident* page : waiting) {
        if (draws.size() >= budget) break;
        draws.push_back({page->page, page->physical});
        page->pending = false;
        page->stale = false;
    }
    return draws;
}

u32 VsmPageAllocator::InvalidateSphere(const VsmLight& light, const Vec3& center, f32 radius) {
    u32 count = 0;
    for (auto& [key, page] : pages_) {
        if (page.pending) continue; // never drawn, or already waiting
        if (!VsmSphereTouchesPage(light, page.page, center, radius)) continue;
        page.pending = true;
        page.stale = true;
        ++count;
    }
    return count;
}

void VsmPageAllocator::InvalidateAll() {
    for (auto& [key, page] : pages_) {
        if (!page.pending) page.stale = true;
        page.pending = true;
    }
}

u32 VsmPageAllocator::PhysicalOf(const VsmPage& page) const {
    const auto found = pages_.find(Key(page));
    return found == pages_.end() ? kVsmNoPage : found->second.physical;
}

bool VsmPageAllocator::IsPending(const VsmPage& page) const {
    const auto found = pages_.find(Key(page));
    return found != pages_.end() && found->second.pending;
}

VsmPageStats VsmPageAllocator::Stats() const {
    VsmPageStats stats = last_;
    stats.resident = static_cast<u32>(pages_.size());
    stats.pending = 0;
    for (const auto& [key, page] : pages_) stats.pending += page.pending ? 1u : 0u;
    return stats;
}

std::vector<u32> VsmPageAllocator::BuildPageTable(const std::array<std::array<i32, 2>, kVsmLevels>& window_origins) const {
    std::vector<u32> table(kTableEntries, 0);
    const i32 window = static_cast<i32>(kVsmWindowPages);
    for (const auto& [key, page] : pages_) {
        if (page.pending || page.page.level >= kVsmLevels) continue;
        const i32 ox = window_origins[page.page.level][0], oy = window_origins[page.page.level][1];
        if (page.page.x < ox || page.page.x >= ox + window || page.page.y < oy || page.page.y >= oy + window) continue;
        const usize index = (static_cast<usize>(page.page.level) * kVsmWindowPages + (static_cast<u32>(page.page.y) & (kVsmWindowPages - 1))) *
                                kVsmWindowPages +
                            (static_cast<u32>(page.page.x) & (kVsmWindowPages - 1));
        table[index] = page.physical + 1;
    }
    return table;
}

void VsmPageAllocator::PhysicalOrigin(u32 physical, u32& x, u32& y) const {
    x = (physical % side_) * kVsmPageTexels;
    y = (physical / side_) * kVsmPageTexels;
}

} // namespace aether::player
