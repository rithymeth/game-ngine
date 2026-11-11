#include "frame_lights.h"

#include "aether/renderer/components.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace aether::player {

namespace {

constexpr char kFrameDataHlsl[] = R"(
// Everything a frame shares (filled by player/src/frame_lights.cpp: keep the two layouts in step).
struct FrameData {
    float4 camera_world;
    float4 sun_direction;
    float4 sun_color;
    float4 sky_color;
    uint4 counts;
    float4 point_position_range[8];
    float4 point_color[8];
    float4 spot_position_range[4];
    float4 spot_direction_outer[4];
    float4 spot_color_inner[4];
    float4x4 shadow_view_projection[4];
    float4 shadow_cascade[4]; // x, y: origin in the atlas (texels); z: world units per texel; w: 1 / depth range
    uint4 shadow_info;        // x: the atlas's bindless index (255: none); y: cascades; z: texels per cascade side
    float4 sky_sh[9];         // the sky's irradiance, SH coefficients (rgb)
    uint4 environment_info;   // x: the prefiltered environment's bindless index (255: none); y: mip levels; z: level 0's height
    float4 cluster_forward;   // xyz: the camera's forward axis; w: depth scale
    float4 cluster_scale;     // x, y: tiles per pixel; z: depth bias
    uint4 cluster_info;       // x: the cells' byte offset; y: the index list's; z: lights; w: 1 when the clustered lights are in use
    float4 vsm_light_right;   // the sun's virtual shadow map (player/src/virtual_shadow.h): the light's basis,
    float4 vsm_light_up;
    float4 vsm_light_direction;
    float4 vsm_camera;        // xyz: the camera; w: a pixel's footprint per metre of distance
    int4 vsm_window[3];       // the window's first page per level, two levels to a row
    uint4 vsm_info;           // x: the page atlas's bindless index (255: none); y: pages per side of the pool; z: the page table's byte offset
    float4 vsm_params;        // x: a footprint that does not depend on distance
    float4 time;              // x: the game's time in seconds
    float4 wind;              // xy: the wind's direction in the world (x, z); z: strength; w: gust speed
    float4 wind_gusts;        // x: gustiness
    uint4 reflection;         // x: the water reflection's bindless index (255: none)
    float4 wind_zones[7];     // the wind zones: xy direction (x, z), z strength, w gust speed
};
ConstantBuffer<FrameData> g_Frame : register(b0, space2);

// Clustered lights (player/src/light_clusters.h): 48 bytes a light, from offset 0; per cell, uint2 (first index, count); the index list,
// 16 bits an entry.
ByteAddressBuffer g_FrameLights : register(t0, space3);

// A world position's depth along the camera's view.
float ClusterViewDepth(float3 world_position) { return dot(world_position - g_Frame.camera_world.xyz, g_Frame.cluster_forward.xyz); }

// The run of the index list for the cell holding pixel `pixel` (SV_Position.xy) at view depth `depth`: x the first index, y how many.
// The grid is 16 x 9 tiles by 24 slices (kClusterTilesX, kClusterTilesY, kClusterSlices).
uint2 ClusterRange(float2 pixel, float depth) {
    uint2 tile = min(uint2(max(pixel * g_Frame.cluster_scale.xy, 0.0)), uint2(15, 8));
    uint slice = (uint)clamp(floor(log(max(depth, 1e-6)) * g_Frame.cluster_forward.w + g_Frame.cluster_scale.z), 0.0, 23.0);
    uint cell = (slice * 9 + tile.y) * 16 + tile.x;
    return g_FrameLights.Load2(g_Frame.cluster_info.x + cell * 8);
}

// The index list's i-th entry: a light's position in the light array.
uint ClusterLightIndex(uint i) { return (g_FrameLights.Load(g_Frame.cluster_info.y + (i >> 1) * 4) >> ((i & 1) * 16)) & 0xFFFF; }

// The environment's radiance from `direction` at mip `lod` (0 sharp; level i prefiltered for roughness i / (levels - 1)). Equirectangular:
// u = 0.5 looks down -Z and grows toward +X, v = 0 is up. v stays half a texel clear of the poles (the sampler wraps).
float3 EnvironmentRadiance(float3 direction, float lod) {
    direction = normalize(direction);
    float u = atan2(direction.x, -direction.z) * 0.15915494 + 0.5;
    float v = acos(clamp(direction.y, -1.0, 1.0)) * 0.31830989;
    float rows = max(1.0, (float)(g_Frame.environment_info.z >> (uint)ceil(lod)));
    v = clamp(v, 0.5 / rows, 1.0 - 0.5 / rows);
    return g_Textures[g_Frame.environment_info.x].SampleLevel(g_Sampler, float2(u, v), lod).rgb;
}

// The sky's irradiance arriving at a surface facing `n` (from the SH coefficients; the cosine lobe is folded in).
float3 SkyIrradiance(float3 n) {
    float3 e = g_Frame.sky_sh[0].rgb * 0.282095
             + g_Frame.sky_sh[1].rgb * (0.488603 * n.y) + g_Frame.sky_sh[2].rgb * (0.488603 * n.z) + g_Frame.sky_sh[3].rgb * (0.488603 * n.x)
             + g_Frame.sky_sh[4].rgb * (1.092548 * n.x * n.y) + g_Frame.sky_sh[5].rgb * (1.092548 * n.y * n.z)
             + g_Frame.sky_sh[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0)) + g_Frame.sky_sh[7].rgb * (1.092548 * n.x * n.z)
             + g_Frame.sky_sh[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
    return max(e, 0.0);
}

// The split-sum environment BRDF, analytic (Karis 2014, "Physically Based Shading on Mobile"): scale and bias applied to F0.
float2 EnvironmentBrdf(float roughness, float n_dot_v) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

// The sky's light on a surface, before the SkyLight's tint: Lambert diffuse from the irradiance, and the prefiltered reflection at the
// roughness's mip times the environment BRDF. Ambient occlusion darkens the diffuse, and the specular through specular occlusion
// (Lagarde and de Rousiers 2014).
float3 SkyLighting(float3 n, float3 v, float3 base_color, float metallic, float roughness, float3 f0, float occlusion) {
    float n_dot_v = saturate(dot(n, v)) + 1e-4;
    float levels = (float)max(g_Frame.environment_info.y, 1u);
    float3 reflected = EnvironmentRadiance(reflect(-v, n), roughness * (levels - 1.0));
    float2 brdf = EnvironmentBrdf(roughness, n_dot_v);
    float specular_occlusion = saturate(pow(n_dot_v + occlusion, exp2(-16.0 * roughness - 1.0)) - 1.0 + occlusion);
    float3 diffuse = SkyIrradiance(n) * base_color * (1.0 - metallic) * (0.31830989 * occlusion);
    return diffuse + reflected * (f0 * brdf.x + brdf.y) * specular_occlusion;
}
)";

} // namespace

const char* FrameDataHlsl() { return kFrameDataHlsl; }

static_assert(kClusterTilesX == 16 && kClusterTilesY == 9 && kClusterSlices == 24, "ClusterRange in kFrameDataHlsl spells out the grid");

namespace {

constexpr usize kReservedSceneLights = 4; // see AddParticleLights
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

Camera LimitViewDistance(const Camera& camera, f32 view_distance) {
    Camera limited = camera;
    if (camera.projection == Projection::Perspective && view_distance > 0.0f && view_distance < camera.far_plane) {
        limited.far_plane = std::max(view_distance, camera.near_plane * 2.0f);
    }
    return limited;
}

const SkyLight* FindSkyLight(const World& world) {
    for (const Entity e : EntitiesWith<SkyLight>(world)) {
        if (const SkyLight* light = world.GetComponent<SkyLight>(e)) return light;
    }
    return nullptr;
}

const Wind* FindWind(const World& world) {
    for (const Entity e : EntitiesWith<Wind>(world)) {
        if (const Wind* wind = world.GetComponent<Wind>(e)) return wind;
    }
    return nullptr;
}

std::vector<WindZoneSite> CollectWindZones(const World& world, const GuidIndex& guids) {
    std::vector<WindZoneSite> sites;
    for (const Entity e : EntitiesWith<WindZone>(world)) {
        if (sites.size() >= kMaxWindZones) break;
        const WindZone* zone = world.GetComponent<WindZone>(e);
        if (zone == nullptr) continue;
        WindZoneSite site;
        if (world.HasComponent<Transform>(e)) site.center = Vec3(ComputeWorldTransform(world, guids, e).cols[3].x, ComputeWorldTransform(world, guids, e).cols[3].y, ComputeWorldTransform(world, guids, e).cols[3].z);
        site.radius = std::isfinite(zone->radius) ? std::clamp(zone->radius, 0.5f, 2000.0f) : 20.0f;
        site.falloff = std::isfinite(zone->falloff) ? std::clamp(zone->falloff, 0.0f, 1.0f) : 0.5f;
        sites.push_back(site);
    }
    return sites;
}

WindSample WindForDraw(const FrameData& frame, u32 zone_bits) {
    WindSample out;
    out.direction[0] = frame.wind[0];
    out.direction[1] = frame.wind[1];
    out.strength = frame.wind[2];
    out.gust_speed = frame.wind[3];
    const u32 slot = zone_bits & 7u;
    if (slot != 0 && slot <= kMaxWindZones) {
        const f32 blend = static_cast<f32>((zone_bits >> 3) & 31u) / 31.0f;
        const f32* zone = frame.wind_zones[slot - 1];
        const f32 bx = frame.wind[0] * frame.wind[2] + (zone[0] * zone[2] - frame.wind[0] * frame.wind[2]) * blend;
        const f32 bz = frame.wind[1] * frame.wind[2] + (zone[1] * zone[2] - frame.wind[1] * frame.wind[2]) * blend;
        out.strength = std::sqrt(bx * bx + bz * bz);
        if (out.strength > 1e-5f) { out.direction[0] = bx / out.strength; out.direction[1] = bz / out.strength; }
        out.gust_speed = frame.wind[3] + (zone[3] - frame.wind[3]) * blend;
    }
    return out;
}

u32 WindZoneBitsAt(const std::vector<WindZoneSite>& zones, const Vec3& position) {
    u32 best = 0;
    f32 best_weight = 0.0f;
    for (usize i = 0; i < zones.size() && i < kMaxWindZones; ++i) {
        const WindZoneSite& z = zones[i];
        const Vec3 d = position - z.center;
        const f32 distance = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (!(distance < z.radius)) continue;
        const f32 fade = z.radius * z.falloff;                       // metres from the edge over which it fades
        const f32 weight = fade <= 0.0f ? 1.0f : std::clamp((z.radius - distance) / fade, 0.0f, 1.0f);
        if (weight > best_weight) { best_weight = weight; best = static_cast<u32>(i) + 1u; }
    }
    if (best == 0) return 0;
    return best | (static_cast<u32>(std::lround(best_weight * 31.0f)) << 3);
}

FrameData BuildFrameData(const World& world, const GuidIndex& guids, const Vec3& camera_position) {
    FrameData data;
    Set4(data.camera_world, camera_position.x, camera_position.y, camera_position.z, 1.0f);
    // What the scene shader used before it had lights: a key light from above and to one side, and a sky fill.
    const Vec3 default_sun = Normalized(Vec3(-0.4f, 0.8f, -0.35f), Vec3(0, 1, 0));
    Set4(data.sun_direction, default_sun.x, default_sun.y, default_sun.z, 0.0f);
    Set4(data.sun_color, 3.0f, 3.0f, 3.0f, 0.0f);
    Set4(data.sky_color, 1.0f, 1.0f, 1.0f, 0.0f);
    // The wind: a breeze from 27 degrees unless the world has a Wind. A direction and the numbers are cleaned, so a bad value cannot throw a shader off.
    {
        Wind wind;
        if (const Wind* set = FindWind(world)) wind = *set;
        const auto finite = [](f32 v, f32 fallback) { return std::isfinite(v) ? v : fallback; };
        const f32 radians = finite(wind.direction_degrees, 27.0f) * 3.14159265f / 180.0f;
        Set4(data.wind, std::cos(radians), std::sin(radians), std::clamp(finite(wind.strength, 1.0f), 0.0f, 4.0f), std::clamp(finite(wind.gust_speed, 1.0f), 0.1f, 4.0f));
        Set4(data.wind_gusts, std::clamp(finite(wind.gustiness, 0.4f), 0.0f, 1.0f), 0.0f, 0.0f, 0.0f);
        u32 slot = 0;
        for (const Entity e : EntitiesWith<WindZone>(world)) {
            if (slot >= kMaxWindZones) break;
            const WindZone* zone = world.GetComponent<WindZone>(e);
            if (zone == nullptr) continue;
            const f32 zr = finite(zone->direction_degrees, 27.0f) * 3.14159265f / 180.0f;
            Set4(data.wind_zones[slot++], std::cos(zr), std::sin(zr), std::clamp(finite(zone->strength, 1.0f), 0.0f, 4.0f), std::clamp(finite(zone->gust_speed, 1.0f), 0.1f, 4.0f));
        }
    }

    for (const Entity e : EntitiesWith<DirectionalLight>(world)) {
        const DirectionalLight* light = world.GetComponent<DirectionalLight>(e);
        if (light == nullptr) continue;
        Vec3 to_light = default_sun;
        if (world.HasComponent<Transform>(e)) to_light = BackAxis(ComputeWorldTransform(world, guids, e), default_sun);
        Set4(data.sun_direction, to_light.x, to_light.y, to_light.z, 1.0f);
        Set4(data.sun_color, light->color.x * light->intensity, light->color.y * light->intensity, light->color.z * light->intensity, 1.0f);
        data.counts[2] = light->cast_shadows ? 1u : 0u;
        break;
    }
    if (const SkyLight* light = FindSkyLight(world)) {
        Set4(data.sky_color, light->color.x * light->intensity, light->color.y * light->intensity, light->color.z * light->intensity, 1.0f);
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

void AddParticleLights(FrameData& data, std::span<const vfx::ParticleLight> lights, const Vec3& camera_position) {
    struct Candidate {
        f32 distance_squared;
        f32 position_range[4];
        f32 color[4];
    };
    std::vector<Candidate> pool;
    const auto distance_squared = [&](f32 x, f32 y, f32 z) {
        const f32 dx = x - camera_position.x, dy = y - camera_position.y, dz = z - camera_position.z;
        return dx * dx + dy * dy + dz * dz;
    };
    for (u32 i = 0; i < data.counts[0] && i < kMaxPointLights; ++i) {
        Candidate c{distance_squared(data.point_position_range[i][0], data.point_position_range[i][1], data.point_position_range[i][2]), {}, {}};
        std::copy(std::begin(data.point_position_range[i]), std::end(data.point_position_range[i]), std::begin(c.position_range));
        std::copy(std::begin(data.point_color[i]), std::end(data.point_color[i]), std::begin(c.color));
        pool.push_back(c);
    }
    const usize scene_lights = pool.size();
    std::vector<Candidate> particles;
    for (const vfx::ParticleLight& light : lights) {
        const f32 energy = light.intensity;
        if (!(light.radius > 0.0f) || !(energy > 0.0f)) continue; // (also drops NaN)
        Candidate c{distance_squared(light.position.x, light.position.y, light.position.z), {}, {}};
        Set4(c.position_range, light.position.x, light.position.y, light.position.z, light.radius);
        Set4(c.color, light.color.r * energy, light.color.g * energy, light.color.b * energy, 0.0f);
        particles.push_back(c);
    }
    if (particles.empty()) return; // nothing to add: leave the scene's lights as they are
    // The scene's lights keep up to kReservedSceneLights slots however many particle lights there are (a fire's sparks must not push out the
    // lamps); the particles' nearest lights take the rest, and the scene's take whatever the particles leave.
    const auto nearer = [](const Candidate& l, const Candidate& r) { return l.distance_squared < r.distance_squared; };
    std::stable_sort(particles.begin(), particles.end(), nearer);
    const usize reserved = std::min(scene_lights, kReservedSceneLights);
    const usize particle_keep = std::min(particles.size(), kMaxPointLights - reserved);
    const usize scene_keep = std::min(scene_lights, kMaxPointLights - particle_keep);
    pool.resize(scene_keep);         // the scene's lights were gathered nearest first, so the nearest stay
    pool.insert(pool.end(), particles.begin(), particles.begin() + static_cast<std::ptrdiff_t>(particle_keep));
    std::stable_sort(pool.begin(), pool.end(), nearer);
    for (usize i = 0; i < pool.size(); ++i) {
        std::copy(std::begin(pool[i].position_range), std::end(pool[i].position_range), std::begin(data.point_position_range[i]));
        std::copy(std::begin(pool[i].color), std::end(pool[i].color), std::begin(data.point_color[i]));
    }
    data.counts[0] = static_cast<u32>(pool.size());
}

std::vector<ClusterLight> GatherClusterLights(const World& world, const GuidIndex& guids, std::span<const vfx::ParticleLight> particles) {
    std::vector<ClusterLight> out;
    for (const Entity e : EntitiesWith<PointLight>(world)) {
        const PointLight* light = world.GetComponent<PointLight>(e);
        if (light == nullptr || !(light->range > 0.0f) || !world.HasComponent<Transform>(e)) continue;
        const Vec3 p = Position(ComputeWorldTransform(world, guids, e));
        out.push_back(MakePointClusterLight(p, light->range, light->color * light->intensity));
    }
    for (const Entity e : EntitiesWith<SpotLight>(world)) {
        const SpotLight* light = world.GetComponent<SpotLight>(e);
        if (light == nullptr || !(light->range > 0.0f) || !world.HasComponent<Transform>(e)) continue;
        const Mat4 transform = ComputeWorldTransform(world, guids, e);
        const Vec3 back = BackAxis(transform, Vec3(0, 0, 1));
        out.push_back(MakeSpotClusterLight(Position(transform), light->range, light->color * light->intensity, back * -1.0f,
                                           light->outer_angle_degrees, light->inner_angle_degrees));
    }
    for (const vfx::ParticleLight& light : particles) {
        if (!(light.radius > 0.0f) || !(light.intensity > 0.0f)) continue;
        out.push_back(MakePointClusterLight(light.position, light.radius,
                                            Vec3(light.color.r, light.color.g, light.color.b) * light.intensity));
    }
    return out;
}

void FillClusterData(FrameData& data, const LightClusters& clusters, const ClusterView& view, u32 width, u32 height) {
    const Vec3 forward = Normalized(Vec3(-view.view.cols[0].z, -view.view.cols[1].z, -view.view.cols[2].z), Vec3(0, 0, -1));
    Set4(data.cluster_forward, forward.x, forward.y, forward.z, clusters.depth_scale);
    Set4(data.cluster_scale, static_cast<f32>(kClusterTilesX) / static_cast<f32>(std::max(width, 1u)),
         static_cast<f32>(kClusterTilesY) / static_cast<f32>(std::max(height, 1u)), clusters.depth_bias, 0.0f);
    data.cluster_info[0] = clusters.cells_offset;
    data.cluster_info[1] = clusters.indices_offset;
    data.cluster_info[2] = clusters.light_count;
    data.cluster_info[3] = 1u;
}

} // namespace aether::player
