#include "core/editor_document.h"

namespace aether::editor {

std::string EditorDocument::DisplayName() const {
    const std::filesystem::path& path = FilePath();
    const std::string name = path.filename().string(); // empty for "" and for a path ending in a separator
    return name.empty() ? "Untitled" : name;
}

} // namespace aether::editor
