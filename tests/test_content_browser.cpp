#include "aether/assets/content_browser.h"
#include "aether/assets/gltf_references.h"
#include "aether/assets/model_importer.h"
#include "aether/reflection/serialize.h"
#include "test_framework.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace aether;
using namespace aether::assets;
namespace stdfs = std::filesystem;

namespace {

const stdfs::path kRepoAssets = stdfs::path(AETHER_REPO_ASSETS_DIR);

stdfs::path ProjectWithContent(const char* name) {
    stdfs::path dir = stdfs::temp_directory_path() / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir / "Content");
    stdfs::copy(kRepoAssets / "models", dir / "Content/models", stdfs::copy_options::recursive);
    stdfs::copy(kRepoAssets / "textures", dir / "Content/textures", stdfs::copy_options::recursive);
    return dir;
}

std::vector<std::string> Names(const std::vector<ContentEntry>& entries) {
    std::vector<std::string> names;
    for (const ContentEntry& entry : entries) {
        names.push_back(entry.is_folder ? entry.name + "/" : entry.name);
    }
    return names;
}

std::vector<std::string> ImageUris(const stdfs::path& root, const std::string& gltf) {
    std::vector<std::string> uris;
    for (const GltfFileReference& ref : GltfFileReferences(root, gltf)) {
        uris.push_back(ref.uri);
    }
    return uris;
}

ContentQuery Query(std::string folder, std::string search = "") {
    ContentQuery query;
    query.folder = std::move(folder);
    query.search = std::move(search);
    return query;
}

bool Contains(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

} // namespace

AETHER_TEST(ContentBrowser_ListsSearchesAndFilters) {
    stdfs::path project = ProjectWithContent("aether_test_content_list");
    AssetDatabase db(project / "Content");
    db.Scan();
    std::string error;
    AETHER_CHECK(CreateContentFolder(db, "", "Empty", &error));
    AETHER_CHECK(!CreateContentFolder(db, "", "Empty", &error)); // exists
    AETHER_CHECK(!CreateContentFolder(db, "", "a/b", &error) && !CreateContentFolder(db, "..", "x", &error));
    stdfs::create_directories(project / "Content/.hidden");

    AETHER_CHECK(ContentFolders(db) == (std::vector<std::string>{"Empty", "models", "textures"}));
    // Folders first, case-insensitive order.
    AETHER_CHECK(Names(ListContent(db, ContentQuery{})) == (std::vector<std::string>{"Empty/", "models/", "textures/"}));
    AETHER_CHECK(Names(ListContent(db, Query("textures"))) ==
                 (std::vector<std::string>{"checker_a.png", "checker_b.png", "test_corners.png", "test_solid_red.png"}));
    AETHER_CHECK(ListContent(db, Query("Empty")).empty());

    // Searching looks through subfolders, ignoring case; types filter.
    AETHER_CHECK(Names(ListContent(db, Query("", "CHECKER"))) == (std::vector<std::string>{"checker_a.png", "checker_b.png"}));
    ContentQuery models;
    models.types = {"Model"};
    models.recursive = true;
    AETHER_CHECK(ListContent(db, models).size() == 4);
    const std::vector<ContentEntry> red = ListContent(db, Query("", "solid"));
    AETHER_CHECK(red.size() == 1 && red[0].asset != nullptr && red[0].path == "textures/test_solid_red.png");

    // After importing, a model's pieces can be listed with it.
    ImporterRegistry importers = ImporterRegistry::WithBuiltins();
    DerivedDataCache cache(project / "Intermediate/DDC");
    ImportAll(db, importers, cache);
    ContentQuery pieces;
    pieces.folder = "models";
    pieces.include_sub_assets = true;
    pieces.search = "mesh";
    AETHER_CHECK(ListContent(db, pieces).size() == 4); // one mesh:0 per model
    pieces.search.clear();
    pieces.types = {"Material"};
    const std::vector<ContentEntry> materials = ListContent(db, pieces);
    AETHER_CHECK(materials.size() == 3 && materials[0].name == "material:0" && materials[0].asset->IsSubAsset());
    stdfs::remove_all(project);
}

