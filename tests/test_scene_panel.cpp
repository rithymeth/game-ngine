#include "world/scene_panel.h"

#include "core/commands.h"
#include "content/content_drag.h"
#include "ui/builtin_components.h"
#include "ui/editor_theme.h"
#include "ui/shell_layout.h"
#include "world/scene_view_panel.h"
#include "world/viewport_tools.h"
#include "workspace/editor_workspace.h"
#include "aether/templates/templates.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "test_framework.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

// The scene editor of the portable (ImGui) editor: the hierarchy as a tree, the toolbar's actions, and the panel drawn headless in every state it has.

using namespace aether;
using namespace aether::editor;
namespace stdfs = std::filesystem;

namespace {

Vec3 WorldPositionOf(const SceneDocument& document, Entity entity) {
    const Mat4 m = ComputeWorldTransform(document.GetWorld(), document.Guids(), entity);
    return Vec3(m.cols[3].x, m.cols[3].y, m.cols[3].z);
}

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

stdfs::path Scratch(const char* name) {
    const stdfs::path dir = stdfs::temp_directory_path() / name;
    std::error_code ignored;
    stdfs::remove_all(dir, ignored);
    stdfs::create_directories(dir);
    return dir;
}

std::vector<std::string> Names(const SceneDocument& document, const std::vector<Entity>& entities) {
    std::vector<std::string> out;
    for (const Entity e : entities) out.push_back(document.Name(e));
    return out;
}

} // namespace

AETHER_TEST(ScenePanel_TheHierarchyIsATreeByParentSortedByName) {
    SceneDocument document;
    document.NewScene();
    const usize defaults = document.Entities().size();
    const Entity zebra = document.CreateEntity("Zebra");
    const Entity apple = document.CreateEntity("apple"); // case does not decide the order
    const Entity leaf_b = document.CreateEntity("Leaf B", SceneEntityKind::Empty, {}, zebra);
    const Entity leaf_a = document.CreateEntity("Leaf A", SceneEntityKind::Empty, {}, zebra);
    const Entity deep = document.CreateEntity("Deep", SceneEntityKind::Empty, {}, leaf_a);
    const HierarchyIndex index(document);
    const std::vector<Entity>& roots = index.Children(kNullEntity);
    AETHER_CHECK(roots.size() == defaults + 2);
    const std::vector<std::string> root_names = Names(document, roots);
    AETHER_CHECK(std::is_sorted(root_names.begin(), root_names.end(), [](const std::string& a, const std::string& b) {
        std::string x = a, y = b;
        std::transform(x.begin(), x.end(), x.begin(), ::tolower);
        std::transform(y.begin(), y.end(), y.begin(), ::tolower);
        return x < y;
    }));
    AETHER_CHECK(std::find(roots.begin(), roots.end(), apple) != roots.end() && std::find(roots.begin(), roots.end(), zebra) != roots.end());
    AETHER_CHECK(std::find(roots.begin(), roots.end(), leaf_a) == roots.end()); // a child is not a root
    AETHER_CHECK(Names(document, index.Children(zebra)) == (std::vector<std::string>{"Leaf A", "Leaf B"}));
    AETHER_CHECK(index.Children(leaf_a).size() == 1 && index.Children(leaf_a)[0] == deep);
    AETHER_CHECK(index.HasChildren(zebra) && !index.HasChildren(deep) && !index.HasChildren(leaf_b));
    // Every entity is somewhere in the tree exactly once.
    usize reached = 0;
    std::function<void(Entity)> walk = [&](Entity parent) {
        for (const Entity child : index.Children(parent)) { ++reached; walk(child); }
    };
    walk(kNullEntity);
    AETHER_CHECK(reached == document.Entities().size());
}

AETHER_TEST(ScenePanel_TheFilterMatchesNamesWithoutRegardToCase) {
    SceneDocument document;
    document.NewScene();
    document.CreateEntity("Front Door");
    document.CreateEntity("Back door");
    document.CreateEntity("Window");
    AETHER_CHECK(MatchingEntities(document, "").size() == document.Entities().size());
    const std::vector<Entity> doors = MatchingEntities(document, "DOOR");
    AETHER_CHECK((Names(document, doors) == std::vector<std::string>{"Back door", "Front Door"}));
    AETHER_CHECK(MatchingEntities(document, "nothing like it").empty());
}

AETHER_TEST(ScenePanel_NewAndOpenAskBeforeDiscardingUnsavedWork) {
    const stdfs::path root = Scratch("aether_scene_panel_open");
    ScenePanel panel(root);
    panel.Document().CreateEntity("Crate");
    const usize with_crate = panel.Document().Entities().size();
    AETHER_CHECK(panel.Document().IsDirty());
    // Unsaved: refused, and the scene is as it was.
    AETHER_CHECK(!panel.NewScene() && panel.Status().find("unsaved changes") != std::string::npos);
    AETHER_CHECK(panel.Document().Entities().size() == with_crate);
    AETHER_CHECK(!panel.OpenScene("whatever.ascene") && panel.Document().Entities().size() == with_crate);
    // Saved to a path under the content root, then nothing is lost by opening it.
    AETHER_CHECK(panel.SaveScene("Scenes/Crate.ascene"));
    AETHER_CHECK(stdfs::exists(root / "Scenes/Crate.ascene") && !panel.Document().IsDirty());
    AETHER_CHECK(panel.Status().find("Saved") == 0);
    AETHER_CHECK(panel.NewScene() && panel.Document().Entities().size() < with_crate); // clean: no question
    AETHER_CHECK(panel.OpenScene("Scenes/Crate.ascene") && panel.Document().Entities().size() == with_crate);
    AETHER_CHECK(panel.Status().find("Opened") == 0);
    // A scene that is not there, or is not a scene: refused with the reason, the open scene untouched.
    AETHER_CHECK(!panel.OpenScene("Scenes/missing.ascene") && !panel.Status().empty() && panel.Document().Entities().size() == with_crate);
    { std::ofstream(root / "Scenes/broken.ascene") << "not a scene"; }
    AETHER_CHECK(!panel.OpenScene("Scenes/broken.ascene") && panel.Document().Entities().size() == with_crate);
    // Discarding is explicit.
    panel.Document().CreateEntity("Extra");
    AETHER_CHECK(!panel.NewScene());
    AETHER_CHECK(panel.NewScene(true));
    std::error_code ignored;
    stdfs::remove_all(root, ignored);
}

AETHER_TEST(ScenePanel_ReparentRefusesACycleAndDeleteTakesTheChildrenAndUndoBringsThemBack) {
    ScenePanel panel(Scratch("aether_scene_panel_tree"));
    SceneDocument& document = panel.Document();
    const Entity parent = document.CreateEntity("Parent");
    const Entity child = document.CreateEntity("Child", SceneEntityKind::Empty, {}, parent);
    const Entity grandchild = document.CreateEntity("Grandchild", SceneEntityKind::Empty, {}, child);
    const Entity other = document.CreateEntity("Other");
    // Under its own descendant: refused, with a reason, nothing moved.
    AETHER_CHECK(!panel.Reparent(parent, grandchild) && panel.Status().find("cannot go under") != std::string::npos);
    AETHER_CHECK(document.ParentOf(parent).IsNull() && document.ParentOf(grandchild) == child);
    AETHER_CHECK(!panel.Reparent(parent, parent));
    AETHER_CHECK(!panel.Reparent(child, parent)); // already there
    // A move, and back to the root.
    AETHER_CHECK(panel.Reparent(grandchild, other) && document.ParentOf(grandchild) == other);
    AETHER_CHECK(panel.Reparent(grandchild, kNullEntity) && document.ParentOf(grandchild).IsNull());
    // Deleting the parent takes its subtree: every entity is either there or not, none left hanging under a parent that is gone.
    const usize before = document.Entities().size();
    document.SetSelection({parent});
    AETHER_CHECK(panel.DeleteSelection() == 1);
    const HierarchyIndex after(document);
    usize reached = 0;
    std::function<void(Entity)> walk = [&](Entity p) { for (const Entity c : after.Children(p)) { ++reached; walk(c); } };
    walk(kNullEntity);
    AETHER_CHECK(reached == document.Entities().size() && document.Entities().size() < before);
    AETHER_CHECK(!document.GetWorld().IsAlive(parent));
    AETHER_CHECK(document.Undo() && document.Entities().size() == before);
}

