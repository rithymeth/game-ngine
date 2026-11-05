// aether_plugin (Phase 46 step 3): package, inspect and install .apkg plugin packages from the command line.
// The packaging itself lives in aether/plugin/package.h; this is the CLI the upgrade plan asked for.
#include "aether/plugin/package.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace aether;

namespace {

int Usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  aether_plugin package <plugin_dir> -o <file.apkg>\n"
                 "  aether_plugin inspect <file.apkg>\n"
                 "  aether_plugin install <file.apkg> --plugins-dir <dir> [--replace]\n");
    return 2;
}

void PrintDescriptor(const plugin::PluginDescriptor& d) {
    std::printf("name: %s\n", d.name.c_str());
    std::printf("friendly name: %s\n", d.friendly_name.c_str());
    std::printf("version: %s\n", d.version.c_str());
    std::printf("abi: %u\n", d.abi);
    std::printf("category: %s\n", d.category.c_str());
    std::printf("engine: %s\n", d.engine_version.empty() ? "any" : d.engine_version.c_str());
    std::printf("content: %s\n", d.has_content ? "yes" : "no");
    std::printf("modules: %zu\n", d.modules.size());
    for (const plugin::ModuleDesc& m : d.modules) std::printf("  - %s (%d)\n", m.name.c_str(), static_cast<int>(m.type));
    std::printf("dependencies: %zu\n", d.dependencies.size());
    for (const plugin::PluginDependency& dep : d.dependencies) {
        const std::string min = dep.min_version.empty() ? "" : " >= " + dep.min_version;
        std::printf("  - %s%s%s\n", dep.name.c_str(), min.c_str(), dep.optional ? " (optional)" : "");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return Usage();
    const std::string command = argv[1];
    std::string error;

    if (command == "package") {
        if (argc < 4) return Usage();
        const fs::path plugin_dir = argv[2];
        fs::path output;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) output = argv[++i];
        }
        if (output.empty()) return Usage();
        if (!plugin::PackagePlugin(plugin_dir, output, &error)) {
            std::fprintf(stderr, "package failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("packaged %s -> %s\n", plugin_dir.string().c_str(), output.string().c_str());
        return 0;
    }

    if (command == "inspect") {
        if (argc < 3) return Usage();
        plugin::PluginDescriptor descriptor;
        if (!plugin::InspectPluginPackage(argv[2], descriptor, &error)) {
            std::fprintf(stderr, "inspect failed: %s\n", error.c_str());
            return 1;
        }
        PrintDescriptor(descriptor);
        return 0;
    }

    if (command == "install") {
        if (argc < 3) return Usage();
        const fs::path package = argv[2];
        fs::path plugins_dir;
        bool replace = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--plugins-dir") == 0 && i + 1 < argc) plugins_dir = argv[++i];
            else if (std::strcmp(argv[i], "--replace") == 0) replace = true;
        }
        if (plugins_dir.empty()) return Usage();
        plugin::PluginDescriptor descriptor;
        if (!plugin::InspectPluginPackage(package, descriptor, &error)) {
            std::fprintf(stderr, "inspect failed: %s\n", error.c_str());
            return 1;
        }
        if (!plugin::InstallPluginPackage(package, plugins_dir, replace, &error)) {
            std::fprintf(stderr, "install failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("installed %s %s into %s\n", descriptor.name.c_str(), descriptor.version.c_str(),
                    plugins_dir.string().c_str());
        return 0;
    }

    return Usage();
}
