#pragma once

#include "audio/cue_document.h"
#include "graph/graph_view.h"

#include "aether/audio/cue_player.h"

#include <string>
#include <vector>

namespace aether::editor {

// The sound cue editor (Phase 17 step 6, ROADMAP.md Phase 17): the cue's
// graph drawn with the Phase 12 graph widget (each node's inputs on the
// left, its output on the right, and the cue's Output), Details for the
// selected node or the output's settings (volume, bus, 3D and attenuation
// with its curve), Diagnostics, and preview playback. It fills the current
// ImGui window and runs headless in tests.

constexpr u32 kCueOutputNode = 0xFFFFFF10u;

// Input pins: "in" for one-input nodes; "in0".."inN-1" and an empty "add"
// for multi-input nodes (dropping a link on it adds an input); none for Waves.
std::vector<std::string> CueInputPins(const audio::CueNode& node);
// The input index a pin names on `node`, or -1 (for "add": one past the end).
i32 CueInputIndex(const audio::CueNode& node, const std::string& pin);
std::string CueNodeTitle(const audio::CueNode& node);

GraphViewModel BuildCueGraphView(const audio::SoundCue& cue, const std::vector<audio::CueDiagnostic>& diagnostics, f32 output_x,
                                 f32 output_y);
// Apply what the widget reported; the Output node's moves go to `output_x/y`.
// The reasons for refused edits come back.
std::vector<std::string> ApplyCueGraphEdits(SoundCueDocument& doc, const GraphViewResult& edits, f32& output_x, f32& output_y);

// The attenuation's gain at `samples` distances from 0 to `range` (the Details plot).
std::vector<f32> AttenuationCurve(const audio::AttenuationSettings& settings, u32 samples, f32 range);

class SoundCueEditor {
public:
    explicit SoundCueEditor(SoundCueDocument& document);

    void Draw();

    GraphViewState& View() { return view_; }
    // 0: nothing; kCueOutputNode: the output's settings; otherwise a node.
    u32 Selected() const { return selected_; }
    void Select(u32 id);

    // Preview through a cue player (optional): plays the cue as edited.
    void SetPreview(audio::CuePlayer* player) { player_ = player; }
    bool PlayPreview();
    void StopPreview();
    bool Previewing() const;

    // The add-node menu, and placing a node at its point.
    void OpenPalette(f32 x, f32 y);
    bool PaletteOpen() const { return palette_open_; }
    u32 PlaceNode(audio::CueNodeType type, const std::string& sound = {});

    f32 OutputX() const { return output_x_; }
    f32 OutputY() const { return output_y_; }
    bool SaveNow();
    const std::string& Status() const { return status_; }

private:
    void DrawToolbar();
    void DrawGraph();
    void DrawDetails();
    void DrawNodeDetails(const audio::CueNode& node);
    void DrawOutputDetails();
    void DrawDiagnostics();
    void DrawPalette();
    void HandleKeys();
    void PlaceOutput();

    SoundCueDocument& doc_;
    GraphViewState view_;
    u32 selected_ = 0;
    f32 output_x_ = 0.0f, output_y_ = 0.0f;
    bool palette_open_ = false, palette_popup_ = false;
    f32 palette_x_ = 0.0f, palette_y_ = 0.0f;
    std::string palette_filter_;
    audio::CuePlayer* player_ = nullptr;
    audio::CueHandle preview_ = 0;
    std::string status_;
};

} // namespace aether::editor