AETHER_TEST(ScenePanel_DuplicateSelectsTheCopiesAndDeleteClearsTheSelection) {
    ScenePanel panel(Scratch("aether_scene_panel_dup"));
    SceneDocument& document = panel.Document();
    const Entity a = document.CreateEntity("Alpha");
    const Entity b = document.CreateEntity("Beta");
    const usize before = document.Entities().size();
    document.SetSelection({a, b});
    AETHER_CHECK(panel.DuplicateSelection() == 2);
    AETHER_CHECK(document.Entities().size() == before + 2);
    AETHER_CHECK(document.Selection().size() == 2 && document.Selection()[0] != a && document.Selection()[1] != b); // the copies are what is selected now
    AETHER_CHECK(panel.DeleteSelection() == 2 && document.Entities().size() == before && document.Selection().empty());
}

AETHER_TEST(ScenePanel_TheInspectorsEditsAreTheDocumentsOwnUndoableChanges) {
    // What the inspector does (a command through the document's stack and context) marks the scene as changed and is undone with everything else.
    ScenePanel panel(Scratch("aether_scene_panel_cmd"));
    SceneDocument& document = panel.Document();
    const Entity thing = document.CreateEntity("Thing");
    AETHER_CHECK(panel.SaveScene("Scenes/Thing.ascene") && !document.IsDirty());
    const EntityGuid guid = document.GetWorld().GetComponent<IdComponent>(thing)->guid;
    const ComponentId tags = GetComponentId<Tags>();
    AETHER_CHECK(!document.GetWorld().HasComponentRaw(thing, tags));
    document.Commands().Execute(document.Context(), std::make_unique<AddComponentCommand>(guid, tags));
    AETHER_CHECK(document.GetWorld().HasComponentRaw(thing, tags)); // it landed on the document's world
    AETHER_CHECK(document.IsDirty() && document.CanUndo());         // and the document knows it changed
    AETHER_CHECK(document.Undo() && !document.GetWorld().HasComponentRaw(thing, tags));
    AETHER_CHECK(!document.IsDirty());                              // undone back to what was saved
}

AETHER_TEST(ScenePanel_DrawsEveryStateHeadless) {
    HeadlessImGui imgui;
    ScenePanel panel(Scratch("aether_scene_panel_draw"));
    SceneDocument& document = panel.Document();
    const Entity root = document.CreateEntity("Root");
    const Entity child = document.CreateEntity("Child", SceneEntityKind::Camera, {}, root);
    auto frame = [&] { imgui.Frame([&] { panel.Draw(); }); };
    frame();                                        // nothing selected: the prompt
    document.SetSelection({child});
    frame();                                        // the inspector draws a camera: its reflected components
    document.SetSelection({root, child});
    frame();                                        // two selected: says so, shows the first
    panel.BeginRename(child);
    AETHER_CHECK(panel.Renaming());
    frame();
    frame();                                        // renaming draws a text box; two frames settle it
    document.SetSelection({child});
    document.DestroyEntity(child);                  // a selection that is no longer alive
    frame();
    document.Play();
    frame();                                        // playing: the toolbar changes
    document.Pause();
    frame();
    document.Stop();
    frame();
    AETHER_CHECK(document.PlayState() == PlaySession::State::Editing);
    panel.SetPath("Scenes/x.ascene");
    frame();
    AETHER_CHECK(panel.Path() == "Scenes/x.ascene");
}

// The editor's theme: spacing and colors always apply; a missing font is reported, not fatal.
AETHER_TEST(EditorTheme_AppliesWithoutAFont) {
    HeadlessImGui imgui;
    const bool loaded = ApplyEditorTheme(16.0f, Scratch("aether_theme_none") / "missing.ttf");
    AETHER_CHECK(!loaded);
    const ImGuiStyle& style = ImGui::GetStyle();
    AETHER_CHECK(style.WindowRounding > 0.0f && style.FrameRounding > 0.0f);
    AETHER_CHECK(style.ItemSpacing.y >= 5.0f);
    AETHER_CHECK(style.Colors[ImGuiCol_Text].x > 0.8f);                  // light text on a dark window
    AETHER_CHECK(style.Colors[ImGuiCol_WindowBg].x < 0.2f);
    imgui.Frame([&] { ImGui::Begin("themed"); ImGui::Button("ok"); ImGui::End(); });
}

// The menu bar's File and Edit items act on the Scene tool's document, and draw in a menu bar headless.
AETHER_TEST(EditorWorkspace_MenusReachTheSceneDocument) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    SceneDocument& document = workspace.Scene().Document();
    const usize before = document.Entities().size();
    const Entity thing = document.CreateEntity("Thing");
    document.SetSelection({thing});
    imgui.Frame([&] {
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) { workspace.DrawSceneFileItems(); ImGui::EndMenu(); }
            workspace.DrawEditMenu();
            ImGui::EndMainMenuBar();
        }
    });
    AETHER_CHECK(document.Entities().size() == before + 1);
    AETHER_CHECK(workspace.Scene().DuplicateSelection() == 1);   // what the Edit menu's Duplicate calls
    AETHER_CHECK(document.CanUndo());
    imgui.Frame([&] { if (ImGui::BeginMainMenuBar()) { workspace.DrawEditMenu(); ImGui::EndMainMenuBar(); } });
}

// The toolbar's scene list: what is in the content folder, sorted, with nothing else in it.
AETHER_TEST(ScenePanel_ListsTheScenesInTheContentFolder) {
    const stdfs::path root = Scratch("aether_scene_panel_list");
    ScenePanel panel(root);
    AETHER_CHECK(panel.ListScenes().empty());                  // no folder yet: an empty list, not an error
    panel.Document().CreateEntity("A");
    AETHER_CHECK(panel.SaveScene("Scenes/b.ascene"));
    AETHER_CHECK(panel.SaveScene("Levels/Deep/a.ascene"));
    stdfs::create_directories(root / "Textures");
    std::ofstream(root / "Textures/not_a_scene.png") << "x";
    const std::vector<std::string> scenes = panel.ListScenes();
    AETHER_CHECK(scenes.size() == 2);
    if (scenes.size() == 2) {
        AETHER_CHECK(scenes[0] == "Levels/Deep/a.ascene");      // relative, forward slashes, sorted
        AETHER_CHECK(scenes[1] == "Scenes/b.ascene");
    }
    HeadlessImGui imgui;
    imgui.Frame([&] { panel.Draw(); });                        // the toolbar draws with the list available
    AETHER_CHECK(panel.OpenScene("Levels/Deep/a.ascene", true)); // and what it lists opens
}

