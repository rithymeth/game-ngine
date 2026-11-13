#pragma once

// Stage 0 of docs/design/EDITOR_PARITY_AUDIT.md: one place for the editor to say something happened.
//
// A message is kept in a history the Messages panel lists and is also shown as a toast until it
// expires or is dismissed. Errors can carry the entity, asset or file they are about, which is what
// makes the Messages list a "fix these problems" list rather than a log. No ImGui here: the drawing
// lives in ui/notification_overlay.{h,cpp}, so this half is headless-testable.

#include "aether/core/base.h"

#include <string>
#include <vector>

namespace aether::editor {

enum class NotificationSeverity {
    Info,
    Success,
    Warning,
    Error,
};

// What a message points at. An entity is its GUID text and an asset its content-relative path, so
// the service holds no ECS handles and no panel pointers.
struct NotificationTarget {
    enum class Kind { None, Entity, Asset, File };

    Kind kind = Kind::None;
    std::string value;

    bool IsSet() const { return kind != Kind::None && !value.empty(); }
};

struct Notification {
    u64 id = 0;
    NotificationSeverity severity = NotificationSeverity::Info;
    std::string title;
    std::string message;
    NotificationTarget target;
    // Identical messages posted back to back join the newest row instead of adding one, so a
    // warning that repeats every autosave interval is one line with a count.
    u32 count = 1;
    double posted_at = 0.0;
    // 0: never expires (errors). Otherwise the toast stops showing at this time; the row stays.
    double expires_at = 0.0;
    bool unread = true;
};

class NotificationService {
public:
    // Info and success toasts last 4 s, warnings 8 s, errors until dismissed. 0 means never.
    static double Lifetime(NotificationSeverity severity);

    // Posts a message and shows it as a toast. Returns its id.
    u64 Post(NotificationSeverity severity, std::string title, std::string message,
             NotificationTarget target = {});

    // Advances the clock and stops showing toasts that are past their lifetime; the history keeps them.
    void Update(double now);

    // Hides a toast now. Its row stays in the Messages list.
    void Dismiss(u64 id);

    const std::vector<Notification>& All() const { return messages_; }
    const Notification* Find(u64 id) const;
    bool IsToastShowing(u64 id) const;

    // The history newest first, for a panel that reads top-down. Pointers stay valid until the next
    // Post or Clear.
    std::vector<const Notification*> NewestFirst() const;

    std::size_t UnreadCount() const;
    void MarkAllRead();
    void Clear();

    // The oldest rows go first, with their toasts, once the history is this long.
    static constexpr std::size_t kMaxMessages = 256;

private:
    void StopShowing(u64 id);

    std::vector<Notification> messages_; // oldest first
    std::vector<u64> toasts_;            // ids in posting order
    u64 next_id_ = 1;
    double now_ = 0.0;
};

} // namespace aether::editor
