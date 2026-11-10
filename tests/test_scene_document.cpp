#include "core/scene_document.h"
#include "aether/scene/data_layers.h"
#include "aether/scene/level_instance.h"
#include "core/snapping.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/serialization.h"
#include "test_framework.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::editor;

AETHER_TEST(SceneDocument_SelectionEditsUndoAndRoundTrips) {
    SceneDocument document;
    document.NewScene();
    AETHER_CHECK(document.Entities().size() == 5);

    Entity player = kNullEntity;
    for (Entity entity : document.Entities())
        if (document.Name(entity) == "Player") player = entity;
    AETHER_CHECK(!player.IsNull());

    const Vec3 original = document.GetWorld().GetComponent<Transform>(player)->position;
    const Vec3 moved{4.0f, 2.0f, -3.0f};
    AETHER_CHECK(document.SetPosition(player, Vec3{1.0f, 1.5f, -5.0f}, false));
    AETHER_CHECK(document.SetPosition(player, moved, false));
    AETHER_CHECK(document.SetPosition(player, moved, true));
    AETHER_CHECK(document.IsDirty());
    AETHER_CHECK(document.GetWorld().GetComponent<Transform>(player)->position.x == moved.x);
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(document.GetWorld().GetComponent<Transform>(player)->position.x == original.x);
    AETHER_CHECK(document.Redo());
    AETHER_CHECK(document.GetWorld().GetComponent<Transform>(player)->position.x == moved.x);

    AETHER_CHECK(document.Rename(player, "Hero"));
    AETHER_CHECK(document.Name(player) == "Hero");
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(document.Name(player) == "Player");
    AETHER_CHECK(document.Redo());
    AETHER_CHECK(document.Name(player) == "Hero");

    AETHER_CHECK(document.DestroyEntity(player));
    AETHER_CHECK(document.Entities().size() == 4);
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(document.Entities().size() == 5);
    Entity restored = kNullEntity;
    for (Entity entity : document.Entities())
        if (document.Name(entity) == "Hero") restored = entity;
    AETHER_CHECK(!restored.IsNull());

    const auto path = std::filesystem::temp_directory_path() / "aether_qt_scene_document_test.ascene";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    AETHER_CHECK(document.Save(path));
    AETHER_CHECK(!document.IsDirty());
    SceneDocument loaded;
    AETHER_CHECK(loaded.Load(path));
    AETHER_CHECK(loaded.Entities().size() == 5);
    bool found_hero = false;
    for (Entity entity : loaded.Entities()) {
        if (loaded.Name(entity) == "Hero") {
            found_hero = true;
            AETHER_CHECK(loaded.GetWorld().GetComponent<Transform>(entity)->position.x == moved.x);
        }
    }
    AETHER_CHECK(found_hero);
    std::filesystem::remove(path, ec);

    const auto binary_path = std::filesystem::temp_directory_path() / "aether_qt_scene_document_test.aesc";
    std::filesystem::remove(binary_path, ec);
    AETHER_CHECK(document.Save(binary_path));
    SceneDocument binary_loaded;
    AETHER_CHECK(binary_loaded.Load(binary_path));
    AETHER_CHECK(binary_loaded.Entities().size() == 5);
    found_hero = false;
    for (Entity entity : binary_loaded.Entities()) {
        if (binary_loaded.Name(entity) == "Hero") {
            found_hero = true;
            AETHER_CHECK(binary_loaded.GetWorld().GetComponent<Transform>(entity)->position.x == moved.x);
        }
    }
    AETHER_CHECK(found_hero);
    std::filesystem::remove(binary_path, ec);

    Entity playable_hero = kNullEntity;
    for (Entity entity : document.Entities())
        if (document.Name(entity) == "Hero") playable_hero = entity;
    AETHER_CHECK(!playable_hero.IsNull());
    const EntityGuid playable_guid = document.GetWorld().GetComponent<IdComponent>(playable_hero)->guid;
    document.Play();
    AETHER_CHECK(document.PlayState() == PlaySession::State::Playing);
    document.GetWorld().GetComponent<Transform>(playable_hero)->position = Vec3{99.0f, 99.0f, 99.0f};
    document.Pause();
    AETHER_CHECK(document.PlayState() == PlaySession::State::Paused);
    document.Stop();
    AETHER_CHECK(document.PlayState() == PlaySession::State::Editing);
    playable_hero = document.Guids().Find(document.GetWorld(), playable_guid);
    AETHER_CHECK(!playable_hero.IsNull());
    AETHER_CHECK(document.GetWorld().GetComponent<Transform>(playable_hero)->position.x == moved.x);
}

