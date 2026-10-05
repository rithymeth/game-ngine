#pragma once

#include "save/save_inspector_document.h"

#include <filesystem>
#include <string>
#include <vector>

namespace aether::editor {

// The Save Inspector (Phase 28 step 5, §28.5): a list of the save and
// settings files in a folder, the open file's header (kind, type and version,
// time, size, checksum, backup, and every problem found), and its contents:
// the saved struct drawn by the generic Inspector (read-only) when its type
// is registered in this build, otherwise a JSON tree; and the raw JSON text.
// Actions: Reload, Delete (with confirmation) and Copy as... Nothing here
// changes what a save contains.
class SaveInspectorPanel {
public:
    explicit SaveInspectorPanel(SaveInspectorDocument& document) : doc_(document) {}

    // The folder whose `.asav` and `.asettings` files are listed (rescanned now).
    void SetDirectory(const std::filesystem::path& directory);
    const std::filesystem::path& Directory() const { return directory_; }
    // Rescans the folder (also after a delete or copy).
    void Refresh();

    struct Entry {
        std::string file; // the file's name
        std::string type;
        u64 timestamp = 0;
        u64 bytes = 0;
        bool valid = false;    // a readable envelope
        bool checksum_ok = false;
    };
    const std::vector<Entry>& Entries() const { return entries_; }

    // The tree's size cap: a huge save can't stall a frame.
    static constexpr usize kMaxTreeNodes = 5000;

    void Draw();

    // What Draw's buttons do (callable directly).
    bool DeleteOpen();
    bool CopyOpen(const std::string& name, std::string* error = nullptr);

private:
    void DrawList();
    void DrawHeader();
    void DrawContents();

    SaveInspectorDocument& doc_;
    std::filesystem::path directory_;
    std::vector<Entry> entries_;
    std::string copy_name_;
    std::string message_;
    bool confirm_delete_ = false;
    bool open_copy_ = false;
};

// Counts the nodes a JSON tree of `value` would draw, up to `limit`
// (exposed so the cap can be tested).
usize CountJsonNodes(const reflect::Json& value, usize limit);

} // namespace aether::editor
