#include "aether/scene/hierarchy.h"
#include "aether/scene/prefab.h"
#include "aether/scene/serialization.h"
#include "test_framework.h"

#include <filesystem>

using namespace aether;
namespace stdfs = std::filesystem;
using Json = nlohmann::json;

namespace prefab_test {
struct Loot {
    i32 gold = 0;
    std::vector<std::string> items;
};
} // namespace prefab_test

AETHER_REFLECT(prefab_test::Loot, 1, AETHER_FIELD(gold), AETHER_FIELD(items))

namespace {

using prefab_test::Loot;

struct Fixture {
    World world;
    GuidIndex guids;

    Entity Make(Vec3 position, Entity parent = kNullEntity) {
        Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, Transform{position, Quaternion::Identity()});
        guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
        if (!parent.IsNull()) {
            world.AddComponent<Parent>(e, Parent{world.GetComponent<IdComponent>(parent)->guid});
        }
        return e;
    }
};

// A chest: root (1) with a lid (2, which has a hinge child 3) and loot (4).
PrefabData MakeChest() {
    Fixture f;
    Entity root = f.Make(Vec3{0, 0, 0});
    Entity lid = f.Make(Vec3{0, 1, 0}, root);
    f.Make(Vec3{0, 1, -0.5f}, lid);
    Entity loot = f.Make(Vec3{0, 0.2f, 0}, root);
    f.world.AddComponent<Loot>(loot, Loot{10, {"coin", "gem"}});
    return MakePrefab(f.world, f.guids, root);
}

Entity Linked(World& world, Entity root, PrefabLocalId local_id) {
    const EntityGuid instance = world.GetComponent<IdComponent>(root)->guid;
    Entity found = kNullEntity;
    // Scan every entity with a PrefabLink.
    std::vector<Entity> candidates;
    world.ForEachArchetype([&](const Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<PrefabLink>())) {
            return;
        }
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* entities = archetype.EntityArray(c);
            candidates.insert(candidates.end(), entities, entities + archetype.ChunkEntityCount(c));
        }
    });
    for (Entity e : candidates) {
        const PrefabLink* link = world.GetComponent<PrefabLink>(e);
        if (link->instance == instance && link->local_id == local_id) {
            found = e;
        }
    }
    return found;
}

usize CountLinked(World& world) {
    usize count = 0;
    world.ForEach<PrefabLink>([&](const PrefabLink&) { ++count; });
    return count;
}

} // namespace

AETHER_TEST(Prefab_CaptureAndFileRoundTrip) {
    PrefabData chest = MakeChest();
    AETHER_CHECK(chest.entities.size() == 4);
    AETHER_CHECK(chest.entities[0].id == 1 && chest.entities[0].parent == 0 && chest.Root() == 1);
    AETHER_CHECK(chest.entities[1].parent == 1 && chest.entities[2].parent == 2 && chest.entities[3].parent == 1);
    // Identity and hierarchy aren't stored as components.
    AETHER_CHECK(!chest.entities[1].components.contains("IdComponent") && !chest.entities[1].components.contains("Parent"));
    AETHER_CHECK(chest.entities[3].components["Loot"]["gold"] == 10);

    const stdfs::path file = stdfs::temp_directory_path() / "aether_test_chest.aprefab";
    std::string error;
    AETHER_CHECK(SavePrefab(chest, file, &error));
    PrefabData loaded;
    AETHER_CHECK(LoadPrefab(file, loaded, &error));
    AETHER_CHECK(PrefabToJson(loaded) == PrefabToJson(chest));
    stdfs::remove(file);

    // Malformed prefabs are refused with a reason.
    PrefabData ignored;
    AETHER_CHECK(!PrefabFromJson(Json{{"$type", "Scene"}}, ignored, &error));
    Json two_roots = PrefabToJson(chest);
    two_roots["entities"][1]["parent"] = 0;
    AETHER_CHECK(!PrefabFromJson(two_roots, ignored, &error) && !error.empty());
    Json child_first = PrefabToJson(chest);
    child_first["entities"][1]["parent"] = 3; // entity 3 comes later
    AETHER_CHECK(!PrefabFromJson(child_first, ignored, &error));
    Json duplicate = PrefabToJson(chest);
    duplicate["entities"][2]["id"] = 2;
    AETHER_CHECK(!PrefabFromJson(duplicate, ignored, &error));
    AETHER_CHECK(!LoadPrefab("does/not/exist.aprefab", ignored, &error));
}

