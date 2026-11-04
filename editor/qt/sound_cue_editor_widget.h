#pragma once

#include "audio/cue_document.h"
#include "aether/audio/backend.h"
#include "aether/audio/cue_player.h"

#include <QWidget>
#include <filesystem>
#include <functional>
#include <memory>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;
class QGraphicsScene;
class QGraphicsView;
class QTabWidget;

namespace aether::editor::qt {

class SoundCueEditorWidget final : public QWidget {
public:
    explicit SoundCueEditorWidget(QWidget* parent = nullptr);
    bool OpenFile(const std::filesystem::path& path);
    void ClearDocument();
    bool ConfirmDiscardChanges(QWidget* parent = nullptr) const;
    void SaveCurrent();
    void UndoCurrent();
    void RedoCurrent();
    void SetContentRoot(const std::filesystem::path& root);
    bool dirty() const { return document_.Dirty(); }

private:
    void Refresh();
    void RefreshDetails();
    void RefreshGraph();
    void SetStatus(const QString& message, bool error = false);
    void SaveAs();
    void EditSelected(const std::function<void(aether::audio::CueNode&)>& edit, const std::string& label, const std::string& key);
    void LoadSoundBank();
    void PlayPreview();
    void StopPreview();
    void ShowMixer();

    aether::editor::SoundCueDocument document_;
    aether::audio::SoundCue preview_asset_cue_;
    aether::audio::Mixer mixer_;
    aether::audio::SoundBank sound_bank_;
    std::unique_ptr<aether::audio::CuePlayer> cue_player_;
    std::unique_ptr<aether::audio::AudioOutput> audio_output_;
    std::filesystem::path content_root_;
    QListWidget* nodes_ = nullptr;
    QTabWidget* node_tabs_ = nullptr;
    QGraphicsScene* graph_scene_ = nullptr;
    QGraphicsView* graph_view_ = nullptr;
    QWidget* details_ = nullptr;
    QLineEdit* cue_name_ = nullptr;
    QLabel* diagnostics_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* preview_status_ = nullptr;
    QPushButton* preview_button_ = nullptr;
    QPushButton* preview_asset_button_ = nullptr;
    QTimer* preview_timer_ = nullptr;
    aether::audio::CueHandle preview_handle_ = 0;
    QComboBox* node_type_ = nullptr;
    int selected_row_ = -1;
    bool rebuilding_ = false;
    bool graph_view_fitted_ = false;
};

} // namespace aether::editor::qt
