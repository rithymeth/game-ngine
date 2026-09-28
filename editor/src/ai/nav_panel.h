#pragma once

#include "aether/ai/debug_draw.h"

#include <nlohmann/json.hpp>

#include <string>

namespace aether::editor {

// The agent settings a mesh is baked with, as JSON (the panel edits them field by field).
nlohmann::json NavSettingsToJson(const nav::NavMeshSettings& settings);
bool NavSettingsFromJson(const nlohmann::json& json, nav::NavMeshSettings& out, std::string* error = nullptr);

// The Navigation panel (Phase 20 step 6): the bake settings, Bake and
// Rebuild All, the mesh's stats (tiles, polygons, tiles waiting to be
// rebaked, the revision, volumes and links), and the viewport overlay's
// toggles. BuildOverlay produces what the viewport draws: the navmesh,
// its volumes and links, and (with a crowd) agents' ways ahead and
// perception. It fills the current ImGui window and runs headless in tests.
class NavigationPanel {
public:
    NavigationPanel(World& world, nav::NavWorld& nav, const nav::NavCrowd* crowd = nullptr);

    void Draw();
    bool Bake(); // with the panel's settings
    void BuildOverlay(nav::NavDebugDraw& out);

    nav::NavMeshSettings settings;
    nav::NavDebugOptions mesh_options;
    ai::AIDebugOptions ai_options;
    bool show_overlay = true;
    const nav::NavBuildStats& LastBake() const { return stats_; }
    const std::string& Status() const { return status_; }

private:
    World& world_;
    nav::NavWorld& nav_;
    const nav::NavCrowd* crowd_;
    nav::NavBuildStats stats_;
    std::string status_;
};

} // namespace aether::editor
