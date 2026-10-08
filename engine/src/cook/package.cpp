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

// Inspect each component before opening a package file. Checking only the
// final file misses linked parent folders (including a linked Paks folder).
bool CheckPackageFile(const stdfs::path& directory, const stdfs::path& relative, std::string& error) {
    stdfs::path current = directory;
    for (auto it = relative.begin(); it != relative.end(); ++it) {
        current /= *it;
        std::error_code ec;
        const stdfs::file_status status = stdfs::symlink_status(current, ec);
        if (ec) {
            error = "Can't inspect " + current.string() + ": " + ec.message();
            return false;
        }
        if (stdfs::is_symlink(status)) {
            error = "Package path is a symlink: " + current.string();
            return false;
        }
        auto next = it;
        ++next;
        if (next == relative.end() ? !stdfs::is_regular_file(status) : !stdfs::is_directory(status)) {
            error = "Package path has the wrong file type: " + current.string();
            return false;
        }
    }
    return true;
}

bool CheckDestination(const stdfs::path& destination, bool directory, bool& exists, std::string& error) {
    std::error_code ec;
    const stdfs::file_status status = stdfs::symlink_status(destination, ec);
    if (ec == std::errc::no_such_file_or_directory) ec.clear();
    if (ec) {
        error = "Can't inspect " + destination.string() + ": " + ec.message();
        return false;
    }
    exists = status.type() != stdfs::file_type::not_found;
    if (!exists) return true;
    if (stdfs::is_symlink(status)) {
        error = "Refusing to replace symlink " + destination.string();
        return false;
    }
    if (directory ? !stdfs::is_directory(status) : !stdfs::is_regular_file(status)) {
        error = std::string(directory ? "Refusing to replace non-directory " : "Refusing to replace non-file ") + destination.string();
        return false;
    }
    return true;
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

    // Prepare every package artifact before replacing any published file.
    CookOptions cook = options.cook;
    report.paks_dir = options.output_dir / "Paks";
    stdfs::create_directories(options.output_dir, ec);
    if (ec) return fail("Can't create " + options.output_dir.string() + ": " + ec.message());
    stdfs::path staged_root, previous_root;
    bool staged_ready = false;
    for (usize i = 0; i < 1024; ++i) {
        staged_root = options.output_dir / (".aether-package-staging-" + std::to_string(i));
        previous_root = options.output_dir / (".aether-package-previous-" + std::to_string(i));
        if (stdfs::exists(staged_root, ec) || stdfs::exists(previous_root, ec)) continue;
        if (ec) return fail("Can't inspect package staging folders: " + ec.message());
        if (!stdfs::create_directory(staged_root, ec)) {
            if (ec) return fail("Can't create " + staged_root.string() + ": " + ec.message());
            continue;
        }
        if (stdfs::create_directory(previous_root, ec)) {
            staged_ready = true;
            break;
        }
        std::error_code cleanup_ec;
        stdfs::remove_all(staged_root, cleanup_ec);
        if (ec) return fail("Can't create " + previous_root.string() + ": " + ec.message());
    }
    if (!staged_ready) return fail("No free staging folder in " + options.output_dir.string());
    const auto cleanup_staging = [&] {
        std::error_code cleanup_ec;
        stdfs::remove_all(staged_root, cleanup_ec);
        if (cleanup_ec) AETHER_LOG_WARN("Package", "Couldn't remove staging folder: %s", cleanup_ec.message().c_str());
        stdfs::remove_all(previous_root, cleanup_ec);
        if (cleanup_ec) AETHER_LOG_WARN("Package", "Couldn't remove backup folder: %s", cleanup_ec.message().c_str());
    };
    cook.output_dir = staged_root / "Paks";
    if (options.cook.progress) {
        cook.progress = [&](f32 fraction, const std::string& stage) { options.cook.progress(fraction * 0.9f, stage); };
    }
    report.cook = Cook(cook);
    if (!report.cook.ok) {
        cleanup_staging();
        return fail(report.cook.error);
    }

    progress(0.92f, "Staging the player");
    if (cook.cancel && cook.cancel->load()) {
        cleanup_staging();
        return fail("Cancelled");
    }
    report.game_executable = options.output_dir / GameExecutableName(settings.name);
    const stdfs::path staged_player = staged_root / report.game_executable.filename();
    stdfs::copy_file(player, staged_player, stdfs::copy_options::overwrite_existing, ec);
    if (ec) {
        const std::string message = "Can't stage the player: " + ec.message();
        cleanup_staging();
        return fail(message);
    }
#if !defined(_WIN32)
    stdfs::permissions(staged_player,
                       stdfs::perms::owner_exec | stdfs::perms::group_exec | stdfs::perms::others_exec,
                       stdfs::perm_options::add, ec);
    if (ec) {
        const std::string message = "Can't mark the staged player executable: " + ec.message();
        cleanup_staging();
        return fail(message);
    }
#endif
    report.copied.push_back(report.game_executable);
#if defined(_WIN32)
    // The DLLs the player loads (dxcompiler.dll and the like).
    for (stdfs::directory_iterator it(player.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".dll") continue;
        const stdfs::path to = options.output_dir / it->path().filename();
        std::error_code copy_ec;
        stdfs::copy_file(it->path(), staged_root / to.filename(), stdfs::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            const std::string message = "Can't stage " + it->path().string() + ": " + copy_ec.message();
            cleanup_staging();
            return fail(message);
        }
        report.copied.push_back(to);
    }
    if (ec) {
        const std::string message = "Can't list player DLLs: " + ec.message();
        cleanup_staging();
        return fail(message);
    }
#endif
    std::vector<stdfs::path> files;
    for (const stdfs::path& copied : report.copied) files.push_back(staged_root / copied.filename());
    files.push_back(report.cook.pak_file);
    files.push_back(report.cook.manifest_file);
    std::sort(files.begin(), files.end());
    nlohmann::json file_list = nlohmann::json::array();
    for (const stdfs::path& file : files) {
        u64 bytes = 0;
        u32 crc = 0;
        if (!FileChecksum(file, bytes, crc)) {
            cleanup_staging();
            return fail("Can't checksum " + file.string());
        }
        file_list.push_back({{"path", file.lexically_relative(staged_root).generic_string()},
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
    if (!fs::WriteFileAtomic((staged_root / "PackageManifest.json").string(), manifest_text.data(), manifest_text.size())) {
        cleanup_staging();
        return fail("Can't stage " + report.manifest_file.string());
    }
    std::vector<std::string> problems;
    if (!VerifyPackageManifest(staged_root, problems)) {
        const std::string message = problems.empty() ? "Can't verify staged package" : problems.front();
        cleanup_staging();
        return fail(message);
    }

    // Move previous artifacts aside, install the staged ones, then publish the
    // manifest last. On failure, restore each previous artifact in reverse order.
    struct SwapEntry {
        stdfs::path name;
        bool had_previous = false;
        bool installed = false;
    };
    std::vector<SwapEntry> swaps;
    swaps.push_back({"Paks"});
    for (const stdfs::path& copied : report.copied) swaps.push_back({copied.filename()});
    swaps.push_back({"PackageManifest.json"});
    // Reject unexpected files/folders before moving any published artifact.
    // Recheck at each swap as well, since the destination may have changed.
    for (const SwapEntry& entry : swaps) {
        bool exists = false;
        std::string error;
        if (!CheckDestination(options.output_dir / entry.name, entry.name == "Paks", exists, error)) {
            cleanup_staging();
            return fail(error);
        }
    }
    progress(0.98f, "Installing the package");
    std::string install_error;
    for (SwapEntry& entry : swaps) {
        if (cook.cancel && cook.cancel->load()) {
            install_error = "Cancelled";
            break;
        }
        const stdfs::path destination = options.output_dir / entry.name;
        bool exists = false;
        if (!CheckDestination(destination, entry.name == "Paks", exists, install_error)) break;
        entry.had_previous = exists;
        if (entry.had_previous) {
            stdfs::rename(destination, previous_root / entry.name, ec);
            if (ec) {
                install_error = "Can't back up " + destination.string() + ": " + ec.message();
                entry.had_previous = false;
                break;
            }
        }
        stdfs::rename(staged_root / entry.name, destination, ec);
        if (ec) {
            install_error = "Can't install " + destination.string() + ": " + ec.message();
            break;
        }
        entry.installed = true;
    }
    if (!install_error.empty()) {
        std::string restore_error;
        for (auto it = swaps.rbegin(); it != swaps.rend(); ++it) {
            const stdfs::path destination = options.output_dir / it->name;
            std::error_code rollback_ec;
            if (it->installed) stdfs::remove_all(destination, rollback_ec);
            if (!rollback_ec && it->had_previous) stdfs::rename(previous_root / it->name, destination, rollback_ec);
            if (rollback_ec && restore_error.empty()) restore_error = rollback_ec.message();
        }
        if (restore_error.empty()) cleanup_staging();
        return fail(install_error + (restore_error.empty() ? "" : "; rollback failed (previous files are in " + previous_root.string() + "): " + restore_error));
    }
    report.cook.pak_file = report.paks_dir / report.cook.pak_file.filename();
    report.cook.manifest_file = report.paks_dir / report.cook.manifest_file.filename();
    cleanup_staging();
    progress(1.0f, "Packaged " + report.game_executable.filename().string());
    report.ok = true;
    AETHER_LOG_INFO("Package", "Packaged %s into %s", settings.name.c_str(), options.output_dir.string().c_str());
    return report;
}

bool VerifyPackageManifest(const stdfs::path& directory, std::vector<std::string>& problems) {
    problems.clear();
    std::vector<u8> bytes;
    const stdfs::path manifest_file = directory / "PackageManifest.json";
    std::string error;
    if (!CheckPackageFile(directory, "PackageManifest.json", error)) {
        problems.push_back(error);
        return false;
    }
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
        // NUL can truncate native filesystem calls. ':' can address a Windows
        // drive or alternate data stream even on a manifest read on Unix.
        if (path.empty() || path.find('\0') != std::string::npos || path.find(':') != std::string::npos ||
            pak::NormalizePath(path) != path) {
            problems.push_back("Invalid or repeated package path: " + path);
            continue;
        }
        const stdfs::path local_path(path);
        std::string identity = path;
#if defined(_WIN32)
        std::transform(identity.begin(), identity.end(), identity.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
#endif
        if (local_path.is_absolute() || local_path.has_root_name() || !seen.insert(identity).second) {
            problems.push_back("Invalid or repeated package path: " + path);
            continue;
        }
        if (!CheckPackageFile(directory, local_path, error)) {
            problems.push_back(error);
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
