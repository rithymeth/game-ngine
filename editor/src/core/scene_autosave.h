#pragma once

#include "aether/core/base.h"

#include <filesystem>
#include <string>

namespace aether::editor {

class SceneDocument;

// The one recovery autosave the editor keeps per project, and what it
// remembers about it. There is a single slot, replaced in place: what recovery
// offers is the last edit made before the editor stopped, not a list of every
// version of it.
struct SceneAutosaveSlot {
    std::filesystem::path scene;    // the autosaved .ascene
    std::filesystem::path metadata; // the .json beside it
    std::filesystem::path source;   // the scene it was saved from; empty when that scene had never been saved
    std::string saved;              // when it was written, "YYYY-MM-DD HH:MM UTC"
    usize entities = 0;
    bool IsValid() const { return !scene.empty(); }
};

// Where autosaves live and how they are found again. The base folder is per
// user (%APPDATA%/Aether/Autosave); each project gets a folder named after its
// file plus a hash of its path, so two projects with the same file name do not
// share a recovery.
class SceneAutosaves {
public:
    // AETHER_EDITOR_AUTOSAVE_DIR when set (tests point it at a scratch folder), else the per-user folder.
    static std::filesystem::path DefaultBaseDir();

    explicit SceneAutosaves(std::filesystem::path base = {});

    // Where the slot for `project` is, whether or not anything is in it.
    SceneAutosaveSlot SlotFor(const std::filesystem::path& project) const;
    // Writes `document` into its project's slot. The scene file is written atomically (as a save is), then the
    // metadata, so a crash mid-write leaves the previous autosave loadable or none, never a half-written one.
    bool Write(const SceneDocument& document, const std::filesystem::path& project, const std::filesystem::path& source,
               std::string* error = nullptr) const;
    // The slot to offer, or an invalid one: nothing there, or its scene was deleted. A missing or unreadable
    // metadata file still leaves a recoverable slot -- the scene is the thing that matters -- with an empty source.
    SceneAutosaveSlot Find(const std::filesystem::path& project) const;
    // Removes the slot; true when there was one.
    bool Clear(const std::filesystem::path& project) const;

private:
    std::filesystem::path base_;
};

} // namespace aether::editor
