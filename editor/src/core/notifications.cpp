#include "core/notifications.h"

#include <algorithm>
#include <utility>

namespace aether::editor {

double NotificationService::Lifetime(NotificationSeverity severity) {
    switch (severity) {
        case NotificationSeverity::Info: return 4.0;
        case NotificationSeverity::Success: return 4.0;
        case NotificationSeverity::Warning: return 8.0;
        case NotificationSeverity::Error: return 0.0;
    }
    return 0.0;
}

u64 NotificationService::Post(NotificationSeverity severity, std::string title, std::string message,
                              NotificationTarget target) {
    if (!messages_.empty()) {
        Notification& newest = messages_.back();
        if (newest.severity == severity && newest.title == title && newest.message == message &&
            newest.target.kind == target.kind && newest.target.value == target.value) {
            ++newest.count;
            newest.posted_at = now_;
            newest.unread = true;
            newest.expires_at = Lifetime(severity) == 0.0 ? 0.0 : now_ + Lifetime(severity);
            if (!IsToastShowing(newest.id)) toasts_.push_back(newest.id);
            return newest.id;
        }
    }
    Notification notification;
    notification.id = next_id_++;
    notification.severity = severity;
    notification.title = std::move(title);
    notification.message = std::move(message);
    notification.target = std::move(target);
    notification.posted_at = now_;
    const double lifetime = Lifetime(severity);
    notification.expires_at = lifetime == 0.0 ? 0.0 : now_ + lifetime;
    const u64 id = notification.id;
    messages_.push_back(std::move(notification));
    toasts_.push_back(id);
    if (messages_.size() > kMaxMessages) {
        StopShowing(messages_.front().id);
        messages_.erase(messages_.begin());
    }
    return id;
}

void NotificationService::Update(double now) {
    now_ = now;
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [this](u64 id) {
        const Notification* notification = Find(id);
        return notification == nullptr ||
               (notification->expires_at != 0.0 && now_ >= notification->expires_at);
    }), toasts_.end());
}

void NotificationService::Dismiss(u64 id) { StopShowing(id); }

const Notification* NotificationService::Find(u64 id) const {
    for (const Notification& notification : messages_) {
        if (notification.id == id) return &notification;
    }
    return nullptr;
}

bool NotificationService::IsToastShowing(u64 id) const {
    return std::find(toasts_.begin(), toasts_.end(), id) != toasts_.end();
}

std::vector<const Notification*> NotificationService::NewestFirst() const {
    std::vector<const Notification*> ordered;
    ordered.reserve(messages_.size());
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) ordered.push_back(&*it);
    return ordered;
}

std::size_t NotificationService::UnreadCount() const {
    return static_cast<std::size_t>(std::count_if(messages_.begin(), messages_.end(),
                                                  [](const Notification& n) { return n.unread; }));
}

void NotificationService::MarkAllRead() {
    for (Notification& notification : messages_) notification.unread = false;
}

void NotificationService::Clear() {
    messages_.clear();
    toasts_.clear();
}

void NotificationService::StopShowing(u64 id) {
    toasts_.erase(std::remove(toasts_.begin(), toasts_.end(), id), toasts_.end());
}

} // namespace aether::editor
