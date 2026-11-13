#include "test_framework.h"
#include "core/notifications.h"
#include "ui/notification_overlay.h"

#include <imgui.h>

#include <functional>
#include <string>

using namespace aether;
using namespace aether::editor;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigMacOSXBehaviors = false;
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 800);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        body();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

} // namespace

// Stage 0 of the parity audit: one service keeps the history the Messages panel lists and the toasts
// the overlay shows, and posting is what marks a message unread.
AETHER_TEST(Notifications_HistoryToastsAndReadState) {
    NotificationService service;
    CHECK(service.All().empty());
    CHECK(service.UnreadCount() == 0);

    const u64 info = service.Post(NotificationSeverity::Info, "Saved", "Scenes/Main.ascene",
                                  {NotificationTarget::Kind::File, "Scenes/Main.ascene"});
    const u64 error = service.Post(NotificationSeverity::Error, "Import failed", "T_Rock.png is not a valid PNG",
                                   {NotificationTarget::Kind::Asset, "Textures/T_Rock.png"});
    CHECK(info != 0 && error != 0 && info != error);
    CHECK(service.All().size() == 2);
    CHECK(service.UnreadCount() == 2);
    CHECK(service.IsToastShowing(info));
    CHECK(service.IsToastShowing(error));
    CHECK(service.Find(info) != nullptr && service.Find(info)->title == "Saved");
    CHECK(service.Find(9999) == nullptr);

    service.MarkAllRead();
    CHECK(service.UnreadCount() == 0);
    service.Post(NotificationSeverity::Success, "Saved again", "");
    CHECK(service.UnreadCount() == 1);

    CHECK(service.NewestFirst().front()->title == "Saved again");
    service.Clear();
    CHECK(service.All().empty());
    CHECK(!service.IsToastShowing(info));
}

// Info and success toasts last 4 s, warnings 8 s, errors until dismissed; the history keeps every row.
AETHER_TEST(Notifications_ToastsExpireButErrorsStay) {
    NotificationService service;
    service.Update(0.0);
    const u64 info = service.Post(NotificationSeverity::Info, "Info", "");
    const u64 warning = service.Post(NotificationSeverity::Warning, "Warning", "");
    const u64 error = service.Post(NotificationSeverity::Error, "Error", "");

    service.Update(3.9);
    CHECK(service.IsToastShowing(info));
    service.Update(4.0); // exactly its lifetime: gone
    CHECK(!service.IsToastShowing(info));
    CHECK(service.IsToastShowing(warning));
    service.Update(7.9);
    CHECK(service.IsToastShowing(warning));
    service.Update(8.0);
    CHECK(!service.IsToastShowing(warning));
    service.Update(1.0e9);
    CHECK(service.IsToastShowing(error)); // never expires
    CHECK(service.All().size() == 3);     // the Messages list still has all three

    service.Dismiss(error);
    CHECK(!service.IsToastShowing(error));
    CHECK(service.Find(error) != nullptr); // dismissing hides the toast, not the message
}

// A warning that repeats back to back is one row with a count, refreshed, rather than a growing list.
AETHER_TEST(Notifications_RepeatedMessagesGroupAndRefresh) {
    NotificationService service;
    const NotificationTarget target{NotificationTarget::Kind::File, "Scenes/Main.ascene"};
    const u64 first = service.Post(NotificationSeverity::Warning, "Autosave failed", "disk full", target);
    service.Update(1.0);
    const u64 again = service.Post(NotificationSeverity::Warning, "Autosave failed", "disk full", target);
    CHECK(again == first);
    CHECK(service.All().size() == 1);
    CHECK(service.All().front().count == 2);
    CHECK(service.All().front().posted_at == 1.0);
    CHECK(service.IsToastShowing(first));

    // A different target or severity is a different message.
    CHECK(service.Post(NotificationSeverity::Warning, "Autosave failed", "disk full",
                       {NotificationTarget::Kind::Asset, "Scenes/Main.ascene"}) != first);
    CHECK(service.Post(NotificationSeverity::Error, "Autosave failed", "disk full", target) != first);
    CHECK(service.All().size() == 3);
    CHECK(service.NewestFirst().front()->severity == NotificationSeverity::Error);
}

AETHER_TEST(NotificationOverlay_AgesAndLabels) {
    CHECK(NotificationAge(10.0, 10.4) == "just now");
    CHECK(NotificationAge(10.0, 9.0) == "just now"); // a clock that went backwards is not negative
    CHECK(NotificationAge(10.0, 22.0) == "12 s ago");
    CHECK(NotificationAge(0.0, 190.0) == "3 min ago");
    CHECK(NotificationAge(0.0, 7200.0) == "2 h ago");

    CHECK(NotificationTargetLabel({NotificationTarget::Kind::Entity, "abc"}) == "Entity abc");
    CHECK(NotificationTargetLabel({NotificationTarget::Kind::Asset, "a/b.png"}) == "Asset a/b.png");
    CHECK(NotificationTargetLabel({NotificationTarget::Kind::File, "Scenes/Main.ascene"}) == "File Scenes/Main.ascene");
    CHECK(NotificationTargetLabel({}).empty());
    CHECK(std::string(NotificationSeverityName(NotificationSeverity::Warning)) == "Warning");
}

// Both halves draw headless; the toast stack shows nothing with no messages, and the panel marks what
// it lists read.
AETHER_TEST(NotificationOverlay_DrawsToastsAndMessages) {
    HeadlessImGui imgui;
    NotificationService service;
    u64 clicked = 1;
    imgui.Frame([&] { clicked = DrawNotificationToasts(service); });
    CHECK(clicked == 0);

    service.Post(NotificationSeverity::Info, "Info", "hello");
    service.Post(NotificationSeverity::Error, "Error", "boom", {NotificationTarget::Kind::Entity, "guid"});
    for (int i = 0; i < 6; ++i) {
        service.Post(NotificationSeverity::Success, "Saved " + std::to_string(i), "");
    }
    clicked = 1;
    imgui.Frame([&] { clicked = DrawNotificationToasts(service, 3); });
    CHECK(clicked == 0); // no input, so nothing was chosen

    CHECK(service.UnreadCount() == 8);
    imgui.Frame([&] { DrawMessagesPanel(service, 0.0); });
    CHECK(service.UnreadCount() == 0); // showing the list is reading it
}
