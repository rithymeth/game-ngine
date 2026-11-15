#include "test_framework.h"

#ifdef AETHER_TEST_HAS_LOOT

#include "aether/assets/asset_guid.h"
#include "aether/ecs/world.h"
#include "aether/inventory/inventory_system.h"
#include "aether/loot/loot_kit.h"
#include "aether/loot/loot_system.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/scene/components.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <map>
#include <set>

// The loot kit: loot tables (`.aloot`) as data, rolled deterministically and given to an inventory.

using namespace aether;
using namespace aether::loot;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

inv::ItemDef Item(const char* name, i32 stack) {
    inv::ItemDef d;
    d.name = name;
    d.max_stack = stack;
    return d;
}

LootTableDef Table(const char* name, std::vector<LootEntry> entries, i32 rolls = 1) {
    LootTableDef t;
    t.name = name;
    t.entries = std::move(entries);
    t.rolls = rolls;
    return t;
}

LootEntry Entry(const char* item, i32 weight = 1, i32 min = 1, i32 max = 1) {
    LootEntry e;
    e.item = item;
    e.weight = weight;
    e.min_count = min;
    e.max_count = max;
    return e;
}

i32 CountOf(const std::vector<Drop>& drops, const std::string& item) {
    for (const Drop& d : drops) if (d.item == item) return d.count;
    return 0;
}

// Scrap stacks to 10, Component to 5, Core does not stack.
struct Rig {
    World world;
    inv::ItemLibrary items;
    inv::InventorySystem inventory{world, nullptr, items};
    LootLibrary tables;
    LootSystem system{inventory, tables};
    Entity e;

    Rig() {
        for (const inv::ItemDef& d : {Item("Scrap", 10), Item("Component", 5), Item("Core", 1)}) CHECK(items.Register(d));
        CHECK(tables.Register(Table("Salvage", {Entry("Scrap", 3, 1, 4), Entry("Component", 1, 1, 2)}, 2)));
        CHECK(tables.Register(Table("Cores", {Entry("Core", 1, 3, 3)})));
        e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    }
};

} // namespace

AETHER_TEST(Loot_TablesParseValidateAndRoundTrip) {
    LootTableDef good = Table("Salvage", {Entry("Scrap", 3, 1, 4), Entry("", 5), Entry("Component", 1, 2, 2)}, 3);
    good.title = "Salvage";
    LootTableDef back;
    std::string error;
    CHECK(LootTableFromJson(LootTableToJson(good).dump(), back, &error) && back == good);
    // Only a minimum means exactly that many; weight and rolls default to 1.
    CHECK(LootTableFromJson(R"({"name":"T","entries":[{"item":"Scrap","min":3}]})", back, &error) && back.rolls == 1 && back.entries[0].weight == 1 && back.entries[0].min_count == 3 && back.entries[0].max_count == 3);

    const auto refused = [&](const std::string& text, const char* code) {
        LootTableDef t;
        std::string why;
        return !LootTableFromJson(text, t, &why) && why.rfind(code, 0) == 0;
    };
    CHECK(refused("not json", "loot.bad_json"));
    CHECK(refused(R"({"entries":[{"item":"A"}]})", "loot.name_empty"));
    CHECK(refused(R"({"name":"T"})", "loot.no_entries"));
    CHECK(refused(R"({"name":"T","rolls":0,"entries":[{"item":"A"}]})", "loot.bad_rolls"));
    CHECK(refused(R"({"name":"T","rolls":101,"entries":[{"item":"A"}]})", "loot.bad_rolls"));
    CHECK(refused(R"({"name":"T","rolls":1.5,"entries":[{"item":"A"}]})", "loot.bad_rolls"));
    CHECK(refused(R"({"name":"T","entries":[{"item":"A","weight":0}]})", "loot.bad_weight"));
    CHECK(refused(R"({"name":"T","entries":[{"item":"A","weight":1000001}]})", "loot.bad_weight"));
    CHECK(refused(R"({"name":"T","entries":[{"item":"A","min":0}]})", "loot.bad_count"));
    CHECK(refused(R"({"name":"T","entries":[{"item":"A","min":5,"max":2}]})", "loot.bad_count"));
    CHECK(refused(R"({"name":"T","entries":[{"item":"A","max":100001}]})", "loot.bad_count"));
    CHECK(refused(R"({"name":"T","entries":[{"item":"","weight":3}]})", "loot.nothing_to_drop"));
    CHECK(refused(R"({"name":"T","entries":[{"item":5}]})", "loot.bad_json"));
    CHECK(refused(R"({"name":"T","entries":"no"})", "loot.bad_json"));

    // The library finds by name, lists sorted, refuses invalid tables, and checks that the items are items.
    Rig r;
    CHECK(r.tables.Find("Salvage") != nullptr && r.tables.Find("Nope") == nullptr && r.tables.Size() == 2 && r.tables.Names() == (std::vector<std::string>{"Cores", "Salvage"}));
    CHECK(LootLibrary::CheckItems(*r.tables.Find("Salvage"), r.items).empty());
    const std::string problem = LootLibrary::CheckItems(Table("Ghost", {Entry("Phantom")}), r.items);
    CHECK(problem.rfind("loot.unknown_item", 0) == 0 && problem.find("Phantom") != std::string::npos);
    CHECK(LootLibrary::CheckItems(Table("Maybe", {Entry(""), Entry("Scrap")}), r.items).empty()); // an empty entry is "nothing", not an unknown item
    LootTableDef invalid;
    CHECK(!r.tables.Register(invalid, &error) && error.rfind("loot.name_empty", 0) == 0);
}

AETHER_TEST(Loot_RollsAreDeterministicWeightedAndBounded) {
    const LootTableDef table = Table("T", {Entry("Scrap", 3, 1, 4), Entry("Component", 1, 1, 2), Entry("", 4)}, 1);
    // The same seed, the same drops; the drops depend on the seed.
    CHECK(RollLoot(table, 12345) == RollLoot(table, 12345));
    std::set<std::string> distinct;
    for (u64 seed = 0; seed < 200; ++seed) distinct.insert(DescribeDrops(RollLoot(table, seed)));
    CHECK(distinct.size() > 4);

    // Over many seeds each entry drops in proportion to its weight (3 : 1 : 4 of 8), and counts stay in their range.
    int scrap = 0, component = 0, nothing = 0;
    const int trials = 8000;
    for (int seed = 0; seed < trials; ++seed) {
        const std::vector<Drop> drops = RollLoot(table, static_cast<u64>(seed) * 7919u + 1u);
        if (drops.empty()) {
            ++nothing;
            continue;
        }
        CHECK(drops.size() == 1);
        const Drop& d = drops[0];
        if (d.item == "Scrap") {
            ++scrap;
            CHECK(d.count >= 1 && d.count <= 4);
        } else {
            CHECK(d.item == "Component" && d.count >= 1 && d.count <= 2);
            ++component;
        }
    }
    CHECK(std::abs(scrap - trials * 3 / 8) < 250 && std::abs(component - trials / 8) < 200 && std::abs(nothing - trials / 2) < 250);

    // Several rolls add up drops of the same item (in the order each item first dropped), and a count range reaches both of its ends.
    const LootTableDef many = Table("Many", {Entry("Scrap", 1, 1, 3)}, 50);
    std::set<i32> totals;
    for (u64 seed = 0; seed < 50; ++seed) {
        const std::vector<Drop> drops = RollLoot(many, seed);
        CHECK(drops.size() == 1 && drops[0].item == "Scrap" && drops[0].count >= 50 && drops[0].count <= 150);
        totals.insert(drops[0].count);
    }
    CHECK(totals.size() > 5);
    int ones = 0, threes = 0;
    for (u64 seed = 0; seed < 600; ++seed) {
        const i32 c = RollLoot(Table("One", {Entry("Scrap", 1, 1, 3)}), seed)[0].count;
        ones += c == 1;
        threes += c == 3;
    }
    CHECK(ones > 120 && threes > 120);
    // A table that always drops one thing, and an invalid table (drops nothing, does not crash).
    CHECK(RollLoot(Table("Sure", {Entry("Core", 1, 2, 2)}), 7) == (std::vector<Drop>{{"Core", 2}}));
    LootTableDef invalid = table;
    invalid.rolls = 0;
    CHECK(RollLoot(invalid, 1).empty());
    CHECK(DescribeDrops({}) == "nothing" && DescribeDrops({{"Scrap", 2}, {"Component", 1}}) == "2 Scrap, 1 Component");
}

