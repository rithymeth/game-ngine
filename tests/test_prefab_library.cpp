#include "aether/assets/hot_reload.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/prefab_library.h"
#include "aether/scene/serialization.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>

using namespace aether;
namespace stdfs = std::filesystem;
using Json = nlohmann::json;
using namespace std::chrono_literals;

namespace library_test {
struct Purse {
    i32 gold = 0;
};
// Version 2 renamed `gold` to `coins` (see the migration registered below).
struct Wallet {
    i32 coins = 0;
    i32 cards = 0;
};
} // namespace library_test

AETHER_REFLECT(library_test::Purse, 1, AETHER_FIELD(gold))
AETHER_REFLECT(library_test::Wallet, 2, AETHER_FIELD(coins), AETHER_FIELD(cards))

namespace {

using library_test::Purse;
using library_test::Wallet;

void MigrateWallet(u16 from_version, Json& data) {
    if (from_version < 2 && data.contains("gold")) {
        data["coins"] = data["gold"];
        data.erase("gold");
    }
}

const bool kWalletMigration = [] {
    reflect::RegisterMigration(reflect::Reflect<Wallet>(), &MigrateWallet);
    return true;
}();

PrefabEntity Entity1(PrefabLocalId id, PrefabLocalId parent, i32 gold) {
    PrefabEntity entity;
    entity.id = id;
    entity.parent = parent;
    entity.components["Purse"] = {{"$v", 1}, {"gold", gold}};
    return entity;
}

struct Project {
    stdfs::path dir;
    std::unique_ptr<assets::AssetDatabase> db;
    std::unique_ptr<PrefabLibrary> library;

    explicit Project(const char* name) : dir(stdfs::temp_directory_path() / name) {
        stdfs::remove_all(dir);
        stdfs::create_directories(dir / "Content");
        (void)GetComponentId<Purse>();
        (void)GetComponentId<Wallet>();
        db = std::make_unique<assets::AssetDatabase>(dir / "Content");
        library = std::make_unique<PrefabLibrary>(*db);
    }
    ~Project() { stdfs::remove_all(dir); }

    assets::AssetGuid Write(const std::string& path, const PrefabData& data) {
        SavePrefab(data, dir / "Content" / path);
        db->Scan();
        return db->FindByPath(path)->guid;
    }
};

Entity Linked(World& world, Entity root, PrefabLocalId local_id) {
    const EntityGuid instance = world.GetComponent<IdComponent>(root)->guid;
    Entity found = kNullEntity;
    world.ForEachArchetype([&](const Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<PrefabLink>())) {
            return;
        }
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            for (u32 row = 0; row < archetype.ChunkEntityCount(c); ++row) {
                const Entity e = archetype.EntityArray(c)[row];
                const PrefabLink* link = world.GetComponent<PrefabLink>(e);
                if (link->instance == instance && link->local_id == local_id) {
                    found = e;
                }
            }
        }
    });
    return found;
}

} // namespace