// The shell's remembered layout: it round-trips, and a missing, damaged or out-of-range file never gets in the way.
AETHER_TEST(ShellLayout_RoundTripsAndForgivesABadFile) {
    const stdfs::path dir = Scratch("aether_shell_layout");
    const stdfs::path file = dir / "nested" / "editor_layout.json";
    ShellLayout layout;
    AETHER_CHECK(!LoadShellLayout(file, layout));                       // missing: false, layout untouched
    AETHER_CHECK(layout.tool.empty());
    layout.tool = "Terrain";
    std::string error;
    AETHER_CHECK(SaveShellLayout(file, layout, &error));                // makes the folder
    AETHER_CHECK(!stdfs::exists(stdfs::path(file).concat(".tmp")));     // the temp file did not stay
    ShellLayout back;
    AETHER_CHECK(LoadShellLayout(file, back));
    AETHER_CHECK(back.tool == "Terrain");
    auto write = [&](const std::string& text) { std::ofstream(file, std::ios::binary | std::ios::trunc) << text; };
    write("{ this is not json");
    ShellLayout damaged;
    damaged.tool = "kept";
    AETHER_CHECK(!LoadShellLayout(file, damaged) && damaged.tool == "kept");
    write(R"({"side_fraction": 0.4, "tool": "Scene"})");                // a file from before docking: the old key is ignored, the tool survives
    ShellLayout older;
    AETHER_CHECK(LoadShellLayout(file, older) && older.tool == "Scene");
    write(R"({"tool": 7})");                                            // a wrong type is ignored, not fatal
    ShellLayout typed;
    AETHER_CHECK(LoadShellLayout(file, typed) && typed.tool.empty());
    write("{\"tool\": \"" + std::string(500, 'x') + "\"}");             // an absurd name is dropped
    ShellLayout long_name;
    AETHER_CHECK(LoadShellLayout(file, long_name) && long_name.tool.empty());
    AETHER_CHECK(!SaveShellLayout({}, layout, &error) && !error.empty()); // no path: refused, with a reason
}

// The scene view's orbit camera, projection and picking: plain maths, no window.
AETHER_TEST(SceneView_CameraProjectsAndPicks) {
    SceneViewCamera camera;
    camera.target = Vec3(2.0f, 1.0f, -3.0f);
    camera.distance = 10.0f;
    const Vec3 eye = camera.Eye();
    const Vec3 to_target = camera.target - eye;
    AETHER_CHECK(std::fabs(to_target.Length() - 10.0f) < 1e-3f);          // the eye sits `distance` from the target

    const ImVec2 origin(100.0f, 50.0f), size(800.0f, 600.0f);
    const Mat4 vp = camera.ViewProjection(size.x / size.y);
    ImVec2 at;
    f32 depth = 0.0f;
    AETHER_CHECK(ProjectToViewport(vp, camera.target, origin, size, at, &depth));
    AETHER_CHECK(std::fabs(at.x - (origin.x + size.x * 0.5f)) < 0.5f && std::fabs(at.y - (origin.y + size.y * 0.5f)) < 0.5f); // the target is the middle
    AETHER_CHECK(std::fabs(depth - 10.0f) < 1e-2f);
    AETHER_CHECK(!ProjectToViewport(vp, eye - to_target, origin, size, at)); // behind the camera: not drawn
    // Orientation: world up is up on the screen (smaller y), and the camera's right is right (larger x).
    camera.pitch = 0.3f;
    const Mat4 vp_level = camera.ViewProjection(size.x / size.y);
    ImVec2 centre, above, beside;
    AETHER_CHECK(ProjectToViewport(vp_level, camera.target, origin, size, centre));
    AETHER_CHECK(ProjectToViewport(vp_level, camera.target + Vec3(0.0f, 2.0f, 0.0f), origin, size, above));
    AETHER_CHECK(above.y < centre.y - 5.0f);
    const Mat4 view_matrix = camera.View();
    const Vec3 camera_right(view_matrix.cols[0].x, view_matrix.cols[1].x, view_matrix.cols[2].x);
    AETHER_CHECK(ProjectToViewport(vp_level, camera.target + camera_right * 2.0f, origin, size, beside));
    AETHER_CHECK(beside.x > centre.x + 5.0f);
    camera.pitch = 0.5f;

    camera.Orbit(0.0f, 100.0f);
    AETHER_CHECK(camera.pitch < 1.57f && camera.pitch > 1.0f);             // looking straight down is held short of the pole
    camera.Orbit(0.0f, -100.0f);
    AETHER_CHECK(camera.pitch > -1.57f && camera.pitch < -1.0f);
    const f32 before = camera.distance;
    camera.Zoom(1.0f);
    AETHER_CHECK(camera.distance < before);                                // a wheel step in moves closer
    camera.Zoom(-1000.0f);
    AETHER_CHECK(camera.distance <= 1000.0f);                              // and far out is capped
    const Vec3 target_before = camera.target;
    camera.Pan(1.0f, 0.0f);
    AETHER_CHECK((camera.target - target_before).Length() > 0.99f);       // panning moves the target in the view plane

    // Picking: the entity under the mouse, nothing far from any, and entities without a Transform can't be picked.
    SceneDocument document;
    const Entity near_one = document.CreateEntity("Near");
    const Entity far_one = document.CreateEntity("Far");
    document.SetPosition(near_one, Vec3(0.0f, 0.0f, 0.0f), true);
    document.SetPosition(far_one, Vec3(6.0f, 0.0f, 0.0f), true);
    SceneViewCamera view;
    view.target = Vec3(3.0f, 0.0f, 0.0f);
    view.distance = 15.0f;
    const Mat4 vp2 = view.ViewProjection(size.x / size.y);
    ImVec2 p_near, p_far;
    AETHER_CHECK(ProjectToViewport(vp2, Vec3(0, 0, 0), origin, size, p_near) && ProjectToViewport(vp2, Vec3(6, 0, 0), origin, size, p_far));
    AETHER_CHECK(PickEntity(document, view, origin, size, ImVec2(p_near.x + 3.0f, p_near.y - 2.0f)) == near_one);
    AETHER_CHECK(PickEntity(document, view, origin, size, p_far) == far_one);
    AETHER_CHECK(PickEntity(document, view, origin, size, ImVec2(origin.x + 2.0f, origin.y + 2.0f)) == kNullEntity); // empty space
    AETHER_CHECK(PickEntity(document, view, origin, ImVec2(0.0f, 0.0f), p_near) == kNullEntity);                      // a collapsed viewport picks nothing
}

AETHER_TEST(SceneView_FocusAndDrawHeadless) {
    HeadlessImGui imgui;
    SceneViewPanel panel;
    SceneDocument document;
    const Entity a = document.CreateEntity("A");
    const Entity b = document.CreateEntity("B", SceneEntityKind::Camera);
    document.SetPosition(a, Vec3(10.0f, 0.0f, 0.0f), true);
    document.SetPosition(b, Vec3(20.0f, 0.0f, 0.0f), true);
    document.SetSelection({a});
    panel.FocusSelection(document);
    AETHER_CHECK(std::fabs(panel.Camera().target.x - 10.0f) < 1e-3f);     // the selection
    document.ClearSelection();
    panel.FocusSelection(document);
    AETHER_CHECK(std::fabs(panel.Camera().target.x - 15.0f) < 1e-3f);     // nothing selected: the middle of the scene
    document.SetSelection({a, b});
    auto frame = [&] { imgui.Frame([&] { ImGui::Begin("view"); panel.Draw(document); ImGui::End(); }); };
    frame();                                                               // boxes, axes, a camera and a label
    document.DestroyEntity(a);
    frame();                                                               // a selection that is gone does not break the view
}