AETHER_TEST(Loot_GivesToTheInventoryAndLosesWhatDoesNotFit) {
    Rig r;
    CHECK(LootSystem::Active() == &r.system);
    std::vector<Drop> given;
    CHECK(r.system.GiveSeeded(r.e, "Salvage", 99, &given) == Result::Ok);
    const std::vector<Drop> expected = RollLoot(*r.tables.Find("Salvage"), 99);
    CHECK(given == expected && !given.empty());
    for (const Drop& d : given) CHECK(r.inventory.Count(r.e, d.item) == d.count);
    i32 total = 0;
    for (const Drop& d : given) total += d.count;
    CHECK(r.system.LastGiven() == total);

    // The same seed drops the same again; the seed stream is repeatable too.
    Rig a, b;
    a.system.SetSeed(42);
    b.system.SetSeed(42);
    std::vector<Drop> from_a, from_b;
    CHECK(a.system.Give(a.e, "Salvage", &from_a) == Result::Ok && b.system.Give(b.e, "Salvage", &from_b) == Result::Ok && from_a == from_b);
    CHECK(a.system.Give(a.e, "Salvage", &from_a) == Result::Ok && b.system.Give(b.e, "Salvage", &from_b) == Result::Ok && from_a == from_b); // and the second roll
    b.system.SetSeed(42);
    std::vector<Drop> again;
    CHECK(b.system.Give(b.e, "Salvage", &again) == Result::Ok);
    Rig c;
    c.system.SetSeed(42);
    std::vector<Drop> first;
    CHECK(c.system.Give(c.e, "Salvage", &first) == Result::Ok && again == first); // SetSeed starts the stream over
    std::set<std::string> seen;
    Rig stream;
    for (int i = 0; i < 40; ++i) {
        std::vector<Drop> g;
        stream.system.Give(stream.e, "Salvage", &g);
        seen.insert(DescribeDrops(g));
    }
    CHECK(seen.size() > 3); // the stream moves on: not the same loot every time

    // Loot is not all or nothing: with two slots, three unstackable cores only two fit, and `given` says so.
    Rig tight;
    CHECK(tight.inventory.Configure(tight.e, 2));
    std::vector<Drop> cores;
    CHECK(tight.system.Give(tight.e, "Cores", &cores) == Result::Ok);
    CHECK(tight.inventory.Count(tight.e, "Core") == 2 && cores == (std::vector<Drop>{{"Core", 2}}) && tight.system.LastGiven() == 2);
    // ... and a full pack gets nothing, without an error.
    CHECK(tight.system.Give(tight.e, "Cores", &cores) == Result::Ok && cores.empty() && tight.system.LastGiven() == 0 && tight.inventory.Count(tight.e, "Core") == 2);

    // What it refuses, and an entity that is gone receiving nothing.
    Rig f;
    CHECK(f.system.Give(f.e, "Nope", &given) == Result::UnknownTable && given.empty() && f.system.LastGiven() == 0);
    CHECK(f.system.Give(Entity{}, "Salvage", &given) == Result::DeadEntity && f.system.GiveSeeded(Entity{}, "Salvage", 1) == Result::DeadEntity);
    const Entity gone = f.world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    f.world.DestroyEntity(gone);
    CHECK(f.system.GiveSeeded(gone, "Salvage", 5, &given) == Result::Ok && given.empty() && f.system.LastGiven() == 0);
    CHECK(f.system.Roll("Nope").empty() && !f.system.Roll("Cores").empty());
}