AETHER_TEST(SceneDocument_MapEntitiesDuplicateWithComponentsAndUndo) {
    SceneDocument document;
    document.NewScene();

    const Entity camera = document.CreateEntity("Gameplay Camera", SceneEntityKind::Camera);
    AETHER_CHECK(document.GetWorld().HasComponent<Camera>(camera));
    AETHER_CHECK(!document.GetWorld().HasComponent<ModelRenderer>(camera));

    const Entity model = document.CreateEntity("Castle", SceneEntityKind::Model, "Models/castle.glb");
    const auto* renderer = document.GetWorld().GetComponent<ModelRenderer>(model);
    AETHER_CHECK(renderer != nullptr);
    AETHER_CHECK(std::string(renderer->asset_path) == "Models/castle.glb");
    AETHER_CHECK(document.SetPosition(model, Vec3{2.0f, 0.0f, 3.0f}, true));

    const Entity duplicate = document.DuplicateEntity(model, Vec3{1.0f, 0.0f, -1.0f});
    AETHER_CHECK(!duplicate.IsNull());
    AETHER_CHECK(document.Name(duplicate) == "Castle Copy");
    AETHER_CHECK(document.GetWorld().HasComponent<ModelRenderer>(duplicate));
    const auto* duplicate_transform = document.GetWorld().GetComponent<Transform>(duplicate);
    AETHER_CHECK(duplicate_transform != nullptr);
    AETHER_CHECK(duplicate_transform->position.x == 3.0f && duplicate_transform->position.z == 2.0f);
    const EntityGuid original_guid = document.GetWorld().GetComponent<IdComponent>(model)->guid;
    const EntityGuid duplicate_guid = document.GetWorld().GetComponent<IdComponent>(duplicate)->guid;
    AETHER_CHECK(original_guid != duplicate_guid);

    AETHER_CHECK(document.Undo());
    AETHER_CHECK(document.Guids().Find(document.GetWorld(), duplicate_guid).IsNull());
    AETHER_CHECK(!document.Guids().Find(document.GetWorld(), original_guid).IsNull());
    AETHER_CHECK(document.Redo());
    AETHER_CHECK(!document.Guids().Find(document.GetWorld(), duplicate_guid).IsNull());

    const Entity group = document.CreateEntity("Castle Group");
    AETHER_CHECK(document.ReparentEntity(model, group));
    AETHER_CHECK(document.ParentOf(model) == group);
    AETHER_CHECK(!document.ReparentEntity(group, model));
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(document.ParentOf(model).IsNull());
    AETHER_CHECK(document.Redo());
    AETHER_CHECK(document.ParentOf(model) == group);
    AETHER_CHECK(document.ReparentEntity(model));
    AETHER_CHECK(document.ParentOf(model).IsNull());
}

AETHER_TEST(SceneDocument_LegacyScenesGetReadableUniqueEditorNames) {
    World legacy;
    legacy.CreateEntity(Transform{}, Tags{{"Environment", "Greybox", "CryoPod"}});
    legacy.CreateEntity(Transform{}, Tags{{"Environment", "Greybox", "CryoPod"}});
    legacy.CreateEntity(Transform{}, Tags{{"MainCamera"}}, Camera{});
    ModelRenderer renderer;
    SetModelPath(renderer, "models/aether_greybox_castle_gate.glb");
    legacy.CreateEntity(Transform{}, renderer);

    const auto path = std::filesystem::temp_directory_path() / "aether_qt_legacy_names.ascene";
    std::error_code error;
    std::filesystem::remove(path, error);
    AETHER_CHECK(SaveSceneJson(legacy, path.string()));

    SceneDocument document;
    AETHER_CHECK(document.Load(path));
    std::vector<std::string> names;
    for (Entity entity : document.Entities()) names.push_back(document.Name(entity));
    AETHER_CHECK(names == (std::vector<std::string>{"Cryo Pod", "Cryo Pod 2", "Main Camera", "Castle Gate"}));
    AETHER_CHECK(document.Rename(document.Entities().front(), "Player Pod"));
    AETHER_CHECK(document.Name(document.Entities().front()) == "Player Pod");
    std::filesystem::remove(path, error);
}

AETHER_TEST(SceneDocument_RejectsUnknownComponentsWithoutReplacingCurrentScene) {
    const auto path = std::filesystem::temp_directory_path() / "aether_scene_unknown_component.ascene";
    {
        std::ofstream file(path, std::ios::binary);
        file << R"({"$type":"Scene","$version":1,"entities":[{"components":{"MissingPluginComponent":{"value":1}}}]})";
        AETHER_CHECK(file.good());
    }

    SceneDocument document;
    document.NewScene();
    std::string error;
    AETHER_CHECK(!document.Load(path, &error));
    AETHER_CHECK(!error.empty());
    AETHER_CHECK(document.Entities().size() == 5);
    AETHER_CHECK(document.Name(document.Entities().front()) == "Camera");

    std::error_code filesystem_error;
    std::filesystem::remove(path, filesystem_error);
}

