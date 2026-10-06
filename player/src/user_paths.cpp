#include "aether/player/user_paths.h"

#include <cstdlib>

namespace aether::player {

namespace stdfs = std::filesystem;

std::string SanitizeProjectFolder(const std::string& project_name) {
    std::string out;
    for (const char c : project_name) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        out += keep ? c : '_';
    }
    if (out.empty() || out.find_first_not_of('_') == std::string::npos) return "Game";
    return out;
}

namespace {

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

UserPaths Make(const stdfs::path& root) {
    UserPaths p;
    p.root = root;
    p.saves = root / "Saves";
    p.settings = root / "Config";
    return p;
}

} // namespace

UserPaths ResolveUserPaths(const std::string& project_name, const stdfs::path& override_root) {
    if (!override_root.empty()) return Make(override_root);
    const std::string env = Env("AETHER_USER_DIR");
    if (!env.empty()) return Make(env);
    const std::string folder = SanitizeProjectFolder(project_name);
    stdfs::path base;
#if defined(AETHER_PLATFORM_WINDOWS)
    if (const std::string appdata = Env("APPDATA"); !appdata.empty()) base = appdata;
#elif defined(AETHER_PLATFORM_MACOS)
    if (const std::string home = Env("HOME"); !home.empty()) base = stdfs::path(home) / "Library" / "Application Support";
#else
    if (const std::string xdg = Env("XDG_DATA_HOME"); !xdg.empty()) base = xdg;
    else if (const std::string home = Env("HOME"); !home.empty()) base = stdfs::path(home) / ".local" / "share";
#endif
    if (base.empty()) {
        std::error_code ec;
        base = stdfs::temp_directory_path(ec);
    }
    return Make(base / folder);
}

} // namespace aether::player
