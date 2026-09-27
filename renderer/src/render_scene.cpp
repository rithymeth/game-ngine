#include "aether/renderer/render_scene.h"

#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace aether {

namespace {

Vec3 Point(const Mat4& m, const Vec3& p) {
    const Vec4 r = m * Vec4(p.x, p.y, p.z, 1.0f);
    return Vec3(r.x, r.y, r.z);
}
Vec3 Direction(const Mat4& m, const Vec3& d) {
    const Vec4 r = m * Vec4(d.x, d.y, d.z, 0.0f);
    return Vec3(r.x, r.y, r.z).Normalized();
}

u64 Fnv1a(const void* data, usize size, u64 hash = 1469598103934665603ull) {
    const u8* bytes = static_cast<const u8*>(data);
    for (usize i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

// Same mesh, same key: the model's GUID, or its path for data from before GUIDs.
u64 MeshKey(const ModelRenderer& mesh) {
    if (mesh.model.IsSet()) return Fnv1a(&mesh.model.guid, sizeof(mesh.model.guid));
    return Fnv1a(mesh.asset_path, std::char_traits<char>::length(mesh.asset_path));
}

template <typename T>
std::vector<Entity> EntitiesWith(const World& world) {
    std::vector<Entity> out;
    world.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<T>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            out.insert(out.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    std::sort(out.begin(), out.end(), [](Entity a, Entity b) { return a.index < b.index; });
    return out;
}

f32 Lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

} // namespace

Aabb Aabb::Transformed(const Mat4& m) const {
    // Arvo's method: the new extents are |M| times the old ones.
    const Vec3 c = Point(m, Center());
    const Vec3 e = Extents();
    Vec3 out_e(0, 0, 0);
    const f32* ext = &e.x;
    f32* oe = &out_e.x;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const Vec4& column = m.cols[col];
            const f32 v = row == 0 ? column.x : row == 1 ? column.y : column.z;
            oe[row] += std::fabs(v) * ext[col];
        }
    }
    return {c - out_e, c + out_e};
}

RenderScene ExtractRenderScene(const World& world, const GuidIndex& guids, const ExtractOptions& options) {
    RenderScene scene;
    auto active = [&](Entity e) { return IsActiveInHierarchy(world, guids, e); };
    for (Entity e : EntitiesWith<ModelRenderer>(world)) {
        if (!active(e)) continue;
        const ModelRenderer& mesh = *world.GetComponent<ModelRenderer>(e);
        Aabb local{Vec3(-0.5f, -0.5f, -0.5f), Vec3(0.5f, 0.5f, 0.5f)};
        if (options.mesh_bounds && !options.mesh_bounds(mesh, local)) local = Aabb{Vec3(-0.5f, -0.5f, -0.5f), Vec3(0.5f, 0.5f, 0.5f)};
        RenderObject object;
        object.entity = e;
        object.world = ComputeWorldTransform(world, guids, e);
        object.bounds = local.Transformed(object.world);
        object.mesh_key = MeshKey(mesh);
        scene.objects.push_back(object);
    }
    for (Entity e : EntitiesWith<DirectionalLight>(world)) {
        if (!active(e)) continue;
        const DirectionalLight& l = *world.GetComponent<DirectionalLight>(e);
        const Mat4 m = ComputeWorldTransform(world, guids, e);
        scene.directional_lights.push_back({e, Direction(m, Vec3(0, 0, -1)), l.color * l.intensity, l.cast_shadows});
    }
    for (Entity e : EntitiesWith<PointLight>(world)) {
        if (!active(e)) continue;
        const PointLight& l = *world.GetComponent<PointLight>(e);
        const Mat4 m = ComputeWorldTransform(world, guids, e);
        scene.point_lights.push_back({e, Point(m, Vec3(0, 0, 0)), l.color * l.intensity, std::max(l.range, 0.0f), l.cast_shadows});
    }
    for (Entity e : EntitiesWith<SpotLight>(world)) {
        if (!active(e)) continue;
        const SpotLight& l = *world.GetComponent<SpotLight>(e);
        const Mat4 m = ComputeWorldTransform(world, guids, e);
        const f32 outer = std::clamp(l.outer_angle_degrees, 0.0f, 89.0f);
        const f32 inner = std::clamp(l.inner_angle_degrees, 0.0f, outer);
        scene.spot_lights.push_back({e, Point(m, Vec3(0, 0, 0)), Direction(m, Vec3(0, 0, -1)), l.color * l.intensity,
                                     std::max(l.range, 0.0f), std::cos(Radians(inner)), std::cos(Radians(outer)),
                                     l.cast_shadows});
    }
    for (Entity e : EntitiesWith<SkyLight>(world)) {
        if (!active(e)) continue;
        const SkyLight& l = *world.GetComponent<SkyLight>(e);
        scene.sky_radiance = scene.sky_radiance + l.color * l.intensity;
    }
    for (Entity e : EntitiesWith<PostProcessVolume>(world)) {
        if (!active(e)) continue;
        scene.post_volumes.push_back({Point(ComputeWorldTransform(world, guids, e), Vec3(0, 0, 0)), *world.GetComponent<PostProcessVolume>(e)});
    }
    return scene;
}

PostProcessSettings BlendPostProcess(const RenderScene& scene, const Vec3& position) {
    std::vector<const RenderPostVolume*> order;
    for (const RenderPostVolume& v : scene.post_volumes) order.push_back(&v);
    std::stable_sort(order.begin(), order.end(),
                     [](const RenderPostVolume* a, const RenderPostVolume* b) { return a->settings.priority < b->settings.priority; });
    PostProcessSettings out;
    for (const RenderPostVolume* v : order) {
        const PostProcessVolume& s = v->settings;
        f32 w = std::clamp(s.weight, 0.0f, 1.0f);
        if (!s.global) {
            // Distance from the point to the box (0 inside), faded over blend_distance.
            const Vec3 d(std::max(0.0f, std::fabs(position.x - v->position.x) - s.extents.x),
                         std::max(0.0f, std::fabs(position.y - v->position.y) - s.extents.y),
                         std::max(0.0f, std::fabs(position.z - v->position.z) - s.extents.z));
            const f32 outside = d.Length();
            w *= s.blend_distance > 0.0f ? std::clamp(1.0f - outside / s.blend_distance, 0.0f, 1.0f) : (outside > 0.0f ? 0.0f : 1.0f);
        }
        if (w <= 0.0f) continue;
        out.exposure_compensation = Lerp(out.exposure_compensation, s.exposure_compensation, w);
        out.bloom_intensity = Lerp(out.bloom_intensity, s.bloom_intensity, w);
        out.vignette = Lerp(out.vignette, s.vignette, w);
        out.saturation = Lerp(out.saturation, s.saturation, w);
        if (w >= 0.5f) out.tonemapper = s.tonemapper; // a choice, not a blend
    }
    return out;
}

} // namespace aether
