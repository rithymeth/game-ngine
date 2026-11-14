#include "test_framework.h"

#include "aether/assets/content_browser.h"
#include "aether/project/project.h"
#include "content/content_browser_panel.h"

#include <imgui.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

// Multiple selection in the Content Browser and the operations that act on it as one batch: duplicate, delete, move,
// rename. A batch is the transaction -- one database rescan and one refresh per call -- and a delete that something
// outside the batch refers to is refused with nothing removed.

using namespace aether;
using namespace aether::assets;
using namespace aether::editor;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

const stdfs::path kRepoAssets = stdfs::path(AETHER_REPO_ASSETS_DIR);

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigMacOSXBehaviors = false;
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 800);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1100, 700));
        ImGui::Begin("Test");
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

stdfs::path FreshProject(const std::string& name, ProjectPaths& paths) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_content_select_tests" / name;
    std::error_code ec;
    stdfs::remove_all(dir, ec);
    stdfs::create_directories(dir, ec);
    std::string error;
    CHECK(CreateProject(dir, "Game", &paths, &error));
    stdfs::copy(kRepoAssets / "models", paths.content / "models", stdfs::copy_options::recursive);
    stdfs::copy(kRepoAssets / "textures", paths.content / "textures", stdfs::copy_options::recursive);
    return dir;
}

usize RowCount(ContentBrowserPanel& panel, const std::string& path) {
    usize count = 0;
    for (const ContentEntry& e : panel.Rows()) {
        if (e.path == path) ++count;
    }
    return count;
}

} // namespace

AETHER_TEST(ContentOps_DuplicateMakesAnIndependentCopy) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("duplicate", paths);
    AssetDatabase db(paths.content);
    db.Scan();
    const AssetGuid original = db.FindByPath("textures/checker_a.png")->guid;

    std::string created, error;
    CHECK(DuplicateContent(db, "textures/checker_a.png", &created, &error));
    CHECK(created == "textures/checker_a copy.png");
    const AssetRecord* copy = db.FindByPath(created);
    CHECK(copy != nullptr && copy->guid != original);
    CHECK(stdfs::exists(paths.content / created) && stdfs::exists(paths.content / (created + ".ameta")));
    CHECK(db.FindByPath("textures/checker_a.png")->guid == original); // the source keeps its identity

    // A second copy numbers itself, and a free name is found even when the first one exists on disk.
    CHECK(DuplicateContent(db, "textures/checker_a.png", &created));
    CHECK(created == "textures/checker_a copy 2.png");

    // Folders copy their whole tree with fresh identities.
    std::string folder_copy;
    CHECK(DuplicateContent(db, "textures", &folder_copy));
    CHECK(folder_copy == "textures copy");
    const AssetRecord* inner = db.FindByPath("textures copy/checker_b.png");
    CHECK(inner != nullptr && inner->guid != db.FindByPath("textures/checker_b.png")->guid);

    // Refusals: a missing source, a piece of another asset, a meta file, and anything outside Content.
    CHECK(!DuplicateContent(db, "textures/not_here.png", nullptr, &error) && !error.empty());
    CHECK(!DuplicateContent(db, "models/test_textured_cube.gltf#mesh:0", nullptr, &error));
    CHECK(!DuplicateContent(db, "textures/checker_a.png.ameta", nullptr, &error));
    CHECK(!DuplicateContent(db, "../escape.png", nullptr, &error));
    std::error_code ignore;
    stdfs::remove_all(project, ignore);
}

AETHER_TEST(ContentOps_DeleteIsAllOrNothing) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("delete", paths);
    AssetDatabase db(paths.content);
    db.Scan();

    // The models name the textures, so removing a texture on its own is refused...
    CHECK(!db.FindByPath("models/test_textured_cube.gltf")->dependencies.empty());
    std::string error;
    usize deleted = 99;
    const std::vector<std::string> batch = {"textures/checker_a.png", "textures/checker_b.png",
                                            "textures/test_solid_red.png"};
    CHECK(!DeleteContents(db, batch, &error, &deleted));
    CHECK(error.find("test_textured_cube.gltf") != std::string::npos);
    CHECK(deleted == 0);
    for (const std::string& path : batch) CHECK(stdfs::exists(paths.content / path));

    // ...but inside the batch the reference no longer counts, so both folders go together.
    CHECK(DeleteContents(db, {"textures", "models"}, &error, &deleted));
    CHECK(deleted == 2);
    CHECK(!stdfs::exists(paths.content / "textures") && !stdfs::exists(paths.content / "models"));
    CHECK(db.FindByPath("textures/checker_a.png") == nullptr && db.FindByPath("models/test_scene.gltf") == nullptr);

    // A single source takes its .ameta with it, and a name that isn't there is an error rather than a partial batch.
    std::ofstream(paths.content / "note.txt") << "hello";
    db.Scan();
    CHECK(DeleteContents(db, {"note.txt"}, nullptr, &deleted) && deleted == 1);
    CHECK(!stdfs::exists(paths.content / "note.txt") && !stdfs::exists(paths.content / "note.txt.ameta"));
    CHECK(!DeleteContents(db, {"note.txt"}, nullptr, &deleted) && deleted == 0);
    std::error_code ignore;
    stdfs::remove_all(project, ignore);
}