// Phase 42.1: a scene document records its origin and reads it back.
AETHER_TEST(SceneDocument_KeepsItsOrigin) {
    SceneDocument document;
    document.NewScene();
    document.CreateEntity("Far");
    document.SetOrigin(Vec3d(100000.0, 0.0, 200000.0));
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "aether_test_doc_origin.ascene";
    AETHER_CHECK(document.Save(path));

    SceneDocument loaded;
    AETHER_CHECK(loaded.Load(path));
    AETHER_CHECK_NEAR(loaded.Origin().x, 100000.0, 1e-6);
    AETHER_CHECK_NEAR(loaded.Origin().z, 200000.0, 1e-6);

    std::error_code filesystem_error;
    std::filesystem::remove(path, filesystem_error);
}

// Phase 43.3: the gizmo's snapping rules, checked without a viewport.
AETHER_TEST(Editor_SnappingRoundsToTheStep) {
    // A zero step is "snapping off", not "snap to zero".
    AETHER_CHECK_NEAR(SnapScalar(1.37f, 0.0f), 1.37f, 1e-5f);
    AETHER_CHECK_NEAR(SnapScalar(1.37f, 0.5f), 1.5f, 1e-5f);
    AETHER_CHECK_NEAR(SnapScalar(-1.37f, 0.5f), -1.5f, 1e-5f);
    AETHER_CHECK_NEAR(SnapScalar(0.24f, 0.5f), 0.0f, 1e-5f);

    const Vec3 moved = SnapTranslation(Vec3(1.1f, -0.6f, 2.9f), 1.0f);
    AETHER_CHECK_NEAR(moved.x, 1.0f, 1e-5f);
    AETHER_CHECK_NEAR(moved.y, -1.0f, 1e-5f);
    AETHER_CHECK_NEAR(moved.z, 3.0f, 1e-5f);

    AETHER_CHECK_NEAR(SnapRotationDegrees(37.0f, 15.0f), 30.0f, 1e-4f);
    AETHER_CHECK_NEAR(SnapRotationDegrees(83.0f, 15.0f), 90.0f, 1e-4f);

    // Scale snaps but never collapses: a drag to almost nothing lands on one step, keeping sign.
    const Vec3 scaled = SnapScale(Vec3(0.1f, -0.1f, 1.9f), 0.5f);
    AETHER_CHECK_NEAR(scaled.x, 0.5f, 1e-5f);
    AETHER_CHECK_NEAR(scaled.y, -0.5f, 1e-5f);
    AETHER_CHECK_NEAR(scaled.z, 2.0f, 1e-5f);
    AETHER_CHECK_NEAR(SnapScaleAxis(0.0f, 0.5f), 0.5f, 1e-5f); // zero keeps the positive step
}
// Phase 43.3: multi-selection, so a gizmo can act on several entities at once.
AETHER_TEST(SceneDocument_MultiSelection) {
    SceneDocument document;
    document.NewScene();
    const std::vector<Entity> entities = document.Entities();
    AETHER_CHECK(entities.size() >= 3);
    AETHER_CHECK(document.Selection().empty());

    // Duplicates collapse, and the caller's order is kept: a gizmo's pivot follows the first.
    document.SetSelection({entities[0], entities[1], entities[0]});
    AETHER_CHECK(document.Selection().size() == 2);
    AETHER_CHECK(document.Selection()[0] == entities[0] && document.Selection()[1] == entities[1]);
    AETHER_CHECK(document.IsSelected(entities[1]) && !document.IsSelected(entities[2]));

    document.ToggleSelection(entities[2]);
    AETHER_CHECK(document.Selection().size() == 3);
    document.ToggleSelection(entities[0]);
    AETHER_CHECK(!document.IsSelected(entities[0]) && document.Selection().size() == 2);

    document.ClearSelection();
    AETHER_CHECK(document.Selection().empty());

    // Opening another scene drops the selection with everything else.
    document.SetSelection({document.Entities()[0]});
    document.NewScene();
    AETHER_CHECK(document.Selection().empty());
}
// Phase 43.3: copy/paste of the selection. The clipboard holds snapshots, so it outlives the
// scene the entities came from.
AETHER_TEST(SceneDocument_CopyAndPasteOutlivesTheScene) {
    SceneDocument document;
    document.NewScene();
    const std::vector<Entity> entities = document.Entities();
    AETHER_CHECK(entities.size() >= 2);

    document.SetSelection({entities[0], entities[1]});
    AETHER_CHECK(document.CopySelection());
    AETHER_CHECK(document.HasClipboard());

    // Paste into the same scene: two new entities, and it is undoable.
    const usize before = document.Entities().size();
    AETHER_CHECK(document.PasteClipboard(Vec3(0.0f, 1.0f, 0.0f)) == 2);
    AETHER_CHECK(document.Entities().size() == before + 2);
    AETHER_CHECK(document.CanUndo());
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(document.Entities().size() == before + 1); // one command per pasted entity
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(document.Entities().size() == before);

    // The clipboard does not hold handles, so replacing the scene does not empty it.
    document.NewScene();
    AETHER_CHECK(document.HasClipboard());
    const usize fresh = document.Entities().size();
    AETHER_CHECK(document.PasteClipboard() == 2);
    AETHER_CHECK(document.Entities().size() == fresh + 2);
}
// Phase 42.6: data layers switch a named group of entities off and back on, restoring what each
// entity had rather than turning everything on.
AETHER_TEST(DataLayers_SwitchAndRestore) {
    World world;
    auto make = [&](const char* layer, bool active) {
        const Entity e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
        if (layer != nullptr) world.AddComponent<DataLayer>(e, DataLayer{layer});
        if (!active) world.AddComponent<Active>(e, Active{false});
        return e;
    };
    const Entity a = make("Props", true);
    const Entity already_off = make("Props", false); // off before the layer is ever switched
    const Entity c = make("Props", true);
    const Entity terrain = make("Terrain", true);
    const Entity loose = make(nullptr, true);

    AETHER_CHECK(DataLayerNames(world).size() == 2);
    AETHER_CHECK(DataLayerSize(world, "Props") == 3 && DataLayerSize(world, "Terrain") == 1);
    AETHER_CHECK(DataLayerSize(world, "Nope") == 0);
    AETHER_CHECK(!IsDataLayerActive(world, "Props")); // already_off
    AETHER_CHECK(IsDataLayerActive(world, "Terrain"));
    AETHER_CHECK(!IsDataLayerActive(world, "Nope")); // an empty layer is not active

    // Switching the layer off takes the two that were on, and leaves the one that was already off.
    AETHER_CHECK(SetDataLayerActive(world, "Props", false) == 2);
    AETHER_CHECK(!IsDataLayerActive(world, "Props"));
    AETHER_CHECK(SetDataLayerActive(world, "Props", false) == 0); // idempotent
    // Nothing outside the layer is touched.
    AETHER_CHECK(IsDataLayerActive(world, "Terrain"));
    AETHER_CHECK(world.GetComponent<Active>(terrain) == nullptr || world.GetComponent<Active>(terrain)->active);
    AETHER_CHECK(world.GetComponent<Active>(loose) == nullptr || world.GetComponent<Active>(loose)->active);


    // Switching it back on restores the two -- and leaves the one that was already off alone.
    AETHER_CHECK(SetDataLayerActive(world, "Props", true) == 2);
    AETHER_CHECK(world.GetComponent<Active>(a) == nullptr || world.GetComponent<Active>(a)->active);
    AETHER_CHECK(world.GetComponent<Active>(c) == nullptr || world.GetComponent<Active>(c)->active);
    AETHER_CHECK(world.GetComponent<Active>(already_off) != nullptr && !world.GetComponent<Active>(already_off)->active);
    AETHER_CHECK(!IsDataLayerActive(world, "Props")); // already_off is still off

    // A layer nobody belongs to changes nothing and reports no changes.
    AETHER_CHECK(SetDataLayerActive(world, "Nope", false) == 0);
    AETHER_CHECK(SetDataLayerActive(world, "Nope", true) == 0);
}
// Phase 42.6: a saved scene keeps its data layers -- without reflection the round trip would drop
// them and every entity would come back in no layer.
AETHER_TEST(DataLayers_SurviveASaveAndLoad) {
    World world;
    const Entity props = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    world.AddComponent<DataLayer>(props, DataLayer{"Props", false, true});

    const std::vector<u8> bytes = SaveSceneToMemory(world);
    AETHER_CHECK(!bytes.empty());

    World loaded;
    AETHER_CHECK(LoadSceneFromMemory(loaded, bytes, "memory"));
    AETHER_CHECK(DataLayerNames(loaded).size() == 1 && DataLayerNames(loaded)[0] == "Props");
    AETHER_CHECK(DataLayerSize(loaded, "Props") == 1);
    AETHER_CHECK(DataLayerSize(loaded, "Terrain") == 0);

    // The remembered state travels with it, not just the name: a layer switched off before the save
    // still restores correctly after the load.
    loaded.ForEachArchetype([](Archetype& archetype) {
        if ((archetype.Mask() & ComponentMaskOf<DataLayer>()) != ComponentMaskOf<DataLayer>()) return;
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            const u32 count = archetype.ChunkEntityCount(chunk);
            auto* layers = static_cast<DataLayer*>(archetype.ComponentArray(chunk, GetComponentId<DataLayer>()));
            for (u32 i = 0; i < count; ++i) {
                AETHER_CHECK(layers[i].layer == "Props");
                AETHER_CHECK(layers[i].captured && !layers[i].active_when_layer_on);
            }
        }
    });
}
// Phase 42.6: level instances -- a scene placed inside another world, once, through the prefab path.
AETHER_TEST(LevelInstance_ExpandsOnceAndReportsFailures) {
    // The fake loader builds real prefab data from a temporary world, so the test exercises the same
    // InstantiatePrefab path the editor will use without needing a scene file on disk.
    auto loader = [](const std::string& scene, PrefabData& out, std::string& error) {
        if (scene != "town") {
            error = "no such scene";
            return false;
        }
        World temp;
        const Entity root = temp.CreateEntity(Transform{Vec3(1.0f, 0.0f, 0.0f), Quaternion::Identity()}, IdComponent{});
        temp.CreateEntity(Transform{Vec3(2.0f, 0.0f, 0.0f), Quaternion::Identity()}, IdComponent{}, Parent{temp.GetComponent<IdComponent>(root)->guid});
        GuidIndex temp_guids;
        temp_guids.Rebuild(temp);
        out = MakePrefab(temp, temp_guids, root);
        return !out.entities.empty();
    };

    World world;
    const Entity placement = world.CreateEntity(Transform{Vec3(10.0f, 0.0f, 0.0f), Quaternion::Identity()}, IdComponent{});
    world.AddComponent<LevelInstance>(placement, LevelInstance{"town"});
    const Entity broken = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, IdComponent{});
    world.AddComponent<LevelInstance>(broken, LevelInstance{"missing"});
    GuidIndex guids;

    const usize before = world.EntityCount();
    LevelInstanceReport report = ExpandLevelInstances(world, guids, loader);
    AETHER_CHECK(report.expanded == 1);
    AETHER_CHECK(report.failed.size() == 1);
    AETHER_CHECK(world.EntityCount() > before); // the scene's entities arrived

    // The placement is marked, so expanding again duplicates nothing.
    AETHER_CHECK(world.GetComponent<LevelInstance>(placement)->expanded);
    AETHER_CHECK(!world.GetComponent<LevelInstance>(broken)->expanded); // it failed; retry is allowed
    const usize after_first = world.EntityCount();
    LevelInstanceReport again = ExpandLevelInstances(world, guids, loader);
    AETHER_CHECK(again.expanded == 0);
    AETHER_CHECK(world.EntityCount() == after_first);

    // No loader means nothing happens rather than a crash.
    AETHER_CHECK(ExpandLevelInstances(world, guids, {}).expanded == 0);
}
// Phase 38.8 follow-up: the report of an instantiation names the entities it made, so an undoable
// command can remove them. World::DestroyEntity does not recurse, so the list is what makes that possible.
AETHER_TEST(Prefab_InstantiationReportsTheEntitiesItCreated) {
    World source;
    GuidIndex source_guids;
    const Entity a = source.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, IdComponent{});
    source.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, IdComponent{},
                        Parent{source.GetComponent<IdComponent>(a)->guid});
    source_guids.Rebuild(source);
    const PrefabData prefab = MakePrefab(source, source_guids, a);
    AETHER_CHECK(!prefab.entities.empty());

    World world;
    GuidIndex guids;
    ResolveReport report;
    const Entity root = InstantiatePrefab(world, guids, assets::AssetGuid{}, prefab, nullptr, &report);
    AETHER_CHECK(!root.IsNull());
    AETHER_CHECK(report.ok);
    // The report counts the entities the resolve made; the root is InstantiatePrefab's own, added on top.
    AETHER_CHECK(report.created_entities.size() == report.created + 1);
    AETHER_CHECK(!report.created_entities.empty());
    bool found_root = false;
    for (const Entity entity : report.created_entities) {
        found_root = found_root || entity == root;
        AETHER_CHECK(world.IsAlive(entity));
    }
    AETHER_CHECK(found_root); // the root is among them, so destroying the list removes everything
}
// Phase 42.6: turning a scene on disk into prefab data, which is what a level instance places. The
// interesting part is that a scene has as many roots as it has, and a prefab has exactly one -- so a
// synthetic root has to stand in for the scene.
AETHER_TEST(LevelInstance_LoadSceneAsPrefabTakesEveryRoot) {
    World source;
    const Entity a = source.CreateEntity(Transform{Vec3(1.0f, 0.0f, 0.0f), Quaternion::Identity()}, IdComponent{NewEntityGuid()});
    const Entity b = source.CreateEntity(Transform{Vec3(2.0f, 0.0f, 0.0f), Quaternion::Identity()}, IdComponent{NewEntityGuid()});
    source.CreateEntity(Transform{Vec3(3.0f, 0.0f, 0.0f), Quaternion::Identity()}, IdComponent{NewEntityGuid()},
                        Parent{source.GetComponent<IdComponent>(b)->guid});
    const auto path = std::filesystem::temp_directory_path() / "aether_level_instance_scene.ascene";
    AETHER_CHECK(SaveScene(source, path.string()));

    PrefabData prefab;
    std::string error;
    AETHER_CHECK(LoadSceneAsPrefab(path.string(), prefab, error));
    AETHER_CHECK(error.empty());
    // The synthetic root, both scene roots, and b's child: a prefab made from only the first root would
    // have two entries, which is the bug this guards.
    AETHER_CHECK(prefab.entities.size() >= 4);

    // A path that is not a scene fails with a reason rather than producing empty prefab data.
    PrefabData missing;
    std::string missing_error;
    AETHER_CHECK(!LoadSceneAsPrefab((std::filesystem::temp_directory_path() / "aether_no_such_scene.ascene").string(),
                                    missing, missing_error));
    AETHER_CHECK(!missing_error.empty());
    AETHER_CHECK(missing.entities.empty());
}
namespace {

// What one entity must look like after a save and reload, keyed by its GUID (an Entity handle does not survive a reload).
struct ExpectedEntity {
    EntityGuid guid;
    std::string name;
    Vec3 position, scale;
    Quaternion rotation;
    EntityGuid parent; // null: a root
};

EntityGuid GuidOf(const SceneDocument& document, Entity entity) {
    if (entity.IsNull() || !document.GetWorld().IsAlive(entity)) return EntityGuid{}; // a root's parent is the null entity
    const IdComponent* id = document.GetWorld().GetComponent<IdComponent>(entity);
    return id ? id->guid : EntityGuid{};
}

bool Close(f32 a, f32 b) { return std::fabs(a - b) < 1e-5f; }

// Every expected entity is present with the same name, transform and parent, and nothing else was added or lost.
void ExpectScene(SceneDocument& document, const std::vector<ExpectedEntity>& expected, usize total, const char* what) {
    AETHER_CHECK(document.Entities().size() == total);
    usize wrong = 0;
    for (const ExpectedEntity& want : expected) {
        const Entity entity = document.Guids().Find(document.GetWorld(), want.guid);
        if (entity.IsNull()) {
            ++wrong;
            continue;
        }
        const Transform* t = document.GetWorld().GetComponent<Transform>(entity);
        const bool ok = t != nullptr && document.Name(entity) == want.name && Close(t->position.x, want.position.x) &&
                        Close(t->position.y, want.position.y) && Close(t->position.z, want.position.z) &&
                        Close(t->scale.x, want.scale.x) && Close(t->scale.y, want.scale.y) && Close(t->scale.z, want.scale.z) &&
                        Close(t->rotation.x, want.rotation.x) && Close(t->rotation.y, want.rotation.y) &&
                        Close(t->rotation.z, want.rotation.z) && Close(t->rotation.w, want.rotation.w) &&
                        GuidOf(document, document.ParentOf(entity)) == want.parent;
        if (!ok) ++wrong;
    }
    if (wrong != 0) std::printf("    %s: %zu of %zu entities differ\n", what, static_cast<size_t>(wrong), expected.size());
    AETHER_CHECK(wrong == 0);
}

std::filesystem::path FreshSceneDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace

// M9.1: a scene of 100 entities in a three-level hierarchy, edited through the document's own commands, keeps every GUID, name, transform
// and parent link across save and reload (JSON and binary), and its edits undo and redo.
AETHER_TEST(SceneDocument_HundredEntitiesKeepIdsHierarchyAndTransformsAcrossSaveReload) {
    SceneDocument document;
    document.NewScene();
    const usize starting = document.Entities().size();

    std::vector<Entity> made;
    std::vector<Entity> groups, children;
    for (int g = 0; g < 10; ++g) groups.push_back(document.CreateEntity("Group " + std::to_string(g)));
    for (int c = 0; c < 80; ++c) children.push_back(document.CreateEntity("Prop " + std::to_string(c), SceneEntityKind::Empty, {}, groups[c % 10]));
    std::vector<Entity> grandchildren;
    for (int g = 0; g < 10; ++g) grandchildren.push_back(document.CreateEntity("Detail " + std::to_string(g), SceneEntityKind::Empty, {}, children[g]));
    made.insert(made.end(), groups.begin(), groups.end());
    made.insert(made.end(), children.begin(), children.end());
    made.insert(made.end(), grandchildren.begin(), grandchildren.end());
    AETHER_CHECK(made.size() == 100 && document.Entities().size() == starting + 100);

    // Distinct transforms, and a rename on some, so a swapped or reset entity cannot go unnoticed.
    for (usize i = 0; i < made.size(); ++i) {
        const f32 f = static_cast<f32>(i);
        AETHER_CHECK(document.SetPosition(made[i], Vec3{f, f * 0.5f + 1.0f, -f * 0.25f}, true));
        AETHER_CHECK(document.SetRotation(made[i], Quaternion::FromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 0.01f * (f + 1.0f)), true));
        AETHER_CHECK(document.SetScale(made[i], Vec3{1.0f + f * 0.01f, 2.0f, 0.5f + f * 0.02f}, true));
        if (i % 7 == 0) AETHER_CHECK(document.Rename(made[i], "Renamed " + std::to_string(i)));
    }
    AETHER_CHECK(document.IsDirty());

