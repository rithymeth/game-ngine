#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/lifecycle.h"
#include "aether/scene/serialization.h"
#include "test_framework.h"

#include <cmath>
#include <string>
#include <vector>

using namespace aether;

namespace gameplay_test {
struct Behaviour {
    std::string name;
};
struct Other {
    i32 value = 0;
};
} // namespace gameplay_test

AETHER_REFLECT(gameplay_test::Behaviour, 1, AETHER_FIELD(name))
AETHER_REFLECT(gameplay_test::Other, 1, AETHER_FIELD(value))

namespace {

using gameplay_test::Behaviour;

bool Near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) < eps; }

struct Scene {
    World world;
    GuidIndex guids;

    Entity Make(const std::string& name, Entity parent = kNullEntity, Vec3 position = Vec3{0, 0, 0}) {
        Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, Transform{position, Quaternion::Identity()},
                                      Behaviour{name});
        guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
        if (!parent.IsNull()) {
            world.AddComponent<Parent>(e, Parent{world.GetComponent<IdComponent>(parent)->guid});
        }
        return e;
    }
};

// Records every callback as "event:name".
struct Recorder {
    std::vector<std::string> log;
    World* world = nullptr;

    std::string Name(Entity e) const { return world->GetComponent<Behaviour>(e)->name; }
    LifecycleCallbacks Callbacks(const std::string& prefix = "") {
        LifecycleCallbacks c;
        auto note = [this, prefix](const char* event) {
            return [this, prefix, event](Entity e) { log.push_back(prefix + event + ":" + Name(e)); };
        };
        auto note_dt = [this, prefix](const char* event) {
            return [this, prefix, event](Entity e, f32) { log.push_back(prefix + event + ":" + Name(e)); };
        };
        c.on_create = note("create");
        c.on_enable = note("enable");
        c.on_start = note("start");
        c.on_update = note_dt("update");
        c.on_fixed_update = note_dt("fixed");
        c.on_late_update = note_dt("late");
        c.on_disable = note("disable");
        c.on_destroy = note("destroy");
        return c;
    }
    std::vector<std::string> Take() {
        std::vector<std::string> out = std::move(log);
        log.clear();
        return out;
    }
};

using Log = std::vector<std::string>;

} // namespace

AETHER_TEST(Gameplay_CameraProjectionViewAndSelection) {
    // Perspective: the near plane maps to depth 0, the far plane to 1.
    Camera camera;
    camera.near_plane = 0.5f;
    camera.far_plane = 100.0f;
    const Mat4 perspective = CameraProjection(camera, 16.0f / 9.0f);
    Vec4 near_point = perspective * Vec4(0, 0, -0.5f, 1);
    Vec4 far_point = perspective * Vec4(0, 0, -100.0f, 1);
    AETHER_CHECK(Near(near_point.z / near_point.w, 0.0f) && Near(far_point.z / far_point.w, 1.0f));
    // The top of a 60 degree view at distance d is at y = d * tan(30).
    Vec4 top = perspective * Vec4(0, 10.0f * std::tan(30.0f * 3.14159265f / 180.0f), -10.0f, 1);
    AETHER_CHECK(Near(top.y / top.w, 1.0f));

    // Orthographic: a 10 m tall view; the edges map to +-1, depth to [0, 1].
    camera.projection = Projection::Orthographic;
    const Mat4 ortho = CameraProjection(camera, 2.0f);
    Vec4 corner = ortho * Vec4(10.0f, 5.0f, -0.5f, 1);
    Vec4 back = ortho * Vec4(0, 0, -100.0f, 1);
    AETHER_CHECK(Near(corner.x, 1.0f) && Near(corner.y, 1.0f) && Near(corner.z, 0.0f) && Near(back.z, 1.0f));

    // The view matrix undoes the camera's world transform, through parents.
    Scene s;
    Entity rig = s.Make("rig", kNullEntity, Vec3{0, 2, 0});
    Entity cam = s.Make("cam", rig, Vec3{0, 0, 5});
    s.world.GetComponent<Transform>(rig)->rotation = Quaternion::FromAxisAngle(Vec3{0, 1, 0}, 1.2f);
    const Mat4 view = CameraView(s.world, s.guids, cam);
    const Mat4 round_trip = view * ComputeWorldTransform(s.world, s.guids, cam);
    for (int c = 0; c < 4; ++c) {
        const Vec4 expected = Mat4::Identity().cols[c];
        AETHER_CHECK(Near(round_trip.cols[c].x, expected.x) && Near(round_trip.cols[c].y, expected.y) &&
                     Near(round_trip.cols[c].z, expected.z) && Near(round_trip.cols[c].w, expected.w));
    }

    // The active camera: highest priority on an active entity, ties to the first.
    AETHER_CHECK(FindActiveCamera(s.world, s.guids).IsNull());
    s.world.AddComponent<Camera>(cam, Camera{});
    Entity overhead = s.Make("overhead");
    s.world.AddComponent<Camera>(overhead, Camera{});
    AETHER_CHECK(FindActiveCamera(s.world, s.guids) == cam);
    s.world.GetComponent<Camera>(overhead)->priority = 5;
    AETHER_CHECK(FindActiveCamera(s.world, s.guids) == overhead);
    s.world.AddComponent<Active>(overhead, Active{false});
    AETHER_CHECK(FindActiveCamera(s.world, s.guids) == cam);
    s.world.AddComponent<Active>(rig, Active{false}); // inactive parent hides the child's camera
    AETHER_CHECK(FindActiveCamera(s.world, s.guids).IsNull());
    AETHER_CHECK(!IsActiveInHierarchy(s.world, s.guids, cam) && IsActiveInHierarchy(s.world, s.guids, s.Make("free")));

    // Degenerate settings don't produce NaNs.
    Camera bad;
    bad.near_plane = 0.0f;
    bad.far_plane = 0.0f;
    bad.fov_degrees = 0.0f;
    const Mat4 safe = CameraProjection(bad, 0.0f);
    AETHER_CHECK(std::isfinite(safe.cols[0].x) && std::isfinite(safe.cols[2].z) && std::isfinite(safe.cols[3].z));
}