AETHER_TEST(Prefab_InstantiateBuildsTheHierarchy) {
    PrefabData chest = MakeChest();
    Fixture f;
    const assets::AssetGuid source = assets::NewAssetGuid();
    Transform placed{Vec3{5, 0, 5}, Quaternion::Identity()};
    ResolveReport report;
    Entity root = InstantiatePrefab(f.world, f.guids, source, chest, &placed, &report);
    AETHER_CHECK(report.ok && report.created == 3 && report.updated == 1 && report.orphaned.empty());
    AETHER_CHECK(f.world.EntityCount() == 4 && CountLinked(f.world) == 4);
    AETHER_CHECK(f.world.GetComponent<PrefabInstance>(root)->source.guid == source);
    AETHER_CHECK(f.world.GetComponent<Transform>(root)->position.x == 5.0f); // placed, not the prefab's origin

    Entity lid = Linked(f.world, root, 2);
    Entity hinge = Linked(f.world, root, 3);
    Entity loot = Linked(f.world, root, 4);
    AETHER_CHECK(GetParent(f.world, f.guids, lid) == root && GetParent(f.world, f.guids, hinge) == lid);
    AETHER_CHECK(f.world.GetComponent<Loot>(loot)->items.size() == 2);
    AETHER_CHECK(WorldPosition(f.world, f.guids, hinge).x == 5.0f); // follows the placed root

    // Without a placement, the prefab root's own Transform is used.
    Entity second = InstantiatePrefab(f.world, f.guids, source, chest);
    AETHER_CHECK(f.world.GetComponent<Transform>(second)->position.x == 0.0f);
    AETHER_CHECK(Linked(f.world, second, 2) != lid); // separate entities per instance
    AETHER_CHECK(f.world.EntityCount() == 8);
}

AETHER_TEST(Prefab_OneOverrideAmongAHundredInstances) {
    PrefabData chest = MakeChest();
    Fixture f;
    const assets::AssetGuid source = assets::NewAssetGuid();
    std::vector<Entity> roots;
    for (int i = 0; i < 100; ++i) {
        roots.push_back(InstantiatePrefab(f.world, f.guids, source, chest));
    }
    // Instance 42 has more gold.
    PrefabInstance* special = f.world.GetComponent<PrefabInstance>(roots[42]);
    SetOverride(*special, 4, "Loot", "gold", 99);
    SetOverride(*special, 4, "Loot", "gold", 500); // replaces, doesn't add
    AETHER_CHECK(special->overrides.size() == 1);
    AETHER_CHECK(ResolvePrefabInstance(f.world, f.guids, roots[42], chest).ok);
    const Entity special_loot = Linked(f.world, roots[42], 4);
    AETHER_CHECK(f.world.GetComponent<Loot>(special_loot)->gold == 500);

    // The prefab changes the same field, and an item: 99 follow, 1 keeps its gold.
    chest.Find(4)->components["Loot"]["gold"] = 20;
    chest.Find(4)->components["Loot"]["items"].push_back("key");
    auto reports = ResolveAllPrefabInstances(f.world, f.guids, [&](const assets::AssetGuid& guid) {
        return guid == source ? &chest : nullptr;
    });
    AETHER_CHECK(reports.size() == 100);
    usize followed = 0;
    for (usize i = 0; i < roots.size(); ++i) {
        AETHER_CHECK(reports[i].second.ok && reports[i].second.created == 0 && reports[i].second.destroyed == 0);
        const Loot* loot = f.world.GetComponent<Loot>(Linked(f.world, roots[i], 4));
        followed += loot->gold == 20 ? 1 : 0;
        AETHER_CHECK(loot->items.size() == 3);
    }
    AETHER_CHECK(followed == 99 && f.world.GetComponent<Loot>(special_loot)->gold == 500);
    AETHER_CHECK(Linked(f.world, roots[42], 4) == special_loot); // same entity: references stay valid

    // Removing the override brings it back in line.
    AETHER_CHECK(RemoveOverride(*f.world.GetComponent<PrefabInstance>(roots[42]), 4, "Loot", "gold"));
    AETHER_CHECK(!RemoveOverride(*f.world.GetComponent<PrefabInstance>(roots[42]), 4, "Loot", "gold"));
    ResolvePrefabInstance(f.world, f.guids, roots[42], chest);
    AETHER_CHECK(f.world.GetComponent<Loot>(special_loot)->gold == 20);
}

