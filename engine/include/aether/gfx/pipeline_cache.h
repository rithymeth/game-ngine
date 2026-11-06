#pragma once

// Phase 38 step 8: the pipeline cache -- create once per pipeline state, hand the same object back.
//
// A graphics pipeline bundles the two shaders with the blend, depth and raster state and the render
// target formats. Creating one is expensive and a scene asks for the same combination constantly
// (every opaque mesh shares one, every transparent one another), so the state is the key.
//
// The creator is injected and the handle is whatever the backend returns (a `VkPipeline`, a
// `ID3D12PipelineState*`, a test's integer), which keeps the cache out of the backends and lets it be
// tested without a GPU. Templates rather than a void* so a caller cannot put one backend's handle in
// another backend's cache by mistake.

#include "aether/gfx/shader_cache.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace aether::gfx {

// Everything that changes the pipeline object. Render target formats are part of it because a pipeline
// is compiled against them: the same state with a different target layout is a different pipeline.
struct PipelineKey {
    std::string vertex;   // identity of the compiled vertex shader
    std::string fragment; // identity of the compiled fragment shader
    ShaderBackend backend = ShaderBackend::D3D12;
    bool depth_test = true;
    bool depth_write = true;
    bool blend = false;
    std::vector<u32> render_target_formats;

    bool operator<(const PipelineKey& other) const {
        if (backend != other.backend) return backend < other.backend;
        if (vertex != other.vertex) return vertex < other.vertex;
        if (fragment != other.fragment) return fragment < other.fragment;
        if (depth_test != other.depth_test) return depth_test < other.depth_test;
        if (depth_write != other.depth_write) return depth_write < other.depth_write;
        if (blend != other.blend) return blend < other.blend;
        return render_target_formats < other.render_target_formats;
    }
};

template <typename Handle>
class PipelineCache {
public:
    using Creator = std::function<Handle(const PipelineKey&)>;

    explicit PipelineCache(Creator creator) : creator_(std::move(creator)) {}

    // Creates on the first request for a key and returns the stored handle afterwards. A key that
    // produced no handle is cached too, so a pipeline that cannot be created is reported once rather
    // than retried on every draw.
    const Handle& Get(const PipelineKey& key) {
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            ++hits_;
            return found->second;
        }
        ++misses_;
        return entries_.emplace(key, creator_ ? creator_(key) : Handle{}).first->second;
    }

    usize Hits() const { return hits_; }
    usize Misses() const { return misses_; }
    usize Size() const { return entries_.size(); }
    void Clear() {
        entries_.clear();
        hits_ = 0;
        misses_ = 0;
    }

private:
    Creator creator_;
    std::map<PipelineKey, Handle> entries_;
    usize hits_ = 0;
    usize misses_ = 0;
};

} // namespace aether::gfx