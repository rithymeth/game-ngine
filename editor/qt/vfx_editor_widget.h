#pragma once

#ifdef slots
#pragma push_macro("slots")
#undef slots
#define AETHER_RESTORE_QT_SLOTS
#endif
#include "vfx/vfx_document.h"
#include "vfx/vfx_preview.h"
#ifdef AETHER_RESTORE_QT_SLOTS
#pragma pop_macro("slots")
#undef AETHER_RESTORE_QT_SLOTS
#endif

#include <QWidget>
#include <filesystem>
#include <optional>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QSlider;
class QTimer;
class QLabel;
class QTabWidget;

namespace aether::editor::qt {

class ParticlePreviewCanvas;

// Qt particle system authoring dock for .avfx assets.
class VfxEditorWidget final : public QWidget {
public:
    explicit VfxEditorWidget(QWidget* parent = nullptr);
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
    void RefreshSystemTools();
    void RefreshDiagnostics();
    void SelectEmitter(int row);
    void SelectModule(QTreeWidgetItem* item);
    void AddEmitter();
    void AddModule();
    void EditJsonField(const std::string& key, const nlohmann::json& value);
    void SyncPreview();
    void InspectGpuShader();
    void SetStatus(const QString& text, bool error = false);
    void SaveAs();

    aether::editor::ParticleSystemDocument document_;
    aether::editor::ParticlePreview preview_;
    QListWidget* emitters_ = nullptr;
    QTreeWidget* stack_ = nullptr;
    QWidget* details_ = nullptr;
    QVBoxLayout* details_layout_ = nullptr;
    ParticlePreviewCanvas* canvas_ = nullptr;
    QSlider* timeline_ = nullptr;
    QComboBox* speed_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* stats_ = nullptr;
    QLabel* gpu_status_ = nullptr;
    QListWidget* diagnostics_ = nullptr;
    QTabWidget* system_tabs_ = nullptr;
    QListWidget* parameter_list_ = nullptr;
    QListWidget* binding_list_ = nullptr;
    QListWidget* sub_emitter_list_ = nullptr;
    QPushButton* play_ = nullptr;
    QTimer* timer_ = nullptr;
    usize emitter_index_ = 0;
    std::optional<aether::editor::ModuleRef> module_ref_;
    bool playing_ = false;
    bool rebuilding_ = false;
};

} // namespace aether::editor::qt
