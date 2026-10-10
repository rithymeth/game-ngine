#pragma once

// @stability: experimental
//
// Platform lifecycle (Phase 45 step 2, docs/design/UPGRADE_PLAN.md): the events a phone, a console or a
// window manager sends a running game (suspended, resumed, low on memory) and the permission requests a
// platform asks the user about. The engine side is the same everywhere: a platform backend (or a platform
// plugin) feeds events in, and game code subscribes. `MockLifecycleBackend` drives it from tests.
//
// Everything runs on the main thread: backends must hand events over to it before calling `Dispatch`.

#include "aether/core/base.h"

#include <functional>
#include <vector>

namespace aether::platform {

enum class LifecycleEvent : u8 {
    Suspend,        // the app is going to the background; save and release what you can
    Resume,         // it is back
    MemoryPressure, // the system wants memory back; see `MemoryLevel`
};

enum class MemoryLevel : u8 { Moderate, Critical };

struct LifecycleMessage {
    LifecycleEvent event = LifecycleEvent::Resume;
    MemoryLevel level = MemoryLevel::Moderate; // only for MemoryPressure
};

enum class Permission : u8 { Storage, Microphone, Camera, Notifications };
enum class PermissionState : u8 { NotAsked, Granted, Denied };
const char* PermissionName(Permission permission);

class PlatformLifecycle {
public:
    using Handler = std::function<void(const LifecycleMessage&)>;
    using Handle = u32;
    // Asks the platform for a permission and says how it went. A backend decides how (a dialog, a settings page).
    using PermissionRequester = std::function<void(Permission, std::function<void(bool granted)>)>;

    // Handlers run in subscription order. Returns 0 for an empty handler.
    Handle Subscribe(Handler handler);
    // Safe to call from inside a handler.
    void Unsubscribe(Handle handle);
    void Dispatch(const LifecycleMessage& message);

    bool Suspended() const { return suspended_; }

    // The backend that shows the permission prompt. Without one every request is denied.
    void SetPermissionRequester(PermissionRequester requester) { requester_ = std::move(requester); }
    PermissionState State(Permission permission) const { return states_[static_cast<usize>(permission)]; }
    // Asks once: a permission that is already Granted or Denied answers at once without asking again.
    void Request(Permission permission, std::function<void(PermissionState)> done);

private:
    struct Entry {
        Handle handle;
        Handler handler;
        bool removed = false;
    };
    std::vector<Entry> handlers_;
    Handle next_ = 1;
    u32 dispatching_ = 0;
    bool suspended_ = false;
    PermissionRequester requester_;
    PermissionState states_[4] = {PermissionState::NotAsked, PermissionState::NotAsked, PermissionState::NotAsked, PermissionState::NotAsked};
};

// A backend for tests and headless runs: nothing happens until a test injects it.
class MockLifecycleBackend {
public:
    explicit MockLifecycleBackend(PlatformLifecycle& lifecycle);

    void InjectSuspend() { lifecycle_.Dispatch({LifecycleEvent::Suspend, MemoryLevel::Moderate}); }
    void InjectResume() { lifecycle_.Dispatch({LifecycleEvent::Resume, MemoryLevel::Moderate}); }
    void InjectMemoryPressure(MemoryLevel level) { lifecycle_.Dispatch({LifecycleEvent::MemoryPressure, level}); }
    // What the mock "user" answers when a permission is requested; the default is yes.
    void SetAnswer(Permission permission, bool granted) { answers_[static_cast<usize>(permission)] = granted; }
    usize PromptsShown() const { return prompts_; }

private:
    PlatformLifecycle& lifecycle_;
    bool answers_[4] = {true, true, true, true};
    usize prompts_ = 0;
};

} // namespace aether::platform