    // The GUIDs are unique and non-null, and the hierarchy is what was built.
    std::set<std::string> guids;
    for (Entity e : document.Entities()) guids.insert(ToString(GuidOf(document, e)));
    AETHER_CHECK(guids.size() == document.Entities().size());
    AETHER_CHECK(document.ParentOf(children[13]) == groups[3] && document.ParentOf(grandchildren[4]) == children[4]);

    std::vector<ExpectedEntity> expected;
    for (Entity e : document.Entities()) {
        const Transform* t = document.GetWorld().GetComponent<Transform>(e);
        AETHER_CHECK(t != nullptr);
        if (!t) continue;
        expected.push_back({GuidOf(document, e), document.Name(e), t->position, t->scale, t->rotation, GuidOf(document, document.ParentOf(e))});
    }
    const usize total = document.Entities().size();

    const std::filesystem::path dir = FreshSceneDir("aether_test_hundred_entities");
    const std::filesystem::path json_path = dir / "forest.ascene", binary_path = dir / "forest.aesc";
    AETHER_CHECK(document.Save(json_path) && !document.IsDirty());
    AETHER_CHECK(document.Save(binary_path));

    SceneDocument from_json;
    AETHER_CHECK(from_json.Load(json_path));
    ExpectScene(from_json, expected, total, "JSON reload");
    SceneDocument from_binary;
    AETHER_CHECK(from_binary.Load(binary_path));
    ExpectScene(from_binary, expected, total, "binary reload");
    AETHER_CHECK(!from_json.IsDirty() && !from_json.CanUndo()); // a freshly opened scene has no history to undo into

