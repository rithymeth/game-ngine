#include "aether/assets/asset_manager.h"

#include "aether/assets/image.h"
#include "aether/core/log.h"
#include "aether/gfx/descriptor_heap.h"
#include "aether/gfx/texture.h"

namespace aether::assets {

AssetManager::AssetManager(gfx::Device& device, gfx::DescriptorHeap& bindless_heap)
    : device_(device), bindless_heap_(bindless_heap) {}

AssetManager::~AssetManager() = default;

u32 AssetManager::LoadTexture(const std::string& path, ID3D12GraphicsCommandList* upload_cmd) {
    auto it = path_to_bindless_index_.find(path);
    if (it != path_to_bindless_index_.end()) {
        return it->second;
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
