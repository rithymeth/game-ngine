#include "test_framework.h"

#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/save/save_bag.h"
#include "aether/save/save_games.h"
#include "aether/save/save_system.h"
#include "aether/reflection/registry.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/luau_host.h"
#include "aether/script/save_api.h"
#endif

#include <cmath>
#include <filesystem>

// Phase 28 step 4 (§28.4): the save bag, the SaveGames library, and its
// Blueprint and Luau faces.

using namespace aether;
using namespace aether::save;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace sg_test {
struct Lamp {
    bool on = false;
};
} // namespace sg_test
AETHER_REFLECT(sg_test::Lamp, 1, AETHER_FIELD(on, Field_EditAnywhere))

namespace {

stdfs::path Dir(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_save_games_tests" / name;
    stdfs::remove_all(dir);
    return dir;
}

bool Near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// A system made active for the test, and cleared after.
struct Active {
    SaveSystem system;
    explicit Active(const std::string& name) : system(Dir(name)) { system.MakeActive(); }
};

struct Scene {
    World world;
    GuidIndex guids;
    Entity lamp;
    EntityGuid guid{0x1A4B, 7};
    Scene() {
        (void)GetComponentId<sg_test::Lamp>();
        (void)GetComponentId<SaveableEntity>();
        lamp = world.CreateEntity(sg_test::Lamp{});
        world.AddComponent(lamp, IdComponent{guid});
        SaveableEntity s;
        s.tag = "lamp";
        s.fields = {"Lamp.on"};
        world.AddComponent(lamp, s);
        guids.Rebuild(world);
    }
};

} // namespace

AETHER_TEST(SaveBag_SetsGetsReplacesAndRemoves) {
    SaveBag bag;
    bag.SetInt("coins", 7);
    bag.SetFloat("hp", 12.5f);
    bag.SetBool("seen", true);
    bag.SetString("name", "Ada");
    bag.SetVector("spawn", Vec3(1, 2, 3));
    CHECK(bag.entries.size() == 5 && bag.Has("coins") && !bag.Has("nope"));
    CHECK(bag.GetInt("coins") == 7 && Near(bag.GetFloat("hp"), 12.5f) && bag.GetBool("seen") && bag.GetString("name") == "Ada");
    CHECK(Near(bag.GetVector("spawn").y, 2.0f));
    // The wrong kind, or a missing key, gives the fallback.
    CHECK(bag.GetInt("hp", -1) == -1 && bag.GetString("coins", "?") == "?" && bag.GetBool("name", true) && bag.GetInt("nope", 9) == 9);
    // A new value replaces the old, whatever its kind; keys stay unique.
    bag.SetString("coins", "seven");
    CHECK(bag.entries.size() == 5 && bag.GetString("coins") == "seven" && bag.GetInt("coins", -1) == -1);
    CHECK(bag.Remove("coins") && !bag.Remove("coins") && bag.entries.size() == 4);
    bag.has_world = true;
    bag.Clear();
    CHECK(bag.entries.empty() && !bag.has_world);
}

AETHER_TEST(SaveBag_IsAnOrdinarySaveStructShared_WithCode) {
    SaveSystem saves(Dir("Share"));
    SaveBag bag;
    bag.SetInt("coins", 7);
    bag.SetVector("spawn", Vec3(4, 5, 6));
    bag.SetString("name", "Ada");
    bag.world.destroyed.push_back(EntityGuid{1, 2});
    bag.has_world = true;
    CHECK(saves.Save("slot", bag).ok);
    SaveBag loaded;
    CHECK(saves.Load("slot", loaded).ok);
    CHECK(loaded.GetInt("coins") == 7 && Near(loaded.GetVector("spawn").z, 6.0f) && loaded.GetString("name") == "Ada");
    CHECK(loaded.has_world && loaded.world.destroyed.size() == 1 && loaded.world.destroyed[0] == EntityGuid({1, 2}));
}

