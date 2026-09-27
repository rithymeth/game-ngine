#include "aether/scene/gameplay.h"

#include "aether/scene/hierarchy.h"

#include <algorithm>
#include <cmath>

namespace aether {

namespace {

std::vector<Entity> EntitiesWith(const World& world, ComponentId id) {
    std::vector<Entity> result;
    world.ForEachArchetype([&](const Archetype& archetype) {
        if (!archetype.Mask().test(id)) {
            return;
        }
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* entities = archetype.EntityArray(c);
            result.insert(result.end(), entities, entities + archetype.ChunkEntityCount(c));
        }
    });
    std::sort(result.begin(), result.end(), [](Entity a, Entity b) { return a.index < b.index; });
    return result;
}

} // namespace

bool IsActiveInHierarchy(const World& world, const GuidIndex& guids, Entity entity) {
    for (int depth = 0; world.IsAlive(entity) && depth <= kMaxHierarchyDepth; ++depth) {
        if (const Active* active = world.GetComponent<Active>(entity); active != nullptr && !active->active) {
            return false;
        }
        entity = GetParent(world, guids, entity);
    }
    return true;
}

Mat4 OrthographicRH(f32 width, f32 height, f32 z_near, f32 z_far) {
    Mat4 m;
    m.cols[0] = Vec4(2.0f / width, 0, 0, 0);
    m.cols[1] = Vec4(0, 2.0f / height, 0, 0);
    m.cols[2] = Vec4(0, 0, 1.0f / (z_near - z_far), 0);
    m.cols[3] = Vec4(0, 0, z_near / (z_near - z_far), 1.0f);
    return m;
}

Mat4 CameraProjection(const Camera& camera, f32 aspect) {
    aspect = aspect > 0.0f ? aspect : 1.0f;
    const f32 near_plane = std::max(camera.near_plane, 1e-4f);
    const f32 far_plane = std::max(camera.far_plane, near_plane * 1.001f);
    if (camera.projection == Projection::Orthographic) {
        const f32 height = std::max(camera.ortho_height, 1e-4f);
        return OrthographicRH(height * aspect, height, near_plane, far_plane);
    }
    const f32 fov = std::clamp(camera.fov_degrees, 1.0f, 179.0f) * 3.14159265358979f / 180.0f;
    return Mat4::PerspectiveRH(fov, aspect, near_plane, far_plane);
}

Mat4 CameraView(const World& world, const GuidIndex& guids, Entity entity) {
    const Mat4 w = ComputeWorldTransform(world, guids, entity);
    // [R | t]^-1 = [R^T | -R^T t]
    const Vec4& c0 = w.cols[0];
    const Vec4& c1 = w.cols[1];
    const Vec4& c2 = w.cols[2];
    const Vec4& t = w.cols[3];
    Mat4 view;
    view.cols[0] = Vec4(c0.x, c1.x, c2.x, 0);
    view.cols[1] = Vec4(c0.y, c1.y, c2.y, 0);
    view.cols[2] = Vec4(c0.z, c1.z, c2.z, 0);
    view.cols[3] = Vec4(-(c0.x * t.x + c0.y * t.y + c0.z * t.z), -(c1.x * t.x + c1.y * t.y + c1.z * t.z),
                        -(c2.x * t.x + c2.y * t.y + c2.z * t.z), 1.0f);
    return view;
}

Entity FindActiveCamera(const World& world, const GuidIndex& guids) {
    Entity best = kNullEntity;
    i32 best_priority = 0;
    for (Entity entity : EntitiesWith(world, GetComponentId<Camera>())) {
        if (!IsActiveInHierarchy(world, guids, entity)) {
            continue;
        }
        const i32 priority = world.GetComponent<Camera>(entity)->priority;
        if (best.IsNull() || priority > best_priority) {
            best = entity;
            best_priority = priority;
        }
    }
    return best;
}

bool HasTag(const World& world, Entity entity, std::string_view tag) {
    const Tags* tags = world.IsAlive(entity) ? world.GetComponent<Tags>(entity) : nullptr;
    return tags != nullptr && std::find(tags->names.begin(), tags->names.end(), tag) != tags->names.end();
}

