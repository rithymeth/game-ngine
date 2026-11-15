#include "test_framework.h"

#ifdef AETHER_TEST_HAS_DIALOGUE

#include "aether/assets/asset_guid.h"
#include "aether/dialogue/dialogue_kit.h"
#include "aether/dialogue/dialogue_system.h"
#include "aether/ecs/world.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/scene/components.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>

// The dialogue kit: conversations (`.adialogue`) as data, run per listening entity.

using namespace aether;
using namespace aether::dlg;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

Node N(const char* id, const char* text, const char* next = "") {
    Node n;
    n.id = id;
    n.text = text;
    n.next = next;
    return n;
}

// hello -> ask (Yes -> deal -> bye | No -> bye | Leave: ends) ; bye is the last line.
DialogueDef Innkeeper() {
    DialogueDef d;
    d.name = "Innkeeper";
    d.start = "hello";
    d.nodes.push_back(N("hello", "Welcome, traveller.", "ask"));
    d.nodes.back().speaker = "Maren";
    Node ask = N("ask", "A room for the night?");
    ask.speaker = "Maren";
    ask.choices = {{"Yes please", "deal"}, {"No", "bye"}, {"Leave", ""}};
    d.nodes.push_back(ask);
    Node deal = N("deal", "That is two coins.", "bye");
    deal.event = "take_coin";
    d.nodes.push_back(deal);
    d.nodes.push_back(N("bye", "Safe roads."));
    return d;
}

struct Rig {
    World world;
    DialogueLibrary library;
    DialogueSystem system{world, library};
    Entity e;

    Rig() {
        CHECK(library.Register(Innkeeper()));
        e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    }
};

} // namespace

AETHER_TEST(Dialogue_ParseValidateAndRoundTrip) {
    DialogueDef good = Innkeeper();
    good.title = "The Inn";
    DialogueDef back;
    std::string error;
    CHECK(DialogueFromJson(DialogueToJson(good).dump(), back, &error) && back == good);

    // A file may leave `start` out: the first node is it.
    CHECK(DialogueFromJson(R"({"name":"D","nodes":[{"id":"a","text":"Hi"}]})", back, &error) && back.start == "a");

    const auto refused = [&](const std::string& text, const char* code) {
        DialogueDef d;
        std::string why;
        return !DialogueFromJson(text, d, &why) && why.rfind(code, 0) == 0;
    };
    CHECK(refused("not json", "dialogue.bad_json"));
    CHECK(refused(R"({"nodes":[{"id":"a","text":"Hi"}]})", "dialogue.name_empty"));
    CHECK(refused(R"({"name":"D"})", "dialogue.no_nodes"));
    CHECK(refused(R"({"name":"D","nodes":[{"text":"Hi"}]})", "dialogue.node_id_empty"));
    CHECK(refused(R"({"name":"D","nodes":[{"id":"a","text":"Hi"},{"id":"a","text":"Yo"}]})", "dialogue.duplicate_node"));
    CHECK(refused(R"({"name":"D","start":"zzz","nodes":[{"id":"a","text":"Hi"}]})", "dialogue.unknown_start"));
    CHECK(refused(R"({"name":"D","nodes":[{"id":"a"}]})", "dialogue.text_empty"));
    CHECK(refused(R"({"name":"D","nodes":[{"id":"a","text":"Hi","next":"zzz"}]})", "dialogue.unknown_next"));
    CHECK(refused(R"({"name":"D","nodes":[{"id":"a","text":"Hi","choices":[{"text":"x","next":"zzz"}]}]})", "dialogue.unknown_next"));
    CHECK(refused(R"({"name":"D","nodes":[{"id":"a","text":"Hi","next":"a","choices":[{"text":"x"}]}]})", "dialogue.next_and_choices"));
    CHECK(refused(R"({"name":"D","nodes":[{"id":"a","text":"Hi","choices":[{"text":""}]}]})", "dialogue.choice_text_empty"));
    CHECK(refused(R"({"name":3,"nodes":[{"id":"a","text":"Hi"}]})", "dialogue.bad_json"));
    CHECK(refused(R"({"name":"D","nodes":[{"id":"a","text":"Hi","choices":"no"}]})", "dialogue.bad_json"));

    DialogueDef many;
    many.name = "Many";
    many.start = "n0";
    for (usize i = 0; i <= DialogueDef::kMaxNodes; ++i) many.nodes.push_back(N(("n" + std::to_string(i)).c_str(), "x"));
    CHECK(many.Validate().rfind("dialogue.too_many_nodes", 0) == 0);
    DialogueDef wide = Innkeeper();
    wide.nodes[1].choices.assign(DialogueDef::kMaxChoices + 1, Choice{"x", "bye"});
    CHECK(wide.Validate().rfind("dialogue.too_many_choices", 0) == 0);

    DialogueLibrary library;
    CHECK(library.Register(Innkeeper()) && library.Find("Innkeeper") != nullptr && library.Find("Nope") == nullptr && library.Size() == 1);
    DialogueDef invalid;
    CHECK(!library.Register(invalid, &error) && error.rfind("dialogue.name_empty", 0) == 0);
}

