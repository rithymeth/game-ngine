#include "aether/assets/asset_guid.h"
#include "aether/blueprint/system.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>

using namespace aether;
using namespace aether::bp;
using nlohmann::json;

namespace {

using Lines = std::vector<std::string>;

Blueprint EmptyBlueprint() {
    Blueprint bp;
    Graph g;
    g.name = "EventGraph";
    bp.graphs.push_back(g);
    return bp;
}

void AddVariable(Blueprint& bp, const std::string& name, const char* type) {
    Variable v;
    v.name = name;
    v.type = *ParseType(type);
    v.default_value = DefaultValue(v.type);
    bp.variables.push_back(v);
}

std::shared_ptr<const CompiledBlueprint> Compile(const Blueprint& bp) {
    CompileResult r = CompileBlueprint(bp);
    for (const Diagnostic& d : r.diagnostics.diagnostics) {
        if (d.severity == Severity::Error) std::printf("    %s node %u: %s\n", d.code.c_str(), d.node, d.message.c_str());
    }
    AETHER_CHECK(r.Ok());
    return r.blueprint;
}

NodeId Print(GraphBuilder& b, NodeId after, const std::string& pin, NodeId value, const std::string& out) {
    const NodeId p = b.Add("Debug.Print");
    b.Connect(after, pin, p, "exec").Connect(value, out, p, "text");
    return p;
}

NodeId PrintText(GraphBuilder& b, NodeId after, const std::string& pin, const std::string& text) {
    const NodeId p = b.Add("Debug.Print");
    b.Default(p, "text", text);
    b.Connect(after, pin, p, "exec");
    return p;
}

bool Near(const Vec3& a, const Vec3& b) {
    return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f && std::fabs(a.z - b.z) < 1e-4f;
}

} // namespace

AETHER_TEST(BlueprintWorld_TransformsAndRotation) {
    World world;
    Blueprint bp = EmptyBlueprint();
    AddVariable(bp, "forward", "Vec3");
    AddVariable(bp, "where", "Vec3");
    GraphBuilder b(*bp.FindGraph("EventGraph"));
    const NodeId begin = b.Add("Event.BeginPlay"), set = b.Add("Entity.SetLocation"), offset = b.Add("Entity.AddOffset");
    b.Default(set, "location", json::array({1, 2, 3})).Default(offset, "offset", json::array({1, 0, 0}));
    b.Connect(begin, "then", set, "exec").Connect(set, "then", offset, "exec");
    const NodeId get = b.Add("Entity.GetLocation"), store = b.Add("Var.Set:where");
    b.Connect(offset, "then", store, "exec").Connect(get, "location", store, "value");
    // Turn 90 degrees about +Y, then forward = rotation * (0, 0, 1).
    const NodeId turn = b.Add("Quat.FromAxisAngle"), rot = b.Add("Entity.SetRotation");
    b.Default(turn, "degrees", 90.0);
    b.Connect(store, "then", rot, "exec").Connect(turn, "result", rot, "rotation");
    const NodeId now = b.Add("Entity.GetRotation"), fwd = b.Add("Quat.RotateVector"), keep = b.Add("Var.Set:forward");
    b.Connect(now, "rotation", fwd, "rotation").Connect(fwd, "result", keep, "value").Connect(rot, "then", keep, "exec");
    // Poke: reading the transform of an entity without one (BP201, defaults).
    const NodeId poke = b.Add("Event.Custom", {{"name", "Poke"}, {"params", {{{"name", "who"}, {"type", "Entity"}}}}});
    const NodeId theirs = b.Add("Entity.GetLocation");
    b.Connect(poke, "who", theirs, "target");
    Print(b, poke, "then", theirs, "location");

    BlueprintVM vm(world);
    Lines printed;
    vm.SetPrintHandler([&](Entity, const std::string& t) { printed.push_back(t); });
    const Entity e = world.CreateEntity(Transform{});
    AETHER_CHECK(vm.Attach(e, Compile(bp)));
    AETHER_CHECK(vm.Dispatch(e, "Event.BeginPlay"));
    AETHER_CHECK(Near(world.GetComponent<Transform>(e)->position, {2, 2, 3}));
    AETHER_CHECK(Near(std::get<Vec3>(vm.GetVariable(e, "where")), {2, 2, 3}));
    AETHER_CHECK(Near(std::get<Vec3>(vm.GetVariable(e, "forward")), {1, 0, 0}));

    const Entity bare = world.CreateEntity();
    const VmValue who = bare;
    AETHER_CHECK(vm.Dispatch(e, "Event.Custom:Poke", std::span<const VmValue>(&who, 1)));
    AETHER_CHECK(printed == Lines{"(0, 0, 0)"});
    AETHER_CHECK(vm.Errors().size() == 1 && vm.Errors()[0].code == "BP201" && vm.Errors()[0].node == theirs);
}