// The move gizmo: the mouse ray, a drag along one axis as one undoable edit, snapping, and an entity under a parent.
AETHER_TEST(TransformGizmo_RayAndAxisMaths) {
    SceneViewCamera camera;
    const ImVec2 origin(0.0f, 0.0f), size(800.0f, 600.0f);
    const Mat4 vp = camera.ViewProjection(size.x / size.y);
    const Vec3 point(1.5f, 0.5f, -2.0f);
    ImVec2 on_screen;
    AETHER_CHECK(ProjectToViewport(vp, point, origin, size, on_screen));
    Vec3 ray_origin, ray_direction;
    AETHER_CHECK(MouseRay(vp, origin, size, on_screen, ray_origin, ray_direction));
    AETHER_CHECK(std::fabs(ray_direction.Length() - 1.0f) < 1e-3f);
    const Vec3 to_point = point - ray_origin;                                   // the point lies on the ray through its own pixel
    const Vec3 off_ray = to_point - ray_direction * to_point.Dot(ray_direction);
    AETHER_CHECK(off_ray.Length() < 0.02f);

    f32 t = 0.0f;                                                               // the X axis through the origin passes closest to a ray aimed at (3,0,0) at x = 3
    ImVec2 at_three;
    AETHER_CHECK(ProjectToViewport(vp, Vec3(3, 0, 0), origin, size, at_three));
    AETHER_CHECK(MouseRay(vp, origin, size, at_three, ray_origin, ray_direction));
    AETHER_CHECK(ClosestOnAxis(Vec3(0, 0, 0), Vec3(1, 0, 0), ray_origin, ray_direction, t));
    AETHER_CHECK(std::fabs(t - 3.0f) < 0.02f);
    AETHER_CHECK(!ClosestOnAxis(Vec3(0, 0, 0), Vec3(0, 0, 1), Vec3(0, 5, 0), Vec3(0, 0, 1), t)); // parallel: no answer
    AETHER_CHECK(!MouseRay(vp, origin, ImVec2(0, 0), on_screen, ray_origin, ray_direction));     // a collapsed viewport has no ray
}

AETHER_TEST(TransformGizmo_ADragIsOneUndoableMoveAlongOneAxis) {
    SceneDocument document;
    const Entity thing = document.CreateEntity("Thing");
    document.SetPosition(thing, Vec3(1.0f, 2.0f, 3.0f), true);
    document.ClearSelection();
    SceneViewCamera camera;
    camera.target = Vec3(1.0f, 2.0f, 3.0f);
    camera.distance = 12.0f;
    const ImVec2 origin(0.0f, 0.0f), size(800.0f, 600.0f);
    const Mat4 vp = camera.ViewProjection(size.x / size.y);
    TransformGizmo gizmo;
    ImVec2 handle_start;
    AETHER_CHECK(ProjectToViewport(vp, Vec3(1.0f, 2.0f, 3.0f) + Vec3(TransformGizmo::HandleLength(camera) * 0.5f, 0, 0), origin, size, handle_start));
    AETHER_CHECK(gizmo.HitTest(document, camera, origin, size, handle_start, GizmoMode::Move) == GizmoAxis::None); // nothing selected: no gizmo to hit
    document.SetSelection({thing});
    AETHER_CHECK(gizmo.HitTest(document, camera, origin, size, handle_start, GizmoMode::Move) == GizmoAxis::X);   // the middle of the X handle
    AETHER_CHECK(gizmo.HitTest(document, camera, origin, size, ImVec2(5.0f, 5.0f), GizmoMode::Move) == GizmoAxis::None);
    AETHER_CHECK(!gizmo.Begin(document, camera, origin, size, ImVec2(5.0f, 5.0f), GizmoMode::Move) && !gizmo.Dragging());
    document.Commands().Clear();                                                                  // so the undo count below is only the drag
    AETHER_CHECK(gizmo.Begin(document, camera, origin, size, handle_start, GizmoMode::Move) && gizmo.Dragging() && gizmo.Axis() == GizmoAxis::X);

    // Drag the grabbed point 4 units along world X, over several frames; the mouse path is a straight line on the screen.
    const Vec3 grab_world = Vec3(1.0f, 2.0f, 3.0f) + Vec3(TransformGizmo::HandleLength(camera) * 0.5f, 0, 0);
    ImVec2 target_pixel;
    AETHER_CHECK(ProjectToViewport(vp, grab_world + Vec3(4.0f, 0, 0), origin, size, target_pixel));
    ImVec2 mouse = handle_start;
    for (int step = 1; step <= 5; ++step) {
        mouse = ImVec2(handle_start.x + (target_pixel.x - handle_start.x) * step / 5.0f, handle_start.y + (target_pixel.y - handle_start.y) * step / 5.0f);
        AETHER_CHECK(gizmo.Update(document, camera, origin, size, mouse, 0.0f));
    }
    gizmo.End(document, camera, origin, size, mouse, 0.0f);
    AETHER_CHECK(!gizmo.Dragging());
    const Vec3 moved = WorldPositionOf(document, thing);
    AETHER_CHECK(std::fabs(moved.x - 5.0f) < 0.01f);                                             // 4 along X
    AETHER_CHECK(std::fabs(moved.y - 2.0f) < 1e-3f && std::fabs(moved.z - 3.0f) < 1e-3f);        // the other axes did not move
    AETHER_CHECK(document.Undo());                                                                // the whole drag is one undo step
    const Vec3 back = WorldPositionOf(document, thing);
    AETHER_CHECK(std::fabs(back.x - 1.0f) < 1e-3f && std::fabs(back.y - 2.0f) < 1e-3f && std::fabs(back.z - 3.0f) < 1e-3f);
    AETHER_CHECK(!document.CanUndo());
}

AETHER_TEST(TransformGizmo_SnapsAndMovesAChildInWorldSpace) {
    SceneDocument document;
    const Entity parent = document.CreateEntity("Parent");
    document.SetPosition(parent, Vec3(10.0f, 0.0f, 0.0f), true);
    const Entity child = document.CreateEntity("Child", SceneEntityKind::Empty, {}, parent);
    document.SetPosition(child, Vec3(1.0f, 0.0f, 0.0f), true); // local: the child is at world x = 11
    document.SetSelection({child});
    SceneViewCamera camera;
    camera.target = Vec3(11.0f, 0.0f, 0.0f);
    camera.distance = 12.0f;
    const ImVec2 origin(0.0f, 0.0f), size(800.0f, 600.0f);
    const Mat4 vp = camera.ViewProjection(size.x / size.y);
    TransformGizmo gizmo;
    ImVec2 grab, to;
    AETHER_CHECK(ProjectToViewport(vp, Vec3(11.0f, 0, 0) + Vec3(TransformGizmo::HandleLength(camera) * 0.5f, 0, 0), origin, size, grab));
    AETHER_CHECK(gizmo.Begin(document, camera, origin, size, grab, GizmoMode::Move));
    AETHER_CHECK(ProjectToViewport(vp, Vec3(11.0f + 2.3f + TransformGizmo::HandleLength(camera) * 0.5f, 0, 0), origin, size, to)); // dragged 2.3 along X
    AETHER_CHECK(gizmo.Update(document, camera, origin, size, to, 0.5f));
    gizmo.End(document, camera, origin, size, to, 0.5f);
    const Vec3 world = WorldPositionOf(document, child);
    AETHER_CHECK(std::fabs(world.x - 13.5f) < 0.01f);          // 11 + 2.3 = 13.3, snapped to the 0.5 grid in world space
    const Transform* local = document.GetWorld().GetComponent<Transform>(child);
    AETHER_CHECK(local && std::fabs(local->position.x - 3.5f) < 0.01f); // and stored relative to the parent at x = 10
}

