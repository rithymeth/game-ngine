#pragma once

#include "aether/core/base.h"
#include "aether/math/vec.h"

#include <filesystem>
#include <map>
#include <string>

namespace aether::editor {

// An axis-aligned box in a model's own space.
struct LocalBounds {
    Vec3 min{-0.5f, -0.5f, -0.5f};
    Vec3 max{0.5f, 0.5f, 0.5f};
};

// The bounding boxes of the glTF models a scene names, read once from the content folder and kept, so a view can pick and outline an object by its real
// extent instead of by its origin. The box covers every mesh of the model (each placed by its node's transform). A model that is missing, unreadable or
// has no geometry has no box (Get is false) and the view falls back to a unit box. The cache is never refreshed on its own: call Clear() when models may
// have changed on disk.
class ModelBoundsCache {
public:
    explicit ModelBoundsCache(std::filesystem::path content_root = {}) : content_root_(std::move(content_root)) {}
    // Where model paths are relative to. Changing it clears the cache.
    void SetContentRoot(std::filesystem::path content_root);
    const std::filesystem::path& ContentRoot() const { return content_root_; }
    void Clear() { cache_.clear(); }
    // The model at `asset_path` (relative to the content root), or false. A negative answer is remembered too, so a missing file is looked for once.
    bool Get(const std::string& asset_path, LocalBounds& out) const;
    usize CachedCount() const { return cache_.size(); }

private:
    struct Entry {
        bool valid = false;
        LocalBounds bounds;
    };
    std::filesystem::path content_root_;
    mutable std::map<std::string, Entry> cache_;
};

} // namespace aether::editor