AETHER_TEST(PrefabLibrary_PropagatesSavesToDependentInstances) {
    Project p("aether_test_prefab_library");
    PrefabData coin;
    coin.entities = {Entity1(1, 0, 1)};
    const assets::AssetGuid coin_guid = p.Write("coin.aprefab", coin);
    PrefabData pouch; // holds a coin
    PrefabEntity holder = Entity1(1, 0, 0);
    PrefabEntity nested;
    nested.id = 2;
    nested.parent = 1;
    nested.nested = NestedPrefab{coin_guid, {}, {}};
    pouch.entities = {holder, nested};
    const assets::AssetGuid pouch_guid = p.Write("pouch.aprefab", pouch);
    PrefabData big_pouch; // a variant of the pouch
    big_pouch.base = pouch_guid;
    const assets::AssetGuid big_guid = p.Write("big_pouch.aprefab", big_pouch);
    PrefabData gem;
    gem.entities = {Entity1(1, 0, 100)};
    const assets::AssetGuid gem_guid = p.Write("gem.aprefab", gem);

    AETHER_CHECK(p.library->Dependents(coin_guid).size() == 3);
    AETHER_CHECK(p.library->Dependents(gem_guid) == std::vector<assets::AssetGuid>{gem_guid});
    AETHER_CHECK(p.library->Flattened(big_guid) != nullptr && p.library->Flattened(big_guid)->entities.size() == 2);
    AETHER_CHECK(p.library->Get(assets::NewAssetGuid()) == nullptr);

    World world;
    GuidIndex guids;
    Entity coin_i = InstantiatePrefab(world, guids, coin_guid, *p.library->Flattened(coin_guid));
    Entity pouch_i = InstantiatePrefab(world, guids, pouch_guid, *p.library->Flattened(pouch_guid));
    Entity big_i = InstantiatePrefab(world, guids, big_guid, *p.library->Flattened(big_guid));
    Entity gem_i = InstantiatePrefab(world, guids, gem_guid, *p.library->Flattened(gem_guid));
    SetOverride(*world.GetComponent<PrefabInstance>(pouch_i), 2, "Purse", "gold", 42);
    ResolvePrefabInstance(world, guids, pouch_i, *p.library->Flattened(pouch_guid));
    const Entity pouch_coin = Linked(world, pouch_i, 2);

    // Save a change to the coin: every instance built on it follows, the
    // gem's doesn't get touched, and entities are reused.
    coin.entities[0].components["Purse"]["gold"] = 5;
    std::string error;
    AETHER_CHECK(p.library->Save(coin_guid, coin, &error).size() == 3);
    PropagationReport report = PropagatePrefabChange(world, guids, *p.library, coin_guid);
    AETHER_CHECK(report.instances == 3 && report.failed == 0 && report.orphaned == 0);
    AETHER_CHECK(world.GetComponent<Purse>(coin_i)->gold == 5);
    AETHER_CHECK(world.GetComponent<Purse>(Linked(world, big_i, 2))->gold == 5);
    AETHER_CHECK(Linked(world, pouch_i, 2) == pouch_coin && world.GetComponent<Purse>(pouch_coin)->gold == 42);
    AETHER_CHECK(world.GetComponent<Purse>(gem_i)->gold == 100);

    // The file on disk has the change (a fresh library sees it).
    PrefabLibrary fresh(*p.db);
    AETHER_CHECK(fresh.Get(coin_guid)->entities[0].components["Purse"]["gold"] == 5);

    // A breaking change: the coin loses its Purse, so the pouch instance's
    // override is orphaned (and reported), not deleted.
    coin.entities[0].components.erase("Purse");
    p.library->Save(coin_guid, coin);
    report = PropagatePrefabChange(world, guids, *p.library, coin_guid);
    AETHER_CHECK(report.instances == 3 && report.orphaned == 1);
    AETHER_CHECK(world.GetComponent<PrefabInstance>(pouch_i)->overrides.size() == 1);
    AETHER_CHECK(!world.HasComponent<Purse>(coin_i));

    // A broken prefab (a cycle) fails its instances and says why.
    PrefabData looped = coin;
    looped.base = big_guid; // coin -> big_pouch -> pouch -> coin
    looped.entities.clear();
    p.library->Save(coin_guid, looped);
    report = PropagatePrefabChange(world, guids, *p.library, coin_guid);
    AETHER_CHECK(report.failed == 3);
    AETHER_CHECK(report.results[0].second.error.find("cycle") != std::string::npos);
}