AETHER_TEST(SaveGames_SaveLoadExistsDeleteAndList) {
    Active a("Library");
    CHECK(!SaveGames::DoesSaveExist("s1") && SaveGames::GetSlotCount() == 0);
    SaveGames::SetInt("coins", 7);
    SaveGames::SetString("name", "Ada");
    SaveGames::SetBool("done", true);
    SaveGames::SetFloat("hp", 3.5f);
    SaveGames::SetVector("pos", Vec3(1, 2, 3));
    CHECK(SaveGames::HasKey("coins") && SaveGames::SaveToSlot("s1") && SaveGames::DoesSaveExist("s1"));
    CHECK(SaveGames::SaveToSlot("alpha") && SaveGames::GetSlotCount() == 2 && SaveGames::GetSlotName(0) == "alpha" && SaveGames::GetSlotName(1) == "s1");
    CHECK(SaveGames::GetSlotName(5).empty() && SaveGames::GetSlotName(-1).empty());

    SaveGames::CreateSaveObject(); // a fresh, empty object
    CHECK(!SaveGames::HasKey("coins") && SaveGames::GetInt("coins", -1) == -1);
    CHECK(SaveGames::LoadFromSlot("s1"));
    CHECK(SaveGames::GetInt("coins", -1) == 7 && SaveGames::GetString("name", "") == "Ada" && SaveGames::GetBool("done", false));
    CHECK(Near(SaveGames::GetFloat("hp", 0), 3.5f) && Near(SaveGames::GetVector("pos", Vec3()).x, 1.0f));
    CHECK(SaveGames::RemoveKey("coins") && !SaveGames::HasKey("coins"));

    // Failures return false and say why; a failed load leaves the object alone.
    CHECK(!SaveGames::LoadFromSlot("missing") && !SaveGames::GetLastError().empty());
    CHECK(SaveGames::GetString("name", "") == "Ada");
    CHECK(!SaveGames::SaveToSlot("../evil") && SaveGames::GetLastError().find("valid slot name") != std::string::npos);
    CHECK(SaveGames::LoadFromSlot("s1") && SaveGames::GetLastError().empty()); // a success clears the message
    CHECK(SaveGames::DeleteSave("s1") && !SaveGames::DoesSaveExist("s1") && !SaveGames::DeleteSave("s1"));
}

AETHER_TEST(SaveGames_DoNothingWithoutAnActiveSystem) {
    CHECK(SaveSystem::Active() == nullptr);
    SaveGames::SetInt("x", 1);
    CHECK(SaveGames::GetInt("x", 5) == 5 && !SaveGames::HasKey("x") && !SaveGames::SaveToSlot("a") && !SaveGames::LoadFromSlot("a"));
    CHECK(!SaveGames::DoesSaveExist("a") && SaveGames::GetSlotCount() == 0 && SaveGames::GetLastError().empty());
    CHECK(!SaveGames::CaptureWorld() && !SaveGames::RestoreWorld());
    SaveGames::SaveToSlotAsync("a");
    SaveGames::CreateSaveObject();
}

AETHER_TEST(SaveGames_CaptureAndRestoreTheWorldThroughASlot) {
    Active a("World");
    Scene scene;
    CHECK(!SaveGames::CaptureWorld() && SaveGames::GetLastError().find("no world") != std::string::npos);
    SaveSystem::WorldContext context;
    context.world = &scene.world;
    context.guids = &scene.guids;
    a.system.SetWorldContext(context);
    CHECK(!SaveGames::RestoreWorld() && SaveGames::GetLastError().find("no world state") != std::string::npos);

    scene.world.GetComponent<sg_test::Lamp>(scene.lamp)->on = true;
    CHECK(SaveGames::CaptureWorld() && SaveGames::SaveToSlot("w"));
    // A new scene load, the lamp off again; load the slot and restore.
    Scene fresh;
    context.world = &fresh.world;
    context.guids = &fresh.guids;
    a.system.SetWorldContext(context);
    SaveGames::CreateSaveObject();
    CHECK(SaveGames::LoadFromSlot("w") && a.system.Bag().has_world);
    CHECK(!fresh.world.GetComponent<sg_test::Lamp>(fresh.lamp)->on);
    CHECK(SaveGames::RestoreWorld() && fresh.world.GetComponent<sg_test::Lamp>(fresh.lamp)->on);
    // A saved entity the scene no longer has makes Restore report it (and still restore the rest).
    a.system.Bag().world.entities.push_back(SavedEntity{EntityGuid{9, 9}, "gone", {}});
    CHECK(!SaveGames::RestoreWorld() && SaveGames::GetLastError().find("aren't in the scene") != std::string::npos);
}

