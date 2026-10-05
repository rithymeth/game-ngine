#include "test_framework.h"

#include "aether/assets/asset_guid.h"
#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/ecs/world.h"
#include "aether/gameplay/attribute_library.h"
#include "aether/gameplay/attribute_system.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/reflection/registry.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <limits>

// Phase 30 step 2 (§30.2): attributes, their clamps, change events, the
// Blueprint library and the player's Player.Attributes system.

using namespace aether;
using namespace aether::gas;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {
bool Near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }
Entity Make(World& w) { return w.CreateEntity(Transform{Vec3(), Quaternion::Identity()}); }
const f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
} // namespace

AETHER_TEST(AttributeSet_DefineGetAndSortedOrder) {
    AttributeSet s;
    CHECK(!s.Has("Health") && s.Get("Health", 7.0f) == 7.0f);
    CHECK(s.Define("Mana", 50, 0, 100) && s.Define("Health", 100, 0, 100) && s.Define("Armor", 5));
    CHECK(s.attributes.size() == 3 && s.attributes[0].name == "Armor" && s.attributes[1].name == "Health" && s.attributes[2].name == "Mana"); // sorted
    CHECK(Near(s.Get("Health"), 100) && Near(s.Get("Mana"), 50) && s.Has("Armor"));
    CHECK(!s.Define("", 1) && !s.Define("X", kNaN) && !s.Define("X", 1, kNaN, 2) && !s.Has("X"));
    // Defining again redefines: new bounds, base re-clamped.
    CHECK(s.Define("Health", 150, 0, 120) && Near(s.Get("Health"), 120) && s.attributes.size() == 3);
}

AETHER_TEST(AttributeSet_ClampsAndNanIsRejected) {
    AttributeSet s;
    s.Define("Health", 50, 0, 100);
    CHECK(s.SetBase("Health", 500) && Near(s.Get("Health"), 100) && Near(s.Find("Health")->base, 100)); // the cap
    CHECK(s.SetBase("Health", -20) && Near(s.Get("Health"), 0));
    CHECK(s.AddBase("Health", 30) && Near(s.Get("Health"), 30) && s.AddBase("Health", -10) && Near(s.Get("Health"), 20));
    CHECK(s.AddBase("Health", 1000) && Near(s.Get("Health"), 100) && s.AddBase("Health", -5) && Near(s.Get("Health"), 95)); // a capped base doesn't hide later drops
    CHECK(!s.SetBase("Health", kNaN) && !s.AddBase("Health", kNaN) && Near(s.Get("Health"), 95)); // NaN changes nothing
    CHECK(!s.SetBase("Nope", 1) && !s.AddBase("Nope", 1));
    // An upside-down range is the range the other way round.
    s.Define("Odd", 5, 10, 0);
    CHECK(Near(s.Find("Odd")->min, 0) && Near(s.Find("Odd")->max, 10) && Near(s.Get("Odd"), 5));
    // Hand-edited data is repaired.
    AttributeSet bad;
    bad.attributes = {{"b", 500, 999, 10, 0}, {"a", kNaN, 1, kNaN, kNaN}};
    bad.Normalize();
    CHECK(bad.attributes[0].name == "a" && bad.attributes[1].name == "b");
    CHECK(Near(bad.Get("b"), 10) && Near(bad.Find("b")->min, 0) && Near(bad.Find("b")->max, 10) && Near(bad.Get("a"), 0));
}

