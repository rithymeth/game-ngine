#include "aether/assets/asset_manager.h"

#include "aether/assets/image.h"
#include "aether/core/log.h"
#include "aether/gfx/descriptor_heap.h"
#include "aether/gfx/device.h"
#include "aether/gfx/texture.h"

namespace aether::assets {

AssetManager::AssetManager(gfx::Device& device, gfx::DescriptorHeap& bindless_heap)
    : device_(device), bindless_heap_(bindless_heap) {}

AssetManager::~AssetManager() = default;

void AssetManager::ReleaseUploadStagingAfterFence(u64 fence_value) {
    if (fence_value == 0) {
        AETHER_LOG_ERROR("Assets", "Cannot release texture upload staging without a submitted fence");
        return;
    }
    device_.WaitForFence(fence_value);
    if (!device_.IsFenceComplete(fence_value)) {
        AETHER_LOG_ERROR("Assets", "Texture upload fence %llu was not submitted on this device",
                         static_cast<unsigned long long>(fence_value));
        return;
    }
    for (const std::unique_ptr<gfx::Texture>& texture : textures_) {
        texture->ReleaseUploadStaging();
    }
}

u32 AssetManager::LoadTexture(const std::string& path, ID3D12GraphicsCommandList* upload_cmd) {
    auto it = path_to_bindless_index_.find(path);
    if (it != path_to_bindless_index_.end()) {
        return it->second;
    }
    if (!bindless_heap_.CanAllocate()) {
        AETHER_LOG_ERROR("Assets", "Cannot load texture \"%s\": bindless descriptor heap is full", path.c_str());
        return gfx::DescriptorHeap::kInvalidIndex;
    }

    ImageData image;
    if (!DecodeImageFile(path, image)) {
        return gfx::DescriptorHeap::kInvalidIndex;
    }

    auto texture =
        std::make_unique<gfx::Texture>(device_, bindless_heap_, upload_cmd, image.width, image.height,
                                        image.pixels.data());
    u32 bindless_index = texture->BindlessIndex();
    textures_.push_back(std::move(texture));
    path_to_bindless_index_.emplace(path, bindless_index);

    AETHER_LOG_INFO("Assets", "Loaded texture \"%s\" (%ux%u, bindless index %u)", path.c_str(), image.width,
                     image.height, bindless_index);
    return bindless_index;
}

} // namespace aether::assets
