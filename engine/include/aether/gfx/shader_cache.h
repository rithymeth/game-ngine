#pragma once

// Phase 38 step 8: the shader cache -- compile once per (source, entry point, profile, backend).
//
// Shader compilation is the slowest thing a renderer does at startup, and the same shader is asked
// for from several places: a material, a shadow pass, a depth-only variant of the same program. This
// keeps the compiled bytecode and hands it back.
//
// The compiler is injected rather than called directly, so the cache is testable without a working
// DXC: a fake counts how many times it was asked. It also means a caller can put a disk cache or an
// offline compile step behind the same interface.

#include "aether/core/profiler.h"
#include "aether/gfx/shader_compiler.h"

#include <functional>
#include <map>
#include <string>

namespace aether::gfx {

enum class ShaderBackend {
    D3D12,  // DXIL through D3DCompile
    Vulkan, // SPIR-V through DXC
};

// Everything that changes the compiled bytes. Anything left out of this would be two different
// shaders sharing one cache entry, which shows up as the wrong program drawing -- so the key is the
// whole request, compared exactly rather than hashed.
struct ShaderKey {
    std::string source;
    std::string entry_point;
    std::string profile;
    ShaderBackend backend = ShaderBackend::D3D12;

    bool operator<(const ShaderKey& other) const {
        if (backend != other.backend) return backend < other.backend;
        if (entry_point != other.entry_point) return entry_point < other.entry_point;
        if (profile != other.profile) return profile < other.profile;
        return source < other.source;
    }
};

class ShaderCache {
public:
    using Compiler = std::function<ShaderBytecode(const ShaderKey&)>;

    explicit ShaderCache(Compiler compiler) : compiler_(std::move(compiler)) {}

    // Compiles on the first request for a key and returns the stored result afterwards. A compile
    // that produced no bytes is cached too: a broken shader stays broken, and retrying it on every
    // request would turn one error into a log flood.
    const ShaderBytecode& Get(const ShaderKey& key) {
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            ++hits_;
            return found->second;
        }
        ++misses_;
        AETHER_PROFILE_ZONE("gfx.shader_compile");
        return entries_.emplace(key, compiler_ ? compiler_(key) : ShaderBytecode{}).first->second;
    }

    usize Hits() const { return hits_; }
    usize Misses() const { return misses_; }
    usize Size() const { return entries_.size(); }
    void Clear() { entries_.clear(); }

private:
    Compiler compiler_;
    std::map<ShaderKey, ShaderBytecode> entries_;
    usize hits_ = 0;
    usize misses_ = 0;
};

} // namespace aether::gfx