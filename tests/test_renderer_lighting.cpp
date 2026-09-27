#include "aether/renderer/clusters.h"
#include "aether/renderer/shadows.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <random>

using namespace aether;

// Phase 14 step 2: light clusters and cascaded shadow maps.

namespace {

View TestView(bool perspective = true) {
    Camera camera;
    camera.projection = perspective ? Projection::Perspective : Projection::Orthographic;
    camera.fov_degrees = 70.0f;
    camera.ortho_height = 20.0f;
    camera.near_plane = 0.1f;
    camera.far_plane = 100.0f;
    return MakeView(camera, Mat4::Identity(), 16.0f / 9.0f); // at the origin, looking down -Z
}

// A random point inside the view volume.
Vec3 RandomVisiblePoint(const View& view, std::mt19937& rng) {
    std::uniform_real_distribution<f32> unit(-0.98f, 0.98f), depth(view.near_plane * 1.01f, view.far_plane * 0.99f);
    const f32 d = depth(rng);
    const f32 half_h = view.perspective ? d * std::tan(Radians(view.fov_degrees) * 0.5f) : view.ortho_height * 0.5f;
    return Vec3(unit(rng) * half_h * view.aspect, unit(rng) * half_h, -d);
}

bool Lists(const LightClusters& c, u32 cluster, u32 light, bool spot) {
    const ClusterLightRange& r = c.clusters[cluster];
    const u32 begin = r.offset + (spot ? r.point_count : 0);
    const u32 end = begin + (spot ? r.spot_count : r.point_count);
    return std::find(c.indices.begin() + begin, c.indices.begin() + end, light) != c.indices.begin() + end;
}

} // namespace

AETHER_TEST(RendererLighting_ClustersListEveryLightThatReaches) {
    std::mt19937 rng(7);
    for (const bool perspective : {true, false}) {
        const View view = TestView(perspective);
        RenderScene scene;
        std::uniform_real_distribution<f32> range(0.5f, 6.0f);
        for (int i = 0; i < 200; ++i) {
            RenderPointLight l;
            l.position = RandomVisiblePoint(view, rng);
            l.range = range(rng);
            scene.point_lights.push_back(l);
        }
        for (int i = 0; i < 40; ++i) {
            RenderSpotLight l;
            l.position = RandomVisiblePoint(view, rng);
            l.direction = Vec3(0.3f, -1, -0.2f).Normalized();
            l.range = range(rng) * 2;
            l.cos_outer = std::cos(Radians(i % 2 ? 20.0f : 60.0f)); // narrow and wide cones
            scene.spot_lights.push_back(l);
        }
        const LightClusters clusters = BuildLightClusters(scene, view);
        AETHER_CHECK(clusters.clusters.size() == 16u * 9u * 24u && clusters.dropped == 0);
        AETHER_CHECK(clusters.slice_depths.size() == 25 && std::fabs(clusters.slice_depths.front() - 0.1f) < 1e-5f &&
                     std::fabs(clusters.slice_depths.back() - 100.0f) < 1e-3f);
        AETHER_CHECK(std::is_sorted(clusters.slice_depths.begin(), clusters.slice_depths.end()));
        if (perspective) AETHER_CHECK(clusters.slice_depths[1] - clusters.slice_depths[0] < clusters.slice_depths[24] - clusters.slice_depths[23]);

        // No light is missed where it reaches: sample points, and every
        // light whose range holds a sample must be in that sample's cluster.
        int checked = 0;
        for (int i = 0; i < 3000; ++i) {
            const Vec3 p = RandomVisiblePoint(view, rng);
            u32 cluster;
            AETHER_CHECK(clusters.ClusterOf(view, p, cluster));
            for (u32 l = 0; l < scene.point_lights.size(); ++l) {
                if ((scene.point_lights[l].position - p).Length() < scene.point_lights[l].range) {
                    ++checked;
                    AETHER_CHECK(Lists(clusters, cluster, l, false));
                }
            }
            for (u32 l = 0; l < scene.spot_lights.size(); ++l) {
                Vec3 c;
                f32 r;
                SpotBoundingSphere(scene.spot_lights[l], c, r);
                if ((c - p).Length() < r) AETHER_CHECK(Lists(clusters, cluster, l, true));
            }
        }
        AETHER_CHECK(checked > 100);
        // And lights stay local: on average a light is in a small part of the grid.
        AETHER_CHECK(clusters.indices.size() < 240u * clusters.clusters.size() / 20);
    }

    // A light far away from a cluster isn't listed there.
    const View view = TestView();
    RenderScene one;
    RenderPointLight lamp;
    lamp.position = Vec3(0, 0, -5);
    lamp.range = 1.0f;
    one.point_lights.push_back(lamp);
    const LightClusters c = BuildLightClusters(one, view);
    u32 home, far_cluster;
    AETHER_CHECK(c.ClusterOf(view, Vec3(0, 0, -5), home) && Lists(c, home, 0, false));
    AETHER_CHECK(c.ClusterOf(view, Vec3(0, 0, -50), far_cluster) && !Lists(c, far_cluster, 0, false));
    AETHER_CHECK(!c.ClusterOf(view, Vec3(0, 0, 5), home)); // behind the camera

    // Too many lights for a cluster: the extras are dropped and counted.
    RenderScene crowd;
    for (int i = 0; i < 10; ++i) crowd.point_lights.push_back(lamp);
    const LightClusters capped = BuildLightClusters(crowd, view, ClusterGrid{}, 4);
    AETHER_CHECK(capped.dropped > 0 && capped.clusters[home].point_count == 4);
}

