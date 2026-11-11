#include "aether/assets/environment.h"
#include "aether/assets/image.h"
#include "aether/cook/cooker.h"
#include "aether/core/log.h"
#include "aether/cook/texture_cook.h"
#include "aether/pak/pak.h"
#include "aether/project/project.h"
#include "aether/reflection/reflection.h"
#include "test_framework.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Phase 25 step 2: the cooker - what it cooks (reachable from the startup
// scene and always_cook), what it strips (editor-only fields), and what
// the archive and manifest hold.

namespace cook_test {
struct Notes {
    std::string author;
    std::string todo;
    aether::f32 weight = 1.0f;
};
struct CookTestComponent {
    std::string texture;  // an asset GUID
    std::string editor_comment;
    Notes notes;
    std::vector<Notes> history;
};
} // namespace cook_test

AETHER_REFLECT(cook_test::Notes, 1,
    AETHER_FIELD(author, aether::reflect::Field_EditorOnly),
    AETHER_FIELD(todo, aether::reflect::Field_EditorOnly),
    AETHER_FIELD(weight, aether::reflect::Field_EditAnywhere)
)
AETHER_REFLECT(cook_test::CookTestComponent, 1,
    AETHER_FIELD(texture, aether::reflect::Field_EditAnywhere),
    AETHER_FIELD(editor_comment, aether::reflect::Field_EditorOnly),
    AETHER_FIELD(notes, aether::reflect::Field_EditAnywhere),
    AETHER_FIELD(history, aether::reflect::Field_EditAnywhere)
)

using namespace aether;
using nlohmann::json;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

void WriteText(const stdfs::path& file, const std::string& text) {
    stdfs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string GuidOf(const stdfs::path& content, const std::string& path) {
    assets::AssetDatabase db(content);
    db.Scan();
    const assets::AssetRecord* r = db.FindByPath(path);
    return r ? assets::ToString(r->guid) : std::string();
}

// The component's reflected name; using it also registers the type, as
// registering a component does in a game.
std::string ComponentName() {
    return reflect::Reflect<cook_test::CookTestComponent>().name;
}

json Component(const std::string& texture) {
    const json notes = {{"$v", 1}, {"author", "Grace"}, {"todo", "tune"}, {"weight", 2.5}};
    return {{"$v", 1}, {"texture", texture}, {"editor_comment", "remember the lighting"},
            {"notes", notes}, {"history", json::array({notes, notes})}};
}

// A project: a startup scene that uses texture A and a prefab, the prefab
// using texture B, a model (kept by always_cook) with its .bin, and an
// unused texture.
stdfs::path MakeProject(const std::string& name) {
    const stdfs::path parent = stdfs::temp_directory_path() / "aether_cook_tests";
    stdfs::remove_all(parent / name);
    stdfs::create_directories(parent);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(parent, name, &paths, &error));
    const stdfs::path assets_dir = AETHER_REPO_ASSETS_DIR;
    stdfs::create_directories(paths.content / "Textures");
    stdfs::copy_file(assets_dir / "textures/checker_a.png", paths.content / "Textures/a.png");
    stdfs::copy_file(assets_dir / "textures/checker_b.png", paths.content / "Textures/b.png");
    stdfs::copy_file(assets_dir / "textures/test_corners.png", paths.content / "Textures/unused.png");
    stdfs::create_directories(paths.content / "Models");
    stdfs::copy_file(assets_dir / "models/test_cube.bin", paths.content / "Models/test_cube.bin");
    // The model points at Textures/a.png (as ../Textures/a.png).
    std::ifstream in(assets_dir / "models/test_scene.gltf");
    std::string gltf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const usize at = gltf.find("../textures/checker_a.png");
    if (at != std::string::npos) gltf.replace(at, 25, "../Textures/a.png");
    WriteText(paths.content / "Models/scene.gltf", gltf);

    const std::string a = GuidOf(paths.content, "Textures/a.png");
    const std::string b = GuidOf(paths.content, "Textures/b.png");
    json prefab = {{"$type", "Prefab"}, {"$version", 1},
                   {"entities", json::array({{{"id", 1}, {"parent", 0}, {"components", {{ComponentName(), Component(b)}}}}})}};
    WriteText(paths.content / "Prefabs/crate.aprefab", prefab.dump(2));
    const std::string crate = GuidOf(paths.content, "Prefabs/crate.aprefab");
    json scene = {{"$type", "Scene"}, {"$version", 1},
                  {"entities", json::array({{{"components", {{ComponentName(), Component(a)}}}},
                                            {{"components", {{"PrefabInstance", {{"source", crate}}}}}}})}};
    WriteText(paths.content / "Scenes/start.ascene", scene.dump(2));

    ProjectSettings settings;
    AETHER_CHECK(LoadProject(paths.file, settings, &error));
    settings.startup_scene = "Scenes/start.ascene";
    settings.always_cook = {"Models/"};
    AETHER_CHECK(SaveProject(paths.file, settings, &error));
    return paths.file;
}

} // namespace

AETHER_TEST(Cook_ConfigurationNames) {
    cook::BuildConfiguration c{};
    CHECK(cook::ParseConfiguration("Shipping", c) && c == cook::BuildConfiguration::Shipping);
    CHECK(cook::ParseConfiguration("dev", c) && c == cook::BuildConfiguration::Development);
    CHECK(cook::ParseConfiguration("DEBUG", c) && c == cook::BuildConfiguration::Debug);
    CHECK(!cook::ParseConfiguration("release", c));
    CHECK(std::string(cook::ConfigurationName(cook::BuildConfiguration::Shipping)) == "Shipping");
}

