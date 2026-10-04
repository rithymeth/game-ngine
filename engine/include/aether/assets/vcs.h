#pragma once

#include "aether/core/base.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Version control status (Phase 26 step 5, docs/design/PHASE_SPECS.md §26.7):
// which files in the project's repository are modified, added, deleted,
// untracked or in conflict, for the Content Browser's badges. Git is the
// system (assets and their .ameta sidecars are plain files, so a project is
// normally a git repository); the status comes from `git status`, and
// everything that interprets it is pure and testable.

namespace aether::assets {

enum class VcsState : u8 {
    Clean,      // tracked and unchanged (or not mentioned)
    Untracked,  // new, not added
    Renamed,
    Added,      // staged as new
    Modified,
    Deleted,
    Conflicted, // an unresolved merge conflict
};

// Worse states win when states combine (a folder shows its worst child).
inline int VcsSeverity(VcsState s) {
    switch (s) {
    case VcsState::Conflicted: return 6;
    case VcsState::Deleted: return 5;
    case VcsState::Modified: return 4;
    case VcsState::Renamed: return 3;
    case VcsState::Added: return 2;
    case VcsState::Untracked: return 1;
    case VcsState::Clean: return 0;
    }
    return 0;
}
inline VcsState WorseVcsState(VcsState a, VcsState b) { return VcsSeverity(a) >= VcsSeverity(b) ? a : b; }

const char* VcsStateName(VcsState state); // "Modified"
char VcsStateLetter(VcsState state);       // 'M'; ' ' for Clean

struct VcsStatus {
    bool available = false; // a repository was found and git answered
    std::string error;      // why not, when it wasn't
    // The project directory's path inside the repository ("MyGame/Content/",
    // or "" at the top), '/'-separated with a trailing slash.
    std::string prefix;
    // Changed files by their path from the repository root, '/'-separated.
    std::unordered_map<std::string, VcsState> files;

    // A file's state by its repository-relative path (Clean if unchanged).
    VcsState Of(const std::string& repo_path) const;
    // The worst state of everything under a repository-relative folder.
    VcsState OfFolder(const std::string& repo_folder) const;
    // The same, by path relative to the project directory this was queried in.
    VcsState OfLocal(const std::string& local_path) const { return Of(prefix + local_path); }
    VcsState OfLocalFolder(const std::string& local_folder) const;
    usize ChangedCount() const { return files.size(); }
};

// Parses `git status --porcelain=v1 -z` output ("XY path\\0", renames as
// "R  new\\0old\\0"). Ignored entries are left out.
VcsStatus ParseGitStatus(std::string_view output);

// Asks git about `directory` (and what is under it). On a directory outside
// any repository, or with no git installed, `available` is false and `error`
// says so; never throws.
VcsStatus QueryGitStatus(const std::filesystem::path& directory);

} // namespace aether::assets