bool AddTag(World& world, Entity entity, const std::string& tag) {
    if (HasTag(world, entity, tag)) {
        return false;
    }
    if (!world.HasComponent<Tags>(entity)) {
        world.AddComponent<Tags>(entity);
    }
    world.GetComponent<Tags>(entity)->names.push_back(tag);
    return true;
}

bool RemoveTag(World& world, Entity entity, std::string_view tag) {
    Tags* tags = world.IsAlive(entity) ? world.GetComponent<Tags>(entity) : nullptr;
    if (tags == nullptr) {
        return false;
    }
    auto it = std::find(tags->names.begin(), tags->names.end(), tag);
    if (it == tags->names.end()) {
        return false;
    }
    tags->names.erase(it);
    return true;
}

std::vector<Entity> FindEntitiesWithTag(const World& world, std::string_view tag) {
    std::vector<Entity> result;
    for (Entity entity : EntitiesWith(world, GetComponentId<Tags>())) {
        if (HasTag(world, entity, tag)) {
            result.push_back(entity);
        }
    }
    return result;
}

i32 FindLayer(const ProjectSettings& settings, std::string_view name) {
    for (usize i = 0; i < settings.layers.size() && i < kMaxLayers; ++i) {
        if (settings.layers[i] == name) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

LayerMask MakeLayerMask(const ProjectSettings& settings, const std::vector<std::string>& names,
                        std::vector<std::string>* unknown) {
    LayerMask mask = 0;
    for (const std::string& name : names) {
        const i32 index = FindLayer(settings, name);
        if (index < 0) {
            if (unknown != nullptr) {
                unknown->push_back(name);
            }
            continue;
        }
        mask |= 1u << index;
    }
    return mask;
}

u8 LayerOf(const World& world, Entity entity) {
    const Layer* layer = world.IsAlive(entity) ? world.GetComponent<Layer>(entity) : nullptr;
    return layer != nullptr && layer->index < kMaxLayers ? layer->index : 0;
}

void CollisionMatrix::Set(u8 a, u8 b, bool collide) {
    if (a >= kMaxLayers || b >= kMaxLayers) return;
    if (collide) {
        rows[a] |= 1u << b;
        rows[b] |= 1u << a;
    } else {
        rows[a] &= ~(1u << b);
        rows[b] &= ~(1u << a);
    }
}

CollisionMatrix MakeCollisionMatrix(const ProjectSettings& settings) {
    CollisionMatrix matrix;
    for (usize i = 0; i < settings.collision_matrix.size() && i < kMaxLayers; ++i) matrix.rows[i] = settings.collision_matrix[i];
    // Keep it symmetric even if the file isn't: a pair collides only if both rows say so.
    for (u8 a = 0; a < kMaxLayers; ++a) {
        for (u8 b = 0; b < kMaxLayers; ++b) {
            if (!matrix.ShouldCollide(b, a)) matrix.rows[a] &= ~(1u << b);
        }
    }
    return matrix;
}

bool SetLayersCollide(ProjectSettings& settings, u8 a, u8 b, bool collide) {
    const usize count = std::min<usize>(settings.layers.size(), kMaxLayers);
    if (a >= count || b >= count) return false;
    CollisionMatrix matrix = MakeCollisionMatrix(settings);
    matrix.Set(a, b, collide);
    // Store rows up to the last one that isn't "everything", so untouched projects stay empty.
    usize last = 0;
    for (usize i = 0; i < kMaxLayers; ++i) {
        if (matrix.rows[i] != kAllLayers) last = i + 1;
    }
    settings.collision_matrix.assign(matrix.rows.begin(), matrix.rows.begin() + static_cast<std::ptrdiff_t>(last));
    return true;
}

bool IsInLayerMask(const World& world, Entity entity, LayerMask mask) {
    return (mask & (1u << LayerOf(world, entity))) != 0;
}

} // namespace aether
