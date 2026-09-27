#pragma once

#include "aether/ecs/world.h"
#include "aether/renderer/components.h"
#include "aether/renderer/material_instance.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"

#include <functional>
#include <vector>

namespace aether {

// An axis-aligned box.
struct Aabb {
    Vec3 min{0, 0, 0}, max{0, 0, 0};
    Vec3 Center() const { return (min + max) * 0.5f; }
    Vec3 Extents() const { return (max - min) * 0.5f; }
    // The box around this one after `m` (rotation, translation, scale).
    Aabb Transformed(const Mat4& m) const;
};

// One thing to draw. `mesh_key` groups identical meshes for instancing.
struct RenderObject {
    Entity entity;
    Mat4 world;
    Aabb bounds; // world space
    u64 mesh_key = 0;
    // The material it draws with: a hash of MaterialParameters::material, 0
    // for the model's own. Its per-object overrides, if any, are
    // RenderScene::material_parameters[parameters].
    u64 material_key = 0;
    i32 parameters = -1;
    bool cast_shadows = true;
};

struct RenderDirectionalLight {
    Entity entity;
    Vec3 direction{0, -1, 0}; // the way the light travels
    Vec3 radiance{1, 1, 1};   // color * intensity
    bool cast_shadows = true;
};
struct RenderPointLight {
    Entity entity;
    Vec3 position{0, 0, 0};
    Vec3 radiance{1, 1, 1};
    f32 range = 10.0f;
    bool cast_shadows = false;
};
struct RenderSpotLight {
    Entity entity;
    Vec3 position{0, 0, 0};
    Vec3 direction{0, 0, -1};
    Vec3 radiance{1, 1, 1};
    f32 range = 10.0f;
    f32 cos_inner = 0.94f, cos_outer = 0.87f;
    bool cast_shadows = false;
};

// Post-processing volume data, blended per view (BlendPostProcess).
struct RenderPostVolume {
    Vec3 position{0, 0, 0};
    PostProcessVolume settings;
};
struct PostProcessSettings {
    f32 exposure_compensation = 0.0f;
    f32 bloom_intensity = 0.05f;
    f32 vignette = 0.0f;
    f32 saturation = 1.0f;
    Tonemapper tonemapper = Tonemapper::ACES;
};

// What the renderer draws, copied from the ECS each frame (Phase 14 §14.1)
// so the game can move on while it renders.
struct RenderScene {
    std::vector<RenderObject> objects;
    std::vector<RenderDirectionalLight> directional_lights;
    std::vector<RenderPointLight> point_lights;
    std::vector<RenderSpotLight> spot_lights;
    Vec3 sky_radiance{0, 0, 0};
    std::vector<RenderPostVolume> post_volumes;
    std::vector<MaterialParameters> material_parameters; // copies, for RenderObject::parameters
};

struct ExtractOptions {
    // A mesh's local bounds, for culling. Without one, objects get a unit cube.
    std::function<bool(const ModelRenderer& mesh, Aabb& local_bounds)> mesh_bounds;
};

// Every active entity (Active in its hierarchy) with a ModelRenderer or a
// light, with world transforms through the hierarchy. In entity order.
RenderScene ExtractRenderScene(const World& world, const GuidIndex& guids, const ExtractOptions& options = {});

// The post-processing settings at `position`: volumes in priority order,
// each blended in by its weight (and, if bounded, by how far outside it the
// point is).
PostProcessSettings BlendPostProcess(const RenderScene& scene, const Vec3& position);

} // namespace aether
