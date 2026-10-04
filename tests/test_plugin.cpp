#include "aether/cook/cooker.h"
#include "aether/core/version.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/plugin/plugin.h"
#include "aether/project/project.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/gameplay.h"
#include "test_framework.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Phase 26 step 1: plugin descriptors, discovery, resolution, modules, and
// plugins through the cook into the player.

using namespace aether;
using namespace aether::plugin;
namespace stdfs = std::filesystem;
using nlohmann::json;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

stdfs::path Dir(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_plugin_tests" / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    return dir;
}

PluginDescriptor Desc(const std::string& name, std::vector<PluginDependency> deps = {}, std::vector<ModuleDesc> modules = {}) {
    PluginDescriptor d;
    d.name = name;
    d.dependencies = std::move(deps);
    d.modules = std::move(modules);
    return d;
}

void Install(const stdfs::path& root, const PluginDescriptor& d) {
    AETHER_CHECK(SavePluginDescriptor(root / d.name / (d.name + kPluginExtension), d));
}

bool Has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

std::vector<std::string> EnabledNames(const PluginManager& m) {
    std::vector<std::string> names;
    for (const PluginInfo* p : m.Enabled()) names.push_back(p->descriptor.name);
    return names;
}

// Test modules that record what happens to them.
std::vector<std::string>* g_events = nullptr;
class RecordingModule : public IModule {
public:
    explicit RecordingModule(std::string name) : name_(std::move(name)) {}
    void Startup(const ModuleContext& c) override {
        if (g_events) g_events->push_back("start " + name_ + (c.plugin ? " of " + c.plugin->name : ""));
    }
    void Shutdown() override {
        if (g_events) g_events->push_back("stop " + name_);
    }

private:
    std::string name_;
};

void RegisterRecording(const std::string& name) {
    ModuleRegistry::Get().Register(name, [name] { return std::make_unique<RecordingModule>(name); });
}

} // namespace

AETHER_TEST(Plugin_DescriptorsAndVersions) {
    const stdfs::path dir = Dir("Descriptors");
    PluginDescriptor d = Desc("Coins", {{"Physics", "1.2", false}}, {{"Coins", ModuleType::Runtime, LoadingPhase::PostDefault},
                                                                       {"CoinsEditor", ModuleType::Editor, LoadingPhase::Default}});
    d.version = "2.1.0";
    d.description = "Collectable coins";
    d.has_content = true;
    std::string error;
    CHECK(SavePluginDescriptor(dir / "Coins.aplugin", d, &error));
    PluginDescriptor back;
    CHECK(LoadPluginDescriptor(dir / "Coins.aplugin", back, &error));
    CHECK(back.name == "Coins" && back.friendly_name == "Coins" && back.version == "2.1.0" && back.has_content);
    CHECK(back.dependencies.size() == 1 && back.dependencies[0].min_version == "1.2");
    CHECK(back.modules.size() == 2 && back.modules[1].type == ModuleType::Editor && back.modules[0].phase == LoadingPhase::PostDefault);

    std::ofstream(dir / "bad.aplugin") << R"({"$type": "Scene"})";
    CHECK(!LoadPluginDescriptor(dir / "bad.aplugin", back, &error) && error.find("isn't a plugin") != std::string::npos);
    PluginDescriptor bad = Desc("9lives");
    CHECK(SavePluginDescriptor(dir / "x.aplugin", bad));
    CHECK(!LoadPluginDescriptor(dir / "x.aplugin", back, &error) && error.find("valid plugin name") != std::string::npos);

    CHECK(IsValidPluginName("My_Plugin2") && !IsValidPluginName("") && !IsValidPluginName("a-b") && !IsValidPluginName("_x"));
    CHECK(CompareVersions("1.10.0", "1.9") > 0 && CompareVersions("1.0", "1.0.0") == 0 && CompareVersions("0.7", "0.12") < 0);

    PluginDescriptor made;
    stdfs::path file;
    CHECK(CreatePluginScaffold(dir, "Weather", &file, &error) && LoadPluginDescriptor(file, made));
    CHECK(made.has_content && made.modules.size() == 1 && made.modules[0].name == "Weather" && stdfs::is_directory(dir / "Weather/Content"));
    CHECK(!CreatePluginScaffold(dir, "Weather", nullptr, &error) && error.find("already exists") != std::string::npos);
    CHECK(!CreatePluginScaffold(dir, "no way", nullptr, &error));
}

