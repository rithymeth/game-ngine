#include "test_framework.h"
#include "save/save_inspector_document.h"
#include "save/save_inspector_panel.h"
#include "ui/extensions.h"
#include "workspace/editor_workspace.h"

#include "aether/reflection/registry.h"
#include "aether/save/save_bag.h"
#include "aether/save/save_system.h"

#include <imgui.h>

#include <filesystem>
#include <fstream>
#include <functional>

// The Save Inspector (Phase 28 step 5, §28.5).

using namespace aether;
using namespace aether::editor;
using namespace aether::save;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigMacOSXBehaviors = false;
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
        ImGui::SetNextWindowSize(ImVec2(1300, 800));
        ImGui::Begin("Test");
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

stdfs::path Dir(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_save_inspector_tests" / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    return dir;
}

std::string Slurp(const stdfs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void Spit(const stdfs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; }

// Two saves so a backup exists.
stdfs::path MakeBagSave(const stdfs::path& dir) {
    SaveSystem saves(dir);
    SaveBag bag;
    bag.SetInt("coins", 42);
    bag.SetBool("done", true);
    AETHER_CHECK(saves.Save("slot1", bag).ok);
    AETHER_CHECK(saves.Save("slot1", bag).ok);
    return dir / "slot1.asav";
}

} // namespace

AETHER_TEST(SaveInspector_OpensASaveBag) {
    const stdfs::path file = MakeBagSave(Dir("open"));
    SaveInspectorDocument doc;
    CHECK(!doc.IsOpen());
    CHECK(doc.Open(file));
    CHECK(doc.IsOpen() && doc.Info().ok && doc.Info().checksum_ok);
    CHECK(doc.Info().type == "SaveBag");
    CHECK(doc.HasBackup());
    CHECK(doc.Problems().empty());
    CHECK(doc.Type() != nullptr && doc.Instance() != nullptr);
    const u64 revision = doc.Revision();
    CHECK(doc.Reload());
    CHECK(doc.Revision() != revision);
}

AETHER_TEST(SaveInspector_TamperedFileIsFlagged) {
    const stdfs::path dir = Dir("tamper");
    const stdfs::path file = MakeBagSave(dir);
    std::string text = Slurp(file);
    const usize at = text.find("\"i\": 42");
    CHECK(at != std::string::npos);
    text.replace(at + 5, 2, "99");
    Spit(file, text);
    SaveInspectorDocument doc;
    CHECK(doc.Open(file));
    CHECK(!doc.Info().checksum_ok);
    CHECK(!doc.Problems().empty());
}

AETHER_TEST(SaveInspector_UnreadableFilesGiveAnError) {
    const stdfs::path dir = Dir("bad");
    Spit(dir / "junk.asav", "this is not json");
    Spit(dir / "empty.asav", "");
    SaveInspectorDocument doc;
    CHECK(!doc.Open(dir / "junk.asav"));
    CHECK(doc.IsOpen() && !doc.Info().ok && !doc.Info().error.empty());
    CHECK(doc.Instance() == nullptr);
    CHECK(!doc.Open(dir / "empty.asav"));
    CHECK(!doc.Open(dir / "missing.asav"));
    CHECK(!envelope::Inspect(dir / "junk.asav").ok);
}

AETHER_TEST(SaveInspector_UnknownTypeFallsBackToJson) {
    const stdfs::path dir = Dir("unknown");
    const stdfs::path file = MakeBagSave(dir);
    std::string text = Slurp(file);
    const usize at = text.find("SaveBag");
    CHECK(at != std::string::npos);
    text.replace(at, 7, "Nonsens");
    Spit(file, text);
    SaveInspectorDocument doc;
    doc.Open(file);
    CHECK(doc.Type() == nullptr && doc.Instance() == nullptr);
    CHECK(!doc.Problems().empty());
    CHECK(doc.Info().data.is_object());
}

