#pragma once

#include "aether/core/base.h"
#include "aether/reflection/reflection.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Plugins (Phase 26 step 1, docs/design/PHASE_SPECS.md §26.1). A plugin is
// a folder with a `<Name>.aplugin` descriptor (JSON), optional modules
// (code: a runtime and/or an editor part) and optional content
// (`Content/`). The engine's own optional modules (physics, audio, AI,
// networking) are plugins too, in the engine's plugins/ folder; a
// project's own live in its Plugins/ folder.
//
// Modules are compiled into the executables that use them and register a
// factory under their name with AETHER_MODULE (linked in like platform
// plugins, by aether_link_modules() in cmake/AetherModules.cmake). The
// PluginManager decides which plugins are enabled, checks their
// dependencies and the engine version, and starts their modules in order.

namespace aether::plugin {

enum class ModuleType : u8 { Runtime, Editor };
// When a module starts, relative to the others: PreDefault ones first (e.g.
// what registers components the rest use), then Default, then PostDefault.
enum class LoadingPhase : u8 { PreDefault, Default, PostDefault };

struct ModuleDesc {
    std::string name;
    ModuleType type = ModuleType::Runtime;
    LoadingPhase phase = LoadingPhase::Default;
};

struct PluginDependency {
    std::string name;
    std::string min_version; // "" for any
    bool optional = false;   // used when present; no error when it's not
};

struct PluginDescriptor {
    std::string name;            // the folder's and file's name, an identifier
    std::string friendly_name;
    std::string version = "1.0.0";
    std::string description;
    std::string category = "Other";
    std::string created_by;
    std::string engine_version;  // the oldest engine it works with; "" for any
    bool enabled_by_default = false;
    bool has_content = false;    // it has a Content/ folder of assets
    std::vector<PluginDependency> dependencies;
    std::vector<ModuleDesc> modules;
};

inline constexpr const char* kPluginExtension = ".aplugin";

bool LoadPluginDescriptor(const std::filesystem::path& file, PluginDescriptor& out, std::string* error = nullptr);
bool SavePluginDescriptor(const std::filesystem::path& file, const PluginDescriptor& descriptor,
                          std::string* error = nullptr);
// Valid names are identifiers: a letter, then letters, digits and '_'.
bool IsValidPluginName(const std::string& name);
// Compares dotted versions ("1.10.0" > "1.9"); missing parts count as 0.
int CompareVersions(const std::string& a, const std::string& b);

// A new plugin: <parent>/<name>/<name>.aplugin (one runtime module named
// after it, not enabled by default) and an empty Content/ folder. False if
// the name isn't valid or the folder exists.
bool CreatePluginScaffold(const std::filesystem::path& parent, const std::string& name,
                          std::filesystem::path* descriptor_file = nullptr, std::string* error = nullptr);

// The plugins a project gets: the engine's (EnginePluginsDir) and its own
// (<project>/Plugins), discovered and resolved against its `plugins` list.
class PluginManager;
bool ResolveProjectPlugins(const std::filesystem::path& project_file, PluginManager& out, std::string* error = nullptr,
                           std::vector<std::string>* warnings = nullptr);

// --- Modules -----------------------------------------------------------------

struct ModuleContext {
    const PluginDescriptor* plugin = nullptr;
    std::filesystem::path plugin_dir;
};

class IModule {
public:
    virtual ~IModule() = default;
    virtual void Startup(const ModuleContext& context) { (void)context; }
    virtual void Shutdown() {}
};

class ModuleRegistry {
public:
    using Factory = std::function<std::unique_ptr<IModule>()>;
    static ModuleRegistry& Get();
    // False (and nothing registered) when the name is taken.
    bool Register(const std::string& name, Factory factory);
    bool Has(const std::string& name) const;
    std::unique_ptr<IModule> Create(const std::string& name) const;
    std::vector<std::string> Names() const;
    void UnregisterForTesting(const std::string& name);

private:
    std::vector<std::pair<std::string, Factory>> factories_;
};

// --- The manager ---------------------------------------------------------------

enum class PluginSource : u8 { Engine, Project };

struct PluginInfo {
    PluginDescriptor descriptor;
    std::filesystem::path dir;   // the plugin's folder
    std::filesystem::path file;  // its .aplugin
    PluginSource source = PluginSource::Engine;
    bool enabled = false;        // after Resolve
    std::string enabled_reason;  // "project", "default", "needed by X"
};

class PluginManager {
public:
    PluginManager() = default;
    ~PluginManager() { ShutdownModules(); }
    PluginManager(PluginManager&&) noexcept = default;
    // Shuts this one's started modules down first.
    PluginManager& operator=(PluginManager&& other) noexcept;
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // Folders whose subfolders are plugins (<dir>/<Name>/<Name>.aplugin).
    void AddSearchPath(const std::filesystem::path& dir, PluginSource source);
    // Finds the plugins; a project plugin of the same name replaces an
    // engine one. Problems (unreadable descriptors, misnamed files) are
    // warnings, and those plugins are skipped.
    std::vector<std::string> Discover();

