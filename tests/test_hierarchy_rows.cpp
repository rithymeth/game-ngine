// The hierarchy's row badges and prefab state (parity Stage 4): what kind of entity a row is, judged by its component
// types, and whether it is a prefab instance's root or part, carries overrides, or lost its prefab. Headless.

#include "world/scene_panel.h"

#include "aether/reflection/reflect_macros.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/prefab.h"
#include "aether/scene/script_component.h"
#include "test_framework.h"

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <string>

using namespace aether;
using namespace aether::editor;
namespace hrfs = std::filesystem;

namespace rows_test {

struct Crate {
    i32 weight = 4;
};

} // namespace rows_test

AETHER_REFLECT(rows_test::Crate, 1, AETHER_FIELD(weight, Field_EditAnywhere))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400, 900);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1400, 900));
        ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

hrfs::path Scratch(const char* name) {
    const hrfs::path dir = hrfs::temp_directory_path() / name;
    std::error_code ignored;
    hrfs::remove_all(dir, ignored);
    hrfs::create_directories(dir);
    return dir;
}

// A two-entity prefab (Crate root 1, Lid 2 under it) instantiated into `document`, with one override on the lid.
struct PrefabScene {
    PrefabData prefab;
    assets::AssetGuid source = assets::NewAssetGuid();
    Entity root = kNullEntity;
    Entity lid = kNullEntity;

    explicit PrefabScene(SceneDocument& document) {
        (void)GetComponentId<rows_test::Crate>(); // the prefab names its components: register them before resolving
        (void)GetComponentId<EntityName>();
        PrefabEntity crate;
        crate.id = 1;
        crate.components[reflect::Reflect<rows_test::Crate>().name] = reflect::ToJson(rows_test::Crate{});
        crate.components[reflect::Reflect<EntityName>().name] = reflect::ToJson(EntityName{"Crate"});
        PrefabEntity part;
        part.id = 2;
        part.parent = 1;
        part.components[reflect::Reflect<rows_test::Crate>().name] = reflect::ToJson(rows_test::Crate{});
        part.components[reflect::Reflect<EntityName>().name] = reflect::ToJson(EntityName{"Lid"});
        prefab.entities = {crate, part};

        ResolveReport report;
        root = InstantiatePrefab(document.GetWorld(), document.Guids(), source, prefab, nullptr, &report);
        PrefabInstance& instance = *document.GetWorld().GetComponent<PrefabInstance>(root);
        SetOverride(instance, 2, reflect::Reflect<rows_test::Crate>().name, "weight", 9);
        (void)ResolvePrefabInstance(document.GetWorld(), document.Guids(), root, prefab);
        for (const Entity e : report.created_entities) {
            const PrefabLink* link = document.GetWorld().GetComponent<PrefabLink>(e);
            if (link != nullptr && link->local_id == 2) lid = e;
        }
    }

    PrefabLookup Lookup() const {
        return [this](const assets::AssetGuid& guid) { return guid == source ? &prefab : nullptr; };
    }
};

} // namespace

AETHER_TEST(HierarchyRows_ComponentNamesMapToKinds) {
    AETHER_CHECK(ComponentKind("aether::Camera") == EntityKind::Camera);
    AETHER_CHECK(ComponentKind("aether::CineCamera") == EntityKind::Camera);
    AETHER_CHECK(ComponentKind("aether::DirectionalLight") == EntityKind::Light);
    AETHER_CHECK(ComponentKind("aether::sprite2d::Light2D") == EntityKind::Light);
    AETHER_CHECK(ComponentKind("aether::ModelRenderer") == EntityKind::Model);
    AETHER_CHECK(ComponentKind("aether::sprite2d::Sprite") == EntityKind::Model);
    AETHER_CHECK(ComponentKind("aether::Decal") == EntityKind::Model);
    AETHER_CHECK(ComponentKind("aether::Particles") == EntityKind::Particles);
    AETHER_CHECK(ComponentKind("aether::AudioSource") == EntityKind::Audio);
    AETHER_CHECK(ComponentKind("aether::WidgetComponent") == EntityKind::Ui);
    AETHER_CHECK(ComponentKind("aether::RigidBody") == EntityKind::Physics);
    AETHER_CHECK(ComponentKind("aether::MeshCollider") == EntityKind::Physics); // a collider, not a mesh
    AETHER_CHECK(ComponentKind("aether::HeightfieldCollider") == EntityKind::Physics);
    AETHER_CHECK(ComponentKind("aether::ScriptComponent") == EntityKind::Script);
    AETHER_CHECK(ComponentKind("aether::bp::BlueprintInstance") == EntityKind::Script);
    AETHER_CHECK(ComponentKind("aether::PostProcessVolume") == EntityKind::Volume);
    AETHER_CHECK(ComponentKind("aether::WindZone") == EntityKind::Volume);

    // Bookkeeping says nothing about what an entity is; an unknown component is a plain entity.
    AETHER_CHECK(ComponentKind("aether::Transform") == EntityKind::Empty);
    AETHER_CHECK(ComponentKind("aether::EntityName") == EntityKind::Empty);
    AETHER_CHECK(ComponentKind("aether::PrefabLink") == EntityKind::Empty);
    AETHER_CHECK(ComponentKind("struct aether::PreviousTransform") == EntityKind::Empty); // a typeid name
    AETHER_CHECK(ComponentKind("rows_test::Crate") == EntityKind::Other);

    // Every kind has a name and a letter of its own.
    for (int a = 0; a <= static_cast<int>(EntityKind::Empty); ++a) {
        AETHER_CHECK(std::string(EntityKindName(static_cast<EntityKind>(a))).size() > 1);
        for (int b = a + 1; b <= static_cast<int>(EntityKind::Empty); ++b)
            AETHER_CHECK(EntityKindGlyph(static_cast<EntityKind>(a)) != EntityKindGlyph(static_cast<EntityKind>(b)));
    }
}

