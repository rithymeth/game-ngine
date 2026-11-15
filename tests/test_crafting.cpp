#include "test_framework.h"

#ifdef AETHER_TEST_HAS_CRAFTING

#include "aether/assets/asset_guid.h"
#include "aether/crafting/crafting_kit.h"
#include "aether/crafting/crafting_system.h"
#include "aether/ecs/world.h"
#include "aether/inventory/inventory_system.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/scene/components.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <tuple>

// The crafting kit: recipes (`.arecipe`) as data, crafted all or nothing from an entity's inventory.

using namespace aether;
using namespace aether::craft;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

inv::ItemDef Item(const char* name, i32 stack) {
    inv::ItemDef d;
    d.name = name;
    d.max_stack = stack;
    return d;
}

RecipeDef Recipe(const char* name, std::vector<Ingredient> inputs, std::vector<Ingredient> outputs) {
    RecipeDef r;
    r.name = name;
    r.inputs = std::move(inputs);
    r.outputs = std::move(outputs);
    return r;
}

// Scrap stacks to 10, Component to 5, Core does not stack; the Core recipe takes 4 Scrap and 2 Components.
struct Rig {
    World world;
    inv::ItemLibrary items;
    inv::InventorySystem inventory{world, nullptr, items};
    RecipeLibrary recipes;
    Entity e;

    Rig() {
        for (const inv::ItemDef& d : {Item("Scrap", 10), Item("Component", 5), Item("Core", 1)}) CHECK(items.Register(d));
        CHECK(recipes.Register(Recipe("Core", {{"Scrap", 4}, {"Component", 2}}, {{"Core", 1}})));
        CHECK(recipes.Register(Recipe("Cores", {{"Scrap", 1}}, {{"Core", 3}}))); // three unstackable outputs: needs three slots
        e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    }
};

} // namespace

AETHER_TEST(Crafting_RecipesParseValidateAndRoundTrip) {
    RecipeDef good = Recipe("Core", {{"Scrap", 4}, {"Component", 2}}, {{"Core", 1}});
    good.title = "Generator Core";
    RecipeDef back;
    std::string error;
    CHECK(RecipeFromJson(RecipeToJson(good).dump(), back, &error) && back == good);

    const auto refused = [&](const std::string& text, const char* code) {
        RecipeDef r;
        std::string why;
        return !RecipeFromJson(text, r, &why) && why.rfind(code, 0) == 0;
    };
    CHECK(refused("not json", "recipe.bad_json"));
    CHECK(refused(R"({"inputs":[{"item":"A"}],"outputs":[{"item":"B"}]})", "recipe.name_empty"));
    CHECK(refused(R"({"name":"R","outputs":[{"item":"B"}]})", "recipe.no_inputs"));
    CHECK(refused(R"({"name":"R","inputs":[{"item":"A"}]})", "recipe.no_outputs"));
    CHECK(refused(R"({"name":"R","inputs":[{"item":"A","count":0}],"outputs":[{"item":"B"}]})", "recipe.bad_count"));
    CHECK(refused(R"({"name":"R","inputs":[{"item":"A","count":100001}],"outputs":[{"item":"B"}]})", "recipe.bad_count"));
    CHECK(refused(R"({"name":"R","inputs":[{"item":"A","count":1.5}],"outputs":[{"item":"B"}]})", "recipe.bad_count"));
    CHECK(refused(R"({"name":"R","inputs":[{"item":"A"},{"item":"A"}],"outputs":[{"item":"B"}]})", "recipe.duplicate_item"));
    CHECK(refused(R"({"name":"R","inputs":[{"item":""}],"outputs":[{"item":"B"}]})", "recipe.item_empty"));
    CHECK(refused(R"({"name":"R","inputs":[{"count":2}],"outputs":[{"item":"B"}]})", "recipe.bad_json"));
    CHECK(refused(R"({"name":3,"inputs":[{"item":"A"}],"outputs":[{"item":"B"}]})", "recipe.bad_json"));

    // The library finds by name, lists sorted, and checks that the items are items.
    Rig r;
    CHECK(r.recipes.Find("Core") != nullptr && r.recipes.Find("Nope") == nullptr && r.recipes.Size() == 2);
    CHECK(r.recipes.Names() == (std::vector<std::string>{"Core", "Cores"}));
    CHECK(r.recipes.Check(r.items).empty());
    CHECK(r.recipes.Register(Recipe("Ghost", {{"Scrap", 1}}, {{"Phantom", 1}})));
    const std::vector<std::string> problems = r.recipes.Check(r.items);
    CHECK(problems.size() == 1 && problems[0].rfind("recipe.unknown_item", 0) == 0 && problems[0].find("Phantom") != std::string::npos);
    RecipeDef invalid;
    CHECK(!r.recipes.Register(invalid, &error) && error.rfind("recipe.name_empty", 0) == 0);
}

