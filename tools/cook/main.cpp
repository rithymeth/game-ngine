// aether_cook: cooks a project's content for shipping (Phase 25 step 2,
// docs/design/PHASE_SPECS.md §25.2).
//
//   aether_cook <project.aproject> --out <dir> [--config debug|development|shipping]
//               [--always <path-or-folder/>]... [--compression auto|lz4|zstd|none]
//               [--no-imported] [--pak-name Game] [--verbose]
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
                 "                   [--no-imported] [--pak-name Game] [--verbose]\n");
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
        } else if (arg == "--pak-name" && has_value) {
            options.pak_name = argv[++i];
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
    std::printf("%llu bytes -> %llu bytes\n", static_cast<unsigned long long>(report.original_bytes),
                static_cast<unsigned long long>(report.pak_bytes));
    return 0;
}
