#include "test_framework.h"

#ifdef AETHER_TEST_HAS_ACHIEVEMENTS

#include "aether/achievements/achievement_system.h"
#include "aether/achievements/achievements_kit.h"
#include "aether/assets/asset_guid.h"
#include "aether/ecs/world.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/player/user_paths.h"
#include "aether/save/save_system.h"
#include "aether/scene/components.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>

// The achievements kit: goals (`.aachievement`) as data, with progress kept in the game's save bag.

using namespace aether;
using namespace aether::ach;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

AchievementDef Goal(const char* name, i32 target = 1) {
    AchievementDef a;
    a.name = name;
    a.target = target;
    return a;
}

struct Rig {
    save::SaveBag bag;
    AchievementLibrary library;
    AchievementSystem system{bag, library};

    Rig() {
        AchievementDef first = Goal("FirstSteps");
        first.title = "First Steps";
        first.description = "Walk out of the camp.";
        CHECK(library.Register(first));
        CHECK(library.Register(Goal("Salvager", 5)));
        AchievementDef secret = Goal("Secret", 2);
        secret.hidden = true;
        CHECK(library.Register(secret));
    }
};

} // namespace

AETHER_TEST(Achievements_ParseValidateAndRoundTrip) {
    AchievementDef good = Goal("Salvager", 50);
    good.title = "Salvager";
    good.description = "Take 50 pieces of salvage.";
    good.hidden = true;
    AchievementDef back;
    std::string error;
    CHECK(AchievementFromJson(AchievementToJson(good).dump(), back, &error) && back == good);
    CHECK(AchievementFromJson(R"({"name":"One"})", back, &error) && back.target == 1 && !back.hidden); // the target defaults to 1

    const auto refused = [&](const std::string& text, const char* code) {
        AchievementDef a;
        std::string why;
        return !AchievementFromJson(text, a, &why) && why.rfind(code, 0) == 0;
    };
    CHECK(refused("not json", "achievement.bad_json"));
    CHECK(refused(R"({"target":2})", "achievement.name_empty"));
    CHECK(refused(R"({"name":"A","target":0})", "achievement.bad_target"));
    CHECK(refused(R"({"name":"A","target":-3})", "achievement.bad_target"));
    CHECK(refused(R"({"name":"A","target":1000001})", "achievement.bad_target"));
    CHECK(refused(R"({"name":"A","target":2.5})", "achievement.bad_target"));
    CHECK(refused(R"({"name":"A","target":"many"})", "achievement.bad_target"));
    CHECK(refused(R"({"name":7})", "achievement.bad_json"));
    CHECK(refused(R"({"name":"A","hidden":"yes"})", "achievement.bad_json"));

    AchievementLibrary library;
    CHECK(library.Register(Goal("B")) && library.Register(Goal("A")) && library.Size() == 2 && library.Names() == (std::vector<std::string>{"A", "B"}));
    CHECK(library.Find("A") != nullptr && library.Find("Nope") == nullptr);
    AchievementDef invalid;
    CHECK(!library.Register(invalid, &error) && error.rfind("achievement.name_empty", 0) == 0);
}

