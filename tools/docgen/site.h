#pragma once

#include <filesystem>
#include <string>

namespace aether::docs {

// Generates a dependency-free, offline documentation site from Markdown files.
// The API reference is included from `api/API.md` in output_directory.
bool WriteDocumentationSite(const std::filesystem::path& docs_directory,
                            const std::filesystem::path& output_directory,
                            std::string* error = nullptr);

} // namespace aether::docs