AETHER_TEST(Gameplay_TagsAndLayers) {
    Scene s;
    Entity a = s.Make("a");
    Entity b = s.Make("b");
    Entity c = s.Make("c");
    AETHER_CHECK(!HasTag(s.world, a, "Enemy") && !RemoveTag(s.world, a, "Enemy"));
    AETHER_CHECK(AddTag(s.world, a, "Enemy") && !AddTag(s.world, a, "Enemy"));
    AETHER_CHECK(AddTag(s.world, c, "Enemy") && AddTag(s.world, c, "Boss") && AddTag(s.world, b, "Pickup"));
    AETHER_CHECK(HasTag(s.world, c, "Boss") && !HasTag(s.world, c, "boss")); // exact names
    AETHER_CHECK((FindEntitiesWithTag(s.world, "Enemy") == std::vector<Entity>{a, c}));
    AETHER_CHECK(RemoveTag(s.world, a, "Enemy") && FindEntitiesWithTag(s.world, "Enemy") == std::vector<Entity>{c});

    ProjectSettings settings;
    settings.layers = {"Default", "Player", "Enemy", "Pickup"};
    AETHER_CHECK(FindLayer(settings, "Enemy") == 2 && FindLayer(settings, "Water") == -1);
    std::vector<std::string> unknown;
    const LayerMask mask = MakeLayerMask(settings, {"Player", "Enemy", "Water"}, &unknown);
    AETHER_CHECK(mask == 0b110 && unknown == std::vector<std::string>{"Water"});
    AETHER_CHECK(LayerOf(s.world, a) == 0 && IsInLayerMask(s.world, a, 1u) && !IsInLayerMask(s.world, a, mask));
    s.world.AddComponent<Layer>(c, Layer{2});
    AETHER_CHECK(LayerOf(s.world, c) == 2 && IsInLayerMask(s.world, c, mask) && IsInLayerMask(s.world, c, kAllLayers));
    s.world.GetComponent<Layer>(c)->index = 200; // out of range: treated as Default
    AETHER_CHECK(LayerOf(s.world, c) == 0);

    // The gameplay components survive a scene round trip.
    s.world.GetComponent<Layer>(c)->index = 3;
    s.world.AddComponent<Camera>(b, Camera{Projection::Orthographic, 45.0f, 20.0f, 0.3f, 50.0f, 7});
    s.world.AddComponent<Active>(b, Active{false});
    World loaded;
    AETHER_CHECK(LoadSceneFromMemory(loaded, SaveSceneToMemory(s.world)));
    GuidIndex loaded_guids;
    loaded_guids.Rebuild(loaded);
    const Entity lb = loaded_guids.Find(loaded, s.world.GetComponent<IdComponent>(b)->guid);
    const Entity lc = loaded_guids.Find(loaded, s.world.GetComponent<IdComponent>(c)->guid);
    AETHER_CHECK(loaded.GetComponent<Camera>(lb)->priority == 7 &&
                 loaded.GetComponent<Camera>(lb)->projection == Projection::Orthographic);
    AETHER_CHECK(!loaded.GetComponent<Active>(lb)->active && HasTag(loaded, lb, "Pickup"));
    AETHER_CHECK(LayerOf(loaded, lc) == 3 && HasTag(loaded, lc, "Boss"));
}

