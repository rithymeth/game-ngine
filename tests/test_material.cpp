#include "aether/assets/asset_manager.h"
#include "aether/assets/gltf_loader.h"
#include "aether/gfx/command_list.h"
#include "aether/gfx/descriptor_heap.h"
#include "aether/gfx/device.h"
#include "aether/gfx/material.h"
#include "aether/platform/filesystem.h"
#include "test_framework.h"

#include <filesystem>
#include <string>

using namespace aether;
using namespace aether::gfx;
using namespace aether::assets;

namespace {

// Same embedded 4x4 solid-red PNG used elsewhere (test_asset_manager.cpp),
// duplicated locally so this file has no cross-file dependency on it.
constexpr u8 kSolidRedPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x08, 0x06, 0x00, 0x00, 0x00, 0xa9, 0xf1, 0x9e,
    0x7e, 0x00, 0x00, 0x00, 0x01, 0x73, 0x52, 0x47, 0x42, 0x00, 0xae, 0xce, 0x1c, 0xe9, 0x00, 0x00,
    0x00, 0x04, 0x67, 0x41, 0x4d, 0x41, 0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc, 0x61, 0x05, 0x00, 0x00,
    0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0e, 0xc3, 0x00, 0x00, 0x0e, 0xc3, 0x01, 0xc7,
    0x6f, 0xa8, 0x64, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41, 0x54, 0x18, 0x57, 0x63, 0xf8, 0xcf,
    0xc0, 0xf0, 0x1f, 0x19, 0x33, 0x90, 0x2e, 0x00, 0x00, 0x3c, 0x40, 0x1f, 0xe1, 0x55, 0x0a, 0x05,
    0xe2, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

std::string WriteTempPng(const char* filename) {
    std::string path = (std::filesystem::temp_directory_path() / filename).string();
    fs::WriteFileBytes(path, kSolidRedPng, sizeof(kSolidRedPng));
    return path;
}

} // namespace

AETHER_TEST(Material_LoadMaterialCopiesFactorsAndResolvesTexture) {
    Device device(/*enable_debug_layer=*/false);
    DescriptorHeap bindless_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, /*shader_visible=*/true);
    AssetManager assets(device, bindless_heap);

    std::string texture_path = WriteTempPng("aether_material_test.png");

    GltfMaterial source;
    source.base_color[0] = 0.1f;
    source.base_color[1] = 0.2f;
    source.base_color[2] = 0.3f;
    source.base_color[3] = 1.0f;
    source.metallic = 0.5f;
    source.roughness = 0.7f;
    source.base_color_texture = texture_path;

    CommandList cmd(device);
    cmd.Reset();
    MaterialData material = LoadMaterial(source, assets, cmd.Get());
    cmd.Close();
    ID3D12CommandList* lists[] = {cmd.Get()};
    device.WaitForFence(device.Submit(lists, 1));

    AETHER_CHECK(material.base_color[0] == 0.1f);
    AETHER_CHECK(material.base_color[1] == 0.2f);
    AETHER_CHECK(material.base_color[2] == 0.3f);
    AETHER_CHECK(material.base_color[3] == 1.0f);
    AETHER_CHECK(material.metallic == 0.5f);
    AETHER_CHECK(material.roughness == 0.7f);
    AETHER_CHECK(material.base_color_texture != DescriptorHeap::kInvalidIndex);
    AETHER_CHECK(material.normal_texture == DescriptorHeap::kInvalidIndex);
    AETHER_CHECK(material.metallic_roughness_texture == DescriptorHeap::kInvalidIndex);

    std::filesystem::remove(texture_path);
}

AETHER_TEST(Material_SharedTexturePathAcrossMaterialsIsCachedOnce) {
    Device device(/*enable_debug_layer=*/false);
    DescriptorHeap bindless_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, /*shader_visible=*/true);
    AssetManager assets(device, bindless_heap);

    std::string texture_path = WriteTempPng("aether_material_shared.png");

    GltfMaterial source_a;
    source_a.base_color_texture = texture_path;
    GltfMaterial source_b;
    source_b.base_color_texture = texture_path;

    CommandList cmd(device);
    cmd.Reset();
    MaterialData material_a = LoadMaterial(source_a, assets, cmd.Get());
    MaterialData material_b = LoadMaterial(source_b, assets, cmd.Get());
    cmd.Close();
    ID3D12CommandList* lists[] = {cmd.Get()};
    device.WaitForFence(device.Submit(lists, 1));

    AETHER_CHECK(material_a.base_color_texture != DescriptorHeap::kInvalidIndex);
    AETHER_CHECK(material_a.base_color_texture == material_b.base_color_texture);
    AETHER_CHECK(assets.LoadedTextureCount() == 1);

    std::filesystem::remove(texture_path);
}

AETHER_TEST(Material_NoTexturesLeavesAllIndicesInvalid) {
    Device device(/*enable_debug_layer=*/false);
    DescriptorHeap bindless_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, /*shader_visible=*/true);
    AssetManager assets(device, bindless_heap);

    GltfMaterial source;
    source.metallic = 1.0f;
    source.roughness = 1.0f;

    CommandList cmd(device);
    cmd.Reset();
    MaterialData material = LoadMaterial(source, assets, cmd.Get());
    cmd.Close();

    AETHER_CHECK(material.base_color_texture == DescriptorHeap::kInvalidIndex);
    AETHER_CHECK(material.normal_texture == DescriptorHeap::kInvalidIndex);
    AETHER_CHECK(material.metallic_roughness_texture == DescriptorHeap::kInvalidIndex);
    AETHER_CHECK(assets.LoadedTextureCount() == 0);
}
