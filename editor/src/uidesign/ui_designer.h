#pragma once

#include "uidesign/ui_layout_document.h"

#include "aether/ui/viewport.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aether::editor {

// The UI Designer (Phase 18 step 6, ROADMAP.md Phase 18): a palette of
// widget types (click to add, or drag onto the canvas), the hierarchy
// (select, drag to reparent or reorder), the canvas (the layout at a
// chosen resolution and safe area, with selection, move and resize handles
// for Canvas children, grid snapping, anchors shown), Details for the
// selection (every property, the slot, anchor presets, bindings), the
// animation timeline (tracks, keys, scrubbing and playback on a preview
// copy), and Diagnostics. It fills the current ImGui window and runs
// headless in tests.

struct ResolutionPreset {
    std::string name;
    ui::Vec2 size;    // pixels
    ui::Margin safe;  // pixel insets (a phone's notch)
};
const std::vector<ResolutionPreset>& ResolutionPresets();

class UIDesigner {
public:
    explicit UIDesigner(UILayoutDocument& document);

    void Draw();

    // --- Selection ----------------------------------------------------------------------
    const std::vector<WidgetPath>& Selection() const { return selection_; }
    void Select(const WidgetPath& path, bool add = false); // add: toggles it in a multi-selection
    void ClearSelection() { selection_.clear(); }
    std::optional<WidgetPath> Primary() const; // the last selected

    // --- The preview --------------------------------------------------------------------
    void SetResolution(usize preset);
    void SetCustomResolution(ui::Vec2 pixels, const ui::Margin& safe = {});
    ui::Vec2 Resolution() const { return viewport_.Size(); }
    ui::ScaleSettings& Scaling() { return viewport_.scale_settings; }
    ui::Rect PreviewArea() const { return viewport_.SafeRect(); } // where the root goes, layout units
    void SetTheme(const ui::Theme* theme) {
        theme_ = theme;
        preview_revision_ = 0;
    }
    // The widget as previewed (theme and animation applied); same paths as the document.
    ui::Widget* PreviewWidget(const WidgetPath& path) const;
    // The deepest visible widget at a layout point (the root if nothing else).
    WidgetPath PickAt(ui::Vec2 layout_point) const;

    // The canvas view: layout units to screen and back (after a Draw).
    ui::Vec2 LayoutToScreen(ui::Vec2 p) const;
    ui::Vec2 ScreenToLayout(ui::Vec2 p) const;
    f32 Zoom() const { return zoom_; }
    void SetZoom(f32 zoom);
    void RequestFit() { fit_ = true; }

    bool snap = true;
    f32 grid = 8.0f; // layout units

    // --- Editing helpers (what the panels do) ------------------------------------------
    // A new widget of `type` at a layout point: into the container under it
    // (or the root), placed there if that's a Canvas; selected.
    std::optional<WidgetPath> PlaceWidget(const std::string& type, ui::Vec2 layout_point);
    void DeleteSelection();
    void DuplicateSelection();
    // Moves the selected Canvas children (layout units), snapped if `snap`.
    void Nudge(ui::Vec2 delta);

    // --- The animation timeline ---------------------------------------------------------
    void SelectAnimation(const std::string& name);
    const std::string& ActiveAnimation() const { return animation_; }
    f32 Playhead() const { return playhead_; }
    void SetPlayhead(f32 time);
    void Play() { playing_ = true; }
    void Stop() { playing_ = false; }
    bool Playing() const { return playing_; }
    // Records a key at the playhead with the widget's current value in the document.
    bool KeyAtPlayhead(usize track);

    bool SaveNow();
    const std::string& Status() const { return status_; }

private:
    enum class Drag : u8 { None, Move, Resize, Pan, Key, Scrub };
    void Refresh(); // rebuild the preview when the document or the playhead changed
    void DrawToolbar();
    void DrawPalette();
    void DrawHierarchy();
    void DrawHierarchyNode(const ui::Widget& w, WidgetPath& path);
    void DrawCanvas();
    void DrawDetails();
    void DrawSlot(const WidgetPath& path);
    void DrawBindings(const WidgetPath& path);
    void DrawTimeline();
    void DrawDiagnostics();
    void HandleKeys();
    void Fit(ui::Vec2 area);
    std::vector<WidgetPath> CanvasSelection() const; // selected widgets placed by rectangle
    int HandleAt(ui::Vec2 screen) const;             // the primary selection's resize handle, or -1
    ui::Rect Snapped(const ui::Rect& r) const;

    UILayoutDocument& doc_;
    std::vector<WidgetPath> selection_;
    ui::Viewport viewport_; // only for sizes and scale; widgets aren't added to it
    const ui::Theme* theme_ = nullptr;
    std::unique_ptr<ui::Widget> preview_;
    u64 preview_revision_ = 0;
    f32 preview_time_ = -1.0f;
    std::string preview_animation_;
    usize preset_ = 0;

    // Canvas view.
    f32 zoom_ = 0.5f;
    ui::Vec2 pan_{20.0f, 20.0f}; // screen offset of the preview's corner within the canvas
    ui::Vec2 origin_;            // the canvas's screen corner
    ui::Vec2 canvas_size_;
    bool fit_ = true;
    Drag drag_ = Drag::None;
    int handle_ = -1;
    ui::Vec2 drag_start_;                 // screen
    std::vector<std::pair<WidgetPath, ui::Rect>> drag_rects_; // at the drag's start
    u64 drag_id_ = 0;

    // Timeline.
    std::string animation_;
    f32 playhead_ = 0.0f;
    bool playing_ = false;
    i64 key_track_ = -1, key_index_ = -1; // the key being dragged
    std::string new_track_widget_, new_track_property_ = "opacity";

    std::string name_edit_;
    WidgetPath name_edit_for_;
    std::string status_;
};

} // namespace aether::editor
