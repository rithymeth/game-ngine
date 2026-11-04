#pragma once

// @stability: experimental
//
// Plugin packaging (Phase 46 step 3, docs/design/UPGRADE_PLAN.md). A plugin folder becomes one `.apkg` file (an
// .apak archive whose paths all start with `<Name>/`) that can be inspected without unpacking, checked against what
// is installed, and installed into a Plugins/ folder. Installing never trusts the archive: every path must stay
// inside `<Name>/`, the descriptor must match the folder, sizes are capped, and the files are written to a staging
// folder first, so a failure leaves nothing half installed.

#include "aether/plugin/plugin.h"

#include <filesystem>
#include <string>
#include <vector>

namespace aether::plugin {

inline constexpr const char* kPackageExtension = ".apkg";
inline constexpr usize kMaxPackageEntries = 100000;
inline constexpr u64 kMaxPackageBytes = 2ull * 1024 * 1024 * 1024; // unpacked

// Packs `<plugin_dir>` (its descriptor must be `<Name>/<Name>.aplugin`, valid, for this ABI) into `package_file`.
[[nodiscard]] bool PackagePlugin(const std::filesystem::path& plugin_dir, const std::filesystem::path& package_file,
                                 std::string* error = nullptr);

// Reads the descriptor of a package without unpacking it; false if it isn't a well formed plugin package.
[[nodiscard]] bool InspectPluginPackage(const std::filesystem::path& package_file, PluginDescriptor& out,
                                        std::string* error = nullptr);

// The required dependencies of `descriptor` that `installed` does not satisfy (missing, or older than
// min_version), as readable lines such as "Physics >= 1.2 (found 1.0.0)". Optional dependencies are skipped.
std::vector<std::string> UnmetDependencies(const PluginDescriptor& descriptor, const std::vector<PluginInfo>& installed);

// Unpacks into `<plugins_dir>/<Name>/`. An existing folder of that name is refused unless `replace`, which moves it
// aside and restores it if the install fails. Dependencies are not installed here; check UnmetDependencies first.
[[nodiscard]] bool InstallPluginPackage(const std::filesystem::path& package_file, const std::filesystem::path& plugins_dir,
                                        bool replace = false, std::string* error = nullptr);

} // namespace aether::plugin
