#pragma once

#include "ai/ai_debugger.h"

#include <QWidget>

#include <functional>
#include <vector>

class QCheckBox;
class QLabel;
class QTableWidget;

namespace aether::editor::qt {

// M8.8 AI Debugger dock: during Play-in-Editor, one row per AI agent (name,
// reported state, perception target, known actors, path status). Rows come
// from editor::BuildAiDebugRows; this widget only shows them. Picking a row
// asks the editor to select that entity.
class AiDebuggerWidget final : public QWidget {
public:
    explicit AiDebuggerWidget(QWidget* parent = nullptr);

    void SetRows(const std::vector<AiDebugRow>& rows); // keeps the selected entity's row selected
    void Clear(); // Play stopped
    void SetSelectedEntity(Entity entity); // follows the outliner without calling back
    bool ShowAllAgents() const;
    void SetEntityActivatedCallback(std::function<void(Entity)> callback) { activated_ = std::move(callback); }
    void SetOverlayChangedCallback(std::function<void()> callback) { overlay_changed_ = std::move(callback); }

private:
    void SyncSelection();
    QTableWidget* table_ = nullptr;
    QCheckBox* show_all_ = nullptr;
    QLabel* status_ = nullptr;
    std::vector<Entity> entities_; // per row
    Entity selected_;
    bool updating_ = false;
    std::function<void(Entity)> activated_;
    std::function<void()> overlay_changed_;
};

} // namespace aether::editor::qt