AETHER_TEST(BlueprintWorld_TagsHierarchyAndTime) {
    World world;
    GuidIndex guids;
    Blueprint bp = EmptyBlueprint();
    GraphBuilder b(*bp.FindGraph("EventGraph"));
    // Tag(other): tag self and other "enemy", print how many there are, remove other's, print again.
    const NodeId tag = b.Add("Event.Custom", {{"name", "Tag"}, {"params", {{{"name", "other"}, {"type", "Entity"}}}}});
    const NodeId mine = b.Add("Entity.AddTag"), theirs = b.Add("Entity.AddTag");
    b.Default(mine, "tag", "enemy").Default(theirs, "tag", "enemy");
    b.Connect(tag, "then", mine, "exec").Connect(mine, "then", theirs, "exec").Connect(tag, "other", theirs, "target");
    auto count = [&](NodeId after) {
        const NodeId find = b.Add("Entity.FindWithTag"), len = b.Add("Array.Length:Entity");
        b.Default(find, "tag", "enemy");
        b.Connect(find, "entities", len, "array");
        return Print(b, after, "then", len, "result");
    };
    const NodeId c1 = count(theirs);
    const NodeId untag = b.Add("Entity.RemoveTag");
    b.Default(untag, "tag", "enemy");
    b.Connect(c1, "then", untag, "exec").Connect(tag, "other", untag, "target");
    const NodeId c2 = count(untag);
    const NodeId has = b.Add("Entity.HasTag");
    b.Default(has, "tag", "enemy");
    Print(b, c2, "then", has, "result");
    // Hang(parent): attach self to parent, print parent == Get Parent, world location, detach, valid parent?
    const NodeId hang = b.Add("Event.Custom", {{"name", "Hang"}, {"params", {{{"name", "parent"}, {"type", "Entity"}}}}});
    const NodeId attach = b.Add("Entity.AttachTo"), gp = b.Add("Entity.GetParent"), same = b.Add("Math.Equal:Entity");
    b.Connect(hang, "then", attach, "exec").Connect(hang, "parent", attach, "parent");
    b.Connect(gp, "parent", same, "a").Connect(hang, "parent", same, "b");
    const NodeId p1 = Print(b, attach, "then", same, "result");
    const NodeId wl = b.Add("Entity.GetWorldLocation");
    const NodeId p2 = Print(b, p1, "then", wl, "location");
    const NodeId detach = b.Add("Entity.Detach"), gp2 = b.Add("Entity.GetParent"), valid = b.Add("Entity.IsValid");
    b.Connect(p2, "then", detach, "exec").Connect(gp2, "parent", valid, "entity");
    Print(b, detach, "then", valid, "result");
    // Clock: game time and delta.
    const NodeId clock = b.Add("Event.Custom", {{"name", "Clock"}});
    const NodeId gt = b.Add("World.GameTime"), dt = b.Add("World.DeltaSeconds");
    Print(b, Print(b, clock, "then", gt, "seconds"), "then", dt, "seconds");

    BlueprintVM vm(world);
    vm.SetGuidIndex(&guids);
    Lines printed;
    vm.SetPrintHandler([&](Entity, const std::string& t) { printed.push_back(t); });
    auto spawn = [&](Vec3 at) {
        Transform t;
        t.position = at;
        const Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, t);
        guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
        return e;
    };
    const Entity self = spawn({1, 0, 0}), other = spawn({0, 0, 0}), parent = spawn({10, 5, 0});
    AETHER_CHECK(vm.Attach(self, Compile(bp)));

    VmValue arg = other;
    AETHER_CHECK(vm.Dispatch(self, "Event.Custom:Tag", std::span<const VmValue>(&arg, 1)));
    AETHER_CHECK(printed == (Lines{"2", "1", "true"}));
    AETHER_CHECK(HasTag(world, self, "enemy") && !HasTag(world, other, "enemy"));

    printed.clear();
    arg = parent;
    AETHER_CHECK(vm.Dispatch(self, "Event.Custom:Hang", std::span<const VmValue>(&arg, 1)));
    AETHER_CHECK(printed == (Lines{"true", "(11, 5, 0)", "false"}));
    AETHER_CHECK(world.GetComponent<Parent>(self) == nullptr);

    // A cycle is refused (BP201), not made: parent can't hang under its own child.
    AETHER_CHECK(vm.Attach(parent, Compile(bp)));
    world.AddComponent(self, Parent{world.GetComponent<IdComponent>(parent)->guid});
    arg = self;
    vm.Dispatch(parent, "Event.Custom:Hang", std::span<const VmValue>(&arg, 1));
    AETHER_CHECK(world.GetComponent<Parent>(parent) == nullptr);
    AETHER_CHECK(std::any_of(vm.Errors().begin(), vm.Errors().end(),
                             [](const RuntimeError& e) { return e.code == "BP201"; }));

    printed.clear();
    vm.Tick(0.5f);
    AETHER_CHECK(vm.Dispatch(self, "Event.Custom:Clock"));
    AETHER_CHECK(printed == (Lines{"0.5", "0.5"}));
}

