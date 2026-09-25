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

usize FileSize(const std::string& path);

std::string ParentPath(const std::string& path);
std::string Extension(const std::string& path);

} // namespace aether::fs
