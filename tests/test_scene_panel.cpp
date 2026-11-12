#include "world/scene_panel.h"

#include "core/commands.h"
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