AETHER_TEST(Prefab_OverridePathsAndOrphans) {
    PrefabData chest = MakeChest();
    Fixture f;
    Entity root = InstantiatePrefab(f.world, f.guids, assets::NewAssetGuid(), chest);
    PrefabInstance& instance = *f.world.GetComponent<PrefabInstance>(root);
    SetOverride(instance, 2, "Transform", "position[1]", 3.5);         // a vector element
    SetOverride(instance, 4, "Loot", "items[1]", "ruby");  // an array element
    SetOverride(instance, 4, "Loot", "missing_field", 1);  // orphaned: no such field
    SetOverride(instance, 9, "Transform", "position", Json::array({0, 0, 0})); // orphaned: no such entity
    SetOverride(instance, 3, "Loot", "gold", 1);           // orphaned: no such component there
    SetOverride(instance, 4, "Loot", "items[7]", "x");     // orphaned: out of range
    ResolveReport report = ResolvePrefabInstance(f.world, f.guids, root, chest);
    AETHER_CHECK(report.ok && report.orphaned.size() == 4);
    AETHER_CHECK(f.world.GetComponent<Transform>(Linked(f.world, root, 2))->position.y == 3.5f);
    AETHER_CHECK(f.world.GetComponent<Loot>(Linked(f.world, root, 4))->items[1] == "ruby");
    // Orphans are kept on the instance, not silently deleted.
    AETHER_CHECK(f.world.GetComponent<PrefabInstance>(root)->overrides.size() == 6);

    // A whole-component override.
    // A whole-component override is applied first, so the field override
    // above ("items[1]" = "ruby") still wins over it.
    SetOverride(*f.world.GetComponent<PrefabInstance>(root), 4, "Loot", "",
                Json{{"gold", 7}, {"items", Json::array({"sword", "shield"})}});
    ResolvePrefabInstance(f.world, f.guids, root, chest);
    const Loot* loot = f.world.GetComponent<Loot>(Linked(f.world, root, 4));
    AETHER_CHECK(loot->gold == 7 && loot->items.size() == 2 && loot->items[0] == "sword" && loot->items[1] == "ruby");
}

