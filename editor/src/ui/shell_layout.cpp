#include "ui/shell_layout.h"

#include "aether/platform/filesystem.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <system_error>

namespace aether::editor {

namespace stdfs = std::filesystem;

stdfs::path DefaultShellLayoutPath() {
    if (const char* file = std::getenv("AETHER_EDITOR_LAYOUT")) return *file ? stdfs::path(file) : stdfs::path();
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA")) return stdfs::path(appdata) / "Aether" / "editor_layout.json";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return stdfs::path(xdg) / "aether" / "editor_layout.json";
    if (const char* home = std::getenv("HOME"); home && *home) return stdfs::path(home) / ".config" / "aether" / "editor_layout.json";
#endif
    return {};
}

bool LoadShellLayout(const stdfs::path& file, ShellLayout& layout) {
    std::string text;
    if (file.empty() || !fs::ReadFileText(file.string(), text)) return false;
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!json.is_object()) return false;
    ShellLayout loaded;
    const auto tool = json.find("tool");
    if (tool != json.end() && tool->is_string()) {
        loaded.tool = tool->get<std::string>();
        if (loaded.tool.size() > 128) loaded.tool.clear();
    }
    layout = std::move(loaded);
    return true;
}

bool SaveShellLayout(const stdfs::path& file, const ShellLayout& layout, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (file.empty()) return fail("no layout file path");
    std::error_code ec;
    if (file.has_parent_path()) stdfs::create_directories(file.parent_path(), ec);
    if (ec) return fail("can't make " + file.parent_path().string() + ": " + ec.message());
    const nlohmann::json json = {{"tool", layout.tool}};
    stdfs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << json.dump(2) << '\n';
        if (!out.good()) return fail("can't write " + temp.string());
    }
    stdfs::rename(temp, file, ec);
    if (ec) {
        stdfs::remove(temp, ec);
        return fail("can't replace " + file.string());
    }
    return true;
}

} // namespace aether::editor
