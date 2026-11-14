#include "workspace/layout_presets.h"

#include "core/recent_projects.h"
#include "ui/shell_layout.h"

#include "aether/platform/filesystem.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace aether::editor {

namespace stdfs = std::filesystem;

namespace {

constexpr std::size_t kMaxNameLength = 64;

// A name a file can hold on every platform the editor builds for. UTF-8 bytes are kept; a path separator, a colon
// or another punctuation character becomes '_'; leading and trailing dots and spaces go, so ".." cannot escape and
// Windows does not reject the result.
std::string Sanitize(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte >= 0x80 || std::isalnum(byte) || c == ' ' || c == '-' || c == '_' || c == '.') {
            out += c;
        } else {
            out += '_';
        }
    }
    const std::size_t first = out.find_first_not_of(" .");
    const std::size_t last = out.find_last_not_of(" .");
    if (first == std::string::npos) return {};
    out = out.substr(first, last - first + 1);
    if (out == "." || out == "..") return {};
    if (out.size() > kMaxNameLength) {
        out.resize(kMaxNameLength);
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    }
    return out;
}

stdfs::path DefaultDirectory() {
    if (const stdfs::path layout = DefaultShellLayoutPath(); !layout.empty()) return layout.parent_path() / "layouts";
    if (const stdfs::path config = RecentProjects::DefaultConfigDir(); !config.empty()) return config / "layouts";
    return {};
}

} // namespace

LayoutPresets::LayoutPresets(stdfs::path directory) : directory_(directory.empty() ? DefaultDirectory() : std::move(directory)) {}

const std::vector<std::string>& LayoutPresets::BuiltinNames() {
    static const std::vector<std::string> names = {"Default", "Level design", "Scripting", "Animation"};
    return names;
}

bool LayoutPresets::IsBuiltin(const std::string& name) {
    for (const std::string& builtin : BuiltinNames()) {
        if (builtin == name) return true;
    }
    return false;
}

std::vector<std::string> LayoutPresets::Names() const {
    std::vector<std::string> names = BuiltinNames();
    for (const std::string& saved : SavedNames()) names.push_back(saved);
    return names;
}

std::vector<std::string> LayoutPresets::SavedNames() const {
    std::vector<std::string> names;
    if (directory_.empty()) return names;
    std::error_code ec;
    for (const auto& entry : stdfs::directory_iterator(directory_, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".ini") continue;
        const std::string name = entry.path().stem().string();
        if (name.empty() || IsBuiltin(name)) continue;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        std::string lower_a = a, lower_b = b;
        std::transform(lower_a.begin(), lower_a.end(), lower_a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(lower_b.begin(), lower_b.end(), lower_b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower_a == lower_b ? a < b : lower_a < lower_b;
    });
    const auto same_name = [](const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    };
    names.erase(std::unique(names.begin(), names.end(), same_name), names.end());
    return names;
}

stdfs::path LayoutPresets::Path(const std::string& name) const {
    const std::string file = Sanitize(name);
    if (directory_.empty() || file.empty()) return {};
    return directory_ / (file + ".ini");
}

bool LayoutPresets::Save(const std::string& name, const std::string& ini, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (Sanitize(name).empty()) return fail("the layout needs a name");
    if (ini.empty()) return fail("there is no layout to save");
    const stdfs::path file = Path(name);
    if (file.empty()) return fail("no folder for layouts");
    std::error_code ec;
    stdfs::create_directories(file.parent_path(), ec);
    if (ec) return fail("can't make " + file.parent_path().string() + ": " + ec.message());
    stdfs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << ini;
        if (!out.good()) return fail("can't write " + temp.string());
    }
    stdfs::rename(temp, file, ec);
    if (ec) {
        stdfs::remove(temp, ec);
        return fail("can't replace " + file.string());
    }
    return true;
}

std::optional<std::string> LayoutPresets::Load(const std::string& name) const {
    const stdfs::path file = Path(name);
    if (file.empty()) return std::nullopt;
    std::string text;
    if (!fs::ReadFileText(file.string(), text) || text.empty()) return std::nullopt;
    return text;
}

} // namespace aether::editor
