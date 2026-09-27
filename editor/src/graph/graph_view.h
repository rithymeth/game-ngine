#pragma once

#include "aether/core/base.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace aether::editor {

// A reusable node-graph widget (Phase 12 step 6, ROADMAP.md §12.6), built
// only on Dear ImGui so it runs headless in tests. It knows nothing about
// Blueprints: it draws a view model and reports what the user asked for;
// the owner (the Blueprint editor, later the Material and Animation
// editors) applies it. Positions are in canvas units; pan and zoom map them
// to the screen.

struct GraphPinView {
    std::string name;
    std::string label;      // shown next to the pin ("" = none, e.g. an exec pin named "then")
    bool exec = false;      // drawn as a triangle
    bool array = false;     // drawn as a grid
    u32 color = 0xFFFFFFFF; // ImGui IM_COL32 order (ABGR)
    bool connected = false; // filled when connected
};

struct GraphNodeView {
    u32 id = 0;
    std::string title;
    u32 header_color = 0xFF505050;
    float x = 0.0f, y = 0.0f;
    std::vector<GraphPinView> inputs;
    std::vector<GraphPinView> outputs;
    bool pure = false;      // no header strip, rounder look
    bool highlighted = false; // the debugger's current node
    bool breakpoint = false;
    std::string error;      // a compile error on the node (red outline, shown on hover)
    std::string comment;    // a bubble above the node
};

struct GraphLinkView {
    u32 from_node = 0;
    std::string from_pin; // an output
    u32 to_node = 0;
    std::string to_pin;   // an input
    u32 color = 0xFFFFFFFF;
    float glow = 0.0f; // 0..1: the exec trace makes wires glow as they fire
};

// A comment box behind the nodes, identified by its index in `comments`.
struct GraphCommentView {
    std::string text;
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    u32 color = 0x40FFFFFF;
};

struct GraphViewModel {
    std::vector<GraphNodeView> nodes;
    std::vector<GraphLinkView> links;
    std::vector<GraphCommentView> comments;
    const GraphNodeView* Find(u32 id) const;
};

// Pin geometry of a node, in canvas units relative to the node's corner.
struct NodeLayout {
    float width = 0.0f, height = 0.0f;
    float header = 0.0f;
    std::vector<float> input_y, output_y; // pin centers
};
NodeLayout LayoutNode(const GraphNodeView& node, float font_size);

// The canvas rectangle around these nodes (all of them if `nodes` is empty);
// false if none are in the model. Needs an ImGui context (text widths).
bool NodeBounds(const GraphViewModel& model, const std::set<u32>& nodes, float font_size, float& x0, float& y0, float& x1,
                float& y1);

struct GraphPinRef {
    u32 node = 0;
    std::string pin;
    bool output = false;
    bool operator==(const GraphPinRef& o) const { return node == o.node && pin == o.pin && output == o.output; }
};

// Interaction state the owner keeps (one per open graph).
struct GraphViewState {
    float pan_x = 0.0f, pan_y = 0.0f; // canvas point at the view's top-left
    float zoom = 1.0f;                // 0.25 .. 2
    std::set<u32> selection;
    int selected_comment = -1; // a comment box, selected by clicking its title bar
    bool request_fit = false;           // fit every node on the next draw (Home)
    bool request_fit_selection = false; // with request_fit: only the selection (F)

    // In-progress gestures.
    bool dragging_nodes = false;
    bool dragging_link = false;
    GraphPinRef link_from;
    bool box_selecting = false;
    float box_x0 = 0.0f, box_y0 = 0.0f;
    float drag_dx = 0.0f, drag_dy = 0.0f; // node drag so far, canvas units
    int dragging_comment = -1;             // moving a comment box (and the nodes inside it)
    int resizing_comment = -1;             // dragging a comment box's corner
    std::set<u32> comment_nodes;           // the nodes moving with the comment

    // Where the last draw put the canvas (screen space).
    float origin_x = 0.0f, origin_y = 0.0f, width = 0.0f, height = 0.0f;
    float font_size = 13.0f;

    float CanvasToScreenX(float x) const { return origin_x + (x - pan_x) * zoom; }
    float CanvasToScreenY(float y) const { return origin_y + (y - pan_y) * zoom; }
    float ScreenToCanvasX(float x) const { return (x - origin_x) / zoom + pan_x; }
    float ScreenToCanvasY(float y) const { return (y - origin_y) / zoom + pan_y; }
};

struct GraphViewResult {
    std::vector<std::pair<u32, std::pair<float, float>>> moved; // node -> new position (after a drag)
    bool connect = false;
    GraphPinRef connect_from, connect_to; // output -> input, whichever end the drag started from
    bool disconnect = false;              // Alt+click on a wire
    GraphLinkView disconnected;
    std::vector<u32> deleted; // Delete with a selection
    // Palette: right-click on empty canvas, or a link dragged onto empty
    // canvas (then `palette_from` is the pin, for a context-sensitive list).
    bool open_palette = false;
    float palette_x = 0.0f, palette_y = 0.0f; // canvas position for the new node
    bool palette_from_pin = false;
    GraphPinRef palette_from;
    u32 double_clicked = 0; // a node, e.g. to open a function graph
    bool selection_changed = false;

    // Comment boxes: index -> new rectangle (after a move or resize), a
    // Delete with a comment selected, and C with nodes selected (the owner
    // adds a box around them).
    struct CommentRect {
        usize index = 0;
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    };
    std::vector<CommentRect> comments_changed;
    int deleted_comment = -1;
    bool comment_selection = false;

    // Clipboard keys (Ctrl+C/X/V/D), for the owner. Pastes go at the
    // mouse's canvas position.
    bool copy = false, cut = false, paste = false, duplicate = false;
    float mouse_x = 0.0f, mouse_y = 0.0f; // canvas units
};

// Mouse: drag nodes (Ctrl+click adds to the selection), drag from a pin to
// another to link them (or to empty canvas for the palette), drag on empty
// canvas to box-select, Alt+click a wire to break it, right-click for the
// palette, middle-drag to pan, wheel to zoom about the cursor. A comment
// box's title bar selects and moves it with its nodes; its bottom-right
// corner resizes it.
// Keys (while hovered): Delete, Home (fit all), F (frame the selection),
// Tab (palette), C (comment the selection), Ctrl+A, Ctrl+C/X/V/D.
GraphViewResult DrawGraphView(const char* id, const GraphViewModel& model, GraphViewState& state);

// Hit testing (screen space), for the widget and for tests.
bool PinScreenPosition(const GraphViewModel& model, const GraphViewState& state, const GraphPinRef& pin, float& x,
                       float& y);
bool HitTestPin(const GraphViewModel& model, const GraphViewState& state, float x, float y, GraphPinRef& out);
u32 HitTestNode(const GraphViewModel& model, const GraphViewState& state, float x, float y); // 0 = none
// A comment box's title bar or resize corner under (x, y); -1 if none.
int HitTestCommentTitle(const GraphViewModel& model, const GraphViewState& state, float x, float y);
int HitTestCommentCorner(const GraphViewModel& model, const GraphViewState& state, float x, float y);
// The wire under (x, y), within `tolerance` pixels; -1 if none.
int HitTestLink(const GraphViewModel& model, const GraphViewState& state, float x, float y, float tolerance = 5.0f);

// Palette search: the letters of `query` in order within `text`, ignoring
// case and spaces; higher is better, 0 is no match. Prefixes and whole
// substrings rank first.
int FuzzyScore(std::string_view query, std::string_view text);

} // namespace aether::editor
