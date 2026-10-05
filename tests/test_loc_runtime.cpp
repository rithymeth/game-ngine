#include "aether/assets/asset_guid.h"
#include "aether/cook/cooker.h"
#include "aether/loc/localization.h"
#include "aether/loc/localize.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/project/project.h"
#include "aether/reflection/registry.h"
#include "test_framework.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/loc_api.h"
#include "aether/script/luau_host.h"
#endif

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

// Phase 29 step 2 (§29.2): .astrings as a cooked asset type, the Localization
// service in the player and its link to the language setting, and the
// Blueprint and Luau library.

using namespace aether;
using namespace aether::player;
using nlohmann::json;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

stdfs::path TestDir(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_loc_runtime_tests" / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    return dir;
}

// A cooked game with the given string tables (path, CSV text).
stdfs::path MakeGame(const stdfs::path& dir, const std::vector<std::pair<std::string, std::string>>& tables) {
    const json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", json::array()}};
    json assets = json::array({{{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Scenes/start.ascene"}, {"importer", "Scene"}}});
    for (const auto& [path, text] : tables) {
        assets.push_back({{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", "StringTable"}});
    }
    const json manifest = {{"$type", "CookManifest"}, {"$version", 1},        {"project", "Demo"}, {"configuration", "Development"},
                           {"startup_scene", "Scenes/start.ascene"},         {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                           {"layers", {"Default"}}, {"collision_matrix", json::array()}, {"assets", assets}, {"files", json::array()}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump(2));
    writer.Add("Content/Scenes/start.ascene", scene.dump(2));
    for (const auto& [path, text] : tables) writer.Add("Content/" + path, text);
    const stdfs::path file = dir / "Game.apak";
    std::string error;
    writer.Write(file.string(), &error);
    return file;
}

struct Loaded {
    GamePackage package;
    explicit Loaded(const stdfs::path& pak) {
        std::string error;
        package.Mount(pak.string(), 0, &error);
        package.LoadManifest(&error);
    }
};

} // namespace

AETHER_TEST(LocRuntime_AstringsIsAnAssetTypeAndAlwaysCooks) {
    const stdfs::path dir = TestDir("cook");
    ProjectPaths paths;
    std::string error;
    CHECK(CreateProject(dir, "Words", &paths, &error));
    stdfs::create_directories(paths.content / "Text");
    stdfs::create_directories(paths.content / "Scenes");
    std::ofstream(paths.content / "Text/ui.astrings", std::ios::binary) << "key,en,fr\nplay,Play,Jouer\n";
    std::ofstream(paths.content / "Scenes/main.ascene", std::ios::binary) << R"({"$type":"Scene","$version":1,"entities":[]})";
    ProjectSettings settings;
    CHECK(LoadProject(paths.file, settings, &error));
    settings.startup_scene = "Scenes/main.ascene"; // nothing names the table, and it isn't in always_cook
    CHECK(SaveProject(paths.file, settings, &error));
    cook::CookOptions options;
    options.project_file = paths.file;
    options.output_dir = dir / "Build";
    options.configuration = cook::BuildConfiguration::Shipping;
    CHECK(cook::Cook(options).ok);
    GamePackage package;
    for (const std::string& pak : GamePackage::FindPaks(dir / "Build")) CHECK(package.Mount(pak));
    CHECK(package.LoadManifest(&error));
    const GameManifest::Asset* asset = package.FindAsset("Text/ui.astrings");
    CHECK(asset != nullptr && asset->importer == "StringTable");
    Game game(package);
    CHECK(game.LoadStartupScene(&error));
    CHECK(game.Localization().Text("play") == "Play");
    game.Localization().SetLanguage("fr");
    CHECK(game.Localization().Text("play") == "Jouer");
}

AETHER_TEST(LocRuntime_PlayerMergesTablesAndWarnsOnBadOnes) {
    const stdfs::path dir = TestDir("merge");
    Loaded g(MakeGame(dir, {{"Text/a.astrings", "key,en,fr\nplay,Play,Jouer\nquit,Quit,Quitter\n"},
                            {"Text/b.astrings", "key,en\nplay,Play now\n"},
                            {"Text/c.astrings", "this is not a table\n"}}));
    Game game(g.package);
    std::string error;
    CHECK(game.LoadStartupScene(&error));
    loc::Localization& l = game.Localization();
    CHECK(l.Text("play") == "Play now");  // a later file wins for the same key and language
    CHECK(*l.Table().Find("fr", "play") == "Jouer");
    CHECK(l.Text("quit") == "Quit");
    bool warned = false;
    for (const std::string& w : game.Warnings()) warned = warned || w.find("c.astrings") != std::string::npos;
    CHECK(warned); // the bad table is a warning, the rest still loaded
}

AETHER_TEST(LocRuntime_LanguageFollowsTheSettingsBothWays) {
    const stdfs::path dir = TestDir("language");
    Loaded g(MakeGame(dir, {{"Text/a.astrings", "key,en,pt-BR,pt\nplay,Play,Jogar BR,Jogar\nonly_en,Only,,\n"}}));
    Game game(g.package);
    game.SetUserPaths(ResolveUserPaths("Demo", dir / "user"));
    std::string error;
    CHECK(game.LoadStartupScene(&error));
    int changes = 0;
    std::string last;
    game.Localization().AddListener([&](const std::string& now, const std::string&) {
        ++changes;
        last = now;
    });
    CHECK(game.Localization().Language() == "en");
    // The setting changes the language...
    save::GameSettings s = game.Settings().Get();
    s.language = "pt-BR";
    game.Settings().Set(s);
    CHECK(game.Localization().Language() == "pt-BR" && changes == 1 && last == "pt-BR");
    CHECK(game.Localization().Text("play") == "Jogar BR");
    game.Settings().Set(s); // the same again: nothing
    CHECK(changes == 1);
    // ...and a script's choice changes the setting.
    CHECK(loc::Localization::Active() == &game.Localization());
    loc::Localize::SetLanguage("pt");
    CHECK(changes == 2 && game.Settings().Get().language == "pt");
    CHECK(loc::Localize::GetText("play", "?") == "Jogar" && loc::Localize::GetLanguage() == "pt");
    // The language falls back to the default, then the default text, then the key.
    loc::Localize::SetLanguage("de");
    CHECK(loc::Localize::GetText("play", "?") == "Play");
    CHECK(loc::Localize::GetText("nope", "Fallback text") == "Fallback text" && loc::Localize::GetText("nope", "") == "nope");
}

AETHER_TEST(LocRuntime_SavedLanguageLoadsWithTheSettings) {
    const stdfs::path dir = TestDir("saved");
    Loaded g(MakeGame(dir, {{"Text/a.astrings", "key,en,fr\nplay,Play,Jouer\n"}}));
    const UserPaths paths = ResolveUserPaths("Demo", dir / "user");
    {
        Game game(g.package);
        game.SetUserPaths(paths);
        save::GameSettings s;
        s.language = "fr";
        game.Settings().Set(s);
        CHECK(game.Settings().Save().ok);
    }
    Game game(g.package);
    game.SetUserPaths(paths);
    std::string error;
    CHECK(game.LoadStartupScene(&error));
    game.LoadSettings({});
    CHECK(game.Localization().Language() == "fr" && game.Localization().Text("play") == "Jouer");
}

AETHER_TEST(LocRuntime_LibraryWithoutAServiceReturnsTheFallback) {
    CHECK(loc::Localization::Active() == nullptr);
    CHECK(loc::Localize::GetText("k", "fallback") == "fallback" && loc::Localize::GetText("k", "") == "k");
    CHECK(loc::Localize::FormatInt("k", "{count} coin{count|s}", "count", 2) == "2 coins");
    CHECK(loc::Localize::FormatString("k", "Hi {who}", "who", "Ada") == "Hi Ada");
    CHECK(!loc::Localize::HasText("k") && loc::Localize::GetLanguage() == "en");
    loc::Localize::SetLanguage("fr"); // nothing to change, and no crash
}

AETHER_TEST(LocRuntime_LibraryFormatsWithThePluralRulesOfTheLanguage) {
    loc::Localization l;
    l.MakeActive();
    l.AddFromCsv("key,en,pt-BR\ncoins,{count} coin{count|s},{count|one:moeda;other:moedas}\n");
    CHECK(loc::Localize::FormatInt("coins", "", "count", 1) == "1 coin");
    CHECK(loc::Localize::FormatInt("coins", "", "count", 3) == "3 coins");
    l.SetLanguage("pt-BR");
    CHECK(loc::Localize::FormatInt("coins", "", "count", 0) == "moeda"); // pt-BR: 0 is one
    CHECK(loc::Localize::HasText("coins"));
}

AETHER_TEST(LocRuntime_LibraryIsReflectedForBlueprints) {
    const reflect::TypeInfo* type = reflect::TypeRegistry::Find("Localize");
    CHECK(type != nullptr && type->functions.size() == 6);
    for (const char* fn : {"GetText", "FormatInt", "FormatString", "HasText", "SetLanguage", "GetLanguage"}) {
        bool found = false;
        for (const reflect::FunctionInfo& f : type->functions) found = found || std::string(f.name) == fn;
        CHECK(found);
    }
}

#if AETHER_TEST_HAS_SCRIPTING
AETHER_TEST(LocRuntime_LuauTable) {
    loc::Localization l;
    l.MakeActive();
    l.AddFromCsv("key,en,fr\nplay,Play,Jouer\ncoins,{count} coin{count|s},{count} pièce{count|s}\n");
    script::LuauHost host;
    script::InstallLocApi(host);
    script::ScriptResult r = host.Run(R"(
        local en = Localization.GetText('play', '?')
        Localization.SetLanguage('fr')
        local fr = Localization.GetText('play', '?')
        return en, fr, Localization.GetLanguage(), Localization.Format('coins', '', 'count', 3), Localization.Format('coins', '', 'count', 1),
               Localization.HasText('play'), Localization.HasText('zzz'), Localization.GetText('zzz', 'dflt'), Localization.Format('x', 'Hi {n}', 'n', 'Ada')
    )", "loc");
    CHECK(r.ok && r.values.size() == 9);
    if (r.ok && r.values.size() == 9) {
        const auto str = [](const script::ScriptValue& v) { return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string("<not a string>"); };
        CHECK(str(r.values[0]) == "Play" && str(r.values[1]) == "Jouer" && str(r.values[2]) == "fr");
        CHECK(str(r.values[3]) == "3 pièces" && str(r.values[4]) == "1 pièce");
        CHECK(std::holds_alternative<bool>(r.values[5]) && std::get<bool>(r.values[5]) && !std::get<bool>(r.values[6]));
        CHECK(str(r.values[7]) == "dflt" && str(r.values[8]) == "Hi Ada");
    }
    CHECK(!host.Run("return Localization.GetText(5)", "bad").ok); // a wrong-typed argument is a script error
}
#endif

// ---- Phase 29 step 6: localized assets ----

#include "aether/loc/localized_path.h"

AETHER_TEST(LocAssets_CandidatesFollowTheFallbackChain) {
    using loc::LocalizedCandidates;
    CHECK((LocalizedCandidates("Textures/logo.png", "pt-BR") == std::vector<std::string>{"Textures/logo.pt-BR.png", "Textures/logo.pt.png", "Textures/logo.png"}));
    CHECK((LocalizedCandidates("Textures/logo.png", "fr") == std::vector<std::string>{"Textures/logo.fr.png", "Textures/logo.png"}));
    CHECK((LocalizedCandidates("Textures/logo.png", "en") == std::vector<std::string>{"Textures/logo.png"})); // the base is the default language
    CHECK((LocalizedCandidates("logo.png", "de", "fr") == std::vector<std::string>{"logo.de.png", "logo.png"}));
    CHECK((LocalizedCandidates("Audio/voice.line.ogg", "fr") == std::vector<std::string>{"Audio/voice.line.fr.ogg", "Audio/voice.line.ogg"}));
    CHECK((LocalizedCandidates("README", "fr") == std::vector<std::string>{"README.fr", "README"})); // no extension
    CHECK((LocalizedCandidates("a.dir/readme", "fr") == std::vector<std::string>{"a.dir/readme.fr", "a.dir/readme"}));
}

AETHER_TEST(LocAssets_VariantsAreRecognizedAndOrphansFound) {
    std::string base, language;
    CHECK(loc::SplitVariant("Textures/logo.fr.png", &base, &language) && base == "Textures/logo.png" && language == "fr");
    CHECK(loc::SplitVariant("logo.pt-BR.png", &base, &language) && base == "logo.png" && language == "pt-BR");
    CHECK(loc::SplitVariant("logo.zh-Hans-CN.png", &base, &language) && language == "zh-Hans-CN");
    CHECK(!loc::SplitVariant("Textures/logo.png") && !loc::SplitVariant("icon.large.png") && !loc::SplitVariant("v1.2.png") &&
          !loc::SplitVariant("a.dir/readme") && !loc::SplitVariant(".fr.png") && !loc::SplitVariant("Textures.fr/logo.png"));
    const std::vector<std::string> paths{"logo.png", "logo.fr.png", "menu.fr.png", "Scenes/start.ascene"};
    CHECK((loc::OrphanedVariants(paths) == std::vector<std::string>{"menu.fr.png"}));
}

AETHER_TEST(LocAssets_PackageResolvesTheVariantAndFallsBack) {
    const stdfs::path dir = TestDir("assets");
    const json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", json::array()}};
    json assets = json::array({{{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Scenes/start.ascene"}, {"importer", "Scene"}}});
    for (const char* p : {"Textures/logo.png", "Textures/logo.fr.png", "Textures/logo.pt.png", "Textures/orphan.de.png"}) {
        assets.push_back({{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", p}, {"importer", "Texture"}});
    }
    const json manifest = {{"$type", "CookManifest"}, {"$version", 1},        {"project", "Demo"}, {"configuration", "Development"},
                           {"startup_scene", "Scenes/start.ascene"},         {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                           {"layers", {"Default"}}, {"collision_matrix", json::array()}, {"assets", assets}, {"files", json::array()}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump(2));
    writer.Add("Content/Scenes/start.ascene", scene.dump(2));
    writer.Add("Content/Textures/logo.png", "base");
    writer.Add("Content/Textures/logo.fr.png", "french");
    writer.Add("Content/Textures/logo.pt.png", "portuguese");
    writer.Add("Content/Textures/orphan.de.png", "orphan");
    const stdfs::path file = dir / "Game.apak";
    std::string error;
    CHECK(writer.Write(file.string(), &error));
    Loaded g(file);
    CHECK(g.package.LocalizedPath("Textures/logo.png", "fr") == "Textures/logo.fr.png");
    CHECK(g.package.LocalizedPath("Textures/logo.png", "pt-BR") == "Textures/logo.pt.png"); // pt-BR has none: pt does
    CHECK(g.package.LocalizedPath("Textures/logo.png", "de") == "Textures/logo.png");       // no German: the base
    CHECK(g.package.LocalizedPath("Textures/other.png", "fr") == "Textures/other.png");     // not an asset at all: unchanged
    std::vector<u8> bytes;
    CHECK(g.package.ReadContentLocalized("Textures/logo.png", "fr", bytes, &error) && std::string(bytes.begin(), bytes.end()) == "french");
    CHECK(g.package.ReadContentLocalized("Textures/logo.png", "ja", bytes, &error) && std::string(bytes.begin(), bytes.end()) == "base");
    // The game follows its current language, and warns about the orphan.
    Game game(g.package);
    CHECK(game.LoadStartupScene(&error));
    CHECK(game.LocalizedAsset("Textures/logo.png") == "Textures/logo.png");
    game.Localization().SetLanguage("fr");
    CHECK(game.LocalizedAsset("Textures/logo.png") == "Textures/logo.fr.png");
    bool warned = false;
    for (const std::string& w : game.Warnings()) warned = warned || w.find("orphan.de.png") != std::string::npos;
    CHECK(warned);
}
