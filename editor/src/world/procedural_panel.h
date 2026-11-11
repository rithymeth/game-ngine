#pragma once

#include "world/procedural_document.h"

#include <string>

namespace aether::editor {

// The visual generation editor: every setting of the terrain generator and the scatter rules as sliders with the picture of the result beside
// them, presets, a seed box and dice, undo, and the ways out (into the terrain and foliage editors, or baked to files). Fills the current ImGui
// window and runs headless in tests, like the other world panels.
class ProceduralWorldPanel {
public:
    // `terrain` and `foliage` are the editors "Apply" sends the result to; either can be null (its button is then disabled).
    ProceduralWorldPanel(ProceduralWorldDocument& doc, TerrainEditDocument* terrain = nullptr, FoliagePaintDocument* foliage = nullptr)
        : doc_(doc), terrain_(terrain), foliage_(foliage) {}
    void Draw();

    // Regenerates by itself when the settings change and the last generation was small and quick (up to this many milliseconds).
    bool auto_generate = true;
    double auto_generate_budget_ms = 250.0;
    std::string content_root = "Content";
    BakeOptions bake_options;
    usize selected_rule = 0;
    // The last result line shown at the bottom (apply and bake report here).
    const std::string& Status() const { return status_; }
    const BakeReport& LastBake() const { return bake_; }

private:
    void DrawPreview();
    void DrawTerrainSettings(ProceduralWorldSettings& s, bool& changed);
    void DrawRules(ProceduralWorldSettings& s, bool& changed);

    ProceduralWorldDocument& doc_;
    TerrainEditDocument* terrain_;
    FoliagePaintDocument* foliage_;
    bool was_editing_ = false;
    std::string status_;
    BakeReport bake_;
    u64 preview_revision_ = 0;
    std::vector<u8> preview_;
    u32 preview_w_ = 0, preview_h_ = 0;
    usize variant_shown_ = 0;
};

} // namespace aether::editor