AETHER_TEST(Dialogue_RunsNodesChoicesAndEnds) {
    Rig r;
    CHECK(DialogueSystem::Active() == &r.system);
    CHECK(!r.system.IsActive(r.e) && !r.system.Current(r.e).has_value() && r.system.Advance(r.e) == Result::NotActive && r.system.Choose(r.e, 0) == Result::NotActive);
    CHECK(r.system.Start(r.e, "Nope") == Result::UnknownDialogue && r.system.Start(Entity{}, "Innkeeper") == Result::DeadEntity);

    CHECK(r.system.Start(r.e, "Innkeeper") == Result::Ok && r.system.IsActive(r.e));
    std::optional<Line> line = r.system.Current(r.e);
    CHECK(line.has_value() && line->node == "hello" && line->speaker == "Maren" && line->text == "Welcome, traveller." && line->choices.empty() && !line->last);
    CHECK(r.system.Choose(r.e, 0) == Result::BadChoice); // a line without choices has none to take
    CHECK(r.system.Advance(r.e) == Result::Ok);
    line = r.system.Current(r.e);
    if (!line) return;
    CHECK(line->node == "ask" && line->choices == (std::vector<std::string>{"Yes please", "No", "Leave"}));
    CHECK(r.system.Advance(r.e) == Result::NeedsChoice); // a question is answered, not skipped
    CHECK(r.system.Choose(r.e, 3) == Result::BadChoice && r.system.Choose(r.e, -1) == Result::BadChoice);
    CHECK(r.system.Current(r.e)->node == "ask"); // a refused call changes nothing
    CHECK(r.system.Choose(r.e, 0) == Result::Ok);
    line = r.system.Current(r.e);
    if (!line) return;
    CHECK(line->node == "deal" && line->event == "take_coin");
    CHECK(r.system.Advance(r.e) == Result::Ok);
    line = r.system.Current(r.e);
    if (!line) return;
    CHECK(line->node == "bye" && line->last);
    CHECK(r.system.Advance(r.e) == Result::Ok && !r.system.IsActive(r.e) && r.system.Advance(r.e) == Result::NotActive); // the last line ends it

    // A choice with no next node ends the conversation, and a new one may start over a running one.
    CHECK(r.system.Start(r.e, "Innkeeper") == Result::Ok && r.system.Advance(r.e) == Result::Ok && r.system.Choose(r.e, 2) == Result::Ok && !r.system.IsActive(r.e));
    CHECK(r.system.Start(r.e, "Innkeeper") == Result::Ok && r.system.Advance(r.e) == Result::Ok && r.system.Start(r.e, "Innkeeper") == Result::Ok);
    CHECK(r.system.Current(r.e)->node == "hello");
    r.system.End(r.e);
    CHECK(!r.system.IsActive(r.e));

    // Conversations are per entity, and a destroyed listener hears nothing.
    const Entity other = r.world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    CHECK(r.system.Start(r.e, "Innkeeper") == Result::Ok && !r.system.IsActive(other));
    CHECK(r.system.Start(other, "Innkeeper") == Result::Ok && r.system.Advance(other) == Result::Ok && r.system.Current(r.e)->node == "hello");
    r.world.DestroyEntity(r.e);
    CHECK(!r.system.IsActive(r.e) && r.system.Advance(r.e) == Result::DeadEntity && r.system.Start(r.e, "Innkeeper") == Result::DeadEntity);
}

