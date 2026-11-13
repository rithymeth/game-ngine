#include "core/file_dialog.h"
#include "workspace/editor_workspace.h"
#include "world/scene_panel.h"

#include "test_framework.h"

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

// The native file dialog behind one interface: the panel and the File menu pick a scene through it, and a stand-in
// answers for the platform here.

using namespace aether;
using namespace aether::editor;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 960);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }

private:
    ImGuiContext* context_ = nullptr;
};

// Answers with `answer`, and records what it was asked, so a test can see the options the panel builds.
class FakeFileDialog final : public FileDialog {
public:
    bool available = true;
    std::filesystem::path answer;
    FileDialogOptions last;
    int shows = 0;

    bool Available() const override { return available; }
    std::filesystem::path Show(const FileDialogOptions& options) override {
        last = options;
        ++shows;
        return answer;
    }
};

stdfs::path Scratch(const char* name) {
    const stdfs::path dir = stdfs::temp_directory_path() / name;
    std::error_code ignored;
    stdfs::remove_all(dir, ignored);
    stdfs::create_directories(dir / "Scenes");
    return dir;
}

// A scene file with `entities` entities, written under `file`.
bool WriteScene(const stdfs::path& file, const char* name) {
    SceneDocument document;
    document.CreateEntity(name);
    return document.Save(file);
}

} // namespace

// The picker sees what it needs to be useful: the right verb, the scene types to filter, and the open scene's own file
// as the starting point; what it returns is shown relative to the content folder, which is how scenes are named there.
AETHER_TEST(FileDialog_OpenPicksASceneUnderTheContentFolder) {
    const stdfs::path root = Scratch("aether_file_dialog_open");
    CHECK(WriteScene(root / "Scenes/A.ascene", "crate"));

    ScenePanel panel(root);
    FakeFileDialog picker;
    picker.answer = root / "Scenes/A.ascene";
    panel.SetFileDialog(&picker);

    const std::optional<std::string> picked = panel.PickSceneFile(false);
    CHECK(picked && *picked == "Scenes/A.ascene");
    CHECK(picker.shows == 1);
    CHECK(picker.last.save == false);
    CHECK(picker.last.title == "Open scene");
    CHECK(picker.last.default_extension == "ascene");
    CHECK(!picker.last.filters.empty() && picker.last.filters[0].extensions == "ascene;aesc");
    CHECK(picker.last.start_path == root / "Scenes/Main.ascene"); // the path box's default, until the scene has a file
    CHECK(picker.last.default_name == "Main.ascene");

    CHECK(panel.OpenScene(*picked, true));
    CHECK(panel.Document().Entities().size() == 1);
    CHECK(panel.Document().FilePath() == root / "Scenes/A.ascene");

    // With a scene open, the next pick starts where that scene lives.
    picker.answer = root / "Scenes/A.ascene";
    (void)panel.PickSceneFile(false);
    CHECK(picker.last.start_path == root / "Scenes/A.ascene");
    CHECK(picker.last.default_name == "A.ascene");
}

AETHER_TEST(FileDialog_SaveAsWritesThePickedName) {
    const stdfs::path root = Scratch("aether_file_dialog_save");
    ScenePanel panel(root);
    panel.Document().CreateEntity("crate");
    FakeFileDialog picker;
    picker.answer = root / "Scenes/New.ascene";
    panel.SetFileDialog(&picker);

    const std::optional<std::string> picked = panel.PickSceneFile(true);
    CHECK(picked && *picked == "Scenes/New.ascene");
    CHECK(picker.last.save == true);
    CHECK(picker.last.title == "Save scene");
    CHECK(picker.last.default_name == "Main.ascene");

    // The picker asked about replacing a file, so the panel is told to overwrite and does not ask a second time.
    CHECK(panel.SaveScene(*picked, /*overwrite=*/true));
    CHECK(stdfs::exists(root / "Scenes/New.ascene"));
    CHECK(panel.Path() == "Scenes/New.ascene");
    CHECK(panel.Document().FilePath() == root / "Scenes/New.ascene");
    CHECK(!panel.Document().IsDirty());
}

