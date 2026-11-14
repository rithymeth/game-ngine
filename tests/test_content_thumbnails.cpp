#include "test_framework.h"

#include "aether/assets/content_browser.h"
#include "aether/project/project.h"
#include "content/content_browser_panel.h"

#include <imgui.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>

// The Content Browser's grid of thumbnails: rendered once per source version, decoded from Saved/Thumbnails the second time,
// and never rendered for kinds that have no picture.

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

bool FreshProject(const std::string& name, ProjectPaths& paths) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_thumbnail_tests" / name;
    std::error_code ec;
    stdfs::remove_all(dir, ec);
    stdfs::create_directories(dir, ec);
    std::string error;
    const bool ok = CreateProject(dir, "Game", &paths, &error);
    CHECK(ok);
    return ok;
}

usize CountPngs(const stdfs::path& dir) {
    usize count = 0;
    std::error_code ec;
    for (stdfs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".png") ++count;
    }
    return count;
}

usize WithImporter(ContentBrowserPanel& panel, const std::string& importer) {
    usize count = 0;
    for (const ContentEntry& e : panel.Rows()) {
        if (!e.is_folder && e.asset != nullptr && e.asset->importer == importer) ++count;
    }
    return count;
}

// A few frames with a wait after each: the first frame queues the folder's pictures, the wait finishes them, and the next
// frame asks for them again -- from memory if they have just been made, from disk if this is a later session.
void Settle(HeadlessImGui& imgui, ContentBrowserPanel& panel) {
    for (int frame = 0; frame < 3; ++frame) {
        imgui.Frame([&] { panel.Draw(); });
        panel.Thumbnails().Wait();
    }
}

} // namespace

AETHER_TEST(ContentBrowserGrid_ThumbnailsRenderOnceAndComeFromDiskTheSecondTime) {
    ProjectPaths paths;
    if (!FreshProject("cache", paths)) return;
    stdfs::copy(kRepoAssets / "textures", paths.content / "Textures", stdfs::copy_options::recursive);
    const stdfs::path thumbnails = paths.saved / "Thumbnails";

    usize textures = 0;
    {
        HeadlessImGui imgui;
        ContentBrowserPanel panel(paths.file);
        CHECK(panel.Error().empty());
        CHECK(panel.grid_view); // the grid is the default listing
        panel.OpenFolder("Textures");
        textures = WithImporter(panel, "Texture");
        CHECK(textures >= 3);

        // The list asks for no pictures at all.
        panel.grid_view = false;
        Settle(imgui, panel);
        CHECK(panel.Thumbnails().Rendered() == 0 && panel.Thumbnails().LoadedFromDisk() == 0);
        CHECK(CountPngs(thumbnails) == 0);

        // The grid asks for one per picture kind and the worker makes them.
        panel.grid_view = true;
        Settle(imgui, panel);
        CHECK(panel.Thumbnails().Rendered() == textures);
        CHECK(panel.Thumbnails().LoadedFromDisk() == 0);
        CHECK(CountPngs(thumbnails) == textures);
    }

    {
        // A second open decodes the disk copies: nothing is imported or rendered again.
        HeadlessImGui imgui;
        ContentBrowserPanel second(paths.file);
        second.OpenFolder("Textures");
        Settle(imgui, second);
        CHECK(second.Thumbnails().Rendered() == 0);
        CHECK(second.Thumbnails().LoadedFromDisk() == textures);
        CHECK(CountPngs(thumbnails) == textures);
        std::printf("second open: %zu pictures decoded from disk, none rendered\n", static_cast<size_t>(textures));
    }

    {
        // A changed source invalidates exactly its own picture; the stale copy on disk is replaced, not accumulated.
        stdfs::copy_file(kRepoAssets / "textures" / "test_solid_red.png", paths.content / "Textures" / "checker_a.png",
                         stdfs::copy_options::overwrite_existing);
        HeadlessImGui imgui;
        ContentBrowserPanel changed(paths.file);
        changed.OpenFolder("Textures");
        Settle(imgui, changed);
        CHECK(changed.Thumbnails().Rendered() == 1);
        CHECK(changed.Thumbnails().LoadedFromDisk() == textures - 1);
        CHECK(CountPngs(thumbnails) == textures);
    }
}

AETHER_TEST(ContentBrowserGrid_KindsWithoutAPictureGetATypeTile) {
    ProjectPaths paths;
    if (!FreshProject("kinds", paths)) return;
    stdfs::copy(kRepoAssets / "models", paths.content / "models", stdfs::copy_options::recursive);
    stdfs::copy(kRepoAssets / "textures", paths.content / "textures", stdfs::copy_options::recursive);

    HeadlessImGui imgui;
    ContentBrowserPanel panel(paths.file);
    panel.OpenFolder("models");
    const usize models = WithImporter(panel, "Model");
    CHECK(models >= 3);

    Settle(imgui, panel);
    CHECK(panel.Thumbnails().Rendered() == 0);
    CHECK(panel.Thumbnails().WithoutPicture() == models); // models need the renderer: counted, not retried forever
    CHECK(CountPngs(paths.saved / "Thumbnails") == 0);

    // The tiles still draw, and the grid is what the panel shows.
    imgui.Frame([&] { panel.Draw(); });
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
}
