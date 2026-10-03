// The window on Linux and macOS (Phase 24 step 3): GLFW, with no client
// API (the renderer brings Vulkan). Events come from GLFW's callbacks and,
// for the first gamepad, from polling its state each pump.
#include "aether/core/log.h"
#include "aether/platform/window.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#if defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#else
#define GLFW_EXPOSE_NATIVE_X11
#if defined(AETHER_GLFW_WAYLAND)
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#endif
#include <GLFW/glfw3native.h>
// Xlib's macros collide with engine names (input::Key::None).
#undef None
#undef Bool
#undef Status
#undef Always
#undef Success

#include <cmath>
#include <mutex>

namespace aether {

namespace {

int g_glfw_users = 0;
std::mutex g_glfw_mutex;

bool AcquireGlfw() {
    std::lock_guard<std::mutex> lock(g_glfw_mutex);
    if (g_glfw_users == 0) {
        glfwSetErrorCallback([](int code, const char* text) { AETHER_LOG_ERROR("Window", "GLFW error %d: %s", code, text); });
        if (!glfwInit()) return false;
    }
    ++g_glfw_users;
    return true;
}

void ReleaseGlfw() {
    std::lock_guard<std::mutex> lock(g_glfw_mutex);
    if (g_glfw_users > 0 && --g_glfw_users == 0) glfwTerminate();
}

Window* Owner(GLFWwindow* w) { return static_cast<Window*>(glfwGetWindowUserPointer(w)); }

} // namespace

struct WindowBackendAccess {
    static void MouseAt(Window& w, f32 x, f32 y) {
        WindowEvent e;
        e.type = WindowEventType::MouseMove;
        e.x = x, e.y = y;
        if (w.has_mouse_) e.dx = x - w.last_mouse_x_, e.dy = y - w.last_mouse_y_;
        w.last_mouse_x_ = x, w.last_mouse_y_ = y, w.has_mouse_ = true;
        w.PushEvent(e);
    }
    static void SetHandles(Window& w, void* native, void* display, void* platform) {
        w.native_handle_ = native, w.native_display_ = display, w.platform_window_ = platform;
    }
};

Window::Window(const WindowDesc& desc) : width_(desc.width), height_(desc.height) {
    if (!AcquireGlfw()) {
        AETHER_LOG_ERROR("Window", "No windowing system (is there a display?); \"%s\" not created", desc.title.c_str());
        return;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);
    GLFWwindow* w = glfwCreateWindow(static_cast<int>(desc.width), static_cast<int>(desc.height), desc.title.c_str(), nullptr, nullptr);
    if (!w) {
        ReleaseGlfw();
        AETHER_LOG_ERROR("Window", "Couldn't create window \"%s\"", desc.title.c_str());
        return;
    }
    glfwSetWindowUserPointer(w, this);
    void* native = nullptr;
    void* display = nullptr;
#if defined(__APPLE__)
    native = glfwGetCocoaWindow(w);
#else
    if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
        native = reinterpret_cast<void*>(static_cast<uintptr_t>(glfwGetX11Window(w)));
        display = glfwGetX11Display();
    }
#if defined(AETHER_GLFW_WAYLAND)
    else if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        native = glfwGetWaylandWindow(w);
        display = glfwGetWaylandDisplay();
    }
