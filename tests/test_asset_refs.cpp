#include "aether/assets/asset_database.h"
#include "aether/assets/asset_ref.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/model_assets.h"
#include "aether/scene/serialization.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace aether;
using namespace aether::assets;
namespace stdfs = std::filesystem;

namespace refs_test {
struct Decal {
    AssetRef<TextureAsset> texture;
    f32 size = 1.0f;
};
} // namespace refs_test

AETHER_REFLECT(refs_test::Decal, 1, AETHER_FIELD(texture, Field_EditAnywhere), AETHER_FIELD(size, Field_EditAnywhere))

namespace {

stdfs::path FreshContent(const char* name) {
    stdfs::path dir = stdfs::temp_directory_path() / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    return dir;
}

void Write(const stdfs::path& file, const std::string& text) {
    stdfs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

} // namespace

AETHER_TEST(AssetRef_ReflectsAsGuidString) {
    const reflect::TypeInfo& type = reflect::Reflect<AssetRef<TextureAsset>>();
    AETHER_CHECK(std::string(type.name) == "AssetRef<Texture>");
    AETHER_CHECK(std::string(type.asset_type) == "Texture");
    AETHER_CHECK(&reflect::Reflect<AssetRef<ModelAsset>>() != &type); // distinct types per asset kind

    refs_test::Decal decal;
    AETHER_CHECK(reflect::ToJson(decal)["texture"] == ""); // unset -> ""
    decal.texture.guid = NewAssetGuid();
    reflect::Json json = reflect::ToJson(decal);
    AETHER_CHECK(json["texture"] == ToString(decal.texture.guid));

    refs_test::Decal loaded;
    reflect::LoadReport report;
    AETHER_CHECK(reflect::FromJson(loaded, json, &report) && report.warnings.empty());
    AETHER_CHECK(loaded.texture == decal.texture);
    AETHER_CHECK(reflect::FromJson(loaded, reflect::Json{{"texture", ""}}) && !loaded.texture.IsSet());
    AETHER_CHECK(reflect::FromJson(loaded, reflect::Json{{"texture", "garbage"}}, &report));
    AETHER_CHECK(report.warnings.size() == 1);
}

AETHER_TEST(AssetDatabase_TracksWhichScenesUseAnAsset) {
    stdfs::path root = FreshContent("aether_test_content_refs");
    Write(root / "Textures/splat.png", "pixels");
    Write(root / "Textures/unused.png", "pixels");
    AssetDatabase db(root);
    db.Scan();
    AssetGuid splat = db.FindByPath("Textures/splat.png")->guid;

    // One JSON scene and one binary scene both use the texture.
    World world;
    world.CreateEntity(refs_test::Decal{AssetRef<TextureAsset>{splat}, 2.0f});
    stdfs::create_directories(root / "Maps");
    AETHER_CHECK(SaveSceneJson(world, (root / "Maps/a.ascene").string()));
    AETHER_CHECK(SaveScene(world, (root / "Maps/b.aesc").string()));
    // Entity GUIDs share the format but aren't assets: they must not count.
    World other;
    other.CreateEntity(IdComponent{NewEntityGuid()});
    AETHER_CHECK(SaveSceneJson(other, (root / "Maps/c.ascene").string()));

    db.Scan();
    std::vector<const AssetRecord*> users = db.Referencers(splat);
    AETHER_CHECK(users.size() == 2);
    AETHER_CHECK(users.size() == 2 && users[0]->path == "Maps/a.ascene" && users[1]->path == "Maps/b.aesc");
    AETHER_CHECK(db.FindByPath("Maps/a.ascene") && db.FindByPath("Maps/a.ascene")->dependencies.size() == 1);
    AETHER_CHECK(db.FindByPath("Maps/c.ascene") && db.FindByPath("Maps/c.ascene")->dependencies.empty());
    AETHER_CHECK(db.Referencers(db.FindByPath("Textures/unused.png")->guid).empty());

    // Renaming the texture doesn't touch the scenes: they refer to the GUID.
    std::string error;
    AETHER_CHECK(db.Move(splat, "Textures/Decals/splat.png", &error));
    db.Scan();
    AETHER_CHECK(db.Referencers(splat).size() == 2);

    // Deleting a used asset is refused (naming the users) unless forced.
    AETHER_CHECK(!db.Delete(splat, /*force=*/false, &error));
    AETHER_CHECK(error.find("Maps/a.ascene") != std::string::npos);
    AETHER_CHECK(stdfs::exists(root / "Textures/Decals/splat.png"));
    AETHER_CHECK(db.Delete(db.FindByPath("Textures/unused.png")->guid, false, &error)); // unused: fine
    AETHER_CHECK(!stdfs::exists(root / "Textures/unused.png") && !stdfs::exists(root / "Textures/unused.png.ameta"));
    AETHER_CHECK(db.Delete(splat, /*force=*/true, &error));
    AETHER_CHECK(db.Find(splat) == nullptr && !stdfs::exists(root / "Textures/Decals/splat.png"));
    stdfs::remove_all(root);
}

AETHER_TEST(ModelAssets_ResolveLinksPathsAndFollowsRenames) {
    stdfs::path root = FreshContent("aether_test_content_models");
    Write(root / "models/crate.gltf", "{}");
    AssetDatabase db(root);
    db.Scan();
    AssetGuid crate = db.FindByPath("models/crate.gltf")->guid;

    // Old data: only a path.
    World world;
    ModelRenderer renderer;
    SetModelPath(renderer, "models/crate.gltf");
    Entity e = world.CreateEntity(renderer);
    ModelRenderer lost;
    SetModelPath(lost, "models/nowhere.gltf");
    world.CreateEntity(lost);

    ModelAssetResolveResult first = ResolveModelAssets(world, db);
    AETHER_CHECK(first.linked == 1 && first.unresolved == 1 && first.path_updated == 0);
    AETHER_CHECK(world.GetComponent<ModelRenderer>(e)->model.guid == crate);

    // The file is renamed: the GUID finds it and the path follows.
    std::string error;
    AETHER_CHECK(db.Move(crate, "models/props/wooden_crate.gltf", &error));
    ModelAssetResolveResult second = ResolveModelAssets(world, db);
    AETHER_CHECK(second.path_updated == 1);
    AETHER_CHECK(std::string(world.GetComponent<ModelRenderer>(e)->asset_path) == "models/props/wooden_crate.gltf");
    AETHER_CHECK(ResolveModelAssets(world, db).path_updated == 0); // stable

    // A GUID that points at a non-model asset isn't trusted.
    Write(root / "t.png", "x");
    db.Scan();
    world.GetComponent<ModelRenderer>(e)->model.guid = db.FindByPath("t.png")->guid;
    ResolveModelAssets(world, db);
    AETHER_CHECK(world.GetComponent<ModelRenderer>(e)->model.guid == crate); // re-linked from its path
    stdfs::remove_all(root);
}