AETHER_TEST(Prefab_RemovedAndAddedChildrenAndPrefabEdits) {
    PrefabData chest = MakeChest();
    Fixture f;
    const assets::AssetGuid source = assets::NewAssetGuid();
    Entity root = InstantiatePrefab(f.world, f.guids, source, chest);
    // This instance deletes the lid (and so its hinge), and gets an extra
    // child added by hand under the loot.
    f.world.GetComponent<PrefabInstance>(root)->removed_entities = {2};
    Entity extra = f.Make(Vec3{0, 0, 1}, Linked(f.world, root, 4));
    ResolveReport report = ResolvePrefabInstance(f.world, f.guids, root, chest);
    AETHER_CHECK(report.ok && report.destroyed == 2 && Linked(f.world, root, 2).IsNull());
    AETHER_CHECK(Linked(f.world, root, 3).IsNull() && f.world.IsAlive(extra));

    // The root keeps its placement across re-resolves.
    f.world.GetComponent<Transform>(root)->position = Vec3{1, 2, 3};

    // Both survive a scene save and load (binary and JSON).
    const EntityGuid extra_guid = f.world.GetComponent<IdComponent>(extra)->guid;
    const std::vector<u8> bytes = SaveSceneToMemory(f.world);
    const stdfs::path json_file = stdfs::temp_directory_path() / "aether_test_prefab_scene.ascene";
    AETHER_CHECK(SaveSceneJson(f.world, json_file.string()));
    for (int pass = 0; pass < 2; ++pass) {
        World world;
        GuidIndex guids;
        AETHER_CHECK(pass == 0 ? LoadSceneFromMemory(world, bytes) : LoadSceneJson(world, json_file.string()));
        guids.Rebuild(world);
        // Edit the prefab meanwhile: a new child, a changed field, the hinge dropped.
        PrefabData edited = chest;
        PrefabEntity lock;
        lock.id = 5;
        lock.parent = 1;
        lock.components["Transform"] = chest.Find(2)->components["Transform"];
        edited.entities.push_back(lock);
        edited.Find(4)->components["Loot"]["gold"] = 30;
        auto reports = ResolveAllPrefabInstances(world, guids, [&](const assets::AssetGuid& guid) {
            return guid == source ? &edited : nullptr;
        });
        AETHER_CHECK(reports.size() == 1 && reports[0].second.ok);
        AETHER_CHECK(reports[0].second.created == 1 && reports[0].second.destroyed == 0); // the lock
        Entity loaded_root = reports[0].first;
        AETHER_CHECK(Linked(world, loaded_root, 2).IsNull() && !Linked(world, loaded_root, 5).IsNull());
        AETHER_CHECK(world.GetComponent<Loot>(Linked(world, loaded_root, 4))->gold == 30);
        AETHER_CHECK(world.GetComponent<Transform>(loaded_root)->position.y == 2.0f);
        AETHER_CHECK(!guids.Find(world, extra_guid).IsNull()); // the hand-added child is still there
        AETHER_CHECK(world.EntityCount() == 4); // root, loot, lock and the extra child: nothing stale

        // Deleting the loot from the prefab removes it from instances; a
        // component dropped from the prefab is dropped too.
        edited.entities.erase(edited.entities.begin() + 3);
        edited.Find(5)->components.erase("Transform");
        ResolveReport after = ResolvePrefabInstance(world, guids, loaded_root, edited);
        AETHER_CHECK(after.ok && after.destroyed == 1 && Linked(world, loaded_root, 4).IsNull());
        AETHER_CHECK(!world.HasComponent<Transform>(Linked(world, loaded_root, 5)));

        // A prefab that isn't available is reported, and the instance left alone.
        auto missing = ResolveAllPrefabInstances(world, guids, [](const assets::AssetGuid&) { return nullptr; });
        AETHER_CHECK(missing.size() == 1 && !missing[0].second.ok && !missing[0].second.error.empty());
    }
    stdfs::remove(json_file);
}

AETHER_TEST(Prefab_EditsBecomeOverridesAndRevertByHandRemovesThem) {
    PrefabData chest = MakeChest();
    Fixture f;
    Entity root = InstantiatePrefab(f.world, f.guids, assets::NewAssetGuid(), chest);
    const Entity loot = Linked(f.world, root, 4);
    const Entity lid = Linked(f.world, root, 2);
    const ComponentId loot_id = GetComponentId<Loot>();
    const ComponentId transform_id = GetComponentId<Transform>();
    auto overrides = [&] { return f.world.GetComponent<PrefabInstance>(root)->overrides; };

    f.world.GetComponent<Loot>(loot)->gold = 50;
    AETHER_CHECK(RecordPrefabOverrides(f.world, f.guids, loot, loot_id, chest));
    AETHER_CHECK(overrides().size() == 1 && overrides()[0].field_path == "gold" && overrides()[0].value == "50");
    const PrefabInstance& instance = *f.world.GetComponent<PrefabInstance>(root);
    AETHER_CHECK(IsFieldOverridden(instance, 4, "Loot", "gold") && !IsFieldOverridden(instance, 4, "Loot", "items"));
    AETHER_CHECK(!IsFieldOverridden(instance, 2, "Loot", "gold"));

    // Setting it back by hand removes the override.
    f.world.GetComponent<Loot>(loot)->gold = 10;
    AETHER_CHECK(RecordPrefabOverrides(f.world, f.guids, loot, loot_id, chest) && overrides().empty());

    // One vector element; a resized array is overridden as a whole.
    f.world.GetComponent<Transform>(lid)->position.y = 4.0f;
    RecordPrefabOverrides(f.world, f.guids, lid, transform_id, chest);
    AETHER_CHECK(overrides().size() == 1 && overrides()[0].field_path == "position[1]");
    AETHER_CHECK(IsFieldOverridden(*f.world.GetComponent<PrefabInstance>(root), 2, "Transform", "position"));
    f.world.GetComponent<Loot>(loot)->items.push_back("key");
    RecordPrefabOverrides(f.world, f.guids, loot, loot_id, chest);
    AETHER_CHECK(overrides().size() == 2 && overrides()[1].field_path == "items");

    // Moving the root is placing the instance, not an override; entities
    // outside an instance record nothing.
    f.world.GetComponent<Transform>(root)->position.x = 9.0f;
    AETHER_CHECK(RecordPrefabOverrides(f.world, f.guids, root, transform_id, chest) && overrides().size() == 2);
    Entity loose = f.Make(Vec3{0, 0, 0});
    AETHER_CHECK(!RecordPrefabOverrides(f.world, f.guids, loose, transform_id, chest));
    AETHER_CHECK(FindInstanceRoot(f.world, f.guids, lid) == root && FindInstanceRoot(f.world, f.guids, loose).IsNull());

    // An orphaned override on the component survives re-recording.
    SetOverride(*f.world.GetComponent<PrefabInstance>(root), 4, "Loot", "old_field", 1);
    RecordPrefabOverrides(f.world, f.guids, loot, loot_id, chest);
    AETHER_CHECK(overrides().size() == 3);
}

