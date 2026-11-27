#pragma once

#include <filesystem>
#include <string_view>

namespace aether::editor {

// Writes to a unique sibling staging directory, then replaces the target only
// after the new file closes successfully. Does not create missing parent folders.
bool WriteStagedAsset(const std::filesystem::path& path, std::string_view contents);

} // namespace aether::editor
