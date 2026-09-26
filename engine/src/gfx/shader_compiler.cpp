#include "aether/gfx/shader_compiler.h"

#include "aether/core/log.h"
#include "aether/gfx/d3d12_common.h"

#include <d3dcompiler.h>
#include <dxcapi.h>

#include <stdexcept>

namespace aether::gfx {

namespace {

std::wstring Utf8ToWide(const char* s) {
    if (!s || !*s) {
        return {};
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring result(static_cast<usize>(len > 0 ? len - 1 : 0), L'\0'); // -1: exclude the null terminator MultiByteToWideChar counts
    if (len > 1) {
        MultiByteToWideChar(CP_UTF8, 0, s, -1, result.data(), len);
    }
    return result;
}

} // namespace

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

ShaderBytecode CompileHLSLToSPIRV(const std::string& source, const char* entry_point, const char* target_profile,
                                  const char* debug_name) {
    ComPtr<IDxcUtils> utils;
    AETHER_D3D_CHECK(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)));

    ComPtr<IDxcCompiler3> compiler;
    AETHER_D3D_CHECK(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));

    ComPtr<IDxcBlobEncoding> source_blob;
    AETHER_D3D_CHECK(
        utils->CreateBlob(source.data(), static_cast<UINT32>(source.size()), CP_UTF8, &source_blob));

    DxcBuffer source_buffer{};
    source_buffer.Ptr = source_blob->GetBufferPointer();
    source_buffer.Size = source_blob->GetBufferSize();
    source_buffer.Encoding = DXC_CP_UTF8;

    std::wstring wentry = Utf8ToWide(entry_point);
    std::wstring wprofile = Utf8ToWide(target_profile);

    std::vector<LPCWSTR> args = {L"-E", wentry.c_str(), L"-T", wprofile.c_str(), L"-spirv"};
#if !defined(NDEBUG)
    args.push_back(L"-Od");
#endif

    ComPtr<IDxcResult> result;
    AETHER_D3D_CHECK(compiler->Compile(&source_buffer, args.data(), static_cast<UINT32>(args.size()), nullptr,
                                        IID_PPV_ARGS(&result)));

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status)) {
        const char* message = (errors && errors->GetStringLength() > 0) ? errors->GetStringPointer() : "(no error blob)";
        AETHER_LOG_ERROR("Shader", "Failed to compile \"%s\" (%s) to SPIR-V: %s", debug_name, entry_point, message);
        throw std::runtime_error("HLSL-to-SPIRV compilation failed");
    } else if (errors && errors->GetStringLength() > 0) {
        AETHER_LOG_WARN("Shader", "SPIR-V compile warnings for \"%s\" (%s): %s", debug_name, entry_point,
                         errors->GetStringPointer());
    }

    ComPtr<IDxcBlob> object;
    AETHER_D3D_CHECK(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr));

    ShaderBytecode bytecode;
    bytecode.data.assign(static_cast<u8*>(object->GetBufferPointer()),
                          static_cast<u8*>(object->GetBufferPointer()) + object->GetBufferSize());
    return bytecode;
}

} // namespace aether::gfx