AETHER_TEST(Loot_TheKitLoadsTablesAndReportsBadOnes) {
    Rig r;
    const std::map<std::string, std::string> files = {
        {"Loot/ok.aloot", LootTableToJson(*r.tables.Find("Salvage")).dump()},
        {"Loot/badjson.aloot", "{ nope"},
        {"Loot/ghost.aloot", R"({"name":"Ghost","entries":[{"item":"Phantom"}]})"},
        {"Loot/empty.aloot", R"({"name":"Empty","entries":[{"item":""}]})"},
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
    LootLibrary loaded;
    const std::vector<LootAssetDiagnostic> diagnostics = LoadLootAssets({"Loot/ok.aloot", "Loot/badjson.aloot", "Loot/ghost.aloot", "Loot/empty.aloot", "Loot/gone.aloot"}, read, r.items, loaded);
    CHECK(loaded.Size() == 1 && loaded.Find("Salvage") != nullptr && diagnostics.size() == 4);
    for (const LootAssetDiagnostic& d : diagnostics) {
        if (d.path == "Loot/badjson.aloot") CHECK(!d.content_read_failure && d.message.rfind("loot.bad_json", 0) == 0);
        if (d.path == "Loot/ghost.aloot") CHECK(!d.content_read_failure && d.message.rfind("loot.unknown_item", 0) == 0);
        if (d.path == "Loot/empty.aloot") CHECK(!d.content_read_failure && d.message.rfind("loot.nothing_to_drop", 0) == 0);
        if (d.path == "Loot/gone.aloot") CHECK(d.content_read_failure);
    }
    // The kit names what it needs and loads only the assets of its importer.
    LootLibrary through_kit;
    const std::unique_ptr<kit::IKit> kit = MakeLootKit(&through_kit, &r.items);
    CHECK(std::string(kit->Name()) == "Loot" && kit->Deps() == std::vector<std::string>{"Inventory"} && kit->Stages().empty());
    kit::KitAssetContext context;
    context.assets = {{"LootTableDefinition", "Loot/ok.aloot"}, {"ItemDefinition", "Loot/ghost.aloot"}};
    context.read_content = read;
    kit->LoadAssets(context);
    CHECK(through_kit.Size() == 1 && through_kit.Find("Salvage") != nullptr);
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer) {
    return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", importer}};
}

} // namespace

AETHER_TEST(Loot_PlayerLoadsTablesFromThePackageAndGivesLoot) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_loot_player_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array()}};
    const nlohmann::json assets_json = nlohmann::json::array({AssetEntry("Scenes/start.ascene", "Scene"), AssetEntry("Items/scrap.aitem", "ItemDefinition"),
                                                              AssetEntry("Loot/scrap.aloot", "LootTableDefinition"), AssetEntry("Loot/ghost.aloot", "LootTableDefinition")});
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()}, {"assets", assets_json}, {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    CHECK(writer.Add("Manifest.json", manifest.dump(2)));
    CHECK(writer.Add("Content/Scenes/start.ascene", scene.dump(2)));
    CHECK(writer.Add("Content/Items/scrap.aitem", nlohmann::json{{"name", "Scrap"}, {"max_stack", 10}}.dump()));
    CHECK(writer.Add("Content/Loot/scrap.aloot", nlohmann::json{{"name", "Pile"}, {"rolls", 1}, {"entries", {{{"item", "Scrap"}, {"min", 4}}}}}.dump()));
    CHECK(writer.Add("Content/Loot/ghost.aloot", nlohmann::json{{"name", "Ghost"}, {"entries", {{{"item", "Phantom"}}}}}.dump()));
    const std::filesystem::path file = dir / "Game.apak";
    CHECK(writer.Write(file.string()));
    player::GamePackage package;
    std::string error;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    CHECK(game.LoadStartupScene(&error));
    bool warned = false;
    for (const std::string& w : game.Warnings()) warned = warned || (w.find("ghost.aloot") != std::string::npos && w.find("loot.unknown_item") != std::string::npos);
    CHECK(warned); // the table naming a missing item is reported, the good one loads
    LootSystem* loot = LootSystem::Active();
    inv::InventorySystem* inventory = inv::InventorySystem::Active();
    CHECK(loot != nullptr && inventory != nullptr);
    if (!loot || !inventory) return;
    CHECK(loot->Tables().Find("Pile") != nullptr && loot->Tables().Find("Ghost") == nullptr);
    const Entity e = game.GetWorld().CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    game.BeginPlay();
    std::vector<Drop> given;
    CHECK(loot->Give(e, "Pile", &given) == Result::Ok && given == (std::vector<Drop>{{"Scrap", 4}}) && inventory->Count(e, "Scrap") == 4);
}

