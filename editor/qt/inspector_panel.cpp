#include "inspector_panel.h"

#include "aether/scene/gameplay.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace aether::editor::qt {

QWidget* MakeInspector(QScrollArea*& inspector_scroll, QStackedWidget*& inspector_stack,
                       QPushButton*& empty_add_entity,
                       QLabel*& selection_status, QLineEdit*& entity_name,
                       std::array<QDoubleSpinBox*, 3>& position,
                       std::array<QDoubleSpinBox*, 3>& rotation,
                       std::array<QDoubleSpinBox*, 3>& scale,
                       QPushButton*& reset_position, QPushButton*& reset_rotation,
                       QPushButton*& reset_scale,
                       CameraInspectorWidgets& camera, MeshInspectorWidgets& mesh) {
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);
    selection_status = new QLabel("NO SELECTION Â· SELECT AN ENTITY");
    selection_status->setObjectName("section");
    layout->addWidget(selection_status);
    entity_name = new QLineEdit;
    entity_name->setObjectName("entityNameField");
    entity_name->setAccessibleName("Selected entity name");
    entity_name->setPlaceholderText("Entity name");
    entity_name->setToolTip("Rename the selected scene entity");
    entity_name->setEnabled(false);
    layout->addWidget(entity_name);

    auto* transform_header = new QHBoxLayout;
    auto* transform_title = new QLabel("Transform");
    transform_title->setStyleSheet("font-weight: 700; color: #aebdca; padding-top: 8px;");
    reset_position = new QPushButton("Reset Position");
    reset_position->setToolTip("Set local position to the origin. This edit can be undone.");
    reset_position->setEnabled(false);
    transform_header->addWidget(transform_title);
    transform_header->addStretch();
    transform_header->addWidget(reset_position);
    layout->addLayout(transform_header);
    auto* transform_form = new QFormLayout;
    transform_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    transform_form->setHorizontalSpacing(12);
    transform_form->setVerticalSpacing(6);
    const auto make_axis_label = [](int axis, const QString& caption = QString{}) {
        auto* row = new QWidget;
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(6);
        if (!caption.isEmpty()) {
            auto* text = new QLabel(caption, row);
            text->setObjectName("axisCaption");
            row_layout->addWidget(text);
        }
        auto* badge = new QLabel(QString(QChar('X' + axis)), row);
        badge->setObjectName(QString("axis%1").arg(QChar('X' + axis)));
        badge->setAlignment(Qt::AlignCenter);
        row_layout->addWidget(badge);
        row_layout->addStretch();
        return row;
    };
    for (int axis = 0; axis < 3; ++axis) {
        position[axis] = new QDoubleSpinBox;
        position[axis]->setRange(-100000.0, 100000.0);
        position[axis]->setDecimals(2);
        position[axis]->setSingleStep(0.1);
        position[axis]->setSuffix(" m");
        position[axis]->setKeyboardTracking(false);
        position[axis]->setAccessibleName(QString("Local position %1 in meters").arg(QChar('X' + axis)));
        position[axis]->setToolTip(QString("Local %1 position in meters").arg(QChar('X' + axis)));
        position[axis]->setEnabled(false);
        transform_form->addRow(make_axis_label(axis), position[axis]);
    }
    layout->addLayout(transform_form);

    auto* rotation_header = new QHBoxLayout;
    auto* rotation_title = new QLabel("Rotation Â· degrees");
    rotation_title->setObjectName("muted");
    reset_rotation = new QPushButton("Reset Rotation");
    reset_rotation->setToolTip("Set local rotation to zero. This edit can be undone.");
    reset_rotation->setEnabled(false);
    rotation_header->addWidget(rotation_title);
    rotation_header->addStretch();
    rotation_header->addWidget(reset_rotation);
    layout->addLayout(rotation_header);
    auto* rotation_form = new QFormLayout;
    rotation_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    rotation_form->setHorizontalSpacing(12);
    rotation_form->setVerticalSpacing(6);
    const char* rotation_labels[] = {"PITCH", "YAW", "ROLL"};
    for (int axis = 0; axis < 3; ++axis) {
        rotation[axis] = new QDoubleSpinBox;
        rotation[axis]->setRange(-36000.0, 36000.0);
        rotation[axis]->setDecimals(2);
        rotation[axis]->setSingleStep(1.0);
        rotation[axis]->setSuffix("Â°");
        rotation[axis]->setKeyboardTracking(false);
        rotation[axis]->setAccessibleName(QString("Local %1 rotation in degrees").arg(QChar('X' + axis)));
        rotation[axis]->setToolTip(QString("Local %1 rotation in degrees").arg(QChar('X' + axis)));
        rotation[axis]->setEnabled(false);
        rotation_form->addRow(make_axis_label(axis, QString::fromUtf8(rotation_labels[axis])), rotation[axis]);
    }
    layout->addLayout(rotation_form);

    auto* scale_header = new QHBoxLayout;
    auto* scale_title = new QLabel("Scale Â· local multiplier");
    scale_title->setObjectName("muted");
    reset_scale = new QPushButton("Reset Scale");
    reset_scale->setToolTip("Restore the imported object size (1Ã— on each axis). This edit can be undone.");
    reset_scale->setEnabled(false);
    scale_header->addWidget(scale_title);
    scale_header->addStretch();
    scale_header->addWidget(reset_scale);
    layout->addLayout(scale_header);
    auto* scale_form = new QFormLayout;
    scale_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    scale_form->setHorizontalSpacing(12);
    scale_form->setVerticalSpacing(6);
    for (int axis = 0; axis < 3; ++axis) {
        scale[axis] = new QDoubleSpinBox;
        scale[axis]->setRange(-10000.0, 10000.0);
        scale[axis]->setDecimals(3);
        scale[axis]->setSingleStep(0.1);
        scale[axis]->setSuffix("Ã—");
        scale[axis]->setKeyboardTracking(false);
        scale[axis]->setAccessibleName(QString("Local scale %1").arg(QChar('X' + axis)));
        scale[axis]->setToolTip(QString("Local %1 scale multiplier; 1 preserves the imported size").arg(QChar('X' + axis)));
        scale[axis]->setEnabled(false);
        scale_form->addRow(make_axis_label(axis), scale[axis]);
    }
    layout->addLayout(scale_form);

    camera.group = new QGroupBox("Camera", body);
    auto* camera_form = new QFormLayout(camera.group);
    camera_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    camera_form->setHorizontalSpacing(12);
    camera_form->setVerticalSpacing(6);
    camera.projection = new QComboBox(camera.group);
    camera.projection->addItem("Perspective", static_cast<int>(aether::Projection::Perspective));
    camera.projection->addItem("Orthographic", static_cast<int>(aether::Projection::Orthographic));
    camera.projection->setObjectName("cameraProjection");
    camera_form->addRow("Projection", camera.projection);
    const auto make_camera_spin = [camera](const QString& object_name, double minimum, double maximum,
                                            int decimals, double step, const QString& suffix) {
        auto* spin = new QDoubleSpinBox(camera.group);
        spin->setObjectName(object_name);
        spin->setRange(minimum, maximum);
        spin->setDecimals(decimals);
        spin->setSingleStep(step);
        spin->setSuffix(suffix);
        spin->setKeyboardTracking(false);
        return spin;
    };
    camera.fov = make_camera_spin("cameraFov", 1.0, 179.0, 2, 1.0, "Â°");
    camera.fov->setToolTip("Vertical field of view for perspective projection");
    camera.ortho_height = make_camera_spin("cameraOrthoHeight", 0.01, 100000.0, 2, 0.1, " m");
    camera.ortho_height->setToolTip("Vertical view height for orthographic projection");
    camera.near_plane = make_camera_spin("cameraNearPlane", 0.001, 100000.0, 3, 0.1, " m");
    camera.near_plane->setToolTip("Distance to the nearest visible point");
    camera.far_plane = make_camera_spin("cameraFarPlane", 0.01, 1000000.0, 2, 1.0, " m");
    camera.far_plane->setToolTip("Distance to the furthest visible point");
    camera.priority = new QSpinBox(camera.group);
    camera.priority->setObjectName("cameraPriority");
    camera.priority->setRange(-100000, 100000);
    camera.priority->setToolTip("Priority used when choosing the active camera");
    camera_form->addRow("Field of view", camera.fov);
    camera_form->addRow("Orthographic height", camera.ortho_height);
    camera_form->addRow("Near clip", camera.near_plane);
    camera_form->addRow("Far clip", camera.far_plane);
    camera_form->addRow("Priority", camera.priority);
    camera.group->setVisible(false);
    layout->addWidget(camera.group);

    camera.cine_group = new QGroupBox("Cinematic Lens", body);
    auto* cine_form = new QFormLayout(camera.cine_group);
    cine_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    cine_form->setHorizontalSpacing(12);
    cine_form->setVerticalSpacing(6);
    const auto make_lens_spin = [cine_group = camera.cine_group](const QString& name, double min, double max,
                                                                  double step, const QString& suffix) {
        auto* spin = new QDoubleSpinBox(cine_group);
        spin->setObjectName(name);
        spin->setRange(min, max);
        spin->setDecimals(2);
        spin->setSingleStep(step);
        spin->setSuffix(suffix);
        spin->setKeyboardTracking(false);
        return spin;
    };
    camera.focal_length = make_lens_spin("cineFocalLength", 1.0, 1000.0, 1.0, " mm");
    camera.sensor_width = make_lens_spin("cineSensorWidth", 1.0, 100.0, 0.5, " mm");
    camera.sensor_height = make_lens_spin("cineSensorHeight", 1.0, 100.0, 0.5, " mm");
    camera.aperture = make_lens_spin("cineAperture", 0.7, 64.0, 0.1, " f");
    camera.focus_distance = make_lens_spin("cineFocusDistance", 0.01, 100000.0, 0.1, " m");
    cine_form->addRow("Focal length", camera.focal_length);
    cine_form->addRow("Sensor width", camera.sensor_width);
    cine_form->addRow("Sensor height", camera.sensor_height);
    cine_form->addRow("Aperture", camera.aperture);
    cine_form->addRow("Focus distance", camera.focus_distance);
    camera.cine_group->setVisible(false);
    layout->addWidget(camera.cine_group);

    mesh.group = new QGroupBox("Mesh", body);
    mesh.group->setObjectName("meshInspectorGroup");
    auto* mesh_layout = new QVBoxLayout(mesh.group);
    mesh_layout->setSpacing(6);
    mesh.summary = new QLabel(mesh.group);
    mesh.summary->setObjectName("meshInspectorSummary");
    mesh.summary->setWordWrap(true);
    mesh_layout->addWidget(mesh.summary);
    mesh.table = new QTableWidget(0, 4, mesh.group);
    mesh.table->setObjectName("meshLodTable");
    mesh.table->setHorizontalHeaderLabels({"LOD", "Triangles", "Error", "Meshlets"});
    mesh.table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mesh.table->setSelectionMode(QAbstractItemView::NoSelection);
    mesh.table->verticalHeader()->setVisible(false);
    mesh.table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    mesh.table->setAccessibleName("Level of detail table");
    mesh_layout->addWidget(mesh.table);
    mesh.note = new QLabel(mesh.group);
    mesh.note->setObjectName("muted");
    mesh.note->setWordWrap(true);
    mesh_layout->addWidget(mesh.note);
    mesh.group->setVisible(false);
    layout->addWidget(mesh.group);

    layout->addStretch();

    auto* empty = new QWidget;
    auto* empty_layout = new QVBoxLayout(empty);
    empty_layout->setContentsMargins(24, 30, 24, 30);
    empty_layout->addStretch(1);
    auto* empty_icon = new QLabel("â—‡", empty);
    empty_icon->setObjectName("emptyStateIcon");
    empty_icon->setAlignment(Qt::AlignCenter);
    empty_layout->addWidget(empty_icon);
    auto* empty_title = new QLabel("Nothing selected", empty);
    empty_title->setObjectName("emptyStateTitle");
    empty_title->setAlignment(Qt::AlignCenter);
    empty_layout->addWidget(empty_title);
    auto* empty_hint = new QLabel("Select an entity in the Hierarchy to inspect its properties.", empty);
    empty_hint->setObjectName("muted");
    empty_hint->setWordWrap(true);
    empty_hint->setAlignment(Qt::AlignCenter);
    empty_layout->addWidget(empty_hint);
    empty_add_entity = new QPushButton("Create Entityâ€¦", empty);
    empty_add_entity->setObjectName("emptyStateAction");
    empty_add_entity->setAccessibleName("Create an entity");
    empty_layout->addWidget(empty_add_entity, 0, Qt::AlignHCenter);
    empty_layout->addStretch(2);

    inspector_stack = new QStackedWidget;
    inspector_stack->setObjectName("inspectorPages");
    inspector_stack->addWidget(empty);
    inspector_stack->addWidget(body);
    inspector_stack->setCurrentWidget(empty);
    inspector_scroll = new QScrollArea;
    inspector_scroll->setObjectName("inspectorScrollArea");
    inspector_scroll->setFrameShape(QFrame::NoFrame);
    inspector_scroll->setWidgetResizable(true);
    inspector_scroll->setWidget(inspector_stack);
    return inspector_scroll;
}
} // namespace aether::editor::qt
