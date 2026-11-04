#include "../player/src/frame_lights.h"
#include "../player/src/particle_pass.h"

#include "aether/renderer/components.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "test_framework.h"

#include <cmath>
#include <vector>

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

AETHER_TEST(PlayerLights_ViewDistancePullsInTheFarPlaneOnly) {
    Camera camera;
    camera.near_plane = 0.1f, camera.far_plane = 1000.0f;
    AETHER_CHECK(Near(LimitViewDistance(camera, 300.0f).far_plane, 300.0f));
    AETHER_CHECK(Near(LimitViewDistance(camera, 5000.0f).far_plane, 1000.0f)); // never pushed out
    AETHER_CHECK(Near(LimitViewDistance(camera, 0.0f).far_plane, 1000.0f));    // 0: as authored
    AETHER_CHECK(LimitViewDistance(camera, 0.01f).far_plane > camera.near_plane); // kept clear of the near plane
    camera.projection = Projection::Orthographic;
    AETHER_CHECK(Near(LimitViewDistance(camera, 300.0f).far_plane, 1000.0f));
}

AETHER_TEST(PlayerLights_ParticleLightsJoinTheScenesAndTheNearestEightStay) {
    FrameData d;
    d.counts[0] = 1; // one scene light, far away
    d.point_position_range[0][0] = 100.0f, d.point_position_range[0][3] = 50.0f;
    d.point_color[0][0] = 2.0f;
    const auto particle = [](f32 x, f32 radius, f32 intensity) {
        vfx::ParticleLight l;
        l.position = Vec3(x, 0, 0);
        l.color = {1.0f, 0.5f, 0.0f, 1.0f};
        l.radius = radius;
        l.intensity = intensity;
        return l;
    };
    // Nothing usable (no radius, no energy, NaN): the scene's light is left exactly as it was.
    const std::vector<vfx::ParticleLight> useless = {particle(1, 0.0f, 1.0f), particle(2, 3.0f, 0.0f), particle(3, std::nanf(""), 1.0f)};
    AddParticleLights(d, useless, Vec3(0, 0, 0));
    AETHER_CHECK(d.counts[0] == 1 && d.point_position_range[0][0] == 100.0f);

    // A particle light is a point light with its colour times its intensity, and it sorts in by distance from the camera.
    const std::vector<vfx::ParticleLight> one = {particle(5, 4.0f, 3.0f)};
    AddParticleLights(d, one, Vec3(0, 0, 0));
    AETHER_CHECK(d.counts[0] == 2);
    AETHER_CHECK(d.point_position_range[0][0] == 5.0f && d.point_position_range[0][3] == 4.0f); // nearer than the scene light, so first
    AETHER_CHECK(Near(d.point_color[0][0], 3.0f) && Near(d.point_color[0][1], 1.5f) && Near(d.point_color[0][2], 0.0f));
    AETHER_CHECK(d.point_position_range[1][0] == 100.0f && Near(d.point_color[1][0], 2.0f)); // the scene's light follows

    // Twelve more than fit: the scene's light keeps a slot (it is far away, but a lamp is not pushed out by sparks), the particles' nearest fill the rest.
    std::vector<vfx::ParticleLight> many;
    for (int i = 1; i <= 12; ++i) many.push_back(particle(static_cast<f32>(i), 1.0f, 1.0f));
    FrameData e;
    e.counts[0] = 1;
    e.point_position_range[0][0] = 100.0f, e.point_position_range[0][3] = 50.0f;
    e.point_color[0][0] = 2.0f;
    AddParticleLights(e, many, Vec3(0, 0, 0));
    AETHER_CHECK(e.counts[0] == kMaxPointLights);
    u32 scene_light_slots = 0;
    for (u32 i = 0; i < kMaxPointLights; ++i) {
        if (e.point_position_range[i][0] == 100.0f) ++scene_light_slots;
        if (i > 0) AETHER_CHECK(e.point_position_range[i - 1][0] <= e.point_position_range[i][0]);
    }
    AETHER_CHECK(scene_light_slots == 1);
    AETHER_CHECK(e.point_position_range[0][0] == 1.0f && e.point_position_range[6][0] == 7.0f); // particles 1..7 and the far scene light

    // Eight scene lights: the particles may take only the other four slots.
    FrameData full;
    full.counts[0] = 8;
    for (int i = 0; i < 8; ++i) full.point_position_range[i][0] = 20.0f + static_cast<f32>(i), full.point_position_range[i][3] = 5.0f;
    AddParticleLights(full, many, Vec3(0, 0, 0));
    u32 from_scene = 0;
    for (u32 i = 0; i < kMaxPointLights; ++i) from_scene += full.point_position_range[i][0] >= 20.0f ? 1u : 0u;
    AETHER_CHECK(full.counts[0] == kMaxPointLights && from_scene == 4);
}