AETHER_TEST(RendererLighting_SpotSpheresHoldTheirCones) {
    for (const f32 angle : {10.0f, 30.0f, 44.0f, 46.0f, 60.0f, 85.0f}) {
        RenderSpotLight spot;
        spot.position = Vec3(1, 2, 3);
        spot.direction = Vec3(0, -1, 0);
        spot.range = 10.0f;
        spot.cos_outer = std::cos(Radians(angle));
        Vec3 c;
        f32 r;
        SpotBoundingSphere(spot, c, r);
        const f32 s = std::sin(Radians(angle)) * 10.0f, h = std::cos(Radians(angle)) * 10.0f;
        // The apex, the tip and the rim of the cap are all inside.
        AETHER_CHECK((spot.position - c).Length() <= r + 1e-3f);
        AETHER_CHECK((spot.position + Vec3(0, -10, 0) - c).Length() <= r + 1e-3f);
        AETHER_CHECK((spot.position + Vec3(s, -h, 0) - c).Length() <= r + 1e-3f);
        AETHER_CHECK(r <= 10.0f + 1e-3f); // never worse than a sphere around the apex
    }
}

AETHER_TEST(RendererLighting_CascadesCoverTheViewAndStayStable) {
    // Splits: the practical scheme between uniform and logarithmic.
    const std::vector<f32> uniform = CascadeSplits(1, 101, 4, 0.0f);
    AETHER_CHECK(std::fabs(uniform[1] - 26.0f) < 1e-4f && std::fabs(uniform[2] - 51.0f) < 1e-4f);
    const std::vector<f32> log = CascadeSplits(1, 10000, 4, 1.0f);
    AETHER_CHECK(std::fabs(log[1] - 10.0f) < 1e-3f && std::fabs(log[2] - 100.0f) < 1e-2f);
    const std::vector<f32> mixed = CascadeSplits(0.1f, 100, 4, 0.75f);
    AETHER_CHECK(mixed.size() == 5 && mixed.front() == 0.1f && mixed.back() == 100.0f && std::is_sorted(mixed.begin(), mixed.end()));

    Camera camera;
    camera.fov_degrees = 60.0f;
    camera.near_plane = 0.1f;
    camera.far_plane = 500.0f;
    const Mat4 pose = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 0.4f).ToMat4() * Mat4::Identity();
    Mat4 at = pose;
    at.cols[3] = Vec4(3, 2, 1, 1);
    const View view = MakeView(camera, at, 16.0f / 9.0f);
    const Vec3 sun = Vec3(0.3f, -1, 0.2f).Normalized();
    CascadeSettings settings; // 4 cascades to 100 m
    const std::vector<ShadowCascade> cascades = ComputeCascades(view, sun, settings);
    AETHER_CHECK(cascades.size() == 4 && cascades.back().split_far == 100.0f);
    for (usize i = 0; i < cascades.size(); ++i) {
        const ShadowCascade& c = cascades[i];
        if (i > 0) AETHER_CHECK(c.split_near == cascades[i - 1].split_far && c.radius > cascades[i - 1].radius);
        // Every corner of its slice is inside its light frustum.
        Vec3 corners[8];
        FrustumSliceCorners(view, c.split_near, c.split_far, corners);
        const Frustum f = Frustum::FromViewProjection(c.view_projection);
        for (const Vec3& p : corners) AETHER_CHECK(f.Contains(p));
        // Casters up to depth_padding toward the sun are in its depth range.
        AETHER_CHECK(f.Contains(c.center - sun * (settings.depth_padding * 0.9f)));
        AETHER_CHECK(std::fabs(c.texel_size - 2.0f * c.radius / 2048.0f) < 1e-6f);
    }

    // Moving the camera a little shifts the shadow map by whole texels, so a
    // fixed point lands on the same place within a texel: no shimmering.
    const Vec3 world_point(10, 0, -20);
    auto texel_of = [&](const ShadowCascade& c) {
        const Vec4 clip = c.view_projection * Vec4(world_point.x, world_point.y, world_point.z, 1.0f);
        return std::pair<f32, f32>((clip.x * 0.5f + 0.5f) * 2048.0f, (clip.y * 0.5f + 0.5f) * 2048.0f);
    };
    for (const f32 step : {0.003f, 0.02f, 0.37f}) {
        Mat4 moved = at;
        moved.cols[3] = Vec4(3 + step, 2, 1 - step, 1);
        const std::vector<ShadowCascade> after = ComputeCascades(MakeView(camera, moved, 16.0f / 9.0f), sun, settings);
        const auto [u0, v0] = texel_of(cascades[1]);
        const auto [u1, v1] = texel_of(after[1]);
        AETHER_CHECK(after[1].radius == cascades[1].radius);
        AETHER_CHECK(std::fabs((u1 - u0) - std::round(u1 - u0)) < 0.02f && std::fabs((v1 - v0) - std::round(v1 - v0)) < 0.02f);
    }
    // Turning the camera doesn't change the cascades' sizes.
    Mat4 turned = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.9f).ToMat4();
    turned.cols[3] = at.cols[3];
    const std::vector<ShadowCascade> turned_cascades = ComputeCascades(MakeView(camera, turned, 16.0f / 9.0f), sun, settings);
    for (usize i = 0; i < 4; ++i) AETHER_CHECK(turned_cascades[i].radius == cascades[i].radius);
    // A sun straight down works too (a different up vector).
    AETHER_CHECK(ComputeCascades(view, Vec3(0, -1, 0), settings).size() == 4);
}