AETHER_TEST(PrefabLibrary_ReportsScenesABreakingChangeAffects) {
    Project p("aether_test_prefab_scenes");
    PrefabData coin;
    coin.entities = {Entity1(1, 0, 1)};
    const assets::AssetGuid coin_guid = p.Write("coin.aprefab", coin);
    PrefabData chest; // a variant of the coin
    chest.base = coin_guid;
    const assets::AssetGuid chest_guid = p.Write("chest.aprefab", chest);

    // Two saved scenes: one with an overriding chest instance, one unrelated.
    {
        World world;
        GuidIndex guids;
        Entity chest_i = InstantiatePrefab(world, guids, chest_guid, *p.library->Flattened(chest_guid));
        SetOverride(*world.GetComponent<PrefabInstance>(chest_i), 1, "Purse", "gold", 9);
        AETHER_CHECK(SaveSceneJson(world, (p.dir / "Content/level.ascene").string()));
        World empty;
        empty.CreateEntity(IdComponent{NewEntityGuid()});
        AETHER_CHECK(SaveScene(empty, (p.dir / "Content/menu.aesc").string()));
    }
    p.db->Scan();

    // Harmless change: the scene is listed, with nothing broken.
    coin.entities[0].components["Purse"]["gold"] = 2;
    p.library->Save(coin_guid, coin);
    std::vector<SceneImpact> impacts = CheckScenesUsingPrefab(*p.library, coin_guid);
    AETHER_CHECK(impacts.size() == 1 && impacts[0].scene == "level.ascene" && impacts[0].instances == 1);
    AETHER_CHECK(impacts[0].orphaned.empty() && impacts[0].errors.empty());

    // Breaking change: the override in that scene is reported.
    coin.entities[0].components.erase("Purse");
    p.library->Save(coin_guid, coin);
    impacts = CheckScenesUsingPrefab(*p.library, coin_guid);
    AETHER_CHECK(impacts.size() == 1 && impacts[0].orphaned.size() == 1 && impacts[0].orphaned[0].field_path == "gold");
    AETHER_CHECK(CheckScenesUsingPrefab(*p.library, chest_guid).size() == 1);
    // The scene file itself wasn't touched.
    World reloaded;
    AETHER_CHECK(LoadSceneJson(reloaded, (p.dir / "Content/level.ascene").string()));
}

AETHER_TEST(PrefabLibrary_FileEditsReachInstancesThroughHotReload) {
    Project p("aether_test_prefab_hot_reload");
    PrefabData coin;
    coin.entities = {Entity1(1, 0, 1)};
    const assets::AssetGuid coin_guid = p.Write("coin.aprefab", coin);
    World world;
    GuidIndex guids;
    Entity instance = InstantiatePrefab(world, guids, coin_guid, *p.library->Flattened(coin_guid));

    assets::ImporterRegistry importers = assets::ImporterRegistry::WithBuiltins();
    assets::DerivedDataCache cache(p.dir / "Intermediate/DDC");
    assets::HotReloader reloader(*p.db, importers, cache, 200ms);
    auto t = assets::FileWatcher::Clock::now();
    AETHER_CHECK(reloader.Update(t).empty());

    // Edited outside the editor (a text editor, a version-control update).
    coin.entities[0].components["Purse"]["gold"] = 77;
    SavePrefab(coin, p.dir / "Content/coin.aprefab");
    stdfs::last_write_time(p.dir / "Content/coin.aprefab", stdfs::file_time_type::clock::now() + 10s);
    reloader.Update(t += 10ms);
    std::vector<assets::AssetChange> changes = reloader.Update(t += 300ms);
    AETHER_CHECK(changes.size() == 1 && changes[0].kind == assets::AssetChange::Kind::Changed);
    AETHER_CHECK(changes.size() == 1 && changes[0].guid == coin_guid);
    // What the editor does with it:
    p.library->Reload(coin_guid);
    PropagatePrefabChange(world, guids, *p.library, coin_guid);
    AETHER_CHECK(world.GetComponent<Purse>(instance)->gold == 77);
    // Nothing more to report until the next edit.
    AETHER_CHECK(reloader.Update(t += 500ms).empty());
}

