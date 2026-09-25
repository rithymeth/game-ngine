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

    ShowWindow(hwnd, SW_SHOW);
    AETHER_LOG_INFO("Window", "Created %ux%u window \"%s\"", desc.width, desc.height, desc.title.c_str());
}

Window::~Window() {
    if (native_handle_) {
        DestroyWindow(static_cast<HWND>(native_handle_));
    }
}

void Window::NotifyResized(u32 width, u32 height) {
    width_ = width;
    height_ = height;
    if (on_resize) {
        on_resize(width, height);
    }
}

void Window::NotifyClosed() {
    should_close_ = true;
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