AETHER_TEST(SaveGames_AsyncSavesReportThroughTheHost) {
    Active a("Async");
    SaveGames::SetInt("coins", 3);
    SaveGames::SaveToSlotAsync("later");
    SaveGames::SetInt("coins", 99); // changed right after: the save has 3
    a.system.Flush();
    CHECK(a.system.TakeFinishedSaves().empty()); // delivered by Pump
    CHECK(a.system.Pump() == 1);
    const std::vector<SaveSystem::FinishedSave> done = a.system.TakeFinishedSaves();
    CHECK(done.size() == 1 && done[0].slot == "later" && done[0].success && a.system.TakeFinishedSaves().empty());
    SaveGames::CreateSaveObject();
    CHECK(SaveGames::LoadFromSlot("later") && SaveGames::GetInt("coins", 0) == 3);
    SaveGames::SaveToSlotAsync("bad/slot");
    CHECK(a.system.Pump() == 1);
    const auto failed = a.system.TakeFinishedSaves();
    CHECK(failed.size() == 1 && !failed[0].success && !SaveGames::GetLastError().empty());
}

AETHER_TEST(SaveGames_IsReflectedForBlueprints) {
    const reflect::TypeInfo* type = reflect::TypeRegistry::Find("SaveGames");
    CHECK(type != nullptr && type->functions.size() == 23);
    for (const char* fn : {"SaveToSlot", "LoadFromSlot", "DoesSaveExist", "CreateSaveObject", "SetInt", "GetString", "CaptureWorld"}) {
        bool found = false;
        for (const reflect::FunctionInfo& f : type->functions) found = found || std::string(f.name) == fn;
        CHECK(found);
    }
}

AETHER_TEST(SaveGames_BlueprintNodesSaveAndLoad) {
    Active a("Blueprint");
    bp::Blueprint blueprint;
    bp::Graph events;
    events.name = "EventGraph";
    blueprint.graphs.push_back(events);
    bp::GraphBuilder b(*blueprint.FindGraph("EventGraph"));
    const bp::NodeId begin = b.Add("Event.BeginPlay");
    const bp::NodeId set = b.Add("Call.Native:SaveGames.SetString");
    const bp::NodeId save = b.Add("Call.Native:SaveGames.SaveToSlot");
    const bp::NodeId fresh = b.Add("Call.Native:SaveGames.CreateSaveObject");
    const bp::NodeId load = b.Add("Call.Native:SaveGames.LoadFromSlot");
    const bp::NodeId get = b.Add("Call.Native:SaveGames.GetString");
    const bp::NodeId print = b.Add("Debug.Print");
    b.Default(set, "key", "who").Default(set, "value", "Ada");
    b.Default(save, "slot", "s1").Default(load, "slot", "s1");
    b.Default(get, "key", "who").Default(get, "default", "nobody");
    b.Connect(begin, "then", set, "exec").Connect(set, "then", save, "exec").Connect(save, "then", fresh, "exec");
    b.Connect(fresh, "then", load, "exec").Connect(load, "then", print, "exec");
    b.Connect(get, "return", print, "text");
    // And the async event: its slot is printed when the host dispatches it.
    const bp::NodeId finished = b.Add("Event.OnSaveFinished"), print2 = b.Add("Debug.Print");
    b.Connect(finished, "then", print2, "exec").Connect(finished, "slot", print2, "text");
    bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
    for (const bp::Diagnostic& d : compiled.diagnostics.diagnostics) std::printf("    %s: %s\n", d.code.c_str(), d.message.c_str());
    CHECK(compiled.Ok());

    World world;
    bp::BlueprintVM vm(world);
    std::vector<std::string> printed;
    vm.SetPrintHandler([&](Entity, const std::string& text) { printed.push_back(text); });
    const Entity e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    CHECK(vm.Attach(e, compiled.blueprint));
    vm.BeginPlay();
    CHECK(printed == std::vector<std::string>{"Ada"}); // set, saved, cleared, loaded back
    CHECK(SaveGames::DoesSaveExist("s1"));
    // A game's C++ reads what the Blueprint saved.
    SaveBag bag;
    CHECK(a.system.Load("s1", bag).ok && bag.GetString("who") == "Ada");

    SaveGames::SaveToSlotAsync("s2");
    a.system.Flush();
    a.system.Pump();
    for (const SaveSystem::FinishedSave& f : a.system.TakeFinishedSaves()) {
        const bp::VmValue args[] = {f.slot, f.success};
        vm.Dispatch(e, "Event.OnSaveFinished", args);
    }
    CHECK(printed.size() == 2 && printed[1] == "s2");
}

