#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/nav/crowd.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// Phase 20 step 3: NavAgents on a Detour crowd (walking to points and
// after entities, avoiding walls, obstacles and each other, failing or
// stopping short when cut off, stopping, warping, replanning), the
// Navigation query library and the Blueprint nodes.

using namespace aether;
using namespace aether::nav;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

constexpr f32 kDt = 1.0f / 30.0f;

bool Near(f32 a, f32 b, f32 tol) { return std::fabs(a - b) <= tol; }
f32 Flat(const Vec3& a, const Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }

Transform At(const Vec3& p) { return Transform{p, Quaternion::Identity()}; }

ModelRenderer Model(const char* name) {
    ModelRenderer m;
    SetModelPath(m, name);
    return m;
}

// "floor" 20 x 20, "pad" 6 x 6, "wall" a 1 x 3 x 12 wall along Z.
bool Meshes(const ModelRenderer& m, std::vector<Vec3>& v, std::vector<u32>& i) {
    NavGeometry g;
    if (std::strcmp(m.asset_path, "floor") == 0) g.AddPlane(Vec3(0, 0, 0), 10, 10);
    else if (std::strcmp(m.asset_path, "pad") == 0) g.AddPlane(Vec3(0, 0, 0), 3, 3);
    else if (std::strcmp(m.asset_path, "wall") == 0) g.AddBox(Vec3(0, 1.5f, 0), Vec3(0.5f, 1.5f, 6.0f));
    else return false;
    v = g.vertices;
    i = g.indices;
    return true;
}

struct Scene {
    World world;
    NavWorld nav{world, Meshes};
    NavCrowd crowd{world, nav};
    std::vector<MoveCompletedEvent> events;

    explicit Scene(bool floor = true) {
        if (floor) world.CreateEntity(At({0, 0, 0}), Model("floor"));
    }
    bool Bake() {
        NavMeshSettings s;
        s.tile_size = 24;
        return nav.Bake(s);
    }
    Entity Agent(const Vec3& at, NavAgent a = {}) { return world.CreateEntity(At(at), a); }
    NavAgent& Of(Entity e) { return *world.GetComponent<NavAgent>(e); }
    Vec3 Pos(Entity e) { return world.GetComponent<Transform>(e)->position; }
    // Runs until `done` or `seconds` pass; the time taken.
    template <typename F>
    f32 RunUntil(f32 seconds, F&& done) {
        f32 t = 0.0f;
        while (t < seconds) {
            nav.Update(0);
            crowd.Update(kDt);
            events.insert(events.end(), crowd.Events().begin(), crowd.Events().end());
            t += kDt;
            if (done()) break;
        }
        return t;
    }
    void Run(f32 seconds) {
        RunUntil(seconds, [] { return false; });
    }
};

} // namespace

AETHER_TEST(NavCrowd_WalksToAPoint) {
    Scene s;
    CHECK(s.Bake());
    const Entity e = s.Agent({-5, 0, 0});
    s.Of(e).MoveTo(Vec3(5, 0, 0));
    CHECK(s.Of(e).commands.size() == 1);
    s.Run(kDt);
    CHECK(s.crowd.AgentCount() == 1);
    CHECK(s.Of(e).IsMoving());
    CHECK(s.Of(e).commands.empty());
    CHECK(Near(s.Of(e).GetGoal().x, 5, 0.05f));
    CHECK(!s.crowd.Corners(e).empty());
    const f32 took = s.RunUntil(10, [&] { return !s.Of(e).IsMoving(); });
    CHECK(s.Of(e).HasArrived());
    CHECK(Flat(s.Pos(e), Vec3(5, 0, 0)) <= s.Of(e).stopping_distance + 0.05f);
    // 10 units at up to 3.5 per second, with speeding up and slowing down.
    CHECK(took > 10.0f / 3.5f && took < 6.0f);
    CHECK(s.events.size() == 1);
    CHECK(!s.events.empty() && s.events[0].entity == e && s.events[0].success);
    // It faces where it went (+X is a yaw of 90 degrees).
    const Quaternion& q = s.world.GetComponent<Transform>(e)->rotation;
    const f32 yaw = 2.0f * std::atan2(q.y, q.w) * 180.0f / kPi;
    CHECK(Near(yaw, 90.0f, 15.0f));
    // And comes to rest.
    s.Run(1.5f);
    CHECK(s.Of(e).GetSpeed() < 0.2f);
    CHECK(s.events.size() == 1);
    CHECK(Near(s.Pos(e).y, 0.0f, 0.25f));
}

