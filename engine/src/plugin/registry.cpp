#include "aether/plugin/registry.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace aether::plugin {

namespace {

namespace stdfs = std::filesystem;

bool Fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool IsValidRegistryVersion(const std::string& version) {
    if (version.empty()) return false;
    bool has_digit = false;
    bool previous_dot = true;
    for (const unsigned char c : version) {
        if (std::isdigit(c)) {
            has_digit = true;
            previous_dot = false;
        } else if (c == '.' && !previous_dot) {
            previous_dot = true;
        } else {
            return false;
        }
    }
    return has_digit && !previous_dot;
}

bool IsSymlink(const stdfs::path& path, std::error_code& ec) {
    const auto status = stdfs::symlink_status(path, ec);
    return !ec && stdfs::is_symlink(status);
}

void Warn(std::vector<std::string>* warnings, std::string message) {
    if (warnings) warnings->push_back(std::move(message));
}

} // namespace

bool LocalPluginRegistry::Publish(const stdfs::path& package_file, std::string* error) const {
    if (root_.empty()) return Fail(error, "the local plugin registry has no root directory");
    PluginDescriptor descriptor;
    if (!InspectPluginPackage(package_file, descriptor, error)) return false;
    if (!IsValidRegistryVersion(descriptor.version))
        return Fail(error, "plugin version '" + descriptor.version + "' is not numeric dotted form");

    std::error_code ec;
    if (!stdfs::is_regular_file(package_file, ec) || ec)
        return Fail(error, package_file.string() + " is not a regular package file");

    const stdfs::path plugin_dir = root_ / descriptor.name;
    if (IsSymlink(plugin_dir, ec)) return Fail(error, plugin_dir.string() + " is a symbolic link");
    if (ec) return Fail(error, "couldn't inspect " + plugin_dir.string() + ": " + ec.message());
    stdfs::create_directories(plugin_dir, ec);
    if (ec) return Fail(error, "couldn't create " + plugin_dir.string() + ": " + ec.message());

    const stdfs::path destination = plugin_dir / (descriptor.version + kPackageExtension);
    const auto destination_status = stdfs::symlink_status(destination, ec);
    if (!ec && stdfs::exists(destination_status))
        return Fail(error, descriptor.name + " " + descriptor.version + " is already published");
    if (ec && ec != std::errc::no_such_file_or_directory)
        return Fail(error, "couldn't inspect " + destination.string() + ": " + ec.message());

    ec.clear();
    if (!stdfs::copy_file(package_file, destination, stdfs::copy_options::none, ec))
        return Fail(error, "couldn't publish " + descriptor.name + " " + descriptor.version + ": " + ec.message());

    PluginDescriptor copied;
    if (!InspectPluginPackage(destination, copied, error)) {
        std::error_code cleanup_ec;
        stdfs::remove(destination, cleanup_ec);
        return false;
    }
    if (copied.name != descriptor.name || copied.version != descriptor.version) {
        std::error_code cleanup_ec;
        stdfs::remove(destination, cleanup_ec);
        return Fail(error, "the published package metadata changed while it was copied");
    }
    return true;
}

std::vector<RegistryPackage> LocalPluginRegistry::List(std::vector<std::string>* warnings) const {
    std::vector<RegistryPackage> packages;
    if (root_.empty()) {
        Warn(warnings, "the local plugin registry has no root directory");
        return packages;
    }
    std::error_code ec;
    if (!stdfs::exists(root_, ec) || ec) return packages;
    if (!stdfs::is_directory(root_, ec) || ec) {
        Warn(warnings, root_.string() + " is not a registry directory");
        return packages;
    }

    for (stdfs::directory_iterator plugin_it(root_, ec), end; !ec && plugin_it != end; plugin_it.increment(ec)) {
        const stdfs::path plugin_dir = plugin_it->path();
        if (!IsValidPluginName(plugin_dir.filename().string())) continue;
        if (IsSymlink(plugin_dir, ec)) {
            Warn(warnings, plugin_dir.string() + " is a symbolic link; skipped");
            ec.clear();
            continue;
        }
        if (ec) break;
        if (!stdfs::is_directory(plugin_dir, ec) || ec) {
            ec.clear();
            continue;
        }

        for (stdfs::directory_iterator package_it(plugin_dir, ec), package_end; !ec && package_it != package_end;
             package_it.increment(ec)) {
            const stdfs::path file = package_it->path();
            const std::string filename = file.filename().string();
            if (!filename.ends_with(kPackageExtension)) continue;
            const std::string version = filename.substr(0, filename.size() - std::char_traits<char>::length(kPackageExtension));
            if (!IsValidRegistryVersion(version)) {
                Warn(warnings, file.string() + " has an invalid version filename; skipped");
                continue;
            }
            if (IsSymlink(file, ec)) {
                Warn(warnings, file.string() + " is a symbolic link; skipped");
                ec.clear();
                continue;
            }
            if (ec) break;
            if (!stdfs::is_regular_file(file, ec) || ec) {
                ec.clear();
                continue;
            }
            PluginDescriptor descriptor;
            std::string package_error;
            if (!InspectPluginPackage(file, descriptor, &package_error) || descriptor.name != plugin_dir.filename().string() ||
                descriptor.version != version) {
                Warn(warnings, file.string() + " is invalid: " + (package_error.empty() ? "descriptor does not match its registry path" : package_error));
                continue;
            }
            packages.push_back({std::move(descriptor), file});
        }
        if (ec) {
            Warn(warnings, "couldn't read " + plugin_dir.string() + ": " + ec.message());
            ec.clear();
        }
    }
    if (ec) Warn(warnings, "couldn't read " + root_.string() + ": " + ec.message());

    std::sort(packages.begin(), packages.end(), [](const RegistryPackage& a, const RegistryPackage& b) {
        if (a.descriptor.name != b.descriptor.name) return a.descriptor.name < b.descriptor.name;
        const int version_order = CompareVersions(a.descriptor.version, b.descriptor.version);
        return version_order == 0 ? a.descriptor.version < b.descriptor.version : version_order < 0;
    });
    return packages;
}

bool LocalPluginRegistry::Find(const std::string& name, const std::string& version, RegistryPackage& out,
                               std::string* error) const {
    if (root_.empty()) return Fail(error, "the local plugin registry has no root directory");
    if (!IsValidPluginName(name)) return Fail(error, "'" + name + "' is not a valid plugin name");
    if (!IsValidRegistryVersion(version)) return Fail(error, "'" + version + "' is not a valid numeric dotted version");

    const stdfs::path plugin_dir = root_ / name;
    const stdfs::path file = plugin_dir / (version + kPackageExtension);
    std::error_code ec;
    const bool plugin_dir_is_symlink = IsSymlink(plugin_dir, ec);
    if (ec) return Fail(error, "couldn't inspect " + plugin_dir.string() + ": " + ec.message());
    ec.clear();
    const bool package_is_symlink = IsSymlink(file, ec);
    if (ec) return Fail(error, "couldn't inspect " + file.string() + ": " + ec.message());
    if (plugin_dir_is_symlink || package_is_symlink)
        return Fail(error, "registry entry " + file.string() + " is a symbolic link");
    if (!stdfs::is_regular_file(file, ec) || ec)
        return Fail(error, name + " " + version + " is not in the local registry");

    PluginDescriptor descriptor;
    if (!InspectPluginPackage(file, descriptor, error)) return false;
    if (descriptor.name != name || descriptor.version != version)
        return Fail(error, "registry path does not match the package descriptor");
    out = {std::move(descriptor), file};
    return true;
}

bool LocalPluginRegistry::Install(const std::string& name, const std::string& version, const stdfs::path& plugins_dir,
                                 bool replace, std::string* error) const {
    RegistryPackage package;
    if (!Find(name, version, package, error)) return false;
    return InstallPluginPackage(package.package_file, plugins_dir, replace, error);
}

} // namespace aether::plugin
