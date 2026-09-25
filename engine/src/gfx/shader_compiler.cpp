#include "aether/gfx/shader_compiler.h"

#include "aether/core/log.h"
#include "aether/gfx/d3d12_common.h"

#include <d3dcompiler.h>
#include <stdexcept>

namespace aether::gfx {

ShaderBytecode CompileHLSL(const std::string& source, const char* entry_point, const char* target_profile,
                            const char* debug_name) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if !defined(NDEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(source.data(), source.size(), debug_name, nullptr, nullptr, entry_point, target_profile,
                             flags, 0, &bytecode, &errors);

    if (FAILED(hr)) {
        const char* message = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_ERROR("Shader", "Failed to compile \"%s\" (%s): %s", debug_name, entry_point, message);
        throw std::runtime_error("HLSL compilation failed");
    }

    ShaderBytecode result;
    result.data.assign(static_cast<u8*>(bytecode->GetBufferPointer()),
                        static_cast<u8*>(bytecode->GetBufferPointer()) + bytecode->GetBufferSize());
    return result;
}

} // namespace aether::gfx