namespace {

// A camera looking at the origin from a fixed place, and a viewport; the gizmo tests aim the mouse with the same projection the gizmo uses.
struct GizmoRig {
    SceneViewCamera camera;
    ImVec2 origin{0.0f, 0.0f}, size{800.0f, 600.0f};
    GizmoRig() { camera.target = Vec3(0, 0, 0); camera.distance = 12.0f; }
    ImVec2 Pixel(const Vec3& world) const {
        ImVec2 at(-1, -1);
        ProjectToViewport(camera.ViewProjection(size.x / size.y), world, origin, size, at);
        return at;
    }
    f32 Length() const { return TransformGizmo::HandleLength(camera); }
};

Vec3 WorldAxisOf(const SceneDocument& document, Entity entity, int column) {
    const Mat4 m = ComputeWorldTransform(document.GetWorld(), document.Guids(), entity);
    return Vec3(m.cols[column].x, m.cols[column].y, m.cols[column].z);
}

bool NearVec(const Vec3& a, const Vec3& b, f32 tolerance = 0.05f) { return (a - b).Length() < tolerance; }

} // namespace

AETHER_TEST(TransformGizmo_ScaleFollowsTheEntitysOwnAxes) {
    GizmoRig rig;
    SceneDocument document;
    const Entity thing = document.CreateEntity("Thing");
    document.SetRotation(thing, Quaternion::FromAxisAngle(Vec3(0, 1, 0), 3.14159265f * 0.5f), true); // local X now points along world -Z
    document.SetSelection({thing});
    document.Commands().Clear();
    const Vec3 local_x = TransformGizmo::HandleDirection(document, thing, GizmoAxis::X, GizmoMode::Scale);
    AETHER_CHECK(NearVec(local_x, Vec3(0, 0, -1)));
    AETHER_CHECK(NearVec(TransformGizmo::HandleDirection(document, thing, GizmoAxis::X, GizmoMode::Move), Vec3(1, 0, 0))); // Move stays on the world axes

    TransformGizmo gizmo;
    const f32 half = rig.Length() * 0.5f;
    const ImVec2 grab = rig.Pixel(local_x * half);
    AETHER_CHECK(gizmo.HitTest(document, rig.camera, rig.origin, rig.size, grab, GizmoMode::Scale) == GizmoAxis::X);
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, grab, GizmoMode::Scale));
    ImVec2 mouse = rig.Pixel(local_x * (half * 2.0f));                                              // twice as far out: twice the size
    AETHER_CHECK(gizmo.Update(document, rig.camera, rig.origin, rig.size, mouse, 0.0f));
    gizmo.End(document, rig.camera, rig.origin, rig.size, mouse, 0.0f);
    const Transform* t = document.GetWorld().GetComponent<Transform>(thing);
    AETHER_CHECK(t && std::fabs(t->scale.x - 2.0f) < 0.02f && std::fabs(t->scale.y - 1.0f) < 1e-3f && std::fabs(t->scale.z - 1.0f) < 1e-3f);
    AETHER_CHECK(document.Undo() && !document.CanUndo());                                           // one drag, one undo
    t = document.GetWorld().GetComponent<Transform>(thing);
    AETHER_CHECK(t && std::fabs(t->scale.x - 1.0f) < 1e-3f);

    // Snapping lands on the step; dragging through the origin never collapses or flips the scale.
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, grab, GizmoMode::Scale));
    mouse = rig.Pixel(local_x * (half * 1.7f));
    AETHER_CHECK(gizmo.Update(document, rig.camera, rig.origin, rig.size, mouse, 0.5f));
    t = document.GetWorld().GetComponent<Transform>(thing);
    AETHER_CHECK(t && std::fabs(t->scale.x - 1.5f) < 1e-3f);                                        // 1.7 snapped to the 0.5 step
    mouse = rig.Pixel(local_x * (-half * 3.0f));
    AETHER_CHECK(gizmo.Update(document, rig.camera, rig.origin, rig.size, mouse, 0.0f));
    gizmo.End(document, rig.camera, rig.origin, rig.size, mouse, 0.0f);
    t = document.GetWorld().GetComponent<Transform>(thing);
    AETHER_CHECK(t && t->scale.x > 0.0f && t->scale.x <= 0.02f);                                    // clamped just above zero
}

AETHER_TEST(TransformGizmo_RotateTurnsAboutAWorldAxisInOneUndoableStep) {
    GizmoRig rig;
    SceneDocument document;
    const Entity thing = document.CreateEntity("Thing");
    document.SetSelection({thing});
    document.Commands().Clear();
    Vec3 ring[8];
    TransformGizmo::RingPoints(Vec3(0, 0, 0), Vec3(0, 1, 0), rig.Length(), 8, ring); // 45 degree steps round the Y ring: index 1 = 45, 2 = 90, 3 = 135 ...
    TransformGizmo gizmo;
    AETHER_CHECK(gizmo.HitTest(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring[1]), GizmoMode::Rotate) == GizmoAxis::Y);
    AETHER_CHECK(gizmo.HitTest(document, rig.camera, rig.origin, rig.size, ImVec2(5, 5), GizmoMode::Rotate) == GizmoAxis::None);
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring[1]), GizmoMode::Rotate) && gizmo.Axis() == GizmoAxis::Y);
    AETHER_CHECK(gizmo.Update(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring[2]), 0.0f));
    gizmo.End(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring[3]), 0.0f);                // 45 -> 135: a quarter turn about +Y
    AETHER_CHECK(NearVec(WorldAxisOf(document, thing, 0), Vec3(0, 0, -1)));                          // X now points along -Z
    AETHER_CHECK(NearVec(WorldAxisOf(document, thing, 1), Vec3(0, 1, 0)));                           // Y is the axis, unmoved
    AETHER_CHECK(document.Undo() && !document.CanUndo());                                           // the whole drag was one undo step
    AETHER_CHECK(NearVec(WorldAxisOf(document, thing, 0), Vec3(1, 0, 0)));

    // Past the half turn: 45 -> 315 is 270 degrees one way or 90 the other, and either is the same rotation.
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring[1]), GizmoMode::Rotate));
    for (const int index : {3, 5, 7}) AETHER_CHECK(gizmo.Update(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring[index]), 0.0f));
    gizmo.End(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring[7]), 0.0f);
    AETHER_CHECK(NearVec(WorldAxisOf(document, thing, 0), Vec3(0, 0, 1)));                           // 270 degrees: X along +Z

    // Snapping rounds the turn to the step: 100 degrees dragged with a 15 degree step lands on 105.
    document.Undo();
    Vec3 fine[36];
    TransformGizmo::RingPoints(Vec3(0, 0, 0), Vec3(0, 1, 0), rig.Length(), 36, fine);                // 10 degrees apart
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, rig.Pixel(fine[5]), GizmoMode::Rotate));
    gizmo.Update(document, rig.camera, rig.origin, rig.size, rig.Pixel(fine[10]), 15.0f);
    gizmo.End(document, rig.camera, rig.origin, rig.size, rig.Pixel(fine[15]), 15.0f);               // 50 -> 150
    const f32 radians = 105.0f * 3.14159265f / 180.0f;
    AETHER_CHECK(NearVec(WorldAxisOf(document, thing, 0), Vec3(std::cos(radians), 0.0f, -std::sin(radians)), 0.02f));
}

AETHER_TEST(TransformGizmo_RotateUnderATiltedParentTurnsAboutTheWorldAxis) {
    GizmoRig rig;
    SceneDocument document;
    const Entity parent = document.CreateEntity("Parent");
    document.SetRotation(parent, Quaternion::FromAxisAngle(Vec3(0, 0, 1), 3.14159265f * 0.5f), true); // x -> y, y -> -x
    const Entity child = document.CreateEntity("Child", SceneEntityKind::Empty, {}, parent);
    document.SetSelection({child});
    document.Commands().Clear();
    AETHER_CHECK(NearVec(WorldAxisOf(document, child, 0), Vec3(0, 1, 0)));
    Vec3 ring[8];
    TransformGizmo::RingPoints(Vec3(0, 0, 0), Vec3(0, 1, 0), rig.Length(), 8, ring);
    TransformGizmo gizmo;
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring[1]), GizmoMode::Rotate));
    gizmo.Update(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring[2]), 0.0f);
    gizmo.End(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring[3]), 0.0f);                // a quarter turn about world +Y
    AETHER_CHECK(NearVec(WorldAxisOf(document, child, 0), Vec3(0, 1, 0)));                           // its X was along Y, the axis it turned about: unmoved
    AETHER_CHECK(NearVec(WorldAxisOf(document, child, 1), Vec3(0, 0, 1)));                           // its Y was along -X, which a quarter turn about +Y sends to +Z
}

