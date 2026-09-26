#include "aether/scene/model_assets.h"

#include <cstring>

namespace aether {

ModelAssetResolveResult ResolveModelAssets(World& world, const assets::AssetDatabase& database) {
    ModelAssetResolveResult result;
    world.ForEach<ModelRenderer>([&](ModelRenderer& renderer) {
        const assets::AssetRecord* by_guid = renderer.model.IsSet() ? database.Find(renderer.model.guid) : nullptr;
        if (by_guid != nullptr && by_guid->importer == assets::ModelAsset::kImporter) {
            if (by_guid->path != renderer.asset_path) {
                SetModelPath(renderer, by_guid->path);
                ++result.path_updated;
            }
            return;
        }
        const assets::AssetRecord* by_path = database.FindByPath(renderer.asset_path);
        if (by_path != nullptr && by_path->importer == assets::ModelAsset::kImporter) {
            renderer.model.guid = by_path->guid;
            ++result.linked;
            return;
        }
        ++result.unresolved;
    });
    return result;
}

} // namespace aether
