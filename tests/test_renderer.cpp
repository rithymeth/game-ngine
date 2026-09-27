#include "aether/renderer/draw_list.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "test_framework.h"

#include <cmath>

using namespace aether;

// Phase 14 step 1: render scene extraction, views, culling and draw lists.

namespace {

bool Near(const Vec3& a, const Vec3& b, f32 eps = 1e-4f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

struct Scene {
    World world;
    GuidIndex guids;
    Entity Add(const Vec3& at, const Quaternion& rotation = Quaternion::Identity()) {
        const Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, Transform{at, rotation});
        guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
        return e;
    }
    Entity Mesh(const Vec3& at, const char* path) {
        const Entity e = Add(at);
        ModelRenderer m;
        SetModelPath(m, path);
        world.AddComponent(e, m);
        return e;
    }
};

} // namespace

AETHER_TEST(Renderer_ExtractsObjectsLightsAndVolumes) {
    RegisterRenderComponents();
    Scene s;
    const Entity parent = s.Add(Vec3(10, 0, 0));
    const Entity child = s.Mesh(Vec3(0, 2, 0), "models/crate.gltf");
    s.world.AddComponent(child, Parent{s.world.GetComponent<IdComponent>(parent)->guid});
    const Entity other = s.Mesh(Vec3(0, 0, 0), "models/crate.gltf");
    const Entity rock = s.Mesh(Vec3(0, 0, -5), "models/rock.gltf");
    const Entity hidden = s.Mesh(Vec3(0, 0, 0), "models/crate.gltf");
    s.world.AddComponent(hidden, Active{false});

    // The sun, pitched down 90° about X: it shines straight down.
    const Entity sun = s.Add(Vec3(0, 50, 0), Quaternion::FromAxisAngle(Vec3(1, 0, 0), -kPi / 2));
    s.world.AddComponent(sun, DirectionalLight{Vec3(1, 0.5f, 0.25f), 4.0f, true});
    const Entity lamp = s.Add(Vec3(1, 2, 3));
    s.world.AddComponent(lamp, PointLight{Vec3(1, 1, 1), 5.0f, 8.0f, false});
    const Entity spot = s.Add(Vec3(0, 3, 0));
    SpotLight cone;
    cone.inner_angle_degrees = 60;
    cone.outer_angle_degrees = 45; // inner wider than outer: clamped to outer
    s.world.AddComponent(spot, cone);
    const Entity sky = s.Add(Vec3(0, 0, 0));
    s.world.AddComponent(sky, SkyLight{Vec3(0.5f, 0.5f, 1), 2.0f});

    // Bounds from a provider: crates are 2 m cubes, rocks aren't known (unit cube).
    ExtractOptions options;
    options.mesh_bounds = [](const ModelRenderer& m, Aabb& local) {
        if (std::string(m.asset_path) != "models/crate.gltf") return false;
        local = {Vec3(-1, -1, -1), Vec3(1, 1, 1)};
        return true;
    };
    const RenderScene scene = ExtractRenderScene(s.world, s.guids, options);
    AETHER_CHECK(scene.objects.size() == 3); // not the hidden one
    const RenderObject& c = scene.objects[0];
    AETHER_CHECK(c.entity == child && Near(c.bounds.Center(), Vec3(10, 2, 0)) && Near(c.bounds.Extents(), Vec3(1, 1, 1)));
    AETHER_CHECK(scene.objects[1].entity == other && scene.objects[1].mesh_key == c.mesh_key);
    AETHER_CHECK(scene.objects[2].entity == rock && scene.objects[2].mesh_key != c.mesh_key);
    AETHER_CHECK(Near(scene.objects[2].bounds.Extents(), Vec3(0.5f, 0.5f, 0.5f)));

    AETHER_CHECK(scene.directional_lights.size() == 1);
    AETHER_CHECK(Near(scene.directional_lights[0].direction, Vec3(0, -1, 0)) && Near(scene.directional_lights[0].radiance, Vec3(4, 2, 1)));
    AETHER_CHECK(scene.point_lights.size() == 1 && Near(scene.point_lights[0].position, Vec3(1, 2, 3)) && scene.point_lights[0].range == 8.0f);
    AETHER_CHECK(Near(scene.point_lights[0].radiance, Vec3(5, 5, 5)));
    AETHER_CHECK(scene.spot_lights.size() == 1 && scene.spot_lights[0].cos_inner == scene.spot_lights[0].cos_outer);
    AETHER_CHECK(std::fabs(scene.spot_lights[0].cos_outer - std::cos(45.0f * kPi / 180.0f)) < 1e-5f);
    AETHER_CHECK(Near(scene.sky_radiance, Vec3(1, 1, 2)));

    // A rotated, scaled box: the world bounds contain the turned corners.
    Mat4 m = Quaternion::FromAxisAngle(Vec3(0, 1, 0), kPi / 4).ToMat4();
    m.cols[3] = Vec4(0, 0, 0, 1);
    const Aabb turned = Aabb{Vec3(-1, -1, -1), Vec3(1, 1, 1)}.Transformed(m);
    AETHER_CHECK(std::fabs(turned.max.x - std::sqrt(2.0f)) < 1e-4f && std::fabs(turned.max.y - 1.0f) < 1e-4f);
}

