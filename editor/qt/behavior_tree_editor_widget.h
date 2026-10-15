#pragma once

#include "ai/bt_document.h"
#include "behavior_tree_debug.h"

#include <QWidget>

#include <filesystem>
#include <functional>
#include <vector>

class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QListWidget;
class QComboBox;
class QTabWidget;

namespace aether::editor::qt {

class BehaviorTreeEditorWidget final : public QWidget {
public:
    explicit BehaviorTreeEditorWidget(QWidget* parent = nullptr);
    bool OpenFile(const std::filesystem::path& path);
    bool ConfirmDiscardChanges(QWidget* parent = nullptr) const;
    void ClearDocument();
    bool dirty() const { return document_.Dirty(); }
    void SaveCurrent();
    void UndoCurrent();
    void RedoCurrent();
    void SetLiveDebugSnapshots(const std::vector<BehaviorTreeDebugSnapshot>& snapshots);
    void SetDebugCommandHandler(std::function<void(Entity, const QString&)> handler);

private:
    void Refresh();
    void RefreshDetails();
    void SetStatus(const QString& message, bool error = false);
    void SaveAs();
    void AddNode();
    void MoveSelected(int offset);
    void RefreshBlackboard();
    void RefreshKeyDetails();
    void AddBlackboardKey();
    void AddNodeAttachment(bool service);
    void RefreshDebugger();
    aether::editor::BehaviorTreeDocument document_;
    QTreeWidget* tree_ = nullptr;
    QWidget* properties_ = nullptr;
    QVBoxLayout* properties_layout_ = nullptr;
    QLabel* validation_ = nullptr;
    QLabel* status_ = nullptr;
    QListWidget* blackboard_keys_ = nullptr;
    QWidget* blackboard_details_ = nullptr;
    QVBoxLayout* blackboard_details_layout_ = nullptr;
    QTabWidget* detail_tabs_ = nullptr;
    QComboBox* debug_entities_ = nullptr;
    QTreeWidget* debug_nodes_ = nullptr;
    QTreeWidget* debug_keys_ = nullptr;
    QLabel* debug_summary_ = nullptr;
    std::vector<BehaviorTreeDebugSnapshot> debug_snapshots_;
    std::function<void(Entity, const QString&)> debug_command_handler_;
    bool rebuilding_ = false;
};

} // namespace aether::editor::qt