AETHER_TEST(PlayerLights_LitSpritesTakeTheSunSkyAndNearbyPointLights) {
    player::ParticleLighting lighting;
    lighting.lit = true;
    lighting.sun_direction = Vec3(0, 1, 0);
    lighting.sun_color = Vec3(3, 3, 3);
    lighting.sky_color = Vec3(1, 1, 1);
    const Vec3 camera(0, 0, 10);
    const Vec3 sprite(0, 0, 0);
    const Vec3 base = player::ParticleSpriteLight(lighting, sprite, camera);
    AETHER_CHECK(base.x > 0.3f && Near(base.x, base.y) && Near(base.y, base.z)); // sky fill plus the sun across the face, never black
    // A sprite whose camera sees its top is lit by the sun more than one seen from below.
    AETHER_CHECK(player::ParticleSpriteLight(lighting, sprite, Vec3(0, 10, 0)).x > player::ParticleSpriteLight(lighting, sprite, Vec3(0, -10, 0)).x);
    // A point light adds its colour near it and nothing beyond its range.
    lighting.points.push_back({Vec3(0, 0, 2), 5.0f, Vec3(10, 5, 0)});
    const Vec3 lit = player::ParticleSpriteLight(lighting, sprite, camera);
    AETHER_CHECK(lit.x > base.x && lit.x - base.x > 2.0f * (lit.z - base.z) * 0.9f && Near(lit.z, base.z));
    lighting.points[0].position = Vec3(0, 0, 50);
    AETHER_CHECK(Near(player::ParticleSpriteLight(lighting, sprite, camera).x, base.x));
}

AETHER_TEST(PlayerLights_OnlyALitBatchHasItsSpritesShaded) {
    vfx::ParticleRenderData data;
    for (bool lit : {false, true}) {
        vfx::SpriteBatch batch;
        batch.lit = lit;
        vfx::SpriteInstance s;
        s.center = Vec3(0, 0, 0);
        s.right = Vec3(1, 0, 0);
        s.up = Vec3(0, 1, 0);
        s.color = {1.0f, 1.0f, 1.0f, 1.0f};
        batch.instances.push_back(s);
        data.sprites.push_back(batch);
    }
    vfx::ParticleCamera camera;
    camera.position = Vec3(0, 0, 10);
    player::ParticleLighting lighting;
    lighting.lit = true;
    player::ParticleVertexStreams streams;
    player::PackParticleVertices(data, camera, streams, {}, {}, &lighting);
    AETHER_CHECK(streams.alpha.size() == 12); // two sprites, six vertices each
    const f32 first = streams.alpha[0].color[0], second = streams.alpha[6].color[0];
    const f32 unlit = std::max(first, second), shaded = std::min(first, second);
    AETHER_CHECK(Near(unlit, 1.0f) && shaded > 0.1f && shaded < 0.9f);
    // Without lighting both draw as they always did.
    player::PackParticleVertices(data, camera, streams);
    AETHER_CHECK(Near(streams.alpha[0].color[0], 1.0f) && Near(streams.alpha[6].color[0], 1.0f));
}
