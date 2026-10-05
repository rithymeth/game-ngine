#include "aether/pak/vfs.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

namespace aether::pak {

namespace {

std::string MountPoint(const std::string& mount_point) {
    std::string m = NormalizePath(mount_point);
    if (!m.empty()) m += '/';
    return m;
}

bool HasPakExtension(const std::string& source) {
    const std::string ext = std::filesystem::path(source).extension().string();
    return ext == ".apak" || ext == ".APAK";
}

} // namespace

void VirtualFileSystem::Sort() {
    std::stable_sort(mounts_.begin(), mounts_.end(), [](const MountEntry& a, const MountEntry& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.order > b.order;
    });
}

bool VirtualFileSystem::Mount(const std::string& source, const std::string& mount_point, int priority,
                              std::string* error) {
    MountEntry m;
    m.source = source;
    m.mount_point = MountPoint(mount_point);
    m.priority = priority;
    m.order = next_order_++;
    if (HasPakExtension(source)) {
        auto reader = std::make_shared<PakReader>();
        if (!reader->Open(source, error, keys_)) return false;
        m.is_pak = true;
        m.pak = std::move(reader);
        LoadRemoved(m);
    } else {
        std::error_code ec;
        if (!std::filesystem::is_directory(source, ec)) {
            if (error) *error = source + " is neither a directory nor an .apak archive";
            return false;
        }
    }
    mounts_.push_back(std::move(m));
    Sort();
    return true;
}

void VirtualFileSystem::MountPak(PakReader reader, const std::string& mount_point, int priority) {
    MountEntry m;
    m.source = reader.File();
    m.mount_point = MountPoint(mount_point);
    m.priority = priority;
    m.order = next_order_++;
    m.is_pak = true;
    m.pak = std::make_shared<PakReader>(std::move(reader));
    LoadRemoved(m);
    mounts_.push_back(std::move(m));
    Sort();
}

void VirtualFileSystem::LoadRemoved(MountEntry& m) {
    std::string text;
    if (!m.pak->Contains(kRemovedListPath) || !m.pak->ReadText(kRemovedListPath, text)) return;
    const nlohmann::json list = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!list.is_array()) return;
    for (const nlohmann::json& path : list) {
        if (!path.is_string()) continue;
        const std::string p = NormalizePath(path.get<std::string>());
        if (!p.empty()) m.removed.insert(m.mount_point + p);
    }
}

const VirtualFileSystem::MountEntry* VirtualFileSystem::Locate(const std::string& p, std::string& inner) const {
    if (p.empty()) return nullptr;
    for (const MountEntry& m : mounts_) {
        if (Inner(m, p, inner)) {
            if (m.is_pak) {
                if (m.pak->Contains(inner)) return &m;
            } else {
                std::error_code ec;
                if (std::filesystem::is_regular_file(std::filesystem::path(m.source) / inner, ec)) return &m;
            }
        }
        if (m.removed.count(p)) return nullptr; // a patch above the rest removed it
    }
    return nullptr;
}

bool VirtualFileSystem::Unmount(const std::string& source) {
    const auto it = std::find_if(mounts_.begin(), mounts_.end(), [&](const MountEntry& m) { return m.source == source; });
    if (it == mounts_.end()) return false;
    mounts_.erase(it);
    return true;
}

bool VirtualFileSystem::Inner(const MountEntry& m, const std::string& path, std::string& inner) {
    if (path.compare(0, m.mount_point.size(), m.mount_point) != 0) return false;
    inner = path.substr(m.mount_point.size());
    return !inner.empty();
}

std::string VirtualFileSystem::Resolve(const std::string& path) const {
    std::string inner;
    const MountEntry* m = Locate(NormalizePath(path), inner);
    return m ? m->source : std::string();
}

bool VirtualFileSystem::Exists(const std::string& path) const {
    return !Resolve(path).empty();
}

bool VirtualFileSystem::Read(const std::string& path, std::vector<u8>& out, std::string* error) const {
    std::string inner;
    const MountEntry* m = Locate(NormalizePath(path), inner);
    if (!m) {
        if (error) *error = "'" + path + "' isn't in any mounted directory or archive";
        return false;
    }
    if (m->is_pak) return m->pak->Read(inner, out, error);
    const std::filesystem::path file = std::filesystem::path(m->source) / inner;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error) *error = "can't read " + file.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool VirtualFileSystem::ReadText(const std::string& path, std::string& out, std::string* error) const {
    std::vector<u8> bytes;
    if (!Read(path, bytes, error)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

std::vector<std::string> VirtualFileSystem::List(const std::string& prefix) const {
    const std::string pre = NormalizePath(prefix);
    std::set<std::string> found;
    const auto add_visible = [&](const std::string& path) {
        if (pre.empty() || path.compare(0, pre.size(), pre) == 0) found.insert(path);
    };
    std::set<std::string> hidden; // removed by a patch mounted above
    for (const MountEntry& m : mounts_) {
        const auto add = [&](const std::string& path) {
            if (!hidden.count(path)) add_visible(path);
        };
        if (m.is_pak) {
            for (const PakEntry& e : m.pak->Entries()) {
                if (e.path != kRemovedListPath) add(m.mount_point + e.path);
            }
        } else {
            std::error_code ec;
            for (auto it = std::filesystem::recursive_directory_iterator(m.source, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                add(m.mount_point + std::filesystem::relative(it->path(), m.source, ec).generic_string());
            }
        }
        hidden.insert(m.removed.begin(), m.removed.end());
    }
    return {found.begin(), found.end()};
}

} // namespace aether::pak
