#include "test_framework.h"

#include "aether/assets/content_browser.h"
#include "aether/project/project.h"
#include "content/content_browser_panel.h"

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <string>

// The Content Browser's way of narrowing a listing: the search syntax (`type:` and `path:` words), the type chips, the
// folder scope and the sortable list columns.

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

stdfs::path ProjectWithContent(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_content_filter_tests" / name;
    std::error_code ec;
    stdfs::remove_all(dir, ec);
    stdfs::create_directories(dir / "Content", ec);
    stdfs::copy(kRepoAssets / "models", dir / "Content/models", stdfs::copy_options::recursive);
    stdfs::copy(kRepoAssets / "textures", dir / "Content/textures", stdfs::copy_options::recursive);
    return dir;
}

stdfs::path FreshProject(const std::string& name, ProjectPaths& paths) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_content_filter_tests" / name;
    std::error_code ec;
    stdfs::remove_all(dir, ec);
    stdfs::create_directories(dir, ec);
    std::string error;
    CHECK(CreateProject(dir, "Game", &paths, &error));
    stdfs::copy(kRepoAssets / "models", paths.content / "models", stdfs::copy_options::recursive);
    stdfs::copy(kRepoAssets / "textures", paths.content / "textures", stdfs::copy_options::recursive);
    return dir;
}

std::vector<std::string> Names(const std::vector<ContentEntry>& entries) {
    std::vector<std::string> names;
    for (const ContentEntry& entry : entries) names.push_back(entry.name);
    return names;
}

const ContentBrowserPanel::TypeChip* Chip(const ContentBrowserPanel& panel, const std::string& importer) {
    for (const ContentBrowserPanel::TypeChip& chip : panel.TypeChips()) {
        if (chip.importer == importer) return &chip;
    }
    return nullptr;
}

} // namespace

AETHER_TEST(ContentBrowser_SearchSyntaxFiltersByTypeAndPath) {
    const stdfs::path project = ProjectWithContent("syntax");
    AssetDatabase db(project / "Content");
    db.Scan();
    ContentQuery query;
    const auto search = [&](std::string text) {
        query.search = std::move(text);
        return Names(ListContent(db, query));
    };

    // type: filters by importer, case-insensitively and by prefix; path: by the content path; plain words by the name.
    CHECK(search("type:model") == (std::vector<std::string>{"test_animation.gltf", "test_scene.gltf", "test_skinned_ribbon.gltf",
                                                          "test_textured_cube.gltf"}));
    CHECK(search("TYPE:TEX").size() == 4); // every texture
    CHECK(search("path:textures/").size() == 4);
    CHECK(search("path:models/ type:model").size() == 4);
    CHECK(search("checker type:texture") == (std::vector<std::string>{"checker_a.png", "checker_b.png"}));
    CHECK(search("cube type:model") == (std::vector<std::string>{"test_textured_cube.gltf"}));

    // Every word has to match, and the search still looks in subfolders.
    CHECK(search("test cube") == (std::vector<std::string>{"test_textured_cube.gltf"}));
    CHECK(search("model cube").empty());
    CHECK(search("type:nomatch").empty());
    CHECK(search("path:nomatch/").empty());
    // An empty directive is not a term: with nothing left to match, this is the folder's own listing again.
    CHECK(search("type:").size() == 2);
    CHECK(search("path:").size() == 2);
    std::error_code ignore;
    stdfs::remove_all(project, ignore);
}