AETHER_TEST(ContentBrowser_NamesAreValidatedForEveryPlatform) {
    std::string error;
    AETHER_CHECK(IsValidAssetName("Brick Wall_01.png"));
    for (const char* bad : {"", "a/b", "a\\b", "what?", "a:b", "star*", "x#y", ".hidden", "trailing.", "trailing ",
                            "CON", "nul.txt", "Com1"}) {
        AETHER_CHECK(!IsValidAssetName(bad, &error) && !error.empty());
    }
    AETHER_CHECK(IsValidAssetName("console.png")); // only exact device names are reserved
    AETHER_CHECK(UriEncodePath("my textures/a+b.png") == "my%20textures/a%2Bb.png");
    AETHER_CHECK(UriDecodePath(UriEncodePath("été/x y.png")) == "été/x y.png");
}

AETHER_TEST(ContentBrowser_MovesKeepGltfFileReferencesWorking) {
    stdfs::path project = ProjectWithContent("aether_test_content_move");
    const stdfs::path root = project / "Content";
    AssetDatabase db(root);
    db.Scan();
    ImporterRegistry importers = ImporterRegistry::WithBuiltins();
    DerivedDataCache cache(project / "Intermediate/DDC");
    AETHER_CHECK(ImportAll(db, importers, cache).failed == 0);
    const AssetGuid checker = db.FindByPath("textures/checker_a.png")->guid;
    const AssetGuid cube = db.FindByPath("models/test_textured_cube.gltf")->guid;

    // A .gltf depends on the textures it names, so deleting one is refused.
    AETHER_CHECK(db.Find(cube)->dependencies == std::vector<AssetGuid>{checker});
    std::string error;
    AETHER_CHECK(!db.Delete(checker, false, &error) && error.find("test_textured_cube.gltf") != std::string::npos);

    // Moving the texture rewrites every model that points at it.
    MoveResult moved = MoveContent(db, "textures/checker_a.png", "materials/checker.png");
    AETHER_CHECK(moved.ok && moved.error.empty());
    AETHER_CHECK(moved.rewritten == (std::vector<std::string>{"models/test_animation.gltf", "models/test_scene.gltf",
                                                              "models/test_textured_cube.gltf"}));
    AETHER_CHECK(db.FindByPath("materials/checker.png")->guid == checker); // same identity
    AETHER_CHECK(Contains(ImageUris(root, "models/test_textured_cube.gltf"), "../materials/checker.png"));
    AETHER_CHECK(Contains(ImageUris(root, "models/test_textured_cube.gltf"), "test_cube.bin")); // untouched
    AETHER_CHECK(db.Find(cube)->dependencies == std::vector<AssetGuid>{checker});
    AETHER_CHECK(db.Find(cube)->needs_import); // its file changed
    ImportOutput material = ImportAsset(db, db.FindByPath("models/test_textured_cube.gltf#material:0")->guid,
                                        importers, cache);
    MaterialData data;
    AETHER_CHECK(material.ok && reflect::LoadBinary(data, material.data));
    AETHER_CHECK(data.base_color_texture == "materials/checker.png");

    // Moving a model on its own rewrites its own references (buffer + image).
    moved = MoveContent(db, "models/test_textured_cube.gltf", "props/crates/cube.gltf");
    AETHER_CHECK(moved.ok && moved.rewritten == std::vector<std::string>{"props/crates/cube.gltf"});
    AETHER_CHECK(db.FindByPath("props/crates/cube.gltf")->guid == cube);
    std::vector<std::string> uris = ImageUris(root, "props/crates/cube.gltf");
    AETHER_CHECK(Contains(uris, "../../models/test_cube.bin") && Contains(uris, "../../materials/checker.png"));
    ImportOutput mesh = ImportAsset(db, db.FindByPath("props/crates/cube.gltf#mesh:0")->guid, importers, cache);
    MeshData mesh_data;
    AETHER_CHECK(mesh.ok && DecodeMeshData(mesh.data, mesh_data) && mesh_data.primitives[0].vertices.size() == 24);

    // Moving a folder carries its helper files and keeps inside references as they are.
    const AssetGuid scene = db.FindByPath("models/test_scene.gltf")->guid;
    moved = MoveContent(db, "models", "art/models");
    AETHER_CHECK(moved.ok && stdfs::exists(root / "art/models/test_cube.bin"));
    AETHER_CHECK(db.FindByPath("art/models/test_scene.gltf")->guid == scene);
    uris = ImageUris(root, "art/models/test_scene.gltf");
    AETHER_CHECK(Contains(uris, "test_cube.bin") && Contains(uris, "../../materials/checker.png"));
    // ...and references from outside into it are updated.
    uris = ImageUris(root, "props/crates/cube.gltf");
    AETHER_CHECK(Contains(uris, "../../art/models/test_cube.bin"));
    AETHER_CHECK(ImportAsset(db, cube, importers, cache).ok);

    // Rename: keeps the extension, refuses changing it.
    AETHER_CHECK(RenameContent(db, "textures/checker_b.png", "stone").ok);
    AETHER_CHECK(db.FindByPath("textures/stone.png") != nullptr);
    AETHER_CHECK(!RenameContent(db, "textures/stone.png", "stone.jpg").ok);
    AETHER_CHECK(RenameContent(db, "textures/stone.png", "Stone.PNG").ok); // same extension, any case
    AETHER_CHECK(RenameContent(db, "props", "Props").ok && db.FindByPath("Props/crates/cube.gltf")->guid == cube);

    // Refusals.
    AETHER_CHECK(!MoveContent(db, "art", "art/inner").ok);                     // into itself
    AETHER_CHECK(!MoveContent(db, "textures/test_corners.png", "materials/checker.png").ok); // exists
    AETHER_CHECK(!MoveContent(db, "Props/crates/cube.gltf#mesh:0", "x.gltf").ok);
    AETHER_CHECK(!MoveContent(db, "textures/test_corners.png.ameta", "x.ameta").ok);
    AETHER_CHECK(!MoveContent(db, "textures/test_corners.png", "../outside.png").ok);
    AETHER_CHECK(!MoveContent(db, "nothing_here.png", "x.png").ok);
    AETHER_CHECK(!MoveContent(db, "textures/test_corners.png", "bad?/x.png").ok);
    stdfs::remove_all(project);
}