AETHER_TEST(AttributeSystem_QueuesAChangeOncePerRealChange) {
    World world;
    AttributeSystem sys(world);
    const Entity e = Make(world);
    CHECK(sys.Define(e, "Health", 100, 0, 100));
    CHECK(sys.Events().size() == 1 && sys.Events()[0].name == "Health" && Near(sys.Events()[0].old_value, 0) && Near(sys.Events()[0].new_value, 100)); // appeared
    sys.ClearEvents();
    CHECK(sys.AddBase(e, "Health", -30));
    CHECK(sys.Events().size() == 1 && sys.Events()[0].entity == e && Near(sys.Events()[0].old_value, 100) && Near(sys.Events()[0].new_value, 70));
    sys.ClearEvents();
    CHECK(sys.SetBase(e, "Health", 70) && sys.Events().empty()); // the same value: accepted, no event
    CHECK(sys.AddBase(e, "Health", 50) && sys.Events().size() == 1 && Near(sys.Events()[0].new_value, 100)); // 70 -> the cap
    sys.ClearEvents();
    CHECK(sys.AddBase(e, "Health", 10) && sys.Events().empty()); // already at the cap
    CHECK(!sys.SetBase(e, "Health", kNaN) && !sys.SetBase(e, "Nope", 1) && sys.Events().empty()); // refused: no event
    // Narrowing the bounds re-clamps and says so.
    CHECK(sys.Define(e, "Health", 100, 0, 80) && sys.Events().size() == 1 && Near(sys.Events()[0].old_value, 100) && Near(sys.Events()[0].new_value, 80));
    CHECK(Near(sys.Get(e, "Health"), 80) && Near(sys.GetBase(e, "Health"), 80) && Near(sys.GetMax(e, "Health"), 80) && Near(sys.GetMin(e, "Health"), 0));
    sys.ClearEvents();
    // An entity with no attributes, and a dead one.
    const Entity other = Make(world);
    CHECK(!sys.Has(other, "Health") && sys.Get(other, "Health", 3) == 3.0f && !sys.SetBase(other, "Health", 1));
    world.DestroyEntity(e);
    CHECK(!sys.Define(e, "X", 1) && sys.Get(e, "Health", -1) == -1.0f && !sys.Has(e, "Health"));
    CHECK(!sys.Define(kNullEntity, "X", 1));
}

AETHER_TEST(AttributeSet_ReflectsAndSurvivesASave) {
    AttributeSet s;
    s.Define("Health", 80, 0, 100);
    s.Define("Mana", 20, 0, 50);
    const reflect::Json j = reflect::ToJson(s);
    CHECK(j["attributes"].size() == 2 && j["attributes"][0]["name"] == "Health" && j["attributes"][0]["current"] == 80.0);
    AttributeSet back;
    CHECK(reflect::FromJson(back, j) && back.attributes.size() == 2 && Near(back.Get("Health"), 80) && Near(back.Find("Mana")->max, 50));
    CHECK(reflect::TypeRegistry::Find("AttributeSet") != nullptr && reflect::TypeRegistry::Find("Attribute") != nullptr);
    World world;
    const Entity e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, s);
    CHECK(world.GetComponent<AttributeSet>(e)->Has("Mana"));
}

