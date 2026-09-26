#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace aether::editor {

// The editor's "recent projects" list (docs/design/EDITOR_UI.md §E.1 project
// browser), most recent first, persisted per user. Paths are stored as
// given, so callers should pass absolute paths.
class RecentProjects {
public:
    static constexpr std::size_t kMaxEntries = 10;

    // Per-user config folder: %APPDATA%/Aether on Windows,
    // $XDG_CONFIG_HOME/aether or ~/.config/aether elsewhere.
    static std::filesystem::path DefaultConfigDir();

    // A missing or unreadable file just means an empty list.
    void Load(const std::filesystem::path& file);
    bool Save(const std::filesystem::path& file) const;

    // Moves (or adds) `project_file` to the front, dropping the oldest past
    // kMaxEntries.
    void Add(const std::filesystem::path& project_file);
    void Remove(const std::filesystem::path& project_file);
    // Drops entries whose file no longer exists; returns how many.
    std::size_t RemoveMissing();

    const std::vector<std::filesystem::path>& Entries() const { return entries_; }

private:
    std::vector<std::filesystem::path> entries_;
};

} // namespace aether::editor
