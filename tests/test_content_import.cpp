#include "test_framework.h"

#include "aether/assets/content_browser.h"
#include "aether/assets/importer.h"
#include "aether/assets/texture_importer.h"
#include "aether/project/project.h"
#include "content/content_browser_panel.h"

#include <imgui.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

// Stage 3's import side: the settings drawer (persisted in the .ameta and reimported), reimport itself, and files
// dropped from the OS into the Content Browser.

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
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_content_import_tests" / name;
    std::error_code ec;
    stdfs::remove_all(dir, ec);
    stdfs::create_directories(dir, ec);
    std::string error;
    CHECK(CreateProject(dir, "Game", &paths, &error));
    stdfs::copy(kRepoAssets / "models", paths.content / "models", stdfs::copy_options::recursive);
    stdfs::copy(kRepoAssets / "textures", paths.content / "textures", stdfs::copy_options::recursive);
    return dir;
}

// A folder outside the project, pretending to be Explorer.
stdfs::path DropFolder(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_content_import_drops" / name;
    std::error_code ec;
    stdfs::remove_all(dir, ec);
    stdfs::create_directories(dir, ec);
    return dir;
}

void Write(const stdfs::path& file, const std::string& text) {
    stdfs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::trunc) << text;
}

} // namespace

AETHER_TEST(ImportSettings_RoundTripAndReimportIdentity) {
    const stdfs::path project = stdfs::temp_directory_path() / "aether_content_import_tests" / "settings";
    std::error_code ec;
    stdfs::remove_all(project, ec);
    stdfs::create_directories(project / "Content", ec);
    stdfs::copy_file(kRepoAssets / "textures/checker_a.png", project / "Content/checker.png");

    AssetDatabase db(project / "Content");
    db.Scan();
    const AssetGuid guid = db.FindByPath("checker.png")->guid;
    ImporterRegistry importers = ImporterRegistry::WithBuiltins();
    DerivedDataCache cache(project / "Intermediate/DDC");
    std::string error;

    // The effective settings are the importer's defaults until the .ameta overrides one.
    nlohmann::json settings = ImportSettingsFor(db, importers, guid, &error);
    CHECK(error.empty() && settings.is_object());
    CHECK(settings["srgb"] == true && settings["generate_mips"] == true && settings["max_size"] == 0);

    ImportOutput first = ImportAsset(db, guid, importers, cache);
    CHECK(first.ok && !first.from_cache);
    TextureData texture;
    CHECK(DecodeTextureData(first.data, texture) && texture.srgb && texture.mips.size() == 7);

    // Applying settings writes them into the .ameta and forgets the recorded source hash, which a rescan turns into
    // needs_import -- the same flag a changed source raises.
    const nlohmann::json chosen = {{"srgb", false}, {"generate_mips", false}};
    CHECK(SetImportSettings(db, importers, guid, chosen, &error) && error.empty());
    AssetMeta meta;
    CHECK(LoadAssetMeta(db.MetaPath(guid), meta) && meta.settings == chosen && meta.source_hash.empty());
    CHECK(ImportSettingsFor(db, importers, guid)["srgb"] == false);
    db.Scan();
    CHECK(db.Find(guid)->needs_import);

    ImportOutput second = ImportAsset(db, guid, importers, cache);
    CHECK(second.ok && !second.from_cache);
    TextureData flat;
    CHECK(DecodeTextureData(second.data, flat) && !flat.srgb && flat.mips.size() == 1);
    CHECK(!db.Find(guid)->needs_import);

    // Reimport with nothing changed is served by the cache and hands back the same bytes: reimport is identity.
    ImportOutput again = ImportAsset(db, guid, importers, cache);
    CHECK(again.ok && again.from_cache && again.data == second.data);

    // Back to the defaults: the first import's cache entry, byte for byte, even after the settings round trip.
    CHECK(SetImportSettings(db, importers, guid, nlohmann::json::object(), &error));
    db.Scan();
    ImportOutput restored = ImportAsset(db, guid, importers, cache);
    CHECK(restored.ok && restored.from_cache && restored.data == first.data);

    // Only declared keys of the declared kinds are accepted.
    CHECK(!SetImportSettings(db, importers, guid, {{"quality", 3}}, &error) && error.find("quality") != std::string::npos);
    CHECK(!SetImportSettings(db, importers, guid, {{"srgb", "yes"}}, &error) && error.find("boolean") != std::string::npos);
    CHECK(!SetImportSettings(db, importers, guid, nlohmann::json::array(), &error));
    CHECK(!SetImportSettings(db, importers, AssetGuid{}, {{"srgb", true}}, &error));
    // ...and a failed write left the stored settings alone.
    CHECK(LoadAssetMeta(db.MetaPath(guid), meta) && meta.settings == nlohmann::json::object());

    // A kind with no importer has nothing to configure.
    Write(project / "Content/tool.luau", "print(1)\n");
    db.Scan();
    const AssetRecord* script = db.FindByPath("tool.luau");
    CHECK(script != nullptr);
    CHECK(ImportSettingsFor(db, importers, script->guid, &error).empty() && !error.empty());
    stdfs::remove_all(project, ec);
}