AETHER_TEST(Crafting_CraftsAllOrNothing) {
    Rig r;
    CraftingSystem crafting(r.inventory, r.recipes);
    CHECK(CraftingSystem::Active() == &crafting);
    CHECK(r.inventory.Configure(r.e, 2)); // two slots

    // Short of inputs: nothing changes, and it says what is missing.
    CHECK(r.inventory.Add(r.e, "Scrap", 3) == 3 && r.inventory.Add(r.e, "Component", 2) == 2);
    CHECK(!crafting.CanCraft(r.e, "Core"));
    const std::vector<Ingredient> lacking = crafting.Missing(r.e, "Core");
    CHECK(lacking.size() == 1 && lacking[0].item == "Scrap" && lacking[0].count == 1);
    CHECK(crafting.Craft(r.e, "Core") == Result::MissingInputs);
    CHECK(r.inventory.Count(r.e, "Scrap") == 3 && r.inventory.Count(r.e, "Component") == 2 && r.inventory.Count(r.e, "Core") == 0);

    // With everything: the inputs go, the output arrives.
    CHECK(r.inventory.Add(r.e, "Scrap", 1) == 1);
    CHECK(crafting.CanCraft(r.e, "Core") && crafting.Missing(r.e, "Core").empty());
    CHECK(crafting.Craft(r.e, "Core") == Result::Ok);
    CHECK(r.inventory.Count(r.e, "Scrap") == 0 && r.inventory.Count(r.e, "Component") == 0 && r.inventory.Count(r.e, "Core") == 1);

    // The outputs do not fit (three unstackable Cores in two slots): everything is as before, the Scrap included.
    CHECK(r.inventory.Add(r.e, "Scrap", 1) == 1);
    CHECK(r.inventory.Count(r.e, "Core") == 1 && r.inventory.Count(r.e, "Scrap") == 1);
    CHECK(crafting.Craft(r.e, "Cores") == Result::NoRoom);
    CHECK(r.inventory.Count(r.e, "Scrap") == 1 && r.inventory.Count(r.e, "Core") == 1);

    // Other refusals.
    CHECK(crafting.Craft(r.e, "Nope") == Result::UnknownRecipe && !crafting.CanCraft(r.e, "Nope") && crafting.Missing(r.e, "Nope").empty());
    CHECK(crafting.Craft(r.e, "Core", 0) == Result::BadCount && crafting.Craft(r.e, "Core", CraftingSystem::kMaxTimes + 1) == Result::BadCount && !crafting.CanCraft(r.e, "Core", -1));
    CHECK(crafting.Craft(Entity{}, "Core") == Result::DeadEntity);
    CHECK(std::string(ResultName(Result::NoRoom)) == "no_room" && std::string(ResultName(Result::MissingInputs)) == "missing_inputs");
}

AETHER_TEST(Crafting_CraftsSeveralAtOnceAndFeedsTheItemEvents) {
    Rig r;
    CraftingSystem crafting(r.inventory, r.recipes);
    CHECK(r.inventory.Configure(r.e, 6));
    CHECK(r.inventory.Add(r.e, "Scrap", 8) == 8 && r.inventory.Add(r.e, "Component", 4) == 4);
    r.inventory.ClearEvents();
    CHECK(crafting.Craft(r.e, "Core", 2) == Result::Ok);
    CHECK(r.inventory.Count(r.e, "Core") == 2 && r.inventory.Count(r.e, "Scrap") == 0 && r.inventory.Count(r.e, "Component") == 0);
    // The inventory's own events say what happened (what quests that count items listen to): two inputs taken, one output given.
    i32 removed = 0, added = 0;
    for (const inv::ItemEvent& event : r.inventory.Events()) {
        if (event.kind == inv::ItemEvent::Kind::Removed) removed += event.count;
        if (event.kind == inv::ItemEvent::Kind::Added) added += event.count;
    }
    CHECK(removed == 8 + 4 && added == 2);
    // Twice needs twice the inputs.
    CHECK(r.inventory.Add(r.e, "Scrap", 4) == 4 && r.inventory.Add(r.e, "Component", 2) == 2);
    CHECK(!crafting.CanCraft(r.e, "Core", 2) && crafting.Missing(r.e, "Core", 2).size() == 2 && crafting.CanCraft(r.e, "Core", 1));
}

