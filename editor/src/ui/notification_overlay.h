#pragma once

// Draws what core/notifications.{h,cpp} records. Stage 0 of the parity audit: the toast stack that
// used to be an error-only overlay now shows every severity and is fed by the notification service,
// and the Messages panel lists the same history with links back to the entity, asset or file.

#include "aether/core/base.h"
#include "core/notifications.h"

#include <string>

namespace aether::editor {

const char* NotificationSeverityName(NotificationSeverity severity);
// "Entity <guid>", "Asset <path>", "File <path>"; empty when the target is unset.
std::string NotificationTargetLabel(const NotificationTarget& target);
// "just now", "12 s ago", "3 min ago", "2 h ago".
std::string NotificationAge(double posted_at, double now);

// The live toasts in the bottom-right corner of the main viewport, oldest first, up to `max_shown`
// with a "+N more" line. Each has a close button; a click on a toast returns its id (0 when none)
// so the host can open Messages or the toast's target.
u64 DrawNotificationToasts(NotificationService& service, int max_shown = 4);

// The Messages panel: newest first, with severity, count, age and a clickable target. Drawing the
// list marks every message read, so the unread count is the badge for "something arrived since you
// last looked". Returns the target a row was clicked on this frame (unset when none).
NotificationTarget DrawMessagesPanel(NotificationService& service, double now);

} // namespace aether::editor
