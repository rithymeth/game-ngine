#include "test_framework.h"

#ifdef AETHER_TEST_HAS_QUESTS

#include "aether/assets/asset_guid.h"
#include "aether/ecs/world.h"
#include "aether/loc/localization.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/quests/quest_library.h"
#include "aether/quests/quest_system.h"
#include "aether/reflection/registry.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif
#ifdef AETHER_TEST_HAS_INVENTORY
#include "aether/inventory/inventory_system.h"
#endif

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>

// Phase 30 step 7c (§30.10): quest definitions, the log, progress and rewards, and the faces.

using namespace aether;
using namespace aether::quest;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

Objective Obj(const char* id, Objective::Kind kind, const char* target, i32 required, bool optional = false) {
    Objective o;
    o.id = id;
    o.kind = kind;
    o.target = target;
    o.required = required;
    o.optional = optional;
    return o;
}

struct Rig {
    World world;
    gas::AttributeSystem attrs{world};
    gas::EffectLibrary effects;
    gas::EffectSystem fx{world, attrs, effects};
    QuestLibrary quests;
    QuestSystem sys{world, &fx, quests};
    Entity e;

    Rig() {
        gas::GameplayEffect reward;
        reward.name = "Blessing";
        reward.modifiers = {{"Health", gas::GameplayEffect::Op::Add, 20}};
        effects.Register(reward);
        QuestDef pelts;
        pelts.name = "Pelts";
        pelts.title = "Wolf Pelts";
        pelts.title_key = "quest.pelts.title";
        pelts.objectives = {Obj("collect", Objective::Kind::Count, "Wolf Pelt", 3), Obj("report", Objective::Kind::Flag, "elder", 1), Obj("bonus", Objective::Kind::Count, "Rare Pelt", 1, true)};
        pelts.reward_effects = {"Blessing"};
        pelts.reward_items = {{"Gold", 50}};
        QuestDef next;
        next.name = "Next";
        next.prerequisites = {"Pelts"};
        next.objectives = {Obj("go", Objective::Kind::Tag, "State.There", 1)};
        QuestDef empty;
        empty.name = "Easy";
        CHECK(quests.Register(pelts) && quests.Register(next) && quests.Register(empty));
        e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
        attrs.Define(e, "Health", 50, 0, 100);
    }
};

std::vector<QuestEvent::Kind> Kinds(const QuestSystem& s) {
    std::vector<QuestEvent::Kind> k;
    for (const QuestEvent& e : s.Events()) k.push_back(e.kind);
    return k;
}

} // namespace

AETHER_TEST(Quests_JsonRoundTripAndErrors) {
    QuestDef q;
    q.name = "Pelts";
    q.title = "Wolf Pelts";
    q.title_key = "quest.pelts";
    q.description = "Bring pelts";
    q.description_key = "quest.pelts.desc";
    q.objectives = {Obj("collect", Objective::Kind::Count, "Wolf Pelt", 3), Obj("bonus", Objective::Kind::Flag, "x", 1, true)};
    q.objectives[0].text = "Collect pelts";
    q.objectives[0].text_key = "quest.pelts.collect";
    q.prerequisites = {"Intro"};
    q.reward_effects = {"Blessing"};
    q.reward_items = {{"Gold", 50}};
    QuestDef back;
    std::string err;
    CHECK(QuestFromJson(QuestToJson(q).dump(), back, &err) && back == q);
    const auto fails = [](const char* json, const char* code) {
        QuestDef out;
        std::string e;
        return !QuestFromJson(json, out, &e) && e.rfind(code, 0) == 0;
    };
    CHECK(fails("nonsense", "quest.parse"));
    CHECK(fails("{}", "quest.no_name"));
    CHECK(fails(R"({"name":"x","objectives":[{"id":"a","kind":"kill"}]})", "quest.bad_kind"));
    CHECK(fails(R"({"name":"x","objectives":[{"id":"a","required":0}]})", "quest.bad_required"));
    CHECK(fails(R"({"name":"x","objectives":[{"id":"a"},{"id":"a"}]})", "quest.duplicate_objective"));
    CHECK(fails(R"({"name":"x","objectives":[{"required":1}]})", "quest.duplicate_objective")); // an objective needs an id
    CHECK(fails(R"({"name":"x","rewards":{"items":[{"item":"Gold","count":0}]}})", "quest.parse"));
}

