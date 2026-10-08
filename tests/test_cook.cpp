#include "aether/cook/cooker.h"
#include "aether/cook/texture_cook.h"
#include "aether/pak/pak.h"
#include "aether/project/project.h"
#include "aether/reflection/reflection.h"
#include "test_framework.h"

#include <nlohmann/json.hpp>

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
