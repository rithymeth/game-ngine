#include "world/scene_panel.h"

#include "core/commands.h"
#include "content/content_drag.h"
#include "ui/editor_theme.h"
#include "ui/shell_layout.h"
#include "world/scene_view.h"
#include "workspace/editor_workspace.h"
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