#if AETHER_TEST_HAS_SCRIPTING
AETHER_TEST(Loot_LuauTableActsOnTheSystem) {
    Rig r;
    CHECK(r.tables.Register([] { LootTableDef t = Table("Titled", {Entry("Scrap", 1, 2, 2)}); t.title = "A Titled Pile"; return t; }()));
    script::LuauHost host;
    GuidIndex guids;
    const assets::AssetGuid hero = assets::NewAssetGuid();
    (void)GetComponentId<ScriptComponent>();
    script::ScriptSystem scripts(host, r.world, guids, [&](const assets::AssetGuid& g, std::string& source, std::string& name) {
        if (g != hero) return false;
        source = "local H = {} function H:Run() local e = self.entity; Inventory.Configure(e, 4, 0); "
                 "self.exists = Loot.Exists('Salvage'); self.gone = Loot.Exists('Nope'); self.title = Loot.GetTitle('Titled'); self.plain = Loot.GetTitle('Cores'); "
                 "self.sure = Loot.Give(e, 'Titled', 5); self.scrap = Inventory.Count(e, 'Scrap'); self.given = Loot.LastGiven(); "
                 "self.same_a = Loot.Give(e, 'Salvage', 77); self.after_a = Inventory.Count(e, 'Scrap') + Inventory.Count(e, 'Component'); "
                 "Loot.SetSeed(9); self.roll1 = Loot.Roll('Salvage'); Loot.SetSeed(9); self.roll2 = Loot.Roll('Salvage'); self.roll_bad = Loot.Roll('Nope'); "
                 "self.unknown = Loot.Give(e, 'Nope') end return H";
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
    CHECK(!host.Run("return Loot.Give(5, 'Salvage')").ok && !host.Run("return Loot.Give(game, 7)").ok && !host.Run("return Loot.Exists(7)").ok && !host.Run("return Loot.SetSeed('x')").ok); // wrong types are script errors
    CHECK(scripts.SendEvent(e, "Run") && scripts.Errors().empty());
    const auto text = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string("<not a string>"); };
    const auto flag = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<bool>(v) && std::get<bool>(v); };
    const auto num = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<f64>(v) ? std::get<f64>(v) : -999.0; };
    CHECK(flag("exists") && !flag("gone") && text("title") == "A Titled Pile" && text("plain") == "Cores");
    CHECK(text("sure") == "ok" && num("scrap") == 2.0 && num("given") == 2.0);
    // A seeded give is the table rolled with that seed.
    i32 expected = 0;
    for (const Drop& d : RollLoot(*r.tables.Find("Salvage"), 77)) expected += d.count;
    CHECK(text("same_a") == "ok" && num("after_a") == 2.0 + static_cast<f64>(expected));
    CHECK(!text("roll1").empty() && text("roll1") == text("roll2") && text("roll_bad").empty() && text("unknown") == "unknown_table");
    life.EndPlay();
}
#endif

#endif
