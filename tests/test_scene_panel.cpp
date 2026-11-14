#include "world/scene_panel.h"

#include "core/commands.h"
#include "core/recent_scenes.h"
#include "content/content_drag.h"
#include "ui/builtin_components.h"
#include "ui/editor_theme.h"
#include "ui/reflected_inspector.h"
#include "ui/shell_layout.h"
#include "world/content_drop.h"
#include "world/scene_view_panel.h"
#include "world/viewport_tools.h"
#include "world/model_bounds.h"
#include "aether/assets/procedural_mesh.h"
#include "workspace/editor_workspace.h"
#include "aether/templates/templates.h"
#include "aether/renderer/components.h"
#include "aether/renderer/material_instance.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/prefab.h"
#include "test_framework.h"

#include <imgui.h>
#include <imgui_internal.h> // the drag-drop target the mouse is over

#include <algorithm>
#include <cmath>
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
            if (ImGui::BeginMenu("File")) { workspace.DrawMenuItems("File"); ImGui::EndMenu(); }
            workspace.DrawEditMenu();
            ImGui::EndMainMenuBar();
        }
    });
    AETHER_CHECK(document.Entities().size() == before + 1);
    AETHER_CHECK(workspace.Scene().DuplicateSelection() == 1);   // what the Edit menu's Duplicate calls
    AETHER_CHECK(document.CanUndo());
    imgui.Frame([&] { if (ImGui::BeginMainMenuBar()) { workspace.DrawEditMenu(); ImGui::EndMainMenuBar(); } });
}

// A recovery autosave comes back as the open scene under its own name, dirty until saved, with the file on disk untouched.
AETHER_TEST(ScenePanel_RecoveredChangesAreDirtyUntilSaved) {
    const stdfs::path root = Scratch("aether_scene_panel_recover");
    const stdfs::path source = root / "Scenes" / "Main.ascene";
    const stdfs::path autosave = root / "recovery.ascene";
    usize saved_count = 0;
    {
        SceneDocument document;
        document.CreateEntity("Saved entity");
        AETHER_CHECK(document.Save(source));
        saved_count = document.Entities().size();
        document.CreateEntity("Recovered entity");
        std::string error;
        AETHER_CHECK(document.SaveAutosave(autosave, &error));
    }
    ScenePanel panel(root);
    usize replaced = 0;
    panel.on_document_replaced = [&] { ++replaced; };
    AETHER_CHECK(panel.LoadRecovered(autosave, source));
    AETHER_CHECK(replaced == 1);
    AETHER_CHECK(panel.Document().Entities().size() == saved_count + 1);
    AETHER_CHECK(panel.Document().IsDirty());
    AETHER_CHECK(panel.Path() == "Scenes/Main.ascene"); // the path box names the scene it came from
    { SceneDocument on_disk; AETHER_CHECK(on_disk.Load(source) && on_disk.Entities().size() == saved_count); }
    // Recovering again over the recovered scene would throw that away: it is refused, as any open would be.
    AETHER_CHECK(!panel.LoadRecovered(autosave, source));
    AETHER_CHECK(panel.Document().Entities().size() == saved_count + 1);
    // Saving is what writes it over the scene the autosave came from.
    AETHER_CHECK(panel.SaveScene());
    { SceneDocument on_disk; AETHER_CHECK(on_disk.Load(source) && on_disk.Entities().size() == saved_count + 1); }
    AETHER_CHECK(!panel.Document().IsDirty());
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

// The toolbar's recents list: a newest-first, bounded list that round-trips through a file and survives a bad one.
AETHER_TEST(RecentScenes_KeepsANewestFirstListThatSurvives) {
    RecentScenes recents;
    for (int i = 0; i < 12; ++i) recents.Add("Scenes/S" + std::to_string(i) + ".ascene");
    AETHER_CHECK(recents.Entries().size() == RecentScenes::kMaxEntries);
    AETHER_CHECK(!recents.Entries().empty() && recents.Entries().front() == "Scenes/S11.ascene");
    recents.Add("Scenes/S5.ascene");                           // moved to the front, not repeated
    AETHER_CHECK(recents.Entries().size() == RecentScenes::kMaxEntries);
    AETHER_CHECK(!recents.Entries().empty() && recents.Entries().front() == "Scenes/S5.ascene");
    recents.Remove("Scenes/S5.ascene");
    AETHER_CHECK(!recents.Entries().empty() && recents.Entries().front() == "Scenes/S11.ascene");
    const stdfs::path file = Scratch("aether_recent_scenes_file") / "nested" / "recent_scenes.json";
    AETHER_CHECK(recents.Save(file));                          // makes the folder
    RecentScenes back;
    back.Load(file);
    AETHER_CHECK(back.Entries() == recents.Entries());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "{ not json";
    RecentScenes damaged;
    damaged.Add("Scenes/Kept.ascene");
    damaged.Load(file);
    AETHER_CHECK(damaged.Entries().empty());                   // a damaged file reads as an empty list
    RecentScenes missing;
    missing.Add("Scenes/Kept.ascene");
    missing.Load(file / "absent" / "nowhere.json");
    AETHER_CHECK(missing.Entries().empty());
}

// The panel records what it opens and saves, keeps one entry per scene however it is named, and prunes what is gone.
AETHER_TEST(ScenePanel_RemembersRecentlyOpenedAndSavedScenes) {
    const stdfs::path root = Scratch("aether_recent_scenes_panel");
    const stdfs::path content = root / "Content";
    const stdfs::path file = root / "Saved" / "recent_scenes.json";
    ScenePanel panel(content);
    AETHER_CHECK(panel.RecentScenePaths().empty());
    panel.SetRecentScenesFile(file);
    AETHER_CHECK(panel.SaveScene("Scenes/A.ascene"));          // saving records the scene
    AETHER_CHECK(panel.SaveScene("Scenes/B.ascene"));
    AETHER_CHECK(panel.RecentScenePaths().size() == 2 && panel.RecentScenePaths()[0] == "Scenes/B.ascene" &&
                 panel.RecentScenePaths()[1] == "Scenes/A.ascene");
    AETHER_CHECK(panel.OpenScene("Scenes/A.ascene", true));    // opening moves it to the front
    AETHER_CHECK(panel.RecentScenePaths().size() == 2 && panel.RecentScenePaths()[0] == "Scenes/A.ascene");
    AETHER_CHECK(panel.OpenScene((content / "Scenes" / "B.ascene").generic_string(), true)); // absolute form, one entry
    AETHER_CHECK(panel.RecentScenePaths().size() == 2 && panel.RecentScenePaths()[0] == "Scenes/B.ascene");
    HeadlessImGui imgui;
    imgui.Frame([&] { panel.Draw(); });                        // the toolbar's Recent list draws
    ScenePanel again(content);                                 // per project: another panel reads it back
    again.SetRecentScenesFile(file);
    AETHER_CHECK(again.RecentScenePaths() == panel.RecentScenePaths());
    stdfs::remove(content / "Scenes" / "A.ascene");
    AETHER_CHECK(panel.RemoveMissingRecentScenes() == 1);      // the list's own prune
    AETHER_CHECK(panel.RecentScenePaths().size() == 1 && panel.RecentScenePaths()[0] == "Scenes/B.ascene");
    ScenePanel pruned(content);
    pruned.SetRecentScenesFile(file);
    AETHER_CHECK(pruned.RecentScenePaths().size() == 1 && pruned.RecentScenePaths()[0] == "Scenes/B.ascene");
}

// The shell's remembered layout: it round-trips, and a missing, damaged or out-of-range file never gets in the way.
AETHER_TEST(ShellLayout_RoundTripsAndForgivesABadFile) {
    const stdfs::path dir = Scratch("aether_shell_layout");
    const stdfs::path file = dir / "nested" / "editor_layout.json";
    ShellLayout layout;
    AETHER_CHECK(!LoadShellLayout(file, layout));                       // missing: false, layout untouched
    AETHER_CHECK(layout.tool.empty());
    layout.tool = "Terrain";
    layout.layout = "Level design";
    std::string error;
    AETHER_CHECK(SaveShellLayout(file, layout, &error));                // makes the folder
    AETHER_CHECK(!stdfs::exists(stdfs::path(file).concat(".tmp")));     // the temp file did not stay
    ShellLayout back;
    AETHER_CHECK(LoadShellLayout(file, back));
    AETHER_CHECK(back.tool == "Terrain" && back.layout == "Level design");
    auto write = [&](const std::string& text) { std::ofstream(file, std::ios::binary | std::ios::trunc) << text; };
    write("{ this is not json");
    ShellLayout damaged;
    damaged.tool = "kept";
    AETHER_CHECK(!LoadShellLayout(file, damaged) && damaged.tool == "kept");
    write(R"({"side_fraction": 0.4, "tool": "Scene"})");                // a file from before docking: the old key is ignored, the tool survives
    ShellLayout older;
    AETHER_CHECK(LoadShellLayout(file, older) && older.tool == "Scene" && older.layout.empty());
    write(R"({"tool": 7, "layout": 4})");                               // a wrong type is ignored, not fatal
    ShellLayout typed;
    AETHER_CHECK(LoadShellLayout(file, typed) && typed.tool.empty() && typed.layout.empty());
    write("{\"tool\": \"" + std::string(500, 'x') + "\"}");             // an absurd name is dropped
    ShellLayout long_name;
    AETHER_CHECK(LoadShellLayout(file, long_name) && long_name.tool.empty());
    write("{\"layout\": \"" + std::string(500, 'x') + "\"}");           // so is an absurd layout name
    ShellLayout long_layout;
    AETHER_CHECK(LoadShellLayout(file, long_layout) && long_layout.layout.empty());
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

AETHER_TEST(TransformGizmo_LocalSpaceMovesAndRotatesAlongTheEntitysOwnAxes) {
    GizmoRig rig;
    SceneDocument document;
    const Entity thing = document.CreateEntity("Thing");
    document.SetRotation(thing, Quaternion::FromAxisAngle(Vec3(0, 1, 0), 3.14159265f * 0.5f), true); // local X now points along world -Z
    document.SetSelection({thing});
    document.Commands().Clear();
    const Vec3 local_x = Vec3(0, 0, -1);
    AETHER_CHECK(NearVec(TransformGizmo::HandleDirection(document, thing, GizmoAxis::X, GizmoMode::Move, GizmoSpace::Local), local_x));
    AETHER_CHECK(NearVec(TransformGizmo::HandleDirection(document, thing, GizmoAxis::X, GizmoMode::Rotate, GizmoSpace::Local), local_x));
    AETHER_CHECK(NearVec(TransformGizmo::HandleDirection(document, thing, GizmoAxis::X, GizmoMode::Move, GizmoSpace::World), Vec3(1, 0, 0))); // world is still the default

    TransformGizmo gizmo;
    const f32 half = rig.Length() * 0.5f;
    const ImVec2 grab = rig.Pixel(local_x * half);
    // The arrow sits on the entity's own axis in Local space, and not in World space.
    AETHER_CHECK(gizmo.HitTest(document, rig.camera, rig.origin, rig.size, grab, GizmoMode::Move, GizmoSpace::Local) == GizmoAxis::X);
    AETHER_CHECK(gizmo.HitTest(document, rig.camera, rig.origin, rig.size, grab, GizmoMode::Move, GizmoSpace::World) != GizmoAxis::X);
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, grab, GizmoMode::Move, GizmoSpace::Local));
    const ImVec2 mouse = rig.Pixel(local_x * (half * 2.0f));
    AETHER_CHECK(gizmo.Update(document, rig.camera, rig.origin, rig.size, mouse, 0.0f));
    gizmo.End(document, rig.camera, rig.origin, rig.size, mouse, 0.0f);
    AETHER_CHECK(NearVec(WorldPositionOf(document, thing), local_x * half, 0.05f)); // moved by `half` along its own X, which is world -Z
    AETHER_CHECK(document.Undo() && !document.CanUndo());                           // still one drag, one undo

    // Snapping measures the distance moved along the entity's own axis, since the world grid does not line up with it.
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, grab, GizmoMode::Move, GizmoSpace::Local));
    AETHER_CHECK(gizmo.Update(document, rig.camera, rig.origin, rig.size, rig.Pixel(local_x * (half + 0.9f)), 1.0f));
    AETHER_CHECK(NearVec(WorldPositionOf(document, thing), local_x * 1.0f, 0.02f));
    gizmo.End(document, rig.camera, rig.origin, rig.size, rig.Pixel(local_x * (half + 0.9f)), 1.0f);
    AETHER_CHECK(document.Undo());

    // Rotate about the entity's own X: a quarter turn about it, and the rotation about world X is untouched.
    const f32 radius = rig.Length();
    const Vec3 ring_start = Vec3(0, 1, 0) * radius, ring_end = Vec3(1, 0, 0) * radius; // a quarter of the way round the ring about local X (it lies in the world XY plane)
    AETHER_CHECK(gizmo.HitTest(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring_start), GizmoMode::Rotate, GizmoSpace::Local) == GizmoAxis::X);
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring_start), GizmoMode::Rotate, GizmoSpace::Local));
    AETHER_CHECK(gizmo.Update(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring_end), 0.0f));
    gizmo.End(document, rig.camera, rig.origin, rig.size, rig.Pixel(ring_end), 0.0f);
    const Vec3 up_after = WorldAxisOf(document, thing, 1);
    AETHER_CHECK(std::fabs(up_after.y) < 0.1f && std::fabs(up_after.x) > 0.9f); // local Y lay along world Y and now lies along world X: it turned about Z
    AETHER_CHECK(NearVec(WorldAxisOf(document, thing, 0), local_x, 0.1f));                       // the axis it turned about did not move
}

