#include "navigation_editor_widget.h"

#include "aether/assets/gltf_loader.h"
#include "aether/scene/components.h"

#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace aether::editor::qt {

NavigationEditorWidget::NavigationEditorWidget(SceneDocument& document, QWidget* parent)
    : QWidget(parent), document_(document) {
    CreateNavWorld();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    auto* intro = new QLabel("Bake walkable surfaces from static mesh entities in the active scene. Nav obstacles, area volumes, and links are read from their scene components.", this);
    intro->setWordWrap(true);
    intro->setObjectName("panelDescription");
    root->addWidget(intro);

    auto* settings_group = new QGroupBox("Bake settings", this);
    auto* form = new QFormLayout(settings_group);
    auto make_float = [settings_group](double value, double min, double max, int decimals) {
        auto* control = new QDoubleSpinBox(settings_group);
        control->setRange(min, max);
        control->setDecimals(decimals);
        control->setSingleStep(decimals == 0 ? 1.0 : 0.05);
        control->setValue(value);
        return control;
    };
    cell_size_ = make_float(settings_.cell_size, 0.05, 2.0, 2);
    agent_height_ = make_float(settings_.agent_height, 0.1, 20.0, 2);
    agent_radius_ = make_float(settings_.agent_radius, 0.0, 10.0, 2);
    max_slope_ = make_float(settings_.agent_max_slope, 0.0, 89.0, 1);
    max_climb_ = make_float(settings_.agent_max_climb, 0.0, 10.0, 2);
    tile_size_ = new QSpinBox(settings_group);
    tile_size_->setRange(0, 512);
    tile_size_->setValue(settings_.tile_size);
    tile_size_->setSpecialValueText("Single tile");
    form->addRow("Cell size (m)", cell_size_);
    form->addRow("Agent height (m)", agent_height_);
    form->addRow("Agent radius (m)", agent_radius_);
    form->addRow("Maximum slope (°)", max_slope_);
    form->addRow("Maximum climb (m)", max_climb_);
    form->addRow("Tile size (cells)", tile_size_);
    root->addWidget(settings_group);

    auto* actions = new QHBoxLayout;
    auto* bake_button = new QPushButton("Bake Navigation", this);
    auto* rebuild_button = new QPushButton("Rebuild all tiles", this);
    actions->addWidget(bake_button);
    actions->addWidget(rebuild_button);
    root->addLayout(actions);
    status_ = new QLabel("Not baked", this);
    status_->setWordWrap(true);
    root->addWidget(status_);
    auto* mesh_group = new QGroupBox("Navigation mesh", this);
    auto* mesh_layout = new QVBoxLayout(mesh_group);
    stats_ = new QLabel("No mesh loaded", mesh_group);
    stats_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mesh_layout->addWidget(stats_);
    root->addWidget(mesh_group);
    auto* overlay_group = new QGroupBox("Viewport overlay", this);
    auto* overlay_layout = new QVBoxLayout(overlay_group);
    auto add_overlay_toggle = [this, overlay_layout](const QString& text, bool* value) {
        auto* toggle = new QCheckBox(text, this);
        toggle->setChecked(*value);
        overlay_layout->addWidget(toggle);
        connect(toggle, &QCheckBox::toggled, this, [this, value](bool checked) {
            *value = checked;
            if (overlay_changed_) overlay_changed_();
        });
    };
    add_overlay_toggle("Walkable polygons", &show_polygons_);
    add_overlay_toggle("Polygon edges", &show_edges_);
    add_overlay_toggle("Tile bounds", &show_tiles_);
    add_overlay_toggle("Off-mesh links", &show_links_);
    add_overlay_toggle("Obstacle and area volumes", &show_volumes_);
    add_overlay_toggle("Agent paths and goals", &ai_options_.agent_paths);
    add_overlay_toggle("Sight cones", &ai_options_.sight);
    add_overlay_toggle("Hearing radius", &ai_options_.hearing);
    add_overlay_toggle("Known and remembered actors", &ai_options_.known);
    root->addWidget(overlay_group);
    root->addStretch(1);

    connect(bake_button, &QPushButton::clicked, this, [this] { Bake(); });
    connect(rebuild_button, &QPushButton::clicked, this, [this] {
        if (!nav_world_->Ready()) return;
        nav_world_->Dynamic().MarkAllDirty();
        status_->setText("Rebuilding all dirty navigation tiles…");
        RefreshStats();
    });
    auto* timer = new QTimer(this);
    timer->setInterval(250);
    connect(timer, &QTimer::timeout, this, [this] {
        if (nav_world_->Ready() && nav_world_->Dynamic().PendingTiles() > 0) {
            nav_world_->Update(4);
            RefreshStats();
            if (overlay_changed_) overlay_changed_();
        }
    });
    timer->start();
}

void NavigationEditorWidget::SetContentRoot(const std::filesystem::path& root) {
    content_root_ = root;
    model_cache_.clear();
}

void NavigationEditorWidget::CreateNavWorld() {
    nav_world_ = std::make_unique<nav::NavWorld>(document_.GetWorld(),
        [this](const ModelRenderer& model, std::vector<Vec3>& vertices, std::vector<u32>& indices) {
            return ReadModel(model, vertices, indices);
        }, &document_.Guids());
    nav_world_->MakeActive();
}

