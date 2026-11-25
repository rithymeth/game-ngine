#include "aether/platform/window.h"

namespace aether {

// With no native backend, the window never becomes valid and pumps no events.
Window::Window(const WindowDesc&) {}
Window::~Window() = default;

const char* Window::Backend() const { return "headless"; }
bool Window::PumpMessages() { return false; }
void Window::SetTitle(const std::string&) {}
void Window::RequestClose() {}
void Window::SetCursorMode(CursorMode mode) { cursor_mode_ = mode; }
void Window::NotifyResized(u32, u32) {}
void Window::NotifyClosed() {}

} // namespace aether