AETHER_TEST(Cook_StripsEditorOnlyFields) {
    json doc = {{"$type", "Scene"},
                {"entities", json::array({{{"components", {{ComponentName(), Component("x")},
                                                           {"UnknownComponent", {{"editor_comment", "kept"}}}}}}})}};
    // Top level: editor_comment; nested: notes.author and notes.todo; and the
    // same two in each of the two history entries.
    CHECK(cook::StripEditorOnly(doc) == 7);
    const json& c = doc["entities"][0]["components"][ComponentName()];
    CHECK(!c.contains("editor_comment") && c["texture"] == "x");
    CHECK(!c["notes"].contains("author") && !c["notes"].contains("todo") && c["notes"]["weight"] == 2.5);
    CHECK(c["history"].size() == 2 && !c["history"][1].contains("author") && c["history"][1]["weight"] == 2.5);
    CHECK(doc["entities"][0]["components"]["UnknownComponent"]["editor_comment"] == "kept"); // not reflected: left alone
    json not_a_scene = {{"hello", 1}};
    CHECK(cook::StripEditorOnly(not_a_scene) == 0);
}

AETHER_TEST(Cook_CooksWhatTheGameReaches) {
    const stdfs::path project = MakeProject("Reach");
    const stdfs::path out = stdfs::temp_directory_path() / "aether_cook_tests/Reach_out";
    stdfs::remove_all(out);
    cook::CookOptions options;
    options.project_file = project;
    options.output_dir = out;
    options.configuration = cook::BuildConfiguration::Shipping;
    options.always_cook = {"Textures/missing.png"};
    const cook::CookReport report = cook::Cook(options);
    CHECK(report.ok);

    std::vector<std::string> cooked;
    for (const cook::CookedAsset& a : report.assets) cooked.push_back(a.path);
    CHECK(cooked == (std::vector<std::string>{"Models/scene.gltf", "Prefabs/crate.aprefab", "Scenes/start.ascene",
                                              "Textures/a.png", "Textures/b.png"}));
    CHECK(report.skipped == 1); // Textures/unused.png
    CHECK(report.extra_files == std::vector<std::string>{"Models/test_cube.bin"});
    CHECK(report.stripped_fields == 14); // 7 in the scene, 7 in the prefab
    bool warned = false;
    for (const std::string& w : report.warnings) warned |= w.find("Textures/missing.png") != std::string::npos;
    CHECK(warned);
    for (const cook::CookedAsset& a : report.assets) {
        if (a.path == "Scenes/start.ascene") CHECK(a.reason == "startup scene");
        if (a.path == "Prefabs/crate.aprefab") CHECK(a.reason == "used by Scenes/start.ascene");
        if (a.path == "Textures/b.png") CHECK(a.reason == "used by Prefabs/crate.aprefab");
        if (a.path == "Models/scene.gltf") CHECK(a.reason == "always cook");
        if (a.importer == "Texture") CHECK(a.imported);
    }

    // The archive.
    pak::PakReader reader;
    std::string error;
    CHECK(reader.Open(report.pak_file.string(), &error) && reader.Verify().empty());
    CHECK(reader.Contains("Content/Scenes/start.ascene") && reader.Contains("Content/Models/test_cube.bin"));
    CHECK(!reader.Contains("Content/Textures/unused.png") && !reader.Contains("Content/Textures/a.png.ameta"));
    std::string text;
    CHECK(reader.ReadText("Content/Scenes/start.ascene", text));
    CHECK(text.find('\n') == std::string::npos && text.find("editor_comment") == std::string::npos &&
          text.find("Grace") == std::string::npos && text.find("\"weight\"") != std::string::npos);
    CHECK(reader.ReadText("Manifest.json", text));
    const json manifest = json::parse(text);
    CHECK(manifest["configuration"] == "Shipping" && manifest["startup_scene"] == "Scenes/start.ascene");
    CHECK(manifest["assets"].size() == 5 && manifest["files"] == json::array({"Models/test_cube.bin"}));
    usize imported = 0;
    for (const json& a : manifest["assets"]) {
        if (a["imported"].get<bool>()) {
            ++imported;
            CHECK(reader.Contains("Imported/" + a["guid"].get<std::string>() + ".bin"));
        }
    }
    CHECK(imported >= 2); // the two textures at least
    // Textures are cooked too: block-compressed, with mips.
    usize cooked_textures = 0;
    for (const json& a : manifest["assets"]) {
        if (a["importer"] != "Texture") continue;
        ++cooked_textures;
        const std::string cooked = a["cooked"].get<std::string>();
        CHECK(!cooked.empty() && reader.Contains(cooked) && !a["cooked_format"].get<std::string>().empty());
        std::vector<u8> atex;
        cook::CookedTexture texture;
        CHECK(reader.Read(cooked, atex) && cook::LoadAtex(atex, texture) && texture.mips.size() > 1);
    }
    CHECK(cooked_textures == 2);
    CHECK(stdfs::exists(report.manifest_file) && report.pak_bytes > 0 && report.pak_bytes == stdfs::file_size(report.pak_file));
}

// A mobile cook: ASTC textures, and the source images left out of the archive.
// A folder on the always-cook list means the folder, not only the files an
// importer claims. A font's licence, a readme, a data file a script opens by
// name: none of those is an asset, and every one of them used to be left out of
// the package without a word.
AETHER_TEST(Cook_AnAlwaysCookFolderShipsItsPlainFilesToo) {
    const stdfs::path project = MakeProject("FolderFiles");
    const stdfs::path content = project.parent_path() / "Content";
    WriteText(content / "Models/LICENSE-cube.txt", "the cube's terms");
    const stdfs::path out = stdfs::temp_directory_path() / "aether_cook_tests/FolderFiles_out";
    stdfs::remove_all(out);
    cook::CookOptions options;
    options.project_file = project;
    options.output_dir = out;
    options.configuration = cook::BuildConfiguration::Shipping;
    const cook::CookReport report = cook::Cook(options);
    CHECK(report.ok);

    bool listed = false;
    for (const std::string& file : report.extra_files) listed = listed || file == "Models/LICENSE-cube.txt";
    CHECK(listed);

    pak::PakReader reader;
    std::string error;
    CHECK(reader.Open(report.pak_file.string(), &error));
    std::vector<u8> bytes;
    CHECK(reader.Read("Content/Models/LICENSE-cube.txt", bytes));
    CHECK(std::string(bytes.begin(), bytes.end()) == "the cube's terms");
}