AETHER_TEST(Prefab_ApplyAndRevert) {
    PrefabData chest = MakeChest();
    Fixture f;
    const assets::AssetGuid source = assets::NewAssetGuid();
    Entity a = InstantiatePrefab(f.world, f.guids, source, chest);
    Entity b = InstantiatePrefab(f.world, f.guids, source, chest);
    Entity c = InstantiatePrefab(f.world, f.guids, source, chest);
    SetOverride(*f.world.GetComponent<PrefabInstance>(a), 4, "Loot", "gold", 75);
    SetOverride(*f.world.GetComponent<PrefabInstance>(a), 2, "Transform", "position[1]", 2.0);
    SetOverride(*f.world.GetComponent<PrefabInstance>(a), 4, "Loot", "gone", 1); // orphan
    SetOverride(*f.world.GetComponent<PrefabInstance>(c), 4, "Loot", "gold", 5);

    // Apply just a's gold: the prefab changes, a loses that override.
    AETHER_CHECK(ApplyOverridesToPrefab(chest, *f.world.GetComponent<PrefabInstance>(a), 4, "Loot", "gold") == 1);
    AETHER_CHECK(chest.Find(4)->components["Loot"]["gold"] == 75);
    AETHER_CHECK(f.world.GetComponent<PrefabInstance>(a)->overrides.size() == 2);
    ResolveAllPrefabInstances(f.world, f.guids, [&](const assets::AssetGuid&) { return &chest; });
    AETHER_CHECK(f.world.GetComponent<Loot>(Linked(f.world, b, 4))->gold == 75); // follows the new default
    AETHER_CHECK(f.world.GetComponent<Loot>(Linked(f.world, c, 4))->gold == 5);  // keeps its own
    AETHER_CHECK(f.world.GetComponent<Loot>(Linked(f.world, a, 4))->gold == 75);

    // Apply everything: the orphan can't be applied and stays.
    AETHER_CHECK(ApplyOverridesToPrefab(chest, *f.world.GetComponent<PrefabInstance>(a)) == 1);
    AETHER_CHECK(chest.Find(2)->components["Transform"]["position"][1] == 2.0);
    AETHER_CHECK(f.world.GetComponent<PrefabInstance>(a)->overrides.size() == 1);

    // Revert: c's gold goes back to the prefab's.
    AETHER_CHECK(RevertOverrides(*f.world.GetComponent<PrefabInstance>(c), 4, "Loot") == 1);
    ResolvePrefabInstance(f.world, f.guids, c, chest);
    AETHER_CHECK(f.world.GetComponent<Loot>(Linked(f.world, c, 4))->gold == 75);
    AETHER_CHECK(RevertOverrides(*f.world.GetComponent<PrefabInstance>(a)) == 1); // everything, orphan included
    AETHER_CHECK(RevertOverrides(*f.world.GetComponent<PrefabInstance>(a)) == 0);

    // Path matching: a path covers what's inside it, not its siblings.
    PropertyOverride o{2, "Transform", "position[1]", "0"};
    AETHER_CHECK(OverrideMatches(o, 2, "Transform", "position") && OverrideMatches(o, 0, "", ""));
    AETHER_CHECK(!OverrideMatches(o, 2, "Transform", "pos") && !OverrideMatches(o, 2, "Transform", "position[10]"));
    AETHER_CHECK(!OverrideMatches(o, 3, "Transform", "") && !OverrideMatches(o, 2, "Loot", ""));
}
