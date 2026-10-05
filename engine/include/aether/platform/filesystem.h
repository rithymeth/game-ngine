#pragma once

#include "aether/core/base.h"

#include <string>
#include <vector>

namespace aether::fs {

bool Exists(const std::string& path);

// Reads the whole file into a byte buffer. Returns false (and leaves
// out_bytes untouched) if the file could not be opened.
bool ReadFileBytes(const std::string& path, std::vector<u8>& out_bytes);

// Reads the whole file as text, normalizing line endings via text-mode I/O.
bool ReadFileText(const std::string& path, std::string& out_text);

bool WriteFileBytes(const std::string& path, const void* data, usize size_bytes);

// Writes the file so a crash or power cut leaves either the old contents or
// the new, never half of one: the data goes to "<path>.tmp" and is renamed
// over the target. Creates the folder if needed. A failed write removes the
// temporary file and leaves the target as it was.
bool WriteFileAtomic(const std::string& path, const void* data, usize size_bytes);

// The names (not paths) of the regular files in a folder, sorted; empty if
// the folder doesn't exist.
std::vector<std::string> ListDirectory(const std::string& directory);

usize FileSize(const std::string& path);

std::string ParentPath(const std::string& path);
std::string Extension(const std::string& path);

} // namespace aether::fs
