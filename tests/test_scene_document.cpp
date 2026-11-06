#include "core/scene_document.h"
#include "aether/scene/data_layers.h"
#include "aether/scene/level_instance.h"
#include "core/snapping.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/serialization.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <string>

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