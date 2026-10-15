#pragma once

#include "graph/blueprint_document.h"

#include <QWidget>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

class QComboBox;
class QCheckBox;
class QPushButton;
class QGraphicsScene;
class QGraphicsView;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;

namespace aether::editor::qt {

class BlueprintEditorWidget final : public QWidget {
public:
    struct Breakpoint { std::filesystem::path blueprint; std::string graph; aether::bp::NodeId node = 0; };
    using BreakpointHandler = std::function<void(const std::filesystem::path&, const std::string&, aether::bp::NodeId, bool)>;

    explicit BlueprintEditorWidget(QWidget* parent = nullptr);
    ~BlueprintEditorWidget() override;

    bool OpenFile(const std::filesystem::path& path, bool confirm_changes = true);
    bool ConfirmDiscardChanges(QWidget* parent = nullptr) const;
    void ClearDocument();
    bool dirty() const;
    void SaveCurrent();
    void UndoCurrent();
    void RedoCurrent();
    void SetBreakpointHandler(BreakpointHandler handler) { breakpoint_handler_ = std::move(handler); }
    std::vector<Breakpoint> Breakpoints() const;
    void SetLiveTraceEvents(const std::vector<std::tuple<std::filesystem::path, std::string, aether::bp::NodeId>>& events);
    void ClearLiveTrace();

private:
    void RefreshGraphs();
    void RefreshCanvas();
    void RefreshPalette();
    void RefreshNodeDetails();
    void RefreshMembers();
    void RefreshMemberDetails();
    void AddVariable();
    void AddFunction(bool macro);
    void RenameMember();
    void RemoveMember();
    void Save();
    void Compile();
    void AddSelectedNode();
    void Undo();
    void Redo();
    void ConnectPins(aether::bp::NodeId from_node, const std::string& from_pin,
                     aether::bp::NodeId to_node, const std::string& to_pin);
    void CommitNodePositions(const std::vector<std::pair<aether::bp::NodeId, std::pair<float, float>>>& positions);
    void AddGraphComment();
    void CommitCommentPositions(const std::vector<std::tuple<int, float, float, float, float>>& positions);
    void EditSelectedCommentText();
    void EditSelectedCommentColor();
    void RemoveSelectedComment();
    void ToggleSelectedBreakpoint();
    void ApplyLiveTrace();
    void SetStatus(const QString& text, bool error = false);

    aether::editor::BlueprintDocument document_;
    QGraphicsScene* scene_ = nullptr;
    QGraphicsView* view_ = nullptr;
    QComboBox* graph_combo_ = nullptr;
    QLineEdit* palette_filter_ = nullptr;
    QListWidget* palette_list_ = nullptr;
    QPlainTextEdit* diagnostics_ = nullptr;
    QListWidget* members_list_ = nullptr;
    QFormLayout* member_details_form_ = nullptr;
    QLineEdit* member_default_ = nullptr;
    QLineEdit* member_tooltip_ = nullptr;
    QComboBox* member_type_ = nullptr;
    QCheckBox* member_instance_editable_ = nullptr;
    QCheckBox* member_expose_on_spawn_ = nullptr;
    QLabel* node_details_title_ = nullptr;
    QLineEdit* node_comment_ = nullptr;
    QLineEdit* graph_comment_text_ = nullptr;
    QPushButton* graph_comment_color_ = nullptr;
    QPushButton* graph_comment_remove_ = nullptr;
    QPushButton* breakpoint_toggle_ = nullptr;
    QFormLayout* pin_defaults_form_ = nullptr;
    aether::bp::NodeId selected_node_id_ = 0;
    int selected_comment_index_ = -1;
    std::map<std::string, std::set<aether::bp::NodeId>> breakpoints_;
    BreakpointHandler breakpoint_handler_;
    std::filesystem::path live_trace_asset_;
    std::string live_trace_graph_;
    aether::bp::NodeId live_trace_node_ = 0;
    std::set<aether::bp::NodeId> live_trace_fired_;
    QLabel* file_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    bool refreshing_ = false;
};

} // namespace aether::editor::qt