// Dropping an asset from the Content Browser into the Scene View: where on the ground, which paths place, and the placement itself.
AETHER_TEST(SceneView_GroundPointAndPlacingAModel) {
    SceneViewCamera camera;
    camera.target = Vec3(0, 0, 0);
    camera.distance = 12.0f;
    const ImVec2 origin(0, 0), size(800, 600);
    Mat4 vp = camera.ViewProjection(size.x / size.y);
    ImVec2 pixel;
    AETHER_CHECK(ProjectToViewport(vp, Vec3(3.0f, 0.0f, 2.0f), origin, size, pixel));
    Vec3 ground;
    AETHER_CHECK(GroundPoint(vp, origin, size, pixel, ground));
    AETHER_CHECK(NearVec(ground, Vec3(3.0f, 0.0f, 2.0f), 0.05f));                            // the pixel a ground point projects to leads back to it
    AETHER_CHECK(GroundPoint(vp, origin, size, ImVec2(400, 590), ground) && std::fabs(ground.y) < 1e-3f);
    camera.pitch = 0.0f;                                                                      // looking along the ground: the middle of the view never meets it
    vp = camera.ViewProjection(size.x / size.y);
    AETHER_CHECK(!GroundPoint(vp, origin, size, ImVec2(400, 300), ground));
    AETHER_CHECK(!GroundPoint(vp, origin, size, ImVec2(400, 20), ground));                   // and above the horizon points away from it
    AETHER_CHECK(!GroundPoint(vp, origin, ImVec2(0, 0), ImVec2(400, 300), ground));

    AETHER_CHECK(IsPlaceableModel("Models/Rock.glb") && IsPlaceableModel("a/b.GLTF") && IsPlaceableModel("x.gltf"));
    AETHER_CHECK(!IsPlaceableModel("Textures/rock.png") && !IsPlaceableModel("Scenes/Main.ascene") && !IsPlaceableModel("noextension") && !IsPlaceableModel(""));

    SceneDocument document;
    document.Commands().Clear();
    const usize before = document.Entities().size();
    AETHER_CHECK(PlaceModelAsset(document, "Textures/rock.png", Vec3(1, 0, 1)) == kNullEntity);   // not a model: nothing happens
    AETHER_CHECK(document.Entities().size() == before && !document.CanUndo());
    const Entity rock = PlaceModelAsset(document, "Models/Rock.glb", Vec3(4.0f, 0.0f, -2.0f));
    AETHER_CHECK(rock != kNullEntity && document.Entities().size() == before + 1);
    AETHER_CHECK(document.Name(rock) == "Rock");
    const ModelRenderer* renderer = document.GetWorld().GetComponent<ModelRenderer>(rock);
    AETHER_CHECK(renderer && std::string(renderer->asset_path) == "Models/Rock.glb");
    AETHER_CHECK(NearVec(WorldPositionOf(document, rock), Vec3(4.0f, 0.0f, -2.0f), 1e-3f));
    AETHER_CHECK(document.Selection().size() == 1 && document.Selection()[0] == rock);            // the new entity is selected, ready for the gizmo
    AETHER_CHECK(document.Undo());                                                                // creation and position are one undo step
    AETHER_CHECK(document.Entities().size() == before && !document.CanUndo());
    AETHER_CHECK(document.Redo());
    AETHER_CHECK(document.Entities().size() == before + 1);
}

// Several selected entities follow one drag, as one undo step; a selected child rides with its selected parent instead of moving twice.
AETHER_TEST(TransformGizmo_SubjectsAreTheSelectedLeavingOutDescendants) {
    SceneDocument document;
    const Entity parent = document.CreateEntity("Parent");
    const Entity child = document.CreateEntity("Child", SceneEntityKind::Empty, {}, parent);
    const Entity grandchild = document.CreateEntity("Grandchild", SceneEntityKind::Empty, {}, child);
    const Entity other = document.CreateEntity("Other");
    document.SetSelection({grandchild, other, parent, child});
    const std::vector<Entity> subjects = TransformGizmo::Subjects(document);
    AETHER_CHECK(subjects.size() == 2);                                       // the parent and the unrelated one; the descendants ride along
    if (subjects.size() == 2) AETHER_CHECK(subjects[0] == other && subjects[1] == parent);   // in the selection's order
    document.SetSelection({child, other});
    AETHER_CHECK(TransformGizmo::Subjects(document).size() == 2);             // a child whose parent is NOT selected is its own subject
    document.DestroyEntity(other);
    AETHER_CHECK(TransformGizmo::Subjects(document).size() == 1);             // a destroyed one is dropped
    document.ClearSelection();
    AETHER_CHECK(TransformGizmo::Subjects(document).empty() && TransformGizmo::Subject(document) == kNullEntity);
}

AETHER_TEST(TransformGizmo_MoveScaleAndRotateTakeEveryoneSelected) {
    GizmoRig rig;
    SceneDocument document;
    const Entity a = document.CreateEntity("A"), b = document.CreateEntity("B"), c = document.CreateEntity("C");
    document.SetPosition(b, Vec3(2, 0, 0), true);
    document.SetPosition(c, Vec3(0, 0, 3), true);
    document.SetScale(b, Vec3(2, 1, 1), true);
    document.SetSelection({a, b, c});
    document.Commands().Clear();
    TransformGizmo gizmo;
    const f32 half = rig.Length() * 0.5f;

    // Move: every one shifts by the same 4 along X, in one undo step.
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, rig.Pixel(Vec3(half, 0, 0)), GizmoMode::Move));
    gizmo.Update(document, rig.camera, rig.origin, rig.size, rig.Pixel(Vec3(half + 2.0f, 0, 0)), 0.0f);
    AETHER_CHECK(!document.CanUndo());                                        // nothing is recorded while the mouse is down
    gizmo.End(document, rig.camera, rig.origin, rig.size, rig.Pixel(Vec3(half + 4.0f, 0, 0)), 0.0f);
    AETHER_CHECK(NearVec(WorldPositionOf(document, a), Vec3(4, 0, 0), 0.01f));
    AETHER_CHECK(NearVec(WorldPositionOf(document, b), Vec3(6, 0, 0), 0.01f));
    AETHER_CHECK(NearVec(WorldPositionOf(document, c), Vec3(4, 0, 3), 0.01f));
    AETHER_CHECK(document.UndoLabel() == "Move 3 entities");
    AETHER_CHECK(document.Undo() && !document.CanUndo());                     // all three come back together
    AETHER_CHECK(NearVec(WorldPositionOf(document, a), Vec3(0, 0, 0)) && NearVec(WorldPositionOf(document, b), Vec3(2, 0, 0)) && NearVec(WorldPositionOf(document, c), Vec3(0, 0, 3)));

    // Scale: one ratio for all (the first doubles; B was already 2 wide and goes to 4; Y and Z are untouched).
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, rig.Pixel(Vec3(half, 0, 0)), GizmoMode::Scale));
    gizmo.End(document, rig.camera, rig.origin, rig.size, rig.Pixel(Vec3(half * 2.0f, 0, 0)), 0.0f);
    const Transform* ta = document.GetWorld().GetComponent<Transform>(a);
    const Transform* tb = document.GetWorld().GetComponent<Transform>(b);
    AETHER_CHECK(ta && tb && std::fabs(ta->scale.x - 2.0f) < 0.02f && std::fabs(tb->scale.x - 4.0f) < 0.04f && std::fabs(tb->scale.y - 1.0f) < 1e-3f);
    AETHER_CHECK(document.Undo() && !document.CanUndo());
    tb = document.GetWorld().GetComponent<Transform>(b);
    AETHER_CHECK(tb && std::fabs(tb->scale.x - 2.0f) < 1e-3f);

    // Rotate: a quarter turn about +Y swings B and C round A's position and turns each, as one rigid group.
    Vec3 ring[8];
    TransformGizmo::RingPoints(Vec3(0, 0, 0), Vec3(0, 1, 0), rig.Length(), 8, ring);
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring[1]), GizmoMode::Rotate));
    gizmo.Update(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring[2]), 0.0f);
    gizmo.End(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring[3]), 0.0f);
    AETHER_CHECK(NearVec(WorldPositionOf(document, a), Vec3(0, 0, 0), 0.01f));              // the pivot stays
    AETHER_CHECK(NearVec(WorldPositionOf(document, b), Vec3(0, 0, -2), 0.02f));             // (2,0,0) a quarter turn about +Y is (0,0,-2)
    AETHER_CHECK(NearVec(WorldPositionOf(document, c), Vec3(3, 0, 0), 0.02f));              // (0,0,3) is (3,0,0)
    AETHER_CHECK(NearVec(WorldAxisOf(document, b, 0), Vec3(0, 0, -2), 0.05f));              // and each turned with it (B's X axis is 2 long)
    AETHER_CHECK(document.Undo() && !document.CanUndo());
    AETHER_CHECK(NearVec(WorldPositionOf(document, b), Vec3(2, 0, 0)) && NearVec(WorldPositionOf(document, c), Vec3(0, 0, 3)));
}