AETHER_TEST(Renderer_ViewsAndFrustumCulling) {
    Camera camera;
    camera.fov_degrees = 90.0f;
    camera.near_plane = 0.5f;
    camera.far_plane = 100.0f;
    Mat4 at = Mat4::Translation(Vec3(0, 0, 10)); // looking down -Z toward the origin
    const View view = MakeView(camera, at, 1.0f);
    AETHER_CHECK(Near(view.position, Vec3(0, 0, 10)) && Near(view.forward, Vec3(0, 0, -1)));
    const Vec4 origin = view.view * Vec4(0, 0, 0, 1);
    AETHER_CHECK(std::fabs(origin.z + 10.0f) < 1e-4f); // 10 m in front
    const Frustum& f = view.frustum;
    AETHER_CHECK(f.Contains(Vec3(0, 0, 0)) && f.Contains(Vec3(5, 5, 0)));
    AETHER_CHECK(!f.Contains(Vec3(11, 0, 0)));    // outside the 90° cone at 10 m
    AETHER_CHECK(!f.Contains(Vec3(0, 0, 20)));    // behind
    AETHER_CHECK(!f.Contains(Vec3(0, 0, -95)));   // beyond the far plane
    AETHER_CHECK(!f.Contains(Vec3(0, 0, 9.8f)));  // before the near plane
    AETHER_CHECK(f.IntersectsSphere(Vec3(11, 0, 0), 2.0f) && !f.IntersectsSphere(Vec3(15, 0, 0), 2.0f));

    // It agrees with the scene's own camera helpers.
    Scene s;
    const Entity cam = s.Add(Vec3(3, 4, 5), Quaternion::FromAxisAngle(Vec3(0, 1, 0), 0.7f));
    s.world.AddComponent(cam, camera);
    View active;
    AETHER_CHECK(MakeViewFromActiveCamera(s.world, s.guids, 16.0f / 9.0f, active));
    const Mat4 expected = CameraView(s.world, s.guids, cam);
    for (int c = 0; c < 4; ++c) {
        AETHER_CHECK(Near(Vec3(active.view.cols[c].x, active.view.cols[c].y, active.view.cols[c].z),
                          Vec3(expected.cols[c].x, expected.cols[c].y, expected.cols[c].z)));
    }
    World empty;
    AETHER_CHECK(!MakeViewFromActiveCamera(empty, s.guids, 1.0f, active));

    // Culling boxes: inside, outside, straddling a side, around the camera.
    RenderScene scene;
    auto box = [&](const Vec3& center, f32 half) {
        RenderObject o;
        o.bounds = {center - Vec3(half, half, half), center + Vec3(half, half, half)};
        scene.objects.push_back(o);
    };
    box(Vec3(0, 0, 0), 1);     // 0: in
    box(Vec3(30, 0, 0), 1);    // 1: out, right
    box(Vec3(10.5f, 0, 0), 1); // 2: straddles the right plane
    box(Vec3(0, 0, 10), 3);    // 3: around the camera
    box(Vec3(0, 0, 30), 1);    // 4: behind
    AETHER_CHECK(Cull(scene, view) == (std::vector<u32>{0, 2, 3}));
}

