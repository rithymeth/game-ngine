#include "aether/gfx/rhi/device.h"
#include "aether/assets/gltf_loader.h"
#include "aether/assets/image.h"
#include "aether/audio/audio_system.h"
#include "aether/core/log.h"
#include "aether/ecs/archetype.h"
#include "aether/gameplay/attribute_system.h"
#include "aether/input/keys.h"
#include "aether/physics/character.h"
#include "aether/physics/components.h"
#include "aether/plugin/plugin.h"
#include "aether/project/project.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"
#include "aether/scene/script_component.h"
#include "aether/sequencer/player.h"
#include "aether/sequencer/sequence_system.h"
#include "core/scene_document.h"
#include "core/recent_projects.h"
#include "editor_play_runtime.h"
#include "sequencer/sequence_document.h"
#include "physics_component_panel.h"
#if defined(AETHER_QT_EDITOR_SCRIPTING)
#include "script_component_editor.h"
#endif

#include <QApplication>
#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDebug>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QPainter>
#include <QPainterPath>
#include <QFocusEvent>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QFont>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QKeyEvent>
#include <QLabel>
#include <QInputDialog>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QListView>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPaintEngine>
#include <QPushButton>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QSpinBox>
#include <QSizePolicy>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QStackedWidget>
#include <QToolBar>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWindow>
#include <QWidget>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace rhi = aether::gfx::rhi;