AETHER_TEST(Quests_LibraryCheckFindsCyclesAndMissingThings) {
    QuestLibrary lib;
    QuestDef a, b, c;
    a.name = "A";
    a.prerequisites = {"B"};
    b.name = "B";
    b.prerequisites = {"A"};
    c.name = "C";
    c.prerequisites = {"Nope"};
    c.reward_effects = {"NoEffect"};
    lib.Register(a);
    lib.Register(b);
    lib.Register(c);
    gas::EffectLibrary effects;
    const std::vector<std::string> problems = lib.Check(&effects);
    const auto has = [&](const char* code) { return std::any_of(problems.begin(), problems.end(), [&](const std::string& p) { return p.rfind(code, 0) == 0; }); };
    CHECK(has("quest.prereq_cycle") && has("quest.unknown_prereq") && has("quest.unknown_effect"));
    CHECK(lib.Check(nullptr).size() == problems.size() - 1); // without effects the reward effect isn't checked
    Rig r;
    CHECK(r.quests.Check(&r.effects).empty());
}

AETHER_TEST(Quests_StartPrerequisitesAndStates) {
    Rig r;
    CHECK(r.sys.Start(r.e, "Next") == Result::PrereqUnmet && r.sys.Start(r.e, "Nope") == Result::UnknownQuest && r.sys.Start(Entity{}, "Pelts") == Result::DeadEntity);
    CHECK(r.sys.Start(r.e, "Pelts") == Result::Ok && r.sys.IsActive(r.e, "Pelts") && r.sys.Start(r.e, "Pelts") == Result::AlreadyActive);
    CHECK(r.sys.State(r.e, "Next") == kInactive && r.sys.GetProgress(r.e, "Pelts", "collect") == 0);
    CHECK(r.sys.Abandon(r.e, "Pelts") == Result::Ok && r.sys.State(r.e, "Pelts") == kInactive && r.sys.Abandon(r.e, "Pelts") == Result::NotActive);
    CHECK(r.sys.Start(r.e, "Pelts") == Result::Ok && r.sys.Fail(r.e, "Pelts") == Result::Ok && r.sys.State(r.e, "Pelts") == kFailed && r.sys.Fail(r.e, "Pelts") == Result::NotActive);
    CHECK(r.sys.Start(r.e, "Pelts") == Result::Ok && r.sys.IsActive(r.e, "Pelts")); // a failed quest starts over
    // A quest with no objectives completes at once.
    r.sys.ClearEvents();
    CHECK(r.sys.Start(r.e, "Easy") == Result::Ok && r.sys.IsCompleted(r.e, "Easy"));
    CHECK((Kinds(r.sys) == std::vector<QuestEvent::Kind>{QuestEvent::Kind::Started, QuestEvent::Kind::Completed}));
    CHECK(r.sys.Start(r.e, "Easy") == Result::AlreadyCompleted);
}

