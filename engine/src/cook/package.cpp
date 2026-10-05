#include "aether/cook/package.h"

#include "aether/core/log.h"
#include "aether/platform/process.h"
#include "aether/project/project.h"

#include <cctype>
#include <system_error>

namespace aether::cook {

namespace stdfs = std::filesystem;

namespace {

#if defined(_WIN32)
constexpr const char* kExeSuffix = ".exe";
#else
constexpr const char* kExeSuffix = "";
#endif

} // namespace

std::string GameExecutableName(const std::string& project_name) {
    std::string name;
    for (char c : project_name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') name.push_back(c);
    }
    if (name.empty()) name = "Game";
    return name + kExeSuffix;
}

stdfs::path FindPlayerExecutable() {
    std::error_code ec;
    const stdfs::path exe = platform::ExecutablePath();
    if (!exe.empty()) {
        const stdfs::path beside = exe.parent_path() / (std::string("aether_player") + kExeSuffix);
        if (stdfs::is_regular_file(beside, ec)) return beside;
    }
#ifdef AETHER_PLAYER_BUILD_PATH
    const stdfs::path built = AETHER_PLAYER_BUILD_PATH;
    if (stdfs::is_regular_file(built, ec)) return built;
#endif
    return {};
}

PackageReport Package(const PackageOptions& options) {
    PackageReport report;
    const auto fail = [&](const std::string& message) {
        report.ok = false;
        report.error = message;
        AETHER_LOG_ERROR("Package", "%s", message.c_str());
        return report;
    };
    const auto progress = [&](f32 fraction, const std::string& stage) {
        if (options.cook.progress) options.cook.progress(fraction, stage);
    };

    const stdfs::path player = options.player_executable.empty() ? FindPlayerExecutable() : options.player_executable;
    std::error_code ec;
    if (player.empty() || !stdfs::is_regular_file(player, ec)) {
        return fail(player.empty() ? "Can't find aether_player to package with"
                                   : "No player at " + player.string());
    }
    if (options.output_dir.empty()) return fail("No output folder");
    ProjectSettings settings;
    std::string error;
    if (!LoadProject(options.cook.project_file, settings, &error)) {
        return fail("Can't load " + options.cook.project_file.string() + ": " + error);
    }

    // Cook into Paks/, scaling the cook's progress to 0..0.9.
    CookOptions cook = options.cook;
    report.paks_dir = options.output_dir / "Paks";
    cook.output_dir = report.paks_dir;
    if (options.cook.progress) {
        cook.progress = [&](f32 fraction, const std::string& stage) { options.cook.progress(fraction * 0.9f, stage); };
    }
    stdfs::remove_all(report.paks_dir, ec); // a stale pak would mount over the new one
    report.cook = Cook(cook);
    if (!report.cook.ok) return fail(report.cook.error);

    progress(0.92f, "Staging the player");
    report.game_executable = options.output_dir / GameExecutableName(settings.name);
    stdfs::copy_file(player, report.game_executable, stdfs::copy_options::overwrite_existing, ec);
    if (ec) return fail("Can't copy the player to " + report.game_executable.string() + ": " + ec.message());
    stdfs::permissions(report.game_executable,
                       stdfs::perms::owner_exec | stdfs::perms::group_exec | stdfs::perms::others_exec,
                       stdfs::perm_options::add, ec);
    report.copied.push_back(report.game_executable);
#if defined(_WIN32)
    // The DLLs the player loads (dxcompiler.dll and the like).
    for (stdfs::directory_iterator it(player.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".dll") continue;
        const stdfs::path to = options.output_dir / it->path().filename();
        std::error_code copy_ec;
        stdfs::copy_file(it->path(), to, stdfs::copy_options::overwrite_existing, copy_ec);
        if (!copy_ec) report.copied.push_back(to);
    }
#endif
    progress(1.0f, "Packaged " + report.game_executable.filename().string());
    report.ok = true;
    AETHER_LOG_INFO("Package", "Packaged %s into %s", settings.name.c_str(), options.output_dir.string().c_str());
    return report;
}

} // namespace aether::cook
