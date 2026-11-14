#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace aether::editor {

// Stage 1 of docs/design/EDITOR_PARITY_AUDIT.md: the Scene toolbar's recents list. Scenes opened or saved recently,
// newest first, kept per project in Saved/recent_scenes.json. Entries are the paths the toolbar's path box holds --
// relative to the project's Content folder, or absolute for a scene from elsewhere -- not resolved paths, so the list
// reads the way the box does.
class RecentScenes {
public:
    static constexpr std::size_t kMaxEntries = 10;

    // A missing or unreadable file just means an empty list.
    void Load(const std::filesystem::path& file);
    bool Save(const std::filesystem::path& file) const;

    // Moves `path` to the front (adding it if new), dropping the oldest past kMaxEntries.
    void Add(const std::string& path);
    void Remove(const std::string& path);
    // Drops entries whose file does not exist under `content_root`; returns how many.
    std::size_t RemoveMissing(const std::filesystem::path& content_root);

    const std::vector<std::string>& Entries() const { return entries_; }

private:
    std::vector<std::string> entries_;
};

} // namespace aether::editor
