#include "aether/platform/lifecycle.h"

#include <algorithm>

namespace aether::platform {

const char* PermissionName(Permission permission) {
    switch (permission) {
    case Permission::Storage: return "Storage";
    case Permission::Microphone: return "Microphone";
    case Permission::Camera: return "Camera";
    case Permission::Notifications: return "Notifications";
    }
    return "?";
}

PlatformLifecycle::Handle PlatformLifecycle::Subscribe(Handler handler) {
    if (!handler) return 0;
    const Handle handle = next_++;
    handlers_.push_back({handle, std::move(handler), false});
    return handle;
}

void PlatformLifecycle::Unsubscribe(Handle handle) {
    for (Entry& e : handlers_) {
        if (e.handle == handle) e.removed = true;
    }
    // Inside a handler the list is being walked: it is cleaned up when the outer dispatch ends.
    if (dispatching_ == 0) {
        handlers_.erase(std::remove_if(handlers_.begin(), handlers_.end(), [](const Entry& e) { return e.removed; }), handlers_.end());
    }
}

void PlatformLifecycle::Dispatch(const LifecycleMessage& message) {
    if (message.event == LifecycleEvent::Suspend) suspended_ = true;
    if (message.event == LifecycleEvent::Resume) suspended_ = false;
    ++dispatching_;
    // By index, over the size at the start: a handler may subscribe (it hears the next event) or unsubscribe.
    const usize count = handlers_.size();
    for (usize i = 0; i < count; ++i) {
        if (handlers_[i].removed) continue;
        const Handler handler = handlers_[i].handler; // a copy: the list may grow under us
        handler(message);
    }
    --dispatching_;
    if (dispatching_ == 0) {
        handlers_.erase(std::remove_if(handlers_.begin(), handlers_.end(), [](const Entry& e) { return e.removed; }), handlers_.end());
    }
}

void PlatformLifecycle::Request(Permission permission, std::function<void(PermissionState)> done) {
    const usize index = static_cast<usize>(permission);
    if (states_[index] != PermissionState::NotAsked) {
        if (done) done(states_[index]);
        return;
    }
    if (!requester_) {
        states_[index] = PermissionState::Denied;
        if (done) done(PermissionState::Denied);
        return;
    }
    requester_(permission, [this, index, done = std::move(done)](bool granted) {
        states_[index] = granted ? PermissionState::Granted : PermissionState::Denied;
        if (done) done(states_[index]);
    });
}

MockLifecycleBackend::MockLifecycleBackend(PlatformLifecycle& lifecycle) : lifecycle_(lifecycle) {
    lifecycle_.SetPermissionRequester([this](Permission permission, std::function<void(bool)> answer) {
        ++prompts_;
        answer(answers_[static_cast<usize>(permission)]);
    });
}

} // namespace aether::platform