AETHER_TEST(BlueprintWorld_DestroyIsDeferredAndSpawnUsesTheSpawner) {
    World world;
    // BP_Bullet: BeginPlay prints "bullet"; EndPlay prints "bye".
    Blueprint bullet = EmptyBlueprint();
    {
        GraphBuilder b(*bullet.FindGraph("EventGraph"));
        PrintText(b, b.Add("Event.BeginPlay"), "then", "bullet");
        PrintText(b, b.Add("Event.EndPlay"), "then", "bye");
    }
    const assets::AssetGuid bullet_guid = assets::NewAssetGuid();
    // BP_Gun: Fire spawns a bullet at (0, 1, 0) and prints its location; Die
    // destroys itself, prints "after" (the event finishes first), and EndPlay says "gun down".
    Blueprint gun = EmptyBlueprint();
    {
        GraphBuilder b(*gun.FindGraph("EventGraph"));
        const NodeId fire = b.Add("Event.Custom", {{"name", "Fire"}});
        const NodeId spawn = b.Add("Entity.Spawn", {{"blueprint", assets::ToString(bullet_guid)}});
        b.Default(spawn, "location", json::array({0, 1, 0}));
        b.Connect(fire, "then", spawn, "exec");
        const NodeId where = b.Add("Entity.GetLocation");
        b.Connect(spawn, "spawned", where, "target");
        Print(b, spawn, "then", where, "location");
        const NodeId die = b.Add("Event.Custom", {{"name", "Die"}}), destroy = b.Add("Entity.Destroy");
        b.Connect(die, "then", destroy, "exec");
        PrintText(b, destroy, "then", "after");
        PrintText(b, b.Add("Event.EndPlay"), "then", "gun down");
    }
    std::shared_ptr<const CompiledBlueprint> bullet_bp = Compile(bullet), gun_bp = Compile(gun);

    BlueprintVM vm(world);
    Lines printed;
    vm.SetPrintHandler([&](Entity, const std::string& t) { printed.push_back(t); });
    const Entity g = world.CreateEntity(Transform{});
    AETHER_CHECK(vm.Attach(g, gun_bp));

    // Without a spawner: no entity, BP206.
    AETHER_CHECK(vm.Dispatch(g, "Event.Custom:Fire"));
    AETHER_CHECK(printed == Lines{"(0, 0, 0)"} && vm.Errors().size() == 2);
    AETHER_CHECK(vm.Errors()[0].code == "BP206");

    printed.clear();
    std::vector<Entity> bullets;
    vm.SetSpawnHandler([&](const assets::AssetGuid& guid, const Transform& t) {
        AETHER_CHECK(guid == bullet_guid);
        const Entity b = world.CreateEntity(t);
        vm.Attach(b, bullet_bp);
        vm.Dispatch(b, "Event.BeginPlay");
        bullets.push_back(b);
        return b;
    });
    AETHER_CHECK(vm.Dispatch(g, "Event.Custom:Fire"));
    AETHER_CHECK(printed == (Lines{"bullet", "(0, 1, 0)"}) && bullets.size() == 1);

    printed.clear();
    AETHER_CHECK(vm.Dispatch(g, "Event.Custom:Die"));
    AETHER_CHECK(printed == (Lines{"after", "gun down"}));
    AETHER_CHECK(!world.IsAlive(g) && !vm.IsAttached(g));
}

