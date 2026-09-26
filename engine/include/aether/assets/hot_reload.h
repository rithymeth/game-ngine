#pragma once

#include "aether/assets/file_watcher.h"
#include "aether/assets/importer.h"

#include <unordered_map>

namespace aether::assets {

// One asset affected by files changing on disk.
struct AssetChange {
    enum class Kind {
        Added,      // a new source appeared (and was imported, if it has an importer)
        Reimported, // its source or its .ameta settings changed; `output` is the new data
        Failed,     // (re)import failed; `output.error` says why. Keep using the old data.
                    // Reported once; retried when the file or its settings change.
        Moved,      // renamed or moved outside the editor (its .ameta went with it)
        Missing,    // its source was deleted (the .ameta is kept)
    };
    Kind kind = Kind::Reimported;
    AssetGuid guid;
    std::string path;
    std::string old_path; // Moved only
    ImportOutput output;  // Added, Reimported, Failed
    // Assets referring to it or to its sub-assets, which may need rebuilding
    // too (a material after its texture changed).
    std::vector<AssetGuid> dependents;
};

// Keeps the asset database and imported data in step with the content folder
// while the editor runs (docs/design/PHASE_SPECS.md §8.3): call Update() once
// per frame, before anything uses asset data, and apply the returned changes
// (e.g. AssetStore::Set for Reimported). Importing runs inside Update, so a
// frame with a large change may stall; moving imports to worker jobs comes
// with the job system.
class HotReloader {
public:
    HotReloader(AssetDatabase& database, const ImporterRegistry& importers, DerivedDataCache& cache,
                std::chrono::milliseconds debounce = std::chrono::milliseconds(200),
                std::string platform = "default");

    std::vector<AssetChange> Update(FileWatcher::Clock::time_point now);

    FileWatcher& Watcher() { return watcher_; }

private:
    AssetDatabase& database_;
    const ImporterRegistry& importers_;
    DerivedDataCache& cache_;
    std::string platform_;
    FileWatcher watcher_;
    std::unordered_map<AssetGuid, std::string> settings_; // last seen .ameta settings, to spot edits
    // Source hash + settings of imports that failed: not retried (and not
    // reported again) until the source or its settings change.
    std::unordered_map<AssetGuid, std::string> failed_;
};

} // namespace aether::assets