AETHER_TEST(Quests_ProgressCompletionAndRewards) {
    Rig r;
    r.sys.Start(r.e, "Pelts");
    r.sys.ClearEvents();
    CHECK(r.sys.Progress(r.e, "Pelts", "collect", 2) == Result::Ok && r.sys.GetProgress(r.e, "Pelts", "collect") == 2);
    CHECK(r.sys.Progress(r.e, "Pelts", "collect", 9) == Result::Ok && r.sys.GetProgress(r.e, "Pelts", "collect") == 3); // clamps at required
    CHECK(r.sys.Progress(r.e, "Pelts", "collect", -1) == Result::Ok && r.sys.GetProgress(r.e, "Pelts", "collect") == 3); // done stays done
    CHECK(r.sys.Progress(r.e, "Pelts", "zzz", 1) == Result::UnknownObjective && r.sys.Progress(r.e, "Next", "go", 1) == Result::NotActive);
    CHECK(r.sys.IsActive(r.e, "Pelts")); // the report objective is still open; the optional one doesn't count
    CHECK((Kinds(r.sys) == std::vector<QuestEvent::Kind>{QuestEvent::Kind::ObjectiveProgress, QuestEvent::Kind::ObjectiveProgress, QuestEvent::Kind::ObjectiveCompleted}));
    r.sys.ClearEvents();
    CHECK(r.sys.Progress(r.e, "Pelts", "report", 1) == Result::Ok && r.sys.IsCompleted(r.e, "Pelts")); // a flag is done at once
    CHECK((Kinds(r.sys) == std::vector<QuestEvent::Kind>{QuestEvent::Kind::ObjectiveProgress, QuestEvent::Kind::ObjectiveCompleted, QuestEvent::Kind::Completed}));
    const QuestEvent& done = r.sys.Events().back();
    CHECK(done.quest == "Pelts" && done.reward_items.size() == 1 && done.reward_items[0].item == "Gold" && done.reward_items[0].count == 50);
    CHECK(Near(r.attrs.Get(r.e, "Health"), 70)); // the reward effect
    // Completing it opens the next quest; a completed quest takes no more progress.
    CHECK(r.sys.Progress(r.e, "Pelts", "collect", 1) == Result::NotActive && r.sys.Start(r.e, "Next") == Result::Ok);
}

AETHER_TEST(Quests_NotifyMatchesKindAndTarget) {
    Rig r;
    r.sys.Start(r.e, "Pelts");
    r.sys.Start(r.e, "Easy");
    CHECK(r.sys.Notify(r.e, Objective::Kind::Count, "Wolf Pelt", 2) == 1 && r.sys.GetProgress(r.e, "Pelts", "collect") == 2);
    CHECK(r.sys.Notify(r.e, Objective::Kind::Tag, "Wolf Pelt", 1) == 0 && r.sys.Notify(r.e, Objective::Kind::Count, "Bear Pelt", 1) == 0); // kind and target both match
    CHECK(r.sys.Notify(r.e, Objective::Kind::Count, "Wolf Pelt", -1) == 1 && r.sys.GetProgress(r.e, "Pelts", "collect") == 1); // dropping one takes it back
    CHECK(r.sys.Notify(r.e, Objective::Kind::Flag, "elder", 1) == 1);
    CHECK(r.sys.Notify(r.e, Objective::Kind::Count, "Wolf Pelt", 5) == 1 && r.sys.IsCompleted(r.e, "Pelts"));
    CHECK(r.sys.Notify(r.e, Objective::Kind::Count, "Wolf Pelt", 1) == 0); // nothing active to move
    const Entity other = r.world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    CHECK(r.sys.Notify(other, Objective::Kind::Count, "Wolf Pelt", 1) == 0 && r.sys.Notify(Entity{}, Objective::Kind::Count, "x", 1) == 0);
}

AETHER_TEST(Quests_LogSavesByNameAndTitlesAreLocalized) {
    Rig r;
    r.sys.Start(r.e, "Pelts");
    r.sys.Notify(r.e, Objective::Kind::Count, "Wolf Pelt", 2);
    const QuestLog& log = *r.world.GetComponent<QuestLog>(r.e);
    const reflect::Json j = reflect::ToJson(log);
    QuestLog back;
    CHECK(reflect::FromJson(back, j) && back.quests.size() == 1 && back.quests[0].name == "Pelts" && back.quests[0].state == kActive && back.quests[0].objectives.size() == 3 && back.quests[0].objectives[0].progress == 2);
    CHECK(reflect::TypeRegistry::Find("QuestLog") != nullptr && back.Find("Pelts") != nullptr && back.Find("Nope") == nullptr);
    CHECK(r.sys.Title("Pelts") == "Wolf Pelts" && r.sys.Title("Easy") == "Easy" && r.sys.Title("Nope").empty()); // no translation: the fallback, then the name
    loc::Localization l;
    l.MakeActive();
    l.AddFromCsv("key,en,fr\nquest.pelts.title,Wolf Pelts!,Peaux de loup\n");
    CHECK(r.sys.Title("Pelts") == "Wolf Pelts!");
    l.SetLanguage("fr");
    CHECK(r.sys.Title("Pelts") == "Peaux de loup" && r.sys.ObjectiveText("Pelts", "collect").empty());
}

