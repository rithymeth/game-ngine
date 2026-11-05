#pragma once

#include "aether/core/log.h"

#include <QPlainTextEdit>

namespace aether::editor::qt {

// The Qt editor's OUTPUT dock (Phase 43.1 extraction): a read-only console that
// shows the engine log's recent lines and follows new ones.
class EditorLogConsole final : public QPlainTextEdit {
public:
    explicit EditorLogConsole(QWidget* parent = nullptr);
    ~EditorLogConsole() override;

private:
    void Append(const LogLine& line);
    int sink_id_ = 0;
};

} // namespace aether::editor::qt
