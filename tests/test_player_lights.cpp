#include "../player/src/frame_lights.h"

#include "aether/renderer/components.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "test_framework.h"

#include <cmath>

// The scene's lights as the player's shader reads them (player/src/frame_lights.h): defaults without light components, the sun's direction from its
// rotation, the sky's ambient colour, and the nearest point and spot lights.

using namespace aether;
using namespace aether::player;

namespace {

bool Near(f32 a, f32 b, f32 e = 1e-4f) { return std::fabs(a - b) <= e; }

Transform At(f32 x, f32 y, f32 z, Quaternion rotation = Quaternion::Identity()) {
    Transform t;
    t.position = Vec3(x, y, z);
    t.rotation = rotation;
    return t;
}

} // namespace

AETHER_TEST(PlayerLights_AScenewithoutLightsKeepsTheShadersFixedSunAndSky) {
    RegisterRenderComponents();
    World world;
    GuidIndex guids;
    const FrameData d = BuildFrameData(world, guids, Vec3(1, 2, 3));
    AETHER_CHECK(d.camera_world[0] == 1.0f && d.camera_world[1] == 2.0f && d.camera_world[2] == 3.0f);
    AETHER_CHECK(d.sun_direction[3] == 0.0f && d.sky_color[3] == 0.0f); // nothing set them
    AETHER_CHECK(Near(d.sun_color[0], 3.0f) && Near(d.sky_color[0], 1.0f));
    const f32 length = std::sqrt(d.sun_direction[0] * d.sun_direction[0] + d.sun_direction[1] * d.sun_direction[1] + d.sun_direction[2] * d.sun_direction[2]);
    AETHER_CHECK(Near(length, 1.0f) && d.sun_direction[1] > 0.5f); // a unit vector, mostly from above
    AETHER_CHECK(d.counts[0] == 0 && d.counts[1] == 0);
}

AETHER_TEST(PlayerLights_TheSunPointsAlongItsForwardAxisAndTheSkyBringsItsColour) {
    World world;
    GuidIndex guids;
    // Pitched 90 degrees down about X, the light's forward (-Z) points down, so a surface is lit from straight above.
    DirectionalLight sun;
    sun.color = Vec3(1.0f, 0.5f, 0.25f);
    sun.intensity = 2.0f;
    world.CreateEntity(At(0, 10, 0, Quaternion::FromAxisAngle(Vec3(1, 0, 0), -1.5707963f)), sun);
    SkyLight sky;
    sky.color = Vec3(0.2f, 0.4f, 0.8f);
    sky.intensity = 0.5f;
    world.CreateEntity(At(0, 0, 0), sky);
    const FrameData d = BuildFrameData(world, guids, Vec3(0, 0, 0));
    AETHER_CHECK(d.sun_direction[3] == 1.0f && Near(d.sun_direction[0], 0.0f) && Near(d.sun_direction[1], 1.0f) && Near(d.sun_direction[2], 0.0f));
    AETHER_CHECK(Near(d.sun_color[0], 2.0f) && Near(d.sun_color[1], 1.0f) && Near(d.sun_color[2], 0.5f));
    AETHER_CHECK(d.sky_color[3] == 1.0f && Near(d.sky_color[0], 0.1f) && Near(d.sky_color[1], 0.2f) && Near(d.sky_color[2], 0.4f));
}

AETHER_TEST(PlayerLights_PointAndSpotLightsAreTheNearestOnesAndSpotsPointAlongForward) {
    World world;
    GuidIndex guids;
    // Ten point lights along x: only the eight nearest to the camera are used, nearest first.
    for (int i = 0; i < 10; ++i) {
        PointLight light;
        light.intensity = static_cast<f32>(i + 1);
        light.range = 5.0f;
        world.CreateEntity(At(static_cast<f32>(i * 10), 0, 0), light);
    }
    SpotLight spot;
    spot.intensity = 4.0f;
    spot.range = 20.0f;
    spot.inner_angle_degrees = 10.0f;
    spot.outer_angle_degrees = 30.0f;
    world.CreateEntity(At(0, 5, 0), spot); // identity rotation: it points along -Z
    PointLight dead;                       // no range: skipped
    dead.range = 0.0f;
    world.CreateEntity(At(1, 1, 1), dead);

    const FrameData d = BuildFrameData(world, guids, Vec3(0, 0, 0));
    AETHER_CHECK(d.counts[0] == 8 && d.counts[1] == 1);
    AETHER_CHECK(Near(d.point_position_range[0][0], 0.0f) && Near(d.point_position_range[0][3], 5.0f) && Near(d.point_color[0][0], 1.0f));
    AETHER_CHECK(Near(d.point_position_range[7][0], 70.0f) && Near(d.point_color[7][0], 8.0f)); // the 8th nearest; the two far ones were left out
    AETHER_CHECK(Near(d.spot_direction_outer[0][2], -1.0f) && Near(d.spot_direction_outer[0][3], std::cos(30.0f * 3.14159265f / 180.0f), 1e-3f));
    AETHER_CHECK(Near(d.spot_color_inner[0][3], std::cos(10.0f * 3.14159265f / 180.0f), 1e-3f) && Near(d.spot_color_inner[0][0], 4.0f));
}
