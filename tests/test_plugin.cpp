#include "aether/cook/cooker.h"
#include "aether/core/version.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/plugin/package.h"
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
    AETHER_CHECK(ModuleRegistry::Get().Register(name, [name] { return std::make_unique<RecordingModule>(name); }));
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

AETHER_TEST(Plugin_AbiMustMatchTheEngine) {
    const stdfs::path dir = Dir("Abi");
    PluginDescriptor current = Desc("Current");
    current.abi = kPluginAbi;
    PluginDescriptor legacy = Desc("Legacy"); // abi 0: written before the field existed
    PluginDescriptor future = Desc("Future");
    future.abi = kPluginAbi + 1;
    Install(dir, current);
    Install(dir, legacy);
    Install(dir, future);

    std::string error;
    PluginDescriptor back;
    CHECK(LoadPluginDescriptor(dir / "Future/Future.aplugin", back, &error) && back.abi == kPluginAbi + 1);

    PluginManager ok;
    ok.AddSearchPath(dir, PluginSource::Project);
    ok.Discover();
    CHECK(ok.Resolve({"Current", "Legacy"}, &error));

    PluginManager bad;
    bad.AddSearchPath(dir, PluginSource::Project);
    bad.Discover();
    CHECK(!bad.Resolve({"Future"}, &error) && error.find("plugin ABI") != std::string::npos);

    // A new scaffold carries the current ABI.
    stdfs::path file;
    CHECK(CreatePluginScaffold(dir, "Fresh", &file, &error));
    CHECK(LoadPluginDescriptor(file, back, &error) && back.abi == kPluginAbi);
}

AETHER_TEST(Plugin_DescriptorLoaderRefusesMalformedInputWithoutThrowing) {
    const auto load = [](const std::string& text, std::string& error) {
        PluginDescriptor d;
        return LoadPluginDescriptorFromMemory(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()), "test.aplugin", d, &error);
    };
    std::string error;
    CHECK(load(R"({"$type":"Plugin","name":"Coins","modules":[{"name":"Coins"}]})", error));
    CHECK(!load(R"({"$type":5})", error) && error.find("isn't a plugin descriptor") != std::string::npos); // used to throw
    CHECK(!load(R"({"$type":"Plugin","name":["a"]})", error));
    (void)load(R"({"$type":"Plugin","name":"Coins","modules":"no","abi":"one"})", error); // a wrong-typed field keeps its default; it must not throw
    CHECK(!load(R"({"$type":"Plugin","name":"../escape"})", error) && error.find("valid plugin name") != std::string::npos);
    CHECK(!load(R"({"$type":"Plugin","name":"Coins","modules":[{"name":"a/b"}]})", error) && error.find("valid module name") != std::string::npos);
    CHECK(!load("[]", error));
    CHECK(!load("", error));
    CHECK(!load(std::string(kMaxPluginDescriptorBytes + 1, ' '), error) && error.find("too large") != std::string::npos);
}

AETHER_TEST(Plugin_PackageRoundTripAndInstall) {
    const stdfs::path dir = Dir("Package");
    PluginDescriptor d = Desc("Coins", {{"Physics", "1.2", false}, {"Fancy", "", true}}, {{"Coins", ModuleType::Runtime, LoadingPhase::Default}});
    d.has_content = true;
    d.version = "2.0.0";
    Install(dir / "src", d);
    stdfs::create_directories(dir / "src/Coins/Content/Textures");
    std::ofstream(dir / "src/Coins/Content/Textures/coin.txt") << "shiny";
    std::string error;
    CHECK(PackagePlugin(dir / "src/Coins", dir / "Coins.apkg", &error));

    PluginDescriptor seen;
    CHECK(InspectPluginPackage(dir / "Coins.apkg", seen, &error) && seen.name == "Coins" && seen.version == "2.0.0");

    // Dependencies: Physics 1.2+ is required, Fancy is optional.
    PluginInfo physics;
    physics.descriptor = Desc("Physics");
    physics.descriptor.version = "1.0.0";
    CHECK(UnmetDependencies(seen, {}).size() == 1);
    CHECK(UnmetDependencies(seen, {physics}).size() == 1 && UnmetDependencies(seen, {physics})[0].find("found 1.0.0") != std::string::npos);
    physics.descriptor.version = "1.3.0";
    CHECK(UnmetDependencies(seen, {physics}).empty());

    // Install, refuse a second install, replace.
    CHECK(InstallPluginPackage(dir / "Coins.apkg", dir / "Plugins", false, &error));
    CHECK(stdfs::exists(dir / "Plugins/Coins/Coins.aplugin") && stdfs::exists(dir / "Plugins/Coins/Content/Textures/coin.txt"));
    CHECK(!InstallPluginPackage(dir / "Coins.apkg", dir / "Plugins", false, &error) && error.find("already exists") != std::string::npos);
    CHECK(InstallPluginPackage(dir / "Coins.apkg", dir / "Plugins", true, &error));
    CHECK(!stdfs::exists(dir / "Plugins/.Coins.previous") && !stdfs::exists(dir / "Plugins/.Coins.installing"));

    // The installed folder is discovered like any other.
    PluginManager manager;
    manager.AddSearchPath(dir / "Plugins", PluginSource::Project);
    manager.Discover();
    CHECK(manager.Find("Coins") != nullptr);
}

AETHER_TEST(Plugin_PackageRefusesHostileArchives) {
    const stdfs::path dir = Dir("PackageHostile");
    std::string error;
    PluginDescriptor d = Desc("Good");
    PluginDescriptor out;

    // A descriptor for another name than the folder.
    {
        pak::PakWriter w;
        CHECK(SavePluginDescriptor(dir / "tmp.aplugin", Desc("Other")));
        CHECK(w.AddFile("Good/Good.aplugin", (dir / "tmp.aplugin").string()));
        CHECK(w.Write((dir / "mismatch.apkg").string()));
        CHECK(!InspectPluginPackage(dir / "mismatch.apkg", out, &error) && error.find("folder") != std::string::npos);
    }
    // Two top folders.
    {
        pak::PakWriter w;
        CHECK(SavePluginDescriptor(dir / "g.aplugin", d));
        CHECK(w.AddFile("Good/Good.aplugin", (dir / "g.aplugin").string()));
        CHECK(w.Add("Evil/payload.txt", std::string("x")));
        CHECK(w.Write((dir / "two.apkg").string()));
        CHECK(!InspectPluginPackage(dir / "two.apkg", out, &error));
        CHECK(!InstallPluginPackage(dir / "two.apkg", dir / "Plugins", false, &error));
        CHECK(!stdfs::exists(dir / "Plugins/Good") && !stdfs::exists(dir / "Plugins/Evil"));
    }
    // A traversal path is not even a legal archive path.
    {
        pak::PakWriter w;
        CHECK(!w.Add("Good/../../escape.txt", std::string("x")));
    }
    // A plugin for another ABI.
    {
        PluginDescriptor future = Desc("Future");
        future.abi = kPluginAbi + 1;
        Install(dir / "src", future);
        CHECK(!PackagePlugin(dir / "src/Future", dir / "Future.apkg", &error) && error.find("ABI") != std::string::npos);
    }
    // Not an archive at all.
    std::ofstream(dir / "junk.apkg") << "not a package";
    CHECK(!InspectPluginPackage(dir / "junk.apkg", out, &error));
}
