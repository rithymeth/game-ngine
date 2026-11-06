// aether_package: cooks a project and stages it with the player as a folder
// that runs on its own (Phase 25 step 5, docs/manual/06-packaging.md).
//
//   aether_package <project.aproject> --out <dir> [--config debug|development|shipping]
//                   [--player <aether_player>] [--strict] [--verbose]
//                   [--always <path-or-folder/>]... [--pak-name Game]
//                   [--compression auto|lz4|zstd|none] [--key <64 hex digits>]
//
// The editor's Build and Package window runs the same engine call
// (cook::Package); this is that path without the window, so a scripted or CI
// build can produce a runnable package instead of hand-assembling one. The
// player is found the same way the editor finds it -- beside this executable,
// else the one this engine was built with -- so a build tree needs no
// arguments beyond the project and the destination.
//
// The package is left exactly as a runnable folder:
//   <dir>/<Project>[.exe]         the player, named for the project
//   <dir>/*.dll                   the player's DLLs (Windows)
//   <dir>/Paks/<pak>.apak         the cooked content, with CookManifest.json
//   <dir>/PackageManifest.json    sizes and CRC-32s, verified after staging
#include "aether/cook/package.h"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace aether;

namespace {

int Usage() {
    std::fprintf(stderr,
                 "usage: aether_package <project.aproject> --out <dir> [--config debug|development|shipping]\n"
                 "                    [--player <aether_player>] [--strict] [--verbose]\n"
                 "                    [--always <path-or-folder/>]... [--pak-name Game]\n"
                 "                    [--compression auto|lz4|zstd|none] [--key <64 hex digits>]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return Usage();

    cook::PackageOptions options;
    options.cook.project_file = argv[1];
    bool verbose = false;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--out" && has_value) {
            options.output_dir = argv[++i];
        } else if (arg == "--config" && has_value) {
            if (!cook::ParseConfiguration(argv[++i], options.cook.configuration)) return Usage();
        } else if (arg == "--player" && has_value) {
            options.player_executable = argv[++i];
        } else if (arg == "--always" && has_value) {
            options.cook.always_cook.push_back(argv[++i]);
        } else if (arg == "--pak-name" && has_value) {
            options.cook.pak_name = argv[++i];
        } else if (arg == "--compression" && has_value) {
            const std::string c = argv[++i];
            options.cook.compression = c == "lz4"    ? pak::CompressionPolicy::LZ4
                                       : c == "zstd" ? pak::CompressionPolicy::Zstd
                                       : c == "none" ? pak::CompressionPolicy::None
                                                     : pak::CompressionPolicy::Auto;
        } else if (arg == "--strict") {
            options.cook.strict_validation = true;
        } else if (arg == "--key" && has_value) {
            pak::PakKey key;
            if (!pak::PakKey::FromHex(argv[++i], key)) {
                std::fprintf(stderr, "aether_package: --key takes 64 hex digits\n");
                return 2;
            }
            options.cook.encryption_key = key;
        } else if (arg == "--verbose") {
            verbose = true;
        } else {
            return Usage();
        }
    }
    if (options.output_dir.empty()) return Usage();

    // A failure here names what is wrong with the project, the destination, or
    // the player it would stage; the editor reports the same string.
    const cook::PackageReport report = cook::Package(options);
    for (const std::string& warning : report.cook.warnings) std::printf("warning: %s\n", warning.c_str());
    if (!report.ok) {
        std::fprintf(stderr, "aether_package: %s\n", report.error.c_str());
        if (report.error.find("aether_player") != std::string::npos) {
            std::fprintf(stderr, "               build the player target, or pass --player <path>\n");
        }
        return 1;
    }

    if (verbose) {
        for (const cook::CookedAsset& asset : report.cook.assets) {
            std::printf("  %-10s %8llu  %s%s\n", asset.importer.c_str(),
                        static_cast<unsigned long long>(asset.bytes), asset.path.c_str(),
                        asset.imported ? " +imported" : "");
        }
        for (const std::filesystem::path& copied : report.copied) {
            std::printf("  staged     %s\n", copied.string().c_str());
        }
    }

    std::printf("%s (%s): %zu assets, %zu files, %zu skipped\n", report.game_executable.string().c_str(),
                cook::ConfigurationName(options.cook.configuration), report.cook.assets.size(),
                report.cook.extra_files.size(), report.cook.skipped);
    std::printf("paks:     %s\n", report.paks_dir.string().c_str());
    std::printf("manifest: %s\n", report.manifest_file.string().c_str());
    std::printf("staged %zu file(s) beside the player; run %s to play\n", report.copied.size(),
                report.game_executable.string().c_str());
    return 0;
}
