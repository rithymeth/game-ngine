#include "aether/platform/window.h"

#include "aether/core/log.h"

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <Windows.h>

namespace aether {

namespace {

constexpr const wchar_t* kWindowClassName = L"AetherWindowClass";

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
};

namespace {

std::wstring ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring result(static_cast<usize>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), result.data(), len);
    return result;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    Window* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (window && window->native_message_hook) {
        window->native_message_hook(hwnd, static_cast<u32>(msg), static_cast<u64>(wparam),
                                     static_cast<i64>(lparam));
    }

    if (window) {
        switch (msg) {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYUP: {
                u32 vk = static_cast<u32>(wparam);
                const UINT scancode = static_cast<UINT>((lparam >> 16) & 0xFF);
                const bool extended = ((lparam >> 24) & 1) != 0;
                if (vk == VK_SHIFT) vk = MapVirtualKeyW(scancode, MAPVK_VSC_TO_VK_EX);
                else if (vk == VK_CONTROL) vk = extended ? VK_RCONTROL : VK_LCONTROL;
                else if (vk == VK_MENU) vk = extended ? VK_RMENU : VK_LMENU;
                WindowEvent e;
                e.type = WindowEventType::Key;
                e.key = KeyFromVirtualKey(vk);
                e.down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
                if (e.key != input::Key::None) window->PushEvent(e);
                break; // DefWindowProc still sees them (Alt+F4)
            }
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_XBUTTONDOWN: case WM_XBUTTONUP: {
                WindowEvent e;
                e.type = WindowEventType::Key;
                e.down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN;
                if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) e.key = input::Key::MouseLeft;
                else if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) e.key = input::Key::MouseRight;
                else if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) e.key = input::Key::MouseMiddle;
                else e.key = HIWORD(wparam) == XBUTTON1 ? input::Key::MouseButton4 : input::Key::MouseButton5;
                window->PushEvent(e);
                break;
            }
            case WM_MOUSEMOVE:
                WindowBackendAccess::MouseAt(*window, static_cast<f32>(static_cast<short>(LOWORD(lparam))),
                                             static_cast<f32>(static_cast<short>(HIWORD(lparam))));
                break;
            case WM_MOUSEWHEEL: {
                WindowEvent e;
                e.type = WindowEventType::Scroll;
                e.dy = static_cast<f32>(static_cast<short>(HIWORD(wparam))) / static_cast<f32>(WHEEL_DELTA);
                window->PushEvent(e);
                break;
            }
            case WM_CHAR: {
                WindowEvent e;
                e.type = WindowEventType::Char;
                e.codepoint = static_cast<u32>(wparam);
                window->PushEvent(e);
                break;
            }
            case WM_SETFOCUS:
            case WM_KILLFOCUS: {
                WindowEvent e;
                e.type = WindowEventType::Focus;
                e.down = msg == WM_SETFOCUS;
                window->PushEvent(e);
                break;
            }
            default: break;
        }
    }

    switch (msg) {
        case WM_CREATE: {
            auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create_struct->lpCreateParams));
            return 0;
        }
        case WM_CLOSE:
            if (window) {
                window->NotifyClosed();
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            if (window && wparam != SIZE_MINIMIZED) {
                window->NotifyResized(LOWORD(lparam), HIWORD(lparam));
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

void EnsureWindowClassRegistered() {
    static bool registered = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClassName;
        RegisterClassExW(&wc);
        return true;
    }();
    (void)registered;
}

} // namespace

Window::Window(const WindowDesc& desc) : width_(desc.width), height_(desc.height) {
    EnsureWindowClassRegistered();

    RECT rect{0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    std::wstring wide_title = ToWide(desc.title);
    HWND hwnd = CreateWindowExW(0, kWindowClassName, wide_title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                 CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), this);
    AETHER_ASSERT(hwnd != nullptr);
    native_handle_ = hwnd;

    ShowWindow(hwnd, desc.visible ? SW_SHOW : SW_HIDE);
    AETHER_LOG_INFO("Window", "Created %ux%u window \"%s\"", desc.width, desc.height, desc.title.c_str());
}

Window::~Window() {
    if (native_handle_) {
        DestroyWindow(static_cast<HWND>(native_handle_));
    }
}

const char* Window::Backend() const { return "win32"; }

void Window::SetTitle(const std::string& title) {
    if (native_handle_) SetWindowTextW(static_cast<HWND>(native_handle_), ToWide(title).c_str());
}

void Window::RequestClose() {
    if (native_handle_) PostMessageW(static_cast<HWND>(native_handle_), WM_CLOSE, 0, 0);
}

void Window::NotifyResized(u32 width, u32 height) {
    width_ = width;
    height_ = height;
    WindowEvent e;
    e.type = WindowEventType::Resize;
    e.x = static_cast<f32>(width), e.y = static_cast<f32>(height);
    PushEvent(e);
    if (on_resize) {
        on_resize(width, height);
    }
}

void Window::NotifyClosed() {
    should_close_ = true;
    WindowEvent e;
    e.type = WindowEventType::Close;
    PushEvent(e);
    DestroyWindow(static_cast<HWND>(native_handle_));
}

bool Window::PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            should_close_ = true;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !should_close_;
}

} // namespace aether