AETHER_TEST(Dialogue_TheKitLoadsConversationsAndReportsBadOnes) {
    const std::map<std::string, std::string> files = {
        {"Dialogue/ok.adialogue", DialogueToJson(Innkeeper()).dump()},
        {"Dialogue/badjson.adialogue", "{ not json"},
        {"Dialogue/dangling.adialogue", R"({"name":"Dangling","nodes":[{"id":"a","text":"Hi","next":"zzz"}]})"},
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
    DialogueLibrary loaded;
    const std::vector<DialogueAssetDiagnostic> diagnostics = LoadDialogueAssets(
        {"Dialogue/ok.adialogue", "Dialogue/badjson.adialogue", "Dialogue/dangling.adialogue", "Dialogue/gone.adialogue"}, read, loaded);
    CHECK(loaded.Size() == 1 && loaded.Find("Innkeeper") != nullptr);
    CHECK(diagnostics.size() == 3);
    for (const DialogueAssetDiagnostic& d : diagnostics) {
        if (d.path == "Dialogue/badjson.adialogue") CHECK(!d.content_read_failure && d.message.rfind("dialogue.bad_json", 0) == 0);
        if (d.path == "Dialogue/dangling.adialogue") CHECK(!d.content_read_failure && d.message.rfind("dialogue.unknown_next", 0) == 0);
        if (d.path == "Dialogue/gone.adialogue") CHECK(d.content_read_failure);
    }
    // The kit needs nothing and loads only the assets of its importer.
    DialogueLibrary through_kit;
    const std::unique_ptr<kit::IKit> kit = MakeDialogueKit(&through_kit);
    CHECK(std::string(kit->Name()) == "Dialogue" && kit->Deps().empty() && kit->Stages().empty());
    kit::KitAssetContext context;
    context.assets = {{"DialogueDefinition", "Dialogue/ok.adialogue"}, {"ItemDefinition", "Dialogue/dangling.adialogue"}};
    context.read_content = read;
    kit->LoadAssets(context);
    CHECK(through_kit.Size() == 1 && through_kit.Find("Innkeeper") != nullptr);
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer) {
    return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", importer}};
}

// A package with an empty scene, a good conversation and one that leads to a missing node.
std::filesystem::path MakePackage(const char* dir_name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / dir_name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array()}};
    const nlohmann::json assets_json = nlohmann::json::array({AssetEntry("Scenes/start.ascene", "Scene"), AssetEntry("Dialogue/inn.adialogue", "DialogueDefinition"),
                                                              AssetEntry("Dialogue/dangling.adialogue", "DialogueDefinition")});
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()}, {"assets", assets_json}, {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump(2));
    writer.Add("Content/Scenes/start.ascene", scene.dump(2));
    writer.Add("Content/Dialogue/inn.adialogue", DialogueToJson(Innkeeper()).dump());
    writer.Add("Content/Dialogue/dangling.adialogue", R"({"name":"Dangling","nodes":[{"id":"a","text":"Hi","next":"zzz"}]})");
    const std::filesystem::path file = dir / "Game.apak";
    std::string error;
    AETHER_CHECK(writer.Write(file.string(), &error));
    return file;
}

} // namespace

AETHER_TEST(Dialogue_PlayerLoadsConversationsFromThePackageAndRunsThem) {
    const std::filesystem::path file = MakePackage("aether_dialogue_player_tests");
    std::string error;
    player::GamePackage package;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    CHECK(game.LoadStartupScene(&error));
    bool warned = false;
    for (const std::string& w : game.Warnings()) warned = warned || (w.find("dangling.adialogue") != std::string::npos && w.find("dialogue.unknown_next") != std::string::npos);
    CHECK(warned); // the conversation leading nowhere is reported, the good one loads
    DialogueSystem* dialogue = DialogueSystem::Active();
    CHECK(dialogue != nullptr);
    if (!dialogue) return;
    CHECK(dialogue->Dialogues().Find("Innkeeper") != nullptr && dialogue->Dialogues().Find("Dangling") == nullptr);
    const Entity e = game.GetWorld().CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    game.BeginPlay();
    CHECK(dialogue->Start(e, "Innkeeper") == Result::Ok && dialogue->Advance(e) == Result::Ok && dialogue->Choose(e, 1) == Result::Ok);
    CHECK(dialogue->Current(e).has_value() && dialogue->Current(e)->node == "bye");
}

