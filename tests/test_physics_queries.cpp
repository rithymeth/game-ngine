#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/job/job_system.h"
#include "aether/physics/physics_scene.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>

using namespace aether;
using nlohmann::json;

// Phase 13 step 4: ray casts, shape casts and overlaps, filtered by layer,
// ignore list and trigger flag, and the Blueprint nodes that use them.

namespace {

struct Sim {
    JobSystem jobs{2};
    World world;
    PhysicsWorld physics{jobs};
    PhysicsScene scene{world, physics};

    Entity Box(const Vec3& at, const Vec3& half, u8 layer = 0, bool trigger = false) {
        BoxCollider box;
        box.half_extents = half;
        box.is_trigger = trigger;
        const Entity e = world.CreateEntity(Transform{at, Quaternion::Identity()}, box);
        if (layer != 0) world.AddComponent(e, Layer{layer});
        return e;
    }
    Entity Ball(const Vec3& at, f32 radius = 0.5f) {
        SphereCollider s;
        s.radius = radius;
        RigidBody rb;
        rb.motion = BodyMotion::Kinematic; // stays where it's put
        return world.CreateEntity(Transform{at, Quaternion::Identity()}, rb, s);
    }
};

bool Near(const Vec3& a, const Vec3& b, f32 eps = 1e-3f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

} // namespace

AETHER_TEST(PhysicsQueries_RayCastsHitTheNearestSurface) {
    Sim s;
    const Entity floor = s.Box(Vec3(0, 0, 0), Vec3(10, 0.5f, 10));
    const Entity wall = s.Box(Vec3(5, 2, 0), Vec3(0.5f, 2, 2), /*layer=*/1);
    const Entity zone = s.Box(Vec3(-3, 2, 0), Vec3(1, 1, 1), 0, /*trigger=*/true);
    s.scene.Sync();

    // Straight down onto the floor.
    SceneHit hit = s.scene.RayCast(Vec3(0, 5, 0), Vec3(0, -5, 0));
    AETHER_CHECK(hit.hit && hit.entity == floor);
    AETHER_CHECK(Near(hit.point, Vec3(0, 0.5f, 0)) && Near(hit.normal, Vec3(0, 1, 0)));
    AETHER_CHECK(std::fabs(hit.distance - 4.5f) < 1e-3f);

    // Sideways into the wall's near face.
    hit = s.scene.RayCast(Vec3(0, 2, 0), Vec3(10, 2, 0));
    AETHER_CHECK(hit.hit && hit.entity == wall && Near(hit.point, Vec3(4.5f, 2, 0)) && Near(hit.normal, Vec3(-1, 0, 0)));
    // Layer mask without the wall's layer: nothing along that line.
    AETHER_CHECK(!s.scene.RayCast(Vec3(0, 2, 0), Vec3(10, 2, 0), /*layers=*/1u << 0).hit);
    // Ignoring the floor lets a ray through it.
    AETHER_CHECK(!s.scene.RayCast(Vec3(0, 5, 0), Vec3(0, -5, 0), kAllLayers, floor).hit);
    // Too short: a miss.
    AETHER_CHECK(!s.scene.RayCast(Vec3(0, 5, 0), Vec3(0, 1, 0)).hit);

    // Triggers are skipped unless asked for.
    hit = s.scene.RayCast(Vec3(-3, 6, 0), Vec3(-3, -1, 0));
    AETHER_CHECK(hit.hit && hit.entity == floor);
    hit = s.scene.RayCast(Vec3(-3, 6, 0), Vec3(-3, -1, 0), kAllLayers, kNullEntity, /*include_triggers=*/true);
    AETHER_CHECK(hit.hit && hit.entity == zone && std::fabs(hit.point.y - 3.0f) < 1e-3f);
}

AETHER_TEST(PhysicsQueries_SphereCastsAndOverlaps) {
    Sim s;
    const Entity floor = s.Box(Vec3(0, 0, 0), Vec3(10, 0.5f, 10));
    const Entity a = s.Ball(Vec3(-1, 2, 0)), b = s.Ball(Vec3(1, 2, 0)), far = s.Ball(Vec3(6, 2, 0));
    const Entity ghost = s.Ball(Vec3(0, 2, 3));
    s.world.AddComponent(ghost, Layer{2});
    s.scene.Sync();

    // A 0.5 m sphere swept down from y = 5 stops with its center at y = 1:
    // 4 m travelled, touching the floor's top.
    SceneHit hit = s.scene.SphereCast(0.5f, Vec3(3, 5, 0), Vec3(3, -5, 0));
    AETHER_CHECK(hit.hit && hit.entity == floor && std::fabs(hit.distance - 4.0f) < 0.01f);
    AETHER_CHECK(Near(hit.normal, Vec3(0, 1, 0), 0.01f) && std::fabs(hit.point.y - 0.5f) < 0.01f);
    // A thin line between the two balls misses them; a fat sphere doesn't.
    AETHER_CHECK(!s.scene.RayCast(Vec3(0, 5, 0), Vec3(0, 1.2f, 0)).hit);
    hit = s.scene.SphereCast(0.8f, Vec3(0, 5, 0), Vec3(0, 1.2f, 0));
    AETHER_CHECK(hit.hit && (hit.entity == a || hit.entity == b));
    // Starting inside something: a hit at distance 0.
    hit = s.scene.SphereCast(0.3f, Vec3(-1, 2, 0), Vec3(-1, 4, 0));
    AETHER_CHECK(hit.hit && hit.entity == a && hit.distance == 0.0f);

    // Overlaps: everything touching, in body order, filtered like casts.
    std::vector<Entity> near = s.scene.OverlapSphere(Vec3(0, 2, 0), 1.0f);
    AETHER_CHECK(near == (std::vector<Entity>{a, b}));
    near = s.scene.OverlapSphere(Vec3(0, 2, 0), 3.2f);
    AETHER_CHECK(near.size() == 4 && std::find(near.begin(), near.end(), far) == near.end()); // floor, a, b, ghost
    near = s.scene.OverlapSphere(Vec3(0, 2, 0), 3.2f, kAllLayers & ~(1u << 2), a);
    AETHER_CHECK(near == (std::vector<Entity>{floor, b}));
}

AETHER_TEST(PhysicsQueries_BlueprintNodesTraceAndOverlap) {
    Sim s;
    const Entity floor = s.Box(Vec3(0, 0, 0), Vec3(10, 0.5f, 10));
    const Entity looker = s.Ball(Vec3(0, 3, 0), 0.25f); // the Blueprint's own body: ignored by "ignore self"
    s.Ball(Vec3(1, 3, 0), 0.25f);
    s.Ball(Vec3(-1, 3, 0), 0.25f);
    s.scene.Sync();

    // Look: trace straight down from itself and remember what it hit.
    // Scan: count what's within 1.5 m.
    bp::Blueprint b;
    bp::Graph g;
    g.name = "EventGraph";
    b.graphs.push_back(g);
    for (const auto& [name, type] : {std::pair<const char*, const char*>{"Hit", "bool"}, {"What", "Entity"}, {"Distance", "float"},
                                     {"Near", "int"}}) {
        bp::Variable v;
        v.name = name;
        v.type = *bp::ParseType(type);
        v.default_value = bp::DefaultValue(v.type);
        b.variables.push_back(v);
    }
    bp::GraphBuilder gb(b.graphs[0]);
    const bp::NodeId look = gb.Add("Event.Custom", {{"name", "Look"}});
    const bp::NodeId here = gb.Add("Entity.GetLocation");
    const bp::NodeId down = gb.Add("Math.Add:Vec3");
    gb.Default(down, "b", json::array({0, -10, 0}));
    const bp::NodeId trace = gb.Add("Physics.LineTrace");
    const bp::NodeId set_hit = gb.Add("Var.Set:Hit"), set_what = gb.Add("Var.Set:What"), set_dist = gb.Add("Var.Set:Distance");
    gb.Connect(here, "location", down, "a").Connect(here, "location", trace, "start").Connect(down, "result", trace, "end");
    gb.Connect(look, "then", trace, "exec").Connect(trace, "then", set_hit, "exec").Connect(trace, "hit", set_hit, "value");
    gb.Connect(set_hit, "then", set_what, "exec").Connect(trace, "hit_entity", set_what, "value");
    gb.Connect(set_what, "then", set_dist, "exec").Connect(trace, "distance", set_dist, "value");
    const bp::NodeId scan = gb.Add("Event.Custom", {{"name", "Scan"}});
    const bp::NodeId overlap = gb.Add("Physics.OverlapSphere");
    gb.Default(overlap, "radius", 1.5);
    const bp::NodeId len = gb.Add("Array.Length:Entity"), set_near = gb.Add("Var.Set:Near");
    gb.Connect(scan, "then", overlap, "exec").Connect(here, "location", overlap, "center");
    gb.Connect(overlap, "then", set_near, "exec").Connect(overlap, "entities", len, "array").Connect(len, "result", set_near, "value");
    const bp::CompileResult compiled = bp::CompileBlueprint(b);
    for (const bp::Diagnostic& d : compiled.diagnostics.diagnostics) std::printf("    %s: %s\n", d.code.c_str(), d.message.c_str());
    AETHER_CHECK(compiled.Ok());
    if (!compiled.Ok()) return;

    bp::BlueprintVM vm(s.world);
    AETHER_CHECK(vm.Attach(looker, compiled.blueprint));

    // Without physics connected: no hit, and a warning.
    AETHER_CHECK(vm.Dispatch(looker, "Event.Custom:Look"));
    AETHER_CHECK(!std::get<bool>(vm.GetVariable(looker, "Hit")));
    AETHER_CHECK(std::any_of(vm.Errors().begin(), vm.Errors().end(), [](const bp::RuntimeError& e) { return e.code == "BP207"; }));
    vm.ClearErrors();

    bp::BlueprintVM::PhysicsQueries queries;
    queries.trace = [&](const Vec3& from, const Vec3& to, f32 radius, u32 layers, Entity ignore) {
        const SceneHit h = radius > 0.0f ? s.scene.SphereCast(radius, from, to, layers, ignore) : s.scene.RayCast(from, to, layers, ignore);
        return bp::BlueprintVM::PhysicsHit{h.hit, h.entity, h.point, h.normal, h.distance};
    };
    queries.overlap_sphere = [&](const Vec3& center, f32 radius, u32 layers, Entity ignore) {
        return s.scene.OverlapSphere(center, radius, layers, ignore);
    };
    vm.SetPhysicsQueries(queries);

    AETHER_CHECK(vm.Dispatch(looker, "Event.Custom:Look"));
    AETHER_CHECK(std::get<bool>(vm.GetVariable(looker, "Hit")) && std::get<Entity>(vm.GetVariable(looker, "What")) == floor);
    AETHER_CHECK(std::fabs(std::get<f32>(vm.GetVariable(looker, "Distance")) - 2.5f) < 1e-3f); // from y = 3 to the top at 0.5
    AETHER_CHECK(vm.Dispatch(looker, "Event.Custom:Scan"));
    AETHER_CHECK(std::get<i32>(vm.GetVariable(looker, "Near")) == 2); // the two neighbours, not itself
    AETHER_CHECK(vm.Errors().empty());
}