// A cancel and "this platform has no picker" are different answers: the first is the user's choice, the second sends the
// caller back to its typed path.
AETHER_TEST(FileDialog_CancelAndNoPickerAreDifferent) {
    const stdfs::path root = Scratch("aether_file_dialog_cancel");
    ScenePanel panel(root);
    CHECK(!panel.PickSceneFile(false).has_value()); // no picker at all

    FakeFileDialog picker;
    picker.available = false;
    panel.SetFileDialog(&picker);
    CHECK(!panel.PickSceneFile(false).has_value());
    CHECK(picker.shows == 0); // an unavailable picker is not asked and does not block

    picker.available = true;
    const std::optional<std::string> cancelled = panel.PickSceneFile(false);
    CHECK(cancelled.has_value() && cancelled->empty());
    CHECK(panel.Document().FilePath().empty()); // cancelling leaves the scene alone
}

// A scene outside the project keeps its absolute path: it is not under the content folder, so it has no scene name there.
AETHER_TEST(FileDialog_ASceneOutsideTheContentFolderStaysAbsolute) {
    const stdfs::path root = Scratch("aether_file_dialog_inside");
    const stdfs::path elsewhere = Scratch("aether_file_dialog_elsewhere");
    CHECK(WriteScene(elsewhere / "Level.ascene", "crate"));

    ScenePanel panel(root);
    FakeFileDialog picker;
    picker.answer = elsewhere / "Level.ascene";
    panel.SetFileDialog(&picker);

    const std::optional<std::string> picked = panel.PickSceneFile(false);
    CHECK(picked && *picked == (elsewhere / "Level.ascene").generic_string());
    CHECK(panel.OpenScene(*picked, true));
    CHECK(panel.Document().FilePath() == elsewhere / "Level.ascene");
}

// The File menu drives the same picker: Open reads what it returns, Save As writes it, and a cancel changes nothing.
AETHER_TEST(EditorWorkspace_OpenAndSaveAsUseThePicker) {
    HeadlessImGui imgui;
    const stdfs::path root = Scratch("aether_file_dialog_menu");
    CHECK(WriteScene(root / "Scenes/Other.ascene", "from the picker"));

    EditorWorkspace workspace;
    workspace.Scene().SetContentRoot(root);
    FakeFileDialog picker;
    workspace.Scene().SetFileDialog(&picker);

    workspace.Scene().Document().CreateEntity("to save");
    picker.answer = root / "Scenes/FromMenu.ascene";
    CHECK(workspace.RunCommand("scene.save_as"));
    CHECK(picker.last.save);
    CHECK(stdfs::exists(root / "Scenes/FromMenu.ascene"));
    CHECK(!workspace.Scene().Document().IsDirty());

    // A cancel leaves the document where it was, with no status line to explain a dialog the user closed.
    picker.answer.clear();
    CHECK(workspace.RunCommand("scene.save_as"));
    CHECK(workspace.Scene().Document().FilePath() == root / "Scenes/FromMenu.ascene");

    // Open is the same: the picked file becomes the open scene and the box's name.
    picker.answer = root / "Scenes/Other.ascene";
    CHECK(workspace.RunCommand("scene.open"));
    CHECK(picker.last.save == false);
    CHECK(workspace.Scene().Path() == "Scenes/Other.ascene");
    CHECK(workspace.Scene().Document().Entities().size() == 1);
    CHECK(workspace.Scene().Document().Name(workspace.Scene().Document().Entities().front()) == "from the picker");
}

// Where there is no picker, the menu's commands keep the typed-path behaviour they had: Open reads the box's name and
// Save As writes it, so Linux without zenity still works.
AETHER_TEST(EditorWorkspace_WithoutAPickerTheTypedPathIsUsed) {
    HeadlessImGui imgui;
    const stdfs::path root = Scratch("aether_file_dialog_typed");
    EditorWorkspace workspace;
    workspace.Scene().SetContentRoot(root);
    FakeFileDialog picker;
    picker.available = false;
    workspace.Scene().SetFileDialog(&picker);

    workspace.Scene().Document().CreateEntity("typed");
    CHECK(workspace.RunCommand("scene.save_as"));
    CHECK(stdfs::exists(root / "Scenes/Main.ascene"));
    CHECK(workspace.Scene().Document().FilePath() == root / "Scenes/Main.ascene");
}
