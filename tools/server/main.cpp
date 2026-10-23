// aether_server: the dedicated server (M8.9). Mounts a cooked game like the
// player does, loads its startup scene (or --scene) into a headless
// player::Game - no window, no renderer, no audio output - and hosts it over
// UDP: SessionHost (password, build id, max players) + ReplicationServer +
// RpcRouter at a fixed tick. Configured by server.json and/or the command
// line (see ServerUsage, docs/manual/15-dedicated-server.md). SIGINT/SIGTERM
// shut it down cleanly; --max-runtime and --quit-when-empty end CI runs.

#include "aether/core/log.h"
#include "aether/core/version.h"
#include "aether/player/game.h"
#include "aether/server/dedicated_server.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace aether;
using namespace aether::player;
using namespace aether::server;
namespace stdfs = std::filesystem;

namespace {

enum Exit : int { kOk = 0, kStartupFailed = 1, kBadConfig = 2, kPortInUse = 3 };

volatile std::sig_atomic_t g_stop = 0;
extern "C" void OnSignal(int sig) { g_stop = sig; }

LogLevel ParseLevel(const std::string& s) {
    if (s == "trace") return LogLevel::Trace;
    if (s == "warn") return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    return LogLevel::Info;
}

std::vector<std::string> PaksToMount(const ServerConfig& c, const char* argv0, const stdfs::path& config_dir) {
    std::vector<std::string> paks;
    for (const std::string& raw : c.paks) {
        stdfs::path p(raw);
        if (p.is_relative() && !config_dir.empty() && !stdfs::exists(p)) p = config_dir / p; // relative to server.json
        std::error_code ec;
        if (stdfs::is_directory(p, ec)) {
            for (std::string& f : GamePackage::FindPaks(p)) paks.push_back(std::move(f));
        } else {
            paks.push_back(p.string());
        }
    }
    if (!c.paks.empty()) return paks;
    std::error_code ec;
    const stdfs::path exe_dir = stdfs::absolute(argv0, ec).parent_path();
    paks = GamePackage::FindPaks(exe_dir / "Paks");
    if (paks.empty()) paks = GamePackage::FindPaks(exe_dir.parent_path() / "Paks"); // bin/aether_server in a package
    if (paks.empty()) paks = GamePackage::FindPaks("Paks");
    return paks;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    ServerConfig config;
    std::string error;
    bool help = false;
    // A server.json beside the executable (or the package root) is the default config.
    stdfs::path config_dir;
    {
        bool explicit_config = false;
        for (usize i = 0; i + 1 < args.size(); ++i)
            if (args[i] == "--config") explicit_config = true, config_dir = stdfs::absolute(args[i + 1]).parent_path();
        if (!explicit_config) {
            std::error_code ec;
            const stdfs::path exe_dir = stdfs::absolute(argv[0], ec).parent_path();
            for (const stdfs::path& candidate : {exe_dir / "server.json", exe_dir.parent_path() / "server.json"})
                if (stdfs::exists(candidate, ec)) {
                    args.insert(args.begin(), {"--config", candidate.string()});
                    config_dir = candidate.parent_path();
                    break;
                }
        }
    }
    if (!ParseServerArgs(args, config, &error, &help)) {
        std::fprintf(stderr, "aether_server: %s\n%s", error.c_str(), ServerUsage());
        return kBadConfig;
    }
    if (help) {
        std::fputs(ServerUsage(), stdout);
        return kOk;
    }

    // A relative log_file from server.json is relative to that file (the package root), not the working directory.
    if (!config.log_file.empty() && stdfs::path(config.log_file).is_relative() && !config_dir.empty() &&
        std::find(args.begin(), args.end(), "--log-file") == args.end())
        config.log_file = (config_dir / config.log_file).string();
    Logger::Instance().SetMinLevel(ParseLevel(config.log_level));
    std::ofstream log_file;
    std::mutex log_mutex;
    int sink = -1;
    if (!config.log_file.empty()) {
        std::error_code ec;
        const stdfs::path parent = stdfs::path(config.log_file).parent_path();
        if (!parent.empty()) stdfs::create_directories(parent, ec);
        log_file.open(config.log_file, std::ios::app);
        if (!log_file) {
            std::fprintf(stderr, "aether_server: can't open log file %s\n", config.log_file.c_str());
            return kBadConfig;
        }
        sink = Logger::Instance().AddSink([&](const LogLine& line) {
            std::lock_guard lock(log_mutex);
            char stamp[32];
            std::snprintf(stamp, sizeof(stamp), "[%10.3f]", line.time);
            log_file << stamp << " [" << LogLevelName(line.level) << "] [" << line.category << "] " << line.message << '\n';
            log_file.flush();
        });
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    GamePackage package;
    if (const char* env = std::getenv("AETHER_PAK_KEY")) {
        pak::PakKey key;
        if (!pak::PakKey::FromHex(env, key)) {
            AETHER_LOG_ERROR("Server", "AETHER_PAK_KEY must be 64 hex digits");
            return kBadConfig;
        }
        package.AddKey(key);
    }
    const std::vector<std::string> paks = PaksToMount(config, argv[0], config_dir);
    if (paks.empty()) {
        AETHER_LOG_ERROR("Server", "no .apak to mount (set \"paks\" in server.json, pass --pak, or put them in Paks/)");
        return kStartupFailed;
    }
    for (usize i = 0; i < paks.size(); ++i)
        if (!package.Mount(paks[i], static_cast<int>(i), &error)) {
            AETHER_LOG_ERROR("Server", "%s", error.c_str());
            return kStartupFailed;
        }
    if (!package.LoadManifest(&error)) {
        AETHER_LOG_ERROR("Server", "%s", error.c_str());
        return kStartupFailed;
    }
    AETHER_LOG_INFO("Server", "%s (engine %s), %zu archive(s), %zu assets", package.Manifest().project.c_str(), kEngineVersion,
                    paks.size(), package.Manifest().assets.size());

    int exit_code = kOk;
    {
        Game game(package);
        const bool loaded = config.scene.empty() ? game.LoadStartupScene(&error) : game.LoadScene(config.scene, &error);
        if (!loaded) {
            AETHER_LOG_ERROR("Server", "%s", error.c_str());
            return kStartupFailed;
        }
        for (const std::string& w : game.Warnings()) AETHER_LOG_WARN("Server", "%s", w.c_str());
        game.BeginPlay();

        DedicatedServer server;
        if (!server.Start(config, game.GetWorld(), &error)) {
            AETHER_LOG_ERROR("Server", "%s", error.c_str());
            game.EndPlay();
            return kPortInUse;
        }
        AETHER_LOG_INFO("Server", "'%s' listening on UDP %u: scene %s, %u players max, build %u, %s, %.0f Hz, %zu replicated entities",
                        config.name.c_str(), server.Port(), game.SceneName().c_str(), config.max_players, config.build_id,
                        config.password.empty() ? "no password" : "password set", config.tick_rate,
                        server.Stats().replicated_entities);
        // Machine-readable readiness for scripts that wait on the log.
        std::printf("AETHER_SERVER_READY port=%u\n", server.Port());
        std::fflush(stdout);

        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        const auto step = std::chrono::duration<f64>(1.0 / config.tick_rate);
        auto next = start;
        f64 last_status = 0;
        const char* reason = "signal";
        for (;;) {
            if (g_stop) {
                AETHER_LOG_INFO("Server", "signal %d: shutting down", static_cast<int>(g_stop));
                break;
            }
            const f64 now = std::chrono::duration<f64>(Clock::now() - start).count();
            if (config.max_runtime > 0 && now >= config.max_runtime) {
                reason = "max_runtime";
                break;
            }
            if (config.quit_when_empty && server.EmptyAfterPlay()) {
                reason = "empty";
                break;
            }
            game.Tick(static_cast<f32>(1.0 / config.tick_rate));
            server.Tick(now);
            if (game.ExitRequested()) {
                reason = "game_exit";
                break;
            }
            if (config.status_interval > 0 && now - last_status >= config.status_interval) {
                last_status = now;
                const ServerStats s = server.Stats();
                AETHER_LOG_INFO("Server", "status: t=%.0fs players=%u peak=%u joins=%llu refused=%llu entities=%zu sent=%llu B",
                                now, s.players, s.peak_players, static_cast<unsigned long long>(s.joins),
                                static_cast<unsigned long long>(s.refused), s.replicated_entities,
                                static_cast<unsigned long long>(s.bytes_sent));
            }
            next += std::chrono::duration_cast<Clock::duration>(step);
            const auto t = Clock::now();
            if (next < t - std::chrono::seconds(1)) next = t; // fell far behind (a stall): don't spiral
            std::this_thread::sleep_until(next);
        }
        const ServerStats s = server.Stats();
        server.Stop();
        game.EndPlay();
        std::printf("AETHER_SERVER_SUMMARY {\"reason\":\"%s\",\"ticks\":%llu,\"joins\":%llu,\"leaves\":%llu,\"refused\":%llu,"
                    "\"peak_players\":%u,\"pings\":%llu}\n",
                    reason, static_cast<unsigned long long>(s.ticks), static_cast<unsigned long long>(s.joins),
                    static_cast<unsigned long long>(s.leaves), static_cast<unsigned long long>(s.refused), s.peak_players,
                    static_cast<unsigned long long>(s.pings));
        AETHER_LOG_INFO("Server", "stopped (%s)", reason);
    }
    if (sink >= 0) Logger::Instance().RemoveSink(sink);
    return exit_code;
}
