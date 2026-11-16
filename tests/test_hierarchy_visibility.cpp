// The hierarchy's eye and lock (parity Stage 4): hiding is the engine's Active component (one undo step, inherited
// down the tree, honored by the render scene and by picking), and locking is editor-only and keeps the viewport from
// picking or moving an entity. Headless.

#include "world/scene_panel.h"
#include "world/scene_view.h"

#include "aether/renderer/components.h"
#include "aether/renderer/render_scene.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "test_framework.h"

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <vector>

using namespace aether;
using namespace aether::editor;
namespace vfs = std::filesystem;

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

vfs::path Scratch(const char* name) {
    const vfs::path dir = vfs::temp_directory_path() / name;
    std::error_code ignored;
    vfs::remove_all(dir, ignored);
    vfs::create_directories(dir);
    return dir;
}

} // namespace

// Hiding uses the Active component, which a newly made entity does not have; undo puts back exactly that absence.
AETHER_TEST(HierarchyVisibility_HidingIsTheActiveFlagAndUndoesExactly) {
    SceneDocument document;
    document.NewScene();
    const Entity a = document.CreateEntity("A");
    const Entity b = document.CreateEntity("B");
    const World& world = document.GetWorld();
    AETHER_CHECK(document.IsActive(a) && !document.IsHidden(a));
    AETHER_CHECK(!world.HasComponent<Active>(a));

    AETHER_CHECK(!document.SetActive(a, true)); // already active: nothing recorded
    AETHER_CHECK(document.SetActive(a, false));
    AETHER_CHECK(!document.SetActive(a, false)); // already hidden: nothing recorded
    AETHER_CHECK(!document.IsActive(a) && document.IsHidden(a));
    AETHER_CHECK(world.HasComponent<Active>(a));

    AETHER_CHECK(document.Undo());
    AETHER_CHECK(document.IsActive(a) && !document.IsHidden(a));
    AETHER_CHECK(!world.HasComponent<Active>(a)); // back to no component at all
    AETHER_CHECK(document.Redo());
    AETHER_CHECK(document.IsHidden(a));

    // Showing it again, then undoing that, hides it again: the old flag comes back, not "no component".
    AETHER_CHECK(document.SetActive(a, true));
    AETHER_CHECK(document.Undo());
    const Active* flag = world.GetComponent<Active>(a);
    AETHER_CHECK(flag != nullptr && !flag->active && document.IsHidden(a));

    // Hiding is inherited down the tree, but a child's own flag can still be set and is remembered for later.
    const Entity parent = document.CreateEntity("Parent");
    const Entity child = document.CreateEntity("Child", SceneEntityKind::Empty, {}, parent);
    AETHER_CHECK(document.SetActive(parent, false));
    AETHER_CHECK(document.IsHidden(child) && document.IsActive(child)); // its own flag is untouched
    AETHER_CHECK(document.SetActive(child, false) && document.SetActive(child, true));
    AETHER_CHECK(document.IsHidden(child)); // the parent still hides it
    AETHER_CHECK(document.SetActive(parent, true));
    AETHER_CHECK(!document.IsHidden(child) && !document.IsHidden(parent));
}

// The row's eye hides a whole selection as one undo step; the lock records nothing and is cleared with the scene.
AETHER_TEST(HierarchyVisibility_PanelHidesASetAsOneStepAndLocksWithoutUndo) {
    ScenePanel panel(Scratch("aether_hierarchy_visibility_panel"));
    SceneDocument& document = panel.Document();
    const Entity a = document.CreateEntity("A");
    const Entity b = document.CreateEntity("B");
    const EntityGuid guid_a = document.GetWorld().GetComponent<IdComponent>(a)->guid;
    const std::vector<Entity> both{a, b};
    const usize undo = document.Commands().UndoCount();

    AETHER_CHECK(panel.SetHidden(both, true) == 2);
    AETHER_CHECK(document.Commands().UndoCount() == undo + 1);
    AETHER_CHECK(panel.IsHidden(a) && panel.IsHidden(b));
    AETHER_CHECK(panel.SetHidden(both, true) == 0); // already hidden

    AETHER_CHECK(document.Undo());
    AETHER_CHECK(panel.IsHidden(a) == false && panel.IsHidden(b) == false);
    AETHER_CHECK(!document.GetWorld().HasComponent<Active>(a) && !document.GetWorld().HasComponent<Active>(b));
    AETHER_CHECK(document.Redo());
    AETHER_CHECK(panel.SetHidden(a, false)); // one entity alone is its own step

    const u64 undo_now = document.Commands().UndoCount();
    AETHER_CHECK(panel.SetLocked(both, true) == 2);
    AETHER_CHECK(panel.IsLocked(a) && panel.IsLocked(b));
    AETHER_CHECK(document.Commands().UndoCount() == undo_now); // locking is not an edit
    AETHER_CHECK(panel.SetLocked(both, true) == 0);
    AETHER_CHECK(document.Undo()); // the undo goes to the last edit, not to a lock
    AETHER_CHECK(panel.IsLocked(a)); // and leaves the lock alone

    document.NewScene();
    AETHER_CHECK(!document.IsLocked(guid_a)); // a new scene starts unlocked
}

