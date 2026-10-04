#include "aether/plugin/plugin.h"

#include "aether/core/log.h"
#include "aether/core/version.h"
#include "aether/platform/filesystem.h"
#include "aether/project/project.h"
#include "aether/reflection/serialize.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

namespace aether::plugin {

namespace stdfs = std::filesystem;

namespace {

bool Fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

std::vector<long> VersionParts(const std::string& v) {
    std::vector<long> parts;
    std::stringstream in(v);
    for (std::string part; std::getline(in, part, '.');) parts.push_back(std::strtol(part.c_str(), nullptr, 10));
    return parts;
}

} // namespace

bool IsValidPluginName(const std::string& name) {
    if (name.empty() || !std::isalpha(static_cast<unsigned char>(name[0]))) return false;
    return std::all_of(name.begin(), name.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

int CompareVersions(const std::string& a, const std::string& b) {
    const std::vector<long> x = VersionParts(a), y = VersionParts(b);
    for (usize i = 0; i < std::max(x.size(), y.size()); ++i) {
        const long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

bool LoadPluginDescriptor(const stdfs::path& file, PluginDescriptor& out, std::string* error) {
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(file.string(), bytes)) return Fail(error, "Can't read " + file.string());
    const reflect::Json json = reflect::Json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded() || !json.is_object() || json.value("$type", "") != "Plugin") {
        return Fail(error, file.string() + " isn't a plugin descriptor");
    }
    PluginDescriptor d;
    if (!reflect::FromJson(d, json)) return Fail(error, file.string() + " can't be read as a plugin descriptor");
    if (!IsValidPluginName(d.name)) return Fail(error, file.string() + ": '" + d.name + "' isn't a valid plugin name");
    for (const ModuleDesc& m : d.modules) {
        if (!IsValidPluginName(m.name)) return Fail(error, file.string() + ": '" + m.name + "' isn't a valid module name");
    }
    if (d.friendly_name.empty()) d.friendly_name = d.name;
    out = std::move(d);
    return true;
}

bool SavePluginDescriptor(const stdfs::path& file, const PluginDescriptor& descriptor, std::string* error) {
    reflect::Json json = reflect::ToJson(descriptor);
    json["$type"] = "Plugin";
    std::string text = json.dump(2);
    text.push_back('\n');
    std::error_code ec;
    if (file.has_parent_path()) stdfs::create_directories(file.parent_path(), ec);
    if (!fs::WriteFileBytes(file.string(), text.data(), text.size())) return Fail(error, "Can't write " + file.string());
    return true;
}

bool CreatePluginScaffold(const stdfs::path& parent, const std::string& name, stdfs::path* descriptor_file,
                          std::string* error) {
    if (!IsValidPluginName(name)) return Fail(error, "'" + name + "' isn't a valid plugin name (a letter, then letters, digits and _)");
    const stdfs::path dir = parent / name;
    std::error_code ec;
    if (stdfs::exists(dir, ec)) return Fail(error, dir.string() + " already exists");
    PluginDescriptor d;
    d.name = name;
    d.friendly_name = name;
    d.description = "";
    d.engine_version = kEngineVersion;
    d.has_content = true;
    d.modules.push_back({name, ModuleType::Runtime, LoadingPhase::Default});
    const stdfs::path file = dir / (name + kPluginExtension);
    if (!SavePluginDescriptor(file, d, error)) return false;
    stdfs::create_directories(dir / "Content", ec);
    if (descriptor_file) *descriptor_file = file;
    return true;
}

bool ResolveProjectPlugins(const stdfs::path& project_file, PluginManager& out, std::string* error,
                           std::vector<std::string>* warnings) {
    ProjectSettings settings;
    if (!LoadProject(project_file, settings, error)) return false;
    out = PluginManager();
    if (const stdfs::path engine = PluginManager::EnginePluginsDir(); !engine.empty()) {
        out.AddSearchPath(engine, PluginSource::Engine);
    }
    out.AddSearchPath(ProjectPaths::ForFile(project_file).root / "Plugins", PluginSource::Project);
    for (std::string& w : out.Discover()) {
        if (warnings) warnings->push_back(std::move(w));
    }
    return out.Resolve(settings.plugins, error, warnings);
}

// --- Modules -----------------------------------------------------------------

ModuleRegistry& ModuleRegistry::Get() {
    static ModuleRegistry registry;
    return registry;
}

bool ModuleRegistry::Register(const std::string& name, Factory factory) {
    if (Has(name) || !factory) return false;
    factories_.push_back({name, std::move(factory)});
    return true;
}

bool ModuleRegistry::Has(const std::string& name) const {
    return std::any_of(factories_.begin(), factories_.end(), [&](const auto& f) { return f.first == name; });
}

std::unique_ptr<IModule> ModuleRegistry::Create(const std::string& name) const {
    for (const auto& [n, factory] : factories_) {
        if (n == name) return factory();
    }
    return nullptr;
}

std::vector<std::string> ModuleRegistry::Names() const {
    std::vector<std::string> names;
    for (const auto& f : factories_) names.push_back(f.first);
    std::sort(names.begin(), names.end());
    return names;
}

void ModuleRegistry::UnregisterForTesting(const std::string& name) {
    factories_.erase(std::remove_if(factories_.begin(), factories_.end(), [&](const auto& f) { return f.first == name; }),
                     factories_.end());
}

// --- The manager ---------------------------------------------------------------

stdfs::path PluginManager::EnginePluginsDir() {
#ifdef AETHER_ENGINE_PLUGINS_DIR
    std::error_code ec;
    const stdfs::path dir = AETHER_ENGINE_PLUGINS_DIR;
    if (stdfs::is_directory(dir, ec)) return dir;
#endif
    return {};
}

PluginManager& PluginManager::operator=(PluginManager&& other) noexcept {
    if (this != &other) {
        ShutdownModules();
        search_paths_ = std::move(other.search_paths_);
        plugins_ = std::move(other.plugins_);
        order_ = std::move(other.order_);
        started_ = std::move(other.started_);
    }
    return *this;
}

void PluginManager::AddSearchPath(const stdfs::path& dir, PluginSource source) {
    search_paths_.push_back({dir, source});
}

std::vector<std::string> PluginManager::Discover() {
    std::vector<std::string> warnings;
    plugins_.clear();
    order_.clear();
    for (const auto& [dir, source] : search_paths_) {
        std::error_code ec;
        if (!stdfs::is_directory(dir, ec)) continue;
        std::vector<stdfs::path> folders;
        for (stdfs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec)) folders.push_back(it->path());
        }
        std::sort(folders.begin(), folders.end());
        for (const stdfs::path& folder : folders) {
            const stdfs::path file = folder / (folder.filename().string() + kPluginExtension);
            if (!stdfs::is_regular_file(file, ec)) continue;
            PluginInfo info;
            std::string error;
            if (!LoadPluginDescriptor(file, info.descriptor, &error)) {
                warnings.push_back(error + "; skipped");
                continue;
            }
            if (info.descriptor.name != folder.filename().string()) {
                warnings.push_back(file.string() + " names the plugin '" + info.descriptor.name +
                                   "', but its folder is '" + folder.filename().string() + "'; skipped");
                continue;
            }
            info.dir = folder;
            info.file = file;
            info.source = source;
            const auto existing = std::find_if(plugins_.begin(), plugins_.end(), [&](const PluginInfo& p) {
                return p.descriptor.name == info.descriptor.name;
            });
            if (existing == plugins_.end()) {
                plugins_.push_back(std::move(info));
            } else if (source == PluginSource::Project && existing->source == PluginSource::Engine) {
                *existing = std::move(info); // the project's copy wins
            } else {
                warnings.push_back("Two plugins are named '" + info.descriptor.name + "' (" + existing->file.string() +
                                   ", " + file.string() + "); the first is used");
            }
        }
    }
    std::sort(plugins_.begin(), plugins_.end(),
              [](const PluginInfo& a, const PluginInfo& b) { return a.descriptor.name < b.descriptor.name; });
    return warnings;
}

const PluginInfo* PluginManager::Find(const std::string& name) const {
    for (const PluginInfo& p : plugins_) {
        if (p.descriptor.name == name) return &p;
    }
    return nullptr;
}

bool PluginManager::Resolve(const std::vector<std::string>& requested, std::string* error,
                            std::vector<std::string>* warnings) {
    std::map<std::string, usize> index;
    for (usize i = 0; i < plugins_.size(); ++i) {
        index[plugins_[i].descriptor.name] = i;
        plugins_[i].enabled = false;
        plugins_[i].enabled_reason.clear();
    }
    order_.clear();
    const auto warn = [&](std::string w) {
        if (warnings) warnings->push_back(std::move(w));
    };

    // What's wanted, and why.
    std::vector<std::pair<usize, std::string>> queue;
    for (const std::string& name : requested) {
        const auto it = index.find(name);
        if (it == index.end()) return Fail(error, "The project enables the plugin '" + name + "', which isn't installed");
        queue.push_back({it->second, "project"});
    }
    for (usize i = 0; i < plugins_.size(); ++i) {
        if (plugins_[i].descriptor.enabled_by_default) queue.push_back({i, "default"});
    }
    // Enable them and what they need.
    for (usize q = 0; q < queue.size(); ++q) {
        PluginInfo& p = plugins_[queue[q].first];
        if (p.enabled) continue;
        const PluginDescriptor& d = p.descriptor;
        if (!d.engine_version.empty() && CompareVersions(d.engine_version, kEngineVersion) > 0) {
            return Fail(error, "The plugin '" + d.name + "' needs engine " + d.engine_version + " or newer (this is " +
                                   kEngineVersion + ")");
        }
        p.enabled = true;
        p.enabled_reason = queue[q].second;
        for (const PluginDependency& dep : d.dependencies) {
            const auto it = index.find(dep.name);
            if (it == index.end()) {
                if (dep.optional) {
                    warn("'" + d.name + "' can use the plugin '" + dep.name + "', which isn't installed");
                    continue;
                }
                return Fail(error, "'" + d.name + "' needs the plugin '" + dep.name + "', which isn't installed");
            }
            const PluginDescriptor& have = plugins_[it->second].descriptor;
            if (!dep.min_version.empty() && CompareVersions(have.version, dep.min_version) < 0) {
                const std::string message = "'" + d.name + "' needs '" + dep.name + "' " + dep.min_version +
                                            " or newer; " + have.version + " is installed";
                if (dep.optional) {
                    warn(message);
                    continue;
                }
                return Fail(error, message);
            }
            queue.push_back({it->second, "needed by " + d.name});
        }
    }

    // Dependencies first (depth-first, by name for a stable order).
    std::vector<int> state(plugins_.size(), 0); // 0 unvisited, 1 on the path, 2 done
    std::vector<usize> path;
    std::string cycle;
    std::function<bool(usize)> visit = [&](usize i) {
        if (state[i] == 2) return true;
        if (state[i] == 1) {
            // The cycle is the path from this plugin's first visit, back to it.
            const auto from = std::find(path.begin(), path.end(), i);
            for (auto it = from; it != path.end(); ++it) cycle += plugins_[*it].descriptor.name + " -> ";
            cycle += plugins_[i].descriptor.name;
            return false;
        }
        state[i] = 1;
        path.push_back(i);
        for (const PluginDependency& dep : plugins_[i].descriptor.dependencies) {
            const auto it = index.find(dep.name);
            if (it == index.end() || !plugins_[it->second].enabled) continue;
            if (!visit(it->second)) return false;
        }
        path.pop_back();
        state[i] = 2;
        order_.push_back(i);
        return true;
    };
    for (usize i = 0; i < plugins_.size(); ++i) {
        if (plugins_[i].enabled && !visit(i)) {
            order_.clear();
            for (PluginInfo& p : plugins_) p.enabled = false;
            return Fail(error, "The plugins depend on each other in a cycle: " + cycle);
        }
    }
    return true;
}

std::vector<const PluginInfo*> PluginManager::Enabled() const {
    std::vector<const PluginInfo*> out;
    for (usize i : order_) out.push_back(&plugins_[i]);
    return out;
}

std::vector<std::string> PluginManager::ModuleOrder(bool runtime, bool editor) const {
    std::vector<std::string> names;
    for (LoadingPhase phase : {LoadingPhase::PreDefault, LoadingPhase::Default, LoadingPhase::PostDefault}) {
        for (usize i : order_) {
            for (const ModuleDesc& m : plugins_[i].descriptor.modules) {
                const bool wanted = m.type == ModuleType::Runtime ? runtime : editor;
                if (wanted && m.phase == phase) names.push_back(m.name);
            }
        }
    }
    return names;
}

std::vector<std::string> PluginManager::StartModules(bool runtime, bool editor) {
    std::vector<std::string> warnings;
    for (LoadingPhase phase : {LoadingPhase::PreDefault, LoadingPhase::Default, LoadingPhase::PostDefault}) {
        for (usize i : order_) {
            const PluginInfo& p = plugins_[i];
            for (const ModuleDesc& m : p.descriptor.modules) {
                const bool wanted = m.type == ModuleType::Runtime ? runtime : editor;
                if (!wanted || m.phase != phase) continue;
                if (std::any_of(started_.begin(), started_.end(), [&](const Started& s) { return s.name == m.name; })) continue;
                std::unique_ptr<IModule> module = ModuleRegistry::Get().Create(m.name);
                if (!module) {
                    warnings.push_back("The module '" + m.name + "' (plugin '" + p.descriptor.name +
                                       "') isn't built into this executable");
                    continue;
                }
                ModuleContext context;
                context.plugin = &p.descriptor;
                context.plugin_dir = p.dir;
                module->Startup(context);
                started_.push_back({m.name, std::move(module)});
            }
        }
    }
    for (const std::string& w : warnings) AETHER_LOG_WARN("Plugins", "%s", w.c_str());
    return warnings;
}

void PluginManager::ShutdownModules() {
    while (!started_.empty()) {
        started_.back().module->Shutdown();
        started_.pop_back();
    }
}

std::vector<std::string> PluginManager::StartedModules() const {
    std::vector<std::string> names;
    for (const Started& s : started_) names.push_back(s.name);
    return names;
}

std::vector<std::pair<stdfs::path, std::string>> PluginManager::ContentMounts() const {
    std::vector<std::pair<stdfs::path, std::string>> mounts;
    for (usize i : order_) {
        const PluginInfo& p = plugins_[i];
        std::error_code ec;
        if (p.descriptor.has_content && stdfs::is_directory(p.dir / "Content", ec)) {
            mounts.push_back({p.dir / "Content", "Plugins/" + p.descriptor.name + "/"});
        }
    }
    return mounts;
}

} // namespace aether::plugin