AETHER_TEST(Quests_BlueprintLibraryActsOnTheActiveSystem) {
    const reflect::TypeInfo* type = reflect::TypeRegistry::Find("Quests");
    CHECK(type != nullptr && type->functions.size() == 10);
    World world;
    const Entity e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    CHECK(QuestSystem::Active() == nullptr);
    CHECK(!Quests::StartQuest(e, "Pelts") && Quests::GetQuestState(e, "Pelts") == "inactive" && Quests::NotifyQuests(e, "count", "x", 1) == 0 && Quests::GetQuestTitle("Pelts").empty());
    Rig r;
    CHECK(QuestSystem::Active() == &r.sys);
    CHECK(Quests::StartQuest(r.e, "Pelts") && Quests::IsQuestActive(r.e, "Pelts") && Quests::GetQuestState(r.e, "Pelts") == "active");
    CHECK(Quests::NotifyQuests(r.e, "count", "Wolf Pelt", 2) == 1 && Quests::NotifyQuests(r.e, "nonsense", "Wolf Pelt", 1) == 0 && Quests::GetQuestProgress(r.e, "Pelts", "collect") == 2);
    CHECK(Quests::ProgressQuest(r.e, "Pelts", "collect", 1) && Quests::ProgressQuest(r.e, "Pelts", "report", 1) && Quests::IsQuestCompleted(r.e, "Pelts") && Quests::GetQuestState(r.e, "Pelts") == "completed");
    CHECK(Quests::StartQuest(r.e, "Next") && Quests::FailQuest(r.e, "Next") && Quests::GetQuestState(r.e, "Next") == "failed" && !Quests::AbandonQuest(r.e, "Next"));
    CHECK(Quests::GetQuestTitle("Pelts") == "Wolf Pelts");
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer, const assets::AssetGuid& guid) {
    return {{"guid", assets::ToString(guid)}, {"path", path}, {"importer", importer}};
}

std::filesystem::path MakePackage(const char* dir_name, const assets::AssetGuid& script_guid, const std::string& script, bool with_items) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / dir_name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array()}};
    nlohmann::json assets_json = nlohmann::json::array({AssetEntry("Scenes/start.ascene", "Scene", assets::NewAssetGuid()), AssetEntry("Effects/blessing.aeffect", "GameplayEffect", assets::NewAssetGuid()),
                                                        AssetEntry("Quests/pelts.aquest", "QuestDefinition", assets::NewAssetGuid()), AssetEntry("Quests/bad.aquest", "QuestDefinition", assets::NewAssetGuid()),
                                                        AssetEntry("Quests/loop.aquest", "QuestDefinition", assets::NewAssetGuid())});
    if (with_items) {
        assets_json.push_back(AssetEntry("Items/pelt.aitem", "ItemDefinition", assets::NewAssetGuid()));
        assets_json.push_back(AssetEntry("Items/gold.aitem", "ItemDefinition", assets::NewAssetGuid()));
    }
    if (!script.empty()) assets_json.push_back(AssetEntry("Scripts/Hero.luau", "Script", script_guid));
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()}, {"assets", assets_json}, {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump(2));
    writer.Add("Content/Scenes/start.ascene", scene.dump(2));
    writer.Add("Content/Effects/blessing.aeffect", nlohmann::json{{"name", "Blessing"}, {"modifiers", nlohmann::json::array({{{"attribute", "Health"}, {"op", "add"}, {"magnitude", 20}}})}}.dump());
    writer.Add("Content/Quests/pelts.aquest", nlohmann::json{{"name", "Pelts"}, {"objectives", nlohmann::json::array({{{"id", "collect"}, {"kind", "count"}, {"target", "Pelt"}, {"required", 2}}})},
                                                             {"rewards", {{"effects", {"Blessing"}}, {"items", nlohmann::json::array({{{"item", "Gold"}, {"count", 5}}})}}}}.dump());
    writer.Add("Content/Quests/bad.aquest", std::string("{\"name\":\"\"}"));
    writer.Add("Content/Quests/loop.aquest", nlohmann::json{{"name", "Loop"}, {"prerequisites", {"Loop"}}}.dump());
    if (with_items) {
        writer.Add("Content/Items/pelt.aitem", nlohmann::json{{"name", "Pelt"}, {"max_stack", 10}}.dump());
        writer.Add("Content/Items/gold.aitem", nlohmann::json{{"name", "Gold"}, {"max_stack", 100}}.dump());
    }
    if (!script.empty()) writer.Add("Content/Scripts/Hero.luau", script);
    const std::filesystem::path file = dir / "Game.apak";
    std::string error;
    writer.Write(file.string(), &error);
    return file;
}

} // namespace

