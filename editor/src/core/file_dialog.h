#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace aether::editor {

// One row in a picker's file-type list. Extensions are bare and separated by ';' ("ascene;aesc"); "*" is all files.
struct FileDialogFilter {
    std::string label;
    std::string extensions;
};

struct FileDialogOptions {
    std::string title = "Choose a file";
    // Where the picker starts: a folder (or a file, whose folder is used), empty for the platform's own default.
    std::filesystem::path start_path;
    // The name a save picker suggests (the folder still comes from start_path).
    std::string default_name;
    std::string default_extension; // no dot
    std::vector<FileDialogFilter> filters;
    bool save = false; // false: choose an existing file
};

// One native file picker, so every path field and save action goes through the same platform code and a test can stand
// in for the platform. Show() blocks until the user is done.
class FileDialog {
public:
    virtual ~FileDialog() = default;
    // False when the platform offers no picker the editor can drive (no IFileDialog, no zenity or kdialog). A caller
    // then keeps its own way of naming a file instead of calling Show().
    virtual bool Available() const = 0;
    // The absolute path the user chose, or an empty path when they cancelled.
    virtual std::filesystem::path Show(const FileDialogOptions& options) = 0;
};

// The platform's picker: the Windows common item dialog, zenity or kdialog on Linux, and nothing elsewhere.
std::unique_ptr<FileDialog> CreateNativeFileDialog();

} // namespace aether::editor
