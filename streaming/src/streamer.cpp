#include "aether/streaming/streamer.h"

#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_set>

namespace aether::streaming {

namespace {
std::vector<Entity> AllEntities(const World& world) {
    std::vector<Entity> out;
    world.ForEachArchetype([&](Archetype& archetype) {
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            out.insert(out.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    return out;
}
u64 Key(Entity e) { return (static_cast<u64>(e.generation) << 32) | e.index; }
const std::vector<Entity> kNone;
} // namespace

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
    std::unordered_set<u64> before;
    for (Entity e : AllEntities(world_)) before.insert(Key(e));
    if (!LoadSceneFromMemory(world_, bytes, "cell " + CellName(cell))) {
        failed_[cell] = CellState::Failed;
        problems_.push_back("cell " + CellName(cell) + " isn't a scene");
        return false;
    }
    std::vector<Entity>& added = loaded_[cell];
    for (Entity e : AllEntities(world_))
        if (before.count(Key(e)) == 0) added.push_back(e);
    // Cells are saved in whole-world positions; Transforms are local to the floating origin.
    const Vec3 shift(static_cast<f32>(offset_.x), static_cast<f32>(offset_.y), static_cast<f32>(offset_.z));
    for (Entity e : added) {
        if (Transform* t = world_.GetComponent<Transform>(e); t != nullptr && world_.GetComponent<Parent>(e) == nullptr) t->position = t->position - shift;
        if (guids_ != nullptr)
            if (const IdComponent* id = world_.GetComponent<IdComponent>(e)) guids_->Add(id->guid, e);
    }
    events_.push_back({cell, true, added.size()});
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
    // Load the nearest wanted cells first.
    std::vector<std::pair<f32, CellCoord>> wanted;
    for (const CellInfo& info : index_.cells) {
        if (loaded_.count(info.coord) != 0 || failed_.count(info.coord) != 0) continue;
        const f32 n = nearness(info.coord);
        if (n <= 1.0f) wanted.push_back({n, info.coord});
    }
    for (const CellCoord& cell : pinned_) {
        if (index_.Find(cell) == nullptr && loaded_.count(cell) == 0 && failed_.count(cell) == 0) wanted.push_back({0.0f, cell});
    }
    std::sort(wanted.begin(), wanted.end());
    pending_.clear();
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

bool FloatingOrigin::Update(World& world, const Vec3& focus, const GuidIndex* guids) {
    last_shift = Vec3();
    if (std::sqrt(focus.x * focus.x + focus.z * focus.z) <= threshold || step <= 0.0f) return false;
    const Vec3 shift(std::round(focus.x / step) * step, 0.0f, std::round(focus.z / step) * step);
    if (shift.x == 0.0f && shift.z == 0.0f) return false;
    // Root Transforms move; children are relative to their parents.
    world.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<Transform>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* es = archetype.EntityArray(c);
            for (usize i = 0; i < archetype.ChunkEntityCount(c); ++i) {
                const Entity e = es[i];
                if (world.GetComponent<Parent>(e) != nullptr && (guids == nullptr || !GetParent(world, *guids, e).IsNull())) continue;
                Transform* t = world.GetComponent<Transform>(e);
                t->position = t->position - shift;
            }
        }
    });
    offset.x += shift.x;
    offset.z += shift.z;
    last_shift = shift;
    return true;
}

} // namespace aether::streaming