namespace {

aether::input::Key InputKeyFromQt(int key) {
    using aether::input::Key;
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return static_cast<Key>(static_cast<int>(Key::A) + key - Qt::Key_A);
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return static_cast<Key>(static_cast<int>(Key::Num0) + key - Qt::Key_0);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
        return static_cast<Key>(static_cast<int>(Key::F1) + key - Qt::Key_F1);
    switch (key) {
    case Qt::Key_Space: return Key::Space;
    case Qt::Key_Return:
    case Qt::Key_Enter: return Key::Enter;
    case Qt::Key_Escape: return Key::Escape;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return Key::Tab;
    case Qt::Key_Backspace: return Key::Backspace;
    case Qt::Key_Delete: return Key::Delete;
    case Qt::Key_Insert: return Key::Insert;
    case Qt::Key_Home: return Key::Home;
    case Qt::Key_End: return Key::End;
    case Qt::Key_PageUp: return Key::PageUp;
    case Qt::Key_PageDown: return Key::PageDown;
    case Qt::Key_Shift: return Key::LeftShift;
    case Qt::Key_Control: return Key::LeftCtrl;
    case Qt::Key_Alt: return Key::LeftAlt;
    case Qt::Key_Up: return Key::Up;
    case Qt::Key_Down: return Key::Down;
    case Qt::Key_Left: return Key::Left;
    case Qt::Key_Right: return Key::Right;
    case Qt::Key_Minus: return Key::Minus;
    case Qt::Key_Equal: return Key::Equals;
    case Qt::Key_BracketLeft: return Key::LeftBracket;
    case Qt::Key_BracketRight: return Key::RightBracket;
    case Qt::Key_Semicolon: return Key::Semicolon;
    case Qt::Key_Apostrophe: return Key::Apostrophe;
    case Qt::Key_Comma: return Key::Comma;
    case Qt::Key_Period: return Key::Period;
    case Qt::Key_Slash: return Key::Slash;
    case Qt::Key_Backslash: return Key::Backslash;
    case Qt::Key_QuoteLeft: return Key::Grave;
    default: return Key::None;
    }
}

aether::input::Key InputMouseKeyFromQt(Qt::MouseButton button) {
    using aether::input::Key;
    switch (button) {
    case Qt::LeftButton: return Key::MouseLeft;
    case Qt::RightButton: return Key::MouseRight;
    case Qt::MiddleButton: return Key::MouseMiddle;
    case Qt::BackButton: return Key::MouseButton4;
    case Qt::ForwardButton: return Key::MouseButton5;
    default: return Key::None;
    }
}

void RegisterEditorComponentSchemas() {
    aether::RegisterAudioComponents();
    aether::gas::RegisterGameplayComponents();
    aether::RegisterSequenceComponents();
    aether::RegisterPhysicsComponentSerializers();
    (void)aether::GetComponentId<aether::RigidBody>();
    (void)aether::GetComponentId<aether::BoxCollider>();
    (void)aether::GetComponentId<aether::SphereCollider>();
    (void)aether::GetComponentId<aether::CapsuleCollider>();
    (void)aether::GetComponentId<aether::ConvexCollider>();
    (void)aether::GetComponentId<aether::MeshCollider>();
    (void)aether::GetComponentId<aether::CharacterMovement>();
    (void)aether::GetComponentId<aether::Layer>();
    (void)aether::GetComponentId<aether::ScriptComponent>();
}

std::vector<aether::u32> RemapCollisionMatrix(const std::vector<std::string>& source_names,
                                              const std::vector<aether::u32>& source_masks,
                                              const std::vector<std::string>& target_names) {
    aether::ProjectSettings source;
    source.layers = source_names;
    source.collision_matrix = source_masks;
    const aether::CollisionMatrix old_matrix = aether::MakeCollisionMatrix(source);
    std::vector<aether::u32> result(target_names.size(), 0);
    for (std::size_t row = 0; row < target_names.size(); ++row) {
        for (std::size_t column = 0; column < target_names.size(); ++column) {
            const auto old_row = std::find(source_names.begin(), source_names.end(), target_names[row]);
            const auto old_column = std::find(source_names.begin(), source_names.end(), target_names[column]);
            const bool collide = old_row == source_names.end() || old_column == source_names.end()
                ? true : old_matrix.ShouldCollide(static_cast<aether::u8>(old_row - source_names.begin()),
                                                  static_cast<aether::u8>(old_column - source_names.begin()));
            if (collide) result[row] |= (1u << column);
        }
    }
    return result;
}

bool ShowCollisionMatrixDialog(QWidget* parent, const std::vector<std::string>& names,
                               const std::vector<std::string>& old_names,
                               const std::vector<aether::u32>& old_masks,
                               std::vector<aether::u32>& edited_masks) {
    if (names.empty() || names.size() > aether::kMaxLayers) return false;
    QDialog dialog(parent);
    dialog.setWindowTitle("Collision Matrix");
    dialog.resize(780, 580);
    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel("Checked pairs collide. The matrix stays symmetric.", &dialog);
    layout->addWidget(description);
    auto* table = new QTableWidget(static_cast<int>(names.size()), static_cast<int>(names.size()), &dialog);
    QStringList labels;
    for (const std::string& name : names) labels.push_back(QString::fromStdString(name));
    table->setHorizontalHeaderLabels(labels);
    table->setVerticalHeaderLabels(labels);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->setAlternatingRowColors(true);
    const std::vector<aether::u32> initial = RemapCollisionMatrix(old_names, old_masks, names);
    for (std::size_t row = 0; row < names.size(); ++row) {
        for (std::size_t column = 0; column < names.size(); ++column) {
            auto* cell = new QTableWidgetItem;
            Qt::ItemFlags flags = Qt::ItemIsEnabled;
            if (row <= column) flags |= Qt::ItemIsUserCheckable | Qt::ItemIsSelectable;
            cell->setFlags(flags);
            cell->setCheckState((initial[row] & (1u << column)) ? Qt::Checked : Qt::Unchecked);
            table->setItem(static_cast<int>(row), static_cast<int>(column), cell);
        }
    }
    QObject::connect(table, &QTableWidget::itemChanged, table, [table](QTableWidgetItem* item) {
        if (!item || item->row() >= item->column()) return;
        const QSignalBlocker blocker(table);
        QTableWidgetItem* mirror = table->item(item->column(), item->row());
        if (mirror) mirror->setCheckState(item->checkState());
    });
    layout->addWidget(table, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted) return false;
    edited_masks.assign(names.size(), 0);
    for (std::size_t row = 0; row < names.size(); ++row)
        for (std::size_t column = 0; column < names.size(); ++column)
            if (table->item(static_cast<int>(row), static_cast<int>(column))->checkState() == Qt::Checked)
                edited_masks[row] |= (1u << column);
    return true;
}

bool ValidatePluginModules(const aether::plugin::PluginManager& manager, std::string* error) {
    for (const std::string& module : manager.ModuleOrder(true, true)) {
        if (!aether::plugin::ModuleRegistry::Get().Has(module)) {
            if (error) *error = "The project requires plugin module '" + module +
                                "', but it isn't built into this editor.";
            return false;
        }
    }
    return true;
}

bool StartDefaultEnginePlugins(aether::plugin::PluginManager& manager, std::string* error) {
    const auto plugins_directory = aether::plugin::PluginManager::EnginePluginsDir();
    if (plugins_directory.empty()) return true;
    manager.AddSearchPath(plugins_directory, aether::plugin::PluginSource::Engine);
    (void)manager.Discover();
    if (!manager.Resolve({}, error)) return false;
    if (!ValidatePluginModules(manager, error)) return false;
    const auto warnings = manager.StartModules(true, true);
    if (!warnings.empty()) {
        manager.ShutdownModules();
        if (error) *error = warnings.front();
        return false;
    }
    return true;
}

constexpr auto kStyle = R"(
* { color: #dce4ec; font-family: "Segoe UI"; font-size: 9pt; }
QMainWindow, QWidget { background: #171c23; }
QMainWindow { border: 1px solid #29343e; }
QWidget#brandLockup { background: transparent; }
QMenuBar { background: #10151b; border-bottom: 1px solid #29343e; padding: 3px 6px; }
QMenuBar::item { padding: 6px 11px; border-radius: 4px; }
QMenuBar::item:selected, QMenu::item:selected { background: #263943; color: #f1f7fa; }
QMenu { background: #1b222a; border: 1px solid #34414d; padding: 5px; }
QMenu::item { padding: 7px 26px 7px 10px; border-radius: 3px; }
QMenu::separator { height: 1px; background: #303b46; margin: 5px 8px; }
QToolBar { background: #11171e; border: 0; border-bottom: 1px solid #2a333d; spacing: 6px; padding: 5px 10px; }
QToolBar#editorToolbar { background: #10151b; padding: 7px 12px; }
QToolBar#viewportTools { background: #171e25; padding: 4px 10px; }
QToolBar::separator { width: 1px; background: #303b46; margin: 4px 7px; }
QToolButton, QPushButton { background: #222c36; border: 1px solid #35424e; border-radius: 5px; padding: 6px 11px; }
QToolBar#editorToolbar QToolButton { background: transparent; border-color: transparent; padding: 7px 9px; }
QToolBar#editorToolbar QToolButton:hover { background: #202a33; border-color: #303d48; }
QToolBar#viewportTools QToolButton { background: #202a33; border-color: #303d48; padding: 6px 12px; }
QToolBar#viewportTools QToolButton:checked { background: #17443f; border-color: #36a98d; color: #a9f5dc; }
QToolBar#viewportTools QToolButton:hover:!checked { background: #293741; border-color: #465a68; }
QToolButton:hover, QPushButton:hover { background: #2b3b47; border-color: #486171; }
QToolButton:pressed, QPushButton:pressed { background: #20333c; }
QToolButton:focus, QPushButton:focus { border-color: #53c7a4; }
QToolButton:checked { background: #203a39; border-color: #3d9d88; color: #a2f2d7; }
QToolButton:disabled, QPushButton:disabled { color: #687581; background: #1a2027; border-color: #29323b; }
QPushButton#play { background: #35ba91; color: #0c211b; font-weight: 700; border: 0; padding: 8px 20px; }
QPushButton#play:hover { background: #4bd2a7; }
QPushButton#play:disabled { background: #235746; color: #91b9aa; }
QPushButton#emptyStateAction { background: #277d70; color: #f0fffb; border: 0; font-weight: 600; }
QPushButton#sequencePreviewButton { background: #277d70; color: #f0fffb; border-color: #318e7d; font-weight: 600; }
QPushButton#sequencePreviewButton:hover { background: #319580; }
QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; }
QDockWidget::title { background: #1a2129; border-bottom: 1px solid #303b46; padding: 9px 12px; font-weight: 700; color: #c2ced8; letter-spacing: 1px; }
QDockWidget::close-button, QDockWidget::float-button { width: 0; }
QTabBar::tab { background: #1a2027; border: 1px solid #2b3540; padding: 7px 12px; color: #a7b4bf; }
QTabBar::tab:selected { background: #222d36; color: #eef5fa; border-bottom-color: #54c9a7; }
QTabBar::tab:hover:!selected { background: #202a33; }
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QListWidget {
    background: #11171d; border: 1px solid #303b46; border-radius: 5px; padding: 5px 7px; selection-background-color: #286b62;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QPlainTextEdit:focus,
QListWidget:focus, QTreeWidget:focus { border-color: #53c7a4; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled { color: #71808c; background: #181e24; }
QComboBox QAbstractItemView { background: #1b222a; selection-background-color: #286b62; border: 1px solid #35424e; }
QTreeWidget, QTreeView, QListView { background: #151a21; border: 0; alternate-background-color: #19212a; outline: 0; }
QTreeWidget::item, QTreeView::item { padding: 5px 4px; border-radius: 3px; }
QTreeWidget::item:selected, QTreeView::item:selected, QListView::item:selected { background: #244653; color: #f2f8fb; }
QTreeWidget::item:hover:!selected, QTreeView::item:hover:!selected, QListView::item:hover:!selected { background: #202c35; }
QListView::item { padding: 8px; border: 1px solid transparent; border-radius: 6px; }
QListView::item:selected { border-color: #4b9c8a; }
QGroupBox { border: 1px solid #303b46; border-radius: 6px; margin-top: 10px; padding: 10px 8px 8px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #aebdca; }
QHeaderView::section { background: #1b222a; border: 0; border-bottom: 1px solid #303b46; padding: 6px; }
QScrollBar:vertical { width: 10px; background: #151a21; margin: 2px; }
QScrollBar::handle:vertical { background: #35424d; min-height: 28px; border-radius: 4px; }
QScrollBar::handle:vertical:hover { background: #4a5d6a; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical, QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; height: 0; }
QScrollBar:horizontal { height: 10px; background: #151a21; margin: 2px; }
QScrollBar::handle:horizontal { background: #35424d; min-width: 28px; border-radius: 4px; }
QStatusBar { background: #10151b; border-top: 1px solid #2a333d; color: #9eacb8; }
QStatusBar::item { border: 0; }
QLabel#brandGlyph { color: #10251f; background: #45c6a0; border-radius: 7px; font-size: 13pt; font-weight: 900; min-width: 28px; max-width: 28px; min-height: 28px; max-height: 28px; }
QLabel#brandName { color: #eff6fa; font-size: 10pt; font-weight: 800; letter-spacing: 1.2px; }
QLabel#brandSubline { color: #778894; font-size: 7pt; font-weight: 700; letter-spacing: 1px; }
QLabel#workspaceLabel { background: #172923; border: 1px solid #275446; border-radius: 5px; }
QLabel#viewportTitle { color: #e6eef3; font-size: 8pt; font-weight: 800; letter-spacing: 1px; padding: 0 4px; }
QLabel#viewportHint { color: #8d9ba7; font-size: 8pt; padding: 0 5px; }
QLabel#viewportPerspective { color: #a7bac6; background: #202a33; border: 1px solid #35434f; border-radius: 4px; font-size: 8pt; font-weight: 700; padding: 5px 8px; }
QLabel#sceneStateBadge, QLabel#rendererBadge { color: #99aab5; background: #1b242c; border: 1px solid #303d47; border-radius: 4px; font-size: 8pt; font-weight: 700; padding: 3px 7px; }
QLabel#sceneStateBadge[state="saved"] { color: #8ee3c6; background: #172923; border-color: #285848; }
QLabel#sceneStateBadge[state="dirty"] { color: #ffd28d; background: #30271a; border-color: #66502a; }
QLabel#sceneStateBadge[state="playing"] { color: #a8d2ff; background: #192738; border-color: #345577; }
QLabel#sceneStateBadge[state="paused"] { color: #f0c99d; background: #30251d; border-color: #654632; }
QLabel#axisX { color: #ff9e9b; background: #382322; border: 1px solid #68403d; }
QLabel#axisY { color: #a3e4ae; background: #203326; border: 1px solid #3c6344; }
QLabel#axisZ { color: #9ec7ff; background: #202c3c; border: 1px solid #3a5476; }
QLabel#axisX, QLabel#axisY, QLabel#axisZ { border-radius: 4px; min-width: 20px; max-width: 20px; min-height: 20px; max-height: 20px; font-size: 8pt; font-weight: 800; }
QLabel#axisCaption { color: #9eacb8; font-size: 8pt; }
QSlider::groove:horizontal { height: 4px; background: #303b46; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #4bc5a3; border-radius: 2px; }
QSlider::handle:horizontal { width: 13px; margin: -5px 0; border-radius: 7px; background: #dff8ef; border: 2px solid #36a98d; }
QCheckBox { spacing: 7px; }
QCheckBox::indicator { width: 15px; height: 15px; border: 1px solid #465562; border-radius: 4px; background: #11171d; }
QCheckBox::indicator:checked { background: #36a98d; border-color: #52d4b1; }
QCheckBox::indicator:disabled { background: #1a2027; border-color: #29323b; }
QListWidget::item { padding: 5px 7px; border-radius: 4px; }
QListWidget::item:selected { background: #244653; color: #f2f8fb; }
QLabel#muted { color: #93a1ad; }
QLabel#section { color: #67d4b5; font-size: 8pt; font-weight: 700; letter-spacing: 1px; }
QLabel#emptyStateTitle { color: #e8f0f5; font-size: 12pt; font-weight: 700; }
QLabel#emptyStateIcon { color: #5ac9a8; font-size: 26pt; font-weight: 600; }
QLabel#brand { color: #eef4f8; font-size: 9pt; font-weight: 700; letter-spacing: 1px; }
QLabel#workspaceLabel { color: #75dbbd; font-size: 8pt; font-weight: 700; letter-spacing: 1px; padding: 5px 8px; }
QWidget#contentWelcomePanel { background: #151a21; border: 1px dashed #3a4854; border-radius: 10px; }
QLabel#contentWelcomeTitle { color: #e8f0f5; font-size: 14pt; font-weight: 700; }
QLabel#contentWelcomeDescription { color: #93a1ad; }
QToolTip { background: #202a33; color: #edf4f8; border: 1px solid #42515e; padding: 5px 7px; }
)";

class RhiViewport final : public QWidget {
public:
    static constexpr float kStagedModelBelowWorldY = -10.0f;

    explicit RhiViewport(aether::editor::SceneDocument& document) : document_(document) {
        setAccessibleName("3D World View");
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_PaintOnScreen);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_OpaquePaintEvent);
        navigation_clock_.start();
        timer_.setInterval(16);
        QObject::connect(&timer_, &QTimer::timeout, [this] {
            UpdateCameraNavigation();
            Render();
        });
        timer_.start();
    }

    ~RhiViewport() override {
        timer_.stop();
        if (device_) device_->WaitForFence(fence_);
    }

    void SetSelectedGuid(const aether::EntityGuid& guid) {
        if (guid != selected_guid_) EndKeyboardObjectMove();
        selected_guid_ = guid;
    }
    void SetToolMode(const QString& mode) {
        EndKeyboardObjectMove();
        move_tool_ = mode == "Move";
        rotate_y_tool_ = mode == "Rotate Y";
        scale_tool_ = mode == "Scale";
        setCursor(move_tool_ || scale_tool_ ? Qt::SizeAllCursor
                  : rotate_y_tool_ ? Qt::CrossCursor : Qt::ArrowCursor);
        dragging_ = false;
        drag_move_axis_ = -1;
    }
    void SetGridSnap(bool enabled) { grid_snap_ = enabled; }
    void SetContentRoot(const std::filesystem::path& content_root) {
        if (content_root_ == content_root) return;
        content_root_ = content_root;
        models_.clear();
    }
    void SetEntitySelectedCallback(std::function<void(const aether::EntityGuid&)> callback) {
        entity_selected_ = std::move(callback);
    }
    void SetTransformChangedCallback(std::function<void(const aether::EntityGuid&)> callback) {
        transform_changed_ = std::move(callback);
    }
    void SetInputCallbacks(std::function<void(aether::input::Key, bool)> button,
                           std::function<void(float, float)> mouse_delta,
                           std::function<void(float)> mouse_wheel,
                           std::function<void()> clear) {
        input_button_ = std::move(button);
        input_mouse_delta_ = std::move(mouse_delta);
        input_mouse_wheel_ = std::move(mouse_wheel);
        input_clear_ = std::move(clear);
    }
    void SetPlayInputFocusChangedCallback(std::function<void(bool)> callback) {
        play_input_focus_changed_ = std::move(callback);
    }
    void SetPlayInputActive(bool active) {
        if (play_input_active_ == active) {
            if (active) setFocus(Qt::OtherFocusReason);
            NotifyPlayInputFocus();
            return;
        }
        play_input_active_ = active;
        navigation_keys_.clear();
        EndKeyboardObjectMove();
        has_play_mouse_position_ = false;
        if (!active && input_clear_) input_clear_();
        if (active) setFocus(Qt::OtherFocusReason);
        NotifyPlayInputFocus();
    }

protected:
    QPaintEngine* paintEngine() const override { return nullptr; }
    void showEvent(QShowEvent* event) override { QWidget::showEvent(event); EnsureSurface(); Render(); }
    void resizeEvent(QResizeEvent*) override { EnsureSurface(); }
    void keyPressEvent(QKeyEvent* event) override {
        if (play_input_active_ && hasFocus()) {
            const aether::input::Key key = InputKeyFromQt(event->key());
            if (key != aether::input::Key::None) {
                if (!event->isAutoRepeat() && input_button_) input_button_(key, true);
                event->accept();
                return;
            }
        }
        if (IsCameraNavigationKey(event->key())) {
            navigation_keys_.insert(event->key());
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }
    void keyReleaseEvent(QKeyEvent* event) override {
        if (play_input_active_ && hasFocus()) {
            const aether::input::Key key = InputKeyFromQt(event->key());
            if (key != aether::input::Key::None) {
                if (!event->isAutoRepeat() && input_button_) input_button_(key, false);
                event->accept();
                return;
            }
        }
        if (IsCameraNavigationKey(event->key())) {
            if (event->isAutoRepeat()) {
                event->accept();
                return;
            }
            navigation_keys_.remove(event->key());
            if (!HasTranslationKeyPressed()) EndKeyboardObjectMove();
            event->accept();
            return;
        }
        QWidget::keyReleaseEvent(event);
    }
    void focusOutEvent(QFocusEvent* event) override {
        EndKeyboardObjectMove();
        navigation_keys_.clear();
        has_play_mouse_position_ = false;
        if (play_input_active_ && input_clear_) input_clear_();
        QWidget::focusOutEvent(event);
        NotifyPlayInputFocus();
    }
    void focusInEvent(QFocusEvent* event) override {
        QWidget::focusInEvent(event);
        NotifyPlayInputFocus();
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (width() <= 0 || height() <= 0) return;
        setFocus(Qt::MouseFocusReason);
        if (play_input_active_) {
            has_play_mouse_position_ = true;
            last_play_mouse_position_ = event->position();
            const aether::input::Key key = InputMouseKeyFromQt(event->button());
            if (key != aether::input::Key::None && input_button_) input_button_(key, true);
            event->accept();
            return;
        }
        if (event->button() == Qt::RightButton) {
            orbiting_ = true;
            camera_drag_start_ = event->position();
            camera_yaw_start_ = camera_yaw_;
            camera_pitch_start_ = camera_pitch_;
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        if (event->button() == Qt::MiddleButton) {
            panning_ = true;
            camera_drag_start_ = event->position();
            pan_origin_ = camera_target_;
            event->accept();
            return;
        }
        if (event->button() != Qt::LeftButton) return;
        if (move_tool_) {
            int axis = -1;
            if (IsNearMoveGizmo(event->position(), axis)) {
                const aether::Entity entity = document_.Guids().Find(document_.GetWorld(), selected_guid_);
                if (const auto* transform = document_.GetWorld().GetComponent<aether::Transform>(entity)) {
                    dragging_ = true;
                    drag_guid_ = selected_guid_;
                    drag_origin_ = aether::WorldPosition(document_.GetWorld(), document_.Guids(), entity);
                    drag_start_ = event->position();
                    drag_move_axis_ = axis;
                    event->accept();
                    return;
                }
            }
        }
        if (scale_tool_ && IsNearScaleHandle(event->position())) {
            const aether::Entity entity = document_.Guids().Find(document_.GetWorld(), selected_guid_);
            if (const auto* transform = document_.GetWorld().GetComponent<aether::Transform>(entity)) {
                dragging_ = true;
                drag_guid_ = selected_guid_;
                drag_scale_origin_ = transform->scale;
                drag_start_ = event->position();
                event->accept();
                return;
            }
        }
        if (rotate_y_tool_ && IsNearRotationRing(event->position())) {
            const aether::Entity entity = document_.Guids().Find(document_.GetWorld(), selected_guid_);
            if (const auto* transform = document_.GetWorld().GetComponent<aether::Transform>(entity)) {
                dragging_ = true;
                drag_guid_ = selected_guid_;
                drag_rotation_origin_ = transform->rotation;
                drag_start_ = event->position();
                event->accept();
                return;
            }
        }
        const aether::EntityGuid hit = PickEntity(event->position());
        if (hit.IsNull()) return;
        if (entity_selected_) entity_selected_(hit);
        if ((move_tool_ || scale_tool_) && hit == selected_guid_) {
            const aether::Entity entity = document_.Guids().Find(document_.GetWorld(), hit);
            if (const auto* transform = document_.GetWorld().GetComponent<aether::Transform>(entity)) {
                dragging_ = true;
                drag_guid_ = hit;
                drag_origin_ = aether::WorldPosition(document_.GetWorld(), document_.Guids(), entity);
                drag_scale_origin_ = transform->scale;
                drag_start_ = event->position();
                drag_move_axis_ = -1;
                event->accept();
            }
        }
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (play_input_active_ && hasFocus()) {
            if (has_play_mouse_position_ && input_mouse_delta_) {
                const QPointF delta = event->position() - last_play_mouse_position_;
                input_mouse_delta_(static_cast<float>(delta.x()), static_cast<float>(delta.y()));
            }
            last_play_mouse_position_ = event->position();
            has_play_mouse_position_ = true;
            event->accept();
            return;
        }
        if (orbiting_ && (event->buttons() & Qt::RightButton)) {
            const QPointF delta = event->position() - camera_drag_start_;
            camera_yaw_ = camera_yaw_start_ - static_cast<float>(delta.x()) * 0.008f;
            camera_pitch_ = std::clamp(camera_pitch_start_ + static_cast<float>(delta.y()) * 0.006f,
                                       0.08f, 1.48f);
            event->accept();
            return;
        }
        if (panning_ && (event->buttons() & Qt::MiddleButton)) {
            const QPointF delta = event->position() - camera_drag_start_;
            const float units_per_pixel = camera_distance_ * 0.0017f;
            const aether::Vec3 right = CameraRight();
            const aether::Vec3 up = right.Cross(CameraForward()).Normalized();
            camera_target_ = pan_origin_ - right * (static_cast<float>(delta.x()) * units_per_pixel)
                + up * (static_cast<float>(delta.y()) * units_per_pixel);
            event->accept();
            return;
        }
        if (dragging_ && (event->buttons() & Qt::LeftButton)) ApplyActiveDrag(event->position(), false);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (play_input_active_) {
            const aether::input::Key key = InputMouseKeyFromQt(event->button());
            if (key != aether::input::Key::None && input_button_) input_button_(key, false);
            event->accept();
            return;
        }
        if (event->button() == Qt::RightButton && orbiting_) {
            orbiting_ = false;
            setCursor(move_tool_ || scale_tool_ ? Qt::SizeAllCursor
                      : rotate_y_tool_ ? Qt::CrossCursor : Qt::ArrowCursor);
            event->accept();
            return;
        }
        if (event->button() == Qt::MiddleButton && panning_) {
            panning_ = false;
            event->accept();
            return;
        }
        if (event->button() != Qt::LeftButton || !dragging_) return;
        ApplyActiveDrag(event->position(), true);
        dragging_ = false;
        drag_move_axis_ = -1;
        event->accept();
    }
    void wheelEvent(QWheelEvent* event) override {
        if (play_input_active_ && hasFocus()) {
            if (input_mouse_wheel_)
                input_mouse_wheel_(static_cast<float>(event->angleDelta().y()) / 120.0f);
            event->accept();
            return;
        }
        if (width() <= 0 || height() <= 0 || event->angleDelta().y() == 0) return;
        camera_distance_ = std::clamp(camera_distance_ *
            std::pow(0.84f, static_cast<float>(event->angleDelta().y()) / 120.0f), 1.5f, 250.0f);
        event->accept();
    }

private:
    void NotifyPlayInputFocus() {
        if (play_input_focus_changed_) play_input_focus_changed_(play_input_active_ && hasFocus());
    }

    static bool IsCameraNavigationKey(int key) {
        return key == Qt::Key_W || key == Qt::Key_A || key == Qt::Key_S || key == Qt::Key_D ||
            key == Qt::Key_Q || key == Qt::Key_E || key == Qt::Key_Shift;
    }

    bool HasTranslationKeyPressed() const {
        return navigation_keys_.contains(Qt::Key_W) || navigation_keys_.contains(Qt::Key_A) ||
            navigation_keys_.contains(Qt::Key_S) || navigation_keys_.contains(Qt::Key_D) ||
            navigation_keys_.contains(Qt::Key_Q) || navigation_keys_.contains(Qt::Key_E);
    }

    void EndKeyboardObjectMove() {
        if (keyboard_move_guid_.IsNull()) return;
        const aether::Entity entity = document_.Guids().Find(document_.GetWorld(), keyboard_move_guid_);
        if (!entity.IsNull()) {
            const aether::Vec3 position = aether::WorldPosition(document_.GetWorld(), document_.Guids(), entity);
            if (SetWorldPosition(entity, position, true) && transform_changed_)
                transform_changed_(keyboard_move_guid_);
        }
        keyboard_move_guid_ = {};
    }

    void UpdateCameraNavigation() {
        const qint64 elapsed_ms = navigation_clock_.restart();
        if (navigation_keys_.isEmpty() || elapsed_ms <= 0) return;
        const float delta_time = std::clamp(static_cast<float>(elapsed_ms) / 1000.0f, 0.0f, 0.05f);
        aether::Vec3 forward = CameraForward();
        forward.y = 0.0f;
        if (forward.LengthSq() > 0.0001f) forward = forward.Normalized();
        aether::Vec3 right = CameraRight();
        right.y = 0.0f;
        if (right.LengthSq() > 0.0001f) right = right.Normalized();
        aether::Vec3 movement{};
        if (navigation_keys_.contains(Qt::Key_W)) movement = movement + forward;
        if (navigation_keys_.contains(Qt::Key_S)) movement = movement - forward;
        if (navigation_keys_.contains(Qt::Key_D)) movement = movement + right;
        if (navigation_keys_.contains(Qt::Key_A)) movement = movement - right;
        if (navigation_keys_.contains(Qt::Key_E)) movement.y += 1.0f;
        if (navigation_keys_.contains(Qt::Key_Q)) movement.y -= 1.0f;
        if (movement.LengthSq() < 0.0001f) return;
        movement = movement.Normalized();
        const float speed = std::max(1.0f, camera_distance_ * 0.12f) *
            (navigation_keys_.contains(Qt::Key_Shift) ? 3.0f : 1.0f);
        const aether::Entity selected = document_.Guids().Find(document_.GetWorld(), selected_guid_);
        if (move_tool_ && !selected.IsNull() &&
            document_.GetWorld().HasComponent<aether::Transform>(selected)) {
            const aether::Vec3 position = aether::WorldPosition(document_.GetWorld(), document_.Guids(), selected);
            if (SetWorldPosition(selected, position + movement * (speed * delta_time), false)) {
                keyboard_move_guid_ = selected_guid_;
                if (transform_changed_) transform_changed_(keyboard_move_guid_);
            }
            return;
        }
        camera_target_ = camera_target_ + movement * (speed * delta_time);
    }

    aether::Vec3 CameraPosition() const {
        const float horizontal = std::cos(camera_pitch_) * camera_distance_;
        return camera_target_ + aether::Vec3{std::sin(camera_yaw_) * horizontal,
            std::sin(camera_pitch_) * camera_distance_, -std::cos(camera_yaw_) * horizontal};
    }
    aether::Vec3 CameraForward() const { return (camera_target_ - CameraPosition()).Normalized(); }
    aether::Vec3 CameraRight() const { return CameraForward().Cross({0.0f, 1.0f, 0.0f}).Normalized(); }
    aether::Mat4 ViewProjection() const {
        const float aspect = static_cast<float>(std::max(width(), 1)) / static_cast<float>(std::max(height(), 1));
        return aether::Mat4::PerspectiveRH(aether::Radians(55.0f), aspect, 0.05f, 1000.0f) *
            aether::Mat4::LookAtRH(CameraPosition(), camera_target_, {0.0f, 1.0f, 0.0f});
    }
    bool ProjectToScreen(const aether::Vec3& world, QPointF& screen, float* depth = nullptr) const {
        const aether::Vec4 clip = ViewProjection() * aether::Vec4(world.x, world.y, world.z, 1.0f);
        if (clip.w <= 0.001f) return false;
        const float x = clip.x / clip.w;
        const float y = clip.y / clip.w;
        if (depth) *depth = clip.z / clip.w;
        screen = QPointF((x * 0.5f + 0.5f) * width(), (1.0f - (y * 0.5f + 0.5f)) * height());
        return true;
    }

    float MoveGizmoLength() const { return std::clamp(camera_distance_ * 0.075f, 0.9f, 2.2f); }

    bool IsNearMoveGizmo(const QPointF& point, int& axis_hit) const {
        axis_hit = -1;
        const aether::Entity entity = document_.Guids().Find(document_.GetWorld(), selected_guid_);
        if (entity.IsNull()) return false;
        const aether::Vec3 origin = aether::WorldPosition(document_.GetWorld(), document_.Guids(), entity);
        QPointF screen_origin;
        if (!ProjectToScreen(origin, screen_origin)) return false;
        const float length = MoveGizmoLength();
        const aether::Vec3 axes[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        float closest_distance_sq = 16.0f * 16.0f;
        for (int axis = 0; axis < 3; ++axis) {
            const aether::Vec3 endpoint = origin + axes[axis] * length;
            QPointF screen_endpoint;
            if (!ProjectToScreen(endpoint, screen_endpoint)) continue;
            const QPointF segment = screen_endpoint - screen_origin;
            const QPointF offset = point - screen_origin;
            const float segment_length_sq = static_cast<float>(segment.x() * segment.x() + segment.y() * segment.y());
            if (segment_length_sq < 9.0f) continue;
            const float t = std::clamp(static_cast<float>(
                (offset.x() * segment.x() + offset.y() * segment.y()) / segment_length_sq), 0.0f, 1.0f);
            if (t < 0.18f) continue;
            const QPointF closest = screen_origin + segment * t;
            const QPointF delta = point - closest;
            const float distance_sq = static_cast<float>(delta.x() * delta.x() + delta.y() * delta.y());
            if (distance_sq <= closest_distance_sq) {
                closest_distance_sq = distance_sq;
                axis_hit = axis;
            }
        }
        return axis_hit >= 0;
    }

    bool IsNearScaleHandle(const QPointF& point) const {
        const aether::Entity entity = document_.Guids().Find(document_.GetWorld(), selected_guid_);
        if (entity.IsNull()) return false;
        const aether::Vec3 origin = aether::WorldPosition(document_.GetWorld(), document_.Guids(), entity);
        QPointF handle;
        if (!ProjectToScreen(origin + aether::Vec3{0.0f, MoveGizmoLength() * 0.85f, 0.0f}, handle)) return false;
        const QPointF delta = point - handle;
        return delta.x() * delta.x() + delta.y() * delta.y() <= 18.0 * 18.0;
    }

public:
    void FocusSelection() {
        const aether::Entity entity = document_.Guids().Find(document_.GetWorld(), selected_guid_);
        if (!entity.IsNull()) {
            Bounds bounds;
            if (EntityBounds(entity, bounds)) {
                camera_target_ = (bounds.minimum + bounds.maximum) * 0.5f;
                camera_distance_ = DistanceForExtent(bounds.maximum - bounds.minimum);
            } else {
                camera_target_ = aether::WorldPosition(document_.GetWorld(), document_.Guids(), entity);
                camera_distance_ = 7.0f;
            }
        }
    }

    void FitAll() {
        Bounds bounds;
        bool waiting_for_models = false;
        for (aether::Entity entity : document_.Entities()) {
            const auto* model = document_.GetWorld().GetComponent<aether::ModelRenderer>(entity);
            const aether::Vec3 position = aether::WorldPosition(document_.GetWorld(), document_.Guids(), entity);
            // Negative-height model instances are used by gameplay as hidden
            // staging points (for example, dormant enemy tells). They remain
            // selectable and focusable, but shouldn't pull the map framing down.
            if (model && position.y < kStagedModelBelowWorldY) continue;
            if (model && !device_) waiting_for_models = true;
            Bounds entity_bounds;
            if (EntityBounds(entity, entity_bounds)) Include(bounds, entity_bounds);
        }
        fit_pending_ = waiting_for_models;
        if (!bounds.valid) { camera_target_ = {}; camera_distance_ = 18.0f; return; }
        camera_target_ = (bounds.minimum + bounds.maximum) * 0.5f;
        camera_distance_ = DistanceForExtent(bounds.maximum - bounds.minimum);
    }

    static float DistanceForExtent(const aether::Vec3& extent, float vertical_fov = 55.0f,
                                   float aspect = 1.5f) {
        const float horizontal_fov = 2.0f * std::atan(std::tan(aether::Radians(vertical_fov) * 0.5f) *
                                                       std::max(aspect, 0.1f));
        const float half_width = std::max(extent.x, extent.z) * 0.5f;
        const float half_height = extent.y * 0.5f;
        const float horizontal_distance = half_width / std::max(std::tan(horizontal_fov * 0.5f), 0.01f);
        const float vertical_distance = half_height / std::max(std::tan(aether::Radians(vertical_fov) * 0.5f), 0.01f);
        return std::clamp(std::max({horizontal_distance, vertical_distance, 1.5f}) * 1.25f, 1.5f, 250.0f);
    }

private:

    struct Bounds {
        aether::Vec3 minimum{};
        aether::Vec3 maximum{};
        bool valid = false;
    };

    static void Include(Bounds& target, const aether::Vec3& point) {
        if (!target.valid) {
            target.minimum = target.maximum = point;
            target.valid = true;
            return;
        }
        target.minimum.x = std::min(target.minimum.x, point.x);
        target.minimum.y = std::min(target.minimum.y, point.y);
        target.minimum.z = std::min(target.minimum.z, point.z);
        target.maximum.x = std::max(target.maximum.x, point.x);
        target.maximum.y = std::max(target.maximum.y, point.y);
        target.maximum.z = std::max(target.maximum.z, point.z);
    }

    static void Include(Bounds& target, const Bounds& source) {
        if (!source.valid) return;
        Include(target, source.minimum);
        Include(target, source.maximum);
    }

    static void IncludeTransformed(Bounds& bounds, const Bounds& source, const aether::Mat4& transform) {
        if (!source.valid) return;
        for (int corner = 0; corner < 8; ++corner) {
            const aether::Vec4 point(
                (corner & 1) ? source.maximum.x : source.minimum.x,
                (corner & 2) ? source.maximum.y : source.minimum.y,
                (corner & 4) ? source.maximum.z : source.minimum.z, 1.0f);
            const aether::Vec4 world = transform * point;
            Include(bounds, {world.x, world.y, world.z});
        }
    }

    bool EntityBounds(aether::Entity entity, Bounds& bounds) {
        const auto* model_renderer = document_.GetWorld().GetComponent<aether::ModelRenderer>(entity);
        if (model_renderer) {
            if (ModelAsset* model = FindModel(*model_renderer)) {
                const aether::Mat4 entity_transform =
                    aether::ComputeWorldTransform(document_.GetWorld(), document_.Guids(), entity);
                Bounds model_bounds;
                for (const auto& instance : model->scene.node_instances) {
                    if (instance.mesh_index >= model->mesh_bounds.size()) continue;
                    IncludeTransformed(model_bounds, model->mesh_bounds[instance.mesh_index],
                                       entity_transform * instance.world_transform);
                }
                if (model_bounds.valid) {
                    bounds = model_bounds;
                    return true;
                }
            }
        }

        if (!document_.GetWorld().GetComponent<aether::Transform>(entity)) return false;
        aether::Vec3 marker_scale{0.42f, 0.42f, 0.42f};
        if (document_.GetWorld().HasComponent<aether::Camera>(entity)) marker_scale = {0.7f, 0.7f, 0.7f};
        if (document_.Name(entity) == "Ground") marker_scale = {3.5f, 0.12f, 3.5f};
        const Bounds marker{{-marker_scale.x, -marker_scale.y, -marker_scale.z},
                            {marker_scale.x, marker_scale.y, marker_scale.z}, true};
        IncludeTransformed(bounds, marker,
            aether::ComputeWorldTransform(document_.GetWorld(), document_.Guids(), entity));
        return bounds.valid;
    }

    struct ScenePushConstants {
        aether::Mat4 mvp;
        float color[4];
    };

    struct ModelVertex {
        float position[3];
        float uv[2];
    };
    static_assert(sizeof(ModelVertex) == sizeof(float) * 5);

    struct ModelPushConstants {
        aether::Mat4 mvp;
        float color[4];
        aether::u32 texture_index = 0;
        aether::u32 use_texture = 0;
        aether::u32 padding[2]{};
    };
    static_assert(sizeof(ModelPushConstants) == 96);

    struct ModelPrimitive {
        rhi::BufferHandle vertices;
        rhi::BufferHandle indices;
        aether::u32 index_count = 0;
        float color[4]{1.0f, 1.0f, 1.0f, 1.0f};
        rhi::SampledTextureHandle texture;
        bool has_texture = false;
    };

    struct ModelAsset {
        aether::assets::GltfScene scene;
        std::vector<std::vector<ModelPrimitive>> meshes;
        std::vector<Bounds> mesh_bounds;
        bool valid = false;
    };

    std::filesystem::path ResolveModelPath(const aether::ModelRenderer& renderer) const {
        std::filesystem::path path(renderer.asset_path);
        if (path.is_relative()) path = content_root_ / path;
        return path.lexically_normal();
    }

    bool LoadTexture(const std::string& path, rhi::SampledTextureHandle& out) {
        if (path.empty()) { out = white_texture_; return false; }
        const std::string key = std::filesystem::path(path).lexically_normal().generic_string();
        if (const auto found = textures_.find(key); found != textures_.end()) {
            out = found->second;
            return out.IsValid() && out.index != white_texture_.index;
        }
        if (textures_.size() >= rhi::kMaxUserBindlessTextures) {
            AETHER_LOG_WARN("QtViewport", "Texture limit reached; using white for '%s'", key.c_str());
            out = white_texture_;
            return false;
        }
        aether::assets::ImageData image;
        if (!aether::assets::DecodeImageFile(key, image)) {
            AETHER_LOG_WARN("QtViewport", "Couldn't decode model texture '%s'", key.c_str());
            out = white_texture_;
            return false;
        }
        out = device_->CreateTexture(image.width, image.height, image.pixels.data());
        if (out.IsValid()) textures_.emplace(key, out);
        return out.IsValid() && out.index != white_texture_.index;
    }

    bool LoadModel(const std::filesystem::path& path, ModelAsset& out) {
        if (path.empty() || !std::filesystem::exists(path) ||
            !aether::assets::LoadGltf(path.string(), out.scene)) {
            AETHER_LOG_WARN("QtViewport", "Couldn't load model '%s'", path.string().c_str());
            return false;
        }
        std::vector<rhi::SampledTextureHandle> material_textures(out.scene.materials.size(), white_texture_);
        std::vector<bool> material_has_textures(out.scene.materials.size(), false);
        for (std::size_t index = 0; index < out.scene.materials.size(); ++index)
            material_has_textures[index] = LoadTexture(out.scene.materials[index].base_color_texture,
                                                       material_textures[index]);

        out.meshes.resize(out.scene.meshes.size());
        out.mesh_bounds.resize(out.scene.meshes.size());
        for (std::size_t mesh_index = 0; mesh_index < out.scene.meshes.size(); ++mesh_index) {
            for (const auto& source : out.scene.meshes[mesh_index].primitives) {
                if (source.vertices.empty() || source.indices.empty()) continue;
                for (const auto& vertex : source.vertices)
                    Include(out.mesh_bounds[mesh_index],
                            {vertex.position[0], vertex.position[1], vertex.position[2]});
                std::vector<ModelVertex> vertices;
                vertices.reserve(source.vertices.size());
                for (const auto& vertex : source.vertices)
                    vertices.push_back({{vertex.position[0], vertex.position[1], vertex.position[2]},
                                        {vertex.uv[0], vertex.uv[1]}});
                ModelPrimitive primitive;
                primitive.vertices = device_->CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(ModelVertex));
                primitive.indices = device_->CreateIndexBuffer(source.indices.data(),
                    source.indices.size() * sizeof(aether::u32), rhi::IndexFormat::UInt32);
                primitive.index_count = static_cast<aether::u32>(source.indices.size());
                if (source.material_index >= 0 &&
                    static_cast<std::size_t>(source.material_index) < out.scene.materials.size()) {
                    const std::size_t material = static_cast<std::size_t>(source.material_index);
                    std::copy(std::begin(out.scene.materials[material].base_color),
                              std::end(out.scene.materials[material].base_color), primitive.color);
                    primitive.texture = material_textures[material];
                    primitive.has_texture = material_has_textures[material];
                } else {
                    primitive.texture = white_texture_;
                }
                out.meshes[mesh_index].push_back(primitive);
            }
        }
        if (out.scene.node_instances.empty()) {
            for (std::size_t mesh_index = 0; mesh_index < out.scene.meshes.size(); ++mesh_index)
                out.scene.node_instances.push_back({mesh_index, aether::Mat4::Identity()});
        }
        out.valid = true;
        AETHER_LOG_INFO("QtViewport", "Loaded '%s' (%zu meshes, %zu instances)", path.string().c_str(),
                        out.scene.meshes.size(), out.scene.node_instances.size());
        return true;
    }

    ModelAsset* FindModel(const aether::ModelRenderer& renderer) {
        if (!device_ || content_root_.empty() || renderer.asset_path[0] == '\0') return nullptr;
        const std::string key = ResolveModelPath(renderer).generic_string();
        auto [item, inserted] = models_.try_emplace(key);
        if (inserted) LoadModel(std::filesystem::path(key), item->second);
        return item->second.valid ? &item->second : nullptr;
    }

    void PrepareModels() {
        for (aether::Entity entity : document_.Entities())
            if (const auto* renderer = document_.GetWorld().GetComponent<aether::ModelRenderer>(entity))
                FindModel(*renderer);
        if (fit_pending_) {
            fit_pending_ = false;
            FitAll();
        }
    }

    aether::EntityGuid PickEntity(const QPointF& point) {
        float closest_center_distance_sq = std::numeric_limits<float>::max();
        float closest_depth = std::numeric_limits<float>::max();
        aether::EntityGuid hit{};
        for (aether::Entity entity : document_.Entities()) {
            const auto* id = document_.GetWorld().GetComponent<aether::IdComponent>(entity);
            if (!id) continue;
            Bounds bounds;
            if (!EntityBounds(entity, bounds)) continue;
            float left = std::numeric_limits<float>::max();
            float top = std::numeric_limits<float>::max();
            float right = std::numeric_limits<float>::lowest();
            float bottom = std::numeric_limits<float>::lowest();
            float nearest_depth = std::numeric_limits<float>::max();
            for (int corner = 0; corner < 8; ++corner) {
                const aether::Vec3 world_point{
                    (corner & 1) ? bounds.maximum.x : bounds.minimum.x,
                    (corner & 2) ? bounds.maximum.y : bounds.minimum.y,
                    (corner & 4) ? bounds.maximum.z : bounds.minimum.z};
                QPointF screen;
                float depth = 0.0f;
                if (!ProjectToScreen(world_point, screen, &depth) || depth < 0.0f || depth > 1.0f) continue;
                left = std::min(left, static_cast<float>(screen.x()));
                top = std::min(top, static_cast<float>(screen.y()));
                right = std::max(right, static_cast<float>(screen.x()));
                bottom = std::max(bottom, static_cast<float>(screen.y()));
                nearest_depth = std::min(nearest_depth, depth);
            }
            if (nearest_depth == std::numeric_limits<float>::max()) continue;
            const aether::Vec3 bounds_center = (bounds.minimum + bounds.maximum) * 0.5f;
            QPointF projected_center;
            float center_depth = nearest_depth;
            if (ProjectToScreen(bounds_center, projected_center, &center_depth) &&
                (center_depth < 0.0f || center_depth > 1.0f)) center_depth = nearest_depth;
            constexpr float padding = 8.0f;
            if (point.x() < left - padding || point.x() > right + padding ||
                point.y() < top - padding || point.y() > bottom + padding) continue;
            const float center_x = (left + right) * 0.5f;
            const float center_y = (top + bottom) * 0.5f;
            const float dx = static_cast<float>(point.x()) - center_x;
            const float dy = static_cast<float>(point.y()) - center_y;
            const float center_distance_sq = dx * dx + dy * dy;
            if (center_depth < closest_depth - 0.002f ||
                (std::abs(center_depth - closest_depth) <= 0.002f &&
                 center_distance_sq < closest_center_distance_sq)) {
                closest_center_distance_sq = center_distance_sq;
                closest_depth = center_depth;
                hit = id->guid;
            }
        }
        return hit;
    }

    bool SetWorldPosition(aether::Entity entity, const aether::Vec3& world_position, bool committed) {
        aether::Vec3 local_position = world_position;
        const aether::Entity parent = document_.ParentOf(entity);
        if (!parent.IsNull()) {
            const aether::Mat4 parent_world =
                aether::ComputeWorldTransform(document_.GetWorld(), document_.Guids(), parent);
            aether::Mat4 inverse;
            if (!parent_world.TryInverse(inverse)) return false;
            const aether::Vec4 local = inverse * aether::Vec4(world_position.x, world_position.y, world_position.z, 1.0f);
            if (std::abs(local.w) < 0.0001f) return false;
            local_position = {local.x / local.w, local.y / local.w, local.z / local.w};
        }
        return document_.SetPosition(entity, local_position, committed);
    }

    void ApplyDrag(const QPointF& point, bool committed) {
        if (width() <= 0 || height() <= 0) return;
        const float units_per_pixel = camera_distance_ * 0.0017f;
        const float dx = static_cast<float>(point.x() - drag_start_.x()) * units_per_pixel;
        const float dy = static_cast<float>(point.y() - drag_start_.y()) * units_per_pixel;
        const aether::Vec3 right = CameraRight();
        aether::Vec3 ground_forward = CameraForward();
        ground_forward.y = 0.0f;
        if (ground_forward.LengthSq() > 0.0001f) ground_forward = ground_forward.Normalized();
        aether::Vec3 position = drag_origin_ + right * dx - ground_forward * dy;
        position.y = drag_origin_.y;
        if (grid_snap_) {
            position.x = std::round(position.x);
            position.z = std::round(position.z);
        }
        const aether::Entity entity = document_.Guids().Find(document_.GetWorld(), drag_guid_);
        if (entity.IsNull()) return;
        if (SetWorldPosition(entity, position, committed) && transform_changed_)
            transform_changed_(drag_guid_);
    }

    void ApplyAxisDrag(const QPointF& point, bool committed) {
        const aether::Entity entity = document_.Guids().Find(document_.GetWorld(), drag_guid_);
        if (entity.IsNull() || drag_move_axis_ < 0 || drag_move_axis_ > 2) return;
        const aether::Vec3 axes[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        const float length = MoveGizmoLength();
        QPointF origin_screen, endpoint_screen;
        if (!ProjectToScreen(drag_origin_, origin_screen) ||
            !ProjectToScreen(drag_origin_ + axes[drag_move_axis_] * length, endpoint_screen)) return;
        const QPointF projected_axis = endpoint_screen - origin_screen;
        const float projected_length_sq = static_cast<float>(
            projected_axis.x() * projected_axis.x() + projected_axis.y() * projected_axis.y());
        if (projected_length_sq < 9.0f) return;
        const QPointF delta = point - drag_start_;
        const float screen_fraction = static_cast<float>(
            (delta.x() * projected_axis.x() + delta.y() * projected_axis.y()) / projected_length_sq);
        float world_distance = std::clamp(screen_fraction, -10000.0f, 10000.0f) * length;
        aether::Vec3 world_position = drag_origin_ + axes[drag_move_axis_] * world_distance;
        if (grid_snap_) {
            world_position.x = std::round(world_position.x);
            world_position.y = std::round(world_position.y);
            world_position.z = std::round(world_position.z);
        }
        if (SetWorldPosition(entity, world_position, committed) && transform_changed_)
            transform_changed_(drag_guid_);
    }

    void ApplyUniformScaleDrag(const QPointF& point, bool committed) {
        const aether::Entity entity = document_.Guids().Find(document_.GetWorld(), drag_guid_);
        if (entity.IsNull()) return;
        const float exponent = std::clamp(
            static_cast<float>(drag_start_.y() - point.y()) * 0.01f, -8.0f, 8.0f);
        const float multiplier = std::exp(exponent);
        aether::Vec3 scale = drag_scale_origin_ * multiplier;
        if (grid_snap_) {
            scale.x = std::round(scale.x * 10.0f) / 10.0f;
            scale.y = std::round(scale.y * 10.0f) / 10.0f;
            scale.z = std::round(scale.z * 10.0f) / 10.0f;
        }
        if (document_.SetScale(entity, scale, committed) && transform_changed_)
            transform_changed_(drag_guid_);
    }

    bool IsNearRotationRing(const QPointF& point) const {
        const aether::Entity entity = document_.Guids().Find(document_.GetWorld(), selected_guid_);
        if (entity.IsNull() || !document_.GetWorld().HasComponent<aether::Transform>(entity)) return false;
        const aether::Mat4 entity_world = aether::ComputeWorldTransform(document_.GetWorld(), document_.Guids(), entity);
        const float radius = std::clamp(camera_distance_ * 0.07f, 0.8f, 2.0f);
        constexpr int segments = 48;
        float closest_sq = 12.0f * 12.0f;
        QPointF previous;
        bool have_previous = false;
        for (int i = 0; i <= segments; ++i) {
            const float angle = static_cast<float>(i) * 2.0f * aether::kPi / static_cast<float>(segments);
            const aether::Vec3 local{radius * std::cos(angle), 0.0f, radius * std::sin(angle)};
            const aether::Vec4 world = entity_world * aether::Vec4(local.x, local.y, local.z, 1.0f);
            QPointF projected;
            if (!ProjectToScreen({world.x, world.y, world.z}, projected)) {
                have_previous = false;
                continue;
            }
            if (have_previous) {
                const QPointF edge = projected - previous;
                const QPointF to_point = point - previous;
                const float length_sq = static_cast<float>(edge.x() * edge.x() + edge.y() * edge.y());
                const float t = length_sq > 0.0001f
                    ? std::clamp(static_cast<float>((to_point.x() * edge.x() + to_point.y() * edge.y()) / length_sq),
                                 0.0f, 1.0f)
                    : 0.0f;
                const QPointF closest = previous + edge * t;
                const QPointF delta = point - closest;
                closest_sq = std::min(closest_sq,
                    static_cast<float>(delta.x() * delta.x() + delta.y() * delta.y()));
            }
            previous = projected;
            have_previous = true;
        }
        return closest_sq < 12.0f * 12.0f;
    }

    void ApplyActiveDrag(const QPointF& point, bool committed) {
        if (move_tool_) {
            if (drag_move_axis_ >= 0) ApplyAxisDrag(point, committed);
            else ApplyDrag(point, committed);
        } else if (scale_tool_) {
            ApplyUniformScaleDrag(point, committed);
        } else if (rotate_y_tool_) {
            const float dx = static_cast<float>(point.x() - drag_start_.x());
            float angle = dx * 0.01f;
            if (grid_snap_) {
                constexpr float step = aether::kPi / 12.0f;
                angle = std::round(angle / step) * step;
            }
            const aether::Quaternion delta = aether::Quaternion::FromAxisAngle({0.0f, 1.0f, 0.0f}, angle);
            const aether::Quaternion rotation = (drag_rotation_origin_ * delta).Normalized();
            const aether::Entity entity = document_.Guids().Find(document_.GetWorld(), drag_guid_);
            if (document_.SetRotation(entity, rotation, committed) && transform_changed_)
                transform_changed_(drag_guid_);
        }
    }

    void DrawCube(const aether::Mat4& model, float r, float g, float b, float a = 1.0f) {
        const ScenePushConstants constants{ViewProjection() * model, {r, g, b, a}};
        command_list_->SetPushConstants(&constants, sizeof(constants));
        command_list_->Draw(36);
    }

    void DrawRotationRing(const aether::Entity& entity) {
        const aether::Mat4 entity_world = aether::ComputeWorldTransform(document_.GetWorld(), document_.Guids(), entity);
        const float radius = std::clamp(camera_distance_ * 0.07f, 0.8f, 2.0f);
        constexpr int segments = 32;
        const float half_length = radius * std::sin(aether::kPi / static_cast<float>(segments));
        for (int i = 0; i < segments; ++i) {
            const float angle = (static_cast<float>(i) + 0.5f) * 2.0f * aether::kPi / static_cast<float>(segments);
            const aether::Vec3 local{radius * std::cos(angle), 0.0f, radius * std::sin(angle)};
            const aether::Quaternion tangent = aether::Quaternion::FromAxisAngle(
                {0.0f, 1.0f, 0.0f}, -angle - aether::kPi * 0.5f);
            const aether::Mat4 model = entity_world * aether::Mat4::Translation(local) * tangent.ToMat4() *
                aether::Mat4::Scale({half_length, 0.025f, 0.025f});
            DrawCube(model, 1.0f, 0.66f, 0.24f);
        }
    }

    void EnsureSurface() {
        if (!isVisible() || width() < 1 || height() < 1) return;
        if (!device_) {
            device_ = rhi::CreateDevice(rhi::Backend::D3D12, false);
            if (!device_) return;
            swap_chain_ = device_->CreateSwapChain(reinterpret_cast<void*>(winId()),
                static_cast<aether::u32>(width()), static_cast<aether::u32>(height()), 2);
            if (!swap_chain_) { device_.reset(); return; }
            command_list_ = device_->CreateCommandList();
            rhi::PipelineDesc scene;
            scene.push_constant_size_bytes = sizeof(ScenePushConstants);
            scene.hlsl_source = R"(
cbuffer SceneData : register(b0) { float4x4 viewProjectionModel; float4 tint; };
struct VertexOut { float4 position : SV_POSITION; float4 color : COLOR0; };
VertexOut VSMain(uint id : SV_VertexID) {
    const float3 corners[8] = {
        float3(-1,-1,-1), float3(1,-1,-1), float3(1,1,-1), float3(-1,1,-1),
        float3(-1,-1, 1), float3(1,-1, 1), float3(1,1, 1), float3(-1,1, 1)
    };
    const uint indices[36] = {
        0,2,1, 0,3,2, 5,6,4, 6,7,4,
        4,3,0, 4,7,3, 1,2,5, 2,6,5,
        3,7,2, 2,7,6, 4,0,5, 5,0,1
    };
    const float3 normals[6] = {
        float3(0,0,-1), float3(0,0,1), float3(-1,0,0),
        float3(1,0,0), float3(0,1,0), float3(0,-1,0)
    };
    float light = 0.42 + 0.58 * saturate(dot(normals[id / 6], normalize(float3(-0.4,0.8,-0.25))));
    VertexOut o;
    o.position = mul(viewProjectionModel, float4(corners[indices[id]], 1.0));
    o.color = float4(tint.rgb * light, tint.a);
    return o;
}
float4 PSMain(VertexOut input) : SV_TARGET { return input.color; }
)";
            scene.cull_back_face = false;
            scene.depth_test = true;
            scene_pipeline_ = device_->CreatePipeline(scene, *swap_chain_);

            rhi::PipelineDesc model;
            model.push_constant_size_bytes = sizeof(ModelPushConstants);
            model.use_vertex_buffer = true;
            model.enable_bindless_textures = true;
            model.depth_test = true;
            model.cull_back_face = false;
            model.hlsl_source = R"(
struct PushConstants {
    float4x4 mvp;
    float4 tint;
    uint textureIndex;
    uint useTexture;
    uint2 padding;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> pc : register(b0);
struct VertexIn { float3 position : POSITION; float2 uv : TEXCOORD0; };
struct VertexOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Texture2D modelTextures[32] : register(t0, space0);
SamplerState modelSampler : register(s0, space1);
VertexOut VSMain(VertexIn input) {
    VertexOut output;
    output.position = mul(pc.mvp, float4(input.position, 1.0));
    output.uv = input.uv;
    return output;
}
float4 PSMain(VertexOut input) : SV_TARGET {
    float4 surface = pc.useTexture != 0 ? modelTextures[pc.textureIndex].Sample(modelSampler, input.uv)
                                        : float4(1, 1, 1, 1);
    return surface * pc.tint;
}
)";
            model_pipeline_ = device_->CreatePipeline(model, *swap_chain_);
            constexpr std::array<aether::u8, 4> white{255, 255, 255, 255};
            white_texture_ = device_->CreateTexture(1, 1, white.data());
            if (white_texture_.IsValid()) textures_.emplace("", white_texture_);
        } else if (swap_chain_ &&
                   (swap_chain_->Width() != static_cast<aether::u32>(width()) ||
                    swap_chain_->Height() != static_cast<aether::u32>(height()))) {
            device_->WaitForFence(fence_);
            swap_chain_->Resize(static_cast<aether::u32>(width()), static_cast<aether::u32>(height()));
        }
    }

    void Render() {
        if (!isVisible()) return;
        EnsureSurface();
        if (!device_ || !swap_chain_ || !command_list_) return;
        PrepareModels();
        device_->WaitForFence(fence_);
        swap_chain_->AcquireNextImage();
        command_list_->Reset();
        command_list_->BeginRenderPass(*swap_chain_, {0.075f, 0.105f, 0.13f, 1.0f});
        if (scene_pipeline_.IsValid()) {
            command_list_->BindPipeline(scene_pipeline_);
            const int grid_center_x = static_cast<int>(std::round(camera_target_.x));
            const int grid_center_z = static_cast<int>(std::round(camera_target_.z));
            constexpr int grid_radius = 20;
            for (int offset = -grid_radius; offset <= grid_radius; ++offset) {
                const int x = grid_center_x + offset;
                const int z = grid_center_z + offset;
                const bool x_axis = x == 0;
                const bool z_axis = z == 0;
                DrawCube(aether::Mat4::Translation({static_cast<float>(x), -0.055f, static_cast<float>(grid_center_z)}) *
                             aether::Mat4::Scale({x_axis ? 0.025f : 0.009f, 0.01f, static_cast<float>(grid_radius)}),
                         x_axis ? 0.28f : 0.16f, x_axis ? 0.63f : 0.22f, x_axis ? 0.52f : 0.27f);
                DrawCube(aether::Mat4::Translation({static_cast<float>(grid_center_x), -0.055f, static_cast<float>(z)}) *
                             aether::Mat4::Scale({static_cast<float>(grid_radius), 0.01f, z_axis ? 0.025f : 0.009f}),
                         z_axis ? 0.54f : 0.16f, z_axis ? 0.30f : 0.22f, z_axis ? 0.27f : 0.27f);
            }
            for (aether::Entity entity : document_.Entities()) {
                const auto* transform = document_.GetWorld().GetComponent<aether::Transform>(entity);
                const auto* id = document_.GetWorld().GetComponent<aether::IdComponent>(entity);
                if (!transform || !id) continue;
                const auto* model_renderer = document_.GetWorld().GetComponent<aether::ModelRenderer>(entity);
                if (model_renderer && FindModel(*model_renderer)) continue;
                float r = 0.65f, g = 0.79f, b = 0.85f;
                if (document_.GetWorld().HasComponent<aether::Camera>(entity)) { r = 0.34f; g = 0.67f; b = 1.0f; }
                else if (document_.GetWorld().HasComponent<aether::ModelRenderer>(entity)) { r = 0.67f; g = 0.48f; b = 0.95f; }
                else if (document_.Name(entity) == "Directional Light") { r = 1.0f; g = 0.69f; b = 0.32f; }
                else if (document_.Name(entity) == "Ground") { r = 0.34f; g = 0.76f; b = 0.61f; }
                aether::Vec3 marker_scale{0.42f, 0.42f, 0.42f};
                if (document_.GetWorld().HasComponent<aether::ModelRenderer>(entity)) marker_scale = {0.7f, 0.7f, 0.7f};
                if (document_.Name(entity) == "Ground") marker_scale = {3.5f, 0.12f, 3.5f};
                if (id->guid == selected_guid_) { r = 0.20f; g = 0.95f; b = 0.70f; }
                DrawCube(aether::ComputeWorldTransform(document_.GetWorld(), document_.Guids(), entity) *
                             aether::Mat4::Scale(marker_scale), r, g, b);
            }

            if (model_pipeline_.IsValid()) {
                command_list_->BindPipeline(model_pipeline_);
                command_list_->BindBindlessTextures();
                for (aether::Entity entity : document_.Entities()) {
                    const auto* renderer = document_.GetWorld().GetComponent<aether::ModelRenderer>(entity);
                    if (!renderer) continue;
                    const ModelAsset* model = FindModel(*renderer);
                    if (!model) continue;
                    const aether::Mat4 entity_transform =
                        aether::ComputeWorldTransform(document_.GetWorld(), document_.Guids(), entity);
                    const auto* id = document_.GetWorld().GetComponent<aether::IdComponent>(entity);
                    const bool selected = id && id->guid == selected_guid_;
                    for (const auto& node : model->scene.node_instances) {
                        if (node.mesh_index >= model->meshes.size()) continue;
                        const aether::Mat4 mvp = ViewProjection() * entity_transform * node.world_transform;
                        for (const ModelPrimitive& primitive : model->meshes[node.mesh_index]) {
                            ModelPushConstants constants;
                            constants.mvp = mvp;
                            for (int channel = 0; channel < 4; ++channel)
                                constants.color[channel] = primitive.color[channel];
                            if (selected) {
                                constants.color[0] = constants.color[0] * 0.65f + 0.08f;
                                constants.color[1] = constants.color[1] * 0.65f + 0.35f;
                                constants.color[2] = constants.color[2] * 0.65f + 0.22f;
                            }
                            constants.texture_index = primitive.texture.IsValid()
                                ? primitive.texture.index : white_texture_.index;
                            constants.use_texture = primitive.has_texture ? 1u : 0u;
                            command_list_->BindVertexBuffer(primitive.vertices, sizeof(ModelVertex));
                            command_list_->BindIndexBuffer(primitive.indices, rhi::IndexFormat::UInt32);
                            command_list_->SetPushConstants(&constants, sizeof(constants));
                            command_list_->DrawIndexed(primitive.index_count);
                        }
                    }
                }
                command_list_->BindPipeline(scene_pipeline_);
            }

            const aether::Entity selected = document_.Guids().Find(document_.GetWorld(), selected_guid_);
            if (!selected.IsNull()) {
                const aether::Vec3 position = aether::WorldPosition(document_.GetWorld(), document_.Guids(), selected);
                if (move_tool_) {
                    const float length = MoveGizmoLength();
                    const float thickness = std::clamp(length * 0.035f, 0.025f, 0.08f);
                    DrawCube(aether::Mat4::Translation(position + aether::Vec3{length * 0.5f, 0.0f, 0.0f}) *
                                 aether::Mat4::Scale({length * 0.5f, thickness, thickness}), 1.0f, 0.18f, 0.18f);
                    DrawCube(aether::Mat4::Translation(position + aether::Vec3{0.0f, length * 0.5f, 0.0f}) *
                                 aether::Mat4::Scale({thickness, length * 0.5f, thickness}), 0.20f, 0.95f, 0.30f);
                    DrawCube(aether::Mat4::Translation(position + aether::Vec3{0.0f, 0.0f, length * 0.5f}) *
                                 aether::Mat4::Scale({thickness, thickness, length * 0.5f}), 0.22f, 0.48f, 1.0f);
                    DrawCube(aether::Mat4::Translation(position + aether::Vec3{length, 0.0f, 0.0f}) *
                                 aether::Mat4::Scale({thickness * 1.8f, thickness * 1.8f, thickness * 1.8f}),
                             1.0f, 0.18f, 0.18f);
                    DrawCube(aether::Mat4::Translation(position + aether::Vec3{0.0f, length, 0.0f}) *
                                 aether::Mat4::Scale({thickness * 1.8f, thickness * 1.8f, thickness * 1.8f}),
                             0.20f, 0.95f, 0.30f);
                    DrawCube(aether::Mat4::Translation(position + aether::Vec3{0.0f, 0.0f, length}) *
                                 aether::Mat4::Scale({thickness * 1.8f, thickness * 1.8f, thickness * 1.8f}),
                             0.22f, 0.48f, 1.0f);
                } else if (scale_tool_) {
                    const float handle = MoveGizmoLength() * 0.85f;
                    DrawCube(aether::Mat4::Translation(position + aether::Vec3{0.0f, handle, 0.0f}) *
                                 aether::Mat4::Scale({0.16f, 0.16f, 0.16f}), 1.0f, 0.72f, 0.24f);
                } else if (rotate_y_tool_) {
                    DrawRotationRing(selected);
                }
            }
        }
        command_list_->EndRenderPass();
        command_list_->Close();
        fence_ = device_->Submit(*command_list_, swap_chain_.get());
        swap_chain_->Present(true);
    }

    QTimer timer_;
    QElapsedTimer navigation_clock_;
    std::unique_ptr<rhi::IDevice> device_;
    std::unique_ptr<rhi::ISwapChain> swap_chain_;
    std::unique_ptr<rhi::ICommandList> command_list_;
    rhi::PipelineHandle scene_pipeline_;
    rhi::PipelineHandle model_pipeline_;
    rhi::SampledTextureHandle white_texture_;
    std::filesystem::path content_root_;
    std::unordered_map<std::string, ModelAsset> models_;
    std::unordered_map<std::string, rhi::SampledTextureHandle> textures_;
    aether::u64 fence_ = 0;
    aether::editor::SceneDocument& document_;
    aether::EntityGuid selected_guid_{};
    std::function<void(const aether::EntityGuid&)> entity_selected_;
    std::function<void(const aether::EntityGuid&)> transform_changed_;
    std::function<void(aether::input::Key, bool)> input_button_;
    std::function<void(float, float)> input_mouse_delta_;
    std::function<void(float)> input_mouse_wheel_;
    std::function<void()> input_clear_;
    std::function<void(bool)> play_input_focus_changed_;
    QSet<int> navigation_keys_;
    bool play_input_active_ = false;
    bool has_play_mouse_position_ = false;
    QPointF last_play_mouse_position_;
    aether::EntityGuid keyboard_move_guid_{};
    bool move_tool_ = false;
    bool fit_pending_ = false;
    bool rotate_y_tool_ = false;
    bool scale_tool_ = false;
    bool grid_snap_ = false;
    bool dragging_ = false;
    bool panning_ = false;
    bool orbiting_ = false;
    aether::EntityGuid drag_guid_{};
    aether::Vec3 drag_origin_{};
    aether::Vec3 drag_scale_origin_{1.0f, 1.0f, 1.0f};
    aether::Quaternion drag_rotation_origin_{};
    int drag_move_axis_ = -1;
    QPointF drag_start_;
    QPointF camera_drag_start_;
    aether::Vec3 pan_origin_{};
    aether::Vec3 camera_target_{0.0f, 0.8f, 0.0f};
    float camera_distance_ = 18.0f;
    float camera_yaw_ = 0.35f;
    float camera_pitch_ = 0.42f;
    float camera_yaw_start_ = 0.0f;
    float camera_pitch_start_ = 0.0f;
};

QDockWidget* MakeDock(const QString& title, QWidget* content, QMainWindow& window,
                      Qt::DockWidgetArea area, const QString& object_name) {
    auto* dock = new QDockWidget(title, &window);
    dock->setObjectName(object_name);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                      QDockWidget::DockWidgetClosable);
    dock->setWidget(content);
    window.addDockWidget(area, dock);
    return dock;
}

class EditorLogConsole final : public QPlainTextEdit {
public:
    EditorLogConsole() {
        setReadOnly(true);
        setMaximumBlockCount(1000);
        for (const auto& line : aether::Logger::Instance().Recent()) Append(line);
        const QPointer<EditorLogConsole> self(this);
        sink_id_ = aether::Logger::Instance().AddSink([self](const aether::LogLine& line) {
            if (!self) return;
            const QString message = QString("[%1] [%2] %3")
                .arg(QString::fromUtf8(aether::LogLevelName(line.level)))
                .arg(QString::fromStdString(line.category))
                .arg(QString::fromStdString(line.message));
            QMetaObject::invokeMethod(self, [self, message] {
                if (self) self->appendPlainText(message);
            }, Qt::QueuedConnection);
        });
    }

    ~EditorLogConsole() override { aether::Logger::Instance().RemoveSink(sink_id_); }

private:
    void Append(const aether::LogLine& line) {
        appendPlainText(QString("[%1] [%2] %3")
            .arg(QString::fromUtf8(aether::LogLevelName(line.level)))
            .arg(QString::fromStdString(line.category))
            .arg(QString::fromStdString(line.message)));
    }

    int sink_id_ = 0;
};

QTreeWidget* MakeHierarchy() {
    auto* tree = new QTreeWidget;
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(true);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setUniformRowHeights(true);
    tree->setAnimated(true);
    tree->setIndentation(16);
    tree->setAccessibleName("World hierarchy");
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    auto* root = new QTreeWidgetItem(tree, {"WORLD"});
    root->setFlags(root->flags() & ~Qt::ItemIsSelectable);
    tree->expandAll();
    return tree;
}

// The editor displays local rotation as pitch (X), yaw (Y), roll (Z), using
// the engine's Y-X-Z composition order. Scene data remains a normalized
// quaternion, so editing does not change the serialized Transform format.
aether::Vec3 RotationToEulerDegrees(aether::Quaternion rotation) {
    rotation = rotation.Normalized();
    const aether::f32 x = rotation.x;
    const aether::f32 y = rotation.y;
    const aether::f32 z = rotation.z;
    const aether::f32 w = rotation.w;
    const aether::f32 sin_pitch = std::clamp(2.0f * (w * x - y * z), -1.0f, 1.0f);
    const aether::f32 pitch = std::asin(sin_pitch);
    aether::f32 yaw;
    aether::f32 roll;
    if (std::fabs(sin_pitch) > 0.9999f) {
        yaw = std::atan2(-2.0f * (x * z - w * y), 1.0f - 2.0f * (y * y + z * z));
        roll = 0.0f;
    } else {
        yaw = std::atan2(2.0f * (w * y + z * x), 1.0f - 2.0f * (x * x + y * y));
        roll = std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (x * x + z * z));
    }
    return {aether::Degrees(pitch), aether::Degrees(yaw), aether::Degrees(roll)};
}

aether::Quaternion EulerDegreesToRotation(const aether::Vec3& degrees) {
    const aether::Quaternion pitch = aether::Quaternion::FromAxisAngle({1.0f, 0.0f, 0.0f}, aether::Radians(degrees.x));
    const aether::Quaternion yaw = aether::Quaternion::FromAxisAngle({0.0f, 1.0f, 0.0f}, aether::Radians(degrees.y));
    const aether::Quaternion roll = aether::Quaternion::FromAxisAngle({0.0f, 0.0f, 1.0f}, aether::Radians(degrees.z));
    return (yaw * pitch * roll).Normalized();
}

QWidget* MakeHierarchyPanel(QTreeWidget*& tree, QLineEdit*& filter,
                            QPushButton*& add, QPushButton*& duplicate) {
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    filter = new QLineEdit(body);
    filter->setObjectName("hierarchyFilter");
    filter->setAccessibleName("Filter the World hierarchy");
    filter->setPlaceholderText("Filter entities…");
    filter->setToolTip("Filter entity names. Matching ancestors remain visible for context. Press Ctrl+F to focus search.");
    filter->setClearButtonEnabled(true);
    auto* focus_hierarchy_search = new QAction("Focus Hierarchy Search", body);
    focus_hierarchy_search->setShortcut(QKeySequence::Find);
    focus_hierarchy_search->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    body->addAction(focus_hierarchy_search);
    QObject::connect(focus_hierarchy_search, &QAction::triggered, filter, [filter] {
        filter->setFocus(Qt::ShortcutFocusReason);
        filter->selectAll();
    });
    layout->addWidget(filter);
    auto* actions = new QHBoxLayout;
    add = new QPushButton("Add Entity", body);
    add->setAccessibleName("Add entity to the World");
    add->setToolTip("Create a new entity in the active world");
    duplicate = new QPushButton("Duplicate", body);
    duplicate->setAccessibleName("Duplicate selected entity");
    duplicate->setEnabled(false);
    actions->addWidget(add);
    actions->addWidget(duplicate);
    layout->addLayout(actions);
    tree = MakeHierarchy();
    layout->addWidget(tree, 1);
    return body;
}

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

QWidget* MakeInspector(QScrollArea*& inspector_scroll, QStackedWidget*& inspector_stack,
                       QPushButton*& empty_add_entity,
                       QLabel*& selection_status, QLineEdit*& entity_name,
                       std::array<QDoubleSpinBox*, 3>& position,
                       std::array<QDoubleSpinBox*, 3>& rotation,
                       std::array<QDoubleSpinBox*, 3>& scale,
                       QPushButton*& reset_position, QPushButton*& reset_rotation,
                       QPushButton*& reset_scale,
                       CameraInspectorWidgets& camera) {
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);
    selection_status = new QLabel("NO SELECTION · SELECT AN ENTITY");
    selection_status->setObjectName("section");
    layout->addWidget(selection_status);
    entity_name = new QLineEdit;
    entity_name->setObjectName("entityNameField");
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
    auto* rotation_title = new QLabel("Rotation · degrees");
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
        rotation[axis]->setSuffix("°");
        rotation[axis]->setKeyboardTracking(false);
        rotation[axis]->setAccessibleName(QString("Local %1 rotation in degrees").arg(QChar('X' + axis)));
        rotation[axis]->setToolTip(QString("Local %1 rotation in degrees").arg(QChar('X' + axis)));
        rotation[axis]->setEnabled(false);
        rotation_form->addRow(make_axis_label(axis, QString::fromUtf8(rotation_labels[axis])), rotation[axis]);
    }
    layout->addLayout(rotation_form);

    auto* scale_header = new QHBoxLayout;
    auto* scale_title = new QLabel("Scale · local multiplier");
    scale_title->setObjectName("muted");
    reset_scale = new QPushButton("Reset Scale");
    reset_scale->setToolTip("Restore the imported object size (1× on each axis). This edit can be undone.");
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
        scale[axis]->setSuffix("×");
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
    camera.fov = make_camera_spin("cameraFov", 1.0, 179.0, 2, 1.0, "°");
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

    layout->addStretch();

    auto* empty = new QWidget;
    auto* empty_layout = new QVBoxLayout(empty);
    empty_layout->setContentsMargins(24, 30, 24, 30);
    empty_layout->addStretch(1);
    auto* empty_icon = new QLabel("◇", empty);
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
    empty_add_entity = new QPushButton("Create Entity…", empty);
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

class AssetBrowserDelegate final : public QStyledItemDelegate {
public:
    explicit AssetBrowserDelegate(const QFileSystemModel& model, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), model_(model) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        if (!index.isValid()) return;

        const QFileInfo info = model_.fileInfo(index);
        const QRect icon_rect(option.rect.center().x() - 24, option.rect.top() + 5, 48, 48);
        painter->save();
        QColor icon_back("#151a21");
        if (option.state & QStyle::State_Selected) icon_back = QColor("#244653");
        else if (option.state & QStyle::State_MouseOver) icon_back = QColor("#202c35");
        painter->fillRect(icon_rect, icon_back);
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->translate(icon_rect.topLeft());

        if (info.isDir()) {
            QPainterPath rear;
            rear.moveTo(5, 11); rear.lineTo(20, 11); rear.lineTo(25, 16);
            rear.lineTo(44, 16); rear.lineTo(44, 38); rear.lineTo(5, 38); rear.closeSubpath();
            painter->setPen(QPen(QColor("#527f80"), 1.2));
            painter->setBrush(QColor("#294b55"));
            painter->drawPath(rear);

            QPainterPath front;
            front.moveTo(4, 19); front.lineTo(44, 19); front.lineTo(41, 41);
            front.quadTo(40.7, 43, 38.5, 43); front.lineTo(7, 43);
            front.quadTo(4.5, 43, 4, 40); front.closeSubpath();
            QLinearGradient folder_fill(0, 19, 0, 43);
            folder_fill.setColorAt(0.0, QColor("#5bd0af"));
            folder_fill.setColorAt(1.0, QColor("#277b77"));
            painter->setPen(QPen(QColor("#68d6b7"), 1.1));
            painter->setBrush(folder_fill);
            painter->drawPath(front);
            painter->setPen(QPen(QColor(255, 255, 255, 68), 1.0));
            painter->drawLine(QPointF(8, 23), QPointF(39, 23));
        } else {
            const QString extension = info.suffix().toLower();
            QString badge = "FILE";
            QColor accent("#8da0ae");
            if (extension == "gltf" || extension == "glb" || extension == "fbx" || extension == "obj") {
                badge = "3D"; accent = QColor("#55d7b5");
            } else if (extension == "ascene" || extension == "aesc") {
                badge = "SCN"; accent = QColor("#b9a0ff");
            } else if (extension == "asequence" || extension == "anim") {
                badge = "ANIM"; accent = QColor("#8bbcff");
            } else if (extension == "png" || extension == "jpg" || extension == "jpeg" ||
                       extension == "tga" || extension == "hdr" || extension == "exr") {
                badge = "IMG"; accent = QColor("#f4bd72");
            } else if (extension == "lua" || extension == "luau" || extension == "cs" ||
                       extension == "cpp" || extension == "h" || extension == "hpp") {
                badge = "CODE"; accent = QColor("#86b9ff");
            } else if (extension == "wav" || extension == "ogg" || extension == "mp3" || extension == "flac") {
                badge = "SFX"; accent = QColor("#ec9bc7");
            }

            QPainterPath page;
            page.moveTo(10, 4); page.lineTo(29, 4); page.lineTo(39, 14);
            page.lineTo(39, 41); page.quadTo(39, 44, 36, 44); page.lineTo(11, 44);
            page.quadTo(8, 44, 8, 41); page.lineTo(8, 7); page.quadTo(8, 4, 10, 4);
            page.closeSubpath();
            painter->setPen(QPen(QColor("#455967"), 1.2));
            painter->setBrush(QColor("#202c36"));
            painter->drawPath(page);
            QPainterPath fold;
            fold.moveTo(29, 4); fold.lineTo(29, 13); fold.quadTo(29, 14, 31, 14); fold.lineTo(39, 14);
            painter->setPen(QPen(QColor("#455967"), 1.0));
            painter->setBrush(QColor("#33434f"));
            painter->drawPath(fold);
            painter->setPen(QPen(accent, 2.2, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(QPointF(14, 20), QPointF(14, 37));
            painter->setPen(QPen(QColor("#738490"), 1.2, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(QPointF(20, 23), QPointF(33, 23));
            painter->drawLine(QPointF(20, 28), QPointF(33, 28));
            painter->drawLine(QPointF(20, 33), QPointF(29, 33));

            const QRectF badge_rect(22, 35, 22, 11);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor("#172128"));
            painter->drawRoundedRect(badge_rect, 3, 3);
            painter->setPen(accent);
            QFont badge_font = painter->font();
            badge_font.setPointSizeF(5.8);
            badge_font.setBold(true);
            painter->setFont(badge_font);
            painter->drawText(badge_rect, Qt::AlignCenter, badge);
        }

        painter->restore();
    }

private:
    const QFileSystemModel& model_;
};

QWidget* MakeContentBrowser(QComboBox*& breadcrumb, QFileSystemModel*& asset_model, QListView*& asset_view,
                            QPushButton*& parent_button, QPushButton*& new_folder_button,
                            QPushButton*& place_model_button, QLineEdit*& asset_filter,
                            QComboBox*& asset_type_filter,
                            QLabel*& asset_context, QWidget*& welcome_panel,
                            QPushButton*& welcome_open_project, QPushButton*& welcome_new_project) {
    auto* root = new QWidget;
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(12, 8, 12, 12);
    auto* navigation = new QHBoxLayout;
    parent_button = new QPushButton("Up");
    parent_button->setAccessibleName("Open parent folder");
    parent_button->setToolTip("Parent folder");
    parent_button->setFixedWidth(48);
    new_folder_button = new QPushButton("+ Folder");
    new_folder_button->setAccessibleName("Create a folder in Content");
    place_model_button = new QPushButton("Place Model");
    place_model_button->setAccessibleName("Place selected model in the World");
    place_model_button->setObjectName("placeModelButton");
    place_model_button->setToolTip("Add the selected glTF or GLB asset to the active scene");
    place_model_button->setEnabled(false);
    asset_filter = new QLineEdit;
    asset_filter->setObjectName("contentSearch");
    asset_filter->setAccessibleName("Search names in the current Content folder");
    asset_filter->setPlaceholderText("Search this folder…");
    asset_filter->setToolTip("Search file and folder names in the current Content folder. Combine with the asset type filter. Press Ctrl+F to focus search.");
    asset_filter->setClearButtonEnabled(true);
    asset_filter->setMaximumWidth(220);
    auto* focus_content_search = new QAction("Focus Content Search", root);
    focus_content_search->setShortcut(QKeySequence::Find);
    focus_content_search->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    root->addAction(focus_content_search);
    QObject::connect(focus_content_search, &QAction::triggered, asset_filter, [asset_filter] {
        asset_filter->setFocus(Qt::ShortcutFocusReason);
        asset_filter->selectAll();
    });
    asset_type_filter = new QComboBox(root);
    asset_type_filter->setObjectName("contentTypeFilter");
    asset_type_filter->setAccessibleName("Filter content by asset type");
    asset_type_filter->setToolTip("Filter assets in the current folder by type. This combines with the name search.");
    asset_type_filter->addItem("All Assets", QStringList{});
    asset_type_filter->addItem("Models", QStringList{"gltf", "glb"});
    asset_type_filter->addItem("Scenes", QStringList{"ascene", "aesc"});
    asset_type_filter->addItem("Sequences", QStringList{"asequence"});
    asset_type_filter->addItem("Projects", QStringList{"aproject"});
    breadcrumb = new QComboBox;
    breadcrumb->setObjectName("contentBreadcrumb");
    breadcrumb->setToolTip("Choose a parent folder in the project Content directory");
    breadcrumb->setMinimumContentsLength(22);
    breadcrumb->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    breadcrumb->addItem("NO PROJECT  /  CONTENT");
    breadcrumb->setEnabled(false);
    navigation->addWidget(parent_button);
    navigation->addWidget(breadcrumb, 1);
    navigation->addWidget(asset_type_filter);
    navigation->addWidget(asset_filter);
    navigation->addWidget(new_folder_button);
    navigation->addWidget(place_model_button);
    layout->addLayout(navigation);
    asset_context = new QLabel("", root);
    asset_context->setObjectName("muted");
    asset_context->setWordWrap(true);
    layout->addWidget(asset_context);
    welcome_panel = new QWidget(root);
    welcome_panel->setObjectName("contentWelcomePanel");
    auto* welcome_layout = new QVBoxLayout(welcome_panel);
    welcome_layout->setContentsMargins(24, 18, 24, 18);
    welcome_layout->addStretch();
    auto* welcome_title = new QLabel("Start with a project", welcome_panel);
    welcome_title->setObjectName("contentWelcomeTitle");
    welcome_title->setAlignment(Qt::AlignCenter);
    welcome_layout->addWidget(welcome_title);
    auto* welcome_description = new QLabel(
        "Create a project or open one to browse assets, place models, and edit scenes.", welcome_panel);
    welcome_description->setObjectName("contentWelcomeDescription");
    welcome_description->setWordWrap(true);
    welcome_description->setAlignment(Qt::AlignCenter);
    welcome_layout->addWidget(welcome_description);
    auto* welcome_actions = new QHBoxLayout;
    welcome_actions->addStretch();
    welcome_open_project = new QPushButton("Open Project…", welcome_panel);
    welcome_open_project->setObjectName("emptyStateAction");
    welcome_open_project->setAccessibleName("Open an existing Aether project");
    welcome_open_project->setToolTip("Choose an existing .aproject file");
    welcome_new_project = new QPushButton("Create Project…", welcome_panel);
    welcome_new_project->setAccessibleName("Create a new Aether project");
    welcome_new_project->setToolTip("Create a project with the standard Content folders");
    welcome_actions->addWidget(welcome_open_project);
    welcome_actions->addWidget(welcome_new_project);
    welcome_actions->addStretch();
    welcome_layout->addLayout(welcome_actions);
    welcome_layout->addStretch();
    layout->addWidget(welcome_panel, 1);
    asset_model = new QFileSystemModel(root);
    asset_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    asset_model->setRootPath(QString());
    asset_view = new QListView(root);
    asset_view->setAccessibleName("Project content assets");
    asset_view->setModel(asset_model);
    asset_view->setItemDelegate(new AssetBrowserDelegate(*asset_model, asset_view));
    asset_view->setViewMode(QListView::IconMode);
    asset_view->setFlow(QListView::LeftToRight);
    asset_view->setWrapping(true);
    asset_view->setResizeMode(QListView::Adjust);
    asset_view->setMovement(QListView::Static);
    asset_view->setIconSize(QSize(48, 48));
    asset_view->setGridSize(QSize(148, 104));
    asset_view->setSpacing(8);
    asset_view->setUniformItemSizes(true);
    asset_view->setWordWrap(true);
    asset_view->setTextElideMode(Qt::ElideMiddle);
    asset_view->setSelectionMode(QAbstractItemView::SingleSelection);
    asset_view->setEnabled(false);
    asset_view->setRootIndex(QModelIndex());
    layout->addWidget(asset_view, 1);
    return root;
}

class EditorWindow final : public QMainWindow {
public:
    EditorWindow() {
        RegisterEditorComponentSchemas();
        std::string plugin_error;
        if (!StartDefaultEnginePlugins(plugin_manager_, &plugin_error))
            AETHER_LOG_ERROR("Editor", "Couldn't start the engine plugins: %s", plugin_error.c_str());
        LoadRecentProjects();
        setWindowTitle("Aether Engine · World Editor");
        resize(1480, 900);
        setDockOptions(QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks | QMainWindow::AnimatedDocks);
        BuildMenus(); BuildToolbar();

        viewport_ = new RhiViewport(scene_document_);
        viewport_->setMinimumSize(480, 300);
        viewport_->SetInputCallbacks(
            [this](aether::input::Key key, bool down) {
                if (play_runtime_) play_runtime_->SetButton(key, down);
            },
            [this](float dx, float dy) {
                if (play_runtime_) play_runtime_->AddMouseDelta(dx, dy);
            },
            [this](float delta) {
                if (play_runtime_) play_runtime_->AddMouseWheel(delta);
            },
            [this] {
                if (play_runtime_) play_runtime_->ClearInput();
            });
        viewport_->SetPlayInputFocusChangedCallback([this](bool) { UpdatePlayInputHint(); });
        setCentralWidget(viewport_);
        hierarchy_dock_ = MakeDock("HIERARCHY",
                                   MakeHierarchyPanel(hierarchy_tree_, hierarchy_filter_, add_entity_button_,
                                                      duplicate_entity_button_),
                                   *this, Qt::LeftDockWidgetArea, "hierarchyDock");
        auto* inspector_content = MakeInspector(inspector_scroll_, inspector_stack_,
                                                               empty_add_entity_button_,
                                                               inspector_status_, inspector_name_,
                                                               position_fields_, rotation_fields_, scale_fields_,
                                                               reset_position_button_, reset_rotation_button_, reset_scale_button_,
                                                               camera_inspector_);
        auto* inspector_body = inspector_stack_->widget(1);
        auto* inspector_layout = qobject_cast<QVBoxLayout*>(inspector_body->layout());
        physics_component_panel_ = new aether::editor::qt::PhysicsComponentPanel(scene_document_, [this] {
            RefreshHierarchy(true);
            UpdateSceneStatus();
        }, inspector_body);
        inspector_layout->insertWidget(inspector_layout->count() - 1, physics_component_panel_);
#if defined(AETHER_QT_EDITOR_SCRIPTING)
        script_component_editor_ = new aether::editor::qt::ScriptComponentEditor(scene_document_, [this] {
            RefreshHierarchy(true);
            UpdateSceneStatus();
        }, inspector_body);
        inspector_layout->insertWidget(inspector_layout->count() - 1, script_component_editor_);
#endif
        inspector_dock_ = MakeDock("INSPECTOR", inspector_content, *this,
                                   Qt::RightDockWidgetArea, "inspectorDock");
        hierarchy_dock_->setMinimumWidth(190);
        inspector_dock_->setMinimumWidth(250);
        inspector_dock_->widget()->setMinimumWidth(240);

        content_dock_ = MakeDock("CONTENT BROWSER",
                                      MakeContentBrowser(content_breadcrumb_, asset_model_, asset_view_,
                                                         parent_folder_button_, new_folder_button_,
                                                         place_model_button_, asset_filter_, asset_type_filter_,
                                                         asset_context_,
                                                         content_welcome_panel_, welcome_open_project_button_,
                                                         welcome_new_project_button_), *this,
                                      Qt::BottomDockWidgetArea, "contentDock");
        output_dock_ = MakeDock("OUTPUT", new EditorLogConsole, *this, Qt::BottomDockWidgetArea, "outputDock");
        sequence_dock_ = MakeDock("ANIMATION · SEQUENCER", MakeSequenceEditor(), *this,
                                       Qt::BottomDockWidgetArea, "sequenceDock");
        tabifyDockWidget(content_dock_, output_dock_);
        tabifyDockWidget(content_dock_, sequence_dock_);
        content_dock_->raise();
        resizeDocks({content_dock_}, {230}, Qt::Vertical);
        window_menu_->addAction(hierarchy_dock_->toggleViewAction());
        window_menu_->addAction(inspector_dock_->toggleViewAction());
        window_menu_->addAction(content_dock_->toggleViewAction());
        window_menu_->addAction(output_dock_->toggleViewAction());
        window_menu_->addAction(sequence_dock_->toggleViewAction());
        window_menu_->addSeparator();
        QAction* reset_layout_action = window_menu_->addAction("Reset Workspace Layout");
        reset_layout_action->setToolTip("Restore Hierarchy, Inspector, and Content Browser to their default positions");
        QObject::connect(reset_layout_action, &QAction::triggered, this, [this] { ResetWorkspaceLayout(); });

        QSettings settings("Aether", "EditorQt");
        if (settings.contains("window/geometry")) restoreGeometry(settings.value("window/geometry").toByteArray());
        const bool restored_layout = settings.contains("window/state") &&
            restoreState(settings.value("window/state").toByteArray(), 1);
        const bool compact_screen = screen() && screen()->availableGeometry().width() < 1600;
        if (!restored_layout || compact_screen) QTimer::singleShot(0, this, [this] {
            resizeDocks({hierarchy_dock_, inspector_dock_}, {230, 270}, Qt::Horizontal);
            resizeDocks({content_dock_}, {210}, Qt::Vertical);
        });
        auto* palette_action = new QAction(this);
        palette_action->setShortcut(QKeySequence("Ctrl+K"));
        addAction(palette_action);
        QObject::connect(palette_action, &QAction::triggered, this, [this] { ShowCommandPalette(); });
        QObject::connect(search_button_, &QPushButton::clicked, this, [this] { ShowCommandPalette(); });
        QObject::connect(hierarchy_filter_, &QLineEdit::textChanged, this,
                         [this](const QString&) { RefreshHierarchy(true); });
        QObject::connect(add_entity_button_, &QPushButton::clicked, this, [this] { ShowAddEntityMenu(); });
        QObject::connect(empty_add_entity_button_, &QPushButton::clicked, create_entity_action_, &QAction::trigger);
        QObject::connect(duplicate_entity_button_, &QPushButton::clicked, this, [this] { DuplicateSelectedEntity(); });
        QObject::connect(parent_folder_button_, &QPushButton::clicked, this, [this] { NavigateContentParent(); });
        QObject::connect(welcome_open_project_button_, &QPushButton::clicked,
                         open_project_action_, &QAction::trigger);
        QObject::connect(welcome_new_project_button_, &QPushButton::clicked,
                         new_project_action_, &QAction::trigger);
        QObject::connect(new_folder_button_, &QPushButton::clicked, this, [this] { CreateContentFolder(); });
        QObject::connect(place_model_button_, &QPushButton::clicked, this, [this] { PlaceSelectedContentModel(); });
        QObject::connect(content_breadcrumb_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
            const QString path = content_breadcrumb_->itemData(index).toString();
            if (!path.isEmpty() && path != current_content_path_) SetContentDirectory(path);
        });
        QObject::connect(asset_filter_, &QLineEdit::textChanged, this, [this](const QString&) {
            ApplyContentFilters();
            UpdateSelectedContentAsset(asset_view_->currentIndex());
        });
        QObject::connect(asset_type_filter_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            ApplyContentFilters();
            UpdateSelectedContentAsset(asset_view_->currentIndex());
        });
        QObject::connect(asset_model_, &QFileSystemModel::directoryLoaded, this, [this](const QString& path) {
            if (QDir::cleanPath(path) == QDir::cleanPath(current_content_path_))
                UpdateSelectedContentAsset(asset_view_->currentIndex());
        });
        QObject::connect(asset_view_, &QListView::clicked, this, [this](const QModelIndex& index) {
            UpdateSelectedContentAsset(index);
            statusBar()->showMessage(QString("●  ASSET SELECTED     %1")
                                         .arg(asset_model_->fileName(index)));
        });
        QObject::connect(asset_view_->selectionModel(), &QItemSelectionModel::currentChanged, this,
                         [this](const QModelIndex& current, const QModelIndex&) {
                             UpdateSelectedContentAsset(current);
                         });
        QObject::connect(asset_view_, &QListView::doubleClicked, this, [this](const QModelIndex& index) {
            const QString path = asset_model_->filePath(index);
            if (asset_model_->isDir(index)) SetContentDirectory(path);
            else OpenContentAsset(path);
        });
        statusBar()->showMessage("Ready · World Editor");
        renderer_badge_ = new QLabel("D3D12 · RHI", this);
        renderer_badge_->setObjectName("rendererBadge");
        renderer_badge_->setToolTip("Active graphics backend");
        status_state_badge_ = new QLabel("SCENE SAVED", this);
        status_state_badge_->setObjectName("sceneStateBadge");
        status_state_badge_->setProperty("state", "saved");
        status_state_badge_->setToolTip("Scene save state");
        statusBar()->addPermanentWidget(renderer_badge_);
        statusBar()->addPermanentWidget(status_state_badge_);
        play_input_hint_ = new QLabel(this);
        play_input_hint_->setObjectName("playInputHint");
        play_input_hint_->setStyleSheet("color: #79dfbf; padding: 0 10px; font-weight: 600;");
        play_input_hint_->setVisible(false);
        statusBar()->addPermanentWidget(play_input_hint_);
        save_project_action_->setEnabled(false);
        parent_folder_button_->setEnabled(false);
        new_folder_button_->setEnabled(false);
        asset_filter_->setEnabled(false);
        asset_type_filter_->setEnabled(false);
        asset_view_->setVisible(false);
        asset_context_->setVisible(false);
        content_welcome_panel_->setVisible(true);
        QObject::connect(hierarchy_tree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) { SelectHierarchyItem(current); });
        QObject::connect(hierarchy_tree_, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& point) { ShowHierarchyContextMenu(point); });
        viewport_->SetEntitySelectedCallback([this](const aether::EntityGuid& guid) {
            std::function<QTreeWidgetItem*(QTreeWidgetItem*)> find_item =
                [&](QTreeWidgetItem* item) -> QTreeWidgetItem* {
                    if (item->data(0, Qt::UserRole).toString().toStdString() == aether::ToString(guid))
                        return item;
                    for (int child_index = 0; child_index < item->childCount(); ++child_index)
                        if (auto* found = find_item(item->child(child_index))) return found;
                    return nullptr;
                };
            for (int root_index = 0; root_index < hierarchy_tree_->topLevelItemCount(); ++root_index) {
                if (auto* found = find_item(hierarchy_tree_->topLevelItem(root_index))) {
                    hierarchy_tree_->setCurrentItem(found);
                    return;
                }
            }
        });
        viewport_->SetTransformChangedCallback([this](const aether::EntityGuid& guid) {
            if (guid != selected_guid_) return;
            SyncInspectorPosition();
            UpdateSceneStatus();
        });
        QObject::connect(inspector_name_, &QLineEdit::editingFinished, this, [this] {
            if (selected_entity_.IsNull()) return;
            const std::string name = inspector_name_->text().trimmed().toStdString();
            if (name.empty()) { SelectHierarchyItem(hierarchy_tree_->currentItem()); return; }
            scene_document_.Rename(selected_entity_, name);
            RefreshHierarchy();
            UpdateSceneStatus();
        });
        for (auto* field : position_fields_) {
            QObject::connect(field, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                             [this](double) { ApplyInspectorPosition(false); });
            QObject::connect(field, &QDoubleSpinBox::editingFinished, this,
                             [this] { ApplyInspectorPosition(true); });
        }
        for (auto* field : rotation_fields_) {
            QObject::connect(field, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                             [this](double) { ApplyInspectorRotation(false); });
            QObject::connect(field, &QDoubleSpinBox::editingFinished, this,
                             [this] { ApplyInspectorRotation(true); });
        }
        for (auto* field : scale_fields_) {
            QObject::connect(field, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                             [this](double) { ApplyInspectorScale(false); });
            QObject::connect(field, &QDoubleSpinBox::editingFinished, this,
                             [this] { ApplyInspectorScale(true); });
        }
        QObject::connect(reset_position_button_, &QPushButton::clicked, this, [this] {
            if (selected_entity_.IsNull() ||
                !scene_document_.SetPosition(selected_entity_, aether::Vec3{}, true)) return;
            SyncInspectorPosition();
            UpdateSceneStatus();
        });
        QObject::connect(reset_rotation_button_, &QPushButton::clicked, this, [this] {
            if (selected_entity_.IsNull() ||
                !scene_document_.SetRotation(selected_entity_, aether::Quaternion::Identity(), true)) return;
            SyncInspectorRotation();
            UpdateSceneStatus();
        });
        QObject::connect(reset_scale_button_, &QPushButton::clicked, this, [this] {
            if (selected_entity_.IsNull() ||
                !scene_document_.SetScale(selected_entity_, aether::Vec3{1.0f, 1.0f, 1.0f}, true)) return;
            SyncInspectorScale();
            UpdateSceneStatus();
        });
        QObject::connect(camera_inspector_.projection, qOverload<int>(&QComboBox::currentIndexChanged), this,
                         [this](int index) {
                             if (selected_entity_.IsNull() || index < 0) return;
                             const auto projection = static_cast<aether::Projection>(
                                 camera_inspector_.projection->itemData(index).toInt());
                             if (scene_document_.SetCameraField(selected_entity_, "projection",
                                                                aether::reflect::Any(projection))) {
                                 SyncCameraInspector();
                                 UpdateSceneStatus();
                             }
                         });
        QObject::connect(camera_inspector_.fov, &QDoubleSpinBox::editingFinished, this, [this] {
            ApplyCameraInspectorField("fov_degrees", static_cast<aether::f32>(camera_inspector_.fov->value()));
        });
        QObject::connect(camera_inspector_.ortho_height, &QDoubleSpinBox::editingFinished, this, [this] {
            ApplyCameraInspectorField("ortho_height", static_cast<aether::f32>(camera_inspector_.ortho_height->value()));
        });
        QObject::connect(camera_inspector_.near_plane, &QDoubleSpinBox::editingFinished, this, [this] {
            ApplyCameraInspectorField("near_plane", static_cast<aether::f32>(camera_inspector_.near_plane->value()));
        });
        QObject::connect(camera_inspector_.far_plane, &QDoubleSpinBox::editingFinished, this, [this] {
            ApplyCameraInspectorField("far_plane", static_cast<aether::f32>(camera_inspector_.far_plane->value()));
        });
        QObject::connect(camera_inspector_.priority, &QSpinBox::editingFinished, this, [this] {
            ApplyCameraInspectorField("priority", camera_inspector_.priority->value());
        });
        const auto connect_lens = [this](QDoubleSpinBox* spin, const char* field) {
            QObject::connect(spin, &QDoubleSpinBox::editingFinished, this, [this, spin, field] {
                if (selected_entity_.IsNull()) return;
                if (scene_document_.SetCineCameraField(selected_entity_, field,
                                                       aether::reflect::Any(static_cast<aether::f32>(spin->value())))) {
                    SyncCineCameraInspector();
                    UpdateSceneStatus();
                }
            });
        };
        connect_lens(camera_inspector_.focal_length, "focal_length_mm");
        connect_lens(camera_inspector_.sensor_width, "sensor_width_mm");
        connect_lens(camera_inspector_.sensor_height, "sensor_height_mm");
        connect_lens(camera_inspector_.aperture, "aperture_f");
        connect_lens(camera_inspector_.focus_distance, "focus_distance");
        scene_document_.NewScene();
        RefreshHierarchy();
        viewport_->FitAll();
        UpdateSceneStatus();
    }

    void OpenProjectFile(const std::filesystem::path& file) { OpenProject(file, false); }

    bool RunSelfTest() {
        const auto require = [](bool condition, const char* message) {
            if (!condition) AETHER_LOG_ERROR("QtEditor", "Self-test failed: %s", message);
            return condition;
        };
        bool passed = true;
        passed &= require(scene_document_.Entities().size() == 5, "default scene should have five entities");
        passed &= require(!selected_entity_.IsNull() && inspector_name_->isEnabled() &&
                          inspector_name_->text() == QString::fromStdString(scene_document_.Name(selected_entity_)) &&
                          reset_position_button_->isEnabled() && reset_rotation_button_->isEnabled() &&
                          inspector_status_->text() == "ENTITY · ACTIVE SELECTION",
                          "Inspector should show the real name and state of the selected hierarchy entity");
        SelectHierarchyItem(nullptr);
        passed &= require(!inspector_name_->isEnabled() && inspector_name_->text().isEmpty() &&
                          inspector_status_->text() == "NO SELECTION · SELECT AN ENTITY" &&
                          !reset_position_button_->isEnabled() && !reset_rotation_button_->isEnabled() &&
                          std::all_of(position_fields_.begin(), position_fields_.end(),
                                      [](const QDoubleSpinBox* field) { return !field->isEnabled(); }) &&
                          std::all_of(rotation_fields_.begin(), rotation_fields_.end(),
                                      [](const QDoubleSpinBox* field) { return !field->isEnabled(); }),
                          "empty selection should clear the Inspector and disable entity properties");
        passed &= require(camera_inspector_.group->isHidden(),
                          "Camera properties should be hidden when no scene entity is selected");
        SelectHierarchyItem(hierarchy_tree_->topLevelItem(0)->child(0));
        passed &= require(save_scene_action_ && undo_action_ && redo_action_ && play_action_ &&
                          pause_action_ && stop_action_, "required actions should exist");
        auto* output_dock = findChild<QDockWidget*>("outputDock");
        passed &= require(output_dock && output_dock->toggleViewAction()->isEnabled(),
                          "Window menu should be able to show and hide editor docks");
        const float map_fit_distance = RhiViewport::DistanceForExtent({51.0f, 5.0f, 17.0f}, 55.0f, 1.8f);
        const float compact_fit_distance = RhiViewport::DistanceForExtent({5.0f, 3.0f, 4.0f}, 55.0f, 1.8f);
        passed &= require(map_fit_distance > compact_fit_distance && map_fit_distance < 60.0f,
                          "Fit All should frame map bounds with perspective instead of an oversized fixed multiplier");
        for (const char* component : {"CharacterMovement", "AudioListener", "AudioSource", "BoxCollider",
                                      "RigidBody", "AttributeSet", "SequenceComponent"})
            passed &= require(aether::FindComponentIdByName(component) != aether::kInvalidComponentId,
                              "runtime scene component schemas should be registered before loading or saving");

        const auto runtime_input = std::filesystem::temp_directory_path() / "aether_qt_runtime_components_in.ascene";
        const auto runtime_output = std::filesystem::temp_directory_path() / "aether_qt_runtime_components_out.ascene";
        std::error_code runtime_error;
        std::filesystem::remove(runtime_input, runtime_error);
        std::filesystem::remove(runtime_output, runtime_error);
        aether::World runtime_world;
        runtime_world.CreateEntity(aether::Transform{}, aether::CharacterMovement{}, aether::AudioListener{},
                                   aether::AudioSource{}, aether::RigidBody{}, aether::BoxCollider{},
                                   aether::gas::AttributeSet{}, aether::SequenceComponent{});
        passed &= require(aether::SaveSceneJson(runtime_world, runtime_input.string()),
                          "self-test runtime scene should save");
        aether::editor::SceneDocument runtime_document;
        passed &= require(runtime_document.Load(runtime_input) && runtime_document.Save(runtime_output),
                          "Qt scene document should load and resave runtime components");
        aether::World runtime_reloaded;
        passed &= require(aether::LoadSceneJson(runtime_reloaded, runtime_output.string()),
                          "resaved runtime scene should load");
        const auto runtime_entities = [&] {
            std::vector<aether::Entity> entities;
            runtime_reloaded.ForEachArchetype([&](const aether::Archetype& archetype) {
                for (aether::usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
                    const aether::Entity* values = archetype.EntityArray(chunk);
                    entities.insert(entities.end(), values, values + archetype.ChunkEntityCount(chunk));
                }
            });
            return entities;
        }();
        const aether::Entity runtime_entity = runtime_entities.empty() ? aether::kNullEntity : runtime_entities.front();
        passed &= require(!runtime_entity.IsNull() &&
                          runtime_reloaded.HasComponent<aether::CharacterMovement>(runtime_entity) &&
                          runtime_reloaded.HasComponent<aether::AudioListener>(runtime_entity) &&
                          runtime_reloaded.HasComponent<aether::AudioSource>(runtime_entity) &&
                          runtime_reloaded.HasComponent<aether::RigidBody>(runtime_entity) &&
                          runtime_reloaded.HasComponent<aether::BoxCollider>(runtime_entity) &&
                          runtime_reloaded.HasComponent<aether::gas::AttributeSet>(runtime_entity) &&
                          runtime_reloaded.HasComponent<aether::SequenceComponent>(runtime_entity),
                          "Qt load/save should preserve registered runtime component data");
        std::filesystem::remove(runtime_input, runtime_error);
        std::filesystem::remove(runtime_output, runtime_error);

        const aether::Entity test_entity = scene_document_.CreateEntity("Self Test Entity");
        passed &= require(!test_entity.IsNull() && scene_document_.Entities().size() == 6,
                          "entity creation should update the scene");
        const auto* test_id = scene_document_.GetWorld().GetComponent<aether::IdComponent>(test_entity);
        passed &= require(test_id != nullptr, "created entity should have a persistent GUID");
        const aether::EntityGuid test_guid = test_id ? test_id->guid : aether::EntityGuid{};
        if (test_id) selected_guid_ = test_guid;
        RefreshHierarchy();
        const auto* test_transform = scene_document_.GetWorld().GetComponent<aether::Transform>(test_entity);
        const aether::Quaternion rotation_before = test_transform ? test_transform->rotation : aether::Quaternion::Identity();
        rotation_fields_[0]->setValue(23.0);
        rotation_fields_[1]->setValue(90.0);
        rotation_fields_[2]->setValue(-11.0);
        ApplyInspectorRotation(true);
        test_transform = scene_document_.GetWorld().GetComponent<aether::Transform>(test_entity);
        const aether::Vec3 edited_rotation = test_transform
            ? RotationToEulerDegrees(test_transform->rotation) : aether::Vec3{};
        passed &= require(test_transform && std::abs(test_transform->rotation.Length() - 1.0f) < 0.001f &&
                          std::abs(edited_rotation.x - 23.0f) < 0.05f &&
                          std::abs(edited_rotation.y - 90.0f) < 0.05f &&
                          std::abs(edited_rotation.z + 11.0f) < 0.05f,
                          "Inspector rotation should apply pitch, yaw, and roll as a normalized quaternion");
        undo_action_->trigger();
        test_transform = scene_document_.GetWorld().GetComponent<aether::Transform>(test_entity);
        passed &= require(test_transform &&
                          std::abs(test_transform->rotation.x - rotation_before.x) < 0.001f &&
                          std::abs(test_transform->rotation.y - rotation_before.y) < 0.001f &&
                          std::abs(test_transform->rotation.z - rotation_before.z) < 0.001f &&
                          std::abs(test_transform->rotation.w - rotation_before.w) < 0.001f,
                          "Inspector rotation edit should be undoable through the editor action");
        position_fields_[0]->setValue(3.5);
        position_fields_[1]->setValue(2.0);
        position_fields_[2]->setValue(-4.0);
        ApplyInspectorPosition(true);
        reset_position_button_->click();
        test_transform = scene_document_.GetWorld().GetComponent<aether::Transform>(test_entity);
        passed &= require(test_transform && test_transform->position.Length() < 0.001f,
                          "Reset Position should set the selected entity to the origin");
        undo_action_->trigger();
        test_transform = scene_document_.GetWorld().GetComponent<aether::Transform>(test_entity);
        passed &= require(test_transform && std::abs(test_transform->position.x - 3.5f) < 0.001f &&
                          std::abs(test_transform->position.y - 2.0f) < 0.001f &&
                          std::abs(test_transform->position.z + 4.0f) < 0.001f,
                          "Reset Position should be undoable");
        undo_action_->trigger();
        test_transform = scene_document_.GetWorld().GetComponent<aether::Transform>(test_entity);
        passed &= require(test_transform && test_transform->position.Length() < 0.001f,
                          "undoing the position edit should restore the pre-test value");

        rotation_fields_[0]->setValue(23.0);
        rotation_fields_[1]->setValue(90.0);
        rotation_fields_[2]->setValue(-11.0);
        ApplyInspectorRotation(true);
        reset_rotation_button_->click();
        test_transform = scene_document_.GetWorld().GetComponent<aether::Transform>(test_entity);
        passed &= require(test_transform && std::abs(test_transform->rotation.x) < 0.001f &&
                          std::abs(test_transform->rotation.y) < 0.001f &&
                          std::abs(test_transform->rotation.z) < 0.001f &&
                          std::abs(test_transform->rotation.w - 1.0f) < 0.001f,
                          "Reset Rotation should restore the identity orientation");
        undo_action_->trigger();
        test_transform = scene_document_.GetWorld().GetComponent<aether::Transform>(test_entity);
        const aether::Vec3 restored_rotation = test_transform
            ? RotationToEulerDegrees(test_transform->rotation) : aether::Vec3{};
        passed &= require(test_transform && std::abs(restored_rotation.x - 23.0f) < 0.05f &&
                          std::abs(restored_rotation.y - 90.0f) < 0.05f &&
                          std::abs(restored_rotation.z + 11.0f) < 0.05f,
                          "Reset Rotation should be undoable");
        undo_action_->trigger();
        test_transform = scene_document_.GetWorld().GetComponent<aether::Transform>(test_entity);
        passed &= require(test_transform && std::abs(test_transform->rotation.Length() - 1.0f) < 0.001f &&
                          std::abs(test_transform->rotation.x) < 0.001f &&
                          std::abs(test_transform->rotation.y) < 0.001f &&
                          std::abs(test_transform->rotation.z) < 0.001f,
                          "undoing the rotation edit should restore the pre-test value");
        const aether::Entity test_camera = scene_document_.CreateEntity(
            "Self Test Camera", aether::editor::SceneEntityKind::Camera);
        const auto* test_camera_id = scene_document_.GetWorld().GetComponent<aether::IdComponent>(test_camera);
        const aether::EntityGuid test_camera_guid = test_camera_id ? test_camera_id->guid : aether::EntityGuid{};
        if (test_camera_id) selected_guid_ = test_camera_guid;
        RefreshHierarchy();
        auto* camera_data = scene_document_.GetWorld().GetComponent<aether::Camera>(test_camera);
        passed &= require(camera_data && !camera_inspector_.group->isHidden() &&
                          camera_inspector_.fov->isEnabled() && !camera_inspector_.ortho_height->isEnabled() &&
                          std::abs(camera_inspector_.fov->value() - 60.0) < 0.01,
                          "selecting a camera should show its perspective settings in the Inspector");
        const int orthographic_index = camera_inspector_.projection->findData(
            static_cast<int>(aether::Projection::Orthographic));
        camera_inspector_.projection->setCurrentIndex(orthographic_index);
        passed &= require(camera_data && camera_data->projection == aether::Projection::Orthographic &&
                          !camera_inspector_.fov->isEnabled() && camera_inspector_.ortho_height->isEnabled(),
                          "switching projection should update the camera and enable orthographic height");
        undo_action_->trigger();
        passed &= require(camera_data && camera_data->projection == aether::Projection::Perspective &&
                          camera_inspector_.fov->isEnabled(),
                          "camera projection changes should be undoable through the editor action");
        redo_action_->trigger();
        passed &= require(camera_data && camera_data->projection == aether::Projection::Orthographic,
                          "camera projection changes should be redoable through the editor action");
        undo_action_->trigger();
        camera_inspector_.fov->setValue(75.0);
        passed &= require(QMetaObject::invokeMethod(camera_inspector_.fov, "editingFinished", Qt::DirectConnection),
                          "camera field of view should expose the standard spin-box commit signal");
        passed &= require(camera_data && std::abs(camera_data->fov_degrees - 75.0f) < 0.01f,
                          "editing camera field of view should update the scene component");
        undo_action_->trigger();
        passed &= require(camera_data && std::abs(camera_data->fov_degrees - 60.0f) < 0.01f,
                          "camera field-of-view edits should be undoable");
        redo_action_->trigger();
        passed &= require(camera_data && std::abs(camera_data->fov_degrees - 75.0f) < 0.01f,
                          "camera field-of-view edits should be redoable");
        undo_action_->trigger();
        passed &= require(scene_document_.Undo(), "self-test camera entity should be removable from the scene history");
        const aether::Entity test_cine_camera = scene_document_.CreateEntity(
            "Self Test Cine Camera", aether::editor::SceneEntityKind::CineCamera);
        const auto* test_cine_id = scene_document_.GetWorld().GetComponent<aether::IdComponent>(test_cine_camera);
        if (test_cine_id) selected_guid_ = test_cine_id->guid;
        RefreshHierarchy();
        auto* cine_data = scene_document_.GetWorld().GetComponent<aether::CineCamera>(test_cine_camera);
        passed &= require(cine_data && scene_document_.GetWorld().HasComponent<aether::Camera>(test_cine_camera) &&
                          !camera_inspector_.cine_group->isHidden() &&
                          std::abs(camera_inspector_.focal_length->value() - 35.0) < 0.01,
                          "Cinematic Camera should create camera and lens components and expose lens settings");
        camera_inspector_.focal_length->setValue(50.0);
        passed &= require(QMetaObject::invokeMethod(camera_inspector_.focal_length, "editingFinished", Qt::DirectConnection) &&
                          cine_data && std::abs(cine_data->focal_length_mm - 50.0f) < 0.01f,
                          "editing cinematic focal length should update the scene component");
        undo_action_->trigger();
        passed &= require(cine_data && std::abs(cine_data->focal_length_mm - 35.0f) < 0.01f,
                          "cinematic lens edits should be undoable");
        redo_action_->trigger();
        passed &= require(cine_data && std::abs(cine_data->focal_length_mm - 50.0f) < 0.01f,
                          "cinematic lens edits should be redoable");
        undo_action_->trigger();
        passed &= require(cine_data && std::abs(cine_data->focal_length_mm - 35.0f) < 0.01f,
                          "cinematic lens edit should be undone before removing its test entity");
        passed &= require(scene_document_.Undo(), "self-test cinematic camera should be removable from scene history");
        selected_guid_ = test_guid;
        RefreshHierarchy();
        const aether::Entity test_child = scene_document_.CreateEntity(
            "Self Test Child", aether::editor::SceneEntityKind::Empty, {}, test_entity);
        const auto* child_id = scene_document_.GetWorld().GetComponent<aether::IdComponent>(test_child);
        if (child_id) selected_guid_ = child_id->guid;
        RefreshHierarchy();
        passed &= require(scene_document_.ParentOf(test_child) == test_entity,
                          "child creation should store a real scene parent");
        passed &= require(hierarchy_tree_->currentItem() && hierarchy_tree_->currentItem()->parent() &&
                          hierarchy_tree_->currentItem()->parent()->data(0, Qt::UserRole).toString().toStdString() ==
                              aether::ToString(test_guid),
                          "hierarchy should display scene parent/child relationships");
        passed &= require(scene_document_.Undo(), "child creation should be undoable");
        passed &= require(scene_document_.Entities().size() == 6, "undo should remove the child entity");
        selected_guid_ = test_guid;
        RefreshHierarchy();
        duplicate_entity_button_->click();
        passed &= require(scene_document_.Entities().size() == 7,
                          "Duplicate button should create a real scene entity");
        passed &= require(scene_document_.Name(selected_entity_) == "Self Test Entity Copy",
                          "duplicated entity should be selected and named clearly");
        undo_action_->trigger();
        passed &= require(scene_document_.Entities().size() == 6, "Undo should remove the duplicate");
        redo_action_->trigger();
        passed &= require(scene_document_.Entities().size() == 7, "Redo should restore the duplicate");
        undo_action_->trigger();

        if (!test_guid.IsNull()) selected_guid_ = test_guid;
        RefreshHierarchy();
        sequence_add_track_button_->click();
        passed &= require(sequence_document_.Sequence().tracks.size() == 1,
                          "Transform Track button should create a bound sequence track");
        if (auto* transform = scene_document_.GetWorld().GetComponent<aether::Transform>(selected_entity_))
            transform->position.x = 0.0f;
        sequence_time_->setValue(0.0);
        sequence_set_key_button_->click();
        sequence_time_->setValue(1.0);
        if (auto* transform = scene_document_.GetWorld().GetComponent<aether::Transform>(selected_entity_))
            transform->position.x = 10.0f;
        sequence_set_key_button_->click();
        passed &= require(sequence_key_list_->count() == 2,
                          "position key control should create editable keyframes");
        sequence_time_->setValue(0.5);
        const auto* animated = scene_document_.GetWorld().GetComponent<aether::Transform>(selected_entity_);
        passed &= require(animated && std::abs(animated->position.x - 5.0f) < 0.01f,
                          "sequencer scrub should evaluate the real scene transform");
        sequence_stop_button_->click();
        animated = scene_document_.GetWorld().GetComponent<aether::Transform>(selected_entity_);
        passed &= require(animated && std::abs(animated->position.x) < 0.01f,
                          "Restore should return the scene to its pre-preview transform");

        UpdateSceneStatus();
        undo_action_->trigger();
        passed &= require(scene_document_.Entities().size() == 5, "Undo action should undo entity creation");
        redo_action_->trigger();
        passed &= require(scene_document_.Entities().size() == 6, "Redo action should restore entity creation");

        play_action_->trigger();
        passed &= require(scene_document_.PlayState() == aether::editor::PlaySession::State::Playing,
                          "Play action should start a real play session");
        passed &= require(!save_scene_action_->isEnabled(), "Save should be disabled during Play");
        scene_document_.CreateEntity("Temporary Play Entity");
        passed &= require(scene_document_.Entities().size() == 7, "play-time changes should apply to the play world");
        pause_action_->trigger();
        passed &= require(scene_document_.PlayState() == aether::editor::PlaySession::State::Paused,
                          "Pause action should pause the play session");
        play_action_->trigger();
        passed &= require(scene_document_.PlayState() == aether::editor::PlaySession::State::Playing,
                          "Play action should resume a paused session");
        stop_action_->trigger();
        passed &= require(scene_document_.PlayState() == aether::editor::PlaySession::State::Editing,
                          "Stop action should return to editing");
        passed &= require(scene_document_.Entities().size() == 6,
                          "Stop should discard temporary play entities and restore the edited scene");
        passed &= require(save_scene_action_->isEnabled(), "Save should be re-enabled after Stop");

        const auto self_test_root = std::filesystem::temp_directory_path() / "aether_qt_editor_self_test";
        std::error_code filesystem_error;
        std::filesystem::remove_all(self_test_root, filesystem_error);
        aether::ProjectPaths self_test_paths;
        std::string project_error;
        passed &= require(aether::CreateProject(self_test_root, "FunctionalProject", &self_test_paths, &project_error),
                          "project creation should succeed");
        if (std::filesystem::exists(self_test_paths.file)) {
            OpenProject(self_test_paths.file, false);
            passed &= require(save_project_action_->isEnabled() && !asset_view_->isHidden(),
                              "opening a project should enable project settings and Content Browser");
            const auto content_root = self_test_paths.content;
            const auto scene_folder = content_root / "Scenes";
            std::filesystem::create_directories(scene_folder, filesystem_error);
            SetContentDirectory(QString::fromStdWString(scene_folder.wstring()));
            passed &= require(content_breadcrumb_->count() == 2 && content_breadcrumb_->currentIndex() == 1 &&
                              parent_folder_button_->isEnabled(),
                              "Content Browser breadcrumb should show the current folder and expose its parent");
            content_breadcrumb_->setCurrentIndex(0);
            passed &= require(current_content_path_ == QDir::cleanPath(QString::fromStdWString(content_root.wstring())) &&
                              !parent_folder_button_->isEnabled(),
                              "choosing the Content breadcrumb should return to the project asset root");
            aether::ProjectSettings candidate = project_settings_;
            candidate.window_width = 1600;
            candidate.window_height = 900;
            passed &= require(PersistProjectSettings(candidate, &project_error),
                              "project settings should persist through the Qt adapter");
            aether::ProjectSettings reloaded;
            passed &= require(aether::LoadProject(self_test_paths.file, reloaded, &project_error) &&
                              reloaded.window_width == 1600 && reloaded.window_height == 900,
                              "saved project settings should round-trip from disk");
            passed &= require(SaveSequence(), "Qt sequencer should save an .asequence inside the project");
            aether::editor::SequenceDocument reloaded_sequence;
            passed &= require(reloaded_sequence.Load(sequence_file_, &project_error) &&
                              reloaded_sequence.Sequence().tracks.size() == 1,
                              "saved Qt animation sequence should round-trip from disk");

            const std::filesystem::path source_model(AETHER_QT_TEST_MODEL_PATH);
            const auto content_model = self_test_paths.content / source_model.filename();
            std::error_code copy_error;
            passed &= require(std::filesystem::copy_file(source_model, content_model,
                                std::filesystem::copy_options::overwrite_existing, copy_error) && !copy_error,
                              "self-test model fixture should copy into the project Content folder");
            const auto source_buffer = source_model.parent_path() / "test_cube.bin";
            const auto content_buffer = self_test_paths.content / source_buffer.filename();
            copy_error.clear();
            passed &= require(std::filesystem::copy_file(source_buffer, content_buffer,
                                std::filesystem::copy_options::overwrite_existing, copy_error) && !copy_error,
                              "self-test model buffer should copy alongside the glTF asset");
            const QModelIndex model_index = asset_model_->index(QString::fromStdWString(content_model.wstring()));
            asset_view_->setCurrentIndex(model_index);
            passed &= require(model_index.isValid() && place_model_button_->isEnabled(),
                              "selecting a glTF asset should enable the explicit Place Model action");
            place_model_button_->click();
            const auto* placed_model = scene_document_.GetWorld().GetComponent<aether::ModelRenderer>(selected_entity_);
            passed &= require(placed_model && std::string(placed_model->asset_path) == source_model.filename().string(),
                              "Place Model should create a scene entity referencing the selected project asset");
        }
        std::filesystem::remove_all(self_test_root, filesystem_error);
        if (passed) qInfo() << "Qt editor self-test passed";
        return passed;
    }

private:
    QWidget* MakeSequenceEditor() {
        aether::seq::LevelSequence initial;
        initial.name = "New Sequence";
        initial.fps = 30.0f;
        initial.duration = 5.0f;
        sequence_document_ = aether::editor::SequenceDocument(std::move(initial));

        auto* body = new QWidget;
        auto* layout = new QVBoxLayout(body);
        layout->setContentsMargins(10, 7, 10, 8);
        layout->setSpacing(5);
        sequence_name_ = new QLineEdit(body);
        sequence_fps_ = new QDoubleSpinBox(body);
        sequence_duration_ = new QDoubleSpinBox(body);

        auto* file_row = new QHBoxLayout;
        file_row->setSpacing(6);
        sequence_new_button_ = new QPushButton("New", body);
        sequence_open_button_ = new QPushButton("Open…", body);
        sequence_save_button_ = new QPushButton("Save", body);
        sequence_new_button_->setToolTip("Start a new animation sequence");
        sequence_open_button_->setToolTip("Open an animation sequence asset");
        sequence_save_button_->setToolTip("Save the current animation sequence");
        sequence_name_->setAccessibleName("Sequence name");
        sequence_name_->setPlaceholderText("Sequence name");
        sequence_fps_->setAccessibleName("Sequence frame rate");
        sequence_fps_->setPrefix("FPS ");
        sequence_fps_->setToolTip("Playback frame rate");
        sequence_fps_->setFixedWidth(112);
        sequence_duration_->setAccessibleName("Sequence duration");
        sequence_duration_->setPrefix("Length ");
        sequence_duration_->setToolTip("Sequence duration in seconds");
        sequence_duration_->setFixedWidth(142);
        file_row->addWidget(sequence_new_button_);
        file_row->addWidget(sequence_open_button_);
        file_row->addWidget(sequence_save_button_);
        file_row->addWidget(sequence_name_, 1);
        file_row->addWidget(sequence_fps_);
        file_row->addWidget(sequence_duration_);
        layout->addLayout(file_row);
        sequence_fps_->setRange(1.0, 240.0);
        sequence_fps_->setDecimals(2);
        sequence_duration_->setRange(0.01, 3600.0);
        sequence_duration_->setDecimals(3);
        sequence_duration_->setSuffix(" s");

        sequence_target_ = new QLabel("Target: select an entity in the Hierarchy", body);
        sequence_target_->setObjectName("muted");
        layout->addWidget(sequence_target_);

        auto* lists = new QHBoxLayout;
        lists->setSpacing(12);
        auto* tracks_panel = new QWidget(body);
        auto* tracks_layout = new QVBoxLayout(tracks_panel);
        tracks_layout->setContentsMargins(0, 0, 0, 0);
        tracks_layout->setSpacing(4);
        auto* track_actions = new QHBoxLayout;
        track_actions->setContentsMargins(0, 0, 0, 0);
        track_actions->setSpacing(6);
        auto* tracks_title = new QLabel("TRACKS", tracks_panel);
        tracks_title->setObjectName("section");
        sequence_add_track_button_ = new QPushButton("+ Transform Track", body);
        sequence_remove_track_button_ = new QPushButton("Remove", body);
        sequence_add_track_button_->setToolTip("Add a position track for the selected entity");
        sequence_remove_track_button_->setToolTip("Remove the selected sequence track");
        sequence_add_track_button_->setAccessibleName("Add transform track");
        sequence_add_track_button_->setEnabled(false);
        sequence_remove_track_button_->setEnabled(false);
        track_actions->addWidget(tracks_title);
        track_actions->addStretch();
        track_actions->addWidget(sequence_add_track_button_);
        track_actions->addWidget(sequence_remove_track_button_);
        tracks_layout->addLayout(track_actions);
        sequence_track_list_ = new QListWidget(body);
        sequence_track_list_->setObjectName("sequenceTracks");
        sequence_track_list_->setAccessibleName("Animation tracks");
        sequence_track_list_->setMinimumHeight(48);
        sequence_track_list_->setToolTip("Choose a track to inspect and edit its keys");
        tracks_layout->addWidget(sequence_track_list_, 1);
        lists->addWidget(tracks_panel, 1);

        auto* keys_panel = new QWidget(body);
        auto* keys_layout = new QVBoxLayout(keys_panel);
        keys_layout->setContentsMargins(0, 0, 0, 0);
        keys_layout->setSpacing(4);
        auto* key_actions = new QHBoxLayout;
        key_actions->setContentsMargins(0, 0, 0, 0);
        key_actions->setSpacing(6);
        auto* keys_title = new QLabel("POSITION KEYS", keys_panel);
        keys_title->setObjectName("section");
        sequence_set_key_button_ = new QPushButton("Set / Update", body);
        sequence_remove_key_button_ = new QPushButton("Clear", body);
        sequence_set_key_button_->setToolTip("Create or update a position key at the current time");
        sequence_remove_key_button_->setToolTip("Remove the selected position key");
        sequence_set_key_button_->setEnabled(false);
        sequence_remove_key_button_->setEnabled(false);
        key_actions->addWidget(keys_title);
        key_actions->addStretch();
        key_actions->addWidget(sequence_set_key_button_);
        key_actions->addWidget(sequence_remove_key_button_);
        keys_layout->addLayout(key_actions);
        sequence_key_list_ = new QListWidget(body);
        sequence_key_list_->setObjectName("sequenceKeys");
        sequence_key_list_->setAccessibleName("Position keys");
        sequence_key_list_->setMinimumHeight(48);
        sequence_key_list_->setToolTip("Select a key to move the playhead to its time");
        keys_layout->addWidget(sequence_key_list_, 1);
        lists->addWidget(keys_panel, 1);
        layout->addLayout(lists, 1);

        auto* time_row = new QHBoxLayout;
        time_row->setSpacing(8);
        auto* time_label = new QLabel("TIME", body);
        time_label->setObjectName("section");
        time_row->addWidget(time_label);
        sequence_time_slider_ = new QSlider(Qt::Horizontal, body);
        sequence_time_slider_->setRange(0, 5000);
        sequence_time_slider_->setAccessibleName("Sequence playhead");
        sequence_time_slider_->setToolTip("Scrub the sequence playhead");
        sequence_time_ = new QDoubleSpinBox(body);
        sequence_time_->setRange(0.0, 5.0);
        sequence_time_->setDecimals(3);
        sequence_time_->setKeyboardTracking(false);
        sequence_time_->setSuffix(" s");
        sequence_time_->setFixedWidth(112);
        sequence_time_->setAccessibleName("Playhead time");
        time_row->addWidget(sequence_time_slider_, 1);
        time_row->addWidget(sequence_time_);
        layout->addLayout(time_row);

        auto* transport = new QHBoxLayout;
        transport->setSpacing(6);
        sequence_play_button_ = new QPushButton("▶ Preview", body);
        sequence_pause_button_ = new QPushButton("Ⅱ Pause", body);
        sequence_stop_button_ = new QPushButton("■ Stop", body);
        sequence_play_button_->setToolTip("Preview the animation in the active World");
        sequence_pause_button_->setToolTip("Pause the sequence preview");
        sequence_stop_button_->setToolTip("Stop preview and restore the edited World");
        sequence_play_button_->setObjectName("sequencePreviewButton");
        sequence_pause_button_->setEnabled(false);
        sequence_stop_button_->setEnabled(false);
        sequence_loop_ = new QCheckBox("Loop", body);
        sequence_loop_->setToolTip("Restart preview when it reaches the end");
        transport->addWidget(sequence_play_button_);
        transport->addWidget(sequence_pause_button_);
        transport->addWidget(sequence_stop_button_);
        transport->addWidget(sequence_loop_);
        transport->addStretch();
        layout->addLayout(transport);
        sequence_diagnostics_ = new QLabel(body);
        sequence_diagnostics_->setWordWrap(true);
        sequence_diagnostics_->setObjectName("muted");
        sequence_diagnostics_->setMinimumHeight(18);
        layout->addWidget(sequence_diagnostics_);

        QObject::connect(sequence_new_button_, &QPushButton::clicked, this, [this] { NewSequence(); });
        QObject::connect(sequence_open_button_, &QPushButton::clicked, this, [this] { OpenSequenceDialog(); });
        QObject::connect(sequence_save_button_, &QPushButton::clicked, this, [this] { SaveSequence(); });
        QObject::connect(sequence_name_, &QLineEdit::editingFinished, this, [this] {
            const std::string name = sequence_name_->text().trimmed().toStdString();
            if (!name.empty() && name != sequence_document_.Sequence().name) {
                sequence_document_.SetName(name);
                RefreshSequenceEditor();
            }
        });
        QObject::connect(sequence_fps_, &QDoubleSpinBox::editingFinished, this, [this] {
            const auto fps = static_cast<aether::f32>(sequence_fps_->value());
            if (std::abs(fps - sequence_document_.Sequence().fps) <= 0.001f) return;
            sequence_document_.SetFps(fps);
            RefreshSequenceEditor();
        });
        QObject::connect(sequence_duration_, &QDoubleSpinBox::editingFinished, this, [this] {
            const auto duration = static_cast<aether::f32>(sequence_duration_->value());
            if (std::abs(duration - sequence_document_.Sequence().duration) <= 0.001f) return;
            sequence_document_.SetDuration(duration);
            RefreshSequenceEditor();
            ScrubSequence(std::min(static_cast<aether::f32>(sequence_time_->value()),
                                   sequence_document_.Sequence().EffectiveDuration()));
        });
        QObject::connect(sequence_add_track_button_, &QPushButton::clicked, this, [this] { AddSelectedTransformTrack(); });
        QObject::connect(sequence_remove_track_button_, &QPushButton::clicked, this, [this] { RemoveSelectedSequenceTrack(); });
        QObject::connect(sequence_set_key_button_, &QPushButton::clicked, this, [this] { SetPositionKey(); });
        QObject::connect(sequence_remove_key_button_, &QPushButton::clicked, this, [this] { RemoveSelectedPositionKey(); });
        QObject::connect(sequence_track_list_, &QListWidget::currentRowChanged, this, [this](int) { RefreshSequenceKeys(); });
        QObject::connect(sequence_key_list_, &QListWidget::currentItemChanged, this,
                         [this](QListWidgetItem* item, QListWidgetItem*) {
            sequence_remove_key_button_->setEnabled(item != nullptr);
            if (item) ScrubSequence(static_cast<aether::f32>(item->data(Qt::UserRole).toDouble()));
        });
        QObject::connect(sequence_time_slider_, &QSlider::valueChanged, this, [this](int milliseconds) {
            ScrubSequence(static_cast<aether::f32>(milliseconds) / 1000.0f);
        });
        QObject::connect(sequence_time_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                         [this](double seconds) { ScrubSequence(static_cast<aether::f32>(seconds)); });
        QObject::connect(sequence_play_button_, &QPushButton::clicked, this, [this] { PlaySequencePreview(); });
        QObject::connect(sequence_pause_button_, &QPushButton::clicked, this, [this] { PauseSequencePreview(); });
        QObject::connect(sequence_stop_button_, &QPushButton::clicked, this, [this] { StopSequencePreview(); });
        QObject::connect(sequence_loop_, &QCheckBox::toggled, this, [this](bool loop) {
            if (sequence_player_) sequence_player_->loop = loop;
        });
        sequence_timer_.setInterval(16);
        QObject::connect(&sequence_timer_, &QTimer::timeout, this, [this] { TickSequencePreview(); });
        play_simulation_timer_.setInterval(16);
        QObject::connect(&play_simulation_timer_, &QTimer::timeout, this, [this] { TickPlaySimulation(); });
        RefreshSequenceEditor();
        return body;
    }

    bool ConfirmDiscardSequenceChanges() {
        if (!sequence_document_.dirty()) return true;
        QMessageBox prompt(QMessageBox::Warning, "Unsaved Animation Changes",
                           "Save the animation sequence before continuing?",
                           QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
        const auto choice = static_cast<QMessageBox::StandardButton>(prompt.exec());
        if (choice == QMessageBox::Save) return SaveSequence();
        return choice == QMessageBox::Discard;
    }

    void NewSequence() {
        if (!ConfirmDiscardSequenceChanges()) return;
        StopSequencePreview();
        aether::seq::LevelSequence sequence;
        sequence.name = "New Sequence";
        sequence.fps = 30.0f;
        sequence.duration = 5.0f;
        sequence_document_ = aether::editor::SequenceDocument(std::move(sequence));
        sequence_file_.clear();
        RefreshSequenceEditor();
    }

    void OpenSequenceDialog() {
        if (!ConfirmDiscardSequenceChanges()) return;
        const QString root = project_file_.empty() ? QDir::homePath() : QString::fromStdWString(
            aether::ProjectPaths::ForFile(project_file_).content.wstring());
        const QString file = QFileDialog::getOpenFileName(this, "Open Animation Sequence", root,
                                                          "Aether Sequences (*.asequence)");
        if (!file.isEmpty()) OpenSequence(std::filesystem::path(file.toStdWString()), false);
    }

    bool OpenSequence(const std::filesystem::path& file, bool confirm_changes = true) {
        if (confirm_changes && !ConfirmDiscardSequenceChanges()) return false;
        StopSequencePreview();
        std::string error;
        if (!sequence_document_.Load(file, &error)) {
            QMessageBox::critical(this, "Couldn't open animation sequence", QString::fromStdString(error));
            return false;
        }
        sequence_file_ = file;
        RefreshSequenceEditor();
        if (auto* dock = findChild<QDockWidget*>("sequenceDock")) dock->show(), dock->raise();
        return true;
    }

    bool SaveSequence() {
        std::filesystem::path file = sequence_file_;
        if (file.empty() && !project_file_.empty()) {
            std::string stem = sequence_document_.Sequence().name.empty()
                ? "NewSequence" : sequence_document_.Sequence().name;
            std::replace_if(stem.begin(), stem.end(), [](char c) {
                return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-');
            }, '_');
            file = aether::ProjectPaths::ForFile(project_file_).content / "Sequences" / (stem + ".asequence");
        }
        if (file.empty()) {
            const QString selected = QFileDialog::getSaveFileName(this, "Save Animation Sequence",
                QDir::homePath() + "/NewSequence.asequence", "Aether Sequences (*.asequence)");
            if (selected.isEmpty()) return false;
            file = std::filesystem::path(selected.toStdWString());
        }
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::string error;
        if (ec || !sequence_document_.Save(file, &error)) {
            QMessageBox::critical(this, "Couldn't save animation sequence",
                                  QString::fromStdString(ec ? ec.message() : error));
            return false;
        }
        sequence_file_ = file;
        RefreshSequenceEditor();
        statusBar()->showMessage(QString("●  ANIMATION SAVED     %1")
                                     .arg(QString::fromStdWString(file.filename().wstring())));
        return true;
    }

    void RefreshSequenceEditor() {
        if (!sequence_name_) return;
        const auto& sequence = sequence_document_.Sequence();
        {
            const QSignalBlocker name_blocker(sequence_name_);
            const QSignalBlocker fps_blocker(sequence_fps_);
            const QSignalBlocker duration_blocker(sequence_duration_);
            sequence_name_->setText(QString::fromStdString(sequence.name));
            sequence_fps_->setValue(sequence.fps);
            sequence_duration_->setValue(sequence.duration > 0.0f ? sequence.duration : sequence.EffectiveDuration());
        }
        const int previous = sequence_track_list_->currentRow();
        {
            const QSignalBlocker blocker(sequence_track_list_);
            sequence_track_list_->clear();
            for (const auto& track : sequence.tracks) {
                std::size_t key_count = track.rotation.size();
                for (const auto& channel : track.channels) key_count += channel.keys.size();
                QString type = "Other";
                if (track.type == aether::seq::TrackType::Transform) type = "Transform";
                else if (track.type == aether::seq::TrackType::Animation) type = "Animation";
                else if (track.type == aether::seq::TrackType::Event) type = "Event";
                sequence_track_list_->addItem(QString("%1  ·  %2  ·  %3 keys")
                    .arg(QString::fromStdString(track.name), type).arg(key_count));
            }
            if (!sequence.tracks.empty()) sequence_track_list_->setCurrentRow(
                std::clamp(previous, 0, static_cast<int>(sequence.tracks.size()) - 1));
        }
        const aether::f32 duration = std::max(sequence.EffectiveDuration(), 0.01f);
        sequence_time_slider_->setRange(0, static_cast<int>(std::ceil(duration * 1000.0f)));
        sequence_time_->setRange(0.0, duration);
        sequence_remove_track_button_->setEnabled(sequence_track_list_->currentRow() >= 0);
        sequence_save_button_->setText(sequence_document_.dirty() ? "Save *" : "Save");
        const auto problems = sequence_document_.Problems();
        if (problems.empty()) sequence_diagnostics_->setText("Sequence data is valid.");
        else {
            QStringList messages;
            for (const auto& problem : problems) messages << QString::fromStdString(problem);
            sequence_diagnostics_->setText(messages.join('\n'));
        }
        RefreshSequenceKeys();
        RebuildSequencePlayerIfActive();
    }

    void RefreshSequenceTarget() {
        if (!sequence_target_) return;
        const bool valid = !selected_entity_.IsNull() &&
            scene_document_.GetWorld().IsAlive(selected_entity_);
        sequence_target_->setText(valid
            ? "Target: " + QString::fromStdString(scene_document_.Name(selected_entity_))
            : "Target: select an entity in the Hierarchy");
        sequence_add_track_button_->setEnabled(valid &&
            scene_document_.PlayState() == aether::editor::PlaySession::State::Editing);
        duplicate_entity_button_->setEnabled(valid);
    }

    void RefreshSequenceKeys() {
        if (!sequence_key_list_) return;
        const int row = sequence_track_list_->currentRow();
        sequence_key_list_->clear();
        sequence_set_key_button_->setEnabled(false);
        sequence_remove_key_button_->setEnabled(false);
        if (row < 0 || static_cast<std::size_t>(row) >= sequence_document_.Sequence().tracks.size()) return;
        const auto& track = sequence_document_.Sequence().tracks[static_cast<std::size_t>(row)];
        sequence_set_key_button_->setEnabled(track.type == aether::seq::TrackType::Transform && !track.locked);
        if (track.type != aether::seq::TrackType::Transform) return;
        std::set<aether::f32> times;
        for (const auto& channel : track.channels)
            for (const auto& key : channel.keys) times.insert(key.time);
        for (aether::f32 time : times) {
            auto* item = new QListWidgetItem(QString("%1 s  ·  Position").arg(time, 0, 'f', 3), sequence_key_list_);
            item->setData(Qt::UserRole, static_cast<double>(time));
        }
    }

    void AddSelectedTransformTrack() {
        if (selected_entity_.IsNull()) return;
        const auto* id = scene_document_.GetWorld().GetComponent<aether::IdComponent>(selected_entity_);
        if (!id) return;
        const std::string label = scene_document_.Name(selected_entity_) + " Transform";
        const auto row = sequence_document_.AddTrack(aether::seq::TrackType::Transform, label, id->guid);
        RefreshSequenceEditor();
        sequence_track_list_->setCurrentRow(static_cast<int>(row));
    }

    void RemoveSelectedSequenceTrack() {
        const int row = sequence_track_list_->currentRow();
        if (row < 0 || !sequence_document_.RemoveTrack(static_cast<std::size_t>(row))) return;
        RefreshSequenceEditor();
    }

    void SetPositionKey() {
        const int row = sequence_track_list_->currentRow();
        if (row < 0 || static_cast<std::size_t>(row) >= sequence_document_.Sequence().tracks.size()) return;
        const auto& track = sequence_document_.Sequence().tracks[static_cast<std::size_t>(row)];
        if (track.type != aether::seq::TrackType::Transform) return;
        const aether::Entity entity = scene_document_.Guids().Find(scene_document_.GetWorld(), track.binding);
        const auto* transform = scene_document_.GetWorld().GetComponent<aether::Transform>(entity);
        if (!transform) return;
        const aether::f32 time = static_cast<aether::f32>(sequence_time_->value());
        sequence_document_.BeginEdit();
        sequence_document_.AddKey(static_cast<std::size_t>(row), 0, time, transform->position.x);
        sequence_document_.AddKey(static_cast<std::size_t>(row), 1, time, transform->position.y);
        sequence_document_.AddKey(static_cast<std::size_t>(row), 2, time, transform->position.z);
        sequence_document_.EndEdit();
        RefreshSequenceEditor();
        ScrubSequence(time);
    }

    void RemoveSelectedPositionKey() {
        const int track_row = sequence_track_list_->currentRow();
        auto* selected = sequence_key_list_->currentItem();
        if (track_row < 0 || !selected) return;
        const aether::f32 time = static_cast<aether::f32>(selected->data(Qt::UserRole).toDouble());
        const auto& sequence = sequence_document_.Sequence();
        if (static_cast<std::size_t>(track_row) >= sequence.tracks.size()) return;
        sequence_document_.BeginEdit();
        for (std::size_t channel = 0; channel < sequence.tracks[static_cast<std::size_t>(track_row)].channels.size(); ++channel) {
            const auto& keys = sequence_document_.Sequence().tracks[static_cast<std::size_t>(track_row)].channels[channel].keys;
            for (std::size_t index = keys.size(); index-- > 0;) {
                if (std::abs(keys[index].time - time) <= 0.001f)
                    sequence_document_.RemoveKey({static_cast<std::size_t>(track_row),
                        aether::editor::KeyRef::Lane::Channel, channel, index});
            }
        }
        sequence_document_.EndEdit();
        RefreshSequenceEditor();
    }

    void CaptureSequencePreviewBase() {
        if (sequence_preview_active_) return;
        sequence_preview_transforms_.clear();
        for (aether::Entity entity : scene_document_.Entities()) {
            const auto* id = scene_document_.GetWorld().GetComponent<aether::IdComponent>(entity);
            const auto* transform = scene_document_.GetWorld().GetComponent<aether::Transform>(entity);
            if (id && transform) sequence_preview_transforms_[id->guid] = *transform;
        }
        sequence_preview_active_ = true;
    }

    void EnsureSequencePlayer() {
        CaptureSequencePreviewBase();
        if (sequence_player_ && sequence_player_revision_ == sequence_document_.Revision()) return;
        const aether::f32 time = sequence_player_ ? sequence_player_->Time()
                                                  : static_cast<aether::f32>(sequence_time_->value());
        sequence_player_ = std::make_unique<aether::seq::SequencePlayer>(
            sequence_document_.Sequence(), scene_document_.GetWorld(), scene_document_.Guids());
        sequence_player_->Bind();
        sequence_player_->loop = sequence_loop_->isChecked();
        sequence_player_->SetTime(time);
        sequence_player_revision_ = sequence_document_.Revision();
    }

    void RebuildSequencePlayerIfActive() {
        if (!sequence_preview_active_) return;
        sequence_player_.reset();
        sequence_player_revision_ = 0;
        EnsureSequencePlayer();
        sequence_player_->Evaluate();
        SyncInspectorPosition();
    }

    void ScrubSequence(aether::f32 time) {
        if (!sequence_time_) return;
        EnsureSequencePlayer();
        sequence_stop_button_->setEnabled(true);
        const aether::f32 clamped = std::clamp(time, 0.0f, sequence_player_->Duration());
        sequence_player_->SetTime(clamped);
        sequence_player_->Evaluate();
        {
            const QSignalBlocker slider_blocker(sequence_time_slider_);
            const QSignalBlocker time_blocker(sequence_time_);
            sequence_time_slider_->setValue(static_cast<int>(std::round(clamped * 1000.0f)));
            sequence_time_->setValue(clamped);
        }
        SyncInspectorPosition();
    }

    void PlaySequencePreview() {
        EnsureSequencePlayer();
        sequence_player_->loop = sequence_loop_->isChecked();
        sequence_player_->Play();
        sequence_clock_.restart();
        sequence_timer_.start();
        sequence_play_button_->setEnabled(false);
        sequence_pause_button_->setEnabled(true);
        sequence_stop_button_->setEnabled(true);
    }

    void PauseSequencePreview() {
        sequence_timer_.stop();
        if (sequence_player_) sequence_player_->Pause();
        sequence_play_button_->setEnabled(true);
        sequence_pause_button_->setEnabled(false);
    }

    void StopSequencePreview() {
        sequence_timer_.stop();
        if (sequence_player_) sequence_player_->Pause();
        for (const auto& [guid, transform] : sequence_preview_transforms_) {
            const aether::Entity entity = scene_document_.Guids().Find(scene_document_.GetWorld(), guid);
            if (auto* current = scene_document_.GetWorld().GetComponent<aether::Transform>(entity)) *current = transform;
        }
        sequence_player_.reset();
        sequence_player_revision_ = 0;
        sequence_preview_transforms_.clear();
        sequence_preview_active_ = false;
        if (sequence_time_) {
            const QSignalBlocker slider_blocker(sequence_time_slider_);
            const QSignalBlocker time_blocker(sequence_time_);
            sequence_time_slider_->setValue(0);
            sequence_time_->setValue(0.0);
            sequence_play_button_->setEnabled(true);
            sequence_pause_button_->setEnabled(false);
            sequence_stop_button_->setEnabled(false);
        }
        SyncInspectorPosition();
    }

    void TickSequencePreview() {
        if (!sequence_player_) return;
        const qint64 milliseconds = sequence_clock_.restart();
        sequence_player_->Update(static_cast<aether::f32>(std::max<qint64>(milliseconds, 1)) / 1000.0f);
        const aether::f32 time = sequence_player_->Time();
        {
            const QSignalBlocker slider_blocker(sequence_time_slider_);
            const QSignalBlocker time_blocker(sequence_time_);
            sequence_time_slider_->setValue(static_cast<int>(std::round(time * 1000.0f)));
            sequence_time_->setValue(time);
        }
        SyncInspectorPosition();
        if (!sequence_player_->Playing()) PauseSequencePreview();
    }

    bool ConfirmDiscardSceneChanges() {
        if (!scene_document_.IsDirty()) return true;
        QMessageBox prompt(QMessageBox::Warning, "Unsaved Scene Changes",
                           "Save the scene changes before continuing?",
                           QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
        const auto choice = static_cast<QMessageBox::StandardButton>(prompt.exec());
        if (choice == QMessageBox::Save) return SaveScene();
        return choice == QMessageBox::Discard;
    }

    void RefreshHierarchy(bool preserveSelection = false) {
        QTreeWidgetItem* selected = nullptr;
        const QString filter = hierarchy_filter_ ? hierarchy_filter_->text().trimmed() : QString();
        const auto& scene_path = scene_document_.FilePath();
        const QString scene_label = scene_path.empty()
            ? QStringLiteral("Untitled Scene")
            : QString::fromStdWString(scene_path.stem().wstring());
        {
            const QSignalBlocker blocker(hierarchy_tree_);
            hierarchy_tree_->clear();
            auto* root = new QTreeWidgetItem(hierarchy_tree_,
                {QString("%1  ·  %2 entities").arg(scene_label).arg(scene_document_.Entities().size())});
            root->setFlags(root->flags() & ~Qt::ItemIsSelectable);
            std::unordered_map<std::string, QTreeWidgetItem*> items;
            std::unordered_map<std::string, aether::Entity> item_entities;
            std::vector<std::pair<std::string, QTreeWidgetItem*>> ordered_items;
            for (aether::Entity entity : scene_document_.Entities()) {
                const auto* id = scene_document_.GetWorld().GetComponent<aether::IdComponent>(entity);
                if (!id) continue;
                const QString name = QString::fromStdString(scene_document_.Name(entity));
                const std::string guid = aether::ToString(id->guid);
                auto* item = new QTreeWidgetItem({"◇  " + name});
                item->setData(0, Qt::UserRole, QString::fromStdString(guid));
                items[guid] = item;
                item_entities[guid] = entity;
                ordered_items.emplace_back(guid, item);
                if (id->guid == selected_guid_) selected = item;
            }
            for (const auto& [guid, item] : ordered_items) {
                const aether::Entity parent = scene_document_.ParentOf(item_entities.at(guid));
                const auto* parent_id = parent.IsNull() ? nullptr
                    : scene_document_.GetWorld().GetComponent<aether::IdComponent>(parent);
                const auto parent_item = parent_id ? items.find(aether::ToString(parent_id->guid)) : items.end();
                if (parent_item != items.end() && parent_item->second != item)
                    parent_item->second->addChild(item);
                else
                    root->addChild(item);
            }
            std::size_t matching_entities = 0;
            if (!filter.isEmpty()) {
                for (const auto& [guid, item] : ordered_items) {
                    Q_UNUSED(guid);
                    item->setHidden(true);
                }
                for (const auto& [guid, item] : ordered_items) {
                    if (!item->text(0).mid(3).contains(filter, Qt::CaseInsensitive)) continue;
                    ++matching_entities;
                    for (QTreeWidgetItem* ancestor = item; ancestor && ancestor != root; ancestor = ancestor->parent())
                        ancestor->setHidden(false);
                }
            } else {
                matching_entities = ordered_items.size();
            }
            root->setText(0, filter.isEmpty()
                ? QString("%1  ·  %2 entities").arg(scene_label).arg(ordered_items.size())
                : QString("%1  ·  %2 matches / %3 entities")
                      .arg(scene_label).arg(matching_entities).arg(ordered_items.size()));
            if (matching_entities == 0) {
                const QString message = filter.isEmpty() ? "No entities in this world yet" :
                    QString("No entities match “%1”").arg(filter);
                auto* empty_item = new QTreeWidgetItem(root, {message});
                empty_item->setFlags(Qt::NoItemFlags);
                empty_item->setForeground(0, QColor("#8997a4"));
            }
            hierarchy_tree_->expandAll();
            if (selected && selected->isHidden()) selected = nullptr;
            if (!selected && !preserveSelection && !ordered_items.empty()) selected = ordered_items.front().second;
            if (selected && selected->isHidden()) selected = nullptr;
            if (!selected && !preserveSelection) {
                for (const auto& [guid, item] : ordered_items) {
                    Q_UNUSED(guid);
                    if (!item->isHidden()) { selected = item; break; }
                }
            }
            hierarchy_tree_->setCurrentItem(selected ? selected : root);
        }
        SelectHierarchyItem(selected);
    }

    void ShowHierarchyContextMenu(const QPoint& point) {
        QTreeWidgetItem* item = hierarchy_tree_->itemAt(point);
        if (!item || !item->parent()) return;
        hierarchy_tree_->setCurrentItem(item);
        QMenu menu(this);
        QAction* create_child = menu.addAction("Create Child Entity…");
        QAction* move_to_root = menu.addAction("Move to World Root");
        move_to_root->setEnabled(!scene_document_.ParentOf(selected_entity_).IsNull());
        const QAction* choice = menu.exec(hierarchy_tree_->viewport()->mapToGlobal(point));
        if (choice == create_child) CreateChildEntityDialog();
        else if (choice == move_to_root) MoveSelectedEntityToRoot();
    }

    void CreateChildEntityDialog() {
        if (selected_entity_.IsNull()) return;
        const aether::Entity parent = selected_entity_;
        bool accepted = false;
        const QString name = QInputDialog::getText(this, "Create Child Entity", "Entity name:",
                                                   QLineEdit::Normal, "New Child", &accepted).trimmed();
        if (!accepted || name.isEmpty()) return;
        const aether::Entity entity = scene_document_.CreateEntity(
            name.toStdString(), aether::editor::SceneEntityKind::Empty, {}, parent);
        if (const auto* id = scene_document_.GetWorld().GetComponent<aether::IdComponent>(entity))
            selected_guid_ = id->guid;
        RefreshHierarchy();
        UpdateSceneStatus();
    }

    void MoveSelectedEntityToRoot() {
        if (selected_entity_.IsNull() || !scene_document_.ReparentEntity(selected_entity_)) return;
        RefreshHierarchy();
        UpdateSceneStatus();
    }

    void SelectHierarchyItem(QTreeWidgetItem* item) {
        if (!item || !item->parent()) {
            selected_entity_ = aether::kNullEntity;
            selected_guid_ = {};
            viewport_->SetSelectedGuid({});
            inspector_stack_->setCurrentIndex(0);
            inspector_scroll_->verticalScrollBar()->setValue(0);
            inspector_name_->clear();
            inspector_name_->setEnabled(false);
            inspector_status_->setText("NO SELECTION · SELECT AN ENTITY");
            physics_component_panel_->SetEntity(aether::kNullEntity);
#if defined(AETHER_QT_EDITOR_SCRIPTING)
            script_component_editor_->SetEntity(aether::kNullEntity);
#endif
            reset_position_button_->setEnabled(false);
            reset_rotation_button_->setEnabled(false);
            reset_scale_button_->setEnabled(false);
            for (auto* field : position_fields_) {
                const QSignalBlocker blocker(field);
                field->setValue(0.0);
                field->setEnabled(false);
            }
            for (auto* field : rotation_fields_) {
                const QSignalBlocker blocker(field);
                field->setValue(0.0);
                field->setEnabled(false);
            }
            for (auto* field : scale_fields_) {
                const QSignalBlocker blocker(field);
                field->setValue(1.0);
                field->setEnabled(false);
            }
            camera_inspector_.group->setVisible(false);
            camera_inspector_.cine_group->setVisible(false);
            RefreshSequenceTarget();
            UpdateSceneStatus();
            return;
        }
        aether::EntityGuid guid;
        if (!aether::ParseEntityGuid(item->data(0, Qt::UserRole).toString().toStdString(), guid)) return;
        selected_entity_ = scene_document_.Guids().Find(scene_document_.GetWorld(), guid);
        if (selected_entity_.IsNull()) return;
        selected_guid_ = guid;
        viewport_->SetSelectedGuid(guid);
        physics_component_panel_->SetEntity(selected_entity_);
#if defined(AETHER_QT_EDITOR_SCRIPTING)
        script_component_editor_->SetEntity(selected_entity_);
#endif
        inspector_stack_->setCurrentIndex(1);
        inspector_scroll_->verticalScrollBar()->setValue(0);
        inspector_status_->setText("ENTITY · ACTIVE SELECTION");
        const QSignalBlocker name_blocker(inspector_name_);
        inspector_name_->setText(QString::fromStdString(scene_document_.Name(selected_entity_)));
        const auto* transform = scene_document_.GetWorld().GetComponent<aether::Transform>(selected_entity_);
        const auto* camera = scene_document_.GetWorld().GetComponent<aether::Camera>(selected_entity_);
        const auto* cine_camera = scene_document_.GetWorld().GetComponent<aether::CineCamera>(selected_entity_);
        camera_inspector_.group->setVisible(camera != nullptr);
        if (camera) SyncCameraInspector();
        camera_inspector_.cine_group->setVisible(cine_camera != nullptr);
        if (cine_camera) SyncCineCameraInspector();
        reset_position_button_->setEnabled(transform != nullptr);
        reset_rotation_button_->setEnabled(transform != nullptr);
        reset_scale_button_->setEnabled(transform != nullptr);
        for (std::size_t axis = 0; axis < position_fields_.size(); ++axis) {
            const QSignalBlocker blocker(position_fields_[axis]);
            position_fields_[axis]->setEnabled(transform != nullptr);
            if (transform) {
                const float values[] = {transform->position.x, transform->position.y, transform->position.z};
                position_fields_[axis]->setValue(values[axis]);
            }
        }
        SyncInspectorRotation();
        SyncInspectorScale();
        inspector_name_->setEnabled(true);
        delete_entity_action_->setEnabled(true);
        RefreshSequenceTarget();
        UpdateSceneStatus();
    }

    void ApplyInspectorPosition(bool committed) {
        if (selected_entity_.IsNull()) return;
        const aether::Vec3 position{static_cast<aether::f32>(position_fields_[0]->value()),
                                    static_cast<aether::f32>(position_fields_[1]->value()),
                                    static_cast<aether::f32>(position_fields_[2]->value())};
        if (scene_document_.SetPosition(selected_entity_, position, committed)) UpdateSceneStatus();
    }

    void ApplyInspectorRotation(bool committed) {
        if (selected_entity_.IsNull()) return;
        const aether::Vec3 euler_degrees{static_cast<aether::f32>(rotation_fields_[0]->value()),
                                        static_cast<aether::f32>(rotation_fields_[1]->value()),
                                        static_cast<aether::f32>(rotation_fields_[2]->value())};
        if (scene_document_.SetRotation(selected_entity_, EulerDegreesToRotation(euler_degrees), committed)) {
            if (committed) SyncInspectorRotation();
            UpdateSceneStatus();
        }
    }

    void ApplyInspectorScale(bool committed) {
        if (selected_entity_.IsNull()) return;
        const aether::Vec3 scale{static_cast<aether::f32>(scale_fields_[0]->value()),
                                 static_cast<aether::f32>(scale_fields_[1]->value()),
                                 static_cast<aether::f32>(scale_fields_[2]->value())};
        if (scene_document_.SetScale(selected_entity_, scale, committed)) {
            if (committed) SyncInspectorScale();
            UpdateSceneStatus();
        }
    }

    template <typename T>
    void ApplyCameraInspectorField(const char* field_name, T value) {
        if (selected_entity_.IsNull()) return;
        if (scene_document_.SetCameraField(selected_entity_, field_name, aether::reflect::Any(value))) {
            SyncCameraInspector();
            UpdateSceneStatus();
        }
    }

    void SyncCameraInspector() {
        if (selected_entity_.IsNull()) return;
        const auto* camera = scene_document_.GetWorld().GetComponent<aether::Camera>(selected_entity_);
        if (!camera) return;
        {
            const QSignalBlocker blocker(camera_inspector_.projection);
            const int index = camera_inspector_.projection->findData(static_cast<int>(camera->projection));
            if (index >= 0) camera_inspector_.projection->setCurrentIndex(index);
        }
        const QSignalBlocker fov_blocker(camera_inspector_.fov);
        const QSignalBlocker ortho_blocker(camera_inspector_.ortho_height);
        const QSignalBlocker near_blocker(camera_inspector_.near_plane);
        const QSignalBlocker far_blocker(camera_inspector_.far_plane);
        const QSignalBlocker priority_blocker(camera_inspector_.priority);
        camera_inspector_.fov->setValue(camera->fov_degrees);
        camera_inspector_.ortho_height->setValue(camera->ortho_height);
        camera_inspector_.near_plane->setValue(camera->near_plane);
        camera_inspector_.far_plane->setValue(camera->far_plane);
        camera_inspector_.priority->setValue(camera->priority);
        const bool perspective = camera->projection == aether::Projection::Perspective;
        camera_inspector_.fov->setEnabled(perspective);
        camera_inspector_.ortho_height->setEnabled(!perspective);
    }

    void SyncCineCameraInspector() {
        if (selected_entity_.IsNull()) return;
        const auto* camera = scene_document_.GetWorld().GetComponent<aether::CineCamera>(selected_entity_);
        if (!camera) return;
        const QSignalBlocker focal_blocker(camera_inspector_.focal_length);
        const QSignalBlocker width_blocker(camera_inspector_.sensor_width);
        const QSignalBlocker height_blocker(camera_inspector_.sensor_height);
        const QSignalBlocker aperture_blocker(camera_inspector_.aperture);
        const QSignalBlocker focus_blocker(camera_inspector_.focus_distance);
        camera_inspector_.focal_length->setValue(camera->focal_length_mm);
        camera_inspector_.sensor_width->setValue(camera->sensor_width_mm);
        camera_inspector_.sensor_height->setValue(camera->sensor_height_mm);
        camera_inspector_.aperture->setValue(camera->aperture_f);
        camera_inspector_.focus_distance->setValue(camera->focus_distance);
    }

    void SyncInspectorPosition() {
        if (selected_entity_.IsNull()) return;
        const auto* transform = scene_document_.GetWorld().GetComponent<aether::Transform>(selected_entity_);
        if (!transform) return;
        const float values[] = {transform->position.x, transform->position.y, transform->position.z};
        for (std::size_t axis = 0; axis < position_fields_.size(); ++axis) {
            const QSignalBlocker blocker(position_fields_[axis]);
            position_fields_[axis]->setValue(values[axis]);
        }
        SyncInspectorRotation();
        SyncInspectorScale();
    }

    void SyncInspectorRotation() {
        if (selected_entity_.IsNull()) return;
        const auto* transform = scene_document_.GetWorld().GetComponent<aether::Transform>(selected_entity_);
        if (!transform) return;
        const aether::Vec3 degrees = RotationToEulerDegrees(transform->rotation);
        const float values[] = {degrees.x, degrees.y, degrees.z};
        for (std::size_t axis = 0; axis < rotation_fields_.size(); ++axis) {
            const QSignalBlocker blocker(rotation_fields_[axis]);
            rotation_fields_[axis]->setValue(values[axis]);
        }
    }

    void SyncInspectorScale() {
        if (selected_entity_.IsNull()) return;
        const auto* transform = scene_document_.GetWorld().GetComponent<aether::Transform>(selected_entity_);
        if (!transform) return;
        const float values[] = {transform->scale.x, transform->scale.y, transform->scale.z};
        for (std::size_t axis = 0; axis < scale_fields_.size(); ++axis) {
            const QSignalBlocker blocker(scale_fields_[axis]);
            scale_fields_[axis]->setValue(values[axis]);
        }
    }

    void UpdateSceneStatus() {
        const auto play_state = scene_document_.PlayState();
        const bool editing = play_state == aether::editor::PlaySession::State::Editing;
        const bool playing = play_state == aether::editor::PlaySession::State::Playing;
        const QString project_name = project_settings_.name.empty()
            ? QStringLiteral("Aether Engine") : QString::fromStdString(project_settings_.name);
        QString scene_name = scene_document_.FilePath().empty()
            ? QStringLiteral("Untitled Scene")
            : QString::fromStdWString(scene_document_.FilePath().filename().wstring());
        if (scene_document_.IsDirty()) scene_name += " *";
        setWindowTitle(scene_name + " — " + project_name);
        save_scene_action_->setEnabled(editing);
        new_scene_action_->setEnabled(editing);
        open_scene_action_->setEnabled(editing);
        new_project_action_->setEnabled(editing);
        open_project_action_->setEnabled(editing);
        undo_action_->setEnabled(editing && scene_document_.CanUndo());
        redo_action_->setEnabled(editing && scene_document_.CanRedo());
        const bool has_selection = !selected_entity_.IsNull() && scene_document_.GetWorld().IsAlive(selected_entity_);
        physics_component_panel_->setEnabled(editing);
#if defined(AETHER_QT_EDITOR_SCRIPTING)
        script_component_editor_->setEnabled(editing);
#endif
        create_entity_action_->setEnabled(editing);
        create_camera_action_->setEnabled(editing);
        place_model_action_->setEnabled(editing && !project_file_.empty());
        duplicate_entity_action_->setEnabled(editing && has_selection);
        delete_entity_action_->setEnabled(editing && has_selection);
        add_entity_button_->setEnabled(editing);
        empty_add_entity_button_->setEnabled(editing);
        duplicate_entity_button_->setEnabled(editing && has_selection);
        play_action_->setEnabled(!playing);
        pause_action_->setEnabled(playing);
        stop_action_->setEnabled(!editing);
        play_action_->setText(play_state == aether::editor::PlaySession::State::Paused ? "Resume" : "Play");
        play_button_->setText(play_state == aether::editor::PlaySession::State::Paused ? "▶   Resume" : "▶   Play");
        play_button_->setEnabled(!playing);
        pause_button_->setEnabled(playing);
        stop_button_->setEnabled(!editing);
        sequence_add_track_button_->setEnabled(editing && has_selection);
        sequence_remove_track_button_->setEnabled(editing && sequence_track_list_->currentRow() >= 0);
        const int sequence_track_row = sequence_track_list_->currentRow();
        const bool has_sequence_track = sequence_track_row >= 0 &&
            static_cast<std::size_t>(sequence_track_row) < sequence_document_.Sequence().tracks.size();
        sequence_set_key_button_->setEnabled(editing && has_sequence_track &&
            sequence_document_.Sequence().tracks[static_cast<std::size_t>(sequence_track_row)].type ==
                aether::seq::TrackType::Transform &&
            !sequence_document_.Sequence().tracks[static_cast<std::size_t>(sequence_track_row)].locked);
        sequence_play_button_->setEnabled(editing && !sequence_timer_.isActive());
        sequence_pause_button_->setEnabled(editing && sequence_timer_.isActive());
        sequence_stop_button_->setEnabled(editing && sequence_preview_active_);
        const QString state = editing ? (scene_document_.IsDirty() ? "dirty" : "saved")
                                      : (playing ? "playing" : "paused");
        const QString state_text = editing ? (scene_document_.IsDirty() ? "UNSAVED" : "SAVED")
                                           : (playing ? "PLAYING" : "PAUSED");
        for (QLabel* badge : {toolbar_state_badge_, status_state_badge_}) {
            if (!badge) continue;
            badge->setText(state_text);
            badge->setProperty("state", state);
            badge->style()->unpolish(badge);
            badge->style()->polish(badge);
        }
        const QString undo_label = scene_document_.CanUndo()
            ? QString::fromStdString(scene_document_.UndoLabel()) : QStringLiteral("Ready");
        statusBar()->showMessage(QString("%1 entities   ·   %2")
            .arg(scene_document_.Entities().size())
            .arg(undo_label));
        UpdatePlayInputHint();
    }

    void UpdatePlayInputHint() {
        if (!play_input_hint_) return;
        const auto state = scene_document_.PlayState();
        const bool playing = state == aether::editor::PlaySession::State::Playing;
        const bool paused = state == aether::editor::PlaySession::State::Paused;
        play_input_hint_->setVisible(playing || paused);
        if (paused) {
            play_input_hint_->setText("Ⅱ  PLAY PAUSED");
            play_input_hint_->setToolTip("Resume Play to send input to the game.");
        } else if (playing && viewport_ && viewport_->hasFocus()) {
            play_input_hint_->setText("●  GAME INPUT ACTIVE");
            play_input_hint_->setToolTip("Keyboard and mouse input from the 3D view drive project input actions.");
        } else if (playing) {
            play_input_hint_->setText("○  CLICK 3D VIEW FOR GAME INPUT");
            play_input_hint_->setToolTip("Focus the 3D view to send keyboard and mouse input to the game.");
        }
    }

    void StartOrResumePlay() {
        StopSequencePreview();
        const bool starting = scene_document_.PlayState() == aether::editor::PlaySession::State::Editing;
        scene_document_.Play();
        if (starting) {
            try {
                const std::filesystem::path content_root = project_file_.empty()
                    ? std::filesystem::path{} : aether::ProjectPaths::ForFile(project_file_).content;
                play_runtime_ = std::make_unique<aether::editor::qt::EditorPlayRuntime>(
                    scene_document_.GetWorld(), scene_document_.Guids(), content_root, project_settings_);
            } catch (const std::exception& exception) {
                play_runtime_.reset();
                scene_document_.Stop();
                QMessageBox::critical(this, "Couldn't start Play mode", QString::fromUtf8(exception.what()));
                UpdateSceneStatus();
                return;
            }
        }
        viewport_->SetPlayInputActive(true);
        play_clock_.start();
        play_simulation_timer_.start();
        UpdateSceneStatus();
    }

    void PausePlay() {
        play_simulation_timer_.stop();
        viewport_->SetPlayInputActive(false);
        scene_document_.Pause();
        UpdateSceneStatus();
    }

    void StopPlay() {
        if (scene_document_.PlayState() == aether::editor::PlaySession::State::Editing) return;
        play_simulation_timer_.stop();
        viewport_->SetPlayInputActive(false);
        if (play_runtime_) play_runtime_->Stop();
        play_runtime_.reset();
        scene_document_.Stop();
        selected_entity_ = scene_document_.Guids().Find(scene_document_.GetWorld(), selected_guid_);
        if (selected_entity_.IsNull()) selected_guid_ = {};
        RefreshHierarchy();
        UpdateSceneStatus();
    }

    void TickPlaySimulation() {
        if (!play_runtime_ || scene_document_.PlayState() != aether::editor::PlaySession::State::Playing) return;
        const aether::f32 dt = static_cast<aether::f32>(play_clock_.restart()) / 1000.0f;
        play_runtime_->Tick(dt);
        if (!selected_entity_.IsNull() && scene_document_.GetWorld().IsAlive(selected_entity_)) {
            SyncInspectorPosition();
            viewport_->update();
        }
    }

    void NewScene() {
        if (!ConfirmDiscardSceneChanges()) return;
        StopSequencePreview();
        scene_document_.NewScene();
        selected_guid_ = {};
        selected_entity_ = aether::kNullEntity;
        RefreshHierarchy();
        viewport_->FitAll();
        UpdateSceneStatus();
    }

    void OpenSceneDialog() {
        const QString file = QFileDialog::getOpenFileName(this, "Open Scene", QDir::homePath(),
                                                           "Aether Scenes (*.ascene *.aesc)");
        if (!file.isEmpty()) OpenScene(std::filesystem::path(file.toStdWString()));
    }

    bool OpenScene(const std::filesystem::path& file) {
        if (!ConfirmDiscardSceneChanges()) return false;
        StopSequencePreview();
        std::string error;
        if (!scene_document_.Load(file, &error)) {
            QMessageBox::critical(this, "Couldn't open scene", QString::fromStdString(error));
            return false;
        }
        selected_guid_ = {};
        selected_entity_ = aether::kNullEntity;
        RefreshHierarchy();
        viewport_->FitAll();
        UpdateSceneStatus();
        return true;
    }

    bool SaveScene() {
        std::filesystem::path path = scene_document_.FilePath();
        if (path.empty() && !project_file_.empty()) {
            if (!project_settings_.startup_scene.empty())
                path = aether::ProjectPaths::ForFile(project_file_).content / project_settings_.startup_scene;
            else
                path = aether::ProjectPaths::ForFile(project_file_).content / "Scenes" / "Main.ascene";
        }
        if (path.empty()) {
            const QString file = QFileDialog::getSaveFileName(this, "Save Scene", QDir::homePath() + "/Main.ascene",
                                                               "Aether JSON Scene (*.ascene);;Aether Binary Scene (*.aesc)");
            if (file.isEmpty()) return false;
            path = std::filesystem::path(file.toStdWString());
        }
        QDir().mkpath(QString::fromStdWString(path.parent_path().wstring()));
        std::string error;
        if (!scene_document_.Save(path, &error)) {
            QMessageBox::critical(this, "Couldn't save scene", QString::fromStdString(error));
            return false;
        }
        if (!project_file_.empty()) {
            const auto content = aether::ProjectPaths::ForFile(project_file_).content;
            const auto relative = path.lexically_relative(content);
            if (!relative.empty() && *relative.begin() != "..") {
                project_settings_.startup_scene = relative.generic_string();
                aether::SaveProject(project_file_, project_settings_, nullptr);
            }
        }
        UpdateSceneStatus();
        return true;
    }

    void CreateEntityDialog() {
        bool accepted = false;
        const QString name = QInputDialog::getText(this, "Create Entity", "Entity name:",
                                                   QLineEdit::Normal, "New Entity", &accepted).trimmed();
        if (!accepted || name.isEmpty()) return;
        const aether::Entity entity = scene_document_.CreateEntity(name.toStdString());
        if (const auto* id = scene_document_.GetWorld().GetComponent<aether::IdComponent>(entity)) selected_guid_ = id->guid;
        RefreshHierarchy();
        UpdateSceneStatus();
    }

    void CreateCameraDialog() {
        bool accepted = false;
        const QString name = QInputDialog::getText(this, "Create Camera", "Camera name:",
                                                   QLineEdit::Normal, "Camera", &accepted).trimmed();
        if (!accepted || name.isEmpty()) return;
        const aether::Entity entity = scene_document_.CreateEntity(
            name.toStdString(), aether::editor::SceneEntityKind::Camera);
        if (const auto* id = scene_document_.GetWorld().GetComponent<aether::IdComponent>(entity)) selected_guid_ = id->guid;
        RefreshHierarchy();
        UpdateSceneStatus();
    }

    void CreateCineCamera() {
        const aether::Entity entity = scene_document_.CreateEntity(
            "Cinematic Camera", aether::editor::SceneEntityKind::CineCamera);
        if (const auto* id = scene_document_.GetWorld().GetComponent<aether::IdComponent>(entity)) selected_guid_ = id->guid;
        RefreshHierarchy();
        UpdateSceneStatus();
    }

    void PlaceModelDialog() {
        if (project_file_.empty()) {
            QMessageBox::information(this, "Open a project", "Open or create a project before placing a model asset.");
            return;
        }
        const auto content = aether::ProjectPaths::ForFile(project_file_).content;
        const QString file = QFileDialog::getOpenFileName(this, "Place Model",
            QString::fromStdWString(content.wstring()), "3D Models (*.gltf *.glb)");
        if (file.isEmpty()) return;
        PlaceModel(std::filesystem::path(file.toStdWString()));
    }

    void PlaceModel(const std::filesystem::path& file) {
        if (project_file_.empty()) return;
        const auto content = aether::ProjectPaths::ForFile(project_file_).content;
        const auto relative = file.lexically_relative(content);
        if (relative.empty() || *relative.begin() == "..") {
            QMessageBox::warning(this, "Model outside project", "Choose a model inside this project's Content folder.");
            return;
        }
        const std::string name = file.stem().string();
        const aether::Entity entity = scene_document_.CreateEntity(
            name, aether::editor::SceneEntityKind::Model, relative.generic_string());
        if (const auto* id = scene_document_.GetWorld().GetComponent<aether::IdComponent>(entity)) selected_guid_ = id->guid;
        RefreshHierarchy();
        UpdateSceneStatus();
        statusBar()->showMessage(QString("●  MODEL PLACED     %1").arg(QString::fromStdWString(file.filename().wstring())));
    }

    void ShowAddEntityMenu() {
        QMenu menu(this);
        auto* empty = menu.addAction("Empty Entity");
        auto* camera = menu.addAction("Camera");
        auto* cine_camera = menu.addAction("Cinematic Camera");
        auto* model = menu.addAction("Model Asset…");
        model->setEnabled(!project_file_.empty());
        const QAction* choice = menu.exec(add_entity_button_->mapToGlobal(QPoint(0, add_entity_button_->height())));
        if (choice == empty) CreateEntityDialog();
        else if (choice == camera) CreateCameraDialog();
        else if (choice == cine_camera) CreateCineCamera();
        else if (choice == model) PlaceModelDialog();
    }

    void DuplicateSelectedEntity() {
        if (selected_entity_.IsNull()) return;
        const aether::Entity duplicate = scene_document_.DuplicateEntity(selected_entity_);
        if (duplicate.IsNull()) return;
        if (const auto* id = scene_document_.GetWorld().GetComponent<aether::IdComponent>(duplicate)) selected_guid_ = id->guid;
        RefreshHierarchy();
        UpdateSceneStatus();
    }

    void DeleteSelectedEntity() {
        if (selected_entity_.IsNull() || !scene_document_.DestroyEntity(selected_entity_)) return;
        selected_guid_ = {};
        selected_entity_ = aether::kNullEntity;
        RefreshHierarchy();
        viewport_->FitAll();
        UpdateSceneStatus();
    }

    void Undo() {
        if (scene_document_.Undo()) RefreshHierarchy();
        UpdateSceneStatus();
    }

    void Redo() {
        if (scene_document_.Redo()) RefreshHierarchy();
        UpdateSceneStatus();
    }

    void CreateProjectDialog() {
        QDialog dialog(this);
        dialog.setWindowTitle("Create Aether Project");
        auto* layout = new QVBoxLayout(&dialog);
        auto* form = new QFormLayout;
        auto* name = new QLineEdit("MyGame", &dialog);
        auto* location_row = new QWidget(&dialog);
        auto* location_layout = new QHBoxLayout(location_row);
        location_layout->setContentsMargins(0, 0, 0, 0);
        auto* location = new QLineEdit(QDir::homePath(), location_row);
        auto* browse = new QPushButton("Browse…", location_row);
        location_layout->addWidget(location); location_layout->addWidget(browse);
        form->addRow("Project name", name);
        form->addRow("Create in", location_row);
        layout->addLayout(form);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText("Create");
        layout->addWidget(buttons);
        QObject::connect(browse, &QPushButton::clicked, &dialog, [&dialog, location] {
            const QString directory = QFileDialog::getExistingDirectory(&dialog, "Choose project location", location->text());
            if (!directory.isEmpty()) location->setText(directory);
        });
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return;
        if (!ConfirmDiscardSceneChanges()) return;

        aether::ProjectPaths paths;
        std::string error;
        const auto parent = std::filesystem::path(location->text().toStdWString());
        if (!aether::CreateProject(parent, name->text().toStdString(), &paths, &error)) {
            QMessageBox::critical(this, "Couldn't create project", QString::fromStdString(error));
            return;
        }
        OpenProject(paths.file, false);
    }

    void OpenProjectDialog() {
        const QString file = QFileDialog::getOpenFileName(this, "Open Aether Project", QDir::homePath(),
                                                           "Aether Projects (*.aproject)");
        if (!file.isEmpty()) OpenProject(std::filesystem::path(file.toStdWString()));
    }

    void OpenProject(const std::filesystem::path& file, bool confirm_changes = true) {
        if (confirm_changes && (!ConfirmDiscardSequenceChanges() || !ConfirmDiscardSceneChanges())) return;
        StopSequencePreview();
        aether::ProjectSettings settings;
        std::string error;
        if (!aether::LoadProject(file, settings, &error)) {
            QMessageBox::critical(this, "Couldn't open project", QString::fromStdString(error));
            return;
        }
        const auto paths = aether::ProjectPaths::ForFile(file);
        if (!aether::EnsureProjectFolders(paths, &error)) {
            QMessageBox::critical(this, "Couldn't prepare project folders", QString::fromStdString(error));
            return;
        }

        aether::plugin::PluginManager candidate_plugins;
        if (!aether::plugin::ResolveProjectPlugins(file, candidate_plugins, &error) ||
            !ValidatePluginModules(candidate_plugins, &error)) {
            QMessageBox::critical(this, "Couldn't load project plugins", QString::fromStdString(error));
            return;
        }

        // Stop the current project's modules before starting the next set.
        // If startup or scene loading fails, restore the current project modules.
        plugin_manager_.ShutdownModules();
        const auto module_warnings = candidate_plugins.StartModules(true, true);
        if (!module_warnings.empty()) {
            candidate_plugins.ShutdownModules();
            const auto restore_warnings = plugin_manager_.StartModules(true, true);
            error = module_warnings.front();
            if (!restore_warnings.empty()) error += " The previous project's modules could not be restarted: " +
                                                    restore_warnings.front();
            QMessageBox::critical(this, "Couldn't start project plugins", QString::fromStdString(error));
            return;
        }

        std::filesystem::path startup_scene;
        if (!settings.startup_scene.empty()) startup_scene = paths.content / settings.startup_scene;
        if (!startup_scene.empty() && std::filesystem::exists(startup_scene)) {
            if (!scene_document_.Load(startup_scene, &error)) {
                candidate_plugins.ShutdownModules();
                const auto restore_warnings = plugin_manager_.StartModules(true, true);
                if (!restore_warnings.empty()) error += " The previous project's modules could not be restarted: " +
                                                        restore_warnings.front();
                QMessageBox::critical(this, "Couldn't open startup scene", QString::fromStdString(error));
                return;
            }
        } else {
            scene_document_.NewScene();
        }
        plugin_manager_ = std::move(candidate_plugins);
        project_file_ = file;
        project_settings_ = std::move(settings);
        physics_component_panel_->SetLayerNames(project_settings_.layers);
        RememberRecentProject(file);
        viewport_->SetContentRoot(paths.content);
#if defined(AETHER_QT_EDITOR_SCRIPTING)
        script_component_editor_->SetContentRoot(paths.content);
#endif
        selected_guid_ = {};
        selected_entity_ = aether::kNullEntity;
        RefreshHierarchy();
        viewport_->FitAll();
        SetContentDirectory(QString::fromStdWString(paths.content.wstring()));
        const QString project_name = QString::fromStdString(project_settings_.name);
        setWindowTitle(project_name + " — Aether Engine");
        save_project_action_->setEnabled(true);
        new_folder_button_->setEnabled(true);
        asset_filter_->setEnabled(true);
        asset_type_filter_->setEnabled(true);
        asset_view_->setEnabled(true);
        asset_view_->setVisible(true);
        asset_context_->setVisible(true);
        content_welcome_panel_->setVisible(false);
        content_breadcrumb_->setEnabled(true);
        UpdateSceneStatus();
    }

    void LoadRecentProjects() {
        recent_projects_file_ = aether::editor::RecentProjects::DefaultConfigDir() / "recent_projects.json";
        recent_projects_.Load(recent_projects_file_);
        if (recent_projects_.RemoveMissing() != 0 && !recent_projects_.Save(recent_projects_file_)) {
            AETHER_LOG_WARN("QtEditor", "Couldn't remove missing projects from the recent-project list");
        }
    }

    void RememberRecentProject(const std::filesystem::path& file) {
        std::error_code ec;
        std::filesystem::path absolute = std::filesystem::absolute(file, ec);
        if (ec) absolute = file;
        recent_projects_.Add(absolute.lexically_normal());
        if (!recent_projects_.Save(recent_projects_file_)) {
            AETHER_LOG_WARN("QtEditor", "Couldn't save the recent-project list");
        }
        RefreshRecentProjectsMenu();
    }

    void ClearRecentProjects() {
        const auto entries = recent_projects_.Entries();
        for (const auto& entry : entries) recent_projects_.Remove(entry);
        if (!recent_projects_.Save(recent_projects_file_)) {
            AETHER_LOG_WARN("QtEditor", "Couldn't clear the recent-project list");
        }
        RefreshRecentProjectsMenu();
    }

    void RefreshRecentProjectsMenu() {
        if (!recent_projects_menu_) return;
        recent_projects_menu_->clear();
        const auto& entries = recent_projects_.Entries();
        if (entries.empty()) {
            QAction* empty = recent_projects_menu_->addAction("No recent projects");
            empty->setEnabled(false);
            return;
        }
        for (const auto& entry : entries) {
            const QString project_name = QString::fromStdWString(entry.stem().wstring());
            const QString parent_name = QString::fromStdWString(entry.parent_path().filename().wstring());
            const QString label = parent_name.isEmpty()
                ? project_name : QString("%1   —   %2").arg(project_name, parent_name);
            QAction* action = recent_projects_menu_->addAction(label);
            action->setToolTip(QString::fromStdWString(entry.wstring()));
            QObject::connect(action, &QAction::triggered, this, [this, entry] { OpenProject(entry); });
        }
        recent_projects_menu_->addSeparator();
        QAction* clear = recent_projects_menu_->addAction("Clear Recent Projects");
        QObject::connect(clear, &QAction::triggered, this, [this] { ClearRecentProjects(); });
    }

    void OpenContentAsset(const QString& path) {
        const QFileInfo info(path);
        const QString suffix = info.suffix().toLower();
        if (suffix == "ascene" || suffix == "aesc") {
            OpenScene(std::filesystem::path(path.toStdWString()));
            return;
        }
        if (suffix == "aproject") {
            OpenProject(std::filesystem::path(path.toStdWString()));
            return;
        }
        if (suffix == "asequence") {
            OpenSequence(std::filesystem::path(path.toStdWString()));
            return;
        }
        if (suffix == "gltf" || suffix == "glb") {
            PlaceModel(std::filesystem::path(path.toStdWString()));
            return;
        }
        QMessageBox::information(this, "Asset preview unavailable",
                                 QString("The current editor can't open %1 yet.").arg(info.fileName()));
    }

    void UpdateSelectedContentAsset(const QModelIndex& index) {
        selected_content_path_.clear();
        const bool is_directory = index.isValid() && asset_model_->isDir(index);
        if (index.isValid() && !is_directory) {
            const QString suffix = asset_model_->fileInfo(index).suffix().toLower();
            if (suffix == "gltf" || suffix == "glb") selected_content_path_ = asset_model_->filePath(index);
        }
        place_model_button_->setEnabled(!project_file_.empty() && !selected_content_path_.isEmpty());
        if (project_file_.empty()) {
            asset_context_->setText("Open or create a project to browse its Content folder.");
        } else if (is_directory) {
            asset_context_->setText(QString("Folder · double-click to open %1").arg(asset_model_->fileName(index)));
        } else if (index.isValid()) {
            const QString name = asset_model_->fileName(index);
            const QString suffix = asset_model_->fileInfo(index).suffix().toUpper();
            asset_context_->setText(suffix == "GLTF" || suffix == "GLB"
                ? QString("MODEL · %1 · Select Place Model or double-click to add it to the World").arg(name)
                : QString("%1 ASSET · %2 · Double-click to open").arg(suffix.isEmpty() ? "FILE" : suffix, name));
        } else if (asset_view_->model()->rowCount(asset_view_->rootIndex()) == 0) {
            const bool loading = asset_model_->canFetchMore(asset_view_->rootIndex());
            const bool filtered = !asset_filter_->text().trimmed().isEmpty() || asset_type_filter_->currentIndex() != 0;
            asset_context_->setText(loading ? "Loading folder contents…" : filtered
                ? "No matching items in this folder. Clear the search or choose All Assets."
                : "This folder is empty. Add assets to Content or create a folder.");
        } else {
            const int visible_count = asset_view_->model()->rowCount(asset_view_->rootIndex());
            const bool filtered = !asset_filter_->text().trimmed().isEmpty() || asset_type_filter_->currentIndex() != 0;
            const QString noun = filtered ? "matching item" : "item";
            const QString count = QString("%1 %2%3 in this folder")
                .arg(visible_count)
                .arg(noun)
                .arg(visible_count == 1 ? "" : "s");
            asset_context_->setText(QString("%1 · Select glTF or GLB models to place them in the World; double-click folders to browse.")
                                        .arg(count));
        }
    }

    void ApplyContentFilters() {
        const QString query = asset_filter_->text().trimmed();
        const QStringList suffixes = asset_type_filter_->currentData().toStringList();
        QStringList patterns;
        if (suffixes.isEmpty()) {
            if (!query.isEmpty()) patterns.push_back("*" + query + "*");
        } else {
            QString name_query = query;
            const QString query_suffix = QFileInfo(query).suffix().toLower();
            if (!query_suffix.isEmpty() && suffixes.contains(query_suffix)) {
                name_query = QFileInfo(query).completeBaseName();
            }
            for (const QString& suffix : suffixes) {
                patterns.push_back("*" + name_query + "*." + suffix);
            }
        }
        asset_model_->setNameFilters(patterns);
        asset_model_->setNameFilterDisables(false);
    }

    void PlaceSelectedContentModel() {
        if (!place_model_button_->isEnabled() || selected_content_path_.isEmpty()) return;
        PlaceModel(std::filesystem::path(selected_content_path_.toStdWString()));
    }

    bool PersistProjectSettings(const aether::ProjectSettings& candidate, std::string* error) {
        if (project_file_.empty() || !aether::SaveProject(project_file_, candidate, error)) return false;
        project_settings_ = candidate;
        physics_component_panel_->SetLayerNames(project_settings_.layers);
        RefreshContentBreadcrumb();
        UpdateSceneStatus();
        return true;
    }

    void ShowProjectSettingsDialog() {
        if (project_file_.empty()) return;
        QDialog dialog(this);
        dialog.setWindowTitle("Project Settings");
        dialog.resize(820, 720);
        auto* layout = new QVBoxLayout(&dialog);
        auto* form = new QFormLayout;
        auto* name = new QLineEdit(QString::fromStdString(project_settings_.name), &dialog);
        auto* startup_row = new QWidget(&dialog);
        auto* startup_layout = new QHBoxLayout(startup_row);
        startup_layout->setContentsMargins(0, 0, 0, 0);
        auto* startup = new QLineEdit(QString::fromStdString(project_settings_.startup_scene), startup_row);
        auto* browse = new QPushButton("Browse…", startup_row);
        startup_layout->addWidget(startup); startup_layout->addWidget(browse);
        auto* fixed_hz = new QDoubleSpinBox(&dialog);
        fixed_hz->setRange(10.0, 480.0); fixed_hz->setSuffix(" Hz"); fixed_hz->setValue(project_settings_.fixed_timestep_hz);
        std::array<QDoubleSpinBox*, 3> gravity{};
        auto* gravity_row = new QWidget(&dialog);
        auto* gravity_layout = new QHBoxLayout(gravity_row);
        gravity_layout->setContentsMargins(0, 0, 0, 0);
        const float gravity_values[] = {project_settings_.gravity.x, project_settings_.gravity.y, project_settings_.gravity.z};
        for (std::size_t axis = 0; axis < gravity.size(); ++axis) {
            gravity[axis] = new QDoubleSpinBox(gravity_row);
            gravity[axis]->setRange(-1000.0, 1000.0); gravity[axis]->setDecimals(3);
            gravity[axis]->setPrefix(QString(QChar('X' + static_cast<int>(axis))) + " ");
            gravity[axis]->setValue(gravity_values[axis]);
            gravity_layout->addWidget(gravity[axis]);
        }
        auto* title = new QLineEdit(QString::fromStdString(project_settings_.window_title), &dialog);
        auto* width = new QSpinBox(&dialog); width->setRange(320, 7680); width->setValue(static_cast<int>(project_settings_.window_width));
        auto* height = new QSpinBox(&dialog); height->setRange(240, 4320); height->setValue(static_cast<int>(project_settings_.window_height));
        auto* vsync = new QCheckBox("Enabled", &dialog); vsync->setChecked(project_settings_.vsync);
        auto* quality = new QComboBox(&dialog);
        for (const auto& preset : project_settings_.quality_presets) quality->addItem(QString::fromStdString(preset.name));
        quality->setCurrentText(QString::fromStdString(project_settings_.default_quality));
        auto make_lines = [&dialog](const std::vector<std::string>& values) {
            auto* edit = new QPlainTextEdit(&dialog);
            QStringList lines;
            for (const auto& value : values) lines << QString::fromStdString(value);
            edit->setPlainText(lines.join('\n'));
            edit->setMaximumHeight(76);
            return edit;
        };
        auto* always_cook = make_lines(project_settings_.always_cook);
        auto* plugins = make_lines(project_settings_.plugins);
        auto* layers = make_lines(project_settings_.layers);
        auto* layers_row = new QWidget(&dialog);
        auto* layers_layout = new QHBoxLayout(layers_row);
        layers_layout->setContentsMargins(0, 0, 0, 0);
        auto* collision_matrix_button = new QPushButton("Edit Matrix…", layers_row);
        layers_layout->addWidget(layers, 1);
        layers_layout->addWidget(collision_matrix_button);
        std::vector<aether::u32> collision_matrix = project_settings_.collision_matrix;
        std::vector<std::string> collision_matrix_layers = project_settings_.layers;
        const auto parse_lines = [](const QPlainTextEdit* edit) {
            std::vector<std::string> values;
            for (const QString& line : edit->toPlainText().split('\n')) {
                const QString value = line.trimmed();
                if (!value.isEmpty()) values.push_back(value.toStdString());
            }
            return values;
        };
        QObject::connect(collision_matrix_button, &QPushButton::clicked, &dialog, [&] {
            const std::vector<std::string> names = parse_lines(layers);
            if (names.empty() || names.front() != "Default" || names.size() > aether::kMaxLayers) {
                QMessageBox::warning(&dialog, "Invalid collision layers",
                                     "Add 1–32 layer names and keep Default as the first layer before editing the matrix.");
                return;
            }
            std::vector<aether::u32> edited;
            if (ShowCollisionMatrixDialog(&dialog, names, collision_matrix_layers, collision_matrix, edited)) {
                collision_matrix_layers = names;
                collision_matrix = std::move(edited);
            }
        });
        form->addRow("Project name", name);
        form->addRow("Startup scene", startup_row);
        form->addRow("Fixed timestep", fixed_hz);
        form->addRow("Gravity", gravity_row);
        form->addRow("Window title", title);
        form->addRow("Window width", width);
        form->addRow("Window height", height);
        form->addRow("VSync", vsync);
        form->addRow("Default quality", quality);
        form->addRow("Always cook (one per line)", always_cook);
        form->addRow("Plugins (one per line)", plugins);
        form->addRow("Collision layers", layers_row);
        layout->addLayout(form);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        QObject::connect(browse, &QPushButton::clicked, &dialog, [this, &dialog, startup] {
            const auto content = aether::ProjectPaths::ForFile(project_file_).content;
            const QString file = QFileDialog::getOpenFileName(&dialog, "Choose startup scene",
                QString::fromStdWString(content.wstring()), "Aether Scenes (*.ascene *.aesc)");
            if (file.isEmpty()) return;
            const auto relative = std::filesystem::path(file.toStdWString()).lexically_relative(content);
            if (!relative.empty() && *relative.begin() != "..") startup->setText(QString::fromStdString(relative.generic_string()));
        });
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog] { dialog.accept(); });
        if (dialog.exec() != QDialog::Accepted) return;

        const QString project_name = name->text().trimmed();
        const std::filesystem::path startup_path(startup->text().trimmed().toStdWString());
        if (project_name.isEmpty() || startup_path.is_absolute() ||
            (!startup_path.empty() && *startup_path.begin() == "..")) {
            QMessageBox::warning(this, "Invalid project settings",
                                 "Project name is required and the startup scene must be relative to Content/.");
            return;
        }
        std::vector<std::string> layer_values = parse_lines(layers);
        const std::set<std::string> unique_layers(layer_values.begin(), layer_values.end());
        if (layer_values.empty() || layer_values.front() != "Default" || layer_values.size() > aether::kMaxLayers ||
            unique_layers.size() != layer_values.size()) {
            QMessageBox::warning(this, "Invalid collision layers",
                                 "Use 1–32 unique layer names and keep Default as the first layer.");
            return;
        }
        aether::ProjectSettings candidate = project_settings_;
        candidate.name = project_name.toStdString();
        candidate.startup_scene = startup_path.generic_string();
        candidate.fixed_timestep_hz = static_cast<aether::f32>(fixed_hz->value());
        candidate.gravity = {static_cast<aether::f32>(gravity[0]->value()), static_cast<aether::f32>(gravity[1]->value()),
                             static_cast<aether::f32>(gravity[2]->value())};
        candidate.window_title = title->text().toStdString();
        candidate.window_width = static_cast<aether::u32>(width->value());
        candidate.window_height = static_cast<aether::u32>(height->value());
        candidate.vsync = vsync->isChecked();
        candidate.default_quality = quality->currentText().toStdString();
        candidate.always_cook = parse_lines(always_cook);
        candidate.plugins = parse_lines(plugins);
        candidate.layers = std::move(layer_values);
        candidate.collision_matrix = RemapCollisionMatrix(collision_matrix_layers, collision_matrix, candidate.layers);
        std::string error;
        if (!PersistProjectSettings(candidate, &error)) {
            QMessageBox::critical(this, "Couldn't save project settings", QString::fromStdString(error));
            return;
        }
        statusBar()->showMessage("●  PROJECT SETTINGS SAVED");
    }

    void RefreshContentBreadcrumb() {
        const QSignalBlocker blocker(content_breadcrumb_);
        content_breadcrumb_->clear();
        if (project_file_.empty()) {
            content_breadcrumb_->addItem("NO PROJECT  /  CONTENT");
            content_breadcrumb_->setCurrentIndex(0);
            content_breadcrumb_->setEnabled(false);
            return;
        }

        const QString root = QDir::cleanPath(QString::fromStdWString(
            aether::ProjectPaths::ForFile(project_file_).content.wstring()));
        const QString project_name = QString::fromStdString(project_settings_.name).toUpper();
        content_breadcrumb_->addItem(project_name + "  /  CONTENT", root);
        const QString relative = QDir(root).relativeFilePath(current_content_path_);
        if (relative != "." && !relative.startsWith("../") && !QDir::isAbsolutePath(relative)) {
            QString prefix;
            QString path = root;
            for (const QString& segment : relative.split('/', Qt::SkipEmptyParts)) {
                prefix = prefix.isEmpty() ? segment : prefix + "/" + segment;
                path = QDir(path).filePath(segment);
                content_breadcrumb_->addItem(project_name + "  /  CONTENT  /  " + prefix.toUpper(),
                                              QDir::cleanPath(path));
            }
        }
        content_breadcrumb_->setCurrentIndex(content_breadcrumb_->count() - 1);
        content_breadcrumb_->setEnabled(true);
    }

    void SetContentDirectory(QString path) {
        selected_content_path_.clear();
        place_model_button_->setEnabled(false);
        const QString root = QDir::cleanPath(QString::fromStdWString(
            aether::ProjectPaths::ForFile(project_file_).content.wstring()));
        path = QDir::cleanPath(path);
        const QString relative = QDir(root).relativeFilePath(path);
        if (relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative) ||
            !QFileInfo(path).isDir()) path = root;
        current_content_path_ = path;
        asset_view_->setRootIndex(asset_model_->setRootPath(current_content_path_));
        parent_folder_button_->setEnabled(current_content_path_ != root);
        RefreshContentBreadcrumb();
        UpdateSelectedContentAsset(QModelIndex());
    }

    void NavigateContentParent() {
        if (project_file_.empty()) return;
        const QString root = QDir::cleanPath(QString::fromStdWString(
            aether::ProjectPaths::ForFile(project_file_).content.wstring()));
        if (current_content_path_ == root) return;
        const QString parent = QDir::cleanPath(QFileInfo(current_content_path_).dir().absolutePath());
        const QString relative = QDir(root).relativeFilePath(parent);
        if (relative == "." || (!relative.startsWith("..") && !QDir::isAbsolutePath(relative)))
            SetContentDirectory(parent);
        else
            SetContentDirectory(root);
    }

    void CreateContentFolder() {
        if (project_file_.empty()) return;
        bool accepted = false;
        const QString name = QInputDialog::getText(this, "Create Content Folder", "Folder name:",
                                                   QLineEdit::Normal, QString(), &accepted).trimmed();
        if (!accepted) return;
        if (name.isEmpty() || name == "." || name == ".." || QFileInfo(name).fileName() != name) {
            QMessageBox::warning(this, "Invalid folder name", "Choose a single folder name without path separators.");
            return;
        }
        if (!QDir(current_content_path_).mkdir(name)) {
            QMessageBox::warning(this, "Couldn't create folder", "A folder with that name may already exist.");
            return;
        }
        statusBar()->showMessage(QString("●  CONTENT FOLDER CREATED     %1").arg(name));
    }

    void ShowCommandPalette() {
        QDialog dialog(this);
        dialog.setWindowTitle("Search actions");
        dialog.setObjectName("commandPalette");
        dialog.setMinimumSize(520, 400);
        auto* layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(10);
        auto* title = new QLabel("Find a command or editor panel", &dialog);
        title->setObjectName("emptyStateTitle");
        layout->addWidget(title);
        auto* query = new QLineEdit(&dialog);
        query->setObjectName("commandSearch");
        query->setPlaceholderText("Search commands and editor panels…");
        query->setClearButtonEnabled(true);
        auto* results = new QListWidget(&dialog);
        results->setAccessibleName("Command results");
        results->setToolTip("Use the arrow keys to move through results, then press Enter");
        auto* no_results = new QLabel("No matching commands. Try a shorter search.", &dialog);
        no_results->setObjectName("muted");
        no_results->setAlignment(Qt::AlignCenter);
        QStringList commands;
        if (new_scene_action_->isEnabled()) commands << "New Scene";
        if (open_scene_action_->isEnabled()) commands << "Open Scene…";
        if (save_scene_action_->isEnabled()) commands << "Save Scene";
        if (undo_action_->isEnabled()) commands << "Undo";
        if (redo_action_->isEnabled()) commands << "Redo";
        if (create_entity_action_->isEnabled()) commands << "Create Entity" << "Create Camera";
        if (place_model_action_->isEnabled()) commands << "Place Model Asset…";
        if (duplicate_entity_action_->isEnabled()) commands << "Duplicate Selected Entity";
        if (delete_entity_action_->isEnabled()) commands << "Delete Selected Entity";
        if (new_project_action_->isEnabled()) commands << "New Project…";
        if (open_project_action_->isEnabled()) commands << "Open Project…";
        if (save_project_action_->isEnabled()) commands << "Project Settings…";
        if (play_action_->isEnabled()) commands << play_action_->text();
        if (pause_action_->isEnabled()) commands << "Pause";
        if (stop_action_->isEnabled()) commands << "Stop";
        commands << "Show Hierarchy" << "Show Inspector" << "Open Content Browser" << "Open Output"
                 << "Open Animation Sequencer" << "Reset Workspace Layout";
        for (const auto& command : commands) results->addItem(command);
        layout->addWidget(query);
        layout->addWidget(results, 1);
        layout->addWidget(no_results);
        auto* keyboard_hint = new QLabel("↑ ↓  Navigate     Enter  Run     Esc  Close", &dialog);
        keyboard_hint->setObjectName("muted");
        keyboard_hint->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(keyboard_hint);
        no_results->setVisible(false);
        QObject::connect(query, &QLineEdit::textChanged, &dialog, [results, no_results](const QString& text) {
            int first_visible = -1;
            int visible_count = 0;
            for (int i = 0; i < results->count(); ++i)
            {
                const bool hidden = !results->item(i)->text().contains(text, Qt::CaseInsensitive);
                results->item(i)->setHidden(hidden);
                if (!hidden) ++visible_count;
                if (!hidden && first_visible < 0) first_visible = i;
            }
            results->setCurrentRow(first_visible);
            no_results->setVisible(visible_count == 0);
        });
        auto invoke = [this, results, &dialog] {
            auto* item = results->currentItem(); if (!item) return;
            const QString name = item->text();
            if (name == "New Project…") { dialog.accept(); CreateProjectDialog(); return; }
            if (name == "Open Project…") { dialog.accept(); OpenProjectDialog(); return; }
            if (name == "Project Settings…") { dialog.accept(); ShowProjectSettingsDialog(); return; }
            if (name == "New Scene") { dialog.accept(); NewScene(); return; }
            if (name == "Open Scene…") { dialog.accept(); OpenSceneDialog(); return; }
            if (name == "Save Scene") { dialog.accept(); SaveScene(); return; }
            if (name == "Undo") { dialog.accept(); Undo(); return; }
            if (name == "Redo") { dialog.accept(); Redo(); return; }
            if (name == "Create Entity") { dialog.accept(); CreateEntityDialog(); return; }
            if (name == "Create Camera") { dialog.accept(); CreateCameraDialog(); return; }
            if (name == "Place Model Asset…") { dialog.accept(); PlaceModelDialog(); return; }
            if (name == "Duplicate Selected Entity") { dialog.accept(); DuplicateSelectedEntity(); return; }
            if (name == "Delete Selected Entity") { dialog.accept(); DeleteSelectedEntity(); return; }
            if (name == "Play" || name == "Resume") { dialog.accept(); StartOrResumePlay(); return; }
            if (name == "Pause") { dialog.accept(); PausePlay(); return; }
            if (name == "Stop") { dialog.accept(); StopPlay(); return; }
            if (name == "Reset Workspace Layout") { dialog.accept(); ResetWorkspaceLayout(); return; }
            if (name == "Show Hierarchy" || name == "Show Inspector" ||
                name == "Open Content Browser" || name == "Open Output" || name == "Open Animation Sequencer") {
            const QString dock_name = name == "Show Hierarchy" ? "hierarchyDock" :
                name == "Show Inspector" ? "inspectorDock" :
                name == "Open Content Browser" ? "contentDock" :
                name == "Open Output" ? "outputDock" : "sequenceDock";
            if (auto* dock = findChild<QDockWidget*>(dock_name)) dock->show(), dock->raise();
            dialog.accept();
            }
        };
        QObject::connect(results, &QListWidget::itemActivated, &dialog, [invoke](QListWidgetItem*) { invoke(); });
        QObject::connect(query, &QLineEdit::returnPressed, &dialog, invoke);
        results->setCurrentRow(0); query->setFocus(); dialog.exec();
    }

    void closeEvent(QCloseEvent* event) override {
        if (scene_document_.PlayState() != aether::editor::PlaySession::State::Editing) StopPlay();
        StopSequencePreview();
        if (!ConfirmDiscardSequenceChanges() || !ConfirmDiscardSceneChanges()) {
            event->ignore();
            return;
        }
        QSettings settings("Aether", "EditorQt");
        settings.setValue("window/geometry", saveGeometry());
        settings.setValue("window/state", saveState(1));
        QMainWindow::closeEvent(event);
    }

    void BuildMenus() {
        auto* file = menuBar()->addMenu("File");
        new_scene_action_ = file->addAction("New Scene");
        open_scene_action_ = file->addAction("Open Scene…");
        save_scene_action_ = file->addAction("Save Scene", QKeySequence::Save);
        save_scene_action_->setToolTip("Save the active scene (Ctrl+S)");
        file->addSeparator();
        new_project_action_ = file->addAction("New Project…");
        open_project_action_ = file->addAction("Open Project…");
        recent_projects_menu_ = file->addMenu("Recent Projects");
        RefreshRecentProjectsMenu();
        file->addSeparator();
        save_project_action_ = file->addAction("Project Settings…");
        save_project_action_->setEnabled(false);
        file->addSeparator();
        file->addAction("Exit", qApp, &QApplication::quit);
        QObject::connect(new_project_action_, &QAction::triggered, this, [this] { CreateProjectDialog(); });
        QObject::connect(open_project_action_, &QAction::triggered, this, [this] { OpenProjectDialog(); });
        QObject::connect(save_project_action_, &QAction::triggered, this, [this] { ShowProjectSettingsDialog(); });
        QObject::connect(new_scene_action_, &QAction::triggered, this, [this] { NewScene(); });
        QObject::connect(open_scene_action_, &QAction::triggered, this, [this] { OpenSceneDialog(); });
        QObject::connect(save_scene_action_, &QAction::triggered, this, [this] { SaveScene(); });

        auto* edit = menuBar()->addMenu("Edit");
        undo_action_ = edit->addAction("Undo"); undo_action_->setShortcut(QKeySequence::Undo);
        redo_action_ = edit->addAction("Redo"); redo_action_->setShortcut(QKeySequence::Redo);
        undo_action_->setToolTip("Undo the last scene edit (Ctrl+Z)");
        redo_action_->setToolTip("Redo the last undone scene edit (Ctrl+Y)");
        edit->addSeparator();
        create_entity_action_ = edit->addAction("Create Entity"); create_entity_action_->setShortcut(QKeySequence("Ctrl+Shift+N"));
        create_camera_action_ = edit->addAction("Create Camera");
        place_model_action_ = edit->addAction("Place Model Asset…");
        duplicate_entity_action_ = edit->addAction("Duplicate Selected Entity", QKeySequence("Ctrl+D"));
        delete_entity_action_ = edit->addAction("Delete Selected Entity"); delete_entity_action_->setShortcut(QKeySequence::Delete);
        QObject::connect(undo_action_, &QAction::triggered, this, [this] { Undo(); });
        QObject::connect(redo_action_, &QAction::triggered, this, [this] { Redo(); });
        QObject::connect(create_entity_action_, &QAction::triggered, this, [this] { CreateEntityDialog(); });
        QObject::connect(create_camera_action_, &QAction::triggered, this, [this] { CreateCameraDialog(); });
        QObject::connect(place_model_action_, &QAction::triggered, this, [this] { PlaceModelDialog(); });
        QObject::connect(duplicate_entity_action_, &QAction::triggered, this, [this] { DuplicateSelectedEntity(); });
        QObject::connect(delete_entity_action_, &QAction::triggered, this, [this] { DeleteSelectedEntity(); });

        auto* play_menu = menuBar()->addMenu("Play");
        play_action_ = play_menu->addAction("Play", QKeySequence("Alt+P"));
        pause_action_ = play_menu->addAction("Pause", QKeySequence("Alt+Shift+P"));
        stop_action_ = play_menu->addAction("Stop", QKeySequence("Shift+F5"));
        QObject::connect(play_action_, &QAction::triggered, this, [this] { StartOrResumePlay(); });
        QObject::connect(pause_action_, &QAction::triggered, this, [this] { PausePlay(); });
        QObject::connect(stop_action_, &QAction::triggered, this, [this] { StopPlay(); });

        window_menu_ = menuBar()->addMenu("Window");
    }

    void BuildToolbar() {
        auto* bar = addToolBar("Editor");
        bar->setObjectName("editorToolbar");
        bar->setMovable(false);
        auto* brand_lockup = new QWidget;
        brand_lockup->setObjectName("brandLockup");
        auto* brand_layout = new QHBoxLayout(brand_lockup);
        brand_layout->setContentsMargins(0, 0, 4, 0);
        brand_layout->setSpacing(8);
        auto* brand_glyph = new QLabel("A");
        brand_glyph->setObjectName("brandGlyph");
        brand_glyph->setAlignment(Qt::AlignCenter);
        auto* brand_copy = new QVBoxLayout;
        brand_copy->setContentsMargins(0, 0, 0, 0);
        brand_copy->setSpacing(0);
        auto* brand_name = new QLabel("AETHER");
        brand_name->setObjectName("brandName");
        auto* brand_subline = new QLabel("WORLD EDITOR");
        brand_subline->setObjectName("brandSubline");
        brand_copy->addWidget(brand_name);
        brand_copy->addWidget(brand_subline);
        brand_layout->addWidget(brand_glyph, 0, Qt::AlignVCenter);
        brand_layout->addLayout(brand_copy);
        bar->addWidget(brand_lockup);
        bar->addSeparator();
        auto* workspace = new QLabel("  WORLD  ");
        workspace->setObjectName("workspaceLabel");
        bar->addWidget(workspace);
        bar->addAction(save_scene_action_);
        bar->addAction(undo_action_);
        bar->addAction(redo_action_);
        toolbar_state_badge_ = new QLabel("SAVED");
        toolbar_state_badge_->setObjectName("sceneStateBadge");
        toolbar_state_badge_->setProperty("state", "saved");
        toolbar_state_badge_->setToolTip("Scene save and play state");
        bar->addWidget(toolbar_state_badge_);
        bar->addSeparator();
        auto* spacer = new QWidget; spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred); bar->addWidget(spacer);
        spacer->setMaximumWidth(180);
        search_button_ = new QPushButton("⌕  Search  Ctrl+K");
        search_button_->setObjectName("commandSearchButton");
        search_button_->setMinimumWidth(168);
        search_button_->setMaximumWidth(190);
        search_button_->setToolTip("Search commands and editor panels (Ctrl+K)");
        bar->addWidget(search_button_);
        play_button_ = new QPushButton("▶   Play"); play_button_->setObjectName("play"); bar->addWidget(play_button_);
        pause_button_ = new QPushButton("Ⅱ"); pause_button_->setToolTip("Pause Play session"); bar->addWidget(pause_button_);
        stop_button_ = new QPushButton("■"); stop_button_->setToolTip("Stop and restore the edited scene"); bar->addWidget(stop_button_);
        pause_button_->setAccessibleName("Pause Play session");
        stop_button_->setAccessibleName("Stop Play session");
        QObject::connect(play_button_, &QPushButton::clicked, play_action_, &QAction::trigger);
        QObject::connect(pause_button_, &QPushButton::clicked, pause_action_, &QAction::trigger);
        QObject::connect(stop_button_, &QPushButton::clicked, stop_action_, &QAction::trigger);

        addToolBarBreak(Qt::TopToolBarArea);
        auto* viewport_bar = addToolBar("Viewport Tools");
        viewport_bar->setObjectName("viewportTools");
        viewport_bar->setMovable(false);
        auto* view_title = new QLabel("SCENE VIEW");
        view_title->setObjectName("viewportTitle");
        auto* view_hint = new QLabel("ORBIT  RMB   ·   PAN  MMB   ·   ZOOM  WHEEL");
        view_hint->setObjectName("viewportHint");
        view_hint->setAccessibleName("3D World View navigation help");
        view_hint->setToolTip("Right-drag: orbit · Middle-drag: pan · Wheel: zoom · Move tool: WASD/QE moves selected object · Other tools: WASD/QE flies camera · Shift: faster");
        auto* perspective = new QLabel("PERSPECTIVE");
        perspective->setObjectName("viewportPerspective");
        viewport_bar->addWidget(view_title);
        viewport_bar->addSeparator();
        viewport_bar->addWidget(perspective);
        viewport_bar->addWidget(view_hint);
        viewport_bar->addSeparator();
        auto* modes = new QActionGroup(viewport_bar);
        modes->setExclusive(true);
        for (const auto* mode : {"Select", "Move", "Rotate Y", "Scale"}) {
            auto* action = viewport_bar->addAction(mode);
            action->setCheckable(true);
            if (std::string(mode) == "Select") action->setToolTip("Select an entity in the viewport");
            else if (std::string(mode) == "Move") action->setToolTip("Drag the selected object to move on the ground plane, use WASD/QE to move it, or drag an X/Y/Z handle for axis movement");
            else if (std::string(mode) == "Rotate Y") action->setToolTip("Rotate the selected entity around its local Y axis");
            else action->setToolTip("Drag the selected object or gold handle up/down to resize it uniformly");
            modes->addAction(action);
            const QString mode_name = QString::fromUtf8(mode);
            if (QString::fromUtf8(mode) == "Select") action->setChecked(true);
            QObject::connect(action, &QAction::triggered, this, [this, mode] {
                const QString mode_name = QString::fromUtf8(mode);
                viewport_->SetToolMode(mode_name);
                if (mode_name == "Move")
                    statusBar()->showMessage("●  MOVE TOOL     DRAG OR USE WASD/QE TO MOVE OBJECT · DRAG X/Y/Z HANDLE FOR AXIS MOVE");
                else if (mode_name == "Rotate Y")
                    statusBar()->showMessage("●  ROTATE Y TOOL     DRAG THE SELECTED ENTITY'S RING");
                else if (mode_name == "Scale")
                    statusBar()->showMessage("●  SCALE TOOL     DRAG SELECTED OBJECT UP/DOWN TO RESIZE UNIFORMLY");
                else
                    statusBar()->showMessage("●  SELECT TOOL     CLICK AN ENTITY IN THE 3D VIEW");
            });
        }
        viewport_bar->addSeparator();
        auto* snap = viewport_bar->addAction("⌗  Snap");
        snap->setCheckable(true);
        snap->setToolTip("Move snaps to whole world units; Rotate Y snaps to 15°; Scale snaps to 0.1");
        QObject::connect(snap, &QAction::toggled, this, [this](bool enabled) {
            viewport_->SetGridSnap(enabled);
        });
        viewport_bar->addSeparator();
        auto* focus = viewport_bar->addAction("◎  Focus");
        focus->setToolTip("Focus selected entity (F)");
        focus->setShortcut(QKeySequence(Qt::Key_F));
        QObject::connect(focus, &QAction::triggered, this, [this] { viewport_->FocusSelection(); });
        auto* fit = viewport_bar->addAction("▣  Frame All");
        fit->setToolTip("Frame all scene entities (Home)");
        fit->setShortcut(QKeySequence(Qt::Key_Home));
        QObject::connect(fit, &QAction::triggered, this, [this] { viewport_->FitAll(); });
    }

    void ResetWorkspaceLayout() {
        addDockWidget(Qt::LeftDockWidgetArea, hierarchy_dock_);
        addDockWidget(Qt::RightDockWidgetArea, inspector_dock_);
        addDockWidget(Qt::BottomDockWidgetArea, content_dock_);
        addDockWidget(Qt::BottomDockWidgetArea, output_dock_);
        addDockWidget(Qt::BottomDockWidgetArea, sequence_dock_);
        tabifyDockWidget(content_dock_, output_dock_);
        tabifyDockWidget(content_dock_, sequence_dock_);
        hierarchy_dock_->show();
        inspector_dock_->show();
        content_dock_->show();
        output_dock_->show();
        sequence_dock_->show();
        resizeDocks({hierarchy_dock_, inspector_dock_}, {270, 330}, Qt::Horizontal);
        resizeDocks({content_dock_}, {280}, Qt::Vertical);
        content_dock_->raise();
        QSettings settings("Aether", "EditorQt");
        settings.setValue("window/geometry", saveGeometry());
        settings.setValue("window/state", saveState(1));
        statusBar()->showMessage("Workspace layout restored", 3000);
    }

    QMenu* window_menu_ = nullptr;
    QMenu* recent_projects_menu_ = nullptr;
    QDockWidget* hierarchy_dock_ = nullptr;
    QDockWidget* inspector_dock_ = nullptr;
    QDockWidget* content_dock_ = nullptr;
    QDockWidget* output_dock_ = nullptr;
    QDockWidget* sequence_dock_ = nullptr;
    QPushButton* search_button_ = nullptr;
    QPushButton* play_button_ = nullptr;
    QPushButton* pause_button_ = nullptr;
    QPushButton* stop_button_ = nullptr;
    QLabel* inspector_status_ = nullptr;
    QLabel* play_input_hint_ = nullptr;
    QLabel* toolbar_state_badge_ = nullptr;
    QLabel* status_state_badge_ = nullptr;
    QLabel* renderer_badge_ = nullptr;
    QLineEdit* inspector_name_ = nullptr;
    QScrollArea* inspector_scroll_ = nullptr;
    QStackedWidget* inspector_stack_ = nullptr;
    QPushButton* empty_add_entity_button_ = nullptr;
    QPushButton* reset_position_button_ = nullptr;
    QPushButton* reset_rotation_button_ = nullptr;
    QPushButton* reset_scale_button_ = nullptr;
    CameraInspectorWidgets camera_inspector_;
    aether::editor::qt::PhysicsComponentPanel* physics_component_panel_ = nullptr;
#if defined(AETHER_QT_EDITOR_SCRIPTING)
    aether::editor::qt::ScriptComponentEditor* script_component_editor_ = nullptr;
#endif
    QTreeWidget* hierarchy_tree_ = nullptr;
    QLineEdit* hierarchy_filter_ = nullptr;
    QPushButton* add_entity_button_ = nullptr;
    QPushButton* duplicate_entity_button_ = nullptr;
    RhiViewport* viewport_ = nullptr;
    aether::plugin::PluginManager plugin_manager_;
    aether::editor::SceneDocument scene_document_;
    aether::EntityGuid selected_guid_{};
    aether::Entity selected_entity_ = aether::kNullEntity;
    QComboBox* content_breadcrumb_ = nullptr;
    QComboBox* asset_type_filter_ = nullptr;
    QLabel* asset_context_ = nullptr;
    QWidget* content_welcome_panel_ = nullptr;
    QPushButton* welcome_open_project_button_ = nullptr;
    QPushButton* welcome_new_project_button_ = nullptr;
    QAction* save_project_action_ = nullptr;
    QAction* new_scene_action_ = nullptr;
    QAction* open_scene_action_ = nullptr;
    QAction* new_project_action_ = nullptr;
    QAction* open_project_action_ = nullptr;
    QAction* save_scene_action_ = nullptr;
    QAction* undo_action_ = nullptr;
    QAction* redo_action_ = nullptr;
    QAction* create_entity_action_ = nullptr;
    QAction* create_camera_action_ = nullptr;
    QAction* place_model_action_ = nullptr;
    QAction* duplicate_entity_action_ = nullptr;
    QAction* delete_entity_action_ = nullptr;
    QAction* play_action_ = nullptr;
    QAction* pause_action_ = nullptr;
    QAction* stop_action_ = nullptr;
    QFileSystemModel* asset_model_ = nullptr;
    QListView* asset_view_ = nullptr;
    QPushButton* parent_folder_button_ = nullptr;
    QPushButton* new_folder_button_ = nullptr;
    QLineEdit* asset_filter_ = nullptr;
    QPushButton* place_model_button_ = nullptr;
    QString selected_content_path_;
    QString current_content_path_;
    std::array<QDoubleSpinBox*, 3> position_fields_{};
    std::array<QDoubleSpinBox*, 3> rotation_fields_{};
    std::array<QDoubleSpinBox*, 3> scale_fields_{};
    std::filesystem::path project_file_;
    std::filesystem::path recent_projects_file_;
    aether::editor::RecentProjects recent_projects_;
    aether::ProjectSettings project_settings_;
    aether::editor::SequenceDocument sequence_document_;
    std::filesystem::path sequence_file_;
    std::unique_ptr<aether::seq::SequencePlayer> sequence_player_;
    aether::u64 sequence_player_revision_ = 0;
    bool sequence_preview_active_ = false;
    std::unordered_map<aether::EntityGuid, aether::Transform> sequence_preview_transforms_;
    QTimer sequence_timer_;
    QElapsedTimer sequence_clock_;
    QTimer play_simulation_timer_;
    QElapsedTimer play_clock_;
    std::unique_ptr<aether::editor::qt::EditorPlayRuntime> play_runtime_;
    QLineEdit* sequence_name_ = nullptr;
    QDoubleSpinBox* sequence_fps_ = nullptr;
    QDoubleSpinBox* sequence_duration_ = nullptr;
    QLabel* sequence_target_ = nullptr;
    QListWidget* sequence_track_list_ = nullptr;
    QListWidget* sequence_key_list_ = nullptr;
    QSlider* sequence_time_slider_ = nullptr;
    QDoubleSpinBox* sequence_time_ = nullptr;
    QLabel* sequence_diagnostics_ = nullptr;
    QCheckBox* sequence_loop_ = nullptr;
    QPushButton* sequence_new_button_ = nullptr;
    QPushButton* sequence_open_button_ = nullptr;
    QPushButton* sequence_save_button_ = nullptr;
    QPushButton* sequence_add_track_button_ = nullptr;
    QPushButton* sequence_remove_track_button_ = nullptr;
    QPushButton* sequence_set_key_button_ = nullptr;
    QPushButton* sequence_remove_key_button_ = nullptr;
    QPushButton* sequence_play_button_ = nullptr;
    QPushButton* sequence_pause_button_ = nullptr;
    QPushButton* sequence_stop_button_ = nullptr;
};

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    app.setStyleSheet(QString::fromUtf8(kStyle));
    EditorWindow window;
    if (app.arguments().contains("--self-test")) return window.RunSelfTest() ? 0 : 1;
    const QStringList arguments = app.arguments();
    const int project_option = arguments.indexOf("--project");
    if (project_option >= 0 && project_option + 1 < arguments.size())
        window.OpenProjectFile(std::filesystem::path(arguments[project_option + 1].toStdWString()));
    else if (arguments.size() > 1 && arguments[1].endsWith(".aproject", Qt::CaseInsensitive))
        window.OpenProjectFile(std::filesystem::path(arguments[1].toStdWString()));
    window.show();
    return app.exec();
}
