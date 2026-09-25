#include "aether/platform/filesystem.h"

#include <filesystem>
#include <fstream>

namespace aether::fs {

namespace stdfs = std::filesystem;

bool Exists(const std::string& path) {
    std::error_code ec;
    return stdfs::exists(path, ec);
}

bool ReadFileBytes(const std::string& path, std::vector<u8>& out_bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }

    const std::streamsize size = file.tellg();
    if (size < 0) {
        return false;
    }
    file.seekg(0, std::ios::beg);

    out_bytes.resize(static_cast<usize>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(out_bytes.data()), size)) {
        return false;
    }
    return true;
}

bool ReadFileText(const std::string& path, std::string& out_text) {
    std::ifstream file(path, std::ios::in);
    if (!file) {
        return false;
    }
    out_text.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return true;
}

bool WriteFileBytes(const std::string& path, const void* data, usize size_bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size_bytes));
    return file.good();
}

usize FileSize(const std::string& path) {
    std::error_code ec;
    auto size = stdfs::file_size(path, ec);
    return ec ? 0 : static_cast<usize>(size);
}

std::string ParentPath(const std::string& path) {
    return stdfs::path(path).parent_path().string();
}

std::string Extension(const std::string& path) {
    return stdfs::path(path).extension().string();
}

} // namespace aether::fs
