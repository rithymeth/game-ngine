#include "aether/terrain/world.h"

#include <algorithm>
#include <cmath>

namespace aether {
namespace terrain {

CellCoord WorldPartition::CellOf(f64 x, f64 z) const {
    const f64 size = settings_.cell_size;
    return {static_cast<i32>(std::floor(x / size)), static_cast<i32>(std::floor(z / size))};
}

void WorldPartition::CellBounds(CellCoord cell, f64& min_x, f64& min_z, f64& max_x, f64& max_z) const {
    const f64 size = settings_.cell_size;
    min_x = static_cast<f64>(cell.x) * size;
    min_z = static_cast<f64>(cell.z) * size;
    max_x = min_x + size;
    max_z = min_z + size;
}

f64 WorldPartition::DistanceToCell(CellCoord cell, f64 x, f64 z) const {
    f64 min_x, min_z, max_x, max_z;
    CellBounds(cell, min_x, min_z, max_x, max_z);
    const f64 dx = std::max({min_x - x, 0.0, x - max_x});
    const f64 dz = std::max({min_z - z, 0.0, z - max_z});
    return std::sqrt(dx * dx + dz * dz);
}

StreamingDelta WorldPartition::Update(f64 focus_x, f64 focus_z) {
    StreamingDelta delta;
    const f64 size = settings_.cell_size;
    if (size <= 0.0) return delta;

    // Unload first so a cell only unloads after leaving the larger radius.
    std::vector<std::pair<f64, CellCoord>> unloads;
    for (const CellCoord& cell : loaded_) {
        const f64 d = DistanceToCell(cell, focus_x, focus_z);
        if (d > settings_.unload_radius) unloads.emplace_back(d, cell);
    }
    std::sort(unloads.begin(), unloads.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first
                                  : (a.second.x != b.second.x ? a.second.x < b.second.x : a.second.z < b.second.z);
    });
    for (const auto& [d, cell] : unloads) {
        loaded_.erase(cell);
        delta.to_unload.push_back(cell);
    }

    const CellCoord lo = CellOf(focus_x - settings_.load_radius, focus_z - settings_.load_radius);
    const CellCoord hi = CellOf(focus_x + settings_.load_radius, focus_z + settings_.load_radius);
    std::vector<std::pair<f64, CellCoord>> loads;
    for (i32 z = lo.z; z <= hi.z; ++z) {
        for (i32 x = lo.x; x <= hi.x; ++x) {
            const CellCoord cell{x, z};
            if (loaded_.count(cell)) continue;
            const f64 d = DistanceToCell(cell, focus_x, focus_z);
            if (d <= settings_.load_radius) loads.emplace_back(d, cell);
        }
    }
    std::sort(loads.begin(), loads.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first
                                  : (a.second.x != b.second.x ? a.second.x < b.second.x : a.second.z < b.second.z);
    });
    const usize cap = settings_.max_loads_per_update == 0 ? loads.size() : settings_.max_loads_per_update;
    for (usize i = 0; i < loads.size() && i < cap; ++i) {
        loaded_.insert(loads[i].second);
        delta.to_load.push_back(loads[i].second);
    }
    return delta;
}

Vec3 FloatingOrigin::ToLocal(const WorldPosition& world) const {
    return Vec3(static_cast<f32>(world.x - origin_.x),
                static_cast<f32>(world.y - origin_.y),
                static_cast<f32>(world.z - origin_.z));
}

WorldPosition FloatingOrigin::ToWorld(const Vec3& local) const {
    return {origin_.x + static_cast<f64>(local.x),
            origin_.y + static_cast<f64>(local.y),
            origin_.z + static_cast<f64>(local.z)};
}

bool FloatingOrigin::Update(const WorldPosition& focus, Vec3& shift) {
    const f64 dx = focus.x - origin_.x;
    const f64 dy = focus.y - origin_.y;
    const f64 dz = focus.z - origin_.z;
    if (dx * dx + dy * dy + dz * dz <= threshold_ * threshold_) return false;
    shift = Vec3(static_cast<f32>(-dx), static_cast<f32>(-dy), static_cast<f32>(-dz));
    origin_ = focus;
    return true;
}

} // namespace terrain
} // namespace aether