// A component that names its asset by path -- a sound cue on an AudioSource, a
// widget's UI layout, a sequence's audio key -- used to leave that asset out of
// the package unless its folder was on the always-cook list, because the
// dependency scan only looked for GUID strings.
AETHER_TEST(Cook_AnAssetNamedByPathIsCooked) {
    const stdfs::path project = MakeProject("PathNamed");
    json scene = {{"$type", "Scene"},
                  {"$version", 1},
                  {"entities", json::array({{{"components", {{"UITheme", "Textures/unused.png"}}}}})}};
    WriteText(project.parent_path() / "Content/Scenes/path.ascene", scene.dump(2));
    ProjectSettings settings;
    std::string error;
    AETHER_CHECK(LoadProject(project, settings, &error));
    settings.startup_scene = "Scenes/path.ascene";
    AETHER_CHECK(SaveProject(project, settings, &error));

    const stdfs::path out = stdfs::temp_directory_path() / "aether_cook_tests/PathNamed_out";
    stdfs::remove_all(out);
    cook::CookOptions options;
    options.project_file = project;
    options.output_dir = out;
    options.configuration = cook::BuildConfiguration::Shipping;
    const cook::CookReport report = cook::Cook(options);
    CHECK(report.ok);

    bool cooked = false;
    for (const cook::CookedAsset& asset : report.assets) cooked = cooked || asset.path == "Textures/unused.png";
    CHECK(cooked);
}

// Two path hops chained: a scene names a cue, and the cue names its sound. Both
// are plain strings, so both used to be invisible to the dependency scan and
// audio only shipped when its folder was on the always-cook list.
AETHER_TEST(Cook_ACueReachedFromAScenePullsItsSound) {
    const stdfs::path project = MakeProject("CueChain");
    const stdfs::path content = project.parent_path() / "Content";
    // The sound field is a content path like any other; a texture stands in for a
    // wave so the test needs no audio fixture to prove the resolution.
    WriteText(content / "Audio/shot.acue",
              R"({"name":"shot","nodes":[{"id":1,"pos":[0,0],"sound":"Textures/unused.png","type":"Wave"}],)"
              R"("output":{},"root":1,"version":1})");
    json scene = {{"$type", "Scene"},
                  {"$version", 1},
                  {"entities", json::array({{{"components", {{"AudioSource", {{"cue", "Audio/shot.acue"}}}}}}})}};
    WriteText(content / "Scenes/audio.ascene", scene.dump(2));
    ProjectSettings settings;
    std::string error;
    AETHER_CHECK(LoadProject(project, settings, &error));
    settings.startup_scene = "Scenes/audio.ascene";
    AETHER_CHECK(SaveProject(project, settings, &error));

    const stdfs::path out = stdfs::temp_directory_path() / "aether_cook_tests/CueChain_out";
    stdfs::remove_all(out);
    cook::CookOptions options;
    options.project_file = project;
    options.output_dir = out;
    options.configuration = cook::BuildConfiguration::Shipping;
    const cook::CookReport report = cook::Cook(options);
    CHECK(report.ok);

    std::vector<std::string> cooked;
    for (const cook::CookedAsset& asset : report.assets) cooked.push_back(asset.path);
    CHECK(std::find(cooked.begin(), cooked.end(), "Audio/shot.acue") != cooked.end());
    CHECK(std::find(cooked.begin(), cooked.end(), "Textures/unused.png") != cooked.end());
}

AETHER_TEST(Cook_MobileTargetCooksAstcAndCanStripTheSourceTextures) {
    const stdfs::path project = MakeProject("MobileCook");
    cook::CookOptions options;
    options.project_file = project;
    options.output_dir = project.parent_path() / "Mobile";
    options.texture_target = cook::TextureTarget::Mobile;
    options.strip_source_textures = true;
    const cook::CookReport report = cook::Cook(options);
    CHECK(report.ok);
    pak::PakReader reader;
    std::string error;
    CHECK(reader.Open(report.pak_file.string(), &error));
    std::string text;
    CHECK(reader.ReadText("Manifest.json", text));
    const json manifest = json::parse(text);
    usize textures = 0;
    for (const json& a : manifest["assets"]) {
        if (a["importer"] != "Texture") continue;
        ++textures;
        CHECK(a["cooked_format"].get<std::string>().rfind("astc", 0) == 0);
        CHECK(reader.Contains(a["cooked"].get<std::string>()));
        CHECK(!reader.Contains("Content/" + a["path"].get<std::string>())); // the source is not in the archive
    }
    CHECK(textures == 2);

    // Without the option the sources stay beside the cooked copies.
    options.output_dir = project.parent_path() / "Both";
    options.strip_source_textures = false;
    const cook::CookReport both = cook::Cook(options);
    CHECK(both.ok);
    pak::PakReader reader_both;
    CHECK(reader_both.Open(both.pak_file.string(), &error) && reader_both.Contains("Content/Textures/a.png"));
    CHECK(report.pak_bytes < both.pak_bytes);
}