AETHER_TEST(Achievements_CountUnlockAndPersistInTheSaveBag) {
    Rig r;
    CHECK(AchievementSystem::Active() == &r.system);
    CHECK(r.system.Progress("Salvager") == 0 && !r.system.IsUnlocked("Salvager") && r.system.UnlockedCount() == 0);

    // Progress counts up to the target, then it is unlocked, once.
    CHECK(r.system.Add("Salvager", 2) == Result::Ok && r.system.Progress("Salvager") == 2 && !r.system.IsUnlocked("Salvager"));
    CHECK(r.system.NextUnlocked().empty());
    CHECK(r.system.Add("Salvager", 2) == Result::Ok && r.system.Add("Salvager") == Result::Ok);
    CHECK(r.system.IsUnlocked("Salvager") && r.system.Progress("Salvager") == 5 && r.system.UnlockedCount() == 1);
    CHECK(r.system.Add("Salvager", 100) == Result::Ok && r.system.Progress("Salvager") == 5); // it stops at the target
    CHECK(r.system.NextUnlocked() == "Salvager" && r.system.NextUnlocked().empty()); // the toast comes once, not once per later Add

    // Overshooting in one step unlocks too, and several unlocks queue in order.
    CHECK(r.system.Add("Secret", 999) == Result::Ok && r.system.Progress("Secret") == 2 && r.system.IsUnlocked("Secret"));
    CHECK(r.system.Unlock("FirstSteps") == Result::Ok && r.system.IsUnlocked("FirstSteps") && r.system.Unlock("FirstSteps") == Result::Ok);
    CHECK((r.system.TakeUnlocked() == std::vector<std::string>{"Secret", "FirstSteps"}) && r.system.TakeUnlocked().empty());

    // What it refuses changes nothing.
    CHECK(r.system.Add("Nope") == Result::Unknown && r.system.Unlock("Nope") == Result::Unknown && r.system.Reset("Nope") == Result::Unknown);
    CHECK(r.system.Add("Salvager", 0) == Result::BadAmount && r.system.Add("Salvager", -4) == Result::BadAmount && r.system.Add("Salvager", 1000001) == Result::BadAmount);
    save::SaveBag untouched;
    AchievementLibrary library;
    CHECK(library.Register(Goal("Salvager", 5)));
    AchievementSystem fresh(untouched, library);
    CHECK(fresh.Add("Salvager", 0) == Result::BadAmount && untouched.entries.empty());

    // The progress is in the bag, as an integer under "achievement.<name>", so a system made over a copy of the bag (a loaded slot) agrees.
    CHECK(r.bag.GetInt("achievement.Salvager") == 5 && r.bag.GetInt("achievement.Secret") == 2);
    save::SaveBag loaded = r.bag;
    AchievementSystem restored(loaded, r.library);
    CHECK(restored.IsUnlocked("Salvager") && restored.IsUnlocked("Secret") && restored.IsUnlocked("FirstSteps") && restored.UnlockedCount() == 3);
    CHECK(restored.NextUnlocked().empty()); // loading an old unlock is not a new one: no toast
    CHECK(restored.Add("Salvager") == Result::Ok && restored.NextUnlocked().empty());
    r.system.MakeActive();

    // A bag edited by hand (or by another version of the game) never reads outside its range.
    r.bag.SetInt("achievement.Salvager", 9999);
    CHECK(r.system.Progress("Salvager") == 5);
    r.bag.SetInt("achievement.Salvager", -7);
    CHECK(r.system.Progress("Salvager") == 0 && !r.system.IsUnlocked("Salvager"));

    // Reset takes the progress away (and a toast not yet shown).
    CHECK(r.system.Unlock("Salvager") == Result::Ok && r.system.Reset("Salvager") == Result::Ok);
    CHECK(r.system.Progress("Salvager") == 0 && !r.bag.Has("achievement.Salvager") && r.system.NextUnlocked().empty());
}

