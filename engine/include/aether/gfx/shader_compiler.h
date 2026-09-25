#pragma once

#include "aether/core/base.h"

#include <string>
#include <vector>

namespace aether::gfx {

struct ShaderBytecode {
    std::vector<u8> data;
    const void* Data() const { return data.data(); }
    usize Size() const { return data.size(); }
};

// Wraps the classic D3DCompiler (fxc) pipeline: HLSL source text in,
// bytecode out. Throws std::runtime_error (with the compiler's error blob
// logged) on a compile failure. A DXC path (and SPIR-V output for the
// eventual Vulkan backend) is future work; the call site — CompileHLSL(src,
// entry, profile) — is meant to stay stable across that swap.
ShaderBytecode CompileHLSL(const std::string& source, const char* entry_point, const char* target_profile,
                           const char* debug_name = "shader");

} // namespace aether::gfx