AETHER_TEST(Quests_PlayerLoadsAssetsAndRunsTheSystem) {
    const std::filesystem::path file = MakePackage("aether_quests_player_tests", assets::AssetGuid{}, "", true);
    std::string error;
    player::GamePackage package;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    CHECK(game.LoadStartupScene(&error));
    bool bad = false, cycle = false;
    for (const std::string& w : game.Warnings()) {
        bad = bad || (w.find("bad.aquest") != std::string::npos && w.find("quest.no_name") != std::string::npos);
        cycle = cycle || w.find("quest.prereq_cycle") != std::string::npos;
    }
    CHECK(bad && cycle); // a broken quest and a prerequisite cycle are reported
    QuestSystem* sys = QuestSystem::Active();
    gas::AttributeSystem* attrs = gas::AttributeSystem::Active();
    CHECK(sys != nullptr && attrs != nullptr);
    if (!sys || !attrs) return;
    const Entity e = game.GetWorld().CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    attrs->Define(e, "Health", 10, 0, 100);
    game.BeginPlay();
    CHECK(sys->Start(e, "Pelts") == Result::Ok);
#ifdef AETHER_TEST_HAS_INVENTORY
    inv::InventorySystem* items = inv::InventorySystem::Active();
    CHECK(items != nullptr);
    if (!items) return;
    items->Configure(e, 5);
    CHECK(items->Add(e, "Pelt", 2) == 2);
    game.Tick(0.016f); // Player.Inventory forwards the pickup, Player.Quests completes the quest and hands out the reward
    CHECK(sys->IsCompleted(e, "Pelts") && sys->Events().empty());
    CHECK(items->Count(e, "Gold") == 5 && Near(attrs->Get(e, "Health"), 30)); // the reward item and effect
#else
    CHECK(sys->Notify(e, Objective::Kind::Count, "Pelt", 2) == 1);
    game.Tick(0.016f);
    CHECK(sys->IsCompleted(e, "Pelts") && Near(attrs->Get(e, "Health"), 30));
#endif
}

