#include "ui/notification_overlay.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace aether::editor {

namespace {

ImVec4 SeverityColor(NotificationSeverity severity) {
    switch (severity) {
        case NotificationSeverity::Info: return ImVec4(0.72f, 0.84f, 1.0f, 1.0f);
        case NotificationSeverity::Success: return ImVec4(0.55f, 0.92f, 0.60f, 1.0f);
        case NotificationSeverity::Warning: return ImVec4(1.0f, 0.84f, 0.42f, 1.0f);
        case NotificationSeverity::Error: return ImVec4(1.0f, 0.55f, 0.55f, 1.0f);
    }
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

std::string TitleWithCount(const Notification& notification) {
    if (notification.count <= 1) return notification.title;
    return notification.title + " (x" + std::to_string(notification.count) + ")";
}

std::vector<const Notification*> Showing(const NotificationService& service) {
    std::vector<const Notification*> shown;
    for (const Notification& notification : service.All()) {
        if (service.IsToastShowing(notification.id)) shown.push_back(&notification);
    }
    return shown;
}

} // namespace

const char* NotificationSeverityName(NotificationSeverity severity) {
    switch (severity) {
        case NotificationSeverity::Info: return "Info";
        case NotificationSeverity::Success: return "Success";
        case NotificationSeverity::Warning: return "Warning";
        case NotificationSeverity::Error: return "Error";
    }
    return "Info";
}

std::string NotificationTargetLabel(const NotificationTarget& target) {
    switch (target.kind) {
        case NotificationTarget::Kind::None: return {};
        case NotificationTarget::Kind::Entity: return "Entity " + target.value;
        case NotificationTarget::Kind::Asset: return "Asset " + target.value;
        case NotificationTarget::Kind::File: return "File " + target.value;
    }
    return {};
}

std::string NotificationAge(double posted_at, double now) {
    const double seconds = std::max(0.0, now - posted_at);
    if (seconds < 1.0) return "just now";
    if (seconds < 60.0) return std::to_string(static_cast<int>(seconds)) + " s ago";
    if (seconds < 3600.0) return std::to_string(static_cast<int>(seconds / 60.0)) + " min ago";
    return std::to_string(static_cast<int>(seconds / 3600.0)) + " h ago";
}

u64 DrawNotificationToasts(NotificationService& service, int max_shown) {
    const std::vector<const Notification*> shown = Showing(service);
    if (shown.empty()) return 0;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 corner(viewport->WorkPos.x + viewport->WorkSize.x - 12.0f,
                        viewport->WorkPos.y + viewport->WorkSize.y - 12.0f);
    ImGui::SetNextWindowPos(corner, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    u64 clicked = 0;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
    if (ImGui::Begin("##notification_toasts", nullptr, flags)) {
        const int count = static_cast<int>(shown.size());
        const int visible = std::min(count, std::max(max_shown, 1));
        if (count > visible) ImGui::TextDisabled("+%d more", count - visible);
        for (int i = count - visible; i < count; ++i) {
            const Notification& notification = *shown[static_cast<std::size_t>(i)];
            ImGui::PushID(static_cast<int>(notification.id));
            ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(notification.severity));
            const std::string title = TitleWithCount(notification);
            if (ImGui::Selectable(title.c_str())) clicked = notification.id;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s: %s", NotificationSeverityName(notification.severity), notification.message.c_str());
            }
            if (!notification.message.empty()) {
                ImGui::SameLine();
                ImGui::TextUnformatted(notification.message.c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) service.Dismiss(notification.id);
            ImGui::PopID();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    return clicked;
}

NotificationTarget DrawMessagesPanel(NotificationService& service, double now) {
    NotificationTarget clicked;
    ImGui::TextDisabled("%zu messages, %zu unread", service.All().size(), service.UnreadCount());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) service.Clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("Mark all read")) service.MarkAllRead();
    ImGui::Separator();
    if (service.All().empty()) {
        ImGui::TextDisabled("No messages yet.");
        return clicked;
    }
    for (const Notification* notification : service.NewestFirst()) {
        ImGui::PushID(static_cast<int>(notification->id));
        ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(notification->severity));
        ImGui::TextUnformatted(NotificationSeverityName(notification->severity));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted(TitleWithCount(*notification).c_str());
        if (notification->count > 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("x%u", notification->count);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("  %s", NotificationAge(notification->posted_at, now).c_str());
        if (!notification->message.empty()) ImGui::TextWrapped("%s", notification->message.c_str());
        if (notification->target.IsSet()) {
            if (ImGui::Selectable(NotificationTargetLabel(notification->target).c_str())) clicked = notification->target;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open %s", NotificationTargetLabel(notification->target).c_str());
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    // Looking at the list is what "read" means; the count above is what arrived before this frame.
    service.MarkAllRead();
    return clicked;
}

} // namespace aether::editor