// A sky's .hdr environment (SkyLight::environment) keeps its source in the pak even when sources are stripped, and gets no block-compressed
// copy: 8 bits per channel can't stand in for its range. Its cooked form is the prefiltered environment (.aenv), not a texture.
AETHER_TEST(Cook_KeepsHdrSkiesAsTheyAre) {
    const stdfs::path project = MakeProject("HdrSky");
    const stdfs::path content = project.parent_path() / "Content";
    // A 2x1 Radiance image, flat (unencoded) scanlines: (1, 1, 1) and (4, 2, 1).
    std::string hdr = std::string("#?RADIANCE") + '\n' + "FORMAT=32-bit_rle_rgbe" + '\n' + '\n' + "-Y 1 +X 2" + '\n';
    for (const unsigned char b : {128, 128, 128, 129, 128, 64, 32, 131}) hdr.push_back(static_cast<char>(b));
    WriteText(content / "Skies/sunset.hdr", hdr);
    json scene = {{"$type", "Scene"},
                  {"$version", 1},
                  {"entities", json::array({{{"components", {{"SkyLight", {{"$v", 1}, {"environment", "Skies/sunset.hdr"}}}}}}})}};
    WriteText(content / "Scenes/sky.ascene", scene.dump(2));
    ProjectSettings settings;
    std::string error;
    AETHER_CHECK(LoadProject(project, settings, &error));
    settings.startup_scene = "Scenes/sky.ascene";
    AETHER_CHECK(SaveProject(project, settings, &error));

    cook::CookOptions options;
    options.project_file = project;
    options.output_dir = project.parent_path() / "HdrOut";
    options.configuration = cook::BuildConfiguration::Shipping;
    options.strip_source_textures = true;
    const cook::CookReport report = cook::Cook(options);
    CHECK(report.ok);
    pak::PakReader reader;
    CHECK(reader.Open(report.pak_file.string(), &error));
    std::vector<u8> bytes;
    CHECK(reader.Read("Content/Skies/sunset.hdr", bytes) && bytes.size() == hdr.size());
    std::string text;
    CHECK(reader.ReadText("Manifest.json", text));
    bool listed = false;
    const json manifest = json::parse(text);
    for (const json& a : manifest["assets"]) {
        if (a.value("path", std::string()) != "Skies/sunset.hdr") continue;
        listed = true;
        const std::string cooked = a.value("cooked", std::string());
        CHECK(cooked.size() > 5 && cooked.substr(cooked.size() - 5) == ".aenv"); // the environment, not an .atex
        CHECK(a.value("cooked_format", std::string()) == "aenv1");
    }
    CHECK(listed);
}

// The cooked environment is what the player would prefilter itself (rotation 0, default settings), reused from the derived data cache on a
// repeat cook, and left out with cook_environments off.
AETHER_TEST(Cook_BuildsAnEnvironmentForAnHdrSky) {
    const stdfs::path project = MakeProject("HdrEnvironment");
    const stdfs::path content = project.parent_path() / "Content";
    // A 64 x 32 Radiance image, flat scanlines: a gradient with a bright patch.
    constexpr u32 kWidth = 64, kHeight = 32;
    std::string hdr = std::string("#?RADIANCE") + '\n' + "FORMAT=32-bit_rle_rgbe" + '\n' + '\n' + "-Y 32 +X 64" + '\n';
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const bool patch = x >= 40 && x < 48 && y >= 6 && y < 12;
            const int mantissa = patch ? 200 : 60 + static_cast<int>(y) * 3;
            for (const int b : {mantissa, mantissa * 3 / 4, mantissa / 2, patch ? 134 : 128}) hdr.push_back(static_cast<char>(b));
        }
    }
    WriteText(content / "Skies/gradient.hdr", hdr);
    json scene = {{"$type", "Scene"},
                  {"$version", 1},
                  {"entities", json::array({{{"components", {{"SkyLight", {{"$v", 1}, {"environment", "Skies/gradient.hdr"}}}}}}})}};
    WriteText(content / "Scenes/sky.ascene", scene.dump(2));
    ProjectSettings settings;
    std::string error;
    AETHER_CHECK(LoadProject(project, settings, &error));
    settings.startup_scene = "Scenes/sky.ascene";
    AETHER_CHECK(SaveProject(project, settings, &error));

    cook::CookOptions options;
    options.project_file = project;
    options.output_dir = project.parent_path() / "EnvOne";
    const cook::CookReport first = cook::Cook(options);
    CHECK(first.ok && first.environment_cache_hits == 0);
    options.output_dir = project.parent_path() / "EnvTwo";
    const cook::CookReport second = cook::Cook(options);
    CHECK(second.ok && second.environment_cache_hits == 1);

    // The .aenv in the archive is exactly what the same prefilter gives for the decoded image.
    pak::PakReader reader;
    CHECK(reader.Open(first.pak_file.string(), &error));
    std::string text;
    CHECK(reader.ReadText("Manifest.json", text));
    std::string cooked_path;
    const json manifest = json::parse(text);
    for (const json& a : manifest["assets"]) {
        if (a.value("path", std::string()) == "Skies/gradient.hdr") cooked_path = a.value("cooked", std::string());
    }
    CHECK(!cooked_path.empty());
    std::vector<u8> aenv;
    CHECK(reader.Read(cooked_path, aenv));
    assets::PrefilteredEnvironment cooked;
    assets::PrefilterSettings built;
    CHECK(assets::LoadEnvironment(aenv, cooked, &built, &error));
    std::vector<u8> source(hdr.begin(), hdr.end());
    assets::HdrImageData image;
    CHECK(assets::DecodeHdrImageFromMemory(source, image));
    const assets::PrefilterSettings defaults;
    const assets::PrefilteredEnvironment expected =
        assets::PrefilterEnvironment(assets::MakeEnvironment(image.width, image.height, image.rgb, 0.0f), defaults);
    CHECK(expected.mip_count > 0 && cooked.mip_count == expected.mip_count && cooked.width == expected.width);
    CHECK(cooked.halves == expected.halves);
    CHECK(std::memcmp(cooked.irradiance_sh, expected.irradiance_sh, sizeof(cooked.irradiance_sh)) == 0);
    CHECK(built.max_width == defaults.max_width && built.smallest_width == defaults.smallest_width && built.samples == defaults.samples);

    // Off: no environment, the source still there.
    options.cook_environments = false;
    options.output_dir = project.parent_path() / "EnvOff";
    const cook::CookReport off = cook::Cook(options);
    CHECK(off.ok);
    pak::PakReader off_reader;
    CHECK(off_reader.Open(off.pak_file.string(), &error));
    CHECK(off_reader.Contains("Content/Skies/gradient.hdr"));
    CHECK(!off_reader.Contains(cooked_path));
}

