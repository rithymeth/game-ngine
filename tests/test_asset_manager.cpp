#include "aether/assets/asset_manager.h"
#include "aether/gfx/command_list.h"
#include "aether/gfx/descriptor_heap.h"
#include "aether/gfx/device.h"
#include "aether/platform/filesystem.h"
#include "test_framework.h"

#include <filesystem>
#include <string>

using namespace aether;
using namespace aether::gfx;
using namespace aether::assets;

namespace {

// Same embedded 4x4 solid-red PNG as test_assets.cpp, duplicated locally so
// this file has no cross-file dependency on it.
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

// A second, differently-sized PNG (2x2) so tests can tell "two different
// files" apart from "the same file loaded twice".
constexpr u8 kCornersPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d,
    0x24, 0x00, 0x00, 0x00, 0x01, 0x73, 0x52, 0x47, 0x42, 0x00, 0xae, 0xce, 0x1c, 0xe9, 0x00, 0x00,
    0x00, 0x04, 0x67, 0x41, 0x4d, 0x41, 0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc, 0x61, 0x05, 0x00, 0x00,
    0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0e, 0xc3, 0x00, 0x00, 0x0e, 0xc3, 0x01, 0xc7,
    0x6f, 0xa8, 0x64, 0x00, 0x00, 0x00, 0x16, 0x49, 0x44, 0x41, 0x54, 0x18, 0x57, 0x63, 0xf8, 0xcf,
    0xc0, 0xf0, 0x1f, 0x0c, 0x19, 0x18, 0xfe, 0xff, 0xff, 0x0f, 0x64, 0x00, 0x00, 0x47, 0xca, 0x08,
    0xf8, 0x26, 0x7b, 0x18, 0x99, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60,
    0x82,
};

std::string WriteTempPng(const char* filename, const u8* data, usize size) {
    std::string path = (std::filesystem::temp_directory_path() / filename).string();
    AETHER_CHECK(fs::WriteFileBytes(path, data, size));
    return path;
}

} // namespace

AETHER_TEST(AssetManager_LoadingSamePathTwiceReturnsCachedIndexWithoutReupload) {
    Device device(/*enable_debug_layer=*/false);
    DescriptorHeap bindless_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, /*shader_visible=*/true);
    AssetManager assets(device, bindless_heap);

    std::string path = WriteTempPng("aether_asset_manager_red.png", kSolidRedPng, sizeof(kSolidRedPng));

    CommandList cmd(device);
    cmd.Reset();
    u32 first_index = assets.LoadTexture(path, cmd.Get());
    AETHER_CHECK(first_index != DescriptorHeap::kInvalidIndex);
    AETHER_CHECK(assets.LoadedTextureCount() == 1);

    u32 second_index = assets.LoadTexture(path, cmd.Get());
    AETHER_CHECK(second_index == first_index);
    AETHER_CHECK(assets.LoadedTextureCount() == 1); // no re-decode/re-upload on the cache hit

    cmd.Close();
    ID3D12CommandList* lists[] = {cmd.Get()};
    device.WaitForFence(device.Submit(lists, 1));

    std::filesystem::remove(path);
}

AETHER_TEST(AssetManager_DifferentPathsGetDistinctIndices) {
    Device device(/*enable_debug_layer=*/false);
    DescriptorHeap bindless_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, /*shader_visible=*/true);
    AssetManager assets(device, bindless_heap);

    std::string red_path = WriteTempPng("aether_asset_manager_red2.png", kSolidRedPng, sizeof(kSolidRedPng));
    std::string corners_path = WriteTempPng("aether_asset_manager_corners.png", kCornersPng, sizeof(kCornersPng));

    CommandList cmd(device);
    cmd.Reset();
    u32 red_index = assets.LoadTexture(red_path, cmd.Get());
    u32 corners_index = assets.LoadTexture(corners_path, cmd.Get());
    AETHER_CHECK(red_index != DescriptorHeap::kInvalidIndex);
    AETHER_CHECK(corners_index != DescriptorHeap::kInvalidIndex);
    AETHER_CHECK(red_index != corners_index);
    AETHER_CHECK(assets.LoadedTextureCount() == 2);

    cmd.Close();
    ID3D12CommandList* lists[] = {cmd.Get()};
    device.WaitForFence(device.Submit(lists, 1));

    std::filesystem::remove(red_path);
    std::filesystem::remove(corners_path);
}

AETHER_TEST(AssetManager_LoadTextureMissingFileReturnsInvalidIndex) {
    Device device(/*enable_debug_layer=*/false);
    DescriptorHeap bindless_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, /*shader_visible=*/true);
    AssetManager assets(device, bindless_heap);

    CommandList cmd(device);
    cmd.Reset();
    u32 index = assets.LoadTexture("does/not/exist.png", cmd.Get());
    AETHER_CHECK(index == DescriptorHeap::kInvalidIndex);
    AETHER_CHECK(assets.LoadedTextureCount() == 0);
    cmd.Close();
}