void NavigationEditorWidget::ResetForScene() {
    model_cache_.clear();
    CreateNavWorld();
    status_->setText("Scene changed · bake navigation to refresh the mesh");
    RefreshStats();
}

bool NavigationEditorWidget::ReadModel(const ModelRenderer& model, std::vector<Vec3>& vertices,
                                       std::vector<u32>& indices) {
    if (model.asset_path[0] == '\0') return false;
    std::filesystem::path path(model.asset_path);
    if (path.is_relative()) path = content_root_ / path;
    path = path.lexically_normal();
    const std::string key = path.generic_string();
    auto found = model_cache_.find(key);
    if (found == model_cache_.end()) {
        assets::GltfScene scene;
        if (!assets::LoadGltf(path.string(), scene)) return false;
        std::pair<std::vector<Vec3>, std::vector<u32>> geometry;
        auto append_mesh = [&](usize mesh_index, const Mat4& transform) {
            if (mesh_index >= scene.meshes.size()) return;
            for (const auto& primitive : scene.meshes[mesh_index].primitives) {
                const u32 base = static_cast<u32>(geometry.first.size());
                for (const auto& vertex : primitive.vertices) {
                    const Vec4 p = transform * Vec4(vertex.position[0], vertex.position[1], vertex.position[2], 1.0f);
                    geometry.first.emplace_back(p.x, p.y, p.z);
                }
                for (u32 index : primitive.indices)
                    if (index < primitive.vertices.size()) geometry.second.push_back(base + index);
            }
        };
        if (scene.node_instances.empty()) {
            for (usize mesh = 0; mesh < scene.meshes.size(); ++mesh) append_mesh(mesh, Mat4::Identity());
        } else {
            for (const auto& instance : scene.node_instances) append_mesh(instance.mesh_index, instance.world_transform);
        }
        found = model_cache_.emplace(key, std::move(geometry)).first;
    }
    vertices = found->second.first;
    indices = found->second.second;
    return !vertices.empty() && !indices.empty();
}

void NavigationEditorWidget::ApplySettings() {
    settings_.cell_size = static_cast<f32>(cell_size_->value());
    settings_.agent_height = static_cast<f32>(agent_height_->value());
    settings_.agent_radius = static_cast<f32>(agent_radius_->value());
    settings_.agent_max_slope = static_cast<f32>(max_slope_->value());
    settings_.agent_max_climb = static_cast<f32>(max_climb_->value());
    settings_.tile_size = tile_size_->value();
}

void NavigationEditorWidget::Bake() {
    ApplySettings();
    model_cache_.clear();
    nav::NavBuildStats result;
    std::string error;
    if (!nav_world_->Bake(settings_, &error, &result)) {
        status_->setText(QString("Bake failed: %1").arg(QString::fromStdString(error)));
        RefreshStats();
        return;
    }
    status_->setText(QString("Baked %1 tiles and %2 polygons in %3 ms.")
                         .arg(static_cast<qulonglong>(result.tiles))
                         .arg(static_cast<qulonglong>(result.polygons))
                         .arg(result.milliseconds, 0, 'f', 1));
    RefreshStats();
    if (overlay_changed_) overlay_changed_();
}

nav::NavDebugDraw NavigationEditorWidget::DebugDraw(const nav::NavCrowd* crowd) const {
    nav::NavDebugDraw draw;
    if (nav_world_ && nav_world_->Ready()) {
        nav::NavDebugOptions options;
        options.polygons = show_polygons_;
        options.edges = show_edges_;
        options.tile_bounds = show_tiles_;
        options.links = show_links_;
        options.volumes = show_volumes_;
        nav::DrawDynamicNavMesh(nav_world_->Dynamic(), options, draw);
    }
    ai::DrawAIDebug(document_.GetWorld(), crowd, ai_options_, draw);
    return draw;
}

void NavigationEditorWidget::RefreshStats() {
    if (!nav_world_->Ready()) {
        stats_->setText("No mesh loaded");
        return;
    }
    const auto& dynamic = nav_world_->Dynamic();
    const auto& mesh = dynamic.Mesh();
    stats_->setText(QString("Tiles  %1  (%2 × %3)\nPolygons  %4\nWaiting to rebake  %5\nTiles rebuilt  %6\nRevision  %7\nVolumes  %8    Links  %9")
        .arg(static_cast<qulonglong>(mesh.TileCount()))
        .arg(mesh.Data().tiles_x).arg(mesh.Data().tiles_z)
        .arg(static_cast<qulonglong>(mesh.PolygonCount()))
        .arg(static_cast<qulonglong>(dynamic.PendingTiles()))
        .arg(static_cast<qulonglong>(dynamic.TilesRebuilt()))
        .arg(static_cast<qulonglong>(dynamic.Revision()))
        .arg(static_cast<qulonglong>(dynamic.VolumeCount() + dynamic.Geometry().volumes.size()))
        .arg(static_cast<qulonglong>(mesh.Links().size())));
}

} // namespace aether::editor::qt