AETHER_TEST(ContentBrowser_TypeChipsAndSearchCombine) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("chips", paths);
    HeadlessImGui imgui;
    ContentBrowserPanel panel(paths.file);
    panel.OpenFolder("models");
    CHECK(panel.Rows().size() == 4); // the helper .bin is not an asset
    CHECK(Chip(panel, "Model") != nullptr && Chip(panel, "Model")->count == 4);
    CHECK(panel.UnfilteredCount() == 4);
    CHECK(Chip(panel, "Texture") == nullptr); // no texture in this folder

    panel.type_filter = {"Model"};
    panel.Refresh();
    CHECK(panel.Rows().size() == 4 && panel.UnfilteredCount() == 4);
    CHECK(panel.Rows().front().asset != nullptr && panel.Rows().front().asset->importer == "Model");

    // A chip and a search narrow together, and the chip counts what the search leaves.
    panel.search = "cube";
    panel.Refresh();
    CHECK(panel.Rows().size() == 1 && panel.Rows()[0].name == "test_textured_cube.gltf");
    CHECK(Chip(panel, "Model") != nullptr && Chip(panel, "Model")->count == 1);

    // A selected chip survives a search that leaves nothing, so it can be switched off again.
    panel.search = "nothing-like-this";
    panel.Refresh();
    CHECK(panel.Rows().empty());
    CHECK(Chip(panel, "Model") != nullptr && Chip(panel, "Model")->count == 0);

    imgui.Frame([&] { panel.Draw(); }); // the chip row and the no-match line draw

    panel.ClearFilters();
    CHECK(panel.search.empty() && panel.type_filter.empty() && !panel.changed_only);
    CHECK(panel.Rows().size() == 4);
    std::error_code ignore;
    stdfs::remove_all(project, ignore);
}

AETHER_TEST(ContentBrowser_FolderScopeAndItemCounts) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("scope", paths);
    ContentBrowserPanel panel(paths.file);
    CHECK(panel.Rows().size() == 2); // models/ and textures/
    CHECK(panel.UnfilteredCount() == 2);
    CHECK(panel.TypeChips().empty()); // no files in the root itself

    panel.scope_recursive = true;
    panel.Refresh();
    CHECK(panel.Rows().size() == 8 && panel.UnfilteredCount() == 8); // with subfolders, no folders shown
    CHECK(Chip(panel, "Model") != nullptr && Chip(panel, "Model")->count == 4);
    CHECK(Chip(panel, "Texture") != nullptr && Chip(panel, "Texture")->count == 4);

    panel.type_filter = {"Texture"};
    panel.Refresh();
    CHECK(panel.Rows().size() == 4);
    CHECK(panel.UnfilteredCount() == 8); // the chips narrow what is shown, not what is counted
    for (const ContentEntry& e : panel.Rows()) {
        CHECK(!e.is_folder && e.asset != nullptr && e.asset->importer == "Texture");
    }

    // The default scope is the folder itself.
    panel.scope_recursive = false;
    panel.type_filter.clear();
    panel.OpenFolder("models");
    CHECK(panel.Rows().size() == 4 && panel.Folder() == "models");
    std::error_code ignore;
    stdfs::remove_all(project, ignore);
}

AETHER_TEST(ContentBrowser_SortsByColumnAndKeepsFoldersOnTop) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("sorting", paths);
    HeadlessImGui imgui;
    ContentBrowserPanel panel(paths.file);
    panel.grid_view = false;
    panel.scope_recursive = true;
    panel.Refresh();
    CHECK(panel.Rows().size() == 8);

    panel.sort_column = ContentBrowserPanel::SortColumn::Type; // Model before Texture
    panel.Refresh();
    CHECK(panel.Rows().front().asset->importer == "Model");
    CHECK(panel.Rows().back().asset->importer == "Texture");
    panel.sort_descending = true;
    panel.Refresh();
    CHECK(panel.Rows().front().asset->importer == "Texture");

    // Folders stay on top under either column.
    panel.sort_descending = false;
    panel.sort_column = ContentBrowserPanel::SortColumn::Name;
    panel.scope_recursive = false;
    panel.OpenFolder("");
    CHECK(panel.Rows().size() == 2 && panel.Rows()[0].is_folder && panel.Rows()[1].is_folder);
    panel.sort_column = ContentBrowserPanel::SortColumn::Type;
    panel.Refresh();
    CHECK(panel.Rows().size() == 2 && panel.Rows()[0].is_folder && panel.Rows()[1].is_folder);

    // Descending by name, inside a folder.
    panel.OpenFolder("textures");
    panel.sort_column = ContentBrowserPanel::SortColumn::Name;
    panel.sort_descending = true;
    panel.Refresh();
    CHECK(panel.Rows().front().name == "test_solid_red.png");
    CHECK(panel.Rows().back().name == "checker_a.png");
    imgui.Frame([&] { panel.Draw(); }); // the sortable headers and breadcrumbs draw
    std::error_code ignore;
    stdfs::remove_all(project, ignore);
}
