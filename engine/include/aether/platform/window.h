#pragma once

#include "aether/core/base.h"
#include "aether/input/keys.h"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace aether {

struct WindowDesc {
    std::string title = "Aether";
    u32 width = 1280;
    u32 height = 720;
    bool visible = true; // false: create it hidden (tests, offscreen tools)
};

// What a window reports (Phase 24 step 3, docs/design/PHASE_SPECS.md
// §24.3), the same on every platform: keys and buttons in the engine's
// input::Key terms, mouse movement and wheel, typed characters, gamepad
// axes, focus, resizes and the close request.
enum class WindowEventType : u8 { Key, Char, MouseMove, Scroll, Axis, Focus, Resize, Close };

struct WindowEvent {
    WindowEventType type = WindowEventType::Key;
    input::Key key = input::Key::None; // Key: the key or button; Axis: the axis
    bool down = false;                 // Key: pressed; Focus: gained
    f32 x = 0, y = 0;                  // MouseMove: the cursor (pixels); Resize: the size; Axis: x is the value
    f32 dx = 0, dy = 0;                // MouseMove: the movement; Scroll: the wheel (dy is vertical)
    u32 codepoint = 0;                 // Char: the Unicode character typed
};

// Applies a frame's window events to the raw input state: keys and buttons,
// mouse movement and wheel (accumulated), gamepad axes; losing focus
// releases everything. Characters, resizes and closing aren't input state.
void ApplyWindowEvents(std::span<const WindowEvent> events, input::InputState& state);

// The engine's key for a platform key code: Win32 virtual-key codes
// (VK_*), GLFW key codes (GLFW_KEY_*), GLFW mouse buttons and gamepad
// buttons and axes. Key::None for keys the engine has no name for.
input::Key KeyFromVirtualKey(u32 vk);
input::Key KeyFromGlfwKey(int key);
input::Key KeyFromGlfwMouseButton(int button);
input::Key KeyFromGlfwGamepadButton(int button);
input::Key KeyFromGlfwGamepadAxis(int axis);

// A native window: Win32 on Windows, GLFW elsewhere (X11, Wayland, Cocoa).
// It owns the OS window and its event pump, and nothing else - no
// rendering, no input mapping. The graphics backend builds a swap chain
// or surface around NativeHandle() (an HWND, an X11 Window, an NSWindow),
// NativeDisplay() (the X11 Display or Wayland display where there is one)
// or, on GLFW, PlatformWindow() (the GLFWwindow, for glfwCreateWindowSurface).
class Window {
public:
    explicit Window(const WindowDesc& desc);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // False when the window couldn't be created (no display, say).
    bool IsValid() const { return native_handle_ != nullptr || platform_window_ != nullptr; }
    // "win32", "glfw-x11", "glfw-wayland", "glfw-cocoa".
    const char* Backend() const;

    // Drains the OS queue for this window, turning what happened into events
    // (and on_resize calls). Returns false once the window has been closed,
    // at which point the caller should leave its main loop.
    bool PumpMessages();
    // The events since the last call.
    std::vector<WindowEvent> TakeEvents();

    void* NativeHandle() const { return native_handle_; }
    void* NativeDisplay() const { return native_display_; }
    void* PlatformWindow() const { return platform_window_; }
    u32 Width() const { return width_; }
    u32 Height() const { return height_; }
    bool IsMinimized() const { return width_ == 0 || height_ == 0; }
    void SetTitle(const std::string& title);
    void RequestClose();

    // Fired synchronously from PumpMessages when a resize is detected (e.g.
    // so the caller can resize its swap chain).
    std::function<void(u32 width, u32 height)> on_resize;

    // Win32 only: called with every raw message this window's WndProc
    // receives, before Window's own handling - the hook a UI library's
    // Win32 backend (e.g. Dear ImGui's ImGui_ImplWin32_WndProcHandler)
    // needs. Params are the generic Win32 message shape (HWND, UINT, WPARAM,
    // LPARAM) as plain integers so this header doesn't need <Windows.h>.
    std::function<void(void* hwnd, u32 msg, u64 wparam, i64 lparam)> native_message_hook;

    // Called by the platform backend; not for external use.
    void NotifyResized(u32 width, u32 height);
    void NotifyClosed();
    void PushEvent(const WindowEvent& event) { events_.push_back(event); }

private:
    void* native_handle_ = nullptr;
    void* native_display_ = nullptr;
    void* platform_window_ = nullptr;
    u32 width_ = 0;
    u32 height_ = 0;
    bool should_close_ = false;
    f32 last_mouse_x_ = 0, last_mouse_y_ = 0;
    bool has_mouse_ = false;
    std::vector<WindowEvent> events_;
    friend struct WindowBackendAccess;
};

} // namespace aether
