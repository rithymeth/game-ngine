#include "test_framework.h"
#include "save/save_inspector_document.h"
#include "save/save_inspector_panel.h"
#include "ui/extensions.h"
#include "workspace/editor_workspace.h"

#include "aether/reflection/registry.h"
#include "aether/save/save_bag.h"
#include "aether/save/save_system.h"

#include <imgui.h>
#include <imgui_internal.h>

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

// Exercise the actual ImGui controls rather than calling the document actions.
bool ClickSaveControl(HeadlessImGui& ui, const std::function<void()>& draw,
                       const char* window_name, const char* button) {
    auto* window = ImGui::FindWindowByName(window_name);
    if (!window) return false;
    const ImGuiID id = window->GetID(button);
    const auto clip = window->InnerClipRect;
    auto& io = ImGui::GetIO();
    for (float y = clip.Min.y + 3; y < std::min(clip.Min.y + 72.0f, clip.Max.y); y += 6) {
        for (float x = clip.Min.x + 4; x < std::min(clip.Min.x + 330.0f, clip.Max.x); x += 10) {
            io.AddMousePosEvent(x, y);
            ui.Frame(draw);
            if (ImGui::GetHoveredID() != id) continue;
            io.AddMouseButtonEvent(0, true);
            ui.Frame(draw);
            io.AddMouseButtonEvent(0, false);
            ui.Frame(draw);
            return true;
        }
    }
    return false;
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

AETHER_TEST(SaveInspector_TreeOnlyReportsTruncationWhenNodesWereSkipped) {
    HeadlessImGui ui;
    const auto file = Dir("tree_cap_notice") / "slot.asav";
    SaveInspectorDocument doc;
    SaveInspectorPanel panel(doc);
    std::string rendered;
    auto& platform = ImGui::GetPlatformIO();
    platform.Platform_ClipboardUserData = &rendered;
    platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
        *static_cast<std::string*>(ImGui::GetPlatformIO().Platform_ClipboardUserData) = text;
    };

    // Root object and its array consume two of the visible node slots.
    const usize values = SaveInspectorPanel::kMaxTreeNodes - 2;
    for (bool over : {false, true}) {
        reflect::Json data = reflect::Json::array();
        for (usize i = 0; i < values + (over ? 1 : 0); ++i) data.push_back(i);
        reflect::Json envelope = {{"$type", "aether.save"}, {"format", 1}, {"type", "UnknownTreeFixture"},
                                  {"data", {{"items", std::move(data)}}}};
        Spit(file, envelope.dump());
        if (!doc.Open(file)) { CHECK(false); return; }
        ui.Frame([&] { panel.Draw(); });
        ui.Frame([&] {
            ImGui::LogToClipboard();
            panel.Draw();
            ImGui::LogFinish();
        });
        const std::string last_visible = "[" + std::to_string(values - 1) + "]: ";
        const std::string first_hidden = "[" + std::to_string(values) + "]: ";
        CHECK(rendered.find(last_visible) != std::string::npos && rendered.find(first_hidden) == std::string::npos);
        CHECK((rendered.find("truncated") != std::string::npos) == over);
    }
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

AETHER_TEST(SaveInspector_DeletePreservesPrimaryWhenBackupIsBlocked) {
    const auto file = MakeBagSave(Dir("delete_blocked_backup"));
    const auto bytes = Slurp(file);
    auto backup = file;
    backup += ".bak";
    CHECK(stdfs::remove(backup));
    CHECK(stdfs::create_directory(backup));
    Spit(backup / "keep.txt", "not a backup");
    SaveInspectorDocument doc;
    CHECK(doc.Open(file));
    CHECK(!doc.DeleteFile());
    CHECK(doc.IsOpen() && doc.File() == file);
    CHECK(!bytes.empty() && Slurp(file) == bytes);
    CHECK(Slurp(backup / "keep.txt") == "not a backup");
}

AETHER_TEST(SaveInspector_DeletePreservesBackupWhenPrimaryIsADirectory) {
    const auto file = MakeBagSave(Dir("delete_primary_directory"));
    auto backup = file;
    backup += ".bak";
    const auto bytes = Slurp(backup);
    SaveInspectorDocument doc;
    CHECK(doc.Open(file));
    CHECK(stdfs::remove(file));
    CHECK(stdfs::create_directory(file));
    CHECK(!doc.DeleteFile());
    CHECK(doc.IsOpen() && doc.File() == file);
    CHECK(stdfs::is_directory(file));
    CHECK(!bytes.empty() && Slurp(backup) == bytes);
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

AETHER_TEST(SaveInspector_CopyPreservesBackupOnlyDestinations) {
    const auto dir = Dir("copy_backup_only");
    const auto source = MakeBagSave(dir);
    const auto target_backup = dir / "slot2.asav.bak";
    SaveSystem saves(dir);
    SaveBag original;
    original.SetInt("coins", 84);
    if (!saves.Save("slot2", original).ok) { CHECK(false); return; }
    stdfs::rename(dir / "slot2.asav", target_backup);
    const auto bytes = Slurp(target_backup);

    SaveInspectorDocument doc;
    CHECK(doc.Open(source));
    std::string error;
    CHECK(!doc.CopyTo("slot2", &error) && !error.empty());
    CHECK(!stdfs::exists(dir / "slot2.asav") && Slurp(target_backup) == bytes);
    CHECK(doc.IsOpen() && doc.File() == source);
}

AETHER_TEST(SaveInspector_CopyReportsBackupFailureWithoutLeavingAPrimary) {
    const auto dir = Dir("copy_failed_backup");
    const auto source = MakeBagSave(dir);
    const auto backup = dir / "slot1.asav.bak";
    const auto source_bytes = Slurp(source);
    stdfs::remove(backup);
    stdfs::create_directory(backup);
    Spit(backup / "keep.txt", "not a backup");
    SaveInspectorDocument doc;
    CHECK(doc.Open(source));
    std::string error;
    CHECK(!doc.CopyTo("slot2", &error) && error.find("backup") != std::string::npos);
    CHECK(!stdfs::exists(dir / "slot2.asav") && !stdfs::exists(dir / "slot2.asav.bak"));
    CHECK(Slurp(source) == source_bytes && Slurp(backup / "keep.txt") == "not a backup");
    CHECK(doc.IsOpen() && doc.File() == source);
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

AETHER_TEST(SaveInspector_RescuesAnOrphanedBackupThroughCopyAs) {
    const auto dir = Dir("panel_orphan_backup");
    const auto paired = MakeBagSave(dir);
    auto paired_backup = paired;
    paired_backup += ".bak";
    const auto orphan = dir / "orphan.asav.bak";
    CHECK(stdfs::copy_file(paired_backup, orphan));
    const auto original = Slurp(orphan);
    CHECK(!original.empty());
    Spit(dir / "notes.bak", "not a save");

    SaveInspectorDocument doc;
    SaveInspectorPanel panel(doc);
    panel.SetDirectory(dir);
    const auto& entries = panel.Entries();
    CHECK(entries.size() == 2);
    if (entries.size() == 2) {
        CHECK(entries[0].file == "orphan.asav.bak" && entries[0].valid && entries[0].checksum_ok);
        CHECK(entries[1].file == "slot1.asav" && entries[1].valid && entries[1].checksum_ok);
    }
    CHECK(doc.Open(orphan));
    CHECK(!panel.CopyOpen("orphan")); // don't reuse a name the backup already owns
    CHECK(panel.CopyOpen("recovered"));
    CHECK(!stdfs::exists(dir / "recovered.bak") && !stdfs::exists(dir / "orphan.asav"));
    CHECK(Slurp(dir / "recovered.asav") == original && Slurp(orphan) == original);
    SaveSystem saves(dir);
    SaveBag loaded;
    CHECK(saves.Load("recovered", loaded).ok && loaded.GetInt("coins") == 42);
    CHECK(!panel.DeleteOpen()); // never delete the only copy from this view
    CHECK(doc.IsOpen() && doc.File() == orphan && Slurp(orphan) == original);
}

AETHER_TEST(SaveInspector_OpensPairedBackupOfADamagedPrimary) {
    const auto dir = Dir("panel_paired_backup");
    SaveSystem saves(dir);
    SaveBag bag;
    bag.SetInt("coins", 17);
    CHECK(saves.Save("paired", bag).ok);
    bag.SetInt("coins", 99);
    CHECK(saves.Save("paired", bag).ok);
    const auto primary = dir / "paired.asav";
    const auto backup = dir / "paired.asav.bak";
    const auto original = Slurp(backup);
    CHECK(!original.empty());
    Spit(primary, "damaged primary");

    HeadlessImGui ui;
    SaveInspectorDocument doc;
    SaveInspectorPanel panel(doc);
    panel.SetDirectory(dir);
    CHECK(!doc.Open(primary));
    ImVec2 button{};
    const auto draw = [&] {
        // Open backup follows Refresh and Reload on the toolbar.
        const auto origin = ImGui::GetCursorScreenPos();
        const auto& style = ImGui::GetStyle();
        const auto width = [&](const char* label) {
            return ImGui::CalcTextSize(label).x + 2 * style.FramePadding.x;
        };
        button = {origin.x + width("Refresh") + width("Reload") + 2 * style.ItemSpacing.x + width("Open backup") / 2,
                  origin.y + ImGui::GetFrameHeight() / 2};
        panel.Draw();
    };
    ui.Frame(draw);
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(button.x, button.y);
    ui.Frame(draw);
    io.AddMouseButtonEvent(0, true);
    ui.Frame(draw);
    io.AddMouseButtonEvent(0, false);
    ui.Frame(draw);
    if (doc.File() != backup) { CHECK(doc.File() == backup); return; }
    CHECK(doc.IsBackupFile() && doc.Info().checksum_ok);
    CHECK(panel.CopyOpen("rescued"));
    CHECK(!stdfs::exists(dir / "rescued.asav.bak"));
    CHECK(!panel.DeleteOpen());
    CHECK(Slurp(primary) == "damaged primary" && Slurp(backup) == original);
    SaveBag recovered;
    const auto loaded = saves.Load("rescued", recovered);
    CHECK(loaded.ok && loaded.warnings.empty() && recovered.GetInt("coins") == 17);
}

AETHER_TEST(SaveInspector_CopyErrorIsVisibleInsideTheModal) {
    HeadlessImGui ui;
    const auto file = MakeBagSave(Dir("panel_copy_error"));
    SaveInspectorDocument doc;
    SaveInspectorPanel panel(doc);
    panel.SetDirectory(file.parent_path());
    CHECK(doc.Open(file));
    std::string rendered;
    auto& platform = ImGui::GetPlatformIO();
    platform.Platform_ClipboardUserData = &rendered;
    platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
        *static_cast<std::string*>(ImGui::GetPlatformIO().Platform_ClipboardUserData) = text;
    };

    const auto draw = [&] { panel.Draw(); };
    ui.Frame(draw);
    if (!ClickSaveControl(ui, draw, "Test", "Copy as...")) { CHECK(false); return; }
    std::string reason;
    CHECK(!panel.CopyOpen("slot1", &reason) && !reason.empty());
    ui.Frame([&] {
        ImGui::LogToClipboard();
        panel.Draw();
        ImGui::LogFinish();
        CHECK(ImGui::IsPopupOpen("Copy save"));
    });
    const auto input = rendered.find("New name");
    const auto error = rendered.find(reason);
    CHECK(input != std::string::npos && error != std::string::npos);
    CHECK(error > input); // the message was drawn inside the popup, after its input
}

AETHER_TEST(SaveInspector_ChangingTheOpenFileCancelsCopyAs) {
    HeadlessImGui ui;
    const auto root = Dir("panel_copy_switch");
    const auto original = MakeBagSave(root / "first");
    const auto replacement = MakeBagSave(root / "second");
    SaveBag updated;
    updated.SetInt("coins", 73);
    CHECK(SaveSystem(replacement.parent_path()).Save("slot1", updated).ok);
    CHECK(Slurp(original) != Slurp(replacement));
    SaveInspectorDocument doc;
    SaveInspectorPanel panel(doc);
    CHECK(doc.Open(original));

    const auto draw = [&] { panel.Draw(); };
    ui.Frame(draw);
    if (!ClickSaveControl(ui, draw, "Test", "Copy as...")) { CHECK(false); return; }
    ui.Frame([&] {
        draw();
        CHECK(ImGui::IsPopupOpen("Copy save"));
    });
    if (!ClickSaveControl(ui, draw, "Copy save", "New name")) { CHECK(false); return; }
    ImGui::GetIO().AddInputCharactersUTF8("old_choice");
    ui.Frame(draw);

    CHECK(doc.Open(replacement));
    bool cancelled = false;
    ui.Frame([&] {
        draw();
        cancelled = !ImGui::IsPopupOpen("Copy save");
        CHECK(cancelled);
    });
    if (!cancelled) return;
    CHECK(!stdfs::exists(original.parent_path() / "old_choice.asav"));
    CHECK(!stdfs::exists(replacement.parent_path() / "old_choice.asav"));

    if (!ClickSaveControl(ui, draw, "Test", "Copy as...")) { CHECK(false); return; }
    ui.Frame(draw);
    if (!ClickSaveControl(ui, draw, "Copy save", "New name")) { CHECK(false); return; }
    ImGui::GetIO().AddInputCharactersUTF8("fresh");
    ui.Frame(draw);
    if (!ClickSaveControl(ui, draw, "Copy save", "Copy")) { CHECK(false); return; }
    const auto copy = replacement.parent_path() / "fresh.asav";
    CHECK(stdfs::exists(copy) && Slurp(copy) == Slurp(replacement));
    CHECK(!stdfs::exists(original.parent_path() / "fresh.asav"));
}

AETHER_TEST(SaveInspector_ChangingTheOpenFileCancelsDeleteConfirmation) {
    HeadlessImGui ui;
    const auto original = MakeBagSave(Dir("panel_delete_switch") / "first");
    const auto replacement = MakeBagSave(original.parent_path().parent_path() / "second");
    SaveInspectorDocument doc;
    SaveInspectorPanel panel(doc);
    CHECK(doc.Open(original));

    ImVec2 delete_button;
    const auto draw = [&] {
        delete_button = ImGui::GetCursorScreenPos();
        const auto& style = ImGui::GetStyle();
        for (const char* label : {"Refresh", "Reload", "Open backup"})
            delete_button.x += ImGui::CalcTextSize(label).x + 2 * style.FramePadding.x + style.ItemSpacing.x;
        delete_button.x += 10.0f;
        delete_button.y += ImGui::GetFrameHeight() * 0.5f;
        panel.Draw();
    };
    ui.Frame(draw);
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(delete_button.x, delete_button.y);
    ui.Frame(draw);
    io.AddMouseButtonEvent(0, true);
    ui.Frame(draw);
    io.AddMouseButtonEvent(0, false);
    ui.Frame([&] {
        draw();
        CHECK(ImGui::IsPopupOpen("Delete save?"));
    });

    CHECK(doc.Open(replacement));
    ui.Frame([&] {
        draw();
        CHECK(!ImGui::IsPopupOpen("Delete save?"));
    });
    CHECK(stdfs::exists(original) && stdfs::exists(replacement));
    CHECK(stdfs::exists(original.string() + ".bak") && stdfs::exists(replacement.string() + ".bak"));
}

AETHER_TEST(SaveInspector_ConfirmDeleteRejectsACompletedSaveToTheSameSlot) {
    for (const bool backup_only : {false, true}) {
        HeadlessImGui ui;
        const auto file = MakeBagSave(Dir(backup_only ? "confirm_backup_write" : "confirm_primary_write"));
        const auto backup = stdfs::path(file.string() + ".bak");
        SaveInspectorDocument doc;
        SaveInspectorPanel panel(doc);
        CHECK(doc.Open(file));
        const auto revision = doc.Revision();
        const auto draw = [&] { panel.Draw(); };
        ui.Frame(draw);
        if (!ClickSaveControl(ui, draw, "Test", "Delete...")) { CHECK(false); return; }
        ui.Frame(draw);

        SaveBag updated;
        updated.SetInt("coins", 73); // constant-width change, so file size is not a reliable identity
        updated.SetBool("done", true);
        SaveSystem saves(file.parent_path());
        if (backup_only) {
            CHECK(saves.Save("other", updated).ok);
            Spit(backup, Slurp(file.parent_path() / "other.asav"));
        } else {
            CHECK(saves.Save("slot1", updated).ok);
        }
        const auto primary_after_write = Slurp(file);
        const auto backup_after_write = Slurp(backup);
        CHECK(doc.Revision() == revision);
        if (!ClickSaveControl(ui, draw, "Delete save?", "Delete")) { CHECK(false); continue; }
        const bool preserved = stdfs::exists(file) && stdfs::exists(backup) &&
                               Slurp(file) == primary_after_write && Slurp(backup) == backup_after_write;
        CHECK(preserved);
        if (!preserved) continue;

        // The rejected confirmation closes. A fresh review can delete an unchanged pair.
        ui.Frame([&] { draw(); CHECK(!ImGui::IsPopupOpen("Delete save?")); });
        CHECK(doc.Reload());
        if (!ClickSaveControl(ui, draw, "Test", "Delete...")) { CHECK(false); continue; }
        ui.Frame(draw);
        CHECK(ClickSaveControl(ui, draw, "Delete save?", "Delete"));
        CHECK(!stdfs::exists(file) && !stdfs::exists(backup));
    }
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
