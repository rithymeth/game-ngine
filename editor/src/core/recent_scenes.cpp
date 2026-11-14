#include "core/recent_scenes.h"

#include "aether/platform/filesystem.h"
#include "aether/reflection/serialize.h"

#include <algorithm>

namespace aether::editor {

namespace stdfs = std::filesystem;

void RecentScenes::Load(const stdfs::path& file) {
    entries_.clear();
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(file.string(), bytes)) return;
    reflect::Json json = reflect::Json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    auto list = json.is_object() ? json.find("recent") : json.end();
    if (list == json.end() || !list->is_array()) return;
    for (const reflect::Json& entry : *list) {
        if (entry.is_string() && entries_.size() < kMaxEntries) {
            std::string path = entry.get<std::string>();
            if (std::find(entries_.begin(), entries_.end(), path) == entries_.end()) entries_.push_back(std::move(path));
        }
    }
}

bool RecentScenes::Save(const stdfs::path& file) const {
    reflect::Json list = reflect::Json::array();
    for (const std::string& entry : entries_) list.push_back(entry);
    std::string text = reflect::Json{{"recent", std::move(list)}}.dump(2);
    text.push_back('\n');
    std::error_code ec;
    stdfs::create_directories(file.parent_path(), ec);
    return fs::WriteFileBytes(file.string(), text.data(), text.size());
}

void RecentScenes::Add(const std::string& path) {
    Remove(path);
    entries_.insert(entries_.begin(), path);
    if (entries_.size() > kMaxEntries) entries_.resize(kMaxEntries);
}

void RecentScenes::Remove(const std::string& path) {
    entries_.erase(std::remove(entries_.begin(), entries_.end(), path), entries_.end());
}

std::size_t RecentScenes::RemoveMissing(const stdfs::path& content_root) {
    const std::size_t before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&content_root](const std::string& entry) {
                                      const stdfs::path given(entry);
                                      std::error_code ec;
                                      return !stdfs::exists(given.is_absolute() ? given : content_root / given, ec);
                                  }),
                   entries_.end());
    return before - entries_.size();
}

} // namespace aether::editor
