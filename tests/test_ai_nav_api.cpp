#include "aether/ai/perception.h"
#include "aether/nav/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/script/ai_api.h"
#include "aether/script/luau_host.h"
#include "test_framework.h"

#include <cstdio>
#include <cstring>

// M8.8 step 1: the `AI` and `Nav` script tables (perception and path queries).

using namespace aether;
using namespace aether::nav;
using namespace aether::script;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {
// A 20x20 floor with a solid 2x2 pillar in the middle (a hole in the mesh).
bool FloorWithPillar(const ModelRenderer& m, std::vector<Vec3>& v, std::vector<u32>& i) {
    if (std::strcmp(m.asset_path, "floor") != 0) return false;
    NavGeometry g;
    g.AddPlane(Vec3(0, 0, 0), 10, 10);
    g.AddBox(Vec3(0, 1.5f, 0), Vec3(1, 1.5f, 1));
    v = g.vertices;
    i = g.indices;
    return true;
}
Entity Actor(World& world, GuidIndex& guids, const Vec3& at) {
    const Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, Transform{at, Quaternion::Identity()});
    guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
    return e;
}
void Report(const ScriptResult& r) {
    if (!r.ok) std::printf("    %s\n", r.error.c_str());
}
} // namespace

AETHER_TEST(AINavApi_CanSeeWithOccluder) {
    World world;
    GuidIndex guids;
    LuauHost host;
    InstallAiApi(host);
    (void)GetComponentId<Transform>();
    ModelRenderer floor;
    SetModelPath(floor, "floor");
    world.CreateEntity(Transform{}, floor);
    NavWorld nav(world, FloorWithPillar);
    CHECK(nav.Bake({}));
    ai::PerceptionWorld perception(world);
    perception.UseNavMeshLineOfSight(nav.Mesh());

    const Entity guard = Actor(world, guids, Vec3(-6, 0, 0));
    const Entity behind = Actor(world, guids, Vec3(6, 0, 0));   // the pillar is between them
    const Entity beside = Actor(world, guids, Vec3(-6, 0, 6));  // a clear line
    host.BindWorld(&world, &guids);
    host.SetGlobal("guard", EntityRef{guard});
    host.SetGlobal("behind", EntityRef{behind});
    host.SetGlobal("beside", EntityRef{beside});
    const ScriptResult r = host.Run("return AI.CanSee(guard, behind), AI.CanSee(guard, beside), AI.CanSee(guard, beside, 3), AI.GetKnown(guard)");
    Report(r);
    CHECK(r.ok && r.values.size() == 4);
    if (r.ok && r.values.size() == 4) {
        CHECK(std::get<bool>(r.values[0]) == false);
        CHECK(std::get<bool>(r.values[1]) == true);
        CHECK(std::get<bool>(r.values[2]) == false); // 6 m away, beyond 3
        CHECK(std::holds_alternative<std::monostate>(r.values[3])); // no AIPerception
    }

    // An AIPerception's known actors.
    AIStimuliSource src;
    world.AddComponent(beside, src);
    AIPerception eyes;
    eyes.fov_degrees = 360.0f;
    world.AddComponent(guard, eyes);
    perception.Update(0.1f);
    const ScriptResult k = host.Run(R"(
        local known = AI.GetKnown(guard)
        return #known, known[1] == beside, AI.GetTarget(guard) == beside, AI.GetTarget(behind)
    )");
    Report(k);
    CHECK(k.ok && k.values.size() == 4);
    if (k.ok && k.values.size() == 4) {
        CHECK(std::get<f64>(k.values[0]) == 1.0);
        CHECK(std::get<bool>(k.values[1]));
        CHECK(std::get<bool>(k.values[2]));
        CHECK(std::holds_alternative<std::monostate>(k.values[3]));
    }
    // Wrong arguments are script errors.
    CHECK(!host.Run("return AI.CanSee(1, 2)").ok);
}

AETHER_TEST(AINavApi_FindPathAroundPillar) {
    World world;
    GuidIndex guids;
    LuauHost host;
    InstallAiApi(host);
    (void)GetComponentId<Transform>();
    ModelRenderer floor;
    SetModelPath(floor, "floor");
    world.CreateEntity(Transform{}, floor);
    NavWorld nav(world, FloorWithPillar);
    CHECK(nav.Bake({}));
    nav.MakeActive();
    const Entity walker = Actor(world, guids, Vec3(-6, 0, 0));
    host.BindWorld(&world, &guids);
    host.SetGlobal("walker", EntityRef{walker});
    ScriptResult r = host.Run(R"(
        local path, complete = Nav.FindPath(vector.create(-6, 0, 0), vector.create(6, 0, 0))
        local last = path[#path]
        return Nav.IsReady(), #path, complete, math.abs(last.x - 6) < 0.5 and math.abs(last.z) < 0.5
    )");
    Report(r);
    CHECK(r.ok && r.values.size() == 4);
    if (r.ok && r.values.size() == 4) {
        CHECK(std::get<bool>(r.values[0]));
        CHECK(std::get<f64>(r.values[1]) >= 3.0); // it bends round the pillar
        CHECK(std::get<bool>(r.values[2]));
        CHECK(std::get<bool>(r.values[3]));
    }
    // MoveTowards steps along the path, then arrives.
    r = host.Run(R"(
        local arrived, left = Nav.MoveTowards(walker, vector.create(6, 0, 0), 2, 0.5)
        local first = left
        for i = 1, 40 do
            arrived, left = Nav.MoveTowards(walker, vector.create(6, 0, 0), 2, 0.5)
            if arrived then break end
        end
        return first > 10, arrived
    )");
    Report(r);
    CHECK(r.ok && r.values.size() == 2 && std::get<bool>(r.values[0]) && std::get<bool>(r.values[1]));
    const Vec3 p = world.GetComponent<Transform>(walker)->position;
    CHECK(std::abs(p.x - 6.0f) < 0.5f && std::abs(p.z) < 0.5f);
}

AETHER_TEST(AINavApi_NoNavMeshIsNil) {
    World world;
    GuidIndex guids;
    LuauHost host;
    InstallAiApi(host);
    (void)GetComponentId<Transform>();
    CHECK(NavWorld::Active() == nullptr);
    const Entity walker = Actor(world, guids, Vec3(1, 0, 2));
    host.BindWorld(&world, &guids);
    host.SetGlobal("walker", EntityRef{walker});
    const ScriptResult r = host.Run(R"(
        local arrived, left = Nav.MoveTowards(walker, vector.create(5, 0, 5), 2, 1)
        return Nav.IsReady(), Nav.FindPath(vector.create(0, 0, 0), vector.create(1, 0, 1)), arrived, left
    )");
    Report(r);
    CHECK(r.ok && r.values.size() == 4);
    if (r.ok && r.values.size() == 4) {
        CHECK(std::get<bool>(r.values[0]) == false);
        CHECK(std::holds_alternative<std::monostate>(r.values[1]));
        CHECK(std::get<bool>(r.values[2]) == false);
        CHECK(std::holds_alternative<std::monostate>(r.values[3]));
    }
    CHECK(world.GetComponent<Transform>(walker)->position.x == 1.0f);
}
