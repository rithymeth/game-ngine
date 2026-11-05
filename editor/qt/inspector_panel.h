#pragma once

#include <QWidget>

#include <array>

class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

namespace aether::editor::qt {

struct CameraInspectorWidgets {
    QGroupBox* group = nullptr;
    QComboBox* projection = nullptr;
    QDoubleSpinBox* fov = nullptr;
    QDoubleSpinBox* ortho_height = nullptr;
    QDoubleSpinBox* near_plane = nullptr;
    QDoubleSpinBox* far_plane = nullptr;
    QSpinBox* priority = nullptr;
    QGroupBox* cine_group = nullptr;
    QDoubleSpinBox* focal_length = nullptr;
    QDoubleSpinBox* sensor_width = nullptr;
    QDoubleSpinBox* sensor_height = nullptr;
    QDoubleSpinBox* aperture = nullptr;
    QDoubleSpinBox* focus_distance = nullptr;
};

// The Mesh section of the Inspector: what a placed model cooks to (LOD and meshlet counts).
struct MeshInspectorWidgets {
    QGroupBox* group = nullptr;
    QLabel* summary = nullptr;
    QTableWidget* table = nullptr;
    QLabel* note = nullptr;
};

// The Qt editor's INSPECTOR dock (Phase 43.1 extraction): the no-selection page,
// the transform editor and the per-component pages.
QWidget* MakeInspector(QScrollArea*& inspector_scroll, QStackedWidget*& inspector_stack,
                       QPushButton*& empty_add_entity,
                       QLabel*& selection_status, QLineEdit*& entity_name,
                       std::array<QDoubleSpinBox*, 3>& position,
                       std::array<QDoubleSpinBox*, 3>& rotation,
                       std::array<QDoubleSpinBox*, 3>& scale,
                       QPushButton*& reset_position, QPushButton*& reset_rotation,
                       QPushButton*& reset_scale,
                       CameraInspectorWidgets& camera, MeshInspectorWidgets& mesh);

} // namespace aether::editor::qt