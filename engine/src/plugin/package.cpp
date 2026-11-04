#include "aether/plugin/package.h"

#include "aether/pak/pak.h"
#include "aether/platform/filesystem.h"

#include <algorithm>
#include <system_error>

namespace aether::plugin {

namespace {

namespace stdfs = std::filesystem;

bool Fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

// Reads the descriptor entry `<Name>/<Name>.aplugin` of an opened package. The package's one top folder names it.
bool ReadDescriptor(const pak::PakReader& reader, PluginDescriptor& out, std::string* error) {
    std::string top;
    for (const pak::PakEntry& e : reader.Entries()) {
        const usize slash = e.path.find('/');
        if (slash == std::string::npos || slash == 0) return Fail(error, "entry \"" + e.path + "\" is not inside a plugin folder");
        const std::string first = e.path.substr(0, slash);
        if (top.empty()) top = first;
        else if (top != first) return Fail(error, "the package holds more than one folder (\"" + top + "\" and \"" + first + "\")");
        if (e.path.find("..") != std::string::npos || pak::NormalizePath(e.path) != e.path)
            return Fail(error, "entry \"" + e.path + "\" has an unsafe path");
    }
    if (top.empty()) return Fail(error, "the package is empty");
    if (!IsValidPluginName(top)) return Fail(error, "\"" + top + "\" is not a valid plugin name");
    const std::string descriptor_path = top + "/" + top + kPluginExtension;
    std::vector<u8> bytes;
    std::string read_error;
    if (!reader.Read(descriptor_path, bytes, &read_error)) return Fail(error, "no descriptor " + descriptor_path + ": " + read_error);
    if (!LoadPluginDescriptorFromMemory(bytes, descriptor_path, out, error)) return false;
    if (out.name != top) return Fail(error, "the descriptor names \"" + out.name + "\" but the folder is \"" + top + "\"");
    if (out.abi != 0 && out.abi != kPluginAbi)
        return Fail(error, "built for plugin ABI " + std::to_string(out.abi) + ", this engine is ABI " + std::to_string(kPluginAbi));
    return true;
}

} // namespace

bool PackagePlugin(const stdfs::path& plugin_dir, const stdfs::path& package_file, std::string* error) {
    std::error_code ec;
    if (!stdfs::is_directory(plugin_dir, ec)) return Fail(error, plugin_dir.string() + " is not a folder");
    const std::string name = plugin_dir.filename().string().empty() ? plugin_dir.parent_path().filename().string()
                                                                    : plugin_dir.filename().string();
    if (!IsValidPluginName(name)) return Fail(error, "\"" + name + "\" is not a valid plugin name");
    PluginDescriptor descriptor;
    const stdfs::path descriptor_file = plugin_dir / (name + kPluginExtension);
    if (!LoadPluginDescriptor(descriptor_file, descriptor, error)) return false;
    if (descriptor.name != name) return Fail(error, "the descriptor names \"" + descriptor.name + "\" but the folder is \"" + name + "\"");
    if (descriptor.abi != 0 && descriptor.abi != kPluginAbi)
        return Fail(error, "built for plugin ABI " + std::to_string(descriptor.abi) + ", this engine is ABI " + std::to_string(kPluginAbi));

    pak::PakWriter writer(pak::CompressionPolicy::Auto);
    usize count = 0;
    for (stdfs::recursive_directory_iterator it(plugin_dir, stdfs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) return Fail(error, "couldn't read " + plugin_dir.string() + ": " + ec.message());
        if (it->is_symlink(ec)) return Fail(error, it->path().string() + " is a symbolic link; packages hold plain files only");
        if (!it->is_regular_file(ec)) continue;
        const std::string relative = stdfs::relative(it->path(), plugin_dir, ec).generic_string();
        if (!writer.AddFile(name + "/" + relative, it->path().string())) return Fail(error, "couldn't read " + it->path().string());
        if (++count > kMaxPackageEntries) return Fail(error, "more than " + std::to_string(kMaxPackageEntries) + " files");
    }
    if (writer.OriginalBytes() > kMaxPackageBytes) return Fail(error, "the plugin is larger than the package limit");
    return writer.Write(package_file.string(), error);
}

bool InspectPluginPackage(const stdfs::path& package_file, PluginDescriptor& out, std::string* error) {
    pak::PakReader reader;
    if (!reader.Open(package_file.string(), error)) return false;
    return ReadDescriptor(reader, out, error);
}

std::vector<std::string> UnmetDependencies(const PluginDescriptor& descriptor, const std::vector<PluginInfo>& installed) {
    std::vector<std::string> unmet;
    for (const PluginDependency& dep : descriptor.dependencies) {
        if (dep.optional) continue;
        const auto it = std::find_if(installed.begin(), installed.end(), [&](const PluginInfo& p) { return p.descriptor.name == dep.name; });
        const std::string wanted = dep.min_version.empty() ? dep.name : dep.name + " >= " + dep.min_version;
        if (it == installed.end()) unmet.push_back(wanted + " (not installed)");
        else if (!dep.min_version.empty() && CompareVersions(it->descriptor.version, dep.min_version) < 0)
            unmet.push_back(wanted + " (found " + it->descriptor.version + ")");
    }
    return unmet;
}

bool InstallPluginPackage(const stdfs::path& package_file, const stdfs::path& plugins_dir, bool replace, std::string* error) {
    pak::PakReader reader;
    if (!reader.Open(package_file.string(), error)) return false;
    PluginDescriptor descriptor;
    if (!ReadDescriptor(reader, descriptor, error)) return false;
    if (reader.Entries().size() > kMaxPackageEntries) return Fail(error, "the package holds too many files");
    u64 total = 0;
    for (const pak::PakEntry& e : reader.Entries()) total += e.size;
    if (total > kMaxPackageBytes) return Fail(error, "the package unpacks to more than the limit");

    std::error_code ec;
    stdfs::create_directories(plugins_dir, ec);
    const stdfs::path target = plugins_dir / descriptor.name;
    const bool exists = stdfs::exists(target, ec);
    if (exists && !replace) return Fail(error, target.string() + " already exists");

    // Unpack to a staging folder beside the target, checking every entry's CRC as it is read.
    const stdfs::path staging = plugins_dir / ("." + descriptor.name + ".installing");
    stdfs::remove_all(staging, ec);
    const auto abort = [&](const std::string& message) {
        stdfs::remove_all(staging, ec);
        return Fail(error, message);
    };
    for (const pak::PakEntry& e : reader.Entries()) {
        std::vector<u8> bytes;
        std::string read_error;
        if (!reader.Read(e.path, bytes, &read_error)) return abort("\"" + e.path + "\" is damaged: " + read_error);
        const stdfs::path file = staging / e.path.substr(descriptor.name.size() + 1);
        stdfs::create_directories(file.parent_path(), ec);
        if (ec) return abort("couldn't create " + file.parent_path().string());
        if (!fs::WriteFileBytes(file.string(), bytes.data(), bytes.size())) return abort("couldn't write " + file.string());
    }

    // Swap in. An existing install moves aside first and comes back if the rename fails.
    stdfs::path aside;
    if (exists) {
        aside = plugins_dir / ("." + descriptor.name + ".previous");
        stdfs::remove_all(aside, ec);
        stdfs::rename(target, aside, ec);
        if (ec) return abort("couldn't replace " + target.string() + ": " + ec.message());
    }
    stdfs::rename(staging, target, ec); // the files were staged without the <Name>/ prefix
    if (ec) {
        const std::string message = ec.message();
        if (!aside.empty()) stdfs::rename(aside, target, ec);
        return abort("couldn't finish installing " + descriptor.name + ": " + message);
    }
    if (!aside.empty()) stdfs::remove_all(aside, ec);
    return true;
}

} // namespace aether::plugin