AETHER_TEST(ContentBrowser_ReferenceViewerWalksBothWays) {
    stdfs::path project = ProjectWithContent("aether_test_content_refs");
    const stdfs::path root = project / "Content";
    AssetDatabase db(root);
    db.Scan();
    ImporterRegistry importers = ImporterRegistry::WithBuiltins();
    DerivedDataCache cache(project / "Intermediate/DDC");
    ImportAll(db, importers, cache);
    const AssetGuid checker = db.FindByPath("textures/checker_a.png")->guid;
    const AssetGuid cube_mesh = db.FindByPath("models/test_textured_cube.gltf#mesh:0")->guid;
    // Two levels using the cube's mesh, and referring to each other.
    std::ofstream(root / "a.ascene") << "{\"mesh\": \"" << ToString(cube_mesh) << "\"}";
    db.Scan();
    const AssetGuid level_a = db.FindByPath("a.ascene")->guid;
    std::ofstream(root / "b.ascene") << "{\"next\": \"" << ToString(level_a) << "\"}";
    db.Scan();
    const AssetGuid level_b = db.FindByPath("b.ascene")->guid;
    std::ofstream(root / "a.ascene") << "{\"mesh\": \"" << ToString(cube_mesh) << "\", \"back\": \"" << ToString(level_b)
                                     << "\"}";
    db.Scan();

    // What uses the texture: the models, then (through the cube's mesh) the levels.
    std::vector<ReferenceNode> users = CollectReferences(db, checker, ReferenceDirection::Referencers);
    AETHER_CHECK(users.size() >= 5 && users[0].guid == checker && users[0].depth == 0);
    std::vector<std::string> paths;
    for (const ReferenceNode& node : users) {
        paths.push_back(node.path + "@" + std::to_string(node.depth) + (node.repeated ? "*" : ""));
    }
    AETHER_CHECK(Contains(paths, "models/test_textured_cube.gltf@1"));
    AETHER_CHECK(Contains(paths, "models/test_scene.gltf@1"));
    AETHER_CHECK(Contains(paths, "a.ascene@2"));
    AETHER_CHECK(Contains(paths, "b.ascene@3"));
    AETHER_CHECK(Contains(paths, "a.ascene@4*")); // the cycle is shown once, not followed

    // What the level uses.
    std::vector<ReferenceNode> uses = CollectReferences(db, level_a, ReferenceDirection::Dependencies);
    AETHER_CHECK(uses.size() == 4); // a, the mesh, b, then a again (repeated)
    AETHER_CHECK(uses[1].path == "b.ascene" && uses[2].path == "a.ascene" && uses[2].repeated);
    AETHER_CHECK(uses[3].guid == cube_mesh && uses[3].depth == 1);
    AETHER_CHECK(CollectReferences(db, level_a, ReferenceDirection::Dependencies, 0).size() == 1);
    AETHER_CHECK(CollectReferences(db, NewAssetGuid(), ReferenceDirection::Referencers).empty());
    stdfs::remove_all(project);
}