#if AETHER_TEST_HAS_SCRIPTING
AETHER_TEST(Dialogue_LuauTableActsOnTheSystem) {
    Rig r;
    script::LuauHost host;
    GuidIndex guids;
    const assets::AssetGuid hero = assets::NewAssetGuid();
    (void)GetComponentId<ScriptComponent>();
    script::ScriptSystem scripts(host, r.world, guids, [&](const assets::AssetGuid& g, std::string& source, std::string& name) {
        if (g != hero) return false;
        source = "local H = {} function H:Run() local e = self.entity; self.idle = Dialogue.IsActive(e); self.started = Dialogue.Start(e, 'Innkeeper'); "
                 "self.speaker = Dialogue.Speaker(e); self.text = Dialogue.Text(e); self.count0 = Dialogue.ChoiceCount(e); self.adv = Dialogue.Advance(e); "
                 "self.count = Dialogue.ChoiceCount(e); self.first = Dialogue.Choice(e, 1); self.third = Dialogue.Choice(e, 3); self.none = Dialogue.Choice(e, 4); "
                 "self.skip = Dialogue.Advance(e); self.zero = Dialogue.Choose(e, 0); self.pick = Dialogue.Choose(e, 1); self.event = Dialogue.Event(e); "
                 "self.last0 = Dialogue.IsLast(e); Dialogue.Advance(e); self.last1 = Dialogue.IsLast(e); self.over = Dialogue.Advance(e); self.active = Dialogue.IsActive(e); "
                 "self.unknown = Dialogue.Start(e, 'Nope'); self.exists = Dialogue.Exists('Innkeeper'); self.gone = Dialogue.Exists('Nope'); "
                 "Dialogue.Start(e, 'Innkeeper'); Dialogue.End(e); self.ended = Dialogue.IsActive(e) end return H";
        name = "H.luau";
        return true;
    });
    ScriptComponent component;
    component.script.guid = hero;
    const Entity e = r.world.CreateEntity(IdComponent{NewEntityGuid()}, std::move(component));
    guids.Add(r.world.GetComponent<IdComponent>(e)->guid, e);
    Lifecycle life(r.world, guids);
    scripts.Register(life);
    life.BeginPlay();
    CHECK(!host.Run("return Dialogue.Start(5, 'Innkeeper')").ok && !host.Run("return Dialogue.IsActive(nil)").ok && !host.Run("return Dialogue.Exists(7)").ok &&
          !host.Run("return Dialogue.Choose(game, 'x')").ok); // wrong types are script errors
    CHECK(scripts.SendEvent(e, "Run") && scripts.Errors().empty());
    const auto text = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string("<not a string>"); };
    const auto flag = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<bool>(v) && std::get<bool>(v); };
    const auto num = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<f64>(v) ? std::get<f64>(v) : -999.0; };
    CHECK(!flag("idle") && text("started") == "ok" && text("speaker") == "Maren" && text("text") == "Welcome, traveller." && num("count0") == 0.0);
    CHECK(text("adv") == "ok" && num("count") == 3.0 && text("first") == "Yes please" && text("third") == "Leave" && text("none").empty());
    CHECK(text("skip") == "needs_choice" && text("zero") == "bad_choice" && text("pick") == "ok" && text("event") == "take_coin");
    CHECK(!flag("last0") && flag("last1") && text("over") == "ok" && !flag("active"));
    CHECK(text("unknown") == "unknown_dialogue" && flag("exists") && !flag("gone") && !flag("ended"));
    life.EndPlay();
}
#endif

#endif
