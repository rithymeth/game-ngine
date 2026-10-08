// aether_cook: cooks a project's content for shipping (Phase 25 step 2,
// docs/design/PHASE_SPECS.md §25.2).
//
//   aether_cook <project.aproject> --out <dir> [--config debug|development|shipping]
//               [--always <path-or-folder/>]... [--compression auto|lz4|zstd|none]
//               [--no-imported] [--pak-name Game] [--strict] [--verbose]
//               [--key <64 hex digits>] [--patch-of <base.apak>]
//               [--dlc <Name> [--dlc-base <base.apak>]]
//
// Phase 25 step 6 (§25.6): --key encrypts the archive; --patch-of writes
// only what changed since an earlier archive (name the patch so it sorts
// after the base, e.g. --pak-name Game_p1); --dlc cooks the --always roots
// as a DLC, leaving out what --dlc-base already has.
#include "aether/cook/cooker.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace aether;

namespace {

int Usage() {
    std::fprintf(stderr,
                 "usage: aether_cook <project.aproject> --out <dir> [--config debug|development|shipping]\n"
                 "                   [--always <path-or-folder/>]... [--compression auto|lz4|zstd|none]\n"
                 "                   [--no-imported] [--pak-name Game] [--strict] [--verbose]\n"
                 "                   [--key <64 hex digits>] [--patch-of <base.apak>]\n"
                 "                   [--dlc <Name> [--dlc-base <base.apak>]]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return Usage();
    cook::CookOptions options;
    options.project_file = argv[1];
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--out" && has_value) {
            options.output_dir = argv[++i];
        } else if (arg == "--config" && has_value) {
            if (!cook::ParseConfiguration(argv[++i], options.configuration)) return Usage();
        } else if (arg == "--always" && has_value) {
            options.always_cook.push_back(argv[++i]);
        } else if (arg == "--compression" && has_value) {
            const std::string c = argv[++i];
            options.compression = c == "lz4"    ? pak::CompressionPolicy::LZ4
                                  : c == "zstd" ? pak::CompressionPolicy::Zstd
                                  : c == "none" ? pak::CompressionPolicy::None
                                                : pak::CompressionPolicy::Auto;
        } else if (arg == "--no-imported") {
            options.include_imported = false;
        } else if (arg == "--strict") {
            options.strict_validation = true;
        } else if (arg == "--pak-name" && has_value) {
            options.pak_name = argv[++i];
        } else if (arg == "--key" && has_value) {
            pak::PakKey key;
            if (!pak::PakKey::FromHex(argv[++i], key)) {
                std::fprintf(stderr, "aether_cook: --key takes 64 hex digits\n");
                return 2;
            }
            options.encryption_key = key;
        } else if (arg == "--patch-of" && has_value) {
            options.patch_base = argv[++i];
        } else if (arg == "--dlc" && has_value) {
            options.dlc_name = argv[++i];
        } else if (arg == "--dlc-base" && has_value) {
            options.dlc_base = argv[++i];
        } else if (arg == "--verbose") {
            verbose = true;
        } else {
            return Usage();
        }
    }
    if (options.output_dir.empty()) return Usage();

    const cook::CookReport report = cook::Cook(options);
    for (const std::string& warning : report.warnings) std::printf("warning: %s\n", warning.c_str());
    if (!report.ok) {
        std::fprintf(stderr, "aether_cook: %s\n", report.error.c_str());
        return 1;
    }
    if (verbose) {
        for (const cook::CookedAsset& a : report.assets) {
            std::printf("  %-10s %8llu  %s%s  (%s)\n", a.importer.c_str(), static_cast<unsigned long long>(a.bytes),
                        a.path.c_str(), a.imported ? " +imported" : "", a.reason.c_str());
        }
        for (const std::string& f : report.extra_files) std::printf("  %-10s %8s  %s\n", "file", "", f.c_str());
    }
    std::printf("%s (%s): %zu assets, %zu files, %zu skipped, %zu editor-only fields stripped\n",
                report.pak_file.string().c_str(), cook::ConfigurationName(options.configuration), report.assets.size(),
                report.extra_files.size(), report.skipped, report.stripped_fields);
    if (report.is_patch) {
        std::printf("patch: %zu added, %zu changed, %zu removed, %zu unchanged\n", report.patch.added.size(),
                    report.patch.changed.size(), report.patch.removed.size(), report.patch.unchanged);
    }
    if (!options.dlc_name.empty()) std::printf("DLC %s: %zu assets the base already has left out\n", options.dlc_name.c_str(), report.in_base);
    if (options.encryption_key) std::printf("encrypted (key id %08x)\n", options.encryption_key->Id());
    std::printf("%llu bytes -> %llu bytes\n", static_cast<unsigned long long>(report.original_bytes),
                static_cast<unsigned long long>(report.pak_bytes));
    std::printf("texture cache: %zu reused, %zu cooked\n", report.texture_cache_hits, report.texture_cache_misses);
    return 0;
}
