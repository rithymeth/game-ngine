#include "aether/streaming/streamer.h"

#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"
#include "aether/scene/world_origin.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <cmath>
#include <fstream>

namespace aether::streaming {

namespace {
const std::vector<Entity> kNone;
} // namespace

struct WorldStreamer::Shared {
    struct Result {
        bool ok = false;
        std::vector<u8> bytes;
    };
    std::mutex mutex;
    std::set<CellCoord> inflight;       // requested, not yet finished
    std::map<CellCoord, Result> ready;  // finished, waiting for Update to apply
};

usize WorldStreamer::InFlight() const {
    if (!shared_) return 0;
    std::lock_guard lock(shared_->mutex);
    return shared_->inflight.size() + shared_->ready.size();
}

WorldStreamer::WorldStreamer(World& world, WorldIndex index, CellLoader loader, GuidIndex* guids)
    : world_(world), index_(std::move(index)), loader_(std::move(loader)), guids_(guids) {
    RegisterStreamingComponents();
}

WorldStreamer::CellLoader WorldStreamer::FileLoader(std::filesystem::path dir) {
    return [dir = std::move(dir)](const CellCoord& cell, std::vector<u8>& bytes) {
        std::ifstream in(CellFile(dir, cell), std::ios::binary);
        if (!in) return false;
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    };
}

f32 WorldStreamer::DistanceToCell(const Vec3& local, const CellCoord& cell) const {
    // In the whole world (cells are), on the ground plane.
    const f64 wx = offset_.x + local.x, wz = offset_.z + local.z;
    const f64 size = index_.settings.cell_size;
    const f64 x0 = index_.settings.origin.x + cell.x * size, z0 = index_.settings.origin.z + cell.z * size;
    const f64 dx = std::max({x0 - wx, 0.0, wx - (x0 + size)});
    const f64 dz = std::max({z0 - wz, 0.0, wz - (z0 + size)});
    return static_cast<f32>(std::sqrt(dx * dx + dz * dz));
}

CellState WorldStreamer::State(const CellCoord& cell) const {
    if (loaded_.count(cell) != 0) return CellState::Loaded;
    return failed_.count(cell) != 0 ? CellState::Failed : CellState::Unloaded;
}

std::vector<CellCoord> WorldStreamer::LoadedCells() const {
    std::vector<CellCoord> out;
    for (const auto& [cell, entities] : loaded_) out.push_back(cell);
    return out;
}

const std::vector<Entity>& WorldStreamer::EntitiesIn(const CellCoord& cell) const {
    const auto it = loaded_.find(cell);
    return it == loaded_.end() ? kNone : it->second;
}

void WorldStreamer::Pin(const CellCoord& cell) { pinned_.insert(cell); }
void WorldStreamer::Unpin(const CellCoord& cell) { pinned_.erase(cell); }

bool WorldStreamer::LoadCell(const CellCoord& cell) {
    std::vector<u8> bytes;
    if (!loader_ || !loader_(cell, bytes)) {
        failed_[cell] = CellState::Failed;
        problems_.push_back("cell " + CellName(cell) + " couldn't be read");
        return false;
    }
    return ApplyCell(cell, bytes);
}

bool WorldStreamer::ApplyCell(const CellCoord& cell, std::vector<u8>& bytes) {
    // The loader reports exactly the entities it creates, so a cell costs its
    // own entities rather than two scans of the whole world (which made
    // applying cells quadratic in a large world).
    std::vector<Entity> added;
    SceneLoadOptions options;
    options.loaded_entities = &added;
    if (!LoadSceneFromMemory(world_, bytes, "cell " + CellName(cell), options)) {
        failed_[cell] = CellState::Failed;
        problems_.push_back("cell " + CellName(cell) + " isn't a scene");
        return false;
    }
    // Cells are saved in whole-world positions; Transforms are local to the floating origin.
    const Vec3 shift(static_cast<f32>(offset_.x), static_cast<f32>(offset_.y), static_cast<f32>(offset_.z));
    for (Entity e : added) {
        if (Transform* t = world_.GetComponent<Transform>(e); t != nullptr && world_.GetComponent<Parent>(e) == nullptr) t->position = t->position - shift;
        if (guids_ != nullptr)
            if (const IdComponent* id = world_.GetComponent<IdComponent>(e)) guids_->Add(id->guid, e);
    }
    loaded_[cell] = std::move(added);
    events_.push_back({cell, true, loaded_[cell].size()});
    if (on_cell_) on_cell_(cell, true);
    return true;
}

void WorldStreamer::UnloadCell(const CellCoord& cell) {
    const auto it = loaded_.find(cell);
    if (it == loaded_.end()) return;
    usize count = 0;
    for (Entity e : it->second) {
        if (!world_.IsAlive(e)) continue; // gameplay already removed it
        if (guids_ != nullptr)
            if (const IdComponent* id = world_.GetComponent<IdComponent>(e)) guids_->Remove(id->guid);
        world_.DestroyEntity(e);
        ++count;
    }
    loaded_.erase(it);
    events_.push_back({cell, false, count});
    if (on_cell_) on_cell_(cell, false);
}

void WorldStreamer::Update() {
    events_.clear();
    struct Source {
        Vec3 position;
        f32 radius;
    };
    std::vector<Source> sources;
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<StreamingSource>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* es = archetype.EntityArray(c);
            for (usize i = 0; i < archetype.ChunkEntityCount(c); ++i) {
                const StreamingSource* s = world_.GetComponent<StreamingSource>(es[i]);
                const Transform* t = world_.GetComponent<Transform>(es[i]);
                if (s != nullptr && t != nullptr && s->enabled) sources.push_back({t->position, s->radius});
            }
        }
    });
    // How near each cell is to wanting to load (<= 1: within a source's radius; <= margin: keep).
    const auto nearness = [&](const CellCoord& cell) {
        if (pinned_.count(cell) != 0) return 0.0f;
        f32 best = 1e30f;
        for (const Source& s : sources) {
            const f32 d = DistanceToCell(s.position, cell);
            best = std::min(best, s.radius > 0.0f ? d / s.radius : (d <= 0.0f ? 0.0f : 1e30f));
        }
        return best;
    };
    // Unload what's beyond every source's reach (with the margin).
    usize unloads = 0;
    for (const CellCoord& cell : LoadedCells()) {
        if (unloads >= max_unloads_per_update) break;
        if (nearness(cell) > unload_margin) {
            UnloadCell(cell);
            ++unloads;
        }
    }
    // WorldIndex::cells is sorted by (x,z). Narrow source windows can skip
    // entire columns and binary-search the z range inside each surviving one,
    // rather than testing every cell against every source on every frame.
    struct Window {
        i32 min_x, max_x, min_z, max_z;
    };
    std::vector<Window> windows;
    bool full_scan = false;
    f64 estimated_cells = 0.0;
    const f64 size = index_.settings.cell_size;
    constexpr i32 min_coord = std::numeric_limits<i32>::min();
    constexpr i32 max_coord = std::numeric_limits<i32>::max();
    for (const Source& source : sources) {
        // Keep the old distance predicate for unusual/non-finite inputs and
        // queries covering most of the index; don't enumerate a huge grid.
        if (!(size > 0.0) || !std::isfinite(size) || !std::isfinite(source.radius)) {
            full_scan = true;
            break;
        }
        // DistanceToCell and the normalized priority use f32. Expand by one
        // float ULP, then pad one grid cell so rounded radius boundaries and
        // cells touching a grid line cannot be missed by the broad phase.
        const f64 radius = source.radius > 0.0f
            ? std::nextafter(source.radius, std::numeric_limits<f32>::infinity()) : 0.0;
        const f64 world_x = offset_.x + source.position.x;
        const f64 world_z = offset_.z + source.position.z;
        const f64 magnitude = std::max({std::abs(world_x), std::abs(world_z),
            std::abs(static_cast<f64>(index_.settings.origin.x)), std::abs(static_cast<f64>(index_.settings.origin.z))});
        if (magnitude * std::numeric_limits<f64>::epsilon() * 4.0 >= size) {
            // At this scale double-precision bounds can collapse adjacent
            // cells. Only the original world-space predicate is authoritative.
            full_scan = true;
            break;
        }
        const f64 x = world_x - index_.settings.origin.x;
        const f64 z = world_z - index_.settings.origin.z;
        const f64 lo_x = std::floor((x - radius) / size) - 1.0;
        const f64 hi_x = std::floor((x + radius) / size) + 1.0;
        const f64 lo_z = std::floor((z - radius) / size) - 1.0;
        const f64 hi_z = std::floor((z + radius) / size) + 1.0;
        if (!std::isfinite(lo_x) || !std::isfinite(hi_x) ||
            !std::isfinite(lo_z) || !std::isfinite(hi_z)) {
            full_scan = true;
            break;
        }
        const auto clamp_coord = [](f64 value) {
            return static_cast<i32>(std::clamp(value, static_cast<f64>(min_coord), static_cast<f64>(max_coord)));
        };
        const Window window{clamp_coord(lo_x), clamp_coord(hi_x), clamp_coord(lo_z), clamp_coord(hi_z)};
        estimated_cells += (static_cast<f64>(window.max_x) - window.min_x + 1.0) *
                           (static_cast<f64>(window.max_z) - window.min_z + 1.0);
        if (estimated_cells >= static_cast<f64>(index_.cells.size())) {
            full_scan = true;
            break;
        }
        windows.push_back(window);
    }

    // Load the nearest wanted cells first. The broad phase only selects
    // candidates; the existing distance/radius rule remains the final test.
    std::vector<std::pair<f32, CellCoord>> wanted;
    if (full_scan) {
        for (const CellInfo& info : index_.cells) {
            if (loaded_.count(info.coord) != 0 || failed_.count(info.coord) != 0) continue;
            const f32 n = nearness(info.coord);
            if (n <= 1.0f) wanted.push_back({n, info.coord});
        }
        for (const CellCoord& cell : pinned_) {
            if (index_.Find(cell) == nullptr && loaded_.count(cell) == 0 && failed_.count(cell) == 0)
                wanted.push_back({0.0f, cell});
        }
    } else {
        std::map<CellCoord, f32> candidates;
        const auto less = [](const CellInfo& info, const CellCoord& cell) { return info.coord < cell; };
        const auto greater = [](const CellCoord& cell, const CellInfo& info) { return cell < info.coord; };
        for (usize s = 0; s < sources.size(); ++s) {
            const Window& window = windows[s];
            auto column = std::lower_bound(index_.cells.begin(), index_.cells.end(), CellCoord{window.min_x, min_coord}, less);
            while (column != index_.cells.end() && column->coord.x <= window.max_x) {
                const i32 x = column->coord.x;
                const auto end = std::upper_bound(column, index_.cells.end(), CellCoord{x, max_coord}, greater);
                const auto begin = std::lower_bound(column, end, CellCoord{x, window.min_z}, less);
                const auto last = std::upper_bound(begin, end, CellCoord{x, window.max_z}, greater);
                for (auto it = begin; it != last; ++it) {
                    const CellCoord& cell = it->coord;
                    if (loaded_.count(cell) != 0 || failed_.count(cell) != 0) continue;
                    const f32 d = DistanceToCell(sources[s].position, cell);
                    const f32 n = sources[s].radius > 0.0f ? d / sources[s].radius : (d <= 0.0f ? 0.0f : 1e30f);
                    if (n > 1.0f) continue;
                    const auto [found, inserted] = candidates.emplace(cell, n);
                    if (!inserted) found->second = std::min(found->second, n);
                }
                column = end;
            }
        }
        // Pins bypass source windows (including cells absent from the index).
        for (const CellCoord& cell : pinned_)
            if (loaded_.count(cell) == 0 && failed_.count(cell) == 0) candidates[cell] = 0.0f;
        for (const auto& [cell, n] : candidates) wanted.push_back({n, cell});
    }
    std::sort(wanted.begin(), wanted.end());
    pending_.clear();
    if (executor_) {
        UpdateAsync(wanted);
        return;
    }
    usize loads = 0;
    for (const auto& [n, cell] : wanted) {
        if (loads < max_loads_per_update) {
            LoadCell(cell);
            ++loads;
        } else {
            pending_.push_back(cell);
        }
    }
}