    const std::vector<PluginInfo>& Plugins() const { return plugins_; }
    const PluginInfo* Find(const std::string& name) const;

    // Enables the plugins in `requested` (the project's list), those enabled
    // by default, and everything they need, then orders them so each comes
    // after its dependencies. False with `error` for a missing or too-old
    // required dependency, a plugin that needs a newer engine, an unknown
    // requested plugin, or a dependency cycle. Warnings go to `warnings`.
    bool Resolve(const std::vector<std::string>& requested, std::string* error = nullptr,
                 std::vector<std::string>* warnings = nullptr);
    // Enabled plugins in dependency order (after Resolve).
    std::vector<const PluginInfo*> Enabled() const;

    // The enabled plugins' modules of these types, in start order: by
    // loading phase, then plugin dependency order.
    std::vector<std::string> ModuleOrder(bool runtime, bool editor) const;
    // Creates and starts them. Modules this executable wasn't built with
    // are skipped with a warning (the plugin's content still mounts).
    std::vector<std::string> StartModules(bool runtime, bool editor);
    // Shuts the started modules down in reverse order.
    void ShutdownModules();
    std::vector<std::string> StartedModules() const;

    // Each enabled plugin with content: its Content/ folder and where it
    // mounts in the virtual file system ("Plugins/<Name>/").
    std::vector<std::pair<std::filesystem::path, std::string>> ContentMounts() const;

    // The executable's bundled plugins/ folder, then the development source
    // tree's plugins/ folder as a fallback, or empty if neither exists.
    static std::filesystem::path EnginePluginsDir();

private:
    struct Started {
        std::string name;
        std::unique_ptr<IModule> module;
    };
    std::vector<std::pair<std::filesystem::path, PluginSource>> search_paths_;
    std::vector<PluginInfo> plugins_;
    std::vector<usize> order_; // enabled plugins, dependencies first
    std::vector<Started> started_;
};

namespace detail {
struct ModuleRegistrar {
    ModuleRegistrar(const char* name, ModuleRegistry::Factory factory) {
        ModuleRegistry::Get().Register(name, std::move(factory));
    }
};
} // namespace detail

} // namespace aether::plugin

// Registers a module class (derived from IModule) under `name`:
//   class PhysicsModule : public aether::plugin::IModule { ... };
//   AETHER_MODULE(Physics, PhysicsModule);
#define AETHER_MODULE(name, Class)                                                                  \
    static const ::aether::plugin::detail::ModuleRegistrar aether_module_registrar_##name(          \
        #name, [] { return std::unique_ptr<::aether::plugin::IModule>(new Class()); })

AETHER_ENUM(aether::plugin::ModuleType, 1, AETHER_ENUM_VALUE(Runtime), AETHER_ENUM_VALUE(Editor))
AETHER_ENUM(aether::plugin::LoadingPhase, 1, AETHER_ENUM_VALUE(PreDefault), AETHER_ENUM_VALUE(Default),
            AETHER_ENUM_VALUE(PostDefault))
AETHER_REFLECT(aether::plugin::ModuleDesc, 1,
    AETHER_FIELD(name, Field_EditAnywhere),
    AETHER_FIELD(type, Field_EditAnywhere),
    AETHER_FIELD(phase, Field_EditAnywhere)
)
AETHER_REFLECT(aether::plugin::PluginDependency, 1,
    AETHER_FIELD(name, Field_EditAnywhere),
    AETHER_FIELD(min_version, Field_EditAnywhere),
    AETHER_FIELD(optional, Field_EditAnywhere)
)
AETHER_REFLECT(aether::plugin::PluginDescriptor, 1,
    AETHER_FIELD(name, Field_EditAnywhere),
    AETHER_FIELD(friendly_name, Field_EditAnywhere),
    AETHER_FIELD(version, Field_EditAnywhere),
    AETHER_FIELD(description, Field_EditAnywhere),
    AETHER_FIELD(category, Field_EditAnywhere),
    AETHER_FIELD(created_by, Field_EditAnywhere),
    AETHER_FIELD(engine_version, Field_EditAnywhere),
    AETHER_FIELD(enabled_by_default, Field_EditAnywhere),
    AETHER_FIELD(has_content, Field_EditAnywhere),
    AETHER_FIELD(dependencies, Field_EditAnywhere),
    AETHER_FIELD(modules, Field_EditAnywhere)
)
