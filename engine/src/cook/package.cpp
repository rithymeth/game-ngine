#include "aether/cook/package.h"

#include "aether/core/log.h"
#include "aether/platform/filesystem.h"
#include "aether/platform/process.h"
#include "aether/project/project.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <system_error>

namespace aether::cook {

namespace stdfs = std::filesystem;

namespace {

#if defined(_WIN32)
constexpr const char* kExeSuffix = ".exe";
#else
constexpr const char* kExeSuffix = "";
#endif

bool FileChecksum(const stdfs::path& file, u64& bytes, u32& crc) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::array<u8, 64 * 1024> buffer{};
    bytes = 0;
    crc = 0;
    while (in) {
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = in.gcount();
        if (count > 0) {
            crc = pak::Crc32(std::span<const u8>(buffer.data(), static_cast<usize>(count)), crc);
            bytes += static_cast<u64>(count);
        }
    }
    return in.eof();
}

std::string CrcHex(u32 crc) {
    char text[9];
    std::snprintf(text, sizeof(text), "%08x", crc);
    return text;
}

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

    // Cook beside Paks/ so validation failures leave the previous package intact.
    CookOptions cook = options.cook;
    report.paks_dir = options.output_dir / "Paks";
    stdfs::create_directories(options.output_dir, ec);
    if (ec) return fail("Can't create " + options.output_dir.string() + ": " + ec.message());
    stdfs::path staged_paks, previous_paks;
    bool staged_ready = false;
    for (usize i = 0; i < 1024; ++i) {
        staged_paks = options.output_dir / (".aether-paks-staging-" + std::to_string(i));
        previous_paks = options.output_dir / (".aether-paks-previous-" + std::to_string(i));
        if (stdfs::exists(previous_paks, ec)) continue;
        if (ec) return fail("Can't inspect " + previous_paks.string() + ": " + ec.message());
        if (stdfs::create_directory(staged_paks, ec)) {
            staged_ready = true;
            break;
        }
        if (ec) return fail("Can't create " + staged_paks.string() + ": " + ec.message());
    }
    if (!staged_ready) return fail("No free staging folder beside " + report.paks_dir.string());
    cook.output_dir = staged_paks;
    if (options.cook.progress) {
        cook.progress = [&](f32 fraction, const std::string& stage) { options.cook.progress(fraction * 0.9f, stage); };
    }
    report.cook = Cook(cook);
    if (!report.cook.ok) {
        stdfs::remove_all(staged_paks, ec);
        return fail(report.cook.error);
    }

    const bool had_previous = stdfs::exists(report.paks_dir, ec);
    if (ec) {
        stdfs::remove_all(staged_paks, ec);
        return fail("Can't inspect " + report.paks_dir.string());
    }
    if (had_previous) {
        stdfs::rename(report.paks_dir, previous_paks, ec);
        if (ec) {
            const std::string message = "Can't move the previous Paks folder: " + ec.message();
            stdfs::remove_all(staged_paks, ec);
            return fail(message);
        }
    }
    stdfs::rename(staged_paks, report.paks_dir, ec);
    if (ec) {
        const std::string message = "Can't install the new Paks folder: " + ec.message();
        if (had_previous) {
            std::error_code restore_error;
            stdfs::rename(previous_paks, report.paks_dir, restore_error);
            if (restore_error) return fail(message + "; restoring the previous folder failed: " + restore_error.message());
        }
        stdfs::remove_all(staged_paks, ec);
        return fail(message);
    }
    report.cook.pak_file = report.paks_dir / report.cook.pak_file.filename();
    report.cook.manifest_file = report.paks_dir / report.cook.manifest_file.filename();
    if (had_previous) {
        stdfs::remove_all(previous_paks, ec);
        if (ec) AETHER_LOG_WARN("Package", "Couldn't remove previous Paks folder: %s", ec.message().c_str());
    }

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
    std::vector<stdfs::path> files = report.copied;
    files.push_back(report.cook.pak_file);
    files.push_back(report.cook.manifest_file);
    std::sort(files.begin(), files.end());
    nlohmann::json file_list = nlohmann::json::array();
    for (const stdfs::path& file : files) {
        u64 bytes = 0;
        u32 crc = 0;
        if (!FileChecksum(file, bytes, crc)) return fail("Can't checksum " + file.string());
        file_list.push_back({{"path", file.lexically_relative(options.output_dir).generic_string()},
                             {"bytes", bytes}, {"crc32", CrcHex(crc)}});
    }
    const nlohmann::json manifest = {{"$type", "PackageManifest"},
                                     {"$version", 1},
                                     {"project", settings.name},
                                     {"configuration", ConfigurationName(options.cook.configuration)},
                                     {"checksum", "crc32"},
                                     {"files", std::move(file_list)}};
    report.manifest_file = options.output_dir / "PackageManifest.json";
    const std::string manifest_text = manifest.dump(2);
    if (!fs::WriteFileAtomic(report.manifest_file.string(), manifest_text.data(), manifest_text.size())) {
        return fail("Can't write " + report.manifest_file.string());
    }
    progress(1.0f, "Packaged " + report.game_executable.filename().string());
    report.ok = true;
    AETHER_LOG_INFO("Package", "Packaged %s into %s", settings.name.c_str(), options.output_dir.string().c_str());
    return report;
}

bool VerifyPackageManifest(const stdfs::path& directory, std::vector<std::string>& problems) {
    problems.clear();
    std::vector<u8> bytes;
    const stdfs::path manifest_file = directory / "PackageManifest.json";
    if (!fs::ReadFileBytes(manifest_file.string(), bytes)) {
        problems.push_back("Can't read " + manifest_file.string());
        return false;
    }
    const nlohmann::json manifest = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (!manifest.is_object() || !manifest.contains("$type") || !manifest["$type"].is_string() ||
        manifest["$type"] != "PackageManifest" || !manifest.contains("$version") ||
        !manifest["$version"].is_number_integer() || manifest["$version"] != 1 ||
        !manifest.contains("checksum") || !manifest["checksum"].is_string() || manifest["checksum"] != "crc32" ||
        !manifest.contains("files") || !manifest["files"].is_array()) {
        problems.push_back("Invalid PackageManifest.json");
        return false;
    }
    std::set<std::string> seen;
    for (const nlohmann::json& entry : manifest["files"]) {
        if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string() ||
            !entry.contains("bytes") || !entry["bytes"].is_number_unsigned() ||
            !entry.contains("crc32") || !entry["crc32"].is_string()) {
            problems.push_back("Invalid file entry in PackageManifest.json");
            continue;
        }
        const std::string path = entry["path"].get<std::string>();
        const stdfs::path local_path(path);
        if (path.empty() || pak::NormalizePath(path) != path || local_path.is_absolute() || local_path.has_root_name() ||
            !seen.insert(path).second) {
            problems.push_back("Invalid or repeated package path: " + path);
            continue;
        }
        std::error_code ec;
        if (stdfs::is_symlink(directory / local_path, ec)) {
            problems.push_back("Package file is a symlink: " + path);
            continue;
        }
        u64 file_bytes = 0;
        u32 crc = 0;
        if (!FileChecksum(directory / path, file_bytes, crc)) {
            problems.push_back("Can't read " + path);
        } else if (file_bytes != entry["bytes"].get<u64>() || CrcHex(crc) != entry["crc32"].get<std::string>()) {
            problems.push_back("Size or checksum mismatch: " + path);
        }
    }
    if (seen.empty()) problems.push_back("PackageManifest.json lists no files");
    return problems.empty();
}

} // namespace aether::cook