AETHER_TEST(NavCrowd_GoesAroundWalls) {
    Scene s;
    s.world.CreateEntity(At({0, 0, 0}), Model("wall"));
    CHECK(s.Bake());
    NavAgent slow;
    slow.max_speed = 4.0f;
    const Entity e = s.Agent({-5, 0, 0}, slow);
    s.Of(e).MoveTo(Vec3(5, 0, 0));
    bool inside = false;
    f32 widest = 0.0f;
    s.RunUntil(15, [&] {
        const Vec3 p = s.Pos(e);
        inside = inside || (std::fabs(p.x) < 0.5f + 0.3f && std::fabs(p.z) < 6.0f);
        widest = std::max(widest, std::fabs(p.z));
        return !s.Of(e).IsMoving();
    });
    CHECK(!inside);
    CHECK(widest > 6.0f);
    CHECK(s.Of(e).HasArrived());
}

AETHER_TEST(NavCrowd_UnreachableGoals) {
    Scene s(false);
    s.world.CreateEntity(At({-6, 0, 0}), Model("pad"));
    s.world.CreateEntity(At({6, 0, 0}), Model("pad"));
    CHECK(s.Bake());
    const Entity e = s.Agent({-6, 0, 0});
    s.Of(e).MoveTo(Vec3(6, 0, 0));
    s.Run(kDt);
    CHECK(s.Of(e).HasFailed());
    CHECK(s.events.size() == 1 && !s.events[0].success);
    CHECK(Flat(s.Pos(e), Vec3(-6, 0, 0)) < 0.1f);
    // Far off the mesh fails too.
    s.Of(e).MoveTo(Vec3(50, 0, 50));
    s.Run(kDt);
    CHECK(s.Of(e).HasFailed());
    // Allowed partial: walks to the edge nearest the goal.
    s.Of(e).allow_partial = true;
    s.Of(e).MoveTo(Vec3(6, 0, 0));
    s.RunUntil(10, [&] { return !s.Of(e).IsMoving(); });
    CHECK(s.Of(e).HasArrived());
    // The pad's edge (3.6 in, give or take the mesh's simplification), less the stopping distance.
    CHECK(s.Pos(e).x > -4.4f && s.Pos(e).x < -3.0f);
    CHECK(std::fabs(s.Pos(e).z) < 0.3f);
}

AETHER_TEST(NavCrowd_AgentsAvoidEachOther) {
    Scene s;
    CHECK(s.Bake());
    const Entity a = s.Agent({-6, 0, 0.1f});
    const Entity b = s.Agent({6, 0, -0.1f});
    s.Of(a).MoveTo(Vec3(6, 0, 0));
    s.Of(b).MoveTo(Vec3(-6, 0, 0));
    f32 closest = 100.0f;
    s.RunUntil(15, [&] {
        closest = std::min(closest, Flat(s.Pos(a), s.Pos(b)));
        return !s.Of(a).IsMoving() && !s.Of(b).IsMoving();
    });
    CHECK(closest > 0.8f); // two radii of 0.5, give or take the steering
    CHECK(s.Of(a).HasArrived() && s.Of(b).HasArrived());
    CHECK(s.events.size() == 2);
}

AETHER_TEST(NavCrowd_FollowsAnEntity) {
    Scene s;
    CHECK(s.Bake());
    const Entity e = s.Agent({-6, 0, -6});
    const Entity target = s.world.CreateEntity(At({0, 0, 0}));
    NavAgent& c = s.Of(e);
    c.stopping_distance = 1.0f;
    c.MoveToEntity(target);
    s.Run(0.5f);
    CHECK(s.Of(e).IsMoving());
    // The target moves on; the agent goes after it.
    s.world.GetComponent<Transform>(target)->position = Vec3(6, 0, 6);
    s.RunUntil(15, [&] { return !s.Of(e).IsMoving(); });
    CHECK(s.Of(e).HasArrived());
    CHECK(Flat(s.Pos(e), Vec3(6, 0, 6)) < 1.2f);
    // A target that goes: the move fails.
    s.Of(e).MoveToEntity(target);
    s.world.GetComponent<Transform>(target)->position = Vec3(-6, 0, 6);
    s.Run(0.3f);
    CHECK(s.Of(e).IsMoving());
    s.world.DestroyEntity(target);
    s.Run(kDt);
    CHECK(s.Of(e).HasFailed());
    s.Of(e).MoveToEntity(target); // already gone
    s.Run(kDt);
    CHECK(s.Of(e).HasFailed());
}