#endif
#endif
    WindowBackendAccess::SetHandles(*this, native, display, w);

    glfwSetKeyCallback(w, [](GLFWwindow* gw, int key, int, int action, int) {
        if (action == GLFW_REPEAT) return;
        WindowEvent e;
        e.type = WindowEventType::Key;
        e.key = KeyFromGlfwKey(key);
        e.down = action == GLFW_PRESS;
        if (e.key != input::Key::None) Owner(gw)->PushEvent(e);
    });
    glfwSetMouseButtonCallback(w, [](GLFWwindow* gw, int button, int action, int) {
        WindowEvent e;
        e.type = WindowEventType::Key;
        e.key = KeyFromGlfwMouseButton(button);
        e.down = action == GLFW_PRESS;
        if (e.key != input::Key::None) Owner(gw)->PushEvent(e);
    });
    glfwSetCursorPosCallback(w, [](GLFWwindow* gw, double x, double y) {
        WindowBackendAccess::MouseAt(*Owner(gw), static_cast<f32>(x), static_cast<f32>(y));
    });
    glfwSetScrollCallback(w, [](GLFWwindow* gw, double dx, double dy) {
        WindowEvent e;
        e.type = WindowEventType::Scroll;
        e.dx = static_cast<f32>(dx), e.dy = static_cast<f32>(dy);
        Owner(gw)->PushEvent(e);
    });
    glfwSetCharCallback(w, [](GLFWwindow* gw, unsigned int codepoint) {
        WindowEvent e;
        e.type = WindowEventType::Char;
        e.codepoint = codepoint;
        Owner(gw)->PushEvent(e);
    });
    glfwSetWindowFocusCallback(w, [](GLFWwindow* gw, int focused) {
        WindowEvent e;
        e.type = WindowEventType::Focus;
        e.down = focused == GLFW_TRUE;
        Owner(gw)->PushEvent(e);
    });
    glfwSetFramebufferSizeCallback(w, [](GLFWwindow* gw, int width, int height) {
        Owner(gw)->NotifyResized(static_cast<u32>(width), static_cast<u32>(height));
    });
    glfwSetWindowCloseCallback(w, [](GLFWwindow* gw) {
        glfwSetWindowShouldClose(gw, GLFW_FALSE); // Window decides, as on Win32
        Owner(gw)->NotifyClosed();
    });
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(w, &fw, &fh);
    width_ = static_cast<u32>(fw), height_ = static_cast<u32>(fh);
    AETHER_LOG_INFO("Window", "Created %ux%u window \"%s\" (%s)", width_, height_, desc.title.c_str(), Backend());
}

Window::~Window() {
    if (platform_window_) {
        glfwDestroyWindow(static_cast<GLFWwindow*>(platform_window_));
        ReleaseGlfw();
    }
}

const char* Window::Backend() const {
#if defined(__APPLE__)
    return "glfw-cocoa";
#else
    if (!platform_window_) return "none";
    return glfwGetPlatform() == GLFW_PLATFORM_WAYLAND ? "glfw-wayland" : "glfw-x11";
#endif
}

void Window::SetTitle(const std::string& title) {
    if (platform_window_) glfwSetWindowTitle(static_cast<GLFWwindow*>(platform_window_), title.c_str());
}

void Window::RequestClose() {
    if (platform_window_) NotifyClosed();
}

void Window::NotifyResized(u32 width, u32 height) {
    width_ = width;
    height_ = height;
    WindowEvent e;
    e.type = WindowEventType::Resize;
    e.x = static_cast<f32>(width), e.y = static_cast<f32>(height);
    PushEvent(e);
    if (on_resize) on_resize(width, height);
}

void Window::NotifyClosed() {
    if (should_close_) return;
    should_close_ = true;
    WindowEvent e;
    e.type = WindowEventType::Close;
    PushEvent(e);
}

bool Window::PumpMessages() {
    if (!platform_window_) return false;
    glfwPollEvents();
    // The first gamepad: buttons as key events when they change, axes every pump.
    static GLFWgamepadstate previous{};
    GLFWgamepadstate state;
    if (glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &state)) {
        for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b) {
            if (state.buttons[b] == previous.buttons[b]) continue;
            WindowEvent e;
            e.type = WindowEventType::Key;
            e.key = KeyFromGlfwGamepadButton(b);
            e.down = state.buttons[b] == GLFW_PRESS;
            if (e.key != input::Key::None) PushEvent(e);
        }
        for (int a = 0; a <= GLFW_GAMEPAD_AXIS_LAST; ++a) {
            WindowEvent e;
            e.type = WindowEventType::Axis;
            e.key = KeyFromGlfwGamepadAxis(a);
            f32 v = state.axes[a];
            if (a == GLFW_GAMEPAD_AXIS_LEFT_Y || a == GLFW_GAMEPAD_AXIS_RIGHT_Y) v = -v;   // up is positive, as XInput
            if (a >= GLFW_GAMEPAD_AXIS_LEFT_TRIGGER) v = (v + 1.0f) * 0.5f;                  // 0..1
            e.x = v;
            PushEvent(e);
        }
        previous = state;
    }
    return !should_close_;
}

} // namespace aether