AETHER_TEST(TransformGizmo_PlaneSquaresSlideInTwoAxesAtOnce) {
    GizmoRig rig;
    SceneDocument document;
    const Entity thing = document.CreateEntity("Thing");
    document.SetSelection({thing});
    document.Commands().Clear();
    TransformGizmo gizmo;
    const f32 centre = rig.Length() * 0.375f; // the middle of the square, which runs from a quarter to a half of the handle
    const ImVec2 grab = rig.Pixel(Vec3(centre, 0, centre));
    AETHER_CHECK(gizmo.HitTest(document, rig.camera, rig.origin, rig.size, grab, GizmoMode::Move) == GizmoAxis::PlaneXZ);
    AETHER_CHECK(gizmo.HitTest(document, rig.camera, rig.origin, rig.size, grab, GizmoMode::Scale) != GizmoAxis::PlaneXZ); // Move only
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, grab, GizmoMode::Move));
    ImVec2 mouse = rig.Pixel(Vec3(centre + 2.0f, 0, centre + 3.0f));
    AETHER_CHECK(gizmo.Update(document, rig.camera, rig.origin, rig.size, mouse, 0.0f));
    gizmo.End(document, rig.camera, rig.origin, rig.size, mouse, 0.0f);
    AETHER_CHECK(NearVec(WorldPositionOf(document, thing), Vec3(2, 0, 3), 0.05f)); // both axes at once, and not off the plane
    AETHER_CHECK(document.Undo() && !document.CanUndo());                         // one drag, one undo

    // Snapping rounds each of the two axes on its own.
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, grab, GizmoMode::Move));
    mouse = rig.Pixel(Vec3(centre + 1.4f, 0, centre + 2.6f));
    AETHER_CHECK(gizmo.Update(document, rig.camera, rig.origin, rig.size, mouse, 1.0f));
    AETHER_CHECK(NearVec(WorldPositionOf(document, thing), Vec3(1, 0, 3), 0.02f));
    gizmo.End(document, rig.camera, rig.origin, rig.size, mouse, 1.0f);
    AETHER_CHECK(document.Undo());

    // In Local space the squares sit on the entity's own axes: tipped a quarter turn about X, its XY square lies where the world's XZ one did.
    document.SetRotation(thing, Quaternion::FromAxisAngle(Vec3(1, 0, 0), 3.14159265f * 0.5f), true);
    document.Commands().Clear();
    // (Off the middle, where the entity's own Z arrow, pointing down, happens to be drawn across the square from this camera.)
    const Vec3 corner_ish(rig.Length() * 0.47f, 0, rig.Length() * 0.28f);
    const ImVec2 local_grab = rig.Pixel(corner_ish);
    AETHER_CHECK(gizmo.HitTest(document, rig.camera, rig.origin, rig.size, local_grab, GizmoMode::Move, GizmoSpace::Local) == GizmoAxis::PlaneXY);
    AETHER_CHECK(gizmo.Begin(document, rig.camera, rig.origin, rig.size, local_grab, GizmoMode::Move, GizmoSpace::Local));
    mouse = rig.Pixel(corner_ish + Vec3(2.0f, 0, 3.0f));
    AETHER_CHECK(gizmo.Update(document, rig.camera, rig.origin, rig.size, mouse, 0.0f));
    gizmo.End(document, rig.camera, rig.origin, rig.size, mouse, 0.0f);
    AETHER_CHECK(NearVec(WorldPositionOf(document, thing), Vec3(2, 0, 3), 0.05f));
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