AETHER_TEST(ContentSelection_SelectsExtendsTogglesAndBoxSelects) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("selection", paths);
    HeadlessImGui imgui;
    ContentBrowserPanel panel(paths.file);
    CHECK(panel.Error().empty());
    panel.OpenFolder("textures");
    CHECK(panel.Rows().size() == 4);
    const std::string a = panel.Rows()[0].path;
    const std::string b = panel.Rows()[1].path;
    const std::string c = panel.Rows()[2].path;
    const std::string d = panel.Rows()[3].path;

    panel.Select(b);
    CHECK(panel.Selection().size() == 1 && panel.IsSelected(b));
    panel.Select(d, false, true); // Shift extends from the anchor
    CHECK(panel.Selection() == (std::vector<std::string>{b, c, d}));
    panel.Select(a, true); // Ctrl adds
    CHECK(panel.Selection().size() == 4 && panel.IsSelected(a) && panel.Selection().front() == b);
    panel.Select(a, true); // and removes
    CHECK(panel.Selection().size() == 3 && !panel.IsSelected(a));
    panel.SelectAll();
    CHECK(panel.Selection().size() == 4);
    panel.ClearSelection();
    CHECK(panel.Selection().empty());

    // The grid records where its tiles were; a rectangle round all of them takes all of them.
    imgui.Frame([&] { panel.Draw(); });
    panel.SelectInRect(0.0f, 0.0f, 4000.0f, 4000.0f);
    CHECK(panel.Selection().size() == 4);
    panel.SelectInRect(-1000.0f, -1000.0f, 0.0f, 0.0f); // nowhere near the tiles
    CHECK(panel.Selection().empty());
    panel.Select(a);
    panel.SelectInRect(0.0f, 0.0f, 4000.0f, 4000.0f, true); // Ctrl+drag adds, once each
    CHECK(panel.Selection().size() == 4);
    std::error_code ignore;
    stdfs::remove_all(project, ignore);
}

AETHER_TEST(ContentSelection_DuplicateSelectsTheCopies) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("duplicate_panel", paths);
    ContentBrowserPanel panel(paths.file);
    panel.OpenFolder("textures");
    const AssetGuid original = panel.Database()->FindByPath("textures/checker_a.png")->guid;

    panel.Select("textures/checker_a.png");
    panel.Select("textures/test_solid_red.png", true);
    CHECK(panel.DuplicateSelection());
    CHECK(panel.Selection() == (std::vector<std::string>{"textures/checker_a copy.png", "textures/test_solid_red copy.png"}));
    CHECK(panel.Rows().size() == 6); // one refresh, so the listing already has them
    const AssetRecord* copy = panel.Database()->FindByPath("textures/checker_a copy.png");
    CHECK(copy != nullptr && copy->guid != original);
    CHECK(RowCount(panel, "textures/checker_a copy.png") == 1);

    // Duplicating the copies numbers from 2, and the selection follows them again.
    CHECK(panel.DuplicateSelection());
    CHECK(panel.Selection() == (std::vector<std::string>{"textures/checker_a copy copy.png", "textures/test_solid_red copy copy.png"}));
    CHECK(panel.Rows().size() == 8);
    std::error_code ignore;
    stdfs::remove_all(project, ignore);
}

