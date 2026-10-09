#include "core/scene_document.h"
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
