#include "aether/assets/asset_ref.h"
#include "aether/assets/gltf_loader.h"
#include "aether/assets/model_importer.h"
#include "aether/reflection/serialize.h"
#include "test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace aether;
using namespace aether::assets;
namespace stdfs = std::filesystem;

namespace {

const stdfs::path kRepoAssets = stdfs::path(AETHER_REPO_ASSETS_DIR);

// A project whose Content/ holds copies of the repo's models/ and textures/.
stdfs::path ProjectWithModels(const char* name) {
    stdfs::path dir = stdfs::temp_directory_path() / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir / "Content");
    stdfs::copy(kRepoAssets / "models", dir / "Content/models", stdfs::copy_options::recursive);
    stdfs::copy(kRepoAssets / "textures", dir / "Content/textures", stdfs::copy_options::recursive);
    return dir;
}

ImportResult ImportRepoModel(const char* file, const nlohmann::json& settings = ModelImporter().DefaultSettings()) {
    AssetRecord record;
    record.path = std::string("models/") + file;
    return ModelImporter().Import({record, kRepoAssets / "models" / file, settings});
}

const SubAssetOutput* FindSub(const std::vector<SubAssetOutput>& subs, const std::string& key) {
    for (const SubAssetOutput& sub : subs) {
        if (sub.key == key) {
            return &sub;
        }
    }
    return nullptr;
}

} // namespace

AETHER_TEST(MeshData_RoundTripsAndRejectsBadInput) {
    MeshData mesh;
    MeshPrimitiveData primitive;
    primitive.vertices = {{{0, 0, 0}, {0, 1, 0}, {0, 0}}, {{1, 2, 3}, {0, 1, 0}, {1, 1}}};
    primitive.indices = {0, 1, 0};
    primitive.material = 2;
    primitive.joints = {{0, 1, 0, 0}, {1, 0, 0, 0}};
    primitive.weights = {{0.5f, 0.5f, 0, 0}, {1, 0, 0, 0}};
    primitive.bounds_max = Vec3{1, 2, 3};
    mesh.primitives = {primitive, MeshPrimitiveData{}};

    std::vector<u8> bytes = EncodeMeshData(mesh);
    MeshData back;
    AETHER_CHECK(DecodeMeshData(bytes, back));
    AETHER_CHECK(back.primitives.size() == 2 && back.primitives[1].vertices.empty());
    const MeshPrimitiveData& p = back.primitives[0];
    AETHER_CHECK(p.vertices.size() == 2 && p.vertices[1].position[2] == 3.0f && p.vertices[1].uv[0] == 1.0f);
    AETHER_CHECK(p.indices == primitive.indices && p.material == 2);
    AETHER_CHECK(p.joints == primitive.joints && p.weights == primitive.weights);
    AETHER_CHECK(p.bounds_max.y == 2.0f && back.primitives[1].material == -1);

    std::vector<u8> truncated(bytes.begin(), bytes.end() - 1);
    AETHER_CHECK(!DecodeMeshData(truncated, back));
    std::vector<u8> trailing = bytes;
    trailing.push_back(0);
    AETHER_CHECK(!DecodeMeshData(trailing, back));
    std::vector<u8> huge_count = bytes;
    huge_count[8] = 0xff; // primitive count far beyond the data
    AETHER_CHECK(!DecodeMeshData(huge_count, back));
    AETHER_CHECK(!DecodeMeshData({'A', 'M', 'S'}, back));
}

AETHER_TEST(GltfMemoryLoader_ParsesBoundedDataAndRejectsExternalUris) {
    const auto parse = [](const std::string& json, GltfScene& scene) {
        return LoadGltfFromMemory(std::span<const u8>(reinterpret_cast<const u8*>(json.data()), json.size()), scene);
    };
    GltfScene scene;
    AETHER_CHECK(parse(R"({"asset":{"version":"2.0"}})", scene));
    AETHER_CHECK(!parse(R"({"asset":{"version":"2.0"},"buffers":[{"uri":"../../private.bin","byteLength":1}]})", scene));
    AETHER_CHECK(!parse(R"({"asset":{"version":"2.0"},"materials":[{"pbrMetallicRoughness":{"baseColorFactor":"bad"}}]})", scene));
}