AETHER_TEST(Renderer_DrawListsBatchByMeshFrontToBack) {
    Camera camera;
    const View view = MakeView(camera, Mat4::Identity(), 1.0f); // at the origin, looking down -Z
    RenderScene scene;
    auto object = [&](u64 mesh, f32 distance) {
        RenderObject o;
        o.mesh_key = mesh;
        o.bounds = {Vec3(-0.5f, -0.5f, -distance - 0.5f), Vec3(0.5f, 0.5f, -distance + 0.5f)};
        scene.objects.push_back(o);
    };
    object(1, 15); // 0
    object(2, 7);  // 1
    object(1, 5);  // 2
    object(1, 10); // 3
    object(2, 20); // 4
    object(3, 500); // 5: off in the distance
    const std::vector<u32> visible = Cull(scene, view);
    AETHER_CHECK(visible.size() == 6);
    const DrawList list = BuildDrawList(scene, view, visible);
    AETHER_CHECK(list.batches.size() == 3 && list.InstanceCount() == 6);
    AETHER_CHECK(list.batches[0].mesh_key == 1 && list.batches[0].instances == (std::vector<u32>{2, 3, 0}));
    AETHER_CHECK(std::fabs(list.batches[0].nearest - 4.5f) < 1e-4f);
    AETHER_CHECK(list.batches[1].mesh_key == 2 && list.batches[1].instances == (std::vector<u32>{1, 4}));
    AETHER_CHECK(list.batches[2].mesh_key == 3);
    AETHER_CHECK(BuildDrawList(scene, view, {}).batches.empty());
}

AETHER_TEST(Renderer_PostProcessVolumesBlend) {
    RenderScene scene;
    PostProcessVolume global;
    global.exposure_compensation = 1.0f;
    scene.post_volumes.push_back({Vec3(0, 0, 0), global});
    PostProcessVolume room;
    room.global = false;
    room.extents = Vec3(2, 2, 2);
    room.blend_distance = 2.0f;
    room.priority = 1;
    room.exposure_compensation = 3.0f;
    room.tonemapper = Tonemapper::AgX;
    scene.post_volumes.push_back({Vec3(10, 0, 0), room});

    PostProcessSettings p = BlendPostProcess(scene, Vec3(10, 0, 0)); // inside the room
    AETHER_CHECK(std::fabs(p.exposure_compensation - 3.0f) < 1e-5f && p.tonemapper == Tonemapper::AgX);
    p = BlendPostProcess(scene, Vec3(13, 0, 0)); // 1 m outside: half way
    AETHER_CHECK(std::fabs(p.exposure_compensation - 2.0f) < 1e-5f && p.tonemapper == Tonemapper::AgX);
    p = BlendPostProcess(scene, Vec3(13.5f, 0, 0)); // 1.5 m out: a quarter
    AETHER_CHECK(std::fabs(p.exposure_compensation - 1.5f) < 1e-5f && p.tonemapper == Tonemapper::ACES);
    p = BlendPostProcess(scene, Vec3(0, 0, 0)); // far away: only the global volume
    AETHER_CHECK(std::fabs(p.exposure_compensation - 1.0f) < 1e-5f && p.bloom_intensity == 0.05f);
    AETHER_CHECK(BlendPostProcess(RenderScene{}, Vec3(0, 0, 0)).exposure_compensation == 0.0f); // defaults
}
