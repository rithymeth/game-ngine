#include "aether/gfx/material.h"

#include "aether/assets/asset_manager.h"
#include "aether/assets/gltf_loader.h"

namespace aether::gfx {

MaterialData LoadMaterial(const assets::GltfMaterial& source, assets::AssetManager& assets,
                           ID3D12GraphicsCommandList* upload_cmd) {
    MaterialData result;
    result.base_color[0] = source.base_color[0];
    result.base_color[1] = source.base_color[1];
    result.base_color[2] = source.base_color[2];
    result.base_color[3] = source.base_color[3];
    result.metallic = source.metallic;
    result.roughness = source.roughness;
    result.occlusion_strength = source.occlusion_strength;
    result.alpha_mode = source.alpha_mode;
    result.alpha_cutoff = source.alpha_cutoff;

    if (!source.base_color_texture.empty()) {
        result.base_color_texture = assets.LoadTexture(source.base_color_texture, upload_cmd);
    }
    if (!source.normal_texture.empty()) {
        result.normal_texture = assets.LoadTexture(source.normal_texture, upload_cmd);
    }
    if (!source.metallic_roughness_texture.empty()) {
        result.metallic_roughness_texture = assets.LoadTexture(source.metallic_roughness_texture, upload_cmd);
    }
    if (!source.occlusion_texture.empty()) {
        result.occlusion_texture = assets.LoadTexture(source.occlusion_texture, upload_cmd);
    }

    return result;
}

} // namespace aether::gfx
