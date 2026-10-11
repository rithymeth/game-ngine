#include "frame_lights.h"

#include "aether/renderer/components.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace aether::player {

namespace {

constexpr f32 kDegreesToRadians = 3.14159265358979f / 180.0f;

Vec3 Normalized(const Vec3& v, const Vec3& fallback) {
    const f32 length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return length > 1e-6f ? Vec3(v.x / length, v.y / length, v.z / length) : fallback;
}

Vec3 Position(const Mat4& m) { return Vec3(m.cols[3].x, m.cols[3].y, m.cols[3].z); }

// The entity's +Z axis in the world: lights shine along -Z, so this is the way back toward the light.
Vec3 BackAxis(const Mat4& m, const Vec3& fallback) { return Normalized(Vec3(m.cols[2].x, m.cols[2].y, m.cols[2].z), fallback); }

void Set4(f32 (&out)[4], f32 x, f32 y, f32 z, f32 w) {
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

template <typename T>
std::vector<Entity> EntitiesWith(const World& world) {
    std::vector<Entity> out;
    world.ForEachArchetype([&](const Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<T>())) return;
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            const Entity* entities = archetype.EntityArray(chunk);
            out.insert(out.end(), entities, entities + archetype.ChunkEntityCount(chunk));
        }
    });
    return out;
}

} // namespace

FrameData BuildFrameData(const World& world, const GuidIndex& guids, const Vec3& camera_position) {
    FrameData data;
    Set4(data.camera_world, camera_position.x, camera_position.y, camera_position.z, 1.0f);
    // What the scene shader used before it had lights: a key light from above and to one side, and a sky fill.
    const Vec3 default_sun = Normalized(Vec3(-0.4f, 0.8f, -0.35f), Vec3(0, 1, 0));
    Set4(data.sun_direction, default_sun.x, default_sun.y, default_sun.z, 0.0f);
    Set4(data.sun_color, 3.0f, 3.0f, 3.0f, 0.0f);
    Set4(data.sky_color, 1.0f, 1.0f, 1.0f, 0.0f);

    for (const Entity e : EntitiesWith<DirectionalLight>(world)) {
        const DirectionalLight* light = world.GetComponent<DirectionalLight>(e);
        if (light == nullptr) continue;
        Vec3 to_light = default_sun;
        if (world.HasComponent<Transform>(e)) to_light = BackAxis(ComputeWorldTransform(world, guids, e), default_sun);
        Set4(data.sun_direction, to_light.x, to_light.y, to_light.z, 1.0f);
        Set4(data.sun_color, light->color.x * light->intensity, light->color.y * light->intensity, light->color.z * light->intensity, 1.0f);
        break;
    }
    for (const Entity e : EntitiesWith<SkyLight>(world)) {
        const SkyLight* light = world.GetComponent<SkyLight>(e);
        if (light == nullptr) continue;
        Set4(data.sky_color, light->color.x * light->intensity, light->color.y * light->intensity, light->color.z * light->intensity, 1.0f);
        break;
    }

    struct Near {
        f32 distance_squared;
        Entity entity;
        Mat4 transform;
    };
    const auto nearest = [&](const std::vector<Entity>& entities, usize limit, auto usable) {
        std::vector<Near> found;
        for (const Entity e : entities) {
            if (!world.HasComponent<Transform>(e) || !usable(e)) continue; // (a light with no range gives nothing: it must not use up a slot)
            const Mat4 transform = ComputeWorldTransform(world, guids, e);
            const Vec3 p = Position(transform);
            const f32 dx = p.x - camera_position.x, dy = p.y - camera_position.y, dz = p.z - camera_position.z;
            found.push_back({dx * dx + dy * dy + dz * dz, e, transform});
        }
        std::sort(found.begin(), found.end(), [](const Near& a, const Near& b) { return a.distance_squared < b.distance_squared; });
        if (found.size() > limit) found.resize(limit);
        return found;
    };

    u32 points = 0;
    for (const Near& n : nearest(EntitiesWith<PointLight>(world), kMaxPointLights, [&](Entity e) { const PointLight* l = world.GetComponent<PointLight>(e); return l != nullptr && l->range > 0.0f; })) {
        const PointLight* light = world.GetComponent<PointLight>(n.entity);
        if (light == nullptr || light->range <= 0.0f) continue;
        const Vec3 p = Position(n.transform);
        Set4(data.point_position_range[points], p.x, p.y, p.z, light->range);
        Set4(data.point_color[points], light->color.x * light->intensity, light->color.y * light->intensity, light->color.z * light->intensity, 0.0f);
        ++points;
    }
    u32 spots = 0;
    for (const Near& n : nearest(EntitiesWith<SpotLight>(world), kMaxSpotLights, [&](Entity e) { const SpotLight* l = world.GetComponent<SpotLight>(e); return l != nullptr && l->range > 0.0f; })) {
        const SpotLight* light = world.GetComponent<SpotLight>(n.entity);
        if (light == nullptr || light->range <= 0.0f) continue;
        const Vec3 p = Position(n.transform);
        const Vec3 back = BackAxis(n.transform, Vec3(0, 0, 1));
        const f32 outer = std::min(light->outer_angle_degrees, 89.0f), inner = std::min(light->inner_angle_degrees, outer);
        Set4(data.spot_position_range[spots], p.x, p.y, p.z, light->range);
        Set4(data.spot_direction_outer[spots], -back.x, -back.y, -back.z, std::cos(outer * kDegreesToRadians));
        Set4(data.spot_color_inner[spots], light->color.x * light->intensity, light->color.y * light->intensity, light->color.z * light->intensity,
             std::cos(inner * kDegreesToRadians));
        ++spots;
    }
    data.counts[0] = points;
    data.counts[1] = spots;
    return data;
}

} // namespace aether::player
