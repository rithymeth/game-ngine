#pragma once

#include "aether/animation/montage.h"

#include <QWidget>

#include <filesystem>
#include <vector>

class QLabel;
class QLineEdit;
class QDoubleSpinBox;
class QListWidget;
class QComboBox;

namespace aether::editor::qt {

class MontageEditorWidget final : public QWidget {
public:
    explicit MontageEditorWidget(QWidget* parent = nullptr);
    bool OpenFile(const std::filesystem::path& path);
    void ClearDocument();
    bool ConfirmDiscardChanges(QWidget* parent = nullptr) const;
    bool dirty() const { return dirty_; }
    void SaveCurrent();
    void UndoCurrent();
    void RedoCurrent();

private:
    void BuildUi();
    void Refresh();
    void Record();
    void EditSection();
    void SaveAs();
    void SetStatus(const QString& message, bool error = false);
    bool SaveTo(const std::filesystem::path& path);

    aether::anim::Montage montage_;
    std::filesystem::path path_;
    std::vector<aether::anim::Montage> undo_;
    std::vector<aether::anim::Montage> redo_;
    QListWidget* sections_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* clip_ = nullptr;
    QLineEdit* slot_ = nullptr;
    QDoubleSpinBox* blend_in_ = nullptr;
    QDoubleSpinBox* blend_out_ = nullptr;
    QLineEdit* section_name_ = nullptr;
    QDoubleSpinBox* section_start_ = nullptr;
    QComboBox* section_next_ = nullptr;
    QDoubleSpinBox* preview_time_ = nullptr;
    QLabel* preview_section_ = nullptr;
    QLabel* diagnostics_ = nullptr;
    QLabel* status_ = nullptr;
    int selected_ = -1;
    bool rebuilding_ = false;
    bool dirty_ = false;
};

} // namespace aether::editor::qt
