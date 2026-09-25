#pragma once

#include "aether/core/base.h"
#include "aether/core/log.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX // Windows.h's min/max macros break any TU that also uses std::min/max (e.g. Jolt Physics)
#endif
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <stdexcept>

namespace aether::gfx {

using Microsoft::WRL::ComPtr;

// ComPtr is a thin, zero-overhead RAII wrapper for COM reference counting
// (AddRef/Release), not a general-purpose smart pointer with hidden
// allocation or indirection — using it here doesn't compromise the "explicit
// GPU control" principle, it just avoids manual Release() bookkeeping on
// every D3D12/DXGI interface pointer.
inline void ThrowIfFailed(HRESULT hr, const char* expression) {
    if (FAILED(hr)) {
        AETHER_LOG_FATAL("D3D12", "%s failed (hr=0x%08lX)", expression, static_cast<unsigned long>(hr));
        throw std::runtime_error(expression);
    }
}

} // namespace aether::gfx

#define AETHER_D3D_CHECK(expr) ::aether::gfx::ThrowIfFailed((expr), #expr)