AETHER_TEST(Cook_RefusesNothingToCook) {
    const stdfs::path parent = stdfs::temp_directory_path() / "aether_cook_tests";
    stdfs::remove_all(parent / "Empty");
    stdfs::create_directories(parent);
    ProjectPaths paths;
    std::string error;
    CHECK(CreateProject(parent, "Empty", &paths, &error));
    cook::CookOptions options;
    options.project_file = paths.file;
    options.output_dir = parent / "Empty_out";
    cook::CookReport report = cook::Cook(options);
    CHECK(!report.ok && report.error.find("Nothing to cook") != std::string::npos);
    options.project_file = parent / "nope.aproject";
    report = cook::Cook(options);
    CHECK(!report.ok && report.error.find("Can't load") != std::string::npos);
}

AETHER_TEST(Cook_RepeatBuildsAreByteIdentical) {
    const stdfs::path project = MakeProject("Repeatable");
    cook::CookOptions options;
    options.project_file = project;
    options.configuration = cook::BuildConfiguration::Shipping;
    options.output_dir = project.parent_path() / "BuildOne";
    const cook::CookReport first = cook::Cook(options);
    CHECK(first.ok);

    options.output_dir = project.parent_path() / "BuildTwo";
    const cook::CookReport second = cook::Cook(options);
    CHECK(second.ok);
    CHECK(first.texture_cache_hits == 0 && first.texture_cache_misses == 2);
    CHECK(second.texture_cache_hits == 2 && second.texture_cache_misses == 0);

    const auto read_bytes = [](const stdfs::path& path) {
        std::ifstream in(path, std::ios::binary);
        AETHER_CHECK(in.good());
        return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    CHECK(read_bytes(first.pak_file) == read_bytes(second.pak_file));
    CHECK(read_bytes(first.manifest_file) == read_bytes(second.manifest_file));

    // Changing one source invalidates only that texture's cooked cache entry.
    stdfs::copy_file(stdfs::path(AETHER_REPO_ASSETS_DIR) / "textures/test_corners.png",
                     project.parent_path() / "Content/Textures/a.png", stdfs::copy_options::overwrite_existing);
    options.output_dir = project.parent_path() / "BuildThree";
    const cook::CookReport third = cook::Cook(options);
    CHECK(third.ok);
    CHECK(third.texture_cache_hits == 1 && third.texture_cache_misses == 1);
    CHECK(read_bytes(first.pak_file) != read_bytes(third.pak_file));
}

// A fresh cook (no Intermediate/ import cache, .ameta files without sub-asset
// GUIDs yet, as a glTF that was never imported is committed) is
// byte-reproducible: the importer's sub-asset GUIDs are derived, not random.
AETHER_TEST(Cook_FreshCooksFromCleanCopiesAreByteIdentical) {
    const stdfs::path project = MakeProject("FreshSource");
    const stdfs::path source_dir = project.parent_path();
    // Every source gets its committed .ameta first (as in a real checkout); only
    // the import outputs and caches are left for the fresh cooks to create.
    CHECK(!GuidOf(source_dir / "Content", "Scenes/start.ascene").empty());
    const auto read_bytes = [](const stdfs::path& path) {
        std::ifstream in(path, std::ios::binary);
        AETHER_CHECK(in.good());
        return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    cook::CookReport reports[2];
    for (int i = 0; i < 2; ++i) {
        const stdfs::path copy = source_dir.parent_path() / (i == 0 ? "FreshCopyA" : "FreshCopyB");
        stdfs::remove_all(copy);
        stdfs::copy(source_dir, copy, stdfs::copy_options::recursive);
        stdfs::remove_all(copy / "Intermediate");
        CHECK(read_bytes(copy / "Content/Models/scene.gltf.ameta") ==
              read_bytes(source_dir / "Content/Models/scene.gltf.ameta"));
        cook::CookOptions options;
        options.project_file = copy / project.filename();
        options.configuration = cook::BuildConfiguration::Development;
        options.output_dir = copy / "Build";
        reports[i] = cook::Cook(options);
        CHECK(reports[i].ok);
        CHECK(reports[i].texture_cache_hits == 0);
    }
    CHECK(read_bytes(reports[0].pak_file) == read_bytes(reports[1].pak_file));
    CHECK(read_bytes(reports[0].manifest_file) == read_bytes(reports[1].manifest_file));
    // The import really produced sub-assets, and both copies agree on their GUIDs.
    const auto meta_a = read_bytes(source_dir.parent_path() / "FreshCopyA/Content/Models/scene.gltf.ameta");
    CHECK(std::string(meta_a.begin(), meta_a.end()).find("\"mesh:0\"") != std::string::npos);
    CHECK(meta_a == read_bytes(source_dir.parent_path() / "FreshCopyB/Content/Models/scene.gltf.ameta"));
}

AETHER_TEST(Cook_StrictRejectsImporterWarnings) {
    const stdfs::path project = MakeProject("ImporterWarning");
    const stdfs::path meta_file = project.parent_path() / "Content/Textures/a.png.ameta";
    assets::AssetMeta meta;
    CHECK(assets::LoadAssetMeta(meta_file, meta));
    meta.settings["generate_mips"] = "invalid";
    meta.settings["mips"] = "invalid";
    meta.settings["cook_format"] = 42;
    CHECK(assets::SaveAssetMeta(meta_file, meta));

    cook::CookOptions options;
    options.project_file = project;
    options.output_dir = project.parent_path() / "StrictBuild";
    options.strict_validation = true;
    const cook::CookReport strict = cook::Cook(options);
    CHECK(!strict.ok && strict.error.find("Cook validation failed") == 0);
    CHECK(!stdfs::exists(options.output_dir / "Game.apak"));
    bool named_import_warning = false;
    bool named_cook_warning = false;
    for (const std::string& warning : strict.warnings) {
        named_import_warning |= warning.find("Textures/a.png: Setting \"generate_mips\" has the wrong type") != std::string::npos;
        named_cook_warning |= warning.find("Textures/a.png: setting 'mips' must be a boolean") != std::string::npos;
    }
    CHECK(named_import_warning && named_cook_warning);

    options.strict_validation = false;
    const cook::CookReport development = cook::Cook(options);
    CHECK(development.ok && stdfs::exists(development.pak_file));
}

// M8.5 importer validation: broken or incompatible sources are reported by their content path with the reason,
// the cook neither crashes nor drops the good assets, and a strict cook refuses to write an archive.
AETHER_TEST(Cook_InvalidSourcesAreReportedByPathWithTheReason) {
    const stdfs::path project = MakeProject("InvalidSources");
    const stdfs::path content = project.parent_path() / "Content";
    WriteText(content / "Broken/not_an_image.png", "this is not a PNG");
    WriteText(content / "Broken/truncated.png", std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16));
    WriteText(content / "Broken/malformed.gltf", "{ \"asset\": { \"version\": \"2.0\" }, \"meshes\": [");
    WriteText(content / "Broken/missing_buffer.gltf",
              R"({"asset":{"version":"2.0"},"buffers":[{"uri":"gone.bin","byteLength":36}],)"
              R"("bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
              R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})");
    WriteText(content / "Broken/noise.wav", std::string("RIFF\x04\0\0\0WAVE", 12)); // no fmt or data chunk
    WriteText(content / "Prefabs/broken.aprefab", "{ not json");

    ProjectSettings settings;
    std::string error;
    CHECK(LoadProject(project, settings, &error));
    settings.always_cook = {"Models/", "Broken/", "Prefabs/broken.aprefab", "Textures/gone.png"};
    CHECK(SaveProject(project, settings, &error));

    cook::CookOptions options;
    options.project_file = project;
    options.output_dir = project.parent_path() / "Lenient";
    const cook::CookReport lenient = cook::Cook(options);
    CHECK(lenient.ok && stdfs::exists(lenient.pak_file));
    const auto reported = [&](const char* path) {
        for (const std::string& w : lenient.warnings) {
            if (w.find(path) != std::string::npos) return true;
        }
        return false;
    };
    for (const std::string& w : lenient.warnings) std::printf("cook warning: %s\n", w.c_str());
    CHECK(reported("Broken/not_an_image.png"));
    CHECK(reported("Broken/truncated.png"));
    CHECK(reported("Broken/malformed.gltf: Failed to parse"));
    CHECK(reported("Broken/missing_buffer.gltf: ") && reported("The buffer file \"gone.bin\" is missing")); // by path, with the buffer named and what is wrong with it
    CHECK(reported("Broken/noise.wav: the WAVE file has no fmt or data chunk"));
    CHECK(reported("Prefabs/broken.aprefab"));
    CHECK(reported("Textures/gone.png"));
    // The good content still cooks.
    pak::PakReader reader;
    CHECK(reader.Open(lenient.pak_file.string(), &error) && reader.Verify().empty());
    CHECK(reader.Contains("Content/Scenes/start.ascene") && reader.Contains("Content/Textures/a.png") && reader.Contains("Content/Models/scene.gltf"));

    options.output_dir = project.parent_path() / "Strict";
    options.strict_validation = true;
    const cook::CookReport strict = cook::Cook(options);
    CHECK(!strict.ok && strict.error.find("Cook validation failed") == 0 && !stdfs::exists(options.output_dir / "Game.apak"));
}

