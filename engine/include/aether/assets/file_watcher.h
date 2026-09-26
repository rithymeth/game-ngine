#pragma once

#include "aether/core/base.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace aether::assets {

struct FileChange {
    enum class Kind { Added, Modified, Removed };
    Kind kind = Kind::Modified;
    std::string path; // relative to the watched folder, '/'-separated
};

// Watches a folder tree for file changes (Phase 8, docs/design/PHASE_SPECS.md
// §8.3), with debouncing: a file's change is reported once it has been quiet
// for `debounce` (default 200 ms). Editors often save in several steps —
// write a temp file, then rename it over the original — and those steps
// collapse into one change: the temp file's add and remove cancel out, and
// the original is reported once, as Modified.
//
// It polls: each Poll() compares the folder against the previous snapshot
// (modification time and size). That works everywhere and is fine for
// editor-sized content folders; native notifications (ReadDirectoryChangesW,
// inotify) can later replace the walk without changing this interface.
// Hidden files and folders (starting with '.') are ignored.
class FileWatcher {
public:
    using Clock = std::chrono::steady_clock;

    // Takes the starting snapshot: files already there aren't reported.
    explicit FileWatcher(std::filesystem::path root,
                         std::chrono::milliseconds debounce = std::chrono::milliseconds(200));

    // Checks for changes, and returns those that have settled by `now`,
    // sorted by path. `now` is passed in so tests (and a paused editor) control
    // time.
    std::vector<FileChange> Poll(Clock::time_point now);

    // Takes the file's current state as known, without reporting it; for
    // files this process wrote itself (e.g. an .ameta after an import).
    void Refresh(const std::string& path);

    // Changes seen but not yet settled.
    usize PendingCount() const { return pending_.size(); }
    const std::filesystem::path& Root() const { return root_; }

private:
    struct FileState {
        std::filesystem::file_time_type written;
        std::uintmax_t size = 0;
        bool operator==(const FileState& o) const { return written == o.written && size == o.size; }
    };
    struct Pending {
        FileChange::Kind kind;
        Clock::time_point last_seen;
    };

    std::map<std::string, FileState> Snapshot() const;
    void Note(const std::string& path, FileChange::Kind kind, Clock::time_point now);

    std::filesystem::path root_;
    std::chrono::milliseconds debounce_;
    std::map<std::string, FileState> files_;
    std::map<std::string, Pending> pending_;
};

} // namespace aether::assets
