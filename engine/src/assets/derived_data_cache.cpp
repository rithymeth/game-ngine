#include "aether/assets/derived_data_cache.h"

#include "aether/platform/filesystem.h"

#include <cstdio>
#include <system_error>

namespace aether::assets {

namespace stdfs = std::filesystem;

DerivedDataCache::DerivedDataCache(stdfs::path directory) : dir_(std::move(directory)) {}

std::string DerivedDataCache::MakeKey(std::string_view importer, u32 importer_version, std::string_view source_hash,
                                      std::string_view settings_json, std::string_view platform) {
    // Two independent FNV-1a 64 streams (different offset bases) over the
    // length-prefixed parts: a 128-bit key, so collisions aren't a concern.
    u64 a = 0xcbf29ce484222325ull;
    u64 b = 0x84222325cbf29ce4ull;
    auto mix = [&](std::string_view part) {
        std::string framed = std::to_string(part.size()) + ":";
        framed.append(part);
        for (unsigned char c : framed) {
            a = (a ^ c) * 0x100000001b3ull;
            b = (b ^ c) * 0x100000001b3ull;
        }
        b = (b ^ 0xff) * 0x100000001b3ull;
    };
    mix(importer);
    mix(std::to_string(importer_version));
    mix(source_hash);
    mix(settings_json);
    mix(platform);
    char key[40];
    std::snprintf(key, sizeof(key), "%016llx%016llx", static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b));
    return key;
}

stdfs::path DerivedDataCache::PathFor(const std::string& key) const {
    // Two-character fan-out keeps any one folder small.
    return dir_ / key.substr(0, 2) / (key + ".ddc");
}

std::optional<std::vector<u8>> DerivedDataCache::Get(const std::string& key) {
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(PathFor(key).string(), bytes)) {
        ++misses_;
        return std::nullopt;
    }
    ++hits_;
    return bytes;
}

bool DerivedDataCache::Put(const std::string& key, const std::vector<u8>& data) {
    const stdfs::path path = PathFor(key);
    std::error_code ec;
    stdfs::create_directories(path.parent_path(), ec);
    stdfs::path temp = path;
    temp += ".tmp";
    if (!fs::WriteFileBytes(temp.string(), data.data(), data.size())) {
        return false;
    }
    stdfs::rename(temp, path, ec);
    if (ec) {
        stdfs::remove(temp, ec);
        return false;
    }
    return true;
}

} // namespace aether::assets
