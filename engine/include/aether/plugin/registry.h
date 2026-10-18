#pragma once

// @stability: experimental
//
// A local, user-managed store for immutable .apkg plugin packages. This is a
// filesystem registry only: it has no network, account, signing, or review
// service. See docs/manual/14-plugin-marketplace.md for the marketplace format
// and trust model.

#include "aether/plugin/package.h"

#include <utility>

namespace aether::plugin {

struct RegistryPackage {
    PluginDescriptor descriptor;
    std::filesystem::path package_file;
};

// A local registry uses <root>/<Plugin>/<version>.apkg as its index. Package
// metadata is read from the archive descriptor, so an index cannot disagree
// with the package it names. Versions use numeric dotted form (for example
// "1.2.0") and an existing version is never overwritten.
class LocalPluginRegistry {
public:
    explicit LocalPluginRegistry(std::filesystem::path root) : root_(std::move(root)) {}

    const std::filesystem::path& Root() const { return root_; }

    // Validates and copies a package into this registry. Publishing is
    // immutable; remove a package explicitly before republishing its version.
    [[nodiscard]] bool Publish(const std::filesystem::path& package_file, std::string* error = nullptr) const;

    // Lists valid packages in deterministic name and version order. Invalid or
    // damaged files are skipped and optionally reported as warnings.
    [[nodiscard]] std::vector<RegistryPackage> List(std::vector<std::string>* warnings = nullptr) const;

    // Finds one exact name/version pair. No remote lookup or version
    // substitution occurs.
    [[nodiscard]] bool Find(const std::string& name, const std::string& version, RegistryPackage& out,
                            std::string* error = nullptr) const;

    // Installs one exact registry entry through the same staged package
    // installer used for direct .apkg installs.
    [[nodiscard]] bool Install(const std::string& name, const std::string& version,
                               const std::filesystem::path& plugins_dir, bool replace = false,
                               std::string* error = nullptr) const;

private:
    std::filesystem::path root_;
};

} // namespace aether::plugin