    // Saving the reloaded scene again changes nothing: the round trip is stable, not just one-way.
    const std::filesystem::path second_path = dir / "forest_again.ascene";
    AETHER_CHECK(from_json.Save(second_path));
    SceneDocument again;
    AETHER_CHECK(again.Load(second_path));
    ExpectScene(again, expected, total, "second JSON reload");

    // Ten more committed edits undo one by one back to the saved state, and redo forward again.
    std::vector<Vec3> before;
    for (int i = 0; i < 10; ++i) {
        const Entity e = document.Guids().Find(document.GetWorld(), expected[static_cast<usize>(i)].guid);
        before.push_back(document.GetWorld().GetComponent<Transform>(e)->position);
        AETHER_CHECK(document.SetPosition(e, Vec3{100.0f + static_cast<f32>(i), 0.0f, 0.0f}, true));
    }
    AETHER_CHECK(document.IsDirty());
    for (int i = 0; i < 10; ++i) AETHER_CHECK(document.Undo());
    AETHER_CHECK(!document.IsDirty()); // back at the saved state
    ExpectScene(document, expected, total, "after undoing ten edits");
    for (int i = 0; i < 10; ++i) AETHER_CHECK(document.Redo());
    const Entity moved = document.Guids().Find(document.GetWorld(), expected[9].guid);
    AETHER_CHECK(Close(document.GetWorld().GetComponent<Transform>(moved)->position.x, 109.0f));
}

