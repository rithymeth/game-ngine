#include "aether/gfx/shader_compiler.h"

#include "aether/core/log.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <cstring>
#include <mutex>
#include <stdexcept>

// The shader compilers off Windows (Phase 24, docs/design/PHASE_SPECS.md
// §24.3): glslang's HLSL front end compiles the same HLSL the Windows
// build hands to DXC into SPIR-V for the Vulkan backend, with DXC's
// register-to-binding mapping (register(tN, spaceM) -> set M, binding N)
// and entry points kept by name. There is no D3D12 off Windows, so
// CompileHLSL (fxc, DXBC) only reports that.

namespace aether::gfx {

namespace {

EShLanguage StageForProfile(const char* profile) {
    if (!profile || std::strlen(profile) < 2) return EShLangCount;
    switch (profile[0]) {
        case 'v': return EShLangVertex;
        case 'p': return EShLangFragment;
        case 'c': return EShLangCompute;
        case 'g': return EShLangGeometry;
        case 'h': return EShLangTessControl;
        case 'd': return EShLangTessEvaluation;
        default: return EShLangCount;
    }
}

void InitializeOnce() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

} // namespace

ShaderBytecode CompileHLSL(const std::string&, const char* entry_point, const char*, const char* debug_name) {
    AETHER_LOG_ERROR("Shader", "CompileHLSL (\"%s\", %s): DXBC needs D3DCompiler, which only exists on Windows",
                     debug_name, entry_point);
    throw std::runtime_error("CompileHLSL is Windows-only");
}

ShaderBytecode CompileHLSLToSPIRV(const std::string& source, const char* entry_point, const char* target_profile,
                                  const char* debug_name) {
    const EShLanguage stage = StageForProfile(target_profile);
    if (stage == EShLangCount) {
        AETHER_LOG_ERROR("Shader", "\"%s\": unknown shader profile \"%s\"", debug_name,
                         target_profile ? target_profile : "");
        throw std::runtime_error("unknown shader profile");
    }
    InitializeOnce();

    glslang::TShader shader(stage);
    const char* text = source.c_str();
    const int length = static_cast<int>(source.size());
    const char* name = debug_name;
    shader.setStringsWithLengthsAndNames(&text, &length, &name, 1);
    shader.setEntryPoint(entry_point);
    shader.setSourceEntryPoint(entry_point);
    shader.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    const EShMessages messages =
        static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl);

    if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
        AETHER_LOG_ERROR("Shader", "Failed to compile \"%s\" (%s) to SPIR-V: %s", debug_name, entry_point,
                         shader.getInfoLog());
        throw std::runtime_error("HLSL-to-SPIRV compilation failed");
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        AETHER_LOG_ERROR("Shader", "Failed to link \"%s\" (%s): %s", debug_name, entry_point, program.getInfoLog());
        throw std::runtime_error("HLSL-to-SPIRV link failed");
    }

    std::vector<unsigned int> words;
    glslang::SpvOptions options;
#if !defined(NDEBUG)
    options.generateDebugInfo = true;
    options.disableOptimizer = true;
#endif
    glslang::GlslangToSpv(*program.getIntermediate(stage), words, &options);

    ShaderBytecode bytecode;
    bytecode.data.resize(words.size() * sizeof(unsigned int));
    std::memcpy(bytecode.data.data(), words.data(), bytecode.data.size());
    return bytecode;
}

} // namespace aether::gfx
