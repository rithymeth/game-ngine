#include "aether/gfx/shader_compiler.h"

#include "aether/core/log.h"
#include "aether/gfx/d3d12_common.h"

#include <d3dcompiler.h>
#include <dxcapi.h>

#include <stdexcept>
#include <string>

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

using D3DCompileFunction = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                            ID3DBlob**, ID3DBlob**);

std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<usize>(length > 0 ? length : 0), '\0');
    if (length > 0) WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), length, nullptr, nullptr);
    return out;
}

// The d3dcompiler import resolves D3DCompiler_47.dll like any DLL: the executable's own folder first. A copy deployed there that predates
// Windows 10's rejects every shader model 5.1 profile with X3506 "unrecognized compiler target" (Qt ships an old one for ANGLE, and
// windeployqt puts it beside the editor), so the compiler is loaded from the system directory by full path, which also keeps it distinct
// from the one the import loaded. Null if the system has none; CompileHLSL then uses the imported one.
D3DCompileFunction SystemD3DCompile(std::string& path) {
    struct Loaded {
        D3DCompileFunction function = nullptr;
        std::string path;
    };
    static const Loaded loaded = [] {
        Loaded result;
        wchar_t directory[MAX_PATH] = {};
        const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) return result;
        const std::wstring full = std::wstring(directory, length) + L"\\D3DCompiler_47.dll";
        if (HMODULE module = LoadLibraryW(full.c_str())) {
            result.function = reinterpret_cast<D3DCompileFunction>(GetProcAddress(module, "D3DCompile"));
            if (result.function) result.path = Narrow(full);
        }
        return result;
    }();
    path = loaded.path;
    return loaded.function;
}

// The path of the D3DCompiler_47.dll the import loaded, for the failure message.
std::string ImportedCompilerPath() {
    wchar_t path[MAX_PATH] = {};
    HMODULE module = GetModuleHandleW(L"D3DCompiler_47.dll");
    const DWORD length = module ? GetModuleFileNameW(module, path, MAX_PATH) : 0;
    return length > 0 && length < MAX_PATH ? Narrow(std::wstring(path, length)) : std::string("(not loaded)");
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
    std::string compiler_path;
    const D3DCompileFunction system_compile = SystemD3DCompile(compiler_path);
    HRESULT hr = system_compile
        ? system_compile(source.data(), source.size(), debug_name, nullptr, nullptr, entry_point, target_profile, flags, 0, &bytecode, &errors)
        : D3DCompile(source.data(), source.size(), debug_name, nullptr, nullptr, entry_point, target_profile, flags, 0, &bytecode, &errors);

    if (FAILED(hr)) {
        const char* message = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_ERROR("Shader", "Failed to compile \"%s\" (%s, target %s): %s", debug_name, entry_point, target_profile, message);
        AETHER_LOG_ERROR("Shader", "  compiler used: %s; the import loaded: %s", system_compile ? compiler_path.c_str() : "the import's",
                         ImportedCompilerPath().c_str());
        throw std::runtime_error("HLSL compilation failed");
    }

    ShaderBytecode result;
    result.data.assign(static_cast<u8*>(bytecode->GetBufferPointer()),
                        static_cast<u8*>(bytecode->GetBufferPointer()) + bytecode->GetBufferSize());
    return result;
}

namespace {

// DXC to SPIR-V (the Vulkan backend) or to DXIL (D3D12 pipelines that need shader model 6).
ShaderBytecode CompileWithDxc(const std::string& source, const char* entry_point, const char* target_profile, const char* debug_name,
                              bool spirv) {
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

    std::vector<LPCWSTR> args = {L"-E", wentry.c_str(), L"-T", wprofile.c_str()};
    if (spirv) {
        args.push_back(L"-spirv");
        // Shader model 6.5 is what inline ray queries need: allow the extension and target Vulkan 1.2 (SPIR-V 1.5).
        if (std::string(target_profile).find("_6_5") != std::string::npos) {
            args.push_back(L"-fspv-target-env=vulkan1.2");
            args.push_back(L"-fspv-extension=SPV_KHR_ray_query");
        }
    }
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
        AETHER_LOG_ERROR("Shader", "Failed to compile \"%s\" (%s) to %s: %s", debug_name, entry_point, spirv ? "SPIR-V" : "DXIL", message);
        throw std::runtime_error(spirv ? "HLSL-to-SPIRV compilation failed" : "HLSL-to-DXIL compilation failed");
    } else if (errors && errors->GetStringLength() > 0) {
        AETHER_LOG_WARN("Shader", "%s compile warnings for \"%s\" (%s): %s", spirv ? "SPIR-V" : "DXIL", debug_name, entry_point,
                         errors->GetStringPointer());
    }

    ComPtr<IDxcBlob> object;
    AETHER_D3D_CHECK(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr));

    ShaderBytecode bytecode;
    bytecode.data.assign(static_cast<u8*>(object->GetBufferPointer()),
                          static_cast<u8*>(object->GetBufferPointer()) + object->GetBufferSize());
    return bytecode;
}

} // namespace

ShaderBytecode CompileHLSLToSPIRV(const std::string& source, const char* entry_point, const char* target_profile,
                                  const char* debug_name) {
    return CompileWithDxc(source, entry_point, target_profile, debug_name, true);
}

ShaderBytecode CompileHLSLToDXIL(const std::string& source, const char* entry_point, const char* target_profile,
                                 const char* debug_name) {
    return CompileWithDxc(source, entry_point, target_profile, debug_name, false);
}

} // namespace aether::gfx
