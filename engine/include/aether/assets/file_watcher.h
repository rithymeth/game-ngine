#pragma once

#include "aether/core/base.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aether::assets {

struct FileChange {
    enum class Kind { Added, Modified, Removed };
    Kind kind = Kind::Modified;
    std::string path; // relative to the watched folder, '/'-separated
};

// Watches a folder tree for file changes (Phase 8
// §8.3), with debouncing: a file's change is reported once it has been quiet
// for `debounce` (default 200 ms). Editors often save in several steps —
// write a temp file, then rename it over the original — and those steps
// collapse into one change: the temp file's add and remove cancel out, and
// the original is reported once, as Modified.
//
// It compares the folder against the previous snapshot (modification time and
// size). In Mode::Poll every Poll() walks the whole tree, which works
// everywhere and costs about 2 ms per 500 files (measured: 23 ms at 5,000 and
// 149 ms at 30,000), so called once per frame it eats a frame at editor sizes.
// In Mode::Native the operating system says when something changed
// (inotify on Linux, ReadDirectoryChangesW on Windows) and Poll() walks the
// tree only then, and once every `kSafetyRescan` regardless, which also covers
// a notification queue that overflowed and folders the OS cannot watch. The
// walk, the debouncing and everything reported are the same in both modes.
// Mode::Native falls back to polling where it cannot be set up (macOS for now,
// no permission, too many watches): UsesNativeNotifications() says which.
// Hidden files and folders (starting with '.') are ignored.
class FileWatcher {
public:
    using Clock = std::chrono::steady_clock;
    enum class Mode { Poll, Native };
    // With native notifications, the longest the tree goes without a walk.
    static constexpr std::chrono::seconds kSafetyRescan{5};

    // Takes the starting snapshot: files already there aren't reported.
    explicit FileWatcher(std::filesystem::path root,
                         std::chrono::milliseconds debounce = std::chrono::milliseconds(200),
                         Mode mode = Mode::Poll);
    ~FileWatcher();
    FileWatcher(FileWatcher&&) noexcept;
    FileWatcher& operator=(FileWatcher&&) noexcept;

    // Checks for changes, and returns those that have settled by `now`,
    // sorted by path. `now` is passed in so tests (and a paused editor) control
    // time.
    std::vector<FileChange> Poll(Clock::time_point now);

    // Takes the file's current state as known, without reporting it; for
    // files this process wrote itself (e.g. an .ameta after an import).
    void Refresh(const std::string& path);

    // Changes seen but not yet settled.
    usize PendingCount() const { return pending_.size(); }
    // True when the OS is telling this watcher about changes (Mode::Native and it could be set up).
    bool UsesNativeNotifications() const { return native_ != nullptr; }
    // How many times the tree has been walked since construction (the cost this class exists to avoid).
    usize ScanCount() const { return scans_; }
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
    std::vector<FileChange> SettledBy(Clock::time_point now); // the pending changes that have been quiet for the debounce

    struct Native; // the OS notification handle; defined per platform in the .cpp

    std::filesystem::path root_;
    std::chrono::milliseconds debounce_;
    std::map<std::string, FileState> files_;
    std::map<std::string, Pending> pending_;
    std::unique_ptr<Native> native_;
    Clock::time_point last_scan_ = Clock::now();
    usize scans_ = 0;
};

} // namespace aether::assets