AETHER_TEST(BlueprintWorld_SystemSpawnsAndDestroysThroughTheLifecycle) {
    World world;
    GuidIndex guids;
    Lifecycle lifecycle(world, guids);
    BlueprintSystem system(world);
    Lines printed;
    system.VM().SetPrintHandler([&](Entity, const std::string& t) { printed.push_back(t); });

    const assets::AssetGuid bullet_guid = assets::NewAssetGuid(), gun_guid = assets::NewAssetGuid();
    Blueprint bullet = EmptyBlueprint();
    {
        GraphBuilder b(*bullet.FindGraph("EventGraph"));
        PrintText(b, b.Add("Event.BeginPlay"), "then", "bullet");
        PrintText(b, b.Add("Event.EndPlay"), "then", "bullet gone");
        const NodeId pop = b.Add("Event.Custom", {{"name", "Pop"}}), destroy = b.Add("Entity.Destroy");
        b.Connect(pop, "then", destroy, "exec");
    }
    Blueprint gun = EmptyBlueprint();
    AddVariable(gun, "last", "Entity");
    {
        GraphBuilder b(*gun.FindGraph("EventGraph"));
        const NodeId begin = b.Add("Event.BeginPlay"), spawn = b.Add("Entity.Spawn", {{"blueprint", assets::ToString(bullet_guid)}});
        const NodeId keep = b.Add("Var.Set:last");
        b.Connect(begin, "then", spawn, "exec").Connect(spawn, "then", keep, "exec").Connect(spawn, "spawned", keep, "value");
    }
    system.SetLoader([&](const assets::AssetGuid& guid, Blueprint& out, std::string& name) {
        if (guid == bullet_guid) out = bullet, name = "BP_Bullet";
        else if (guid == gun_guid) out = gun, name = "BP_Gun";
        else return false;
        return true;
    });
    system.Register(lifecycle);

    BlueprintInstance instance;
    instance.blueprint.guid = gun_guid;
    const Entity g = world.CreateEntity(IdComponent{NewEntityGuid()}, Transform{}, instance);
    guids.Add(world.GetComponent<IdComponent>(g)->guid, g);

    lifecycle.BeginPlay(); // the gun spawns a bullet: attached now, started at the next sync
    const Entity b = std::get<Entity>(system.VM().GetVariable(g, "last"));
    AETHER_CHECK(world.IsAlive(b) && system.VM().IsAttached(b) && world.GetComponent<IdComponent>(b) != nullptr);
    lifecycle.Update(1.0f / 60.0f);
    system.Update(1.0f / 60.0f);
    AETHER_CHECK(std::count(printed.begin(), printed.end(), "bullet") == 1);

    AETHER_CHECK(system.VM().Dispatch(b, "Event.Custom:Pop"));
    AETHER_CHECK(!world.IsAlive(b) && !system.VM().IsAttached(b));
    AETHER_CHECK(std::count(printed.begin(), printed.end(), "bullet gone") == 1); // once, not twice
    AETHER_CHECK(system.VM().Errors().empty());
}