AETHER_TEST(Crafting_TheKitLoadsRecipesAndReportsBadOnes) {
    Rig r;
    RecipeLibrary loaded;
    std::map<std::string, std::string> files = {
        {"Recipes/ok.arecipe", R"({"name":"Core","inputs":[{"item":"Scrap","count":4}],"outputs":[{"item":"Core"}]})"},
        {"Recipes/badjson.arecipe", "{"},
        {"Recipes/ghost.arecipe", R"({"name":"Ghost","inputs":[{"item":"Scrap"}],"outputs":[{"item":"Phantom"}]})"},
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
    const std::vector<RecipeAssetDiagnostic> diagnostics = LoadRecipeAssets({"Recipes/ok.arecipe", "Recipes/badjson.arecipe", "Recipes/ghost.arecipe", "Recipes/gone.arecipe"}, read, r.items, loaded);
    CHECK(loaded.Size() == 1 && loaded.Find("Core") != nullptr);
    CHECK(diagnostics.size() == 3);
    for (const RecipeAssetDiagnostic& d : diagnostics) {
        if (d.path == "Recipes/badjson.arecipe") CHECK(!d.content_read_failure && d.message.rfind("recipe.bad_json", 0) == 0);
        if (d.path == "Recipes/ghost.arecipe") CHECK(!d.content_read_failure && d.message.rfind("recipe.unknown_item", 0) == 0);
        if (d.path == "Recipes/gone.arecipe") CHECK(d.content_read_failure);
    }
    // The kit names what it needs and loads only the assets of its importer.
    RecipeLibrary through_kit;
    const std::unique_ptr<kit::IKit> kit = MakeCraftingKit(&through_kit, &r.items);
    CHECK(std::string(kit->Name()) == "Crafting" && kit->Deps() == std::vector<std::string>{"Inventory"} && kit->Stages().empty());
    kit::KitAssetContext context;
    context.assets = {{"RecipeDefinition", "Recipes/ok.arecipe"}, {"ItemDefinition", "Recipes/ghost.arecipe"}};
    context.read_content = read;
    kit->LoadAssets(context);
    CHECK(through_kit.Size() == 1 && through_kit.Find("Core") != nullptr);
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer) {
    return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", importer}};
}

// A package with an empty scene, two items, a good recipe and one that names a missing item.
std::filesystem::path MakePackage(const char* dir_name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / dir_name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array()}};
    const nlohmann::json assets_json = nlohmann::json::array({AssetEntry("Scenes/start.ascene", "Scene"), AssetEntry("Items/scrap.aitem", "ItemDefinition"),
                                                              AssetEntry("Items/core.aitem", "ItemDefinition"), AssetEntry("Recipes/core.arecipe", "RecipeDefinition"),
                                                              AssetEntry("Recipes/ghost.arecipe", "RecipeDefinition")});
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()}, {"assets", assets_json}, {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump(2));
    writer.Add("Content/Scenes/start.ascene", scene.dump(2));
    writer.Add("Content/Items/scrap.aitem", nlohmann::json{{"name", "Scrap"}, {"max_stack", 10}}.dump());
    writer.Add("Content/Items/core.aitem", nlohmann::json{{"name", "Core"}, {"max_stack", 1}}.dump());
    writer.Add("Content/Recipes/core.arecipe", nlohmann::json{{"name", "Core"}, {"title", "Generator Core"}, {"inputs", {{{"item", "Scrap"}, {"count", 4}}}}, {"outputs", {{{"item", "Core"}}}}}.dump());
    writer.Add("Content/Recipes/ghost.arecipe", nlohmann::json{{"name", "Ghost"}, {"inputs", {{{"item", "Scrap"}}}}, {"outputs", {{{"item", "Phantom"}}}}}.dump());
    const std::filesystem::path file = dir / "Game.apak";
    std::string error;
    AETHER_CHECK(writer.Write(file.string(), &error));
    return file;
}

} // namespace