AETHER_TEST(ContentBrowser_Thumbnails) {
    // 4x2: left half black, right half white, into at most 2 px wide.
    std::vector<u8> pixels;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            const u8 v = x < 2 ? 0 : 255;
            pixels.insert(pixels.end(), {v, v, v, 255});
        }
    }
    Thumbnail small = MakeThumbnail(pixels, 4, 2, 2);
    AETHER_CHECK(small.width == 2 && small.height == 1);
    AETHER_CHECK(small.rgba[0] == 0 && small.rgba[4] == 255 && small.rgba[7] == 255);
    Thumbnail same = MakeThumbnail(pixels, 4, 2, 64); // never enlarged
    AETHER_CHECK(same.width == 4 && same.height == 2 && same.rgba == pixels);
    AETHER_CHECK(MakeThumbnail(pixels, 4, 2, 0).rgba.empty() && MakeThumbnail({}, 4, 2, 8).rgba.empty());

    stdfs::path project = ProjectWithContent("aether_test_content_thumbs");
    AssetDatabase db(project / "Content");
    db.Scan();
    ImporterRegistry importers = ImporterRegistry::WithBuiltins();
    DerivedDataCache cache(project / "Intermediate/DDC");
    ImportAll(db, importers, cache);

    std::optional<Thumbnail> texture =
        AssetThumbnail(db, db.FindByPath("textures/checker_a.png")->guid, importers, cache, 16);
    AETHER_CHECK(texture && texture->width == 16 && texture->height == 16 && texture->rgba.size() == 16u * 16u * 4u);
    std::optional<Thumbnail> red =
        AssetThumbnail(db, db.FindByPath("textures/test_solid_red.png")->guid, importers, cache, 8);
    AETHER_CHECK(red && red->rgba[0] == 255 && red->rgba[1] == 0 && red->rgba[2] == 0);

    // A material uses its base colour texture (tinted by the base colour).
    const AssetGuid material = db.FindByPath("models/test_textured_cube.gltf#material:0")->guid;
    std::optional<Thumbnail> material_thumb = AssetThumbnail(db, material, importers, cache, 16);
    AETHER_CHECK(material_thumb && material_thumb->width == 16);

    // Models and meshes need the renderer: no thumbnail here.
    AETHER_CHECK(!AssetThumbnail(db, db.FindByPath("models/test_textured_cube.gltf")->guid, importers, cache, 16));
    AETHER_CHECK(!AssetThumbnail(db, db.FindByPath("models/test_textured_cube.gltf#mesh:0")->guid, importers, cache, 16));
    stdfs::remove_all(project);
}