AETHER_TEST(TransformGizmo_AClickWithoutADragRecordsNothingAndAParentCarriesItsSelectedChild) {
    GizmoRig rig;
    SceneDocument document;
    const Entity parent = document.CreateEntity("Parent");
    const Entity child = document.CreateEntity("Child", SceneEntityKind::Empty, {}, parent);
    document.SetPosition(child, Vec3(0, 0, 2), true);
    document.SetSelection({child, parent}); // the child first: without leaving it out it would be moved against the parent's old position, then carried again
    document.Commands().Clear();
    TransformGizmo gizmo;
    const f32 half = rig.Length() * 0.5f;
    const ImVec2 grab = rig.Pixel(Vec3(half, 0, 0));
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, grab, GizmoMode::Move));
    gizmo.End(document, rig.camera, rig.origin, rig.size, grab, 0.0f);       // pressed and released on the spot
    AETHER_CHECK(!document.CanUndo() && !document.IsDirty());                // no undo entry, and the scene is not marked changed

    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, grab, GizmoMode::Move));
    gizmo.End(document, rig.camera, rig.origin, rig.size, rig.Pixel(Vec3(half + 3.0f, 0, 0)), 0.0f);
    AETHER_CHECK(NearVec(WorldPositionOf(document, parent), Vec3(3, 0, 0), 0.02f));
    AETHER_CHECK(NearVec(WorldPositionOf(document, child), Vec3(3, 0, 2), 0.02f));          // moved 3 once, not 6
    const Transform* local = document.GetWorld().GetComponent<Transform>(child);
    AETHER_CHECK(local && NearVec(local->position, Vec3(0, 0, 2), 1e-3f));                   // its own offset from the parent unchanged
}