// M9.1: an interrupted editing session. The last saved scene is untouched, the unsaved work is offered back from the autosave, recovering
// it leaves the scene dirty (so it is not silently treated as saved), and saving afterwards writes it to the real file.
AETHER_TEST(SceneDocument_AutosaveRecoversUnsavedWorkAndLeavesTheSavedSceneIntact) {
    const std::filesystem::path dir = FreshSceneDir("aether_test_autosave_recovery");
    const std::filesystem::path saved = dir / "level.ascene", autosave = dir / "Saved" / "Autosaves" / "level.ascene";

    SceneDocument session;
    session.NewScene();
    const Entity crate = session.CreateEntity("Crate");
    AETHER_CHECK(session.SetPosition(crate, Vec3{1.0f, 2.0f, 3.0f}, true));
    AETHER_CHECK(session.Save(saved));
    const usize saved_count = session.Entities().size();
    const EntityGuid crate_guid = GuidOf(session, crate);

    // Unsaved work after the last save: a move, a rename, and a new entity.
    AETHER_CHECK(session.SetPosition(crate, Vec3{9.0f, 9.0f, 9.0f}, true));
    AETHER_CHECK(session.Rename(crate, "Crate (moved)"));
    session.CreateEntity("Lamp");
    AETHER_CHECK(session.IsDirty() && session.Entities().size() == saved_count + 1);

    // The autosave is written, creating its folder; it is refused (with a reason) during Play, when the world is not the document.
    std::string error;
    session.Play();
    AETHER_CHECK(!session.SaveAutosave(autosave, &error) && !error.empty());
    session.Stop();
    AETHER_CHECK(session.SaveAutosave(autosave, &error));
    AETHER_CHECK(std::filesystem::exists(autosave));

    // The editor dies here. Reopening the scene shows the last SAVED state, with none of the unsaved work.
    SceneDocument reopened;
    AETHER_CHECK(reopened.Load(saved));
    AETHER_CHECK(reopened.Entities().size() == saved_count && !reopened.IsDirty());
    Entity old_crate = reopened.Guids().Find(reopened.GetWorld(), crate_guid);
    AETHER_CHECK(!old_crate.IsNull());
    if (!old_crate.IsNull()) {
        AETHER_CHECK(reopened.Name(old_crate) == "Crate");
        AETHER_CHECK(Close(reopened.GetWorld().GetComponent<Transform>(old_crate)->position.x, 1.0f));
    }

    // Restoring the autosave brings the unsaved work back, still attached to the real file and still unsaved.
    SceneDocument recovered;
    AETHER_CHECK(recovered.LoadAutosave(autosave, saved, &error));
    AETHER_CHECK(recovered.Entities().size() == saved_count + 1);
    AETHER_CHECK(recovered.IsDirty()); // not silently treated as saved
    AETHER_CHECK(recovered.FilePath() == saved);
    Entity back = recovered.Guids().Find(recovered.GetWorld(), crate_guid);
    AETHER_CHECK(!back.IsNull());
    if (!back.IsNull()) {
        AETHER_CHECK(recovered.Name(back) == "Crate (moved)");
        AETHER_CHECK(Close(recovered.GetWorld().GetComponent<Transform>(back)->position.x, 9.0f));
    }

    // The real file still holds the old scene until the user saves; saving then makes the recovered work permanent.
    SceneDocument unchanged;
    AETHER_CHECK(unchanged.Load(saved) && unchanged.Entities().size() == saved_count);
    AETHER_CHECK(recovered.Save() && !recovered.IsDirty());
    SceneDocument final_state;
    AETHER_CHECK(final_state.Load(saved) && final_state.Entities().size() == saved_count + 1);
}

// A save that cannot be written does not touch the last good scene (the writer goes through a temporary file), and says so.
AETHER_TEST(SceneDocument_FailedSaveKeepsTheLastSavedSceneLoadable) {
    const std::filesystem::path dir = FreshSceneDir("aether_test_failed_save");
    const std::filesystem::path saved = dir / "level.ascene";
    SceneDocument document;
    document.NewScene();
    document.CreateEntity("Keep me");
    AETHER_CHECK(document.Save(saved));
    const usize count = document.Entities().size();

    document.CreateEntity("Unsaved");
    // A path whose parent is an ordinary file cannot be created, whatever the user's permissions are.
    std::string error;
    AETHER_CHECK(!document.Save(saved / "inside_a_file.ascene", &error));
    AETHER_CHECK(document.IsDirty()); // the failed save did not mark the work as saved

    SceneDocument reloaded;
    AETHER_CHECK(reloaded.Load(saved) && reloaded.Entities().size() == count);
}
