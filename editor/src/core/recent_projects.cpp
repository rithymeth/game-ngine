#include "core/recent_projects.h"

#include "aether/platform/filesystem.h"
#include "aether/reflection/serialize.h"

#include <algorithm>
#include <cstdlib>

namespace aether::editor {

namespace stdfs = std::filesystem;

stdfs::path RecentProjects::DefaultConfigDir() {
#if defined(_WIN32)
    char* appdata = nullptr;
    std::size_t appdata_size = 0;
    if (_dupenv_s(&appdata, &appdata_size, "APPDATA") == 0 && appdata != nullptr) {
        const stdfs::path config_dir = stdfs::path(appdata) / "Aether";
        std::free(appdata);
        return config_dir;
    }
    std::free(appdata);
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        return stdfs::path(xdg) / "aether";
    }
    if (const char* home = std::getenv("HOME")) {
        return stdfs::path(home) / ".config" / "aether";
    }
#endif
    return stdfs::temp_directory_path() / "aether";
}

void RecentProjects::Load(const stdfs::path& file) {
    entries_.clear();
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(file.string(), bytes)) {
        return;
    }
    reflect::Json json = reflect::Json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    auto list = json.is_object() ? json.find("recent") : json.end();
    if (list == json.end() || !list->is_array()) {
        return;
    }
    for (const reflect::Json& entry : *list) {
        if (entry.is_string() && entries_.size() < kMaxEntries) {
            stdfs::path path = stdfs::path(entry.get<std::string>());
            if (std::find(entries_.begin(), entries_.end(), path) == entries_.end()) {
                entries_.push_back(std::move(path));
            }
        }
    }
}

bool RecentProjects::Save(const stdfs::path& file) const {
    reflect::Json list = reflect::Json::array();
    for (const stdfs::path& entry : entries_) {
        list.push_back(entry.string());
    }
    std::string text = reflect::Json{{"recent", std::move(list)}}.dump(2);
    text.push_back('\n');
    std::error_code ec;
    stdfs::create_directories(file.parent_path(), ec);
    return fs::WriteFileBytes(file.string(), text.data(), text.size());
}

void RecentProjects::Add(const stdfs::path& project_file) {
    Remove(project_file);
    entries_.insert(entries_.begin(), project_file);
    if (entries_.size() > kMaxEntries) {
        entries_.resize(kMaxEntries);
    }
}

void RecentProjects::Remove(const stdfs::path& project_file) {
    entries_.erase(std::remove(entries_.begin(), entries_.end(), project_file), entries_.end());
}

std::size_t RecentProjects::RemoveMissing() {
    const std::size_t before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [](const stdfs::path& p) {
                                      std::error_code ec;
                                      return !stdfs::exists(p, ec);
                                  }),
                   entries_.end());
    return before - entries_.size();
}

} // namespace aether::editor
