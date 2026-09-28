#include "aether/ai/bt_world.h"
#include "aether/scene/entity_guid.h"
#include "aether/script/luau_host.h"
#include "test_framework.h"

#include <cstdio>

// Phase 20 step 4: Behavior Tree tasks and services written in Luau, and
// the BehaviorTreeComponent's methods from Luau.

using namespace aether;
using namespace aether::ai;
using namespace aether::script;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

AETHER_TEST(AIScript_LuauTasks) {
    World world;
    GuidIndex guids;
    LuauHost host;
    RegisterBehaviorTreeComponents();
    BehaviorTreeAsset tree;
    tree.blackboard.Add("Seen", BlackboardType::Int, 0);
    BtNode think;
    think.type = BtNodeType::RunLuau;
    think.event = "Think";
    BtNode log;
    log.type = BtNodeType::Log;
    log.event = "thought";
    BtNode wait;
    wait.type = BtNodeType::Wait;
    wait.seconds = 100;
    tree.root.type = BtNodeType::Sequence;
    tree.root.children = {think, log, wait};
    BtService scan;
    scan.type = BtServiceType::Luau;
    scan.event = "Scan";
    scan.interval = 0.25f;
    tree.root.services = {scan};
    CHECK(ValidateBehaviorTree(tree).empty());

    BehaviorTreeWorld bt(world, [&](const std::string&) { return &tree; });
    std::vector<std::string> logs;
    bt.hooks.log = [&](Entity, const std::string& t) { logs.push_back(t); };
    bt.hooks.run_luau = [&](Entity e, const std::string& fn, f32 dt, bool first) {
        const ScriptResult r = host.Call(fn, {EntityRef{e}, static_cast<f64>(dt), first});
        if (!r.ok || r.values.empty() || !std::holds_alternative<std::string>(r.values[0])) return BtStatus::Failure;
        const std::string& s = std::get<std::string>(r.values[0]);
        return s == "running" ? BtStatus::Running : s == "success" ? BtStatus::Success : BtStatus::Failure;
    };
    bt.hooks.luau_service = [&](Entity e, const std::string& fn, f32 dt) { host.Call(fn, {EntityRef{e}, static_cast<f64>(dt)}); };

    BehaviorTreeComponent c;
    c.tree = "Thinker";
    const Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, c);
    guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
    host.BindWorld(&world, &guids);
    ScriptResult r = host.Run(R"(
        thinking = 0
        starts = 0
        function Think(entity, dt, first)
            if first then starts += 1 end
            thinking += 1
            if thinking < 3 then return "running" end
            return "success"
        end
        function Scan(entity, dt)
            local brain = entity:Get("BehaviorTreeComponent")
            brain:SetInt("Seen", brain:GetInt("Seen") + 1)
        end
    )");
    CHECK(r.ok);
    if (!r.ok) std::printf("    %s\n", r.error.c_str());
    for (int i = 0; i < 10; ++i) bt.Update(0.1f);
    r = host.Run("return thinking, starts");
    CHECK(r.ok && r.values.size() == 2 && std::get<f64>(r.values[0]) == 3.0 && std::get<f64>(r.values[1]) == 1.0);
    CHECK(logs == std::vector<std::string>{"thought"});
    // The service ran every quarter second (from the first tick), through the component's methods.
    const i32 seen = world.GetComponent<BehaviorTreeComponent>(e)->GetInt("Seen");
    CHECK(seen >= 4 && seen <= 5);
    host.SetGlobal("guard", EntityRef{e});
    r = host.Run(R"(
        local brain = guard:Get("BehaviorTreeComponent")
        return brain:IsRunning(), brain:GetActiveNode(), brain:SetString("Seen", "x")
    )");
    CHECK(r.ok && r.values.size() == 3);
    if (r.ok && r.values.size() == 3) {
        CHECK(std::get<bool>(r.values[0]));
        CHECK(std::get<std::string>(r.values[1]) == "Wait");
        CHECK(!std::get<bool>(r.values[2])); // the wrong type
    }
}