AETHER_TEST(HierarchyRows_AnEntityIsItsStrongestKind) {
    SceneDocument document;
    document.NewScene();
    World& world = document.GetWorld();
    const Entity empty = document.CreateEntity("Empty");
    const Entity camera = document.CreateEntity("Camera", SceneEntityKind::Camera);
    const Entity model = document.CreateEntity("Model");
    world.AddComponent(model, ModelRenderer{});
    const Entity both = document.CreateEntity("Camera with a model");
    world.AddComponent(both, ModelRenderer{});
    world.AddComponent(both, Camera{});
    const Entity scripted = document.CreateEntity("Scripted");
    world.AddComponent(scripted, ScriptComponent{});
    const Entity crate = document.CreateEntity("Crate");
    world.AddComponent(crate, rows_test::Crate{});

    AETHER_CHECK(ClassifyEntity(world, empty) == EntityKind::Empty);
    AETHER_CHECK(ClassifyEntity(world, camera) == EntityKind::Camera);
    AETHER_CHECK(ClassifyEntity(world, model) == EntityKind::Model);
    AETHER_CHECK(ClassifyEntity(world, both) == EntityKind::Camera);
    AETHER_CHECK(ClassifyEntity(world, scripted) == EntityKind::Script);
    AETHER_CHECK(ClassifyEntity(world, crate) == EntityKind::Other);

    // A scripted model reads as a model: what is seen in the scene outranks what drives it.
    world.AddComponent(scripted, ModelRenderer{});
    AETHER_CHECK(ClassifyEntity(world, scripted) == EntityKind::Model);
}

AETHER_TEST(HierarchyRows_PrefabStateNamesRootPartsOverridesAndAMissingPrefab) {
    SceneDocument document;
    document.NewScene();
    const PrefabScene scene(document);
    AETHER_CHECK(!scene.root.IsNull() && !scene.lid.IsNull());
    if (scene.root.IsNull() || scene.lid.IsNull()) return;
    const Entity plain = document.CreateEntity("Plain");
    const World& world = document.GetWorld();
    const PrefabLookup found = scene.Lookup();

    const PrefabRowState root = PrefabRowStateOf(world, document.Guids(), found, scene.root);
    AETHER_CHECK(root.role == PrefabRowState::Role::Root);
    AETHER_CHECK(root.overrides == 0); // the override is the lid's, not the root's
    AETHER_CHECK(!root.missing_source);
    const PrefabRowState lid = PrefabRowStateOf(world, document.Guids(), found, scene.lid);
    AETHER_CHECK(lid.role == PrefabRowState::Role::Part);
    AETHER_CHECK(lid.overrides == 1);
    AETHER_CHECK(PrefabRowStateOf(world, document.Guids(), found, plain).role == PrefabRowState::Role::None);

    // A lookup that cannot find the prefab marks the instance; no lookup at all cannot tell, so it marks nothing.
    const PrefabLookup lost = [](const assets::AssetGuid&) -> const PrefabData* { return nullptr; };
    AETHER_CHECK(PrefabRowStateOf(world, document.Guids(), lost, scene.lid).missing_source);
    AETHER_CHECK(!PrefabRowStateOf(world, document.Guids(), PrefabLookup{}, scene.lid).missing_source);

    AETHER_CHECK(HierarchyRowTooltip(EntityKind::Light, PrefabRowState{}) == "Light");
    AETHER_CHECK(HierarchyRowTooltip(EntityKind::Other, root) == "Entity - prefab instance");
    AETHER_CHECK(HierarchyRowTooltip(EntityKind::Other, lid) == "Entity - part of a prefab instance (1 override)");
    PrefabRowState missing = lid;
    missing.missing_source = true;
    missing.overrides = 3;
    AETHER_CHECK(HierarchyRowTooltip(EntityKind::Empty, missing) ==
                 "Empty - part of a prefab instance whose prefab is missing (3 overrides)");
}

AETHER_TEST(ScenePanel_BadgedRowsDrawWithoutEditing) {
    HeadlessImGui imgui;
    ScenePanel panel(Scratch("aether_hierarchy_rows_panel"));
    SceneDocument& document = panel.Document();
    const PrefabScene scene(document);
    document.CreateEntity("Camera", SceneEntityKind::Camera);
    const Entity crate = document.CreateEntity("Loose crate");
    document.GetWorld().AddComponent(crate, rows_test::Crate{});
    auto frame = [&] { imgui.Frame([&] { panel.Draw(); }); };
    const usize entities = document.Entities().size();
    const usize undo = document.Commands().UndoCount();

    // Without a prefab lookup, with one that finds the prefab, and with one that has lost it.
    frame();
    frame();
    document.Context().find_prefab = scene.Lookup();
    frame();
    document.Context().find_prefab = [](const assets::AssetGuid&) -> const PrefabData* { return nullptr; };
    frame();
    panel.SetHierarchyFilter("lid"); // a filtered tree draws the same rows
    frame();
    AETHER_CHECK(document.Commands().UndoCount() == undo);
    AETHER_CHECK(document.Entities().size() == entities);
    AETHER_CHECK(document.Selection().empty());
}
