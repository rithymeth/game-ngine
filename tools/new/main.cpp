// aether_new: starts a project from a template on the command line.
//
//   aether_new --list
//   aether_new <Name> [--template <id>] [--dir <parent folder>]
//
// The editor's New Project window calls the same function (templates::CreateProjectFromTemplate); this is that path without the
// editor, so the way from nothing to a game that runs is three commands on any platform the engine builds on:
//
//   aether_new MyGame --template third_person
//   aether_package MyGame/MyGame.aproject --out MyGame/Build
//   MyGame/Build/MyGame
//
// It prints those next two commands with the real paths when it has made the project.
#include "aether/templates/templates.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace aether;
namespace stdfs = std::filesystem;

namespace {

int Usage() {
    std::fprintf(stderr, "usage: aether_new --list\n"
                         "       aether_new <Name> [--template <id>] [--dir <parent folder>]\n"
                         "The template defaults to blank. `aether_new --list` shows the others.\n");
    return 2;
}

void ListTemplates(std::FILE* out) {
    for (const templates::ProjectTemplate& t : templates::ProjectTemplates()) {
        std::fprintf(out, "  %-14s %s (%s)%s\n", t.id.c_str(), t.name.c_str(), t.genre.c_str(), t.available ? "" : "  [not available]");
        std::fprintf(out, "                 %s\n", t.description.c_str());
        if (!t.available && !t.unavailable_reason.empty()) std::fprintf(out, "                 %s\n", t.unavailable_reason.c_str());
    }
}

// A path as the user would type it in a shell here: quoted only when it has a space.
std::string Shown(const stdfs::path& path) {
    const std::string text = path.string();
    return text.find(' ') == std::string::npos ? text : "\"" + text + "\"";
}

} // namespace

int main(int argc, char** argv) {
    std::string name;
    std::string template_id = "blank";
    stdfs::path parent = ".";
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--list") {
            list = true;
        } else if (arg == "--template" && has_value) {
            template_id = argv[++i];
            std::replace(template_id.begin(), template_id.end(), '-', '_'); // third-person is third_person
        } else if (arg == "--dir" && has_value) {
            parent = argv[++i];
        } else if (!arg.empty() && arg[0] != '-' && name.empty()) {
            name = arg;
        } else {
            return Usage();
        }
    }
    if (list) {
        std::printf("Templates:\n");
        ListTemplates(stdout);
        return 0;
    }
    if (name.empty()) return Usage();
    if (templates::FindProjectTemplate(template_id) == nullptr) {
        std::fprintf(stderr, "aether_new: there is no template called '%s'. The templates are:\n", template_id.c_str());
        ListTemplates(stderr);
        return 1;
    }
    ProjectPaths paths;
    std::string error;
    if (!templates::CreateProjectFromTemplate(parent, name, template_id, &paths, &error)) {
        std::fprintf(stderr, "aether_new: %s\n", error.c_str());
        return 1;
    }
    const stdfs::path build = paths.root / "Build";
#if defined(_WIN32)
    const stdfs::path game = build / (name + ".exe");
#else
    const stdfs::path game = build / name;
#endif
    std::printf("Created %s from the %s template.\n", Shown(paths.file).c_str(), templates::FindProjectTemplate(template_id)->name.c_str());
    std::printf("To build it into a folder that runs on its own, and play it:\n");
    std::printf("  aether_package %s --out %s\n", Shown(paths.file).c_str(), Shown(build).c_str());
    std::printf("  %s\n", Shown(game).c_str());
    return 0;
}
