#pragma once

#include "aether/core/base.h"

#include <functional>
#include <string>

namespace aether {

struct WindowDesc {
    std::string title = "Aether";
    u32 width = 1280;
    u32 height = 720;
};

// Thin Win32 window wrapper: owns the HWND and the message pump, and nothing
// else — no rendering, no input mapping. The graphics backend takes
// NativeHandle() (an HWND, kept as void* here so this header doesn't have to
// pull in <Windows.h>) and builds a swap chain around it.
class Window {
public:
    explicit Window(const WindowDesc& desc);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Drains the OS message queue for this window. Returns false once the
    // window has been closed, at which point the caller should exit its main
    // loop.
    bool PumpMessages();

    void* NativeHandle() const { return native_handle_; }
    u32 Width() const { return width_; }
    u32 Height() const { return height_; }
    bool IsMinimized() const { return width_ == 0 || height_ == 0; }

    // Fired synchronously from PumpMessages when a resize is detected (e.g.
    // so the caller can resize its swap chain).
    std::function<void(u32 width, u32 height)> on_resize;

    // Called with every raw message this window's WndProc receives, before
    // Window's own handling — the hook a UI library's Win32 backend (e.g.
    // Dear ImGui's ImGui_ImplWin32_WndProcHandler) needs to see input
    // messages. Params are the generic Win32 message shape (HWND, UINT,
    // WPARAM, LPARAM) as plain integers so this header doesn't need
    // <Windows.h>; a Windows-side caller casts back to the native types.
    std::function<void(void* hwnd, u32 msg, u64 wparam, i64 lparam)> native_message_hook;

    // Called by the internal Win32 message callback; not for external use.
    void NotifyResized(u32 width, u32 height);
    void NotifyClosed();

private:
    void* native_handle_ = nullptr;
    u32 width_ = 0;
    u32 height_ = 0;
    bool should_close_ = false;
};

} // namespace aether
