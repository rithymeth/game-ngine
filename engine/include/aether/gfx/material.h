#pragma once

#include "aether/gfx/d3d12_common.h"
#include "aether/gfx/descriptor_heap.h"
#include "aether/assets/material.h"

namespace aether::assets {
class AssetManager;
struct GltfMaterial;
} // namespace aether::assets

namespace aether::gfx {

// GPU-ready material data: metallic-roughness PBR parameters plus bindless
// texture indices. This is the engine's own material representation, not
// tied to any one file format — it happens to be built from a glTF
// material below (glTF's pbrMetallicRoughness model maps onto it directly),
// but the struct itself doesn't know that.
//
// A texture field is DescriptorHeap::kInvalidIndex when the material has no
// such texture; shaders check for that and fall back to the corresponding
// factor (base_color / metallic / roughness), matching the glTF spec's own
// "factor is multiplied by the texture sample, or used alone if there's no
// texture" semantics.
//
// Layout matters: this is uploaded as-is via root 32-bit constants (see
// gltf_demo), so field order and size must stay stable — sizeof(MaterialData)
// / 4 is how many DWORDs the caller reserves.
struct MaterialData {
    f32 base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    f32 metallic = 1.0f;
    f32 roughness = 1.0f;
    u32 base_color_texture = DescriptorHeap::kInvalidIndex;
    u32 normal_texture = DescriptorHeap::kInvalidIndex;
    u32 metallic_roughness_texture = DescriptorHeap::kInvalidIndex;
    u32 occlusion_texture = DescriptorHeap::kInvalidIndex;
    f32 occlusion_strength = 1.0f;
    assets::MaterialAlphaMode alpha_mode = assets::MaterialAlphaMode::Opaque;
    f32 alpha_cutoff = 0.5f;
    u32 _pad1 = 0;
};

// Resolves a glTF material's texture references through `assets` (which
// caches by path — a texture referenced by multiple materials, or already
// loaded for another purpose, is decoded/uploaded at most once) and returns
// GPU-ready MaterialData with real bindless indices. Records any needed
// texture uploads onto `upload_cmd`, same contract as
// AssetManager::LoadTexture/gfx::Texture — the caller submits and waits on
// it before the material's textures are sampled.
MaterialData LoadMaterial(const assets::GltfMaterial& source, assets::AssetManager& assets,
                           ID3D12GraphicsCommandList* upload_cmd);

} // namespace aether::gfx