// Phase 42.4: the cooker can bake HLOD proxies; the option must not disturb cooking.
AETHER_TEST(Cook_HlodOptionCooks) {
    const stdfs::path project = MakeProject("Hlod");
    const stdfs::path out = stdfs::temp_directory_path() / "aether_cook_tests/Hlod_out";
    stdfs::remove_all(out);
    cook::CookOptions options;
    options.project_file = project;
    options.output_dir = out;
    options.configuration = cook::BuildConfiguration::Shipping;
    options.always_cook = {"Models/scene.gltf"};
    options.cook_hlod = true;
    const cook::CookReport report = cook::Cook(options);
    CHECK(report.ok);
    pak::PakReader reader;
    std::string error;
    CHECK(reader.Open((out / "Game.apak").string(), &error) && reader.Verify().empty());
}

// --- Cooking at scale -------------------------------------------------------------
//
// Roadmap priority 4: cooks stay deterministic and incremental with thousands of assets. The project is generated: `textures`
// 16x16 .bmp files and `models` .gltf grids of `grid` x `grid` vertices, every one different, all kept by always_cook.

namespace {

void WriteBytes(const stdfs::path& file, const std::vector<u8>& bytes) {
    stdfs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

template <typename T> void Append(std::vector<u8>& out, const T& value) {
    const u8* p = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

// A 24-bit uncompressed .bmp whose pixels depend on `seed`.
std::vector<u8> MakeBmp(u32 size, u32 seed) {
    const u32 row = (size * 3 + 3) & ~3u;
    std::vector<u8> out = {'B', 'M'};
    Append<u32>(out, 54 + row * size);
    Append<u32>(out, 0);
    Append<u32>(out, 54);
    Append<u32>(out, 40);
    Append<i32>(out, static_cast<i32>(size));
    Append<i32>(out, static_cast<i32>(size));
    Append<u16>(out, 1);
    Append<u16>(out, 24);
    Append<u32>(out, 0);
    Append<u32>(out, row * size);
    Append<u32>(out, 2835);
    Append<u32>(out, 2835);
    Append<u32>(out, 0);
    Append<u32>(out, 0);
    u32 state = seed * 2654435761u + 1u;
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < row; ++x) {
            state = state * 1664525u + 1013904223u;
            out.push_back(x < size * 3 ? static_cast<u8>(state >> 24) : u8{0});
        }
    }
    return out;
}

// A height-field grid as a .gltf with its .bin next to it; `seed` moves the heights.
void WriteGridModel(const stdfs::path& folder, const std::string& stem, u32 grid, u32 seed) {
    std::vector<u8> bin;
    const u32 vertices = grid * grid;
    for (u32 z = 0; z < grid; ++z) {
        for (u32 x = 0; x < grid; ++x) {
            const f32 fx = static_cast<f32>(x), fz = static_cast<f32>(z);
            Append<f32>(bin, fx);
            Append<f32>(bin, std::sin(fx * 0.37f + static_cast<f32>(seed)) * std::cos(fz * 0.23f) * 2.0f);
            Append<f32>(bin, fz);
        }
    }
    for (u32 i = 0; i < vertices; ++i) {
        Append<f32>(bin, 0.0f);
        Append<f32>(bin, 1.0f);
        Append<f32>(bin, 0.0f);
    }
    const u32 index_offset = static_cast<u32>(bin.size());
    u32 indices = 0;
    for (u32 z = 0; z + 1 < grid; ++z) {
        for (u32 x = 0; x + 1 < grid; ++x) {
            const u32 a = z * grid + x, b = a + 1, c = a + grid, d = c + 1;
            for (u32 i : {a, c, b, b, c, d}) Append<u32>(bin, i);
            indices += 6;
        }
    }
    WriteBytes(folder / (stem + ".bin"), bin);
    const json gltf = {
        {"asset", {{"version", "2.0"}}},
        {"buffers", json::array({{{"uri", stem + ".bin"}, {"byteLength", bin.size()}}})},
        {"bufferViews", json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", vertices * 12}},
                                     {{"buffer", 0}, {"byteOffset", vertices * 12}, {"byteLength", vertices * 12}},
                                     {{"buffer", 0}, {"byteOffset", index_offset}, {"byteLength", indices * 4}}})},
        {"accessors", json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", vertices}, {"type", "VEC3"}},
                                   {{"bufferView", 1}, {"componentType", 5126}, {"count", vertices}, {"type", "VEC3"}},
                                   {{"bufferView", 2}, {"componentType", 5125}, {"count", indices}, {"type", "SCALAR"}}})},
        {"meshes", json::array({{{"name", stem},
                                 {"primitives", json::array({{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}}}, {"indices", 2}, {"mode", 4}}})}}})},
        {"nodes", json::array({{{"name", stem}, {"mesh", 0}}})},
        {"scenes", json::array({{{"nodes", json::array({0})}}})},
        {"scene", 0},
    };
    WriteText(folder / (stem + ".gltf"), gltf.dump());
}