AETHER_TEST(ModelImporter_SplitsATexturedCube) {
    ImportResult result = ImportRepoModel("test_textured_cube.gltf");
    AETHER_CHECK(result.ok && result.warnings.empty());

    ModelData model;
    AETHER_CHECK(reflect::LoadBinary(model, result.data));
    AETHER_CHECK(model.meshes == std::vector<std::string>{"mesh:0"});
    AETHER_CHECK(model.materials == std::vector<std::string>{"material:0"});
    AETHER_CHECK(model.animations.empty() && model.nodes.empty()); // this file has no node tree

    const SubAssetOutput* mesh_sub = FindSub(result.sub_assets, "mesh:0");
    const SubAssetOutput* material_sub = FindSub(result.sub_assets, "material:0");
    AETHER_CHECK(mesh_sub != nullptr && mesh_sub->importer == "Mesh");
    AETHER_CHECK(material_sub != nullptr && material_sub->importer == "Material");
    AETHER_CHECK(result.sub_assets.size() == 2);

    MeshData mesh;
    AETHER_CHECK(DecodeMeshData(mesh_sub->data, mesh));
    AETHER_CHECK(mesh.primitives.size() == 1);
    const MeshPrimitiveData& cube = mesh.primitives[0];
    AETHER_CHECK(cube.vertices.size() == 24 && cube.indices.size() == 36 && cube.material == 0);
    AETHER_CHECK(cube.joints.empty() && cube.weights.empty());
    // A unit cube centred on the origin.
    AETHER_CHECK(cube.bounds_min.x < 0.0f && cube.bounds_max.x > 0.0f);
    AETHER_CHECK(cube.bounds_max.x - cube.bounds_min.x == cube.bounds_max.y - cube.bounds_min.y);

    // The texture URI ("../textures/checker_a.png" from the .gltf) becomes a
    // content-relative asset path.
    MaterialData material;
    AETHER_CHECK(reflect::LoadBinary(material, material_sub->data));
    AETHER_CHECK(material.base_color_texture == "textures/checker_a.png");
    AETHER_CHECK(material.normal_texture.empty());
}

AETHER_TEST(ModelImporter_AnimationsSkinsAndSettings) {
    ImportResult animated = ImportRepoModel("test_animation.gltf");
    AETHER_CHECK(animated.ok);
    const SubAssetOutput* clip = FindSub(animated.sub_assets, "animation:0");
    AETHER_CHECK(clip != nullptr && clip->importer == "Animation");
    AnimationData animation;
    AETHER_CHECK(reflect::LoadBinary(animation, clip->data));
    AETHER_CHECK(animation.duration > 0.0f && !animation.channels.empty());
    for (const AnimationChannelData& channel : animation.channels) {
        const usize width = channel.path == AnimationPath::Rotation ? 4 : 3;
        AETHER_CHECK(!channel.times.empty() && channel.values.size() == channel.times.size() * width);
    }

    ImportResult skinned = ImportRepoModel("test_skinned_ribbon.gltf");
    AETHER_CHECK(skinned.ok);
    ModelData model;
    AETHER_CHECK(reflect::LoadBinary(model, skinned.data));
    AETHER_CHECK(model.skins.size() == 1);
    AETHER_CHECK(model.skins[0].joints.size() == model.skins[0].inverse_bind_matrices.size());
    MeshData ribbon;
    AETHER_CHECK(DecodeMeshData(FindSub(skinned.sub_assets, "mesh:0")->data, ribbon));
    AETHER_CHECK(ribbon.primitives[0].joints.size() == ribbon.primitives[0].vertices.size());

    // The node tree is kept as authored.
    ImportResult tree = ImportRepoModel("test_scene.gltf");
    ModelData scene_model;
    AETHER_CHECK(tree.ok && reflect::LoadBinary(scene_model, tree.data));
    AETHER_CHECK(scene_model.nodes.size() == 4 && scene_model.root_nodes == std::vector<u32>{0});
    AETHER_CHECK(!scene_model.nodes[0].children.empty());

    // Settings drop the optional sub-assets.
    ImportResult bare = ImportRepoModel("test_animation.gltf", {{"import_materials", false}, {"import_animations", false}});
    AETHER_CHECK(bare.ok && FindSub(bare.sub_assets, "mesh:0") != nullptr);
    AETHER_CHECK(FindSub(bare.sub_assets, "animation:0") == nullptr && FindSub(bare.sub_assets, "material:0") == nullptr);
    MeshData without_material;
    AETHER_CHECK(DecodeMeshData(FindSub(bare.sub_assets, "mesh:0")->data, without_material));
    AETHER_CHECK(without_material.primitives[0].material == -1);

    ImportResult tolerant = ImportRepoModel("test_textured_cube.gltf", {{"import_animations", "no"}});
    AETHER_CHECK(tolerant.ok && tolerant.warnings.size() == 1);
    AETHER_CHECK(!ImportRepoModel("does_not_exist.gltf").ok);
}

