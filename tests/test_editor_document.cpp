#include "core/editor_document.h"
#include "core/scene_document.h"
#include "test_framework.h"

#include <filesystem>
#include <string>

using namespace aether;
using namespace aether::editor;

namespace {

// The contract has to be implementable by something that is not a scene (Stage 6 migrates the other
// asset documents), so a stand-in with none of the scene's machinery lives here.
class FakeDocument : public EditorDocument {
public:
    const std::filesystem::path& FilePath() const override { return path; }
    const char* Kind() const override { return "fake"; }
    bool IsDirty() const override { return dirty; }
    bool CanUndo() const override { return false; }
    bool CanRedo() const override { return false; }
    std::string UndoLabel() const override { return {}; }
    std::string RedoLabel() const override { return {}; }
    bool Undo() override { return false; }
    bool Redo() override { return false; }
    bool Save(const std::filesystem::path& file = {}, std::string* = nullptr) override {
        if (!file.empty()) path = file;
        dirty = false;
        return true;
    }

    std::filesystem::path path;
    bool dirty = false;
};

} // namespace

// A scene answers the shell's quit prompt through the contract, and its undo history and save move
// through it unchanged.
AETHER_TEST(EditorDocument_TheSceneAnswersThroughTheContract) {
    SceneDocument scene;
    EditorDocument& document = scene;

    AETHER_CHECK(std::string(document.Kind()) == "scene");
    AETHER_CHECK(document.FilePath().empty());
    AETHER_CHECK(document.DisplayName() == "Untitled");
    AETHER_CHECK(!document.IsDirty()); // a fresh document has nothing to lose
    AETHER_CHECK(!document.CanUndo());

    scene.CreateEntity("Thing");
    AETHER_CHECK(document.IsDirty());
    AETHER_CHECK(document.CanUndo());
    AETHER_CHECK(document.UndoLabel() == "Create Thing");
    AETHER_CHECK(document.Undo()); // an undo back to the saved state is clean again
    AETHER_CHECK(!document.IsDirty());
    AETHER_CHECK(document.CanRedo());
    AETHER_CHECK(document.Redo());
    AETHER_CHECK(document.IsDirty());

    // Save-as through the contract names the document after the file and clears the dirty flag.
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "aether_editor_document_test.ascene";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    AETHER_CHECK(document.Save(path));
    AETHER_CHECK(document.FilePath() == path);
    AETHER_CHECK(document.DisplayName() == "aether_editor_document_test.ascene");
    AETHER_CHECK(!document.IsDirty());
    AETHER_CHECK(std::filesystem::exists(path));

    // A plain save writes to the path the document already has.
    scene.CreateEntity("Other");
    AETHER_CHECK(document.IsDirty());
    AETHER_CHECK(document.Save());
    AETHER_CHECK(!document.IsDirty());
    AETHER_CHECK(document.FilePath() == path);
    std::filesystem::remove(path, ec);
}

// The default DisplayName is the file name, and "Untitled" before a save or for a path that names no
// file; a document that is not a scene gets it without writing any of it.
AETHER_TEST(EditorDocument_DefaultNameIsTheFileNameOrUntitled) {
    FakeDocument document;
    AETHER_CHECK(document.DisplayName() == "Untitled");

    document.path = "Scenes/";
    AETHER_CHECK(document.DisplayName() == "Untitled");

    AETHER_CHECK(document.Save("Scenes/Main.ascene"));
    AETHER_CHECK(document.DisplayName() == "Main.ascene");
    AETHER_CHECK(std::string(document.Kind()) == "fake");
}