namespace {

// A .glb of an axis-aligned box from `low` to `high`, written under `content`/`name`.
void WriteBoxModel(const stdfs::path& content, const std::string& name, const Vec3& low, const Vec3& high) {
    assets::procedural::MeshBuilder builder;
    builder.Box(low, high);
    assets::procedural::ProceduralMesh mesh;
    mesh.materials.push_back(assets::MaterialData{});
    mesh.mesh.primitives.push_back(builder.Finish(assets::procedural::Shading::Flat, assets::procedural::UvMode::BoxProjected, 0));
    const std::vector<u8> glb = assets::procedural::EncodeGlb(mesh);
    stdfs::create_directories((content / name).parent_path());
    std::ofstream(content / name, std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
}

} // namespace

AETHER_TEST(ModelBounds_AModelsRealExtentIsReadOnceAndForgottenOnRequest) {
    const stdfs::path content = Scratch("aether_model_bounds");
    WriteBoxModel(content, "Models/crate.glb", Vec3(-1, 0, -2), Vec3(3, 4, 1));
    ModelBoundsCache cache(content);
    LocalBounds box;
    AETHER_CHECK(cache.Get("Models/crate.glb", box));
    AETHER_CHECK(NearVec(box.min, Vec3(-1, 0, -2), 1e-3f) && NearVec(box.max, Vec3(3, 4, 1), 1e-3f));
    AETHER_CHECK(!cache.Get("Models/missing.glb", box) && !cache.Get("", box));       // not there: no box (and the answer is remembered)
    AETHER_CHECK(cache.CachedCount() == 2);
    WriteBoxModel(content, "Models/crate.glb", Vec3(-5, 0, -5), Vec3(5, 9, 5));       // changed on disk: the old box stays until it is asked to forget
    AETHER_CHECK(cache.Get("Models/crate.glb", box) && NearVec(box.max, Vec3(3, 4, 1), 1e-3f));
    cache.Clear();
    AETHER_CHECK(cache.Get("Models/crate.glb", box) && NearVec(box.max, Vec3(5, 9, 5), 1e-3f));
    cache.SetContentRoot(Scratch("aether_model_bounds_other"));                        // another project: nothing carries over
    AETHER_CHECK(!cache.Get("Models/crate.glb", box) && cache.CachedCount() == 1);
    ModelBoundsCache none;                                                              // no content root at all
    AETHER_CHECK(!none.Get("Models/crate.glb", box));
}

AETHER_TEST(ModelBounds_APickedEntityIsFoundWhereverItsModelIsAndTheNearerOneWins) {
    const stdfs::path content = Scratch("aether_model_pick");
    WriteBoxModel(content, "Models/slab.glb", Vec3(-5, 0, -5), Vec3(5, 1, 5));      // a big, flat model
    WriteBoxModel(content, "Models/crate.glb", Vec3(-0.5f, 0, -0.5f), Vec3(0.5f, 1, 0.5f));
    ModelBoundsCache cache(content);
    SceneDocument document;
    const Entity slab = document.CreateEntity("Slab", SceneEntityKind::Model, "Models/slab.glb");
    const Entity crate = document.CreateEntity("Crate", SceneEntityKind::Model, "Models/crate.glb");
    const Entity camera_entity = document.CreateEntity("Camera", SceneEntityKind::Camera);
    document.SetPosition(crate, Vec3(0, 1, 0), true);                                 // on top of the slab, at its middle
    document.SetPosition(camera_entity, Vec3(8, 6, 8), true);
    SceneViewCamera camera;
    camera.target = Vec3(0, 0, 0);
    camera.distance = 22.0f;
    const ImVec2 origin(0, 0), size(800, 600);
    const Mat4 vp = camera.ViewProjection(size.x / size.y);
    auto pixel = [&](const Vec3& world) { ImVec2 at(-1, -1); ProjectToViewport(vp, world, origin, size, at); return at; };

    const ImVec2 far_corner = pixel(Vec3(4, 0.5f, 4));                                // on the slab, nowhere near its origin (a long way in pixels)
    AETHER_CHECK(PickEntity(document, camera, origin, size, far_corner) == kNullEntity);          // without bounds: only near an origin counts
    AETHER_CHECK(PickEntity(document, camera, origin, size, far_corner, 14.0f, &cache) == slab);  // with them: anywhere on the model
    AETHER_CHECK(PickEntity(document, camera, origin, size, pixel(Vec3(0, 1.5f, 0)), 14.0f, &cache) == crate); // the crate sits in front of the slab behind it
    AETHER_CHECK(PickEntity(document, camera, origin, size, pixel(Vec3(0, 0.5f, 0)), 14.0f, &cache) != kNullEntity);
    AETHER_CHECK(PickEntity(document, camera, origin, size, pixel(Vec3(8, 6, 8)), 14.0f, &cache) == camera_entity);   // no model: still found by its origin
    AETHER_CHECK(PickEntity(document, camera, origin, size, ImVec2(5, 5), 14.0f, &cache) == kNullEntity);             // empty space
}

AETHER_TEST(ModelBounds_AnEntitysBoxFollowsItsScaleAndRotation) {
    SceneDocument document;
    const Entity thing = document.CreateEntity("Thing");                                // no model: a unit box
    LocalBounds box = EntityBounds(document, thing, nullptr);
    AETHER_CHECK(NearVec(box.min, Vec3(-0.5f, -0.5f, -0.5f)) && NearVec(box.max, Vec3(0.5f, 0.5f, 0.5f)));
    const Vec3 up(0, 1, 0);
    f32 distance = 0.0f;
    const Vec3 along_z(0, 0, -1);
    auto hits_at = [&](f32 x) { return RayHitsEntityBox(document, thing, EntityBounds(document, thing, nullptr), Vec3(x, 0, 10), along_z, distance); };
    AETHER_CHECK(hits_at(0.4f) && !hits_at(0.6f));                                      // a unit box is 1 wide
    document.SetScale(thing, Vec3(4, 1, 1), true);
    AETHER_CHECK(hits_at(1.9f) && !hits_at(2.1f));                                      // scaled 4 wide
    AETHER_CHECK(std::fabs(distance - 9.5f) < 1e-3f);                                   // the near face, 9.5 along the ray from z = 10
    document.SetRotation(thing, Quaternion::FromAxisAngle(up, 3.14159265f * 0.5f), true);
    AETHER_CHECK(!hits_at(1.9f) && hits_at(0.4f));                                      // turned a quarter: 4 deep instead, 1 wide in x
    AETHER_CHECK(!RayHitsEntityBox(document, thing, EntityBounds(document, thing, nullptr), Vec3(0, 0, 10), Vec3(0, 0, 1), distance)); // pointing away from it
}

AETHER_TEST(PrefabPlacement_ADroppedPrefabIsOneUndoableInstanceLinkedToTheAsset) {
    const stdfs::path content = Scratch("aether_prefab_place");
    PrefabData crate; // a crate with a lid under it
    PrefabEntity root_entity;
    root_entity.id = 1;
    EntityName crate_name;
    crate_name.value = "Crate";
    const std::string name_key = GetComponentInfo(GetComponentId<EntityName>()).reflected->name; // how a prefab file names the component
    root_entity.components[name_key] = reflect::ToJson(crate_name);
    PrefabEntity lid;
    lid.id = 2;
    lid.parent = 1;
    EntityName lid_name;
    lid_name.value = "Lid";
    lid.components[name_key] = reflect::ToJson(lid_name);
    crate.entities = {root_entity, lid};
    stdfs::create_directories(content / "Prefabs");
    AETHER_CHECK(SavePrefab(crate, content / "Prefabs" / "Crate.aprefab"));

    AETHER_CHECK(IsPlaceablePrefab("Prefabs/Crate.aprefab") && IsPlaceablePrefab("A/B.APREFAB") && !IsPlaceablePrefab("Models/crate.glb") && !IsPlaceablePrefab("aprefab"));

    SceneDocument document;
    document.Commands().Clear();
    const usize before = document.Entities().size();
    std::string error;
    const Entity placed = PlacePrefabAsset(document, content, "Prefabs/Crate.aprefab", Vec3(4, 0, 2), &error);
    AETHER_CHECK(placed != kNullEntity && error.empty());
    if (placed == kNullEntity) return;
    AETHER_CHECK(document.Entities().size() == before + 2);                       // the root and the lid
    AETHER_CHECK(NearVec(WorldPositionOf(document, placed), Vec3(4, 0, 2)));
    AETHER_CHECK(document.Selection().size() == 1 && document.Selection()[0] == placed);
    const PrefabInstance* instance = document.GetWorld().GetComponent<PrefabInstance>(placed);
    AETHER_CHECK(instance != nullptr && !instance->source.guid.IsNull());          // still tied to the asset, so a change to it reaches this
    Entity lid_entity = kNullEntity;
    for (const Entity entity : document.Entities()) {
        if (document.Name(entity) == "Lid") lid_entity = entity;
    }
    AETHER_CHECK(lid_entity != kNullEntity);
    if (lid_entity == kNullEntity) return;
    const Parent* parent = document.GetWorld().GetComponent<Parent>(lid_entity);
    AETHER_CHECK(parent != nullptr && parent->parent == document.GetWorld().GetComponent<IdComponent>(placed)->guid);
    AETHER_CHECK(document.GetWorld().HasComponent<PrefabLink>(lid_entity));

    // One drop, one undo: both entities go, and redo brings both back, the lid still under its crate.
    AETHER_CHECK(document.Undo() && !document.CanUndo());
    AETHER_CHECK(document.Entities().size() == before);
    AETHER_CHECK(document.Redo() && document.Entities().size() == before + 2);
    Entity lid_again = kNullEntity;
    for (const Entity entity : document.Entities()) {
        if (document.Name(entity) == "Lid") lid_again = entity;
    }
    AETHER_CHECK(lid_again != kNullEntity && document.GetWorld().GetComponent<Parent>(lid_again) != nullptr);

    // Something that is not a prefab, or not there, changes nothing and says why.
    const usize now = document.Entities().size();
    AETHER_CHECK(PlacePrefabAsset(document, content, "Models/crate.glb", Vec3(0, 0, 0), &error) == kNullEntity && !error.empty());
    error.clear();
    AETHER_CHECK(PlacePrefabAsset(document, content, "Prefabs/Missing.aprefab", Vec3(0, 0, 0), &error) == kNullEntity && !error.empty());
    AETHER_CHECK(document.Entities().size() == now);
}

// The drop both 3D views share (the Scene View and the Live Viewport): where an asset lands, and what placing it does.
AETHER_TEST(ContentDrop_AnAssetLandsOnTheGroundUnderTheMouseOrAtTheCamerasTarget) {
    SceneViewCamera camera;
    camera.target = Vec3(1, 0, -2);
    camera.distance = 12.0f;
    const ImVec2 origin(0, 0), size(800, 600);
    ImVec2 pixel;
    AETHER_CHECK(ProjectToViewport(camera.ViewProjection(size.x / size.y), Vec3(3.0f, 0.0f, 2.0f), origin, size, pixel));
    AETHER_CHECK(NearVec(DropPoint(camera, origin, size, pixel, 0.0f), Vec3(3.0f, 0.0f, 2.0f), 0.05f));   // the pixel a ground point projects to leads back to it
    AETHER_CHECK(NearVec(DropPoint(camera, origin, size, pixel, 2.0f), Vec3(4.0f, 0.0f, 2.0f), 0.01f));   // rounded to the step (x: 3 to 4 on a step of 2, z stays)
    camera.pitch = 0.0f;                                                                                   // looking along the ground: the middle never meets it
    AETHER_CHECK(NearVec(DropPoint(camera, origin, size, ImVec2(400, 300), 0.0f), camera.target, 1e-4f));
    AETHER_CHECK(NearVec(DropPoint(camera, origin, ImVec2(0, 0), ImVec2(400, 300), 0.0f), camera.target, 1e-4f)); // a view with no size
}

AETHER_TEST(ContentDrop_PlacingAModelOrAPrefabSaysWhatHappened) {
    const stdfs::path content = Scratch("aether_content_drop");
    WriteBoxModel(content, "Models/crate.glb", Vec3(-1, 0, -1), Vec3(1, 2, 1));
    PrefabData prefab;
    PrefabEntity top;
    top.id = 1;
    prefab.entities = {top};
    stdfs::create_directories(content / "Prefabs");
    AETHER_CHECK(SavePrefab(prefab, content / "Prefabs" / "Thing.aprefab"));

    SceneDocument document;
    document.Commands().Clear();
    const usize before = document.Entities().size();
    bool placed = true;
    // A model: one entity, where it was dropped, selected, and it says so.
    std::string said = PlaceContentAsset(document, content, "Models/crate.glb", Vec3(4, 0, 1), &placed);
    AETHER_CHECK(placed && said == "Placed Models/crate.glb" && document.Entities().size() == before + 1);
    AETHER_CHECK(document.Selection().size() == 1 && NearVec(WorldPositionOf(document, document.Selection()[0]), Vec3(4, 0, 1)));
    // A prefab: an instance, and it says so.
    said = PlaceContentAsset(document, content, "Prefabs/Thing.aprefab", Vec3(0, 0, 5), &placed);
    AETHER_CHECK(placed && said == "Placed Prefabs/Thing.aprefab" && document.Entities().size() == before + 2);
    // Each was one undo step.
    AETHER_CHECK(document.Undo() && document.Undo() && !document.CanUndo() && document.Entities().size() == before);

    // What cannot be placed changes nothing, says why, and reports not placed.
    placed = true;
    said = PlaceContentAsset(document, content, "Textures/brick.png", Vec3(0, 0, 0), &placed);
    AETHER_CHECK(!placed && said.find("Only models") == 0);
    said = PlaceContentAsset(document, content, "Prefabs/Missing.aprefab", Vec3(0, 0, 0), &placed);
    AETHER_CHECK(!placed && !said.empty() && said.find("Placed") != 0);
    said = PlaceContentAsset(document, stdfs::path(), "Prefabs/Thing.aprefab", Vec3(0, 0, 0), &placed);   // no content folder to read it from
    AETHER_CHECK(!placed && said == "Could not place Prefabs/Thing.aprefab");
    AETHER_CHECK(document.Entities().size() == before && !document.CanUndo());
}

AETHER_TEST(ContentDrop_APlacedAssetCanGoUnderAnEntityInOneStep) {
    const stdfs::path content = Scratch("aether_content_drop_parent");
    WriteBoxModel(content, "Models/crate.glb", Vec3(-1, 0, -1), Vec3(1, 2, 1));
    PrefabData prefab;
    PrefabEntity top;
    top.id = 1;
    PrefabEntity part;
    part.id = 2;
    part.parent = 1;
    prefab.entities = {top, part};
    stdfs::create_directories(content / "Prefabs");
    AETHER_CHECK(SavePrefab(prefab, content / "Prefabs" / "Thing.aprefab"));

    SceneDocument document;
    const Entity holder = document.CreateEntity("Holder");
    document.SetPosition(holder, Vec3(5, 0, 0), true);
    document.Commands().Clear();
    const usize before = document.Entities().size();
    const EntityGuid holder_guid = document.GetWorld().GetComponent<IdComponent>(holder)->guid;

    // A model dropped on the entity is its child, at its origin (the position is relative to it), and it is one create, not a create and a reparent.
    bool placed = false;
    PlaceContentAsset(document, content, "Models/crate.glb", Vec3(0, 0, 0), &placed, holder);
    AETHER_CHECK(placed && document.Entities().size() == before + 1);
    const Entity model = document.Selection().empty() ? kNullEntity : document.Selection()[0];
    AETHER_CHECK(model != kNullEntity);
    if (model == kNullEntity) return;
    const Parent* link = document.GetWorld().GetComponent<Parent>(model);
    AETHER_CHECK(link != nullptr && link->parent == holder_guid);
    AETHER_CHECK(NearVec(WorldPositionOf(document, model), Vec3(5, 0, 0)));
    // A prefab too: its root is under the entity and its own child stays under the root.
    PlaceContentAsset(document, content, "Prefabs/Thing.aprefab", Vec3(0, 1, 0), &placed, holder);
    AETHER_CHECK(placed && document.Entities().size() == before + 3);
    const Entity root = document.Selection().empty() ? kNullEntity : document.Selection()[0];
    AETHER_CHECK(root != kNullEntity);
    if (root == kNullEntity) return;
    const Parent* root_link = document.GetWorld().GetComponent<Parent>(root);
    AETHER_CHECK(root_link != nullptr && root_link->parent == holder_guid);
    AETHER_CHECK(NearVec(WorldPositionOf(document, root), Vec3(5, 1, 0)));
    Entity child = kNullEntity;
    for (const Entity entity : document.Entities()) {
        const Parent* p = document.GetWorld().GetComponent<Parent>(entity);
        if (entity != model && entity != root && p != nullptr && p->parent == document.GetWorld().GetComponent<IdComponent>(root)->guid) child = entity;
    }
    AETHER_CHECK(child != kNullEntity);
    // Each is one undo step, and redo brings the parent link back with it.
    AETHER_CHECK(document.Undo() && document.Undo() && !document.CanUndo() && document.Entities().size() == before);
    AETHER_CHECK(document.Redo() && document.Redo() && document.Entities().size() == before + 3);
    bool still_under = false;
    for (const Entity entity : document.Entities()) {
        const Parent* p = document.GetWorld().GetComponent<Parent>(entity);
        if (p != nullptr && p->parent == holder_guid) still_under = true;
    }
    AETHER_CHECK(still_under);

    // A parent that is not alive is ignored: the object is placed at the root, where it was told to go.
    Entity gone = document.CreateEntity("Gone");
    document.DestroyEntity(gone);
    const usize now = document.Entities().size();
    PlaceContentAsset(document, content, "Models/crate.glb", Vec3(2, 0, 0), &placed, gone);
    AETHER_CHECK(placed && document.Entities().size() == now + 1);
    const Entity orphan = document.Selection().empty() ? kNullEntity : document.Selection()[0];
    AETHER_CHECK(orphan != kNullEntity && document.GetWorld().GetComponent<Parent>(orphan) == nullptr && NearVec(WorldPositionOf(document, orphan), Vec3(2, 0, 0)));
}

namespace {

// A drag from one item to another through real mouse events, a frame at a time: a button that drags `path` the way the Content Browser's rows do, and a view
// that takes the drop the way the Scene View and the Live Viewport do (ContentDrop::Accept).
struct DropRig {
    HeadlessImGui imgui;
    SceneDocument document;
    SceneViewCamera camera;
    ContentDrop drop;
    stdfs::path content;
    std::string path;
    f32 snap_step = 0.0f;
    ImVec2 origin{0, 0}, size{800, 600}, source_at{20, 20};
    DropRig() { camera.target = Vec3(0, 0, 0); camera.distance = 12.0f; Step(ImVec2(0, 0), false); }
    void Step(const ImVec2& mouse, bool down) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(0, down);
        imgui.Frame([&] {
            ImGui::SetCursorScreenPos(ImVec2(10, 10));
            ImGui::Button("row", ImVec2(120, 24));
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload(kContentPathPayload, path.c_str(), path.size() + 1);
                ImGui::TextUnformatted(path.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::SetCursorScreenPos(ImVec2(200, 60));
            origin = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("view", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
            drop.Accept(document, content, camera, origin, size, snap_step);
        });
    }
    ImVec2 Pixel(const Vec3& world) const {
        ImVec2 at(-1, -1);
        ProjectToViewport(camera.ViewProjection(size.x / size.y), world, origin, size, at);
        return at;
    }
    // Presses on the row and carries the mouse to `to` in steps, still holding; the caller releases.
    void DragTo(const ImVec2& to) {
        Step(source_at, false);
        Step(source_at, true);
        Step(ImVec2(source_at.x + 30.0f, source_at.y), true);
        for (int i = 1; i <= 6; ++i) Step(ImVec2(source_at.x + 30.0f + (to.x - source_at.x - 30.0f) * i / 6.0f, source_at.y + (to.y - source_at.y) * i / 6.0f), true);
    }
};

} // namespace

AETHER_TEST(ContentDrop_ARealDragFromARowIsPlacedWhereTheMouseIsReleased) {
    DropRig rig;
    rig.content = Scratch("aether_content_drop_drag");
    WriteBoxModel(rig.content, "Models/crate.glb", Vec3(-1, 0, -1), Vec3(1, 2, 1));
    rig.path = "Models/crate.glb";
    rig.document.Commands().Clear();
    const usize before = rig.document.Entities().size();
    rig.Step(ImVec2(0, 0), false);
    const ImVec2 over_ground = rig.Pixel(Vec3(3.0f, 0.0f, 2.0f));

    // Mid-drag, over the view: nothing is placed yet, and a marker shows where it would land.
    rig.DragTo(over_ground);
    AETHER_CHECK(rig.drop.preview && NearVec(rig.drop.point, Vec3(3.0f, 0.0f, 2.0f), 0.05f));
    AETHER_CHECK(rig.document.Entities().size() == before && rig.drop.note.empty());
    // Letting go places it there, selected, in one undo step, and says so.
    rig.Step(over_ground, false);
    AETHER_CHECK(rig.document.Entities().size() == before + 1 && rig.drop.note == "Placed Models/crate.glb");
    AETHER_CHECK(rig.document.Selection().size() == 1 && NearVec(WorldPositionOf(rig.document, rig.document.Selection()[0]), Vec3(3.0f, 0.0f, 2.0f), 0.05f));
    rig.Step(over_ground, false);
    AETHER_CHECK(!rig.drop.preview);                                         // the marker goes when the drag does
    AETHER_CHECK(rig.document.Undo() && !rig.document.CanUndo() && rig.document.Entities().size() == before);

    // Snapping rounds where it lands to the step.
    rig.snap_step = 2.0f;
    const ImVec2 near_four = rig.Pixel(Vec3(3.4f, 0.0f, 2.2f)); // not on the halfway point, where either neighbour is as good
    rig.DragTo(near_four);
    rig.Step(near_four, false);
    AETHER_CHECK(rig.document.Entities().size() == before + 1);
    AETHER_CHECK(NearVec(WorldPositionOf(rig.document, rig.document.Selection()[0]), Vec3(4.0f, 0.0f, 2.0f), 0.05f));
    rig.snap_step = 0.0f;

    // Letting go somewhere that is not the view places nothing.
    const usize now = rig.document.Entities().size();
    rig.DragTo(ImVec2(100.0f, 400.0f));
    AETHER_CHECK(!rig.drop.preview);
    rig.Step(ImVec2(100.0f, 400.0f), false);
    AETHER_CHECK(rig.document.Entities().size() == now);

    // Something that is not a model or a prefab shows no marker and is refused with a reason.
    rig.path = "Textures/brick.png";
    rig.DragTo(over_ground);
    AETHER_CHECK(!rig.drop.preview);
    rig.Step(over_ground, false);
    AETHER_CHECK(rig.document.Entities().size() == now && rig.drop.note.find("Only models") == 0);
}

namespace {

// The same real drag, onto the Scene tool's hierarchy: a row that drags `path`, and the whole panel drawn below it.
struct HierarchyDropRig {
    HeadlessImGui imgui;
    ScenePanel panel;
    std::string path = "Models/crate.glb";
    Entity drag_entity = kNullEntity; // when set, the row drags this entity (as a hierarchy row does) instead of an asset path
    ImVec2 source_at{20, 20};
    explicit HierarchyDropRig(const stdfs::path& content) : panel(content) { Step(ImVec2(0, 0), false); }
    void Step(const ImVec2& mouse, bool down) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(0, down);
        imgui.Frame([&] {
            ImGui::SetCursorScreenPos(ImVec2(10, 10));
            ImGui::Button("row", ImVec2(120, 24));
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                if (drag_entity != kNullEntity) ImGui::SetDragDropPayload("AETHER_ENTITY", &drag_entity, sizeof(Entity));
                else ImGui::SetDragDropPayload(kContentPathPayload, path.c_str(), path.size() + 1);
                ImGui::TextUnformatted(path.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::SetCursorScreenPos(ImVec2(10, 50));
            panel.Draw();
        });
    }
    // Presses on the row and carries the mouse down the hierarchy at x = 80, still holding: the y of the middle of each place a drop would be taken, top to bottom, one per target rectangle.
    std::vector<f32> Targets() {
        Step(source_at, false);
        Step(source_at, true);
        Step(ImVec2(source_at.x + 30.0f, source_at.y), true);
        std::vector<f32> found;
        f32 last_top = -1.0f;
        for (f32 y = 60.0f; y < 330.0f; y += 3.0f) {
            Step(ImVec2(80.0f, y), true);
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            if (g.DragDropTargetId == 0 || g.DragDropTargetRect.Min.y == last_top) continue;
            last_top = g.DragDropTargetRect.Min.y; // a new rectangle: the middle of it is where to let go
            found.push_back((g.DragDropTargetRect.Min.y + g.DragDropTargetRect.Max.y) * 0.5f);
        }
        return found;
    }
};

} // namespace

AETHER_TEST(ContentDrop_ARealDragOntoTheHierarchyPlacesUnderTheRowOrAtTheRoot) {
    const stdfs::path content = Scratch("aether_content_drop_hierarchy");
    WriteBoxModel(content, "Models/crate.glb", Vec3(-1, 0, -1), Vec3(1, 2, 1));
    HierarchyDropRig rig(content);
    SceneDocument& document = rig.panel.Document();
    document.Commands().Clear();
    const usize before = document.Entities().size();
    const HierarchyIndex index(document);
    const Entity first_row = index.Children(kNullEntity).front();
    const EntityGuid first_guid = document.GetWorld().GetComponent<IdComponent>(first_row)->guid;
    const Vec3 first_at = WorldPositionOf(document, first_row);

    // Carrying the mouse down the list finds the places that take a drop: the heading, then each row, a distinct one each.
    const std::vector<f32> targets = rig.Targets();
    AETHER_CHECK(targets.size() >= 2);
    if (targets.size() < 2) return;

    // Let go over the first row: the model is placed under that entity, at its origin, in one step.
    rig.Step(ImVec2(80.0f, targets[1]), true);  // over the row, still holding
    rig.Step(ImVec2(80.0f, targets[1]), false);
    AETHER_CHECK(document.Entities().size() == before + 1 && rig.panel.Status() == "Placed Models/crate.glb");
    const Entity placed = document.Selection().empty() ? kNullEntity : document.Selection()[0];
    AETHER_CHECK(placed != kNullEntity);
    if (placed == kNullEntity) return;
    const Parent* link = document.GetWorld().GetComponent<Parent>(placed);
    AETHER_CHECK(link != nullptr && link->parent == first_guid);
    AETHER_CHECK(NearVec(WorldPositionOf(document, placed), first_at, 0.05f));
    AETHER_CHECK(document.Undo() && !document.CanUndo() && document.Entities().size() == before);

    // Let go over the heading instead: placed at the root, at the origin.
    const std::vector<f32> again = rig.Targets();
    AETHER_CHECK(!again.empty());
    if (again.empty()) return;
    rig.Step(ImVec2(80.0f, again[0]), true);  // over the heading, still holding
    rig.Step(ImVec2(80.0f, again[0]), false);
    AETHER_CHECK(document.Entities().size() == before + 1);
    const Entity at_root = document.Selection().empty() ? kNullEntity : document.Selection()[0];
    AETHER_CHECK(at_root != kNullEntity && document.GetWorld().GetComponent<Parent>(at_root) == nullptr);
    AETHER_CHECK(at_root != kNullEntity && NearVec(WorldPositionOf(document, at_root), Vec3(0, 0, 0), 0.05f));
}

AETHER_TEST(ScenePanel_ADragOfAnEntityOntoTheHeadingMakesItARootAgain) {
    const stdfs::path content = Scratch("aether_hierarchy_heading");
    HierarchyDropRig rig(content);
    SceneDocument& document = rig.panel.Document();
    const Entity parent = document.CreateEntity("Zz Parent");
    const Entity child = document.CreateEntity("Zz Child", SceneEntityKind::Empty, {}, parent);
    document.Commands().Clear();
    AETHER_CHECK(document.ParentOf(child) == parent);
    rig.drag_entity = child;
    const std::vector<f32> targets = rig.Targets();
    AETHER_CHECK(!targets.empty());
    if (targets.empty()) return;
    rig.Step(ImVec2(80.0f, targets[0]), true);  // over the heading, still holding
    rig.Step(ImVec2(80.0f, targets[0]), false);
    AETHER_CHECK(document.ParentOf(child).IsNull());
    AETHER_CHECK(document.Undo() && document.ParentOf(child) == parent);
}

namespace {

// A real drag onto the reflected inspector: a row that drags `path`, and an inspector of `type`/`object` below it. Finds each place that takes a drop by carrying the
// mouse down the value column, as HierarchyDropRig does for the hierarchy.
struct InspectorDropRig {
    HeadlessImGui imgui;
    const reflect::TypeInfo& type;
    void* object;
    std::string path;
    ImVec2 source_at{20, 20};
    bool committed = false;             // an edit finished on some frame since the last Reset
    std::string changed;                // the top-level field that changed then
    InspectorDropRig(const reflect::TypeInfo& t, void* o) : type(t), object(o) { Step(ImVec2(0, 0), false); }
    void Reset() { committed = false; changed.clear(); }
    void Step(const ImVec2& mouse, bool down) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(0, down);
        imgui.Frame([&] {
            ImGui::SetCursorScreenPos(ImVec2(10, 10));
            ImGui::Button("row", ImVec2(120, 24));
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload(kContentPathPayload, path.c_str(), path.size() + 1);
                ImGui::TextUnformatted(path.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::SetCursorScreenPos(ImVec2(10, 50));
            const InspectResult result = InspectObject(type, object, "drop_target");
            if (result.committed) committed = true;
            if (result.changed_field != nullptr) changed = result.changed_field->name;
        });
    }
    std::vector<f32> Targets() {
        Step(source_at, false);
        Step(source_at, true);
        Step(ImVec2(source_at.x + 30.0f, source_at.y), true);
        std::vector<f32> found;
        f32 last_top = -1.0f;
        for (f32 y = 60.0f; y < 400.0f; y += 3.0f) {
            Step(ImVec2(900.0f, y), true);
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            if (g.DragDropTargetId == 0 || g.DragDropTargetRect.Min.y == last_top) continue;
            last_top = g.DragDropTargetRect.Min.y;
            found.push_back((g.DragDropTargetRect.Min.y + g.DragDropTargetRect.Max.y) * 0.5f);
        }
        return found;
    }
    // Drags `dropped` onto the n-th target, and lets go.
    void DropOn(usize n, const std::string& dropped) {
        path = dropped;
        const std::vector<f32> targets = Targets();
        AETHER_CHECK(n < targets.size());
        if (n >= targets.size()) return;
        Reset();
        Step(ImVec2(900.0f, targets[n]), true);
        Step(ImVec2(900.0f, targets[n]), false);
    }
};

} // namespace

AETHER_TEST(InspectorDrop_AnAssetDroppedOnAPathFieldOfTheRightKindSetsItInOneEdit) {
    SkyLight sky;
    InspectorDropRig rig(reflect::Reflect<SkyLight>(), &sky);
    // The sky's image field takes a picture, and a drop sets it as one finished edit of that field.
    rig.DropOn(0, "Textures/sky.hdr");
    AETHER_CHECK(sky.environment == "Textures/sky.hdr" && rig.committed && rig.changed == "environment");
    // The extension is matched whatever its case; and a model is not a sky image, so the field stays as it was.
    rig.DropOn(0, "Textures/Sunset.JPG");
    AETHER_CHECK(sky.environment == "Textures/Sunset.JPG");
    rig.DropOn(0, "Models/crate.glb");
    AETHER_CHECK(sky.environment == "Textures/Sunset.JPG" && !rig.committed);
    rig.DropOn(0, "Scenes/Main.ascene");
    AETHER_CHECK(sky.environment == "Textures/Sunset.JPG" && !rig.committed);
}

AETHER_TEST(InspectorDrop_AMaterialFieldTakesAMaterialAndADecalsTextureFieldAPicture) {
    // A material goes on the material field, and not a picture.
    MaterialParameters parameters;
    InspectorDropRig materials(reflect::Reflect<MaterialParameters>(), &parameters);
    materials.DropOn(0, "Materials/Shiny.amat");
    AETHER_CHECK(parameters.material == "Materials/Shiny.amat" && materials.changed == "material" && materials.committed);
    materials.DropOn(0, "Textures/brick.png");
    AETHER_CHECK(parameters.material == "Materials/Shiny.amat" && !materials.committed);
    // A decal's texture takes a picture, and not a material.
    Decal decal;
    InspectorDropRig decals(reflect::Reflect<Decal>(), &decal);
    decals.DropOn(0, "Textures/brick.PNG");
    AETHER_CHECK(decal.texture == "Textures/brick.PNG" && decals.changed == "texture" && decals.committed);
    decals.DropOn(0, "Materials/Shiny.amat");
    AETHER_CHECK(decal.texture == "Textures/brick.PNG" && !decals.committed);
}

AETHER_TEST(InspectorDrop_ThePathFieldsSayWhatTheyTakeAndOtherStringsTakeNothing) {
    // The nested texture of a material parameter (a list of structs, folded in the inspector) is drawn by the same code as the top-level fields above;
    // what it needs is its declaration.
    const reflect::TypeInfo& value = reflect::Reflect<MaterialParameterValue>();
    const reflect::FieldInfo* texture = nullptr;
    const reflect::FieldInfo* name = nullptr;
    for (const reflect::FieldInfo& field : value.fields) {
        if (std::string(field.name) == "texture") texture = &field;
        if (std::string(field.name) == "name") name = &field;
    }
    AETHER_CHECK(texture != nullptr && name != nullptr);
    if (texture == nullptr || name == nullptr) return;
    AETHER_CHECK(HasAssetExtension("Textures/a.png", texture->meta.asset_extensions) && !HasAssetExtension("Materials/a.amat", texture->meta.asset_extensions));
    AETHER_CHECK(name->meta.asset_extensions == nullptr);                       // a name is text, not a path: nothing can be dropped on it
    // And an entity's name takes nothing either, however plain it looks.
    const reflect::TypeInfo& entity_name = reflect::Reflect<EntityName>();
    for (const reflect::FieldInfo& field : entity_name.fields) AETHER_CHECK(field.meta.asset_extensions == nullptr);
}

AETHER_TEST(InspectorDrop_TheExtensionRuleIsCaseInsensitiveAndExact) {
    AETHER_CHECK(HasAssetExtension("A/b.png", ".png;.jpg") && HasAssetExtension("A/b.JPG", ".png;.jpg") && HasAssetExtension("b.Png", ".png"));
    AETHER_CHECK(!HasAssetExtension("A/b.pngx", ".png;.jpg") && !HasAssetExtension("A/b.png.meta", ".png") && !HasAssetExtension("A/png", ".png"));
    AETHER_CHECK(!HasAssetExtension("A/b.png", nullptr) && !HasAssetExtension("A/b.png", "") && !HasAssetExtension("", ".png"));
}

namespace {

// A .glb of several axis-aligned boxes in one mesh, each from `low` to `high`: a model whose bounding box has empty space in it.
void WriteBoxesModel(const stdfs::path& content, const std::string& name, const std::vector<std::pair<Vec3, Vec3>>& boxes) {
    assets::procedural::MeshBuilder builder;
    for (const auto& box : boxes) builder.Box(box.first, box.second);
    assets::procedural::ProceduralMesh mesh;
    mesh.materials.push_back(assets::MaterialData{});
    mesh.mesh.primitives.push_back(builder.Finish(assets::procedural::Shading::Flat, assets::procedural::UvMode::BoxProjected, 0));
    const std::vector<u8> glb = assets::procedural::EncodeGlb(mesh);
    stdfs::create_directories((content / name).parent_path());
    std::ofstream(content / name, std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
}

} // namespace

AETHER_TEST(ModelBounds_TheTrianglesAreKeptWithTheBoxAndOnlyForAModelThatHasThem) {
    const stdfs::path content = Scratch("aether_model_triangles");
    WriteBoxesModel(content, "Models/pair.glb", {{Vec3(4, 0, -1), Vec3(5, 2, 1)}, {Vec3(10, 0, -1), Vec3(11, 2, 1)}});
    ModelBoundsCache cache(content);
    const ModelTriangles* shape = cache.Triangles("Models/pair.glb");
    AETHER_CHECK(shape != nullptr);
    if (shape == nullptr) return;
    AETHER_CHECK(shape->TriangleCount() == 24 && shape->indices.size() == 72);                    // two boxes: six faces of two triangles each
    LocalBounds box;
    AETHER_CHECK(cache.Get("Models/pair.glb", box) && NearVec(box.min, Vec3(4, 0, -1), 1e-3f) && NearVec(box.max, Vec3(11, 2, 1), 1e-3f)); // the box spans the gap
    for (const u32 index : shape->indices) AETHER_CHECK(index < shape->positions.size());
    AETHER_CHECK(cache.Triangles("Models/missing.glb") == nullptr && cache.Triangles("") == nullptr);
    ModelBoundsCache nowhere;
    AETHER_CHECK(nowhere.Triangles("Models/pair.glb") == nullptr);                                  // no content folder: nothing to read
    cache.Clear();
    AETHER_CHECK(cache.CachedCount() == 0 && cache.Triangles("Models/pair.glb") != nullptr);       // read again after a Clear
}

AETHER_TEST(PickEntity_AClickInTheGapOfAModelsBoxMissesItAndOneOnAPartHits) {
    const stdfs::path content = Scratch("aether_pick_mesh");
    WriteBoxesModel(content, "Models/pair.glb", {{Vec3(4, 0, -1), Vec3(5, 2, 1)}, {Vec3(10, 0, -1), Vec3(11, 2, 1)}});
    WriteBoxModel(content, "Models/crate.glb", Vec3(-1, -1, -1), Vec3(1, 1, 1));
    ModelBoundsCache cache(content);
    SceneDocument document;
    const Entity pair = document.CreateEntity("Pair", SceneEntityKind::Model, "Models/pair.glb");
    SceneViewCamera camera;
    camera.target = Vec3(7.5f, 1.0f, 0.0f); // the middle of the gap
    camera.distance = 16.0f;
    camera.pitch = 0.4f;
    camera.yaw = 0.0f;
    const ImVec2 origin(0, 0), size(800, 600);
    auto pixel_of = [&](const Vec3& local) {
        const Mat4 world = ComputeWorldTransform(document.GetWorld(), document.Guids(), pair);
        const Vec4 w = world * Vec4(local.x, local.y, local.z, 1.0f);
        ImVec2 at(-1, -1);
        ProjectToViewport(camera.ViewProjection(size.x / size.y), Vec3(w.x, w.y, w.z), origin, size, at);
        return at;
    };
    // On the first part and on the second: picked. In the gap, which the box spans: not. Nowhere near anything: not.
    AETHER_CHECK(PickEntity(document, camera, origin, size, pixel_of(Vec3(4.5f, 1.0f, 0.0f)), 14.0f, &cache) == pair);
    AETHER_CHECK(PickEntity(document, camera, origin, size, pixel_of(Vec3(10.5f, 1.0f, 0.0f)), 14.0f, &cache) == pair);
    AETHER_CHECK(PickEntity(document, camera, origin, size, pixel_of(Vec3(7.5f, 1.0f, 0.0f)), 14.0f, &cache) == kNullEntity);
    AETHER_CHECK(PickEntity(document, camera, origin, size, ImVec2(40, 40), 14.0f, &cache) == kNullEntity);

    // The same through the entity's transform: turned, scaled and moved, the parts are where the transform puts them and the gap still misses.
    document.SetRotation(pair, Quaternion::FromAxisAngle(Vec3(0, 1, 0), 3.14159265f * 0.5f), true);
    document.SetScale(pair, Vec3(2, 2, 2), true);
    document.SetPosition(pair, Vec3(3, 0, 0), true);
    const Mat4 moved = ComputeWorldTransform(document.GetWorld(), document.Guids(), pair);
    const Vec4 gap = moved * Vec4(7.5f, 1.0f, 0.0f, 1.0f);
    camera.target = Vec3(gap.x, gap.y, gap.z);
    AETHER_CHECK(PickEntity(document, camera, origin, size, pixel_of(Vec3(4.5f, 1.0f, 0.0f)), 14.0f, &cache) == pair);
    AETHER_CHECK(PickEntity(document, camera, origin, size, pixel_of(Vec3(10.5f, 1.0f, 0.0f)), 14.0f, &cache) == pair);
    AETHER_CHECK(PickEntity(document, camera, origin, size, pixel_of(Vec3(7.5f, 1.0f, 0.0f)), 14.0f, &cache) == kNullEntity);

    // A model behind the gap is what a click there selects: the pair no longer hides it by having a box in the way.
    Vec3 ray_origin, ray_direction;
    AETHER_CHECK(MouseRay(camera.ViewProjection(size.x / size.y), origin, size, pixel_of(Vec3(7.5f, 1.0f, 0.0f)), ray_origin, ray_direction));
    const Entity behind = document.CreateEntity("Behind", SceneEntityKind::Model, "Models/crate.glb");
    document.SetPosition(behind, ray_origin + ray_direction * ((camera.target - ray_origin).Length() + 6.0f), true);
    AETHER_CHECK(PickEntity(document, camera, origin, size, pixel_of(Vec3(7.5f, 1.0f, 0.0f)), 14.0f, &cache) == behind);
    // And a model in front of a part hides it, as before: the nearer of the two along the ray wins.
    const Vec4 part_world = moved * Vec4(4.5f, 1.0f, 0.0f, 1.0f);
    const ImVec2 on_part = pixel_of(Vec3(4.5f, 1.0f, 0.0f));
    AETHER_CHECK(MouseRay(camera.ViewProjection(size.x / size.y), origin, size, on_part, ray_origin, ray_direction));
    const f32 to_part = (Vec3(part_world.x, part_world.y, part_world.z) - ray_origin).Length();
    document.SetPosition(behind, ray_origin + ray_direction * (to_part - 5.0f), true);              // in front of the part, on its ray
    AETHER_CHECK(PickEntity(document, camera, origin, size, on_part, 14.0f, &cache) == behind);
    document.SetPosition(behind, ray_origin + ray_direction * (to_part + 5.0f), true);              // and behind it: the part is picked
    AETHER_CHECK(PickEntity(document, camera, origin, size, on_part, 14.0f, &cache) == pair);
}

AETHER_TEST(PickEntity_TheNearerOfTwoIsJudgedByWhereTheRayMeetsTheShapeNotTheBox) {
    const stdfs::path content = Scratch("aether_pick_mesh_depth");
    // A model whose real surface is a wall far behind a small block, so its box starts well in front of anything a ray through the wall can hit.
    WriteBoxesModel(content, "Models/deep.glb", {{Vec3(3, 0, 0.5f), Vec3(4, 1, 1)}, {Vec3(-5, 0, -20), Vec3(5, 2, -19)}});
    WriteBoxModel(content, "Models/crate.glb", Vec3(-1, -1, -1), Vec3(1, 1, 1));
    ModelBoundsCache cache(content);
    SceneDocument document;
    const Entity deep = document.CreateEntity("Deep", SceneEntityKind::Model, "Models/deep.glb");
    SceneViewCamera camera;
    camera.distance = 12.0f;
    camera.pitch = 0.0f; // level, so the ray runs along the model's depth
    camera.yaw = 0.0f;
    const ImVec2 origin(0, 0), size(800, 600);
    Vec3 ray_origin, ray_direction;
    camera.target = Vec3(0, 1, 0);
    AETHER_CHECK(MouseRay(camera.ViewProjection(size.x / size.y), origin, size, ImVec2(400, 300), ray_origin, ray_direction));
    if (ray_direction.z > 0.0f) document.SetRotation(deep, Quaternion::FromAxisAngle(Vec3(0, 1, 0), 3.14159265f), true); // the wall is the far side, whichever way the camera looks
    const Mat4 world = ComputeWorldTransform(document.GetWorld(), document.Guids(), deep);
    const Vec4 wall = world * Vec4(0.0f, 1.0f, -19.5f, 1.0f);
    camera.target = Vec3(wall.x, wall.y, wall.z); // the wall's middle: the ray through the middle of the view meets the wall and nothing of the block
    const ImVec2 middle(400, 300);
    AETHER_CHECK(MouseRay(camera.ViewProjection(size.x / size.y), origin, size, middle, ray_origin, ray_direction));
    AETHER_CHECK(PickEntity(document, camera, origin, size, middle, 14.0f, &cache) == deep);
    // A crate halfway along the ray: well past where the model's box begins, well short of its wall. The wall is what the ray meets, so the crate is nearer and wins.
    const Entity crate = document.CreateEntity("Crate", SceneEntityKind::Model, "Models/crate.glb");
    document.SetPosition(crate, ray_origin + ray_direction * ((camera.target - ray_origin).Length() - 10.0f), true);
    AETHER_CHECK(PickEntity(document, camera, origin, size, middle, 14.0f, &cache) == crate);
}

// The outline of a selected entity is drawn where a running game really has it (a body the physics moved), not where the document left it.
AETHER_TEST(ViewportTools_AnOutlineFollowsTheLivePositionOfARunningBody) {
    HeadlessImGui ui;
    SceneDocument document;
    const Entity thing = document.CreateEntity("Thing");
    document.SetPosition(thing, Vec3(0.0f, 0.0f, 0.0f), true);
    document.SetSelection({thing});
    SceneViewCamera camera;
    camera.distance = 14.0f;
    const ImVec2 origin(0.0f, 0.0f), size(800.0f, 600.0f);
    ViewportTools tools;
    auto mean_x = [&](const ViewportTools::LivePosition& live) {
        f32 sum = 0.0f;
        int count = 0;
        ui.Frame([&] {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const int first = draw->VtxBuffer.Size;
            tools.DrawSelectionOutlines(draw, document, camera, origin, size, false, live);
            for (int i = first; i < draw->VtxBuffer.Size; ++i) {
                sum += draw->VtxBuffer[i].pos.x;
                ++count;
            }
        });
        return count > 0 ? sum / static_cast<f32>(count) : -1.0f;
    };
    const f32 at_document = mean_x(nullptr);
    AETHER_CHECK(at_document > 0.0f);
    AETHER_CHECK(std::fabs(mean_x([](Entity, Vec3&) { return false; }) - at_document) < 0.01f); // no such entity in the game: where the document has it
    const f32 moved = mean_x([](Entity, Vec3& position) { position = Vec3(4.0f, 0.0f, 0.0f); return true; });
    AETHER_CHECK(moved > at_document + 40.0f);                                                   // drawn 4 m to the right
}