AETHER_TEST(SaveInspector_TreeIsCapped) {
    reflect::Json big = reflect::Json::array();
    for (int i = 0; i < 20000; ++i) big.push_back(i);
    CHECK(CountJsonNodes(big, SaveInspectorPanel::kMaxTreeNodes) == SaveInspectorPanel::kMaxTreeNodes);
    reflect::Json small = {{"a", 1}, {"b", {1, 2}}};
    CHECK(CountJsonNodes(small, 100) == 5);
}

AETHER_TEST(SaveInspector_DeleteRemovesFileAndBackup) {
    const stdfs::path file = MakeBagSave(Dir("delete"));
    stdfs::path backup = file;
    backup += ".bak";
    CHECK(stdfs::exists(file) && stdfs::exists(backup));
    SaveInspectorDocument doc;
    doc.Open(file);
    CHECK(doc.DeleteFile());
    CHECK(!stdfs::exists(file) && !stdfs::exists(backup));
    CHECK(!doc.IsOpen());
    CHECK(!doc.DeleteFile());
}

AETHER_TEST(SaveInspector_CopyValidatesTheName) {
    const stdfs::path dir = Dir("copy");
    const stdfs::path file = MakeBagSave(dir);
    SaveInspectorDocument doc;
    doc.Open(file);
    std::string error;
    CHECK(!doc.CopyTo("../evil", &error) && !error.empty());
    CHECK(!doc.CopyTo("", &error));
    CHECK(!doc.CopyTo("slot1", &error)); // already taken
    CHECK(doc.CopyTo("slot2", &error));
    CHECK(stdfs::exists(dir / "slot2.asav"));
    SaveSystem saves(dir);
    SaveBag loaded;
    CHECK(saves.Load("slot2", loaded).ok);
    CHECK(!doc.CopyTo("slot2", &error)); // never overwrites
}

AETHER_TEST(SaveInspector_PanelListsAndDraws) {
    HeadlessImGui ui;
    const stdfs::path dir = Dir("panel");
    const stdfs::path file = MakeBagSave(dir);
    Spit(dir / "junk.asav", "nope");
    Spit(dir / "notes.txt", "ignored");
    SaveInspectorDocument doc;
    SaveInspectorPanel panel(doc);
    panel.SetDirectory(dir);
    CHECK(panel.Entries().size() == 2);
    doc.Open(file);
    for (int frame = 0; frame < 3; ++frame) ui.Frame([&] { panel.Draw(); });
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    doc.Open(dir / "junk.asav");
    ui.Frame([&] { panel.Draw(); });
    // A damaged file can still be copied raw (to keep it for a bug report).
    CHECK(panel.CopyOpen("slot9"));
    CHECK(stdfs::exists(dir / "slot9.asav"));
}

AETHER_TEST(SaveInspector_PanelDeleteRescans) {
    const stdfs::path dir = Dir("panel_delete");
    const stdfs::path file = MakeBagSave(dir);
    SaveInspectorDocument doc;
    SaveInspectorPanel panel(doc);
    panel.SetDirectory(dir);
    CHECK(panel.Entries().size() == 1);
    doc.Open(file);
    CHECK(panel.DeleteOpen());
    CHECK(panel.Entries().empty());
}

AETHER_TEST(SaveInspector_WorkspaceShowsSamplesAndOpensSaves) {
    HeadlessImGui ui;
    EditorWorkspace ws;
    const i64 tool = ws.FindTool("Save Inspector");
    CHECK(tool >= 0 && std::string(ws.ToolCategory(static_cast<usize>(tool))) == "Data");
    ws.Select(static_cast<usize>(tool));
    for (int frame = 0; frame < 2; ++frame) ui.Frame([&] { ws.DrawHubContents(); });
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    CHECK(ws.Extensions().FindAssetTypeByExtension(".asav") != nullptr);
    CHECK(ws.Extensions().FindAssetTypeByExtension(".asettings") != nullptr);
    const stdfs::path sample = stdfs::temp_directory_path() / "aether_editor_save_samples" / "damaged.asav";
    CHECK(stdfs::exists(sample));
    CHECK(ws.Extensions().OpenAsset(sample));
}