#if AETHER_TEST_HAS_SCRIPTING
AETHER_TEST(SaveGames_LuauTable) {
    Active a("Luau");
    script::LuauHost host;
    script::InstallSaveApi(host);
    const auto number = [](const script::ScriptValue& v) { return std::holds_alternative<f64>(v) ? std::get<f64>(v) : -999.0; };
    script::ScriptResult r = host.Run(R"(
        SaveGames.SetNumber('coins', 7)
        SaveGames.SetNumber('ratio', 0.5)
        SaveGames.SetString('name', 'Ada')
        SaveGames.SetBool('done', true)
        SaveGames.SetVector('spawn', 1, 2, 3)
        local saved = SaveGames.SaveToSlot('lua')
        SaveGames.CreateSaveObject()
        local had = SaveGames.HasKey('coins')
        local loaded = SaveGames.LoadFromSlot('lua')
        local x, y, z = SaveGames.GetVector('spawn', 0, 0, 0)
        return saved, had, loaded, SaveGames.GetNumber('coins', -1), SaveGames.GetNumber('ratio', -1), SaveGames.GetString('name', ''),
               SaveGames.GetBool('done', false), x, y, z, SaveGames.DoesSaveExist('lua'), SaveGames.GetSlotCount(), SaveGames.GetSlotName(0)
    )", "save");
    CHECK(r.ok && r.values.size() == 13);
    if (r.ok && r.values.size() == 13) {
        CHECK(std::get<bool>(r.values[0]) && !std::get<bool>(r.values[1]) && std::get<bool>(r.values[2]));
        CHECK(number(r.values[3]) == 7.0 && number(r.values[4]) == 0.5 && std::get<std::string>(r.values[5]) == "Ada" && std::get<bool>(r.values[6]));
        CHECK(number(r.values[7]) == 1.0 && number(r.values[8]) == 2.0 && number(r.values[9]) == 3.0);
        CHECK(std::get<bool>(r.values[10]) && number(r.values[11]) == 1.0 && std::get<std::string>(r.values[12]) == "lua");
    }
    // Blueprint and Luau share the bag: an int a script saved is readable as an int.
    CHECK(SaveGames::GetInt("coins", -1) == 7 && Near(SaveGames::GetFloat("ratio", -1), 0.5f));
    // Failures: false and a message; wrong arguments are script errors.
    r = host.Run("return SaveGames.LoadFromSlot('nope'), SaveGames.GetLastError()", "fail");
    CHECK(r.ok && !std::get<bool>(r.values[0]) && !std::get<std::string>(r.values[1]).empty());
    CHECK(!host.Run("SaveGames.SetNumber('x', 'not a number')", "bad1").ok);
    CHECK(!host.Run("SaveGames.SaveToSlot(5)", "bad2").ok);
    CHECK(!host.Run("SaveGames.SetBool('x', 1)", "bad3").ok);
}
#endif
