// Platforms with the Vulkan backend but no shader compiler yet (Android: glslang is not built for it). Swap chains,
// clears and presents work; anything that compiles HLSL (CreatePipeline, compute pipelines) throws, the same way the
// glslang build reports a compile failure.
#include "aether/core/log.h"
#include "aether/gfx/shader_compiler.h"

#include <stdexcept>

namespace aether::gfx {

ShaderBytecode CompileHLSL(const std::string&, const char* entry_point, const char*, const char* debug_name) {
    AETHER_LOG_ERROR("Shader", "CompileHLSL (\"%s\", %s): DXBC needs D3DCompiler, which only exists on Windows",
                     debug_name, entry_point);
    throw std::runtime_error("CompileHLSL is Windows-only");
}

ShaderBytecode CompileHLSLToSPIRV(const std::string&, const char* entry_point, const char*, const char* debug_name) {
    AETHER_LOG_ERROR("Shader", "\"%s\" (%s): this build has no HLSL to SPIR-V compiler", debug_name, entry_point);
    throw std::runtime_error("no shader compiler in this build");
}

} // namespace aether::gfx
