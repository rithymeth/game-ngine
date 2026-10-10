#pragma once

#include "aether/assets/gltf_loader.h"

#include <QWidget>

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTreeWidget;
class QTimer;

namespace aether::editor::qt {

class AnimationEditorWidget final : public QWidget {
public:
    explicit AnimationEditorWidget(std::function<void(const QString&)> status, QWidget* parent = nullptr);
    bool ConfirmDiscardChanges(QWidget* parent = nullptr);

private:
    struct PreviewWidget;
    struct TimelineWidget;

    void BuildUi();
    bool OpenModel(const std::filesystem::path& path, bool confirm = true);
    bool OpenClipFile(const std::filesystem::path& path, bool confirm = true);
    bool SaveClip(bool save_as = false);
    bool ConfirmDiscard(QWidget* parent = nullptr) const;
    void SelectAnimation(int index);
    void CreateAnimation();
    void RebuildAnimationList(int selected = -1);
    void RebuildRigTree();
    void SelectNode(int node);
    void RefreshPlayhead(bool update_controls = true);
    void RefreshTimeline();
    void RefreshPreview();
    void RefreshControls();
    void SetPlayhead(float time, bool snap_to_frame = false);
    void AddOrUpdateKey();
    void DeleteSelectedKey();
    void MoveSelectedKey(int path, int key, float time);
    void SetInterpolation(int interpolation);
    void SetClipName();
    void SetClipDuration();
    void RecordUndo();
    bool Undo();
    bool Redo();
    aether::assets::GltfAnimation* ActiveAnimation();
    const aether::assets::GltfAnimation* ActiveAnimation() const;
    int FindChannel(int node, aether::assets::GltfAnimationPath path) const;
    void ScrubTo(float time);
    void EmitStatus(const QString& message) const;

    aether::assets::GltfScene scene_;
    std::filesystem::path source_model_path_;
    std::filesystem::path clip_file_path_;
    std::vector<aether::assets::GltfAnimation> undo_;
    std::vector<aether::assets::GltfAnimation> redo_;
    std::function<void(const QString&)> status_;

    QTreeWidget* rig_tree_ = nullptr;
    QComboBox* animation_combo_ = nullptr;
    QLineEdit* animation_name_ = nullptr;
    QDoubleSpinBox* duration_spin_ = nullptr;
    QDoubleSpinBox* fps_spin_ = nullptr;
    QDoubleSpinBox* time_spin_ = nullptr;
    QComboBox* view_combo_ = nullptr;
    QComboBox* interpolation_combo_ = nullptr;
    QDoubleSpinBox* transform_fields_[9]{};
    QPushButton* play_button_ = nullptr;
    QPushButton* loop_button_ = nullptr;
    QPushButton* snap_button_ = nullptr;
    QPushButton* add_key_button_ = nullptr;
    QPushButton* delete_key_button_ = nullptr;
    QPushButton* undo_button_ = nullptr;
    QPushButton* redo_button_ = nullptr;
    QLabel* source_label_ = nullptr;
    QLabel* selected_node_label_ = nullptr;
    QLabel* clip_status_ = nullptr;
    PreviewWidget* preview_ = nullptr;
    TimelineWidget* timeline_ = nullptr;
    QTimer* timer_ = nullptr;

    int animation_index_ = -1;
    int selected_node_ = -1;
    int selected_path_ = 0;
    int selected_key_ = -1;
    float playhead_ = 0.0f;
    bool playing_ = false;
    bool looping_ = true;
    bool snapping_ = true;
    bool dirty_ = false;
    bool recording_key_drag_ = false;
};

} // namespace aether::editor::qt