AETHER_TEST(Lifecycle_OrderAcrossTheHierarchy) {
    Scene s;
    Recorder rec;
    rec.world = &s.world;
    Entity root = s.Make("R");
    Entity child = s.Make("C", root);
    s.Make("G", child);
    Lifecycle life(s.world, s.guids);
    life.Register<Behaviour>(rec.Callbacks());

    life.BeginPlay();
    AETHER_CHECK(life.IsPlaying() && life.TrackedCount() == 3);
    AETHER_CHECK((rec.Take() == Log{"create:R", "create:C", "create:G", "enable:R", "enable:C", "enable:G", "start:R",
                                    "start:C", "start:G"}));
    life.Update(0.016f);
    life.FixedUpdate(0.02f);
    life.LateUpdate(0.016f);
    AETHER_CHECK((rec.Take() == Log{"update:R", "update:C", "update:G", "fixed:R", "fixed:C", "fixed:G", "late:R",
                                    "late:C", "late:G"}));

    // Deactivating the middle disables it and its child (children first);
    // they get no updates until reactivated (parents first).
    life.SetActive(child, false);
    AETHER_CHECK((rec.Take() == Log{"disable:G", "disable:C"}));
    life.Update(0.016f);
    AETHER_CHECK((rec.Take() == Log{"update:R"}));
    life.SetActive(child, true);
    AETHER_CHECK((rec.Take() == Log{"enable:C", "enable:G"}));

    // Destroying removes the subtree, children first.
    life.Destroy(child);
    AETHER_CHECK((rec.Take() == Log{"disable:G", "destroy:G", "disable:C", "destroy:C"}));
    AETHER_CHECK(!s.world.IsAlive(child) && s.world.EntityCount() == 1 && life.TrackedCount() == 1);

    life.EndPlay();
    AETHER_CHECK((rec.Take() == Log{"disable:R", "destroy:R"}) && !life.IsPlaying() && life.TrackedCount() == 0);
}

AETHER_TEST(Lifecycle_ChangesDuringCallbacksAreSafe) {
    Scene s;
    Recorder rec;
    rec.world = &s.world;
    Entity root = s.Make("R");
    Entity victim = s.Make("V");
    Lifecycle life(s.world, s.guids);
    int frame = 0;
    LifecycleCallbacks callbacks = rec.Callbacks();
    auto base_update = callbacks.on_update;
    callbacks.on_update = [&](Entity e, f32 dt) {
        base_update(e, dt);
        if (e == root && frame == 1) {
            s.Make("S");          // spawned: starts next pass
            life.Destroy(victim); // deferred: V still updates this pass
            life.SetActive(root, false);
        }
    };
    life.Register<Behaviour>(callbacks);
    life.BeginPlay();
    rec.Take();

    frame = 1;
    life.Update(0.016f);
    // V still updates this pass; the destroy and the deactivation apply at
    // its end, and the spawned S is created, enabled and started then too.
    AETHER_CHECK((rec.Take() == Log{"update:R", "update:V", "disable:V", "destroy:V", "create:S", "enable:S",
                                    "disable:R", "start:S"}));
    AETHER_CHECK(!s.world.IsAlive(victim));
    frame = 2;
    life.Update(0.016f); // R is inactive
    AETHER_CHECK((rec.Take() == Log{"update:S"}));

}

AETHER_TEST(Lifecycle_PerComponentRegistrations) {
    Scene s;
    Recorder rec;
    rec.world = &s.world;
    Entity a = s.Make("A");
    s.world.AddComponent<gameplay_test::Other>(a);
    Lifecycle life(s.world, s.guids);
    life.Register<Behaviour>(rec.Callbacks());
    // A second kind of component, whose callbacks don't read Behaviour.
    std::vector<std::string> other_log;
    LifecycleCallbacks other;
    other.on_create = [&](Entity) { other_log.push_back("create"); };
    other.on_update = [&](Entity, f32) { other_log.push_back("update"); };
    other.on_destroy = [&](Entity) { other_log.push_back("destroy"); };
    life.Register<gameplay_test::Other>(other);

    life.BeginPlay();
    life.Update(0.016f);
    AETHER_CHECK(life.TrackedCount() == 2 && (other_log == std::vector<std::string>{"create", "update"}));
    AETHER_CHECK(rec.Take().size() == 4); // create, enable, start, update for Behaviour

    // Removing a component ends just that registration (its callbacks run
    // after the component is gone); the entity carries on.
    s.world.RemoveComponent<gameplay_test::Other>(a);
    life.Update(0.016f);
    AETHER_CHECK((other_log == std::vector<std::string>{"create", "update", "destroy"}));
    AETHER_CHECK((rec.Take() == Log{"update:A"}) && life.TrackedCount() == 1);
    // Adding one during play starts it on the next pass.
    s.world.AddComponent<gameplay_test::Other>(a);
    life.Update(0.016f);
    AETHER_CHECK(other_log.size() == 5 && other_log[3] == "create" && other_log[4] == "update");
    rec.Take();

    // An entity destroyed directly, not through Lifecycle, is just forgotten.
    s.guids.Remove(s.world.GetComponent<IdComponent>(a)->guid);
    s.world.DestroyEntity(a);
    life.Update(0.016f);
    AETHER_CHECK(life.TrackedCount() == 0 && rec.Take().empty() && other_log.size() == 5);
    life.EndPlay();
}
