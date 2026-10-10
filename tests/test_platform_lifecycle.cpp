#include "test_framework.h"

#include "aether/platform/lifecycle.h"

#include <string>
#include <vector>

// Platform lifecycle (Phase 45 step 2): events reach subscribers in order, handlers can unsubscribe
// themselves, memory pressure carries its level, and permissions are asked once.

using namespace aether;
using namespace aether::platform;

AETHER_TEST(Lifecycle_EventsReachSubscribersInOrderAndTrackSuspension) {
    PlatformLifecycle lifecycle;
    MockLifecycleBackend mock(lifecycle);
    std::vector<std::string> log;
    lifecycle.Subscribe([&](const LifecycleMessage& m) { log.push_back("a" + std::to_string(static_cast<int>(m.event))); });
    lifecycle.Subscribe([&](const LifecycleMessage& m) { log.push_back("b" + std::to_string(static_cast<int>(m.event))); });
    AETHER_CHECK(lifecycle.Subscribe(nullptr) == 0);

    mock.InjectSuspend();
    AETHER_CHECK(lifecycle.Suspended());
    mock.InjectResume();
    AETHER_CHECK(!lifecycle.Suspended());
    AETHER_CHECK((log == std::vector<std::string>{"a0", "b0", "a1", "b1"}));
}

AETHER_TEST(Lifecycle_MemoryPressureCarriesALevelAndHandlersCanUnsubscribe) {
    PlatformLifecycle lifecycle;
    MockLifecycleBackend mock(lifecycle);
    int critical = 0, moderate = 0, once = 0;
    PlatformLifecycle::Handle once_handle = 0;
    lifecycle.Subscribe([&](const LifecycleMessage& m) {
        if (m.event != LifecycleEvent::MemoryPressure) return;
        (m.level == MemoryLevel::Critical ? critical : moderate)++;
    });
    once_handle = lifecycle.Subscribe([&](const LifecycleMessage&) {
        ++once;
        lifecycle.Unsubscribe(once_handle); // from inside its own handler
    });
    mock.InjectMemoryPressure(MemoryLevel::Moderate);
    mock.InjectMemoryPressure(MemoryLevel::Critical);
    AETHER_CHECK(moderate == 1 && critical == 1 && once == 1);
}

AETHER_TEST(Lifecycle_SubscribingDuringDispatchHearsOnlyTheNextEvent) {
    PlatformLifecycle lifecycle;
    MockLifecycleBackend mock(lifecycle);
    int late = 0;
    bool added = false;
    lifecycle.Subscribe([&](const LifecycleMessage&) {
        if (added) return;
        added = true;
        lifecycle.Subscribe([&](const LifecycleMessage&) { ++late; });
    });
    mock.InjectResume();
    AETHER_CHECK(late == 0);
    mock.InjectResume();
    AETHER_CHECK(late == 1);
}

AETHER_TEST(Lifecycle_PermissionsAreAskedOnceAndRemembered) {
    PlatformLifecycle lifecycle;
    MockLifecycleBackend mock(lifecycle);
    mock.SetAnswer(Permission::Camera, false);
    PermissionState camera = PermissionState::NotAsked, mic = PermissionState::NotAsked;
    AETHER_CHECK(lifecycle.State(Permission::Camera) == PermissionState::NotAsked);
    lifecycle.Request(Permission::Camera, [&](PermissionState s) { camera = s; });
    lifecycle.Request(Permission::Microphone, [&](PermissionState s) { mic = s; });
    AETHER_CHECK(camera == PermissionState::Denied && mic == PermissionState::Granted && mock.PromptsShown() == 2);
    // Asking again answers from memory and shows no new prompt.
    lifecycle.Request(Permission::Camera, [&](PermissionState s) { camera = s; });
    AETHER_CHECK(camera == PermissionState::Denied && mock.PromptsShown() == 2);
    AETHER_CHECK(std::string(PermissionName(Permission::Notifications)) == "Notifications");

    // With no backend every request is denied.
    PlatformLifecycle bare;
    PermissionState storage = PermissionState::Granted;
    bare.Request(Permission::Storage, [&](PermissionState s) { storage = s; });
    AETHER_CHECK(storage == PermissionState::Denied);
}
