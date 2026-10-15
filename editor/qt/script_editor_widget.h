#pragma once

#include <QWidget>

#include <filesystem>
#include <memory>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace aether::script { class LuauHost; }

namespace aether::editor::qt {

class ScriptEditorWidget final : public QWidget {
public:
    explicit ScriptEditorWidget(QWidget* parent = nullptr);
    ~ScriptEditorWidget() override;

    bool OpenFile(const std::filesystem::path& path, bool confirm_changes = true);
    bool ConfirmDiscardChanges(QWidget* parent = nullptr) const;
    void ClearDocument();
    bool dirty() const;

private:
    bool Save();
    void Validate();
    void UpdateTitle();

    std::filesystem::path file_path_;
    std::unique_ptr<script::LuauHost> host_;
    QPushButton* validate_button_ = nullptr;
    QLabel* file_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QPlainTextEdit* editor_ = nullptr;
    QPushButton* save_button_ = nullptr;
};

} // namespace aether::editor::qt
