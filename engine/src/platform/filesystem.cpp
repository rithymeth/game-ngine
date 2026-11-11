#include "aether/platform/filesystem.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

namespace aether::fs {

namespace stdfs = std::filesystem;

bool Exists(const std::string& path) {
    std::error_code ec;
    return stdfs::exists(path, ec);
}

bool ReadFileBytes(const std::string& path, std::vector<u8>& out_bytes) {
    return ReadFileBytes(path, out_bytes, std::numeric_limits<usize>::max());
}

bool ReadFileBytes(const std::string& path, std::vector<u8>& out_bytes, usize max_bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }

    const std::streamsize size = file.tellg();
    if (size < 0) {
        return false;
    }
    if (static_cast<u64>(size) > std::numeric_limits<usize>::max() || static_cast<u64>(size) > max_bytes) {
        return false;
    }
    file.seekg(0, std::ios::beg);

    std::vector<u8> bytes(static_cast<usize>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return false;
    }
    out_bytes = std::move(bytes);
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
    if (size_bytes > static_cast<usize>(std::numeric_limits<std::streamsize>::max())) return false;
    // Report the OS reason. A silent false leaves the caller unable to tell a bad path from a full disk, and the tests
    // that use this print only "CHECK failed", which says nothing about the file that could not be opened.
    errno = 0;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "WriteFileBytes: could not open '%s' (errno=%d: %s)\n", path.c_str(), errno,
                     std::strerror(errno));
        return false;
    }
    errno = 0;
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size_bytes));
    if (!file.good()) {
        std::fprintf(stderr, "WriteFileBytes: write to '%s' failed after %zu bytes (errno=%d: %s)\n", path.c_str(),
                     size_bytes, errno, std::strerror(errno));
        return false;
    }
    return true;
}

bool WriteFileAtomic(const std::string& path, const void* data, usize size_bytes) {
    std::error_code ec;
    const stdfs::path target(path);
    if (!target.parent_path().empty()) stdfs::create_directories(target.parent_path(), ec);
    stdfs::path temp = target;
    temp += ".tmp";
    if (!WriteFileBytes(temp.string(), data, size_bytes)) {
        stdfs::remove(temp, ec);
        return false;
    }
    stdfs::rename(temp, target, ec); // replaces an existing file on every platform we build for
    if (ec) {
        stdfs::remove(temp, ec);
        return false;
    }
    return true;
}

bool WriteFileAtomicWithBackup(const std::string& path, const void* data, usize size_bytes) {
    const stdfs::path target(path);
    const stdfs::path backup = target.string() + ".bak";
    std::error_code ec;
    if (!target.parent_path().empty()) stdfs::create_directories(target.parent_path(), ec);
    if (ec) return false;

    stdfs::path temp = target;
    temp += ".tmp";
    if (!WriteFileBytes(temp.string(), data, size_bytes)) {
        stdfs::remove(temp, ec);
        return false;
    }

    const bool target_exists = stdfs::exists(target, ec);
    if (ec) {
        stdfs::remove(temp, ec);
        return false;
    }
    if (target_exists) {
        stdfs::path backup_temp = backup;
        backup_temp += ".tmp";
        stdfs::copy_file(target, backup_temp, stdfs::copy_options::overwrite_existing, ec);
        if (ec) {
            stdfs::remove(temp, ec);
            stdfs::remove(backup_temp, ec);
            return false;
        }
        stdfs::rename(backup_temp, backup, ec);
        if (ec) {
            stdfs::remove(temp, ec);
            stdfs::remove(backup_temp, ec);
            return false;
        }
    }

    stdfs::rename(temp, target, ec);
    if (ec) {
        stdfs::remove(temp, ec);
        return false;
    }
    return true;
}

std::vector<std::string> ListDirectory(const std::string& directory) {
    std::vector<std::string> names;
    std::error_code ec;
    for (stdfs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        if (it->is_regular_file(type_ec)) names.push_back(it->path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

usize FileSize(const std::string& path) {
    std::error_code ec;
    auto size = stdfs::file_size(path, ec);
    return ec || size > std::numeric_limits<usize>::max() ? 0 : static_cast<usize>(size);
}

std::string ParentPath(const std::string& path) {
    return stdfs::path(path).parent_path().string();
}

std::string Extension(const std::string& path) {
    return stdfs::path(path).extension().string();
}

} // namespace aether::fs