AETHER_TEST(ImportFiles_CopiesIntoTheOpenFolderAndSelects) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("files", paths);
    ContentBrowserPanel panel(paths.file);
    panel.OpenFolder("textures");
    const stdfs::path drop = DropFolder("files");
    stdfs::copy_file(kRepoAssets / "textures/checker_b.png", drop / "hero.png");

    usize imported = 0;
    std::string error;
    CHECK(panel.ImportFiles({(drop / "hero.png").string()}, &imported, &error) && error.empty());
    CHECK(imported == 1);
    CHECK(stdfs::exists(paths.content / "textures/hero.png") && stdfs::exists(paths.content / "textures/hero.png.ameta"));
    CHECK(panel.Selection().size() == 1 && panel.Selection()[0] == "textures/hero.png");
    const assets::AssetRecord* hero = panel.Database()->FindByPath("textures/hero.png");
    CHECK(hero != nullptr && hero->importer == "Texture" && hero->needs_import); // a new source imports on first use
    CHECK(panel.Rows().size() == 5);

    // The same file again is already there: nothing is copied, and it is still what the drop selects.
    CHECK(panel.ImportFiles({(drop / "hero.png").string()}, &imported, &error) && imported == 0);
    CHECK(panel.Selection().size() == 1 && panel.Selection()[0] == "textures/hero.png");

    // A refused file refuses the batch: the good file in it is not copied either.
    stdfs::copy_file(kRepoAssets / "textures/checker_b.png", drop / "second.png");
    Write(drop / "notes.txt", "not an asset");
    CHECK(!panel.ImportFiles({(drop / "second.png").string(), (drop / "notes.txt").string()}, &imported, &error));
    CHECK(error.find("notes.txt") != std::string::npos);
    CHECK(!stdfs::exists(paths.content / "textures/second.png"));
    // ...a directory is not a file...
    CHECK(!panel.ImportFiles({drop.string()}, &imported, &error));
    // ...and a used name with different contents is refused rather than replaced.
    stdfs::copy_file(kRepoAssets / "textures/checker_b.png", drop / "checker_a.png");
    CHECK(!panel.ImportFiles({(drop / "checker_a.png").string()}, &imported, &error));
    CHECK(error.find("checker_a.png") != std::string::npos);
    CHECK(assets::HashFile(paths.content / "textures/checker_a.png") == assets::HashFile(kRepoAssets / "textures/checker_a.png"));

    // Into the content root, where the next scan makes the .ameta.
    panel.OpenFolder("");
    CHECK(panel.ImportFiles({(drop / "second.png").string()}, &imported, &error) && imported == 1);
    CHECK(panel.Database()->FindByPath("second.png") != nullptr && panel.Selection()[0] == "second.png");
    std::error_code ignore;
    stdfs::remove_all(project, ignore);
    stdfs::remove_all(drop, ignore);
}

AETHER_TEST(ImportSettings_DrawerEditsAppliesAndDraws) {
    ProjectPaths paths;
    const stdfs::path project = FreshProject("drawer", paths);
    Write(paths.content / "scripts/tool.luau", "print(1)\n");
    HeadlessImGui imgui;
    ContentBrowserPanel panel(paths.file);
    CHECK(!panel.import_open);

    std::string error;
    CHECK(panel.OpenImportSettings("textures/checker_a.png", &error) && error.empty());
    CHECK(panel.import_open && panel.import_path == "textures/checker_a.png");
    CHECK(panel.import_draft["srgb"] == true && panel.import_draft["generate_mips"] == true);

    // The engine refuses a value of the wrong kind, and the drawer says why without closing.
    panel.import_draft["srgb"] = "yes";
    CHECK(!panel.ApplyImportSettings());
    CHECK(panel.import_open && panel.import_error.find("boolean") != std::string::npos);

    panel.import_draft = {{"srgb", false}, {"generate_mips", false}};
    CHECK(panel.ApplyImportSettings());
    CHECK(panel.import_error.empty() && !panel.import_status.empty());
    AssetMeta meta;
    CHECK(LoadAssetMeta(paths.content / "textures/checker_a.png.ameta", meta));
    CHECK(meta.settings == panel.import_draft);
    const assets::AssetRecord* record = panel.Database()->FindByPath("textures/checker_a.png");
    CHECK(record != nullptr && !record->needs_import);

    // Reimport by itself: the same settings, so the derived data cache answers.
    CHECK(panel.Reimport("textures/checker_a.png", &error) && error.empty());
    CHECK(panel.import_status.find("cache") != std::string::npos);

    // Nothing to configure: a folder, a kind with no importer, a name that isn't there.
    CHECK(!panel.OpenImportSettings("textures", &error) && !error.empty());
    CHECK(!panel.OpenImportSettings("scripts/tool.luau", &error) && error.find("Script") != std::string::npos);
    CHECK(!panel.OpenImportSettings("textures/nope.png", &error) && !error.empty());

    // The drawer draws its widgets and its status without tripping an ImGui id clash.
    CHECK(panel.OpenImportSettings("textures/checker_a.png", &error));
    for (int frame = 0; frame < 2; ++frame) imgui.Frame([&] { panel.Draw(); });
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    std::error_code ignore;
    stdfs::remove_all(project, ignore);
}
