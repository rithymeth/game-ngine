#pragma once

#ifdef slots
#pragma push_macro("slots")
#undef slots
#define AETHER_RESTORE_QT_SLOTS
#endif
#include "anim/blend_space_document.h"
#ifdef AETHER_RESTORE_QT_SLOTS
#pragma pop_macro("slots")
#undef AETHER_RESTORE_QT_SLOTS
#endif

#include <QWidget>
#include <filesystem>

class QLabel;
class QListWidget;
class QVBoxLayout;

namespace aether::editor::qt {
class BlendSpaceCanvasWidget;

class BlendSpaceEditorWidget final : public QWidget {
public:
    explicit BlendSpaceEditorWidget(QWidget* parent = nullptr);
    bool OpenFile(const std::filesystem::path& path);
    bool ConfirmDiscardChanges(QWidget* parent = nullptr) const;
    void ClearDocument();
    void SaveCurrent();
    void UndoCurrent();
    void RedoCurrent();
    bool dirty() const { return document_.Dirty(); }
    void Refresh();
private:
    friend class BlendSpaceCanvasWidget;
    void SaveAs();
    void SetStatus(const QString& text, bool error = false);
    BlendSpaceDocument document_;
    BlendSpaceCanvasWidget* canvas_ = nullptr;
    QListWidget* samples_ = nullptr;
    QWidget* details_ = nullptr;
    QVBoxLayout* details_layout_ = nullptr;
    QLabel* status_ = nullptr;
    int selected_ = -1;
    int snap_divisions_ = 4;
    float preview_x_ = 0, preview_y_ = 0;
    bool rebuilding_ = false;
};
} // namespace aether::editor::qt