AETHER_TEST(ContentSelection_DeleteRefusesTheWholeBatchOrTakesAllOfIt) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("delete_panel", paths);
    ContentBrowserPanel panel(paths.file);

    // Two textures: one is used by a model, so nothing in the batch is removed.
    panel.OpenFolder("textures");
    panel.Select("textures/checker_a.png");
    panel.Select("textures/checker_b.png", true);
    std::string error;
    CHECK(!panel.DeleteSelection(&error));
    CHECK(error.find("test_textured_cube.gltf") != std::string::npos);
    CHECK(std::filesystem::exists(paths.content / "textures/checker_a.png"));
    CHECK(std::filesystem::exists(paths.content / "textures/checker_b.png"));
    CHECK(panel.Selection().size() == 2);
    CHECK(panel.Rows().size() == 4);

    // One nothing uses goes, with its .ameta, and the listing is already up to date.
    panel.ClearSelection();
    panel.Select("textures/test_solid_red.png");
    CHECK(panel.DeleteSelection(&error));
    CHECK(!std::filesystem::exists(paths.content / "textures/test_solid_red.png"));
    CHECK(!std::filesystem::exists(paths.content / "textures/test_solid_red.png.ameta"));
    CHECK(panel.Selection().empty());
    CHECK(panel.Rows().size() == 3);

    // The whole content, both folders at once: every reference stays inside the batch.
    panel.OpenFolder("");
    panel.Select("models");
    panel.Select("textures", true);
    CHECK(panel.DeleteSelection(&error));
    CHECK(!std::filesystem::exists(paths.content / "models") && !std::filesystem::exists(paths.content / "textures"));
    CHECK(panel.Rows().empty());
    std::error_code ignore;
    stdfs::remove_all(project, ignore);
}

AETHER_TEST(ContentSelection_MoveTakesTheSelectionAndFollowsTheOpenFolder) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("move_panel", paths);
    ContentBrowserPanel panel(paths.file);
    panel.OpenFolder("textures");

    panel.Select("textures/checker_a.png");
    panel.Select("textures/checker_b.png", true);
    CHECK(panel.MoveSelection("materials"));
    CHECK(std::filesystem::exists(paths.content / "materials/checker_a.png"));
    CHECK(std::filesystem::exists(paths.content / "materials/checker_b.png"));
    CHECK(panel.Selection() == (std::vector<std::string>{"materials/checker_a.png", "materials/checker_b.png"}));
    CHECK(panel.Database()->FindByPath("materials/checker_b.png") != nullptr);
    CHECK(panel.Rows().size() == 2); // the open folder no longer has either

    // Dropping where things already are is a no-op rather than an error.
    CHECK(panel.MoveSelection("materials"));
    CHECK(panel.Selection().size() == 2);

    // Moving the folder being shown follows it.
    panel.OpenFolder("materials");
    panel.Select("materials");
    CHECK(panel.MoveSelection("art"));
    CHECK(panel.Folder() == "art/materials");
    CHECK(panel.Selection() == (std::vector<std::string>{"art/materials"}));
    CHECK(std::filesystem::exists(paths.content / "art/materials/checker_a.png"));
    CHECK(panel.Rows().size() == 2);
    std::error_code ignore;
    stdfs::remove_all(project, ignore);
}

AETHER_TEST(ContentSelection_RenameEntryKeepsTheTypeAndTheSelection) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("rename_panel", paths);
    ContentBrowserPanel panel(paths.file);
    panel.OpenFolder("textures");

    // A name without an extension keeps the asset's.
    CHECK(panel.RenameEntry("textures/checker_b.png", "stone"));
    CHECK(!std::filesystem::exists(paths.content / "textures/checker_b.png"));
    CHECK(std::filesystem::exists(paths.content / "textures/stone.png"));
    CHECK(panel.Selection() == (std::vector<std::string>{"textures/stone.png"}));
    CHECK(panel.IsSelected("textures/stone.png"));
    CHECK(RowCount(panel, "textures/stone.png") == 1);

    // Changing the type is refused, and the asset stays where it was.
    std::string error;
    CHECK(!panel.RenameEntry("textures/stone.png", "stone.jpg", &error));
    CHECK(!error.empty());
    CHECK(std::filesystem::exists(paths.content / "textures/stone.png"));
    CHECK(!std::filesystem::exists(paths.content / "textures/stone.jpg"));

    // Renaming the open folder keeps it open.
    CHECK(panel.RenameEntry("textures", "Textures"));
    CHECK(panel.Folder() == "Textures");
    CHECK(std::filesystem::exists(paths.content / "Textures/stone.png"));
    CHECK(panel.Rows().size() == 4);
    std::error_code ignore;
    stdfs::remove_all(project, ignore);
}