AETHER_TEST(NavCrowd_StopAndWarp) {
    Scene s;
    CHECK(s.Bake());
    const Entity e = s.Agent({-6, 0, 0});
    s.Of(e).MoveTo(Vec3(6, 0, 0));
    s.Run(1.0f);
    s.Of(e).Stop();
    s.Run(kDt);
    CHECK(s.Of(e).status == NavMoveStatus::Idle);
    const Vec3 stopped = s.Pos(e);
    s.Run(1.5f);
    CHECK(Flat(s.Pos(e), stopped) < 1.5f); // it slows to a halt
    CHECK(s.Of(e).GetSpeed() < 0.2f);
    CHECK(s.events.empty()); // stopping isn't completing
    // Warping lands on the mesh.
    s.Of(e).Warp(Vec3(3, 2, 3));
    s.Run(kDt);
    CHECK(Flat(s.Pos(e), Vec3(3, 0, 3)) < 0.1f);
    CHECK(Near(s.Pos(e).y, 0.0f, 0.25f));
    s.Of(e).base_offset = 1.0f;
    s.Run(kDt);
    CHECK(Near(s.Pos(e).y, 1.0f, 0.25f));
    // Removing the component (or the entity) takes it out of the crowd.
    s.world.DestroyEntity(e);
    s.Run(kDt);
    CHECK(s.crowd.AgentCount() == 0);
}

AETHER_TEST(NavCrowd_ReplansAroundNewObstacles) {
    Scene s;
    CHECK(s.Bake());
    const Entity e = s.Agent({-6, 0, 0});
    s.Of(e).MoveTo(Vec3(6, 0, 0));
    s.Run(0.5f);
    // A crate lands across its way.
    NavObstacle crate;
    crate.half_extents = Vec3(0.5f, 1.0f, 3.0f);
    s.world.CreateEntity(At({0, 1, 0}), crate);
    bool inside = false;
    s.RunUntil(15, [&] {
        const Vec3 p = s.Pos(e);
        inside = inside || (std::fabs(p.x) < 0.5f && std::fabs(p.z) < 3.0f);
        return !s.Of(e).IsMoving();
    });
    CHECK(!inside);
    CHECK(s.Of(e).HasArrived());
    // A goal walled in later fails.
    NavObstacle ring;
    ring.shape = NavObstacleShape::Cylinder;
    ring.radius = 1.5f;
    const Entity cage = s.world.CreateEntity(At({-6, 1, 6}), ring);
    s.Of(e).MoveTo(Vec3(-6, 0, 6));
    s.Run(kDt);
    CHECK(s.Of(e).HasFailed());
    s.world.DestroyEntity(cage);
    s.Of(e).MoveTo(Vec3(-6, 0, 6));
    s.RunUntil(15, [&] { return !s.Of(e).IsMoving(); });
    CHECK(s.Of(e).HasArrived());
}

AETHER_TEST(NavCrowd_WaitsForAMeshAndRebakes) {
    Scene s;
    const Entity e = s.Agent({-5, 0, 0});
    s.Of(e).MoveTo(Vec3(5, 0, 0));
    s.Run(0.2f);
    CHECK(s.crowd.AgentCount() == 0);
    CHECK(s.Of(e).commands.size() == 1); // kept until there's a mesh
    CHECK(s.Bake());
    s.Run(1.0f);
    CHECK(s.Of(e).IsMoving());
    const f32 x = s.Pos(e).x;
    CHECK(x > -5.0f);
    // A fresh bake makes a new crowd; the agent carries on.
    CHECK(s.Bake());
    s.RunUntil(10, [&] { return !s.Of(e).IsMoving(); });
    CHECK(s.Of(e).HasArrived());
}

