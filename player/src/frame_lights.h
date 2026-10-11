#pragma once

#include "aether/ecs/world.h"
#include "aether/math/math.h"
#include "aether/scene/entity_guid.h"

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
    u32 counts[4]{};        // x: point lights used, y: spot lights used
    f32 point_position_range[kMaxPointLights][4]{}; // xyz position, w range (the light fades to nothing there)
    f32 point_color[kMaxPointLights][4]{};          // rgb colour times intensity
    f32 spot_position_range[kMaxSpotLights][4]{};   // xyz position, w range
    f32 spot_direction_outer[kMaxSpotLights][4]{};  // xyz: the way it points; w: cosine of the outer cone angle
    f32 spot_color_inner[kMaxSpotLights][4]{};      // rgb colour times intensity; w: cosine of the inner cone angle
};
static_assert(sizeof(FrameData) % 16 == 0 && sizeof(FrameData) == 528);

// The first DirectionalLight and SkyLight in the world (an entity with no Transform keeps the default direction), and the PointLights and SpotLights
// nearest to `camera_position`, up to the limits above. Positions and directions are world space (parents included).
FrameData BuildFrameData(const World& world, const GuidIndex& guids, const Vec3& camera_position);

} // namespace aether::player