stdfs::path MakeScaleProject(const std::string& name, u32 textures, u32 models, u32 grid) {
    const stdfs::path parent = stdfs::temp_directory_path() / "aether_cook_tests";
    stdfs::remove_all(parent / name);
    stdfs::create_directories(parent);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(parent, name, &paths, &error));
    for (u32 i = 0; i < textures; ++i) {
        WriteBytes(paths.content / "Textures" / ("d" + std::to_string(i % 16)) / ("t" + std::to_string(i) + ".bmp"), MakeBmp(16, i));
    }
    for (u32 i = 0; i < models; ++i) {
        WriteGridModel(paths.content / "Models" / ("d" + std::to_string(i % 16)), "m" + std::to_string(i), grid, i);
    }
    ProjectSettings settings;
    AETHER_CHECK(LoadProject(paths.file, settings, &error));
    settings.always_cook = {"Textures/", "Models/"};
    AETHER_CHECK(SaveProject(paths.file, settings, &error));
    return paths.file;
}

// The scale cooks log three lines for every model on every cook, which was most of a test run's output: warnings and errors only
// while one of these is alive.
struct QuietLog {
    LogLevel before = Logger::Instance().MinLevel();
    QuietLog() { Logger::Instance().SetMinLevel(LogLevel::Warn); }
    ~QuietLog() { Logger::Instance().SetMinLevel(before); }
};

u32 EnvCount(const char* name, u32 fallback) {
    const char* text = std::getenv(name);
    return text && *text ? static_cast<u32>(std::strtoul(text, nullptr, 10)) : fallback;
}

} // namespace

