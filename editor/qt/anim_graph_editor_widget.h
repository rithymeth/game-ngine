#pragma once

#ifdef slots
#pragma push_macro("slots")
#undef slots
#define AETHER_RESTORE_QT_SLOTS
#endif
#include "anim/anim_document.h"
#ifdef AETHER_RESTORE_QT_SLOTS
#pragma pop_macro("slots")
#undef AETHER_RESTORE_QT_SLOTS
#endif

#include <QWidget>
#include <filesystem>
#include <optional>

class QComboBox;
class QDoubleSpinBox;
class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QListWidget;
class QPushButton;
class QVBoxLayout;

namespace aether::editor::qt {

class AnimGraphEditorWidget final : public QWidget {
public:
    explicit AnimGraphEditorWidget(QWidget* parent = nullptr);
    bool OpenFile(const std::filesystem::path& path);
    bool ConfirmDiscardChanges(QWidget* parent = nullptr) const;
    void ClearDocument();
    void SaveCurrent();
    void UndoCurrent();
    void RedoCurrent();
    bool dirty() const { return document_.Dirty(); }

private:
    void NewGraph();
    void SaveAs();
    void Refresh();
    void RefreshDetails();
    void RefreshDiagnostics();
    void SelectNode(u32 id);
    void AddNode(aether::anim::AnimNodeKind kind);
    void CommitNodePosition();
    void EditNode(const QString& label, const std::function<void(aether::anim::AnimNode&)>& edit,
                  const QString& merge = {});
    void SetStatus(const QString& message, bool error = false);
    aether::editor::AnimGraphDocument document_;
    QGraphicsScene* scene_ = nullptr;
    QGraphicsView* view_ = nullptr;
    QListWidget* variables_ = nullptr;
    QWidget* details_ = nullptr;
    QVBoxLayout* details_layout_ = nullptr;
    QListWidget* diagnostics_ = nullptr;
    QLabel* status_ = nullptr;
    u32 selected_node_ = 0;
    std::string selected_variable_;
    bool rebuilding_ = false;
};

} // namespace aether::editor::qt