AETHER_TEST(Crafting_PlayerLoadsRecipesFromThePackageAndCrafts) {
    const std::filesystem::path file = MakePackage("aether_crafting_player_tests");
    std::string error;
    player::GamePackage package;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    CHECK(game.LoadStartupScene(&error));
    bool warned = false;
    for (const std::string& w : game.Warnings()) warned = warned || (w.find("ghost.arecipe") != std::string::npos && w.find("recipe.unknown_item") != std::string::npos);
    CHECK(warned); // the recipe naming a missing item is reported, the good one loads
    CraftingSystem* crafting = CraftingSystem::Active();
    inv::InventorySystem* inventory = inv::InventorySystem::Active();
    CHECK(crafting != nullptr && inventory != nullptr);
    if (!crafting || !inventory) return;
    CHECK(crafting->Recipes().Find("Core") != nullptr && crafting->Recipes().Find("Ghost") == nullptr);
    const Entity e = game.GetWorld().CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    game.BeginPlay();
    CHECK(inventory->Add(e, "Scrap", 4) == 4 && crafting->Craft(e, "Core") == Result::Ok && inventory->Count(e, "Core") == 1 && inventory->Count(e, "Scrap") == 0);
    CHECK(crafting->Craft(e, "Core") == Result::MissingInputs);
}

#if AETHER_TEST_HAS_SCRIPTING
AETHER_TEST(Crafting_LuauTableActsOnTheSystem) {
    Rig r;
    CraftingSystem crafting(r.inventory, r.recipes);
    CHECK(r.recipes.Register([] { RecipeDef d = Recipe("Titled", {{"Scrap", 1}}, {{"Core", 1}}); d.title = "A Titled Thing"; return d; }()));
    script::LuauHost host;
    GuidIndex guids;
    const assets::AssetGuid hero = assets::NewAssetGuid();
    (void)GetComponentId<ScriptComponent>();
    script::ScriptSystem scripts(host, r.world, guids, [&](const assets::AssetGuid& g, std::string& source, std::string& name) {
        if (g != hero) return false;
        source = "local H = {} function H:Run() local e = self.entity; Inventory.Configure(e, 4, 0); Inventory.Add(e, 'Scrap', 3); Inventory.Add(e, 'Component', 2); "
                 "self.can_before = Crafting.CanCraft(e, 'Core'); self.missing = Crafting.MissingCount(e, 'Core'); self.first = Crafting.Craft(e, 'Core'); "
                 "Inventory.Add(e, 'Scrap', 1); self.can_after = Crafting.CanCraft(e, 'Core'); self.second = Crafting.Craft(e, 'Core'); self.cores = Inventory.Count(e, 'Core'); "
                 "self.unknown = Crafting.Craft(e, 'Nope'); self.exists = Crafting.Exists('Core'); self.gone = Crafting.Exists('Nope'); "
                 "self.title = Crafting.GetTitle('Titled'); self.plain = Crafting.GetTitle('Core'); self.text = Crafting.Describe('Core') end return H";
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
    CHECK(!host.Run("return Crafting.Craft(5, 'Core')").ok && !host.Run("return Crafting.CanCraft(nil, 'Core')").ok && !host.Run("return Crafting.Exists(7)").ok); // wrong types are script errors
    CHECK(scripts.SendEvent(e, "Run") && scripts.Errors().empty());
    const auto text = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string("<not a string>"); };
    const auto flag = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<bool>(v) && std::get<bool>(v); };
    const auto num = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<f64>(v) ? std::get<f64>(v) : -999.0; };
    CHECK(!flag("can_before") && num("missing") == 1.0 && text("first") == "missing_inputs");
    CHECK(flag("can_after") && text("second") == "ok" && num("cores") == 1.0 && text("unknown") == "unknown_recipe");
    CHECK(flag("exists") && !flag("gone") && text("title") == "A Titled Thing" && text("plain") == "Core");
    CHECK(text("text") == "4 Scrap + 2 Component -> 1 Core");
    life.EndPlay();
}
#endif

#endif
