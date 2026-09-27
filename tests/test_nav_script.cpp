#include "aether/nav/crowd.h"
#include "aether/script/luau_host.h"
#include "test_framework.h"

#include <cstdio>
#include <cstring>

// Phase 20 step 3: NavAgent's methods from Luau.

using namespace aether;
using namespace aether::nav;
using namespace aether::script;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {
bool Floor(const ModelRenderer& m, std::vector<Vec3>& v, std::vector<u32>& i) {
    if (std::strcmp(m.asset_path, "floor") != 0) return false;
    NavGeometry g;
    g.AddPlane(Vec3(0, 0, 0), 10, 10);
    v = g.vertices;
    i = g.indices;
    return true;
}
} // namespace

AETHER_TEST(NavScript_AgentFromLuau) {
    World world;
    GuidIndex guids;
    LuauHost host;
    RegisterNavAgentComponents();
    (void)GetComponentId<Transform>();
    ModelRenderer floor;
    SetModelPath(floor, "floor");
    world.CreateEntity(Transform{}, floor);
    NavWorld nav(world, Floor);
    CHECK(nav.Bake({}));
    NavCrowd crowd(world, nav);
    const Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, Transform{Vec3(-5, 0, 0), Quaternion::Identity()}, NavAgent{});
    guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
    host.BindWorld(&world, &guids);
    host.SetGlobal("agent", EntityRef{e});

    ScriptResult r = host.Run(R"(
        local a = agent:Get("NavAgent")
        a.max_speed = 5
        a:MoveTo(vector.create(5, 0, 0))
        return a:IsMoving()
    )");
    CHECK(r.ok);
    if (!r.ok) std::printf("    %s\n", r.error.c_str());
    CHECK(world.GetComponent<NavAgent>(e)->commands.size() == 1);
    CHECK(world.GetComponent<NavAgent>(e)->max_speed == 5.0f);
    for (int i = 0; i < 300 && !world.GetComponent<NavAgent>(e)->HasArrived(); ++i) crowd.Update(1.0f / 30.0f);
    r = host.Run(R"(
        local a = agent:Get("NavAgent")
        return a:HasArrived(), a:GetRemainingDistance(), a.status
    )");
    CHECK(r.ok && r.values.size() == 3);
    if (r.ok && r.values.size() == 3) {
        CHECK(std::get<bool>(r.values[0]));
        CHECK(std::get<f64>(r.values[1]) < 0.35);
    }
}