// The numbers in docs/design/PERFORMANCE.md ("Cooking at scale") come from this; it does nothing unless a size is given:
//   AETHER_TEST_FILTER=Cook_ScaleMeasure AETHER_COOK_SCALE_TEXTURES=2000 AETHER_COOK_SCALE_MODELS=1000 aether_tests
AETHER_TEST(Cook_ScaleMeasure) {
    const QuietLog quiet;
    const u32 textures = EnvCount("AETHER_COOK_SCALE_TEXTURES", 0);
    const u32 models = EnvCount("AETHER_COOK_SCALE_MODELS", 0);
    const u32 grid = EnvCount("AETHER_COOK_SCALE_GRID", 33);
    if (textures + models == 0) return;
    const stdfs::path project = MakeScaleProject("ScaleMeasure", textures, models, grid);
    cook::CookOptions options;
    options.project_file = project;
    options.configuration = cook::BuildConfiguration::Shipping;
    options.cook_meshes = EnvCount("AETHER_COOK_SCALE_MESHES", 1) != 0;
    if (EnvCount("AETHER_COOK_SCALE_NOCOMPRESS", 0)) options.compression = pak::CompressionPolicy::None;
    for (int pass = 0; pass < 3; ++pass) {
        options.output_dir = project.parent_path() / ("Build" + std::to_string(pass));
        const auto start = std::chrono::steady_clock::now();
        const cook::CookReport r = cook::Cook(options);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        CHECK(r.ok);
        std::printf("cook %d: %.0f ms, %zu assets, texture hit/miss %zu/%zu, import hit/miss %zu/%zu, mesh hit/miss %zu/%zu, pak %llu bytes\n",
                    pass, ms, r.assets.size(), r.texture_cache_hits, r.texture_cache_misses, r.import_cache_hits,
                    r.import_cache_misses, r.mesh_cache_hits, r.mesh_cache_misses, static_cast<unsigned long long>(r.pak_bytes));
    }
}

// 450 assets: a second cook redoes nothing and gives the same bytes; changing two sources redoes those two only and keeps every
// GUID; and what the caches hand back is what a cook with no cache builds.
AETHER_TEST(Cook_AtScaleIsIncrementalAndDeterministic) {
    const QuietLog quiet;
    const u32 textures = 300, models = 150, grid = 17;
    const stdfs::path project = MakeScaleProject("AtScale", textures, models, grid);
    const stdfs::path root = project.parent_path();
    const auto read_bytes = [](const stdfs::path& path) {
        std::ifstream in(path, std::ios::binary);
        AETHER_CHECK(in.good());
        return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    cook::CookOptions options;
    options.project_file = project;
    options.configuration = cook::BuildConfiguration::Shipping;

    options.output_dir = root / "BuildOne";
    const cook::CookReport first = cook::Cook(options);
    CHECK(first.ok);
    CHECK(first.assets.size() == textures + models);
    CHECK(first.texture_cache_hits == 0 && first.texture_cache_misses == textures);
    CHECK(first.mesh_cache_hits == 0 && first.mesh_cache_misses == models);
    CHECK(first.import_cache_hits == 0 && first.import_cache_misses == textures + models);

    options.output_dir = root / "BuildTwo";
    const cook::CookReport second = cook::Cook(options);
    CHECK(second.ok);
    CHECK(second.texture_cache_hits == textures && second.texture_cache_misses == 0);
    CHECK(second.mesh_cache_hits == models && second.mesh_cache_misses == 0);
    CHECK(second.import_cache_hits == textures + models && second.import_cache_misses == 0);
    CHECK(read_bytes(first.pak_file) == read_bytes(second.pak_file));
    CHECK(read_bytes(first.manifest_file) == read_bytes(second.manifest_file));

    // One texture and one model change. The model's .gltf is the same text: only the .bin beside it differs.
    WriteBytes(root / "Content/Textures/d7/t7.bmp", MakeBmp(16, 9007));
    WriteGridModel(root / "Content/Models/d3", "m3", grid, 9003);
    options.output_dir = root / "BuildThree";
    const cook::CookReport third = cook::Cook(options);
    CHECK(third.ok);
    CHECK(third.texture_cache_hits == textures - 1 && third.texture_cache_misses == 1);
    CHECK(third.mesh_cache_hits == models - 1 && third.mesh_cache_misses == 1);
    CHECK(third.import_cache_hits == textures + models - 2 && third.import_cache_misses == 2);
    CHECK(read_bytes(first.pak_file) != read_bytes(third.pak_file));
    CHECK(third.assets.size() == first.assets.size());
    if (third.assets.size() == first.assets.size()) {
        bool same_identity = true;
        for (usize i = 0; i < first.assets.size(); ++i) {
            same_identity = same_identity && first.assets[i].guid == third.assets[i].guid && first.assets[i].path == third.assets[i].path;
        }
        CHECK(same_identity);
    }

    // Different mesh settings are a different cache entry, not a stale hit.
    options.cook_hlod = true;
    options.output_dir = root / "BuildHlod";
    const cook::CookReport hlod = cook::Cook(options);
    CHECK(hlod.ok);
    CHECK(hlod.mesh_cache_hits == 0 && hlod.mesh_cache_misses == models);
    options.cook_hlod = false;

    // Without the caches the cook builds the same archive the caches gave.
    stdfs::remove_all(root / "Intermediate");
    options.output_dir = root / "BuildFresh";
    const cook::CookReport fresh = cook::Cook(options);
    CHECK(fresh.ok);
    CHECK(fresh.mesh_cache_hits == 0 && fresh.texture_cache_hits == 0 && fresh.import_cache_hits == 0);
    CHECK(read_bytes(third.pak_file) == read_bytes(fresh.pak_file));
    CHECK(read_bytes(third.manifest_file) == read_bytes(fresh.manifest_file));

    // A damaged cache entry is rebuilt, not shipped.
    usize damaged = 0;
    for (const auto& entry : stdfs::recursive_directory_iterator(root / "Intermediate")) {
        if (!entry.is_regular_file() || entry.path().extension() != ".ddc") continue;
        std::error_code ec;
        stdfs::resize_file(entry.path(), entry.file_size() / 2, ec);
        if (!ec) ++damaged;
    }
    CHECK(damaged > 0);
    options.output_dir = root / "BuildRepaired";
    const cook::CookReport repaired = cook::Cook(options);
    CHECK(repaired.ok);
    CHECK(repaired.mesh_cache_hits == 0 && repaired.texture_cache_hits == 0);
    CHECK(read_bytes(third.pak_file) == read_bytes(repaired.pak_file));
}
