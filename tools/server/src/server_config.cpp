#include "aether/reflection/serialize.h"
#include "aether/server/dedicated_server.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace aether::server {

namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

bool ParseU64(const std::string& text, u64 max, u64& out) {
    if (text.empty() || text[0] == '-' || text[0] == '+') return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || *end != '\0' || v > max) return false;
    out = v;
    return true;
}

bool ParseF64(const std::string& text, f64& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const f64 v = std::strtod(text.c_str(), &end);
    if (errno != 0 || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool ValidLevel(const std::string& level) {
    return level == "trace" || level == "info" || level == "warn" || level == "error";
}

bool Validate(const ServerConfig& c, std::string* error) {
    if (c.max_players == 0 || c.max_players > 1024) return Fail(error, "max_players must be 1..1024");
    if (!(c.tick_rate >= 1.0 && c.tick_rate <= 240.0)) return Fail(error, "tick_rate must be 1..240");
    if (c.max_runtime < 0) return Fail(error, "max_runtime must be >= 0");
    if (c.status_interval < 0) return Fail(error, "status_interval must be >= 0");
    if (!ValidLevel(c.log_level)) return Fail(error, "log_level must be trace, info, warn or error");
    if (c.name.size() > 64) return Fail(error, "name must be at most 64 characters");
    return true;
}

bool LoadFile(const std::string& path, ServerConfig& config, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return Fail(error, "can't read config file " + path);
    std::stringstream ss;
    ss << file.rdbuf();
    std::string inner;
    if (!ParseServerConfigJson(ss.str(), config, &inner)) return Fail(error, path + ": " + inner);
    return true;
}

} // namespace

bool ParseServerConfigJson(const std::string& text, ServerConfig& config, std::string* error) {
    reflect::Json j = reflect::Json::parse(text, nullptr, false, true);
    if (j.is_discarded()) return Fail(error, "not valid JSON");
    if (!j.is_object()) return Fail(error, "the config must be a JSON object");
    ServerConfig c = config;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& key = it.key();
        const reflect::Json& v = it.value();
        auto want_string = [&](std::string& out) {
            if (!v.is_string()) return Fail(error, "'" + key + "' must be a string");
            out = v.get<std::string>();
            return true;
        };
        auto want_uint = [&](u64 max, u64& out) {
            if (!v.is_number_unsigned() || v.get<u64>() > max) return Fail(error, "'" + key + "' must be an integer 0.." + std::to_string(max));
            out = v.get<u64>();
            return true;
        };
        auto want_number = [&](f64& out) {
            if (!v.is_number()) return Fail(error, "'" + key + "' must be a number");
            out = v.get<f64>();
            return true;
        };
        auto want_bool = [&](bool& out) {
            if (!v.is_boolean()) return Fail(error, "'" + key + "' must be true or false");
            out = v.get<bool>();
            return true;
        };
        u64 u = 0;
        bool ok = true;
        if (key == "name") ok = want_string(c.name);
        else if (key == "port") ok = want_uint(65535, u) && (c.port = static_cast<u16>(u), true);
        else if (key == "max_players") ok = want_uint(1024, u) && (c.max_players = static_cast<u32>(u), true);
        else if (key == "password") ok = want_string(c.password);
        else if (key == "build_id") ok = want_uint(0xFFFFFFFFull, u) && (c.build_id = static_cast<u32>(u), true);
        else if (key == "scene") ok = want_string(c.scene);
        else if (key == "tick_rate") ok = want_number(c.tick_rate);
        else if (key == "log_file") ok = want_string(c.log_file);
        else if (key == "log_level") ok = want_string(c.log_level);
        else if (key == "replicate_scene") ok = want_bool(c.replicate_scene);
        else if (key == "max_runtime") ok = want_number(c.max_runtime);
        else if (key == "quit_when_empty") ok = want_bool(c.quit_when_empty);
        else if (key == "status_interval") ok = want_number(c.status_interval);
        else if (key == "paks") {
            if (!v.is_array()) return Fail(error, "'paks' must be an array of strings");
            c.paks.clear();
            for (const reflect::Json& p : v) {
                if (!p.is_string()) return Fail(error, "'paks' must be an array of strings");
                c.paks.push_back(p.get<std::string>());
            }
        } else return Fail(error, "unknown key '" + key + "'");
        if (!ok) return false;
    }
    if (!Validate(c, error)) return false;
    config = std::move(c);
    return true;
}