AETHER_TEST(Achievements_TheKitLoadsGoalsAndReportsBadOnes) {
    const std::map<std::string, std::string> files = {
        {"Achievements/ok.aachievement", AchievementToJson(Goal("Salvager", 5)).dump()},
        {"Achievements/badjson.aachievement", "{ not json"},
        {"Achievements/notarget.aachievement", R"({"name":"Broken","target":0})"},
    };
    const auto read = [&](const std::string& path, std::vector<u8>& bytes, std::string* error) {
        const auto it = files.find(path);
        if (it == files.end()) {
            if (error) *error = "no such file";
            return false;
        }
        bytes.assign(it->second.begin(), it->second.end());
        return true;
    };
    AchievementLibrary loaded;
    const std::vector<AchievementAssetDiagnostic> diagnostics = LoadAchievementAssets(
        {"Achievements/ok.aachievement", "Achievements/badjson.aachievement", "Achievements/notarget.aachievement", "Achievements/gone.aachievement"}, read, loaded);
    CHECK(loaded.Size() == 1 && loaded.Find("Salvager") != nullptr && diagnostics.size() == 3);
    for (const AchievementAssetDiagnostic& d : diagnostics) {
        if (d.path == "Achievements/badjson.aachievement") CHECK(!d.content_read_failure && d.message.rfind("achievement.bad_json", 0) == 0);
        if (d.path == "Achievements/notarget.aachievement") CHECK(!d.content_read_failure && d.message.rfind("achievement.bad_target", 0) == 0);
        if (d.path == "Achievements/gone.aachievement") CHECK(d.content_read_failure);
    }
    AchievementLibrary through_kit;
    const std::unique_ptr<kit::IKit> kit = MakeAchievementsKit(&through_kit);
    CHECK(std::string(kit->Name()) == "Achievements" && kit->Deps().empty() && kit->Stages().empty());
    kit::KitAssetContext context;
    context.assets = {{"AchievementDefinition", "Achievements/ok.aachievement"}, {"ItemDefinition", "Achievements/notarget.aachievement"}};
    context.read_content = read;
    kit->LoadAssets(context);
    CHECK(through_kit.Size() == 1 && through_kit.Find("Salvager") != nullptr);

    // The runtime needs the host's library and save bag; without either there is no system (the script table then answers with defaults).
    kit::KitRuntimeContext runtime;
    CHECK(kit->CreateRuntime(runtime) == nullptr);
    save::SaveBag bag;
    runtime.find_service = [&](const char* name) -> void* { return std::string(name) == "achievements.library" ? static_cast<void*>(&through_kit) : nullptr; };
    CHECK(kit->CreateRuntime(runtime) == nullptr);
    runtime.find_service = [&](const char* name) -> void* {
        if (std::string(name) == "achievements.library") return &through_kit;
        if (std::string(name) == "save.bag") return &bag;
        return nullptr;
    };
    std::unique_ptr<kit::IKitRuntime> made = kit->CreateRuntime(runtime);
    CHECK(made != nullptr);
    if (made) {
        auto* system = static_cast<AchievementSystem*>(made->Find("AchievementSystem"));
        CHECK(system != nullptr && system->Add("Salvager", 5) == Result::Ok && bag.GetInt("achievement.Salvager") == 5);
        CHECK(made->Find("Nope") == nullptr);
    }
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer) {
    return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", importer}};
}

} // namespace

// The real player: the package's goals load (a bad one is a warning), the progress goes into the save bag the slot writes, and a scene change leaves it alone.
AETHER_TEST(Achievements_PlayerKeepsProgressInTheSaveBagThroughASceneChange) {
    if (!player::Game::HasScripting()) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_achievements_player_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string script_guid = assets::ToString(assets::NewAssetGuid());
    ScriptComponent component;
    assets::AssetGuid parsed;
    CHECK(assets::ParseAssetGuid(script_guid, parsed));
    component.script.guid = parsed;
    const nlohmann::json a = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array({{{"components", {{"ScriptComponent", reflect::ToJson(component)}}}}})}};
    const nlohmann::json b = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array()}};
    const std::string script = "local S = {}\nfunction S:OnUpdate(dt)\n  if not self.asked then self.asked = true; Scene.Load('Scenes/b.ascene') end\nend\nreturn S\n";
    const nlohmann::json assets_json = nlohmann::json::array({AssetEntry("Scenes/a.ascene", "Scene"), AssetEntry("Scenes/b.ascene", "Scene"),
        AssetEntry("Achievements/salvager.aachievement", "AchievementDefinition"), AssetEntry("Achievements/broken.aachievement", "AchievementDefinition"),
        {{"guid", script_guid}, {"path", "Scripts/Swap.luau"}, {"importer", "Script"}}});
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/a.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()}, {"assets", assets_json}, {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    CHECK(writer.Add("Manifest.json", manifest.dump(2)));
    CHECK(writer.Add("Content/Scenes/a.ascene", a.dump()));
    CHECK(writer.Add("Content/Scenes/b.ascene", b.dump()));
    CHECK(writer.Add("Content/Scripts/Swap.luau", script));
    CHECK(writer.Add("Content/Achievements/salvager.aachievement", AchievementToJson(Goal("Salvager", 5)).dump()));
    CHECK(writer.Add("Content/Achievements/broken.aachievement", R"({"name":"Broken","target":0})"));
    const std::filesystem::path file = dir / "Game.apak";
    CHECK(writer.Write(file.string()));
    player::GamePackage package;
    std::string error;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    game.SetUserPaths(player::ResolveUserPaths("Demo", dir / "user")); // the player does this: without a save system there is no bag, and so no achievements
    CHECK(game.LoadStartupScene(&error));
    bool warned = false;
    for (const std::string& w : game.Warnings()) warned = warned || (w.find("broken.aachievement") != std::string::npos && w.find("achievement.bad_target") != std::string::npos);
    CHECK(warned); // the broken goal is reported, the good one loads
    game.BeginPlay();
    AchievementSystem* system = AchievementSystem::Active();
    CHECK(system != nullptr && game.Saves() != nullptr);
    if (!system || !game.Saves()) return;
    CHECK(system->Library().Find("Salvager") != nullptr && system->Library().Find("Broken") == nullptr);
    CHECK(system->Add("Salvager", 3) == Result::Ok);
    CHECK(game.Saves()->Bag().GetInt("achievement.Salvager") == 3); // in the bag a save slot writes
    for (int i = 0; i < 4; ++i) game.Tick(1.0f / 60.0f);
    CHECK(game.SceneName() == "Scenes/b.ascene" && game.ScriptErrors().empty());
    AchievementSystem* after = AchievementSystem::Active();
    CHECK(after != nullptr && after->Progress("Salvager") == 3 && game.Saves()->Bag().GetInt("achievement.Salvager") == 3); // the scene change left it alone
}

