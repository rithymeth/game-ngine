#pragma once

#include "core/scene_document.h"
#include "aether/nav/components.h"
#include "aether/nav/debug_draw.h"
#include "aether/ai/debug_draw.h"

#include <QWidget>

#include <filesystem>
#include <memory>
#include <functional>
#include <unordered_map>

class QLabel;
class QDoubleSpinBox;
class QSpinBox;

namespace aether::editor::qt {

class NavigationEditorWidget final : public QWidget {
public:
    explicit NavigationEditorWidget(SceneDocument& document, QWidget* parent = nullptr);
    void SetContentRoot(const std::filesystem::path& root);
    void ResetForScene();
    void RefreshStats();
    nav::NavDebugDraw DebugDraw(const nav::NavCrowd* crowd = nullptr) const;
    nav::NavWorld* RuntimeNavWorld() const { return nav_world_.get(); }
    void SetOverlayChangedCallback(std::function<void()> callback) { overlay_changed_ = std::move(callback); }

private:
    bool ReadModel(const ModelRenderer& model, std::vector<Vec3>& vertices, std::vector<u32>& indices);
    void Bake();
    void ApplySettings();
    void CreateNavWorld();
    SceneDocument& document_;
    std::filesystem::path content_root_;
    std::unordered_map<std::string, std::pair<std::vector<Vec3>, std::vector<u32>>> model_cache_;
    std::unique_ptr<nav::NavWorld> nav_world_;
    nav::NavMeshSettings settings_;
    QLabel* status_ = nullptr;
    QLabel* stats_ = nullptr;
    QDoubleSpinBox* cell_size_ = nullptr;
    QDoubleSpinBox* agent_height_ = nullptr;
    QDoubleSpinBox* agent_radius_ = nullptr;
    QDoubleSpinBox* max_slope_ = nullptr;
    QDoubleSpinBox* max_climb_ = nullptr;
    QSpinBox* tile_size_ = nullptr;
    bool show_polygons_ = true;
    bool show_edges_ = true;
    bool show_tiles_ = false;
    bool show_links_ = true;
    bool show_volumes_ = true;
    ai::AIDebugOptions ai_options_;
    std::function<void()> overlay_changed_;
};

} // namespace aether::editor::qt
