#pragma once

#ifdef slots
#pragma push_macro("slots")
#undef slots
#define AETHER_RESTORE_QT_SLOTS
#endif
#include "graph/material_document.h"
#ifdef AETHER_RESTORE_QT_SLOTS
#pragma pop_macro("slots")
#undef AETHER_RESTORE_QT_SLOTS
#endif

#include <QWidget>
#include <QPointF>

#include <filesystem>
#include <map>

class QComboBox;
class QDoubleSpinBox;
class QGraphicsScene;
class QGraphicsItem;
class QGraphicsView;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QFormLayout;

namespace aether::editor::qt {

// Qt authoring surface for project material assets.
class MaterialEditorWidget final : public QWidget {
public:
    explicit MaterialEditorWidget(QWidget* parent = nullptr);
    bool OpenFile(const std::filesystem::path& path);
    bool ConfirmDiscardChanges(QWidget* parent = nullptr) const;
    void ClearDocument();
    void SaveCurrent();
    void UndoCurrent();
    void RedoCurrent();
    bool dirty() const { return document_.Dirty(); }

private:
    void Refresh();
    void RefreshDetails();
    void RefreshPalette();
    void AddSelectedNode();
    void ConnectPins(QGraphicsItem* a, QGraphicsItem* b);
    void CommitPositions(const std::map<aether::mat::NodeId, QPointF>& positions);
    void SetStatus(const QString& text, bool error = false);

    aether::editor::MaterialDocument document_;
    QGraphicsScene* scene_ = nullptr;
    QGraphicsView* view_ = nullptr;
    QListWidget* parameters_ = nullptr;
    QLineEdit* palette_filter_ = nullptr;
    QListWidget* palette_ = nullptr;
    QWidget* details_ = nullptr;
    QFormLayout* details_form_ = nullptr;
    QPlainTextEdit* diagnostics_ = nullptr;
    QPlainTextEdit* shader_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* save_ = nullptr;
    bool refreshing_ = false;
    aether::mat::NodeId selected_node_id_ = 0;
};

} // namespace aether::editor::qt