#if AETHER_TEST_HAS_SCRIPTING
AETHER_TEST(Achievements_LuauTableActsOnTheSystem) {
    Rig r;
    World world;
    script::LuauHost host;
    GuidIndex guids;
    const assets::AssetGuid hero = assets::NewAssetGuid();
    (void)GetComponentId<ScriptComponent>();
    script::ScriptSystem scripts(host, world, guids, [&](const assets::AssetGuid& g, std::string& source, std::string& name) {
        if (g != hero) return false;
        source = "local H = {} function H:Run() self.exists = Achievements.Exists('Salvager'); self.gone = Achievements.Exists('Nope'); "
                 "self.title = Achievements.GetTitle('FirstSteps'); self.plain = Achievements.GetTitle('Salvager'); self.desc = Achievements.GetDescription('FirstSteps'); "
                 "self.target = Achievements.GetTarget('Salvager'); self.hidden = Achievements.IsHidden('Secret'); self.shown = Achievements.IsHidden('Salvager'); "
                 "self.add1 = Achievements.Add('Salvager'); self.p1 = Achievements.GetProgress('Salvager'); self.open = Achievements.IsUnlocked('Salvager'); "
                 "self.add4 = Achievements.Add('Salvager', 4); self.done = Achievements.IsUnlocked('Salvager'); self.count = Achievements.UnlockedCount(); "
                 "self.unknown = Achievements.Add('Nope'); self.bad = Achievements.Add('Salvager', 0); self.bad2 = Achievements.Add('Salvager', -3); "
                 "self.unlock = Achievements.Unlock('Secret'); self.toast1 = Achievements.NextUnlocked(); self.toast2 = Achievements.NextUnlocked(); self.toast3 = Achievements.NextUnlocked(); "
                 "self.reset = Achievements.Reset('Salvager'); self.p2 = Achievements.GetProgress('Salvager') end return H";
        name = "H.luau";
        return true;
    });
    ScriptComponent component;
    component.script.guid = hero;
    const Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, std::move(component));
    guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
    Lifecycle life(world, guids);
    scripts.Register(life);
    life.BeginPlay();
    CHECK(!host.Run("return Achievements.Add(5)").ok && !host.Run("return Achievements.IsUnlocked(nil)").ok && !host.Run("return Achievements.GetTitle({})").ok); // wrong types are script errors
    CHECK(scripts.SendEvent(e, "Run") && scripts.Errors().empty());
    const auto text = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string("<not a string>"); };
    const auto flag = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<bool>(v) && std::get<bool>(v); };
    const auto num = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<f64>(v) ? std::get<f64>(v) : -999.0; };
    CHECK(flag("exists") && !flag("gone") && text("title") == "First Steps" && text("plain") == "Salvager" && text("desc") == "Walk out of the camp.");
    CHECK(num("target") == 5.0 && flag("hidden") && !flag("shown"));
    CHECK(text("add1") == "ok" && num("p1") == 1.0 && !flag("open") && text("add4") == "ok" && flag("done") && num("count") == 1.0);
    CHECK(text("unknown") == "unknown_achievement" && text("bad") == "bad_amount" && text("bad2") == "bad_amount");
    CHECK(text("unlock") == "ok" && text("toast1") == "Salvager" && text("toast2") == "Secret" && text("toast3").empty()); // in the order they unlocked, then ""
    CHECK(text("reset") == "ok" && num("p2") == 0.0);
    life.EndPlay();
}
#endif

#endif
