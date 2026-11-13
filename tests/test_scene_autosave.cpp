#include "core/scene_autosave.h"
#include "core/scene_document.h"
#include "test_framework.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

using namespace aether;
using namespace aether::editor;
namespace stdfs = std::filesystem;

namespace {

struct ScratchFolder {
    stdfs::path path;
    ScratchFolder() {
        static std::atomic<unsigned> next{0};
        path = stdfs::temp_directory_path() / "aether_scene_autosave_tests" / std::to_string(++next);
        std::error_code ec;
        stdfs::remove_all(path, ec);
        stdfs::create_directories(path, ec);
    }
    ~ScratchFolder() {
        std::error_code ec;
        stdfs::remove_all(path, ec);
    }
    stdfs::path project(const char* folder = "MyGame") const { return path / folder / (std::string(folder) + ".aproject"); }
    stdfs::path source(const char* folder = "MyGame") const { return path / folder / "Content" / "Scenes" / "Main.ascene"; }
};

} // namespace

AETHER_TEST(SceneAutosave_ASavedSceneIsFoundAndRecoversOverItsOwnName) {
    ScratchFolder scratch;
    const SceneAutosaves autosaves(scratch.path / "autosaves");
    SceneDocument document;
    document.CreateEntity("Unsaved marker");
    AETHER_CHECK(document.IsDirty());
    AETHER_CHECK(autosaves.Write(document, scratch.project(), scratch.source()));

    const SceneAutosaveSlot slot = autosaves.Find(scratch.project());
    AETHER_CHECK(slot.IsValid());
    AETHER_CHECK(slot.source == scratch.source());
    AETHER_CHECK(slot.entities == document.Entities().size());
    AETHER_CHECK(!slot.saved.empty());
    AETHER_CHECK(stdfs::exists(slot.metadata));

    SceneDocument recovered;
    std::string error;
    AETHER_CHECK(recovered.LoadAutosave(slot.scene, slot.source, &error));
    AETHER_CHECK(recovered.Entities().size() == document.Entities().size());
    AETHER_CHECK(recovered.IsDirty());        // not saved until the user saves it
    AETHER_CHECK(recovered.FilePath() == scratch.source()); // and then it goes to the scene it came from
}

AETHER_TEST(SceneAutosave_OneSlotPerProjectAndOneProjectPerSlot) {
    ScratchFolder scratch;
    const SceneAutosaves autosaves(scratch.path / "autosaves");
    const SceneAutosaveSlot game = autosaves.SlotFor(scratch.project("Game"));
    AETHER_CHECK(game.scene == autosaves.SlotFor(scratch.project("Game")).scene); // the same project, the same folder
    AETHER_CHECK(game.scene != autosaves.SlotFor(scratch.project("Other")).scene);
    // Two projects whose file is called the same still do not share a slot.
    const stdfs::path first = scratch.path / "First" / "Sample.aproject";
    const stdfs::path second = scratch.path / "Second" / "Sample.aproject";
    AETHER_CHECK(autosaves.SlotFor(first).scene != autosaves.SlotFor(second).scene);
}

AETHER_TEST(SceneAutosave_AnUntitledSceneKeepsAnEmptySource) {
    ScratchFolder scratch;
    const SceneAutosaves autosaves(scratch.path / "autosaves");
    SceneDocument document;
    document.CreateEntity("Unsaved marker");
    AETHER_CHECK(autosaves.Write(document, scratch.project(), {}));
    const SceneAutosaveSlot slot = autosaves.Find(scratch.project());
    AETHER_CHECK(slot.IsValid());
    AETHER_CHECK(slot.source.empty()); // it had never been saved, so recovering it must not name a file
    SceneDocument recovered;
    AETHER_CHECK(recovered.LoadAutosave(slot.scene, slot.source));
    AETHER_CHECK(recovered.FilePath().empty());
}

AETHER_TEST(SceneAutosave_ALaterAutosaveReplacesTheEarlierOne) {
    ScratchFolder scratch;
    const SceneAutosaves autosaves(scratch.path / "autosaves");
    SceneDocument document;
    document.CreateEntity("First");
    AETHER_CHECK(autosaves.Write(document, scratch.project(), scratch.source()));
    const usize first_count = document.Entities().size();
    document.CreateEntity("Second");
    AETHER_CHECK(autosaves.Write(document, scratch.project(), scratch.source()));
    const SceneAutosaveSlot slot = autosaves.Find(scratch.project());
    AETHER_CHECK(slot.entities == document.Entities().size());
    AETHER_CHECK(slot.entities == first_count + 1);
    SceneDocument recovered;
    AETHER_CHECK(recovered.LoadAutosave(slot.scene, slot.source));
    AETHER_CHECK(recovered.Entities().size() == first_count + 1);
}

AETHER_TEST(SceneAutosave_ClearRemovesTheSlotAndOnlyThenSaysSo) {
    ScratchFolder scratch;
    const SceneAutosaves autosaves(scratch.path / "autosaves");
    SceneDocument document;
    document.CreateEntity("Unsaved marker");
    AETHER_CHECK(autosaves.Write(document, scratch.project(), scratch.source()));
    AETHER_CHECK(autosaves.Clear(scratch.project()));
    AETHER_CHECK(!autosaves.Find(scratch.project()).IsValid());
    AETHER_CHECK(!autosaves.Clear(scratch.project())); // nothing left to remove
}

AETHER_TEST(SceneAutosave_AMissingSceneOrMetadataIsHandled) {
    ScratchFolder scratch;
    const SceneAutosaves autosaves(scratch.path / "autosaves");
    const stdfs::path project = scratch.project();
    SceneDocument document;
    document.CreateEntity("Unsaved marker");
    AETHER_CHECK(autosaves.Write(document, project, scratch.source()));
    SceneAutosaveSlot slot = autosaves.Find(project);

    // The details are the part that can be lost without losing the work: the scene alone is still recoverable.
    AETHER_CHECK(stdfs::remove(slot.metadata));
    SceneAutosaveSlot without_metadata = autosaves.Find(project);
    AETHER_CHECK(without_metadata.IsValid());
    AETHER_CHECK(without_metadata.source.empty());
    AETHER_CHECK(without_metadata.saved.empty());

    // Without the scene there is nothing to offer, whatever the metadata says.
    AETHER_CHECK(stdfs::remove(slot.scene));
    AETHER_CHECK(!autosaves.Find(project).IsValid());
}

AETHER_TEST(SceneAutosave_ADamagedAutosaveIsFoundButRefusesToLoad) {
    ScratchFolder scratch;
    const SceneAutosaves autosaves(scratch.path / "autosaves");
    const stdfs::path project = scratch.project();
    SceneDocument document;
    document.CreateEntity("Unsaved marker");
    AETHER_CHECK(autosaves.Write(document, project, scratch.source()));
    const SceneAutosaveSlot slot = autosaves.Find(project);
    { std::ofstream(slot.scene, std::ios::binary | std::ios::trunc) << "not a scene"; }
    AETHER_CHECK(autosaves.Find(project).IsValid()); // offered, so the user can at least discard it
    SceneDocument recovered;
    std::string error;
    AETHER_CHECK(!recovered.LoadAutosave(slot.scene, slot.source, &error));
    AETHER_CHECK(!error.empty());
}