void WorldStreamer::UpdateAsync(const std::vector<std::pair<f32, CellCoord>>& wanted) {
    if (!shared_) shared_ = std::make_shared<Shared>();
    const auto start = std::chrono::steady_clock::now();
    std::set<CellCoord> want;
    for (const auto& [n, cell] : wanted) want.insert(cell);

    // Apply finished reads, nearest first, within the budgets; drop the ones nobody wants any more.
    std::map<CellCoord, Shared::Result> ready;
    {
        std::lock_guard lock(shared_->mutex);
        ready.swap(shared_->ready);
    }
    usize applied = 0, entities = 0;
    std::vector<std::pair<CellCoord, Shared::Result>> later;
    for (const auto& [n, cell] : wanted) {
        auto it = ready.find(cell);
        if (it == ready.end()) continue;
        const bool over_entities = max_entities_per_update != 0 && entities >= max_entities_per_update;
        const bool over_time = apply_budget_ms > 0.0 && applied > 0 &&
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count() >= apply_budget_ms;
        if (applied >= max_loads_per_update || over_entities || over_time) {
            later.emplace_back(cell, std::move(it->second));
        } else if (!it->second.ok) {
            failed_[cell] = CellState::Failed;
            problems_.push_back("cell " + CellName(cell) + " couldn't be read");
        } else if (ApplyCell(cell, it->second.bytes)) {
            entities += events_.back().entities;
            ++applied;
        }
        ready.erase(it);
    }
    {
        // Unapplied results wait for the next Update; unwanted ones are discarded with `ready`.
        std::lock_guard lock(shared_->mutex);
        for (auto& [cell, result] : later) shared_->ready[cell] = std::move(result);
    }

    // Request more reads for the nearest wanted cells that are not requested yet.
    for (const auto& [n, cell] : wanted) {
        if (loaded_.count(cell) != 0 || failed_.count(cell) != 0) continue;
        bool request = false;
        {
            std::lock_guard lock(shared_->mutex);
            if (shared_->ready.count(cell) != 0 || shared_->inflight.count(cell) != 0) continue;
            if (shared_->inflight.size() + shared_->ready.size() < max_inflight_reads) {
                shared_->inflight.insert(cell);
                request = true;
            }
        }
        if (!request) {
            pending_.push_back(cell);
            continue;
        }
        executor_([shared = shared_, loader = loader_, cell] {
            Shared::Result result;
            result.ok = loader && loader(cell, result.bytes);
            std::lock_guard lock(shared->mutex);
            shared->inflight.erase(cell);
            shared->ready[cell] = std::move(result);
        });
    }
    for (const auto& [cell, result] : later) pending_.push_back(cell);
}

bool FloatingOrigin::Update(World& world, const Vec3& focus, const GuidIndex* guids) {
    last_shift = Vec3();
    if (std::sqrt(focus.x * focus.x + focus.z * focus.z) <= threshold || step <= 0.0f) return false;
    const Vec3 shift(std::round(focus.x / step) * step, 0.0f, std::round(focus.z / step) * step);
    if (shift.x == 0.0f && shift.z == 0.0f) return false;
    // Root Transforms move; children are relative to their parents.
    RebaseTransforms(world, Vec3d(shift.x, shift.y, shift.z), guids);
    offset.x += shift.x;
    offset.z += shift.z;
    last_shift = shift;
    return true;
}

} // namespace aether::streaming