AETHER_TEST(Plugin_DiscoveryAndResolution) {
    const stdfs::path engine = Dir("Resolve/Engine"), project = Dir("Resolve/Project");
    PluginDescriptor core = Desc("Core");
    core.enabled_by_default = true;
    Install(engine, core);
    Install(engine, Desc("Water", {{"Core", "", false}}));
    PluginDescriptor engine_boats = Desc("Boats", {{"Water", "", false}});
    engine_boats.version = "1.0";
    Install(engine, engine_boats);
    PluginDescriptor project_boats = Desc("Boats", {{"Water", "", false}, {"Sails", "", true}});
    project_boats.version = "2.0";
    Install(project, project_boats); // replaces the engine's
    // A misnamed one and an unreadable one are skipped with warnings.
    stdfs::create_directories(project / "Wrong");
    CHECK(SavePluginDescriptor(project / "Wrong/Wrong.aplugin", Desc("Other")));
    stdfs::create_directories(project / "Broken");
    std::ofstream(project / "Broken/Broken.aplugin") << "{";

    PluginManager m;
    m.AddSearchPath(engine, PluginSource::Engine);
    m.AddSearchPath(project, PluginSource::Project);
    const std::vector<std::string> warnings = m.Discover();
    CHECK(warnings.size() == 2 && m.Plugins().size() == 3);
    CHECK(m.Find("Boats")->descriptor.version == "2.0" && m.Find("Boats")->source == PluginSource::Project);

    std::string error;
    std::vector<std::string> notes;
    CHECK(m.Resolve({"Boats"}, &error, &notes));
    CHECK(EnabledNames(m) == (std::vector<std::string>{"Core", "Water", "Boats"})); // dependencies first
    CHECK(m.Find("Water")->enabled_reason == "needed by Boats" && m.Find("Core")->enabled_reason == "default");
    CHECK(notes.size() == 1 && notes[0].find("Sails") != std::string::npos); // the optional one
    CHECK(m.Resolve({}, &error) && EnabledNames(m) == std::vector<std::string>{"Core"});
    CHECK(!m.Resolve({"Rafts"}, &error) && error.find("isn't installed") != std::string::npos);

    // A too-old dependency, a newer engine, and a cycle.
    const stdfs::path more = Dir("Resolve/More");
    Install(more, Desc("Old"));
    Install(more, Desc("NeedsNew", {{"Old", "3.0", false}}));
    PluginDescriptor future = Desc("Future");
    future.engine_version = "99.0";
    Install(more, future);
    Install(more, Desc("A", {{"B", "", false}}));
    Install(more, Desc("B", {{"C", "", false}}));
    Install(more, Desc("C", {{"A", "", false}}));
    PluginManager n;
    n.AddSearchPath(more, PluginSource::Project);
    n.Discover();
    CHECK(!n.Resolve({"NeedsNew"}, &error) && error.find("3.0 or newer; 1.0.0 is installed") != std::string::npos);
    CHECK(!n.Resolve({"Future"}, &error) && error.find("needs engine 99.0") != std::string::npos);
    CHECK(!n.Resolve({"A"}, &error) && error.find("cycle: A -> B -> C -> A") != std::string::npos);
    CHECK(n.Enabled().empty());
}

AETHER_TEST(Plugin_ModulesStartInOrderAndStop) {
    const stdfs::path root = Dir("Modules");
    Install(root, Desc("Base", {}, {{"TBase", ModuleType::Runtime, LoadingPhase::Default}}));
    Install(root, Desc("Early", {}, {{"TEarly", ModuleType::Runtime, LoadingPhase::PreDefault}}));
    Install(root, Desc("Game", {{"Base", "", false}, {"Early", "", false}},
                       {{"TGame", ModuleType::Runtime, LoadingPhase::Default},
                        {"TGameEditor", ModuleType::Editor, LoadingPhase::PostDefault},
                        {"TMissing", ModuleType::Runtime, LoadingPhase::Default}}));
    for (const char* name : {"TBase", "TEarly", "TGame", "TGameEditor"}) RegisterRecording(name);
    CHECK(!ModuleRegistry::Get().Register("TBase", [] { return std::make_unique<RecordingModule>("x"); }));

    std::vector<std::string> events;
    g_events = &events;
    {
        PluginManager m;
        m.AddSearchPath(root, PluginSource::Project);
        m.Discover();
        CHECK(m.Resolve({"Game"}));
        CHECK(m.ModuleOrder(true, false) == (std::vector<std::string>{"TEarly", "TBase", "TGame", "TMissing"}));
        CHECK(m.ModuleOrder(true, true).back() == "TGameEditor");
        const std::vector<std::string> warnings = m.StartModules(true, false); // a game: no editor modules
        CHECK(warnings.size() == 1 && warnings[0].find("TMissing") != std::string::npos);
        CHECK(events == (std::vector<std::string>{"start TEarly of Early", "start TBase of Base", "start TGame of Game"}));
        CHECK(m.StartModules(true, true).size() == 1 && events.back() == "start TGameEditor of Game"); // only the new one
        events.clear();
    } // the manager shuts its modules down, last first
    CHECK(events == (std::vector<std::string>{"stop TGameEditor", "stop TGame", "stop TBase", "stop TEarly"}));
    g_events = nullptr;
    for (const char* name : {"TBase", "TEarly", "TGame", "TGameEditor"}) ModuleRegistry::Get().UnregisterForTesting(name);
}