// What the renderer is handed leaves hidden entities out, both their own flag and a parent's.
AETHER_TEST(HierarchyVisibility_HiddenEntitiesLeaveTheRenderScene) {
    RegisterRenderComponents();
    (void)GetComponentId<ModelRenderer>();
    SceneDocument document;
    document.NewScene();
    const Entity parent = document.CreateEntity("Parent");
    const Entity crate = document.CreateEntity("Crate", SceneEntityKind::Empty, {}, parent);
    ModelRenderer renderer;
    SetModelPath(renderer, "models/crate.gltf");
    document.GetWorld().AddComponent(crate, renderer);

    AETHER_CHECK(ExtractRenderScene(document.GetWorld(), document.Guids()).objects.size() == 1);
    AETHER_CHECK(document.SetActive(crate, false));
    AETHER_CHECK(ExtractRenderScene(document.GetWorld(), document.Guids()).objects.empty());
    AETHER_CHECK(document.SetActive(crate, true));

    AETHER_CHECK(document.SetActive(parent, false));
    AETHER_CHECK(ExtractRenderScene(document.GetWorld(), document.Guids()).objects.empty()); // inherited
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(ExtractRenderScene(document.GetWorld(), document.Guids()).objects.size() == 1);
}

// The viewport neither picks nor marquee-selects a hidden or a locked entity; a locked one stays selected and editable.
AETHER_TEST(HierarchyVisibility_HiddenAndLockedEntitiesAreNotPickedOrMoved) {
    SceneDocument document;
    const Entity near_one = document.CreateEntity("Near");
    const Entity far_one = document.CreateEntity("Far");
    document.SetPosition(near_one, Vec3(0.0f, 0.0f, 0.0f), true);
    document.SetPosition(far_one, Vec3(6.0f, 0.0f, 0.0f), true);
    SceneViewCamera view;
    view.target = Vec3(3.0f, 0.0f, 0.0f);
    view.distance = 15.0f;
    const ImVec2 origin(10.0f, 20.0f), size(800.0f, 600.0f);
    const Mat4 vp = view.ViewProjection(size.x / size.y);
    ImVec2 p_near, p_far;
    AETHER_CHECK(ProjectToViewport(vp, Vec3(0, 0, 0), origin, size, p_near) && ProjectToViewport(vp, Vec3(6, 0, 0), origin, size, p_far));
    AETHER_CHECK(PickEntity(document, view, origin, size, p_near) == near_one);
    AETHER_CHECK(PickEntity(document, view, origin, size, p_far) == far_one);

    // Locked: not the entity under the mouse, not in the stack under it, not marquee-selected.
    document.SetLocked(far_one, true);
    AETHER_CHECK(PickEntity(document, view, origin, size, p_far) == kNullEntity);
    AETHER_CHECK(PickEntities(document, view, origin, size, p_far).empty());
    AETHER_CHECK(document.IsLocked(far_one) && !document.IsLocked(near_one));
    std::vector<Entity> inside = EntitiesInRectangle(document, view, origin, size, origin, ImVec2(origin.x + size.x, origin.y + size.y));
    AETHER_CHECK(inside.size() == 1 && inside[0] == near_one);

    // A locked entity is still selectable from the hierarchy, renameable, and never moved by the gizmo.
    document.SetSelection({near_one, far_one});
    AETHER_CHECK(document.IsSelected(far_one));
    AETHER_CHECK(document.Rename(far_one, "Renamed"));
    std::vector<Entity> subjects = TransformGizmo::Subjects(document);
    AETHER_CHECK(subjects.size() == 1 && subjects[0] == near_one);

    document.SetLocked(far_one, false);
    subjects = TransformGizmo::Subjects(document);
    AETHER_CHECK(subjects.size() == 2);

    // Hidden: gone from both the pick and the marquee, and its children with it.
    document.SetActive(near_one, false);
    AETHER_CHECK(PickEntity(document, view, origin, size, p_near) == kNullEntity);
    inside = EntitiesInRectangle(document, view, origin, size, origin, ImVec2(origin.x + size.x, origin.y + size.y));
    AETHER_CHECK(inside.size() == 1 && inside[0] == far_one);
    subjects = TransformGizmo::Subjects(document);
    AETHER_CHECK(subjects.size() == 1 && subjects[0] == far_one);
}

// Drawing rows with hidden and locked entities records nothing and leaves the states as they were.
AETHER_TEST(ScenePanel_HiddenAndLockedRowsDrawWithoutEditing) {
    HeadlessImGui imgui;
    ScenePanel panel(Scratch("aether_hierarchy_visibility_rows"));
    SceneDocument& document = panel.Document();
    const Entity hidden = document.CreateEntity("Hidden");
    const Entity locked = document.CreateEntity("Locked");
    AETHER_CHECK(panel.SetHidden(hidden, true));
    AETHER_CHECK(panel.SetLocked(locked, true));
    const usize undo = document.Commands().UndoCount();
    const usize entities = document.Entities().size();
    auto frame = [&] { imgui.Frame([&] { panel.Draw(); }); };
    frame();
    frame();
    AETHER_CHECK(document.Commands().UndoCount() == undo);
    AETHER_CHECK(document.Entities().size() == entities);
    AETHER_CHECK(panel.IsHidden(hidden) && !panel.IsLocked(hidden));
    AETHER_CHECK(panel.IsLocked(locked) && !panel.IsHidden(locked));
}
