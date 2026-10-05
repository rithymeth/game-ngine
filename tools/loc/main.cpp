// aether_loc: the localization gather step (Phase 29 step 4,
// docs/design/PHASE_SPECS.md §29.4).
//
//   aether_loc <project.aproject> [--strings <Content-relative .astrings>]
//              [--source-language en] [--languages fr,de] [--po-out <dir>]
//              [--po-in <dir|file.po>] [--check]
//
// Scans the project's scenes, prefabs, UI layouts and Blueprints for
// localizable text, adds the new keys to the string table (default
// Content/Localization/strings.astrings, CSV: key,en,fr,...), applies
// translated .po files (--po-in), and writes .po files for translators
// (--po-out, one per language). No translation is changed and no key is
// removed: keys nothing uses any more are listed. With --check nothing is
// written. Exit 0 on success, 1 on an error, 2 on bad usage.
#include "aether/loc/project_sync.h"
#include "aether/project/project.h"

#include <cstdio>
#include <sstream>
#include <string>

using namespace aether;

namespace {

int Usage() {
    std::fprintf(stderr,
                 "usage: aether_loc <project.aproject> [--strings <Content-relative .astrings>] [--source-language en]\n"
                 "                  [--languages fr,de] [--po-out <dir>] [--po-in <dir|file.po>] [--check]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return Usage();
    const std::filesystem::path project_file = argv[1];
    std::string strings = "Localization/strings.astrings";
    loc::SyncOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--strings" && has_value) {
            strings = argv[++i];
        } else if (arg == "--source-language" && has_value) {
            options.source_language = argv[++i];
        } else if (arg == "--languages" && has_value) {
            std::stringstream list(argv[++i]);
            for (std::string item; std::getline(list, item, ',');) {
                if (!item.empty()) options.languages.push_back(item);
            }
        } else if (arg == "--po-out" && has_value) {
            options.po_out = argv[++i];
        } else if (arg == "--po-in" && has_value) {
            options.po_in = argv[++i];
        } else if (arg == "--check") {
            options.write = false;
        } else {
            return Usage();
        }
    }
    ProjectSettings settings;
    std::string error;
    if (!LoadProject(project_file, settings, &error)) {
        std::fprintf(stderr, "aether_loc: %s\n", error.c_str());
        return 1;
    }
    const ProjectPaths paths = ProjectPaths::ForFile(project_file);
    options.content_dir = paths.content;
    options.strings_file = paths.content / strings;
    const loc::SyncReport report = loc::SyncProject(options);
    for (const std::string& m : report.messages) std::fprintf(report.ok ? stdout : stderr, "%s: %s\n", report.ok ? "warning" : "aether_loc", m.c_str());
    if (!report.ok) return 1;
    std::printf("%zu string(s) found, %zu added, %zu source text(s) changed, %zu unused; %zu .po file(s) read, %zu written%s\n",
                report.gathered.entries.size(), report.merge.added, report.merge.updated_source, report.merge.unused.size(), report.po_imported,
                report.po_written, options.write ? "" : " (--check: nothing was written)");
    for (const std::string& key : report.merge.unused) std::printf("unused: %s\n", key.c_str());
    return 0;
}