// An untitled scene saved to the default name used to replace the project's real Main scene without a word (the Render and Live Viewport
// buttons both save first). A plain save never replaces a file that is not the open scene.
AETHER_TEST(ScenePanel_APlainSaveNeverReplacesAnotherScenesFile) {
    const stdfs::path root = Scratch("aether_scene_panel_overwrite");
    auto read = [&](const char* name) {
        std::ifstream in(root / name, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    {   // the project's real scene, made by someone else
        ScenePanel author(root);
        author.Document().CreateEntity("Real");
        AETHER_CHECK(author.SaveScene("Scenes/Main.ascene"));
    }
    const std::string real = read("Scenes/Main.ascene");
    AETHER_CHECK(real.find("Real") != std::string::npos);

    ScenePanel panel(root);                                              // untitled, with the default path Scenes/Main.ascene
    panel.Document().CreateEntity("Draft");
    AETHER_CHECK(!panel.SaveScene());                                    // refused
    AETHER_CHECK(panel.Status().find("already exists") != std::string::npos);
    AETHER_CHECK(read("Scenes/Main.ascene") == real);                    // and untouched
    AETHER_CHECK(panel.SaveScene("Scenes/Draft.ascene"));                // another name is fine
    AETHER_CHECK(read("Scenes/Main.ascene") == real && read("Scenes/Draft.ascene").find("Draft") != std::string::npos);
    AETHER_CHECK(panel.SaveScene());                                     // now the open scene is Draft: a plain save writes to it
    AETHER_CHECK(!panel.SaveScene("Scenes/Main.ascene"));                // saving as the real one is refused the first time...
    AETHER_CHECK(read("Scenes/Main.ascene") == real);
    AETHER_CHECK(panel.SaveScene("Scenes/Main.ascene"));                 // ...and replaces it when asked again by name
    AETHER_CHECK(read("Scenes/Main.ascene").find("Draft") != std::string::npos);

    ScenePanel other(root);                                              // an explicit overwrite, and opening a scene first, both work
    other.Document().CreateEntity("Third");
    AETHER_CHECK(!other.SaveScene("Scenes/Draft.ascene") && read("Scenes/Draft.ascene").find("Third") == std::string::npos);
    AETHER_CHECK(other.SaveScene("Scenes/Draft.ascene", true));
    ScenePanel opener(root);
    AETHER_CHECK(opener.OpenScene("Scenes/Main.ascene", true));
    opener.Document().CreateEntity("Added");
    AETHER_CHECK(opener.SaveScene());                                    // the scene that is open saves to its own file without ceremony
    AETHER_CHECK(read("Scenes/Main.ascene").find("Added") != std::string::npos);
}

// The orientation a camera entity needs to look the way the orbit view looks: forward along eye -> target, right side up, same right as the view matrix.
AETHER_TEST(SceneView_RotationPutsACameraEntityWhereTheViewLooks) {
    for (const auto& [yaw, pitch] : {std::pair<f32, f32>{0.0f, 0.0f}, {0.9f, 0.4f}, {-2.3f, 0.8f}, {3.0f, -0.5f}, {1.2f, 1.4f}}) {
        SceneViewCamera camera;
        camera.yaw = yaw;
        camera.pitch = pitch;
        camera.distance = 10.0f;
        camera.target = Vec3(1, 2, 3);
        Transform t;
        t.position = camera.Eye();
        t.rotation = camera.Rotation();
        const Mat4 m = LocalTransformMatrix(t);
        const Vec3 forward = Vec3(m.cols[2].x, m.cols[2].y, m.cols[2].z) * -1.0f;   // a camera looks down its own -Z
        const Vec3 wanted = (camera.target - camera.Eye()).Normalized();
        AETHER_CHECK(NearVec(forward, wanted, 1e-3f));
        AETHER_CHECK(m.cols[1].y > 0.0f);                                           // right side up
        const Mat4 view = camera.View();
        AETHER_CHECK(NearVec(Vec3(m.cols[0].x, m.cols[0].y, m.cols[0].z), Vec3(view.cols[0].x, view.cols[1].x, view.cols[2].x), 1e-3f)); // right is the view's right
    }
}

// A template project's scene uses components from several modules (SkyLight, Blueprint instances, colliders, scripts); an editor that has not
// registered them refuses to open it. (In this process other tests may have registered them already, so this proves the call works, not
// that the editor makes it; the editor's workspace does, which is checked by running the shell on such a project.)
AETHER_TEST(EditorComponents_ATemplateScenesComponentsAreAllKnown) {
    RegisterEditorComponents();
    RegisterEditorComponents(); // again: harmless
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_editor_components_test";
    stdfs::remove_all(dir);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(templates::CreateProjectFromTemplate(dir, "Demo", "third_person", &paths, &error));
    ScenePanel panel(paths.content);
    AETHER_CHECK(panel.OpenScene("Scenes/Main.ascene", true));
    AETHER_CHECK(panel.Document().Entities().size() >= 10);
}

// The document's revision: it goes up with every change a viewer should redraw for, and only then.
AETHER_TEST(SceneDocument_RevisionGoesUpWithEveryChange) {
    SceneDocument document;
    const u64 start = document.Revision();
    document.SetSelection({});
    AETHER_CHECK(document.Revision() == start);                      // selecting is not a change to the scene
    const Entity thing = document.CreateEntity("Thing");
    const u64 created = document.Revision();
    AETHER_CHECK(created > start);
    document.SetPosition(thing, Vec3(1, 2, 3), false);               // an edit still going (merged into the last command)
    const u64 moved = document.Revision();
    AETHER_CHECK(moved > created);
    document.SetPosition(thing, Vec3(4, 2, 3), false);
    AETHER_CHECK(document.Revision() > moved);                       // merged edits count too
    const u64 before_undo = document.Revision();
    AETHER_CHECK(document.Undo());
    const u64 undone = document.Revision();
    AETHER_CHECK(undone > before_undo);
    AETHER_CHECK(document.Redo());
    AETHER_CHECK(document.Revision() > undone);
    const u64 before_touch = document.Revision();
    document.Touch();
    AETHER_CHECK(document.Revision() == before_touch + 1);
    const u64 before_new = document.Revision();
    document.NewScene();
    AETHER_CHECK(document.Revision() > before_new);                  // a new scene is a change to whoever was looking at the old
}

namespace {

// A ViewportTools over an invisible item in a headless ImGui window, driven by real mouse events a frame at a time: what the Scene View and the
// Live Viewport both do with the mouse.
struct ToolRig {
    HeadlessImGui imgui;
    ViewportTools tools;
    SceneDocument document;
    SceneViewCamera camera;
    ImVec2 origin{0, 0}, size{800, 600};
    ToolRig() { camera.target = Vec3(0, 0, 0); camera.distance = 12.0f; Step(ImVec2(0, 0), false); }
    void Step(const ImVec2& mouse, bool down, bool ctrl = false) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(0, down);
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        imgui.Frame([&] {
            origin = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("view", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
            tools.HandleInput(document, camera, origin, size, ImGui::IsItemHovered(), ImGui::IsItemActive());
        });
    }
    ImVec2 Pixel(const Vec3& world) const {
        ImVec2 at(-1, -1);
        ProjectToViewport(camera.ViewProjection(size.x / size.y), world, origin, size, at);
        return at;
    }
    void Click(const ImVec2& at, bool ctrl = false) { Step(at, false, ctrl); Step(at, true, ctrl); Step(at, false, ctrl); }
};

} // namespace

AETHER_TEST(ViewportTools_AClickSelectsWhatIsUnderTheMouse) {
    ToolRig rig;
    const Entity a = rig.document.CreateEntity("A");
    const Entity b = rig.document.CreateEntity("B");
    rig.document.SetPosition(b, Vec3(4, 0, 0), true);
    rig.document.ClearSelection();
    rig.document.Commands().Clear(); // the setup above is not what is being tested
    rig.Click(rig.Pixel(Vec3(4, 0, 0)));
    AETHER_CHECK(rig.document.Selection().size() == 1 && rig.document.Selection()[0] == b);
    rig.Click(rig.Pixel(Vec3(0, 0, 0)), true);                                   // ctrl adds a second
    AETHER_CHECK(rig.document.Selection().size() == 2 && rig.document.IsSelected(a) && rig.document.IsSelected(b));
    rig.Click(rig.Pixel(Vec3(0, 0, 0)), true);                                   // and ctrl on a selected one takes it out
    AETHER_CHECK(rig.document.Selection().size() == 1 && rig.document.IsSelected(b));
    rig.Click(ImVec2(rig.origin.x + 5, rig.origin.y + 5));                       // empty space clears
    AETHER_CHECK(rig.document.Selection().empty());
    rig.Click(rig.Pixel(Vec3(0, 0, 0)));
    AETHER_CHECK(rig.document.Selection().size() == 1 && rig.document.Selection()[0] == a);
    AETHER_CHECK(!rig.document.CanUndo());                                       // selecting is not an edit
}

AETHER_TEST(ViewportTools_DraggingAHandleWithTheMouseMovesTheSelectionInOneUndoStep) {
    ToolRig rig;
    const Entity thing = rig.document.CreateEntity("Thing");
    rig.document.SetSelection({thing});
    rig.document.Commands().Clear();
    const f32 half = TransformGizmo::HandleLength(rig.camera) * 0.5f;
    const ImVec2 grab = rig.Pixel(Vec3(half, 0, 0)), target = rig.Pixel(Vec3(half + 4.0f, 0, 0));
    rig.Step(grab, false);                                                       // hover the +X handle
    rig.Step(grab, true);                                                        // press: takes hold of it, and does not select anything else
    AETHER_CHECK(rig.tools.Dragging());
    for (int i = 1; i <= 5; ++i) rig.Step(ImVec2(grab.x + (target.x - grab.x) * i / 5.0f, grab.y + (target.y - grab.y) * i / 5.0f), true);
    AETHER_CHECK(!rig.document.CanUndo());                                       // nothing recorded while the mouse is down
    AETHER_CHECK(NearVec(WorldPositionOf(rig.document, thing), Vec3(4, 0, 0), 0.05f)); // but the entity is already there
    rig.Step(target, false);                                                     // release
    AETHER_CHECK(!rig.tools.Dragging());
    AETHER_CHECK(rig.document.Selection().size() == 1 && rig.document.Selection()[0] == thing); // the drag did not count as a click
    AETHER_CHECK(rig.document.UndoLabel() == "Move Thing");
    AETHER_CHECK(rig.document.Undo() && !rig.document.CanUndo());
    AETHER_CHECK(NearVec(WorldPositionOf(rig.document, thing), Vec3(0, 0, 0), 0.01f));

    // A press and release on a handle without moving is not a click on empty space: the selection stays, and nothing is recorded.
    rig.Step(grab, false); rig.Step(grab, true); rig.Step(grab, false);
    AETHER_CHECK(rig.document.Selection().size() == 1 && rig.document.Selection()[0] == thing && !rig.document.CanUndo());

    rig.tools.SetMode(GizmoMode::Scale);                                         // the same press picks the mode's handle
    rig.tools.SetSnap(true);
    rig.Step(grab, false); rig.Step(grab, true);
    AETHER_CHECK(rig.tools.Dragging() && rig.tools.SnapStep() == 0.1f);
    rig.Step(rig.Pixel(Vec3(half * 2.0f, 0, 0)), true);
    rig.Step(rig.Pixel(Vec3(half * 2.0f, 0, 0)), false);
    const Transform* t = rig.document.GetWorld().GetComponent<Transform>(thing);
    AETHER_CHECK(t && std::fabs(t->scale.x - 2.0f) < 0.11f);                     // doubled, on the 0.1 step
}