const char* ServerUsage() {
    return "usage: aether_server [--config <server.json>] [--pak <file|dir>]... [--scene <path>]\n"
           "                     [--port <n>] [--max-players <n>] [--password <text>] [--build-id <n>]\n"
           "                     [--name <text>] [--tick-rate <hz>] [--log-file <path>] [--log-level <level>]\n"
           "                     [--max-runtime <seconds>] [--quit-when-empty] [--no-replicate-scene]\n"
           "                     [--status-interval <seconds>]\n"
           "The command line overrides the config file; AETHER_SERVER_PASSWORD overrides both for the password.\n"
           "Exit codes: 0 clean shutdown, 1 startup failure (package/scene), 2 bad config or arguments, 3 port in use.\n";
}

bool ParseServerArgs(const std::vector<std::string>& args, ServerConfig& config, std::string* error, bool* help) {
    if (help) *help = false;
    ServerConfig c = config;
    // The file first, so every other argument overrides it.
    for (usize i = 0; i < args.size(); ++i)
        if (args[i] == "--config") {
            if (i + 1 >= args.size()) return Fail(error, "--config needs a file");
            if (!LoadFile(args[i + 1], c, error)) return false;
        }
    bool cli_paks = false;
    for (usize i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool has = i + 1 < args.size();
        const std::string value = has ? args[i + 1] : std::string();
        auto need = [&](const char* what) { return has ? true : Fail(error, a + " needs " + what); };
        u64 u = 0;
        f64 f = 0;
        if (a == "-h" || a == "--help") {
            if (help) *help = true;
            continue;
        }
        if (a == "--quit-when-empty") { c.quit_when_empty = true; continue; }
        if (a == "--no-replicate-scene") { c.replicate_scene = false; continue; }
        if (a == "--config") { ++i; continue; }
        if (a == "--port") {
            if (!need("a port") || !ParseU64(value, 65535, u)) return Fail(error, "--port must be 0..65535");
            c.port = static_cast<u16>(u);
        } else if (a == "--max-players") {
            if (!need("a count") || !ParseU64(value, 1024, u)) return Fail(error, "--max-players must be 1..1024");
            c.max_players = static_cast<u32>(u);
        } else if (a == "--build-id") {
            if (!need("a number") || !ParseU64(value, 0xFFFFFFFFull, u)) return Fail(error, "--build-id must be an unsigned 32-bit number");
            c.build_id = static_cast<u32>(u);
        } else if (a == "--tick-rate") {
            if (!need("a rate") || !ParseF64(value, f)) return Fail(error, "--tick-rate must be a number");
            c.tick_rate = f;
        } else if (a == "--max-runtime") {
            if (!need("seconds") || !ParseF64(value, f)) return Fail(error, "--max-runtime must be a number");
            c.max_runtime = f;
        } else if (a == "--status-interval") {
            if (!need("seconds") || !ParseF64(value, f)) return Fail(error, "--status-interval must be a number");
            c.status_interval = f;
        } else if (a == "--password") {
            if (!need("a password")) return false;
            c.password = value;
        } else if (a == "--name") {
            if (!need("a name")) return false;
            c.name = value;
        } else if (a == "--scene") {
            if (!need("a scene path")) return false;
            c.scene = value;
        } else if (a == "--log-file") {
            if (!need("a path")) return false;
            c.log_file = value;
        } else if (a == "--log-level") {
            if (!need("a level")) return false;
            c.log_level = value;
        } else if (a == "--pak") {
            if (!need("a file or folder")) return false;
            if (!cli_paks) c.paks.clear(), cli_paks = true; // the command line's paks replace the file's
            c.paks.push_back(value);
        } else {
            return Fail(error, "unknown argument '" + a + "'");
        }
        ++i;
    }
    if (const char* pw = std::getenv("AETHER_SERVER_PASSWORD")) c.password = pw;
    if (!Validate(c, error)) return false;
    config = std::move(c);
    return true;
}

} // namespace aether::server
