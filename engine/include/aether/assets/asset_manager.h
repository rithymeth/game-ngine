#pragma once

#include "aether/gfx/d3d12_common.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether::gfx {
class Device;
class DescriptorHeap;
class Texture;
} // namespace aether::gfx

namespace aether::assets {

// The first slice of a real asset pipeline: file-path-keyed, cached texture
// loading on top of aether::assets::DecodeImageFile + gfx::Texture. Loading the
// same path twice returns the same bindless index and does not re-decode or
// re-upload — the cache is the whole point, not just a convenience.
//
// Scope, deliberately: this owns *decode + GPU upload + caching*, not a
// general asset database (no reference counting, no unloading, no async
// streaming — see the README's asset pipeline section for what's still
// open). Every loaded Texture is kept alive for the AssetManager's own
// lifetime, matching gfx::Texture's existing "demo-scale" persistent-upload
// contract.
class AssetManager {
public:
    AssetManager(gfx::Device& device, gfx::DescriptorHeap& bindless_heap);
    ~AssetManager();

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    // Loads (or returns the cached bindless index for) the texture at
    // `path`. On a cache miss, decodes the file and records the GPU upload
    // onto `upload_cmd` — the caller submits it and waits on the fence
    // before sampling the texture, same contract as gfx::Texture's own
    // constructor. Returns DescriptorHeap::kInvalidIndex if the file
    // couldn't be loaded (logged via aether::assets::DecodeImageFile).
    u32 LoadTexture(const std::string& path, ID3D12GraphicsCommandList* upload_cmd);

    // Number of distinct textures actually loaded (cache hits don't count
    // again) — mainly for tests to verify the cache is doing its job.
    usize LoadedTextureCount() const { return textures_.size(); }

private:
    gfx::Device& device_;
    gfx::DescriptorHeap& bindless_heap_;
    std::unordered_map<std::string, u32> path_to_bindless_index_;
    std::vector<std::unique_ptr<gfx::Texture>> textures_;
};

} // namespace aether::assets