#if AETHER_TEST_HAS_SCRIPTING
AETHER_TEST(Quests_ScriptsHearEventsAndUseTheTable) {
    const assets::AssetGuid script_guid = assets::NewAssetGuid();
    // The hero counts what it hears in attributes, which the test reads back.
    const std::string source = R"(
local Hero = {}
function Hero:OnQuestStarted(q) Attributes.AddBase(self.entity, 'Started', 1) end
function Hero:OnQuestProgress(q, objective, progress, required) Attributes.AddBase(self.entity, 'Progress', 1) end
function Hero:OnQuestCompleted(q) Attributes.AddBase(self.entity, 'Done', 1) end
function Hero:Run()
    local e = self.entity
    self.started = Quests.Start(e, 'Pelts')
    self.moved = Quests.Notify(e, 'count', 'Pelt', 1)
    self.progress = Quests.GetProgress(e, 'Pelts', 'collect')
    self.active = Quests.IsActive(e, 'Pelts')
    self.state = Quests.GetState(e, 'Pelts')
    self.done = Quests.Progress(e, 'Pelts', 'collect', 1)
    self.completed = Quests.IsCompleted(e, 'Pelts')
    self.title = Quests.GetTitle('Pelts')
end
return Hero
)";
    const std::filesystem::path file = MakePackage("aether_quests_script_tests", script_guid, source, false);
    std::string error;
    player::GamePackage package;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    CHECK(game.LoadStartupScene(&error));
    ScriptComponent component;
    component.script.guid = script_guid;
    const Entity e = game.GetWorld().CreateEntity(IdComponent{NewEntityGuid()}, std::move(component));
    game.BeginPlay();
    gas::AttributeSystem* attrs = gas::AttributeSystem::Active();
    QuestSystem* sys = QuestSystem::Active();
    CHECK(attrs != nullptr && sys != nullptr && game.ScriptErrors().empty());
    if (!attrs || !sys) return;
    for (const char* a : {"Started", "Progress", "Done"}) attrs->Define(e, a, 0);
    attrs->Define(e, "Health", 10, 0, 100);
    CHECK(sys->Start(e, "Pelts") == Result::Ok && sys->Notify(e, Objective::Kind::Count, "Pelt", 2) == 1);
    game.Tick(0.016f);
    CHECK(game.ScriptErrors().empty());
    CHECK(Near(attrs->Get(e, "Started"), 1) && Near(attrs->Get(e, "Progress"), 1) && Near(attrs->Get(e, "Done"), 1)); // started, one step of progress (2 at once), completed
}

AETHER_TEST(Quests_LuauTableActsOnTheSystem) {
    Rig r;
    script::LuauHost host;
    GuidIndex guids;
    const assets::AssetGuid hero = assets::NewAssetGuid();
    (void)GetComponentId<ScriptComponent>();
    script::ScriptSystem scripts(host, r.world, guids, [&](const assets::AssetGuid& g, std::string& source, std::string& name) {
        if (g != hero) return false;
        source = "local H = {} function H:Run() local e = self.entity; self.s = Quests.Start(e, 'Pelts'); self.n = Quests.Notify(e, 'count', 'Wolf Pelt', 2); "
                 "self.p = Quests.GetProgress(e, 'Pelts', 'collect'); self.st = Quests.GetState(e, 'Pelts'); self.a = Quests.Progress(e, 'Pelts', 'collect'); "
                 "self.ab = Quests.Abandon(e, 'Pelts'); self.act = Quests.IsActive(e, 'Pelts'); self.t = Quests.GetTitle('Pelts') end return H";
        name = "H.luau";
        return true;
    });
    ScriptComponent component;
    component.script.guid = hero;
    const Entity actor = r.world.CreateEntity(IdComponent{NewEntityGuid()}, std::move(component));
    guids.Add(r.world.GetComponent<IdComponent>(actor)->guid, actor);
    Lifecycle life(r.world, guids);
    scripts.Register(life);
    life.BeginPlay();
    CHECK(!host.Run("return Quests.Start(5, 'x')").ok && !host.Run("return Quests.Notify(nil, 'count', 'x')").ok && !host.Run("return Quests.GetTitle(1)").ok);
    CHECK(scripts.SendEvent(actor, "Run") && scripts.Errors().empty());
    const auto num = [&](const char* f) { const auto v = scripts.GetField(actor, f); return std::holds_alternative<f64>(v) ? std::get<f64>(v) : -999.0; };
    const auto str = [&](const char* f) { const auto v = scripts.GetField(actor, f); return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string("<none>"); };
    const auto flag = [&](const char* f) { const auto v = scripts.GetField(actor, f); return std::holds_alternative<bool>(v) && std::get<bool>(v); };
    CHECK(flag("s") && num("n") == 1.0 && num("p") == 2.0 && str("st") == "active" && flag("a") && flag("ab") && !flag("act") && str("t") == "Wolf Pelts");
    life.EndPlay();
}
#endif

#endif