AETHER_TEST(ModelImport_SubAssetsGetStableGuids) {
    stdfs::path project = ProjectWithModels("aether_test_model_subassets");
    AssetDatabase db(project / "Content");
    db.Scan();
    ImporterRegistry importers = ImporterRegistry::WithBuiltins();
    DerivedDataCache cache(project / "Intermediate/DDC");
    ImportAllResult all = ImportAll(db, importers, cache);
    AETHER_CHECK(all.failed == 0 && all.skipped == 0);
    AETHER_CHECK(all.imported == 4 + 4); // 4 models, 4 textures

    const AssetRecord* cube = db.FindByPath("models/test_textured_cube.gltf");
    AETHER_CHECK(cube != nullptr && cube->sub_assets.size() == 2);
    const AssetGuid cube_guid = cube->guid;
    const AssetRecord* mesh = db.FindByPath("models/test_textured_cube.gltf#mesh:0");
    AETHER_CHECK(mesh != nullptr && mesh->importer == "Mesh" && mesh->parent == cube_guid);
    AETHER_CHECK(mesh->sub_key == "mesh:0" && !mesh->needs_import && mesh->IsSubAsset());
    const AssetGuid mesh_guid = mesh->guid;
    AETHER_CHECK(db.SourcePath(mesh_guid) == db.SourcePath(cube_guid));
    AETHER_CHECK(db.FindByPath("models/test_animation.gltf#animation:0")->importer == "Animation");

    // Importing a sub-asset gives just its data.
    ImportOutput mesh_out = ImportAsset(db, mesh_guid, importers, cache);
    MeshData mesh_data;
    AETHER_CHECK(mesh_out.ok && mesh_out.from_cache && DecodeMeshData(mesh_out.data, mesh_data));
    AETHER_CHECK(mesh_out.sub_assets.empty());

    // A fresh session (new database, same files) finds the same GUIDs in the
    // .ameta, and a cached reimport keeps them.
    AssetDatabase reopened(project / "Content");
    reopened.Scan();
    AETHER_CHECK(reopened.FindByPath("models/test_textured_cube.gltf#mesh:0")->guid == mesh_guid);
    AETHER_CHECK(reopened.Find(mesh_guid)->parent == cube_guid);
    ImportOutput again = ImportAsset(reopened, cube_guid, importers, cache);
    AETHER_CHECK(again.ok && again.from_cache && again.sub_assets.size() == 2);
    AETHER_CHECK(FindSub(again.sub_assets, "mesh:0")->guid == mesh_guid);

    // Turning materials off removes that sub-asset (and only that one).
    AssetMeta meta;
    AETHER_CHECK(LoadAssetMeta(reopened.MetaPath(cube_guid), meta) && meta.sub_assets.size() == 2);
    meta.settings["import_materials"] = false;
    AETHER_CHECK(SaveAssetMeta(reopened.MetaPath(cube_guid), meta));
    ImportOutput fewer = ImportAsset(reopened, cube_guid, importers, cache);
    AETHER_CHECK(fewer.ok && !fewer.from_cache && fewer.sub_assets.size() == 1);
    AETHER_CHECK(reopened.FindByPath("models/test_textured_cube.gltf#material:0") == nullptr);
    AETHER_CHECK(reopened.Find(mesh_guid) != nullptr && reopened.Find(cube_guid)->sub_assets.size() == 1);
    AETHER_CHECK(LoadAssetMeta(reopened.MetaPath(cube_guid), meta) && meta.sub_assets.size() == 1);
    stdfs::remove_all(project);
}

