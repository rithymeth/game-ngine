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

// Wraps the classic D3DCompiler (fxc) pipeline: HLSL source text in, DXIL/
// DXBC bytecode out, for the D3D12 backend. Throws std::runtime_error (with
// the compiler's error blob logged) on a compile failure.
ShaderBytecode CompileHLSL(const std::string& source, const char* entry_point, const char* target_profile,
                           const char* debug_name = "shader");

// Wraps DXC (dxcompiler.dll, bundled with the Windows SDK — no Vulkan SDK
// needed) to compile the *same* HLSL source to SPIR-V for the Vulkan
// backend, via its `-spirv` flag. `target_profile` uses DXC's shader model
// syntax (e.g. "vs_6_0"/"ps_6_0"), not D3DCompile's ("vs_5_0"/"ps_5_0") —
// SM6.0+ is DXC's minimum. Available unconditionally (not gated on
// AETHER_HAS_VULKAN): dxcompiler.lib/.dll ship with the base Windows SDK,
// independent of whether the Vulkan loader/headers were found.
ShaderBytecode CompileHLSLToSPIRV(const std::string& source, const char* entry_point, const char* target_profile,
                                  const char* debug_name = "shader");

} // namespace aether::gfx
