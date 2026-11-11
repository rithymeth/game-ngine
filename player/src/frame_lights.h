#pragma once

#include "aether/ecs/world.h"
#include "aether/math/math.h"
#include "aether/renderer/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/gameplay.h"
#include "aether/vfx/render.h"
#include "light_clusters.h"

#include <span>

// What the scene shader reads once a frame: the camera, the sun, the sky's ambient light and the nearest point and spot lights, gathered from the
// world's DirectionalLight, SkyLight, PointLight and SpotLight components (renderer/components.h). The layout is the HLSL struct `FrameData` in
// scene_renderer.cpp, read through the RHI's frame constants (a 16-byte-aligned block). A scene with no such components gets the values the shader
// always used (a fixed sun and sky), so games without lights look as they did.

namespace aether::player {

constexpr usize kMaxPointLights = 8;
constexpr usize kMaxSpotLights = 4;

struct FrameData {
    f32 camera_world[4]{};  // xyz: the camera's position in the world
    f32 sun_direction[4]{}; // xyz: the unit vector from a surface toward the sun; w: 1 when a DirectionalLight set it
    f32 sun_color[4]{};     // rgb: the sun's colour times its intensity
    f32 sky_color[4]{};     // rgb: the ambient light's colour times its intensity; w: 1 when a SkyLight set it
    u32 counts[4]{};        // x: point lights used, y: spot lights used, z: 1 when the sun is a DirectionalLight that casts shadows
    f32 point_position_range[kMaxPointLights][4]{}; // xyz position, w range (the light fades to nothing there)
    f32 point_color[kMaxPointLights][4]{};          // rgb colour times intensity
    f32 spot_position_range[kMaxSpotLights][4]{};   // xyz position, w range
    f32 spot_direction_outer[kMaxSpotLights][4]{};  // xyz: the way it points; w: cosine of the outer cone angle
    f32 spot_color_inner[kMaxSpotLights][4]{};      // rgb colour times intensity; w: cosine of the inner cone angle
    // The sun's shadow cascades (ShadowPass, player/src/shadow_pass.h): ShadowFrameData's layout.
    f32 shadow_view_projection[4][16]{}; // per cascade, column-major
    f32 shadow_cascade[4][4]{};          // x, y: origin in the atlas (texels); z: world units per texel; w: 1 / depth range
    u32 shadow_info[4]{255u, 0u, 0u, 0u}; // x: the atlas's bindless index (255: no shadows); y: cascades; z: texels per cascade side
    // The sky's image-based lighting (player/src/environment.h): the irradiance's SH coefficients (rgb), and the prefiltered environment.
    f32 sky_sh[9][4]{};
    u32 environment_info[4]{255u, 0u, 0u, 0u}; // x: the environment's bindless index (255: none); y: mip levels; z: level 0's height
    // Clustered lights (light_clusters.h), read from the frame read buffer g_FrameLights. When cluster_info.w is 0 the shader lights
    // with the nearest point and spot lights above instead.
    f32 cluster_forward[4]{}; // xyz: the camera's forward axis (a point's view depth is dot(point - camera, forward)); w: depth scale
    f32 cluster_scale[4]{};   // x, y: tiles per pixel across and down; z: depth bias (slice = floor(log(depth) * scale + bias))
    u32 cluster_info[4]{};    // x: the cells' byte offset in g_FrameLights; y: the index list's; z: lights binned; w: 1 when in use
    // The sun's virtual shadow map (virtual_shadow.h, vsm_pages.h): the light's basis, the camera and the window of pages around it, and the page
    // table in g_FrameLights. vsm_info.x is 255 when there is none and the cascades alone shadow the sun.
    f32 vsm_light_right[4]{};     // xyz
    f32 vsm_light_up[4]{};        // xyz
    f32 vsm_light_direction[4]{}; // xyz: the way the light travels
    f32 vsm_camera[4]{};          // xyz: the camera's position; w: a pixel's footprint per metre of distance
    i32 vsm_window[3][4]{};       // the window's first page, (x, y) for each level, two levels to a row: x0 y0 x1 y1 / x2 y2 x3 y3 / x4 y4 x5 y5
    u32 vsm_info[4]{255u, 0u, 0u, 0u}; // x: the page atlas's bindless index (255: none); y: pages per side of the pool; z: the page table's byte offset
    f32 vsm_params[4]{};          // x: a pixel's footprint that does not depend on distance (an orthographic camera's)
    f32 time[4]{};                // x: the game's time in seconds (the scene shader's water scrolls its ripples with it)
    f32 wind[4]{};                // xy: the wind's direction in the world (x, z), unit length; z: strength; w: gust speed (the vegetation's sway)
    f32 wind_gusts[4]{};          // x: gustiness
    f32 wind_zones[7][4]{};       // the WindZones (kMaxWindZones): xy direction (x, z), z strength, w gust speed; an object names its zone and how far inside it is
};
static_assert(sizeof(FrameData) % 16 == 0 && sizeof(FrameData) == 1376);

// The HLSL for FrameData (the layout above), its constant buffer g_Frame (register b0, space2), and the sky helpers that read it:
// EnvironmentRadiance(direction, lod), SkyIrradiance(normal) and SkyLighting(...), the split-sum image-based lighting. Needs g_Textures
// and g_Sampler declared before it.
const char* FrameDataHlsl();

// The camera as a quality preset's view distance (metres) draws it: the far plane pulled in to that distance, kept clear of the near plane. An
// orthographic camera and a distance of 0 or less are left alone, as is a far plane already nearer.
Camera LimitViewDistance(const Camera& camera, f32 view_distance);

// The first DirectionalLight and SkyLight in the world (an entity with no Transform keeps the default direction), and the PointLights and SpotLights
// nearest to `camera_position`, up to the limits above. Positions and directions are world space (parents included).
FrameData BuildFrameData(const World& world, const GuidIndex& guids, const Vec3& camera_position);
// The first SkyLight in the world (the one BuildFrameData uses), or null.
const SkyLight* FindSkyLight(const World& world);
// The first Wind in the world (the one BuildFrameData uses), or null.
const Wind* FindWind(const World& world);

// The wind zones of a world (the first kMaxWindZones, in entity order), where each stands, and what an object at a point takes from them.
inline constexpr u32 kMaxWindZones = 7;
struct WindZoneSite {
    Vec3 center;
    f32 radius = 1.0f;
    f32 falloff = 0.5f;
};
// The wind an object takes: the world's, or its zone's blended in by how far inside the zone it stands (`zone_bits` from WindZoneBitsAt); what the scene shader does.
struct WindSample {
    f32 direction[2]{1.0f, 0.0f}; // x, z, unit length
    f32 strength = 0.0f;
    f32 gust_speed = 1.0f;
};
WindSample WindForDraw(const FrameData& frame, u32 zone_bits);
std::vector<WindZoneSite> CollectWindZones(const World& world, const GuidIndex& guids);
// The zone an object at `position` is most inside, packed for the draw: bits 0-2 the zone's number plus one (0: none, the world's wind), bits 3-7 how far
// inside it is (0 at the edge to 31 in the core). 0 where no zone reaches the point.
u32 WindZoneBitsAt(const std::vector<WindZoneSite>& zones, const Vec3& position);

// Particle lights (vfx::ParticleLight: fire, sparks, explosions) join the point lights: the scene's and the particles' are pooled and the
// kMaxPointLights nearest to `camera_position` stay, except that the scene's nearest four keep their slots however many particle lights
// there are. A light with no radius or no energy is ignored. Call after
// BuildFrameData, before the frame constants are uploaded.
void AddParticleLights(FrameData& data, std::span<const vfx::ParticleLight> lights, const Vec3& camera_position);

// Every PointLight and SpotLight in the world with a range (positions and directions in world space, parents included), then the
// particle lights with a radius and energy, as clustered lights (light_clusters.h) for BuildLightClusters.
std::vector<ClusterLight> GatherClusterLights(const World& world, const GuidIndex& guids, std::span<const vfx::ParticleLight> particles);
// Turns the clustered path on in `data`: the cluster block above, for clusters built for `view` and a render target of width x height.
void FillClusterData(FrameData& data, const LightClusters& clusters, const ClusterView& view, u32 width, u32 height);

} // namespace aether::player