AETHER_TEST(ModelImport_MoveDeleteAndCopyWithSubAssets) {
    stdfs::path project = ProjectWithModels("aether_test_model_move");
    AssetDatabase db(project / "Content");
    db.Scan();
    ImporterRegistry importers = ImporterRegistry::WithBuiltins();
    DerivedDataCache cache(project / "Intermediate/DDC");
    ImportAll(db, importers, cache);
    const AssetGuid cube = db.FindByPath("models/test_textured_cube.gltf")->guid;
    const AssetGuid mesh = db.FindByPath("models/test_textured_cube.gltf#mesh:0")->guid;

    // Sub-assets can't be moved or deleted on their own.
    std::string error;
    AETHER_CHECK(!db.Move(mesh, "elsewhere.gltf", &error) && !error.empty());
    AETHER_CHECK(!db.Delete(mesh, true, &error));
    AETHER_CHECK(!db.MarkImported(mesh, &error));
    AETHER_CHECK(!db.Move(cube, "models/a#b.gltf", &error));

    // Moving the source takes its sub-assets along (the .bin it needs too, so
    // it still imports).
    stdfs::create_directories(project / "Content/props");
    stdfs::copy_file(project / "Content/models/test_cube.bin", project / "Content/props/test_cube.bin");
    AETHER_CHECK(db.Move(cube, "props/cube.gltf", &error));
    AETHER_CHECK(db.FindByPath("props/cube.gltf#mesh:0")->guid == mesh);
    AETHER_CHECK(db.FindByPath("models/test_textured_cube.gltf#mesh:0") == nullptr);
    AssetDatabase reopened(project / "Content");
    reopened.Scan();
    AETHER_CHECK(reopened.FindByPath("props/cube.gltf#mesh:0")->guid == mesh);

    // A scene referring to the mesh blocks deleting the model.
    std::ofstream(project / "Content/level.ascene") << "{\"mesh\": \"" << ToString(mesh) << "\"}";
    reopened.Scan();
    AETHER_CHECK(reopened.Find(reopened.FindByPath("level.ascene")->guid)->dependencies ==
                 std::vector<AssetGuid>{mesh});
    AETHER_CHECK(!reopened.Delete(cube, false, &error) && error.find("level.ascene") != std::string::npos);
    AETHER_CHECK(reopened.Delete(cube, true, &error));
    AETHER_CHECK(reopened.Find(mesh) == nullptr && reopened.FindByPath("props/cube.gltf#material:0") == nullptr);
    AETHER_CHECK(reopened.Find(reopened.FindByPath("level.ascene")->guid)->dependencies.empty());
    AETHER_CHECK(!stdfs::exists(project / "Content/props/cube.gltf.ameta"));

    // Copying a model together with its .ameta: the copy gets new GUIDs for
    // the model and each sub-asset.
    const AssetGuid ribbon_mesh = reopened.FindByPath("models/test_skinned_ribbon.gltf#mesh:0")->guid;
    stdfs::copy_file(project / "Content/models/test_skinned_ribbon.gltf", project / "Content/models/copy.gltf");
    stdfs::copy_file(project / "Content/models/test_skinned_ribbon.gltf.ameta",
                     project / "Content/models/copy.gltf.ameta");
    stdfs::last_write_time(project / "Content/models/copy.gltf.ameta",
                           stdfs::last_write_time(project / "Content/models/test_skinned_ribbon.gltf.ameta") +
                               std::chrono::seconds(5));
    ScanResult scan = reopened.Scan();
    AETHER_CHECK(scan.duplicates_fixed >= 2);
    AETHER_CHECK(reopened.FindByPath("models/test_skinned_ribbon.gltf#mesh:0")->guid == ribbon_mesh);
    const AssetRecord* copy_mesh = reopened.FindByPath("models/copy.gltf#mesh:0");
    AETHER_CHECK(copy_mesh != nullptr && copy_mesh->guid != ribbon_mesh);
    AETHER_CHECK(copy_mesh->parent == reopened.FindByPath("models/copy.gltf")->guid);

    // Typed references to sub-assets.
    AssetRef<MeshAsset> ref{mesh};
    AETHER_CHECK(std::string(reflect::Reflect<AssetRef<MeshAsset>>().asset_type) == "Mesh" && ref.IsSet());
    stdfs::remove_all(project);
}