AETHER_TEST(PrefabLibrary_OverridesMigrateWithTheirComponent) {
    (void)kWalletMigration;
    (void)GetComponentId<Wallet>();
    // A prefab saved when Wallet was version 1 (with `gold`), and an
    // instance override written back then.
    PrefabData old_prefab;
    PrefabEntity wallet;
    wallet.id = 1;
    wallet.components["Wallet"] = {{"$v", 1}, {"gold", 3}, {"cards", 1}};
    old_prefab.entities = {wallet};

    World world;
    GuidIndex guids;
    Entity root = InstantiatePrefab(world, guids, assets::NewAssetGuid(), old_prefab);
    AETHER_CHECK(world.GetComponent<Wallet>(root)->coins == 3); // the data migrates on load
    world.GetComponent<PrefabInstance>(root)->overrides = {{1, "Wallet", "gold", "7", 1}};
    ResolveReport report = ResolvePrefabInstance(world, guids, root, old_prefab);
    AETHER_CHECK(report.ok && report.orphaned.empty() && world.GetComponent<Wallet>(root)->coins == 7);
    // ...and so does the override, in place (it's saved with the scene).
    const PropertyOverride& migrated = world.GetComponent<PrefabInstance>(root)->overrides[0];
    AETHER_CHECK(migrated.field_path == "coins" && migrated.value == "7" && migrated.version == 2);

    // The prefab re-saved in the new version still resolves the same way.
    PrefabData new_prefab = old_prefab;
    new_prefab.entities[0].components["Wallet"] = {{"$v", 2}, {"coins", 3}, {"cards", 1}};
    AETHER_CHECK(ResolvePrefabInstance(world, guids, root, new_prefab).orphaned.empty());
    AETHER_CHECK(world.GetComponent<Wallet>(root)->coins == 7);

    // Recording against the old prefab data compares in the new version: an
    // untouched field records nothing, an edited one records its new name.
    world.GetComponent<PrefabInstance>(root)->overrides.clear();
    ResolvePrefabInstance(world, guids, root, old_prefab);
    world.GetComponent<Wallet>(root)->cards = 4;
    AETHER_CHECK(RecordPrefabOverrides(world, guids, root, GetComponentId<Wallet>(), old_prefab));
    const std::vector<PropertyOverride>& recorded = world.GetComponent<PrefabInstance>(root)->overrides;
    AETHER_CHECK(recorded.size() == 1 && recorded[0].field_path == "cards" && recorded[0].version == 2);

    // Whole-component overrides migrate too; ones the migration drops are
    // left as they were (and show up as orphaned).
    PropertyOverride whole{1, "Wallet", "", R"({"gold": 11, "cards": 0})", 1};
    AETHER_CHECK(MigrateOverride(whole) && Json::parse(whole.value)["coins"] == 11 && whole.version == 2);
    PropertyOverride unknown{1, "Wallet", "stamps", "1", 1};
    AETHER_CHECK(MigrateOverride(unknown) && unknown.field_path == "stamps"); // untouched name, moved to v2
    PropertyOverride current{1, "Wallet", "coins", "1", 2};
    AETHER_CHECK(!MigrateOverride(current));
    PropertyOverride unversioned{1, "Wallet", "gold", "1", 0};
    AETHER_CHECK(!MigrateOverride(unversioned)); // unknown version: taken as current

    // Apply into an old prefab upgrades both.
    PrefabInstance instance;
    instance.overrides = {{1, "Wallet", "gold", "20", 1}};
    PrefabData to_upgrade = old_prefab;
    AETHER_CHECK(ApplyOverridesToPrefab(to_upgrade, instance) == 1);
    const Json& saved = to_upgrade.entities[0].components["Wallet"];
    AETHER_CHECK(saved["$v"] == 2 && saved["coins"] == 20 && !saved.contains("gold"));

    // MigrateJson itself.
    Json data = {{"$v", 1}, {"gold", 5}};
    AETHER_CHECK(reflect::MigrateJson(reflect::Reflect<Wallet>(), data) && data["coins"] == 5 && data["$v"] == 2);
    AETHER_CHECK(!reflect::MigrateJson(reflect::Reflect<Wallet>(), data)); // already current
}