AETHER_TEST(NavCrowd_NavigationLibrary) {
    {
        World none;
        NavWorld empty(none, Meshes); // active, but not baked
        CHECK(!Navigation::IsReachable(Vec3(0, 0, 0), Vec3(1, 0, 0)));
        CHECK(Navigation::PathLength(Vec3(0, 0, 0), Vec3(1, 0, 0)) < 0.0f);
        CHECK(Flat(Navigation::ProjectPoint(Vec3(1, 2, 3)), Vec3(1, 2, 3)) < 1e-6f);
    }
    Scene s;
    s.world.CreateEntity(At({0, 0, 0}), Model("wall"));
    CHECK(s.Bake());
    CHECK(NavWorld::Active() == &s.nav);
    CHECK(Navigation::IsReachable(Vec3(-5, 0, 0), Vec3(5, 0, 0)));
    CHECK(Navigation::PathLength(Vec3(-5, 0, 0), Vec3(5, 0, 0)) > 14.0f);
    CHECK(Navigation::PathLength(Vec3(-5, 0, 0), Vec3(-5, 0, 3)) < 3.1f);
    const Vec3 on = Navigation::ProjectPoint(Vec3(-5, 3, 0));
    CHECK(Near(on.y, 0.0f, 0.25f));
    CHECK(Navigation::IsOnNavMesh(Vec3(-5, 0, 0)));
    CHECK(!Navigation::IsOnNavMesh(Vec3(0, 0, 0))); // in the wall
    for (int i = 0; i < 10; ++i) {
        const Vec3 r = Navigation::RandomReachablePoint(Vec3(-5, 0, 0), 2.0f);
        CHECK(Flat(r, Vec3(-5, 0, 0)) <= 2.0f + 1e-3f);
    }
    const Vec3 hit = Navigation::Raycast(Vec3(-5, 0, 0), Vec3(5, 0, 0));
    CHECK(hit.x < -0.9f && hit.x > -1.5f);
    const Vec3 clear = Navigation::Raycast(Vec3(-5, 0, -2), Vec3(-5, 0, 2));
    CHECK(Near(clear.z, 2.0f, 1e-3f));
}

AETHER_TEST(NavCrowd_BlueprintNodes) {
    RegisterNavAgentComponents();
    bp::Blueprint blueprint;
    bp::Graph events;
    events.name = "EventGraph";
    blueprint.graphs.push_back(events);
    bp::GraphBuilder b(*blueprint.FindGraph("EventGraph"));
    const bp::NodeId begin = b.Add("Event.BeginPlay");
    const bp::NodeId move = b.Add("Call.Native:NavAgent.MoveTo");
    const bp::NodeId random = b.Add("Call.Native:Navigation.RandomReachablePoint");
    b.Default(random, "origin", nlohmann::json::array({5, 0, 5})).Default(random, "radius", 0.0);
    b.Connect(begin, "then", random, "exec").Connect(random, "then", move, "exec").Connect(random, "return", move, "location");
    const bp::NodeId done = b.Add("Event.OnMoveCompleted"), branch = b.Add("Flow.Branch"), print = b.Add("Debug.Print");
    b.Connect(done, "then", branch, "exec").Connect(done, "success", branch, "condition").Connect(branch, "true", print, "exec");
    b.Default(print, "text", "arrived");
    bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
    for (const bp::Diagnostic& d : compiled.diagnostics.diagnostics) std::printf("    %s: %s\n", d.code.c_str(), d.message.c_str());
    CHECK(compiled.Ok());

    Scene s;
    CHECK(s.Bake());
    bp::BlueprintVM vm(s.world);
    std::vector<std::string> printed;
    vm.SetPrintHandler([&](Entity, const std::string& text) { printed.push_back(text); });
    const Entity e = s.Agent({-5, 0, -5});
    CHECK(vm.Attach(e, compiled.blueprint));
    vm.BeginPlay();
    CHECK(s.Of(e).commands.size() == 1);
    s.RunUntil(10, [&] {
        for (const MoveCompletedEvent& ev : s.crowd.Events()) {
            const bp::VmValue args[] = {ev.success};
            vm.Dispatch(ev.entity, NavCrowd::kMoveCompletedEvent, args);
        }
        return !printed.empty();
    });
    CHECK(printed == std::vector<std::string>{"arrived"});
    CHECK(Flat(s.Pos(e), Vec3(5, 0, 5)) < 0.4f);
}