AETHER_TEST(Attributes_BlueprintLibraryActsOnTheActiveSystem) {
    const reflect::TypeInfo* type = reflect::TypeRegistry::Find("Attributes");
    CHECK(type != nullptr && type->functions.size() == 8);
    World world;
    const Entity e = Make(world);
    CHECK(AttributeSystem::Active() == nullptr);
    CHECK(Attributes::GetAttribute(e, "Health", 9.0f) == 9.0f && !Attributes::HasAttribute(e, "Health") && !Attributes::DefineAttribute(e, "Health", 1, 0, 2) &&
          !Attributes::SetAttributeBase(e, "Health", 1) && !Attributes::AddAttributeBase(e, "Health", 1)); // no system: defaults, no effect
    {
        AttributeSystem sys(world);
        CHECK(AttributeSystem::Active() == &sys);
        CHECK(Attributes::DefineAttribute(e, "Health", 60, 0, 100) && Attributes::HasAttribute(e, "Health"));
        CHECK(Attributes::AddAttributeBase(e, "Health", 100) && Near(Attributes::GetAttribute(e, "Health", 0), 100));
        CHECK(Attributes::SetAttributeBase(e, "Health", 40) && Near(Attributes::GetAttributeBase(e, "Health", 0), 40) && Near(Attributes::GetAttributeMax(e, "Health", 0), 100) &&
              Near(Attributes::GetAttributeMin(e, "Health", -1), 0));
    }
    CHECK(AttributeSystem::Active() == nullptr); // cleared with the system
    // As nodes in a graph: BeginPlay defines Health and takes 30 off, and the event handler prints what it heard.
    World world2;
    AttributeSystem sys(world2);
    bp::Blueprint blueprint;
    bp::Graph events;
    events.name = "EventGraph";
    blueprint.graphs.push_back(events);
    bp::GraphBuilder b(*blueprint.FindGraph("EventGraph"));
    const bp::NodeId begin = b.Add("Event.BeginPlay"), self = b.Add("Entity.Self"), define = b.Add("Call.Native:Attributes.DefineAttribute"), add = b.Add("Call.Native:Attributes.AddAttributeBase");
    b.Default(define, "name", "Health").Default(define, "base", 100).Default(define, "min", 0).Default(define, "max", 100);
    b.Default(add, "name", "Health").Default(add, "delta", -30);
    b.Connect(begin, "then", define, "exec").Connect(define, "then", add, "exec").Connect(self, "self", define, "target").Connect(self, "self", add, "target");
    const bp::NodeId changed = b.Add("Event.OnAttributeChanged"), print = b.Add("Debug.Print");
    b.Connect(changed, "then", print, "exec").Connect(changed, "name", print, "text");
    bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
    for (const bp::Diagnostic& d : compiled.diagnostics.diagnostics) std::printf("    %s: %s\n", d.code.c_str(), d.message.c_str());
    CHECK(compiled.Ok());
    bp::BlueprintVM vm(world2);
    std::vector<std::string> printed;
    vm.SetPrintHandler([&](Entity, const std::string& text) { printed.push_back(text); });
    const Entity actor = Make(world2);
    CHECK(vm.Attach(actor, compiled.blueprint));
    vm.BeginPlay();
    CHECK(Near(sys.Get(actor, "Health"), 70));
    CHECK(sys.Events().size() == 2); // appeared at 100, then 70
    for (const AttributeEvent& ev : sys.Events()) {
        const bp::VmValue args[] = {ev.name, ev.old_value, ev.new_value};
        vm.Dispatch(ev.entity, AttributeSystem::kChangedEvent, args);
    }
    CHECK((printed == std::vector<std::string>{"Health", "Health"}));
}

AETHER_TEST(Attributes_PlayerDrainsChangesEveryFrame) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_gameplay_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array()}};
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()},
                                     {"assets", nlohmann::json::array({{{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Scenes/start.ascene"}, {"importer", "Scene"}}})},
                                     {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump(2));
    writer.Add("Content/Scenes/start.ascene", scene.dump(2));
    const std::filesystem::path file = dir / "Game.apak";
    std::string error;
    CHECK(writer.Write(file.string(), &error));
    player::GamePackage package;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    CHECK(game.LoadStartupScene(&error));
    AttributeSystem* sys = AttributeSystem::Active();
    CHECK(sys != nullptr);
    if (!sys) return;
    const Entity e = Make(game.GetWorld());
    CHECK(sys->Define(e, "Health", 100, 0, 100) && sys->Events().size() == 1);
    game.BeginPlay();
    game.Tick(0.016f);
    CHECK(sys->Events().empty()); // Player.Attributes took them
    CHECK(sys->AddBase(e, "Health", -10) && sys->Events().size() == 1);
    game.Tick(0.016f);
    CHECK(sys->Events().empty() && Near(sys->Get(e, "Health"), 90));
    // Loading another scene gives a fresh system for the new world.
    CHECK(game.LoadScene("Scenes/start.ascene", &error));
    CHECK(AttributeSystem::Active() != nullptr && !AttributeSystem::Active()->Has(e, "Health"));
}