AETHER_TEST(Plugin_TheEnginesModulesArePlugins) {
    const stdfs::path dir = PluginManager::EnginePluginsDir();
    CHECK(!dir.empty());
    PluginManager m;
    m.AddSearchPath(dir, PluginSource::Engine);
    CHECK(m.Discover().empty());
    for (const char* name : {"Physics", "Audio", "Navigation", "AI", "Networking"}) {
        CHECK(m.Find(name) != nullptr && m.Find(name)->descriptor.enabled_by_default);
    }
    CHECK(m.Resolve({}));
    const std::vector<std::string> on = EnabledNames(m);
    CHECK(on.size() == 5);
    CHECK(std::find(on.begin(), on.end(), "Navigation") < std::find(on.begin(), on.end(), "AI"));
    // This executable has the modules its build has (physics is optional).
    for (const char* name : {"Audio", "Navigation", "AI", "Networking"}) CHECK(ModuleRegistry::Get().Has(name));
    const std::vector<std::string> warnings = m.StartModules(true, false);
    CHECK(warnings.size() == (ModuleRegistry::Get().Has("Physics") ? 0u : 1u));
}

AETHER_TEST(Plugin_ThroughTheCookIntoThePlayer) {
    const stdfs::path parent = Dir("Cooked");
    ProjectPaths paths;
    std::string error;
    CHECK(CreateProject(parent, "Coinland", &paths, &error));
    Tags tags;
    tags.names = {"Start"};
    const json scene = {{"$type", "Scene"}, {"$version", 1},
                        {"entities", json::array({{{"components", {{"Tags", reflect::ToJson(tags)}}}}})}};
    stdfs::create_directories(paths.content / "Scenes");
    std::ofstream(paths.content / "Scenes/main.ascene") << scene.dump();
    // A project plugin with a module and content.
    stdfs::path file;
    CHECK(CreatePluginScaffold(paths.root / "Plugins", "Coins", &file, &error));
    PluginDescriptor coins;
    CHECK(LoadPluginDescriptor(file, coins));
    coins.dependencies.push_back({"Audio", "1.0", false});
    CHECK(SavePluginDescriptor(file, coins));
    std::ofstream(paths.root / "Plugins/Coins/Content/coin.txt") << "shiny";
    ProjectSettings settings;
    CHECK(LoadProject(paths.file, settings, &error));
    settings.startup_scene = "Scenes/main.ascene";
    settings.plugins = {"Coins"};
    CHECK(SaveProject(paths.file, settings, &error));

    cook::CookOptions options;
    options.project_file = paths.file;
    options.output_dir = parent / "Paks";
    const cook::CookReport report = cook::Cook(options);
    CHECK(report.ok && Has(report.plugins, "Coins") && Has(report.plugins, "Audio"));
    CHECK(report.modules.back() == "Coins"); // a Default module after the engine's PreDefault ones

    // A plugin the project needs, uninstalled: the cook says so.
    settings.plugins = {"Coins", "Missing"};
    CHECK(SaveProject(paths.file, settings, &error));
    const cook::CookReport broken = cook::Cook(options);
    CHECK(!broken.ok && broken.error.find("'Missing'") != std::string::npos);

    std::vector<std::string> events;
    g_events = &events;
    RegisterRecording("Coins");
    {
        player::GamePackage package;
        CHECK(package.Mount(report.pak_file.string()) && package.LoadManifest(&error));
        CHECK(package.Manifest().modules == report.modules && Has(package.Manifest().plugins, "Coins"));
        std::string text;
        CHECK(package.Files().ReadText("Content/Plugins/Coins/coin.txt", text) && text == "shiny");
        player::Game game(package);
        CHECK(game.LoadStartupScene(&error));
        CHECK(Has(game.StartedModules(), "Coins") && Has(game.StartedModules(), "Audio"));
        CHECK(events == std::vector<std::string>{"start Coins"});
    }
    CHECK(events.back() == "stop Coins");
    g_events = nullptr;
    ModuleRegistry::Get().UnregisterForTesting("Coins");
}
