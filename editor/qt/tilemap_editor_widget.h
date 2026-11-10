#pragma once

#include "../src/sprite2d/tilemap_document.h"

#include <QWidget>

#include <filesystem>
#include <memory>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QWidget;

namespace aether::editor::qt {

// Native Qt authoring surface for a tilemap and its referenced tileset.
class TilemapEditorWidget final : public QWidget {
public:
    explicit TilemapEditorWidget(QWidget* parent = nullptr);
    bool OpenFile(const std::filesystem::path& tilemap_path, const std::filesystem::path& content_root);
    void ClearDocument();
    bool ConfirmDiscardChanges(QWidget* parent = nullptr) const;
    void SaveCurrent();
    void UndoCurrent();
    void RedoCurrent();
    bool dirty() const { return document_ && document_->dirty(); }

private:
    void Refresh();
    void SetStatus(const QString& text, bool error = false);

    std::unique_ptr<TilemapEditDocument> document_;
    std::filesystem::path tilemap_path_;
    std::filesystem::path tileset_path_;
    std::filesystem::path content_root_;
    QWidget* canvas_ = nullptr;
    QComboBox* tool_ = nullptr;
    QComboBox* layer_ = nullptr;
    QListWidget* palette_ = nullptr;
    QPushButton* undo_ = nullptr;
    QPushButton* redo_ = nullptr;
    QLabel* status_ = nullptr;
    bool refreshing_ = false;
};

} // namespace aether::editor::qt
