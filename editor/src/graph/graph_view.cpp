#include "graph/graph_view.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace aether::editor {

namespace {

constexpr float kPad = 8.0f;
constexpr float kPinRadius = 5.0f;
constexpr float kMinZoom = 0.25f, kMaxZoom = 2.0f;
constexpr ImU32 kColBackground = IM_COL32(30, 30, 34, 255);
constexpr ImU32 kColGrid = IM_COL32(45, 45, 52, 255);
constexpr ImU32 kColNodeBody = IM_COL32(40, 40, 44, 235);
constexpr ImU32 kColNodeBorder = IM_COL32(20, 20, 20, 255);
constexpr ImU32 kColSelected = IM_COL32(255, 160, 40, 255);
constexpr ImU32 kColHighlight = IM_COL32(255, 220, 0, 255);
constexpr ImU32 kColError = IM_COL32(230, 40, 40, 255);
constexpr ImU32 kColText = IM_COL32(230, 230, 230, 255);
constexpr ImU32 kColBox = IM_COL32(90, 140, 230, 60);
constexpr ImU32 kColBoxBorder = IM_COL32(90, 140, 230, 200);

float TextWidth(const std::string& text, float font_size) {
    if (text.empty()) return 0.0f;
    return ImGui::CalcTextSize(text.c_str()).x * font_size / ImGui::GetFontSize();
}

ImVec2 Bezier(ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, float t) {
    const float u = 1.0f - t;
    const float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
    return ImVec2(a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y);
}

// A wire's control points: horizontal tangents, longer for longer wires.
void WirePoints(ImVec2 from, ImVec2 to, float zoom, ImVec2& c1, ImVec2& c2) {
    const float dx = std::max(40.0f * zoom, std::fabs(to.x - from.x) * 0.5f);
    c1 = ImVec2(from.x + dx, from.y);
    c2 = ImVec2(to.x - dx, to.y);
}

float SegmentDistance(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float vx = b.x - a.x, vy = b.y - a.y;
    const float len2 = vx * vx + vy * vy;
    float t = len2 > 0.0f ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float dx = a.x + vx * t - p.x, dy = a.y + vy * t - p.y;
    return std::sqrt(dx * dx + dy * dy);
}

const GraphPinView* FindPin(const GraphNodeView& node, const std::string& name, bool output, usize* index = nullptr) {
    const auto& pins = output ? node.outputs : node.inputs;
    for (usize i = 0; i < pins.size(); ++i) {
        if (pins[i].name == name) {
            if (index != nullptr) *index = i;
            return &pins[i];
        }
    }
    return nullptr;
}

} // namespace

const GraphNodeView* GraphViewModel::Find(u32 id) const {
    for (const GraphNodeView& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

NodeLayout LayoutNode(const GraphNodeView& node, float font_size) {
    NodeLayout layout;
    const float row = font_size + 6.0f;
    layout.header = font_size + 8.0f;
    float left = 0.0f, right = 0.0f;
    for (const GraphPinView& p : node.inputs) left = std::max(left, TextWidth(p.label, font_size));
    for (const GraphPinView& p : node.outputs) right = std::max(right, TextWidth(p.label, font_size));
    const float pins = left + right + 4.0f * kPad + 4.0f * kPinRadius;
    layout.width = std::max({TextWidth(node.title, font_size) + 2.0f * kPad, pins, 80.0f});
    const usize rows = std::max(node.inputs.size(), node.outputs.size());
    layout.height = layout.header + static_cast<float>(rows) * row + kPad * 0.5f;
    for (usize i = 0; i < node.inputs.size(); ++i) layout.input_y.push_back(layout.header + row * (static_cast<float>(i) + 0.5f));
    for (usize i = 0; i < node.outputs.size(); ++i) layout.output_y.push_back(layout.header + row * (static_cast<float>(i) + 0.5f));
    return layout;
}

bool NodeBounds(const GraphViewModel& model, const std::set<u32>& nodes, float font_size, float& x0, float& y0, float& x1,
                float& y1) {
    x0 = y0 = 1e30f;
    x1 = y1 = -1e30f;
    bool any = false;
    for (const GraphNodeView& n : model.nodes) {
        if (!nodes.empty() && nodes.count(n.id) == 0) continue;
        const NodeLayout l = LayoutNode(n, font_size);
        x0 = std::min(x0, n.x);
        y0 = std::min(y0, n.y);
        x1 = std::max(x1, n.x + l.width);
        y1 = std::max(y1, n.y + l.height);
        any = true;
    }
    return any;
}

bool PinScreenPosition(const GraphViewModel& model, const GraphViewState& state, const GraphPinRef& pin, float& x,
                       float& y) {
    const GraphNodeView* node = model.Find(pin.node);
    usize index = 0;
    if (node == nullptr || FindPin(*node, pin.pin, pin.output, &index) == nullptr) return false;
    const NodeLayout layout = LayoutNode(*node, state.font_size);
    x = state.CanvasToScreenX(node->x + (pin.output ? layout.width : 0.0f));
    y = state.CanvasToScreenY(node->y + (pin.output ? layout.output_y[index] : layout.input_y[index]));
    return true;
}

bool HitTestPin(const GraphViewModel& model, const GraphViewState& state, float x, float y, GraphPinRef& out) {
    const float radius = (kPinRadius + 4.0f) * state.zoom;
    for (auto node = model.nodes.rbegin(); node != model.nodes.rend(); ++node) { // topmost first
        for (bool output : {false, true}) {
            for (const GraphPinView& p : output ? node->outputs : node->inputs) {
                float px = 0.0f, py = 0.0f;
                const GraphPinRef ref{node->id, p.name, output};
                if (!PinScreenPosition(model, state, ref, px, py)) continue;
                if ((px - x) * (px - x) + (py - y) * (py - y) <= radius * radius) {
                    out = ref;
                    return true;
                }
            }
        }
    }
    return false;
}

u32 HitTestNode(const GraphViewModel& model, const GraphViewState& state, float x, float y) {
    const float cx = state.ScreenToCanvasX(x), cy = state.ScreenToCanvasY(y);
    for (auto node = model.nodes.rbegin(); node != model.nodes.rend(); ++node) {
        const NodeLayout layout = LayoutNode(*node, state.font_size);
        if (cx >= node->x && cx <= node->x + layout.width && cy >= node->y && cy <= node->y + layout.height) {
            return node->id;
        }
    }
    return 0;
}

int HitTestCommentTitle(const GraphViewModel& model, const GraphViewState& state, float x, float y) {
    const float cx = state.ScreenToCanvasX(x), cy = state.ScreenToCanvasY(y);
    const float bar = state.font_size + 8.0f;
    for (usize i = model.comments.size(); i-- > 0;) {
        const GraphCommentView& c = model.comments[i];
        if (cx >= c.x && cx <= c.x + c.width && cy >= c.y && cy <= c.y + bar) return static_cast<int>(i);
    }
    return -1;
}

int HitTestCommentCorner(const GraphViewModel& model, const GraphViewState& state, float x, float y) {
    const float cx = state.ScreenToCanvasX(x), cy = state.ScreenToCanvasY(y);
    const float grip = 12.0f / state.zoom;
    for (usize i = model.comments.size(); i-- > 0;) {
        const GraphCommentView& c = model.comments[i];
        const float rx = c.x + c.width, ry = c.y + c.height;
        if (cx >= rx - grip && cx <= rx && cy >= ry - grip && cy <= ry) return static_cast<int>(i);
    }
    return -1;
}

int HitTestLink(const GraphViewModel& model, const GraphViewState& state, float x, float y, float tolerance) {
    for (usize i = 0; i < model.links.size(); ++i) {
        const GraphLinkView& l = model.links[i];
        float x0, y0, x1, y1;
        if (!PinScreenPosition(model, state, {l.from_node, l.from_pin, true}, x0, y0) ||
            !PinScreenPosition(model, state, {l.to_node, l.to_pin, false}, x1, y1)) {
            continue;
        }
        ImVec2 c1, c2;
        WirePoints(ImVec2(x0, y0), ImVec2(x1, y1), state.zoom, c1, c2);
        ImVec2 prev(x0, y0);
        for (int s = 1; s <= 24; ++s) {
            const ImVec2 p = Bezier(ImVec2(x0, y0), c1, c2, ImVec2(x1, y1), static_cast<float>(s) / 24.0f);
            if (SegmentDistance(ImVec2(x, y), prev, p) <= tolerance) return static_cast<int>(i);
            prev = p;
        }
    }
    return -1;
}

GraphViewResult DrawGraphView(const char* id, const GraphViewModel& model, GraphViewState& state) {
    GraphViewResult result;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::PushID(id);
    ImGui::BeginChild("##graph", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    state.origin_x = origin.x;
    state.origin_y = origin.y;
    state.width = size.x;
    state.height = size.y;
    state.font_size = ImGui::GetFontSize();

    if (state.request_fit && !model.nodes.empty()) {
        state.request_fit = false;
        const bool only_selected = state.request_fit_selection && !state.selection.empty();
        state.request_fit_selection = false;
        float x0, y0, x1, y1;
        if (!NodeBounds(model, only_selected ? state.selection : std::set<u32>{}, state.font_size, x0, y0, x1, y1)) {
            NodeBounds(model, {}, state.font_size, x0, y0, x1, y1);
        }
        const float margin = 40.0f;
        state.zoom = std::clamp(std::min(size.x / (x1 - x0 + 2 * margin), size.y / (y1 - y0 + 2 * margin)), kMinZoom, 1.0f);
        state.pan_x = (x0 + x1) * 0.5f - size.x * 0.5f / state.zoom;
        state.pan_y = (y0 + y1) * 0.5f - size.y * 0.5f / state.zoom;
    }

    ImGui::InvisibleButton("##canvas", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = io.MousePos;

    // --- Input ---------------------------------------------------------------
    if (hovered && io.MouseWheel != 0.0f) {
        const float cx = state.ScreenToCanvasX(mouse.x), cy = state.ScreenToCanvasY(mouse.y);
        state.zoom = std::clamp(state.zoom * std::pow(1.1f, io.MouseWheel), kMinZoom, kMaxZoom);
        state.pan_x = cx - (mouse.x - origin.x) / state.zoom; // keep the point under the cursor
        state.pan_y = cy - (mouse.y - origin.y) / state.zoom;
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                                  ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        state.pan_x -= io.MouseDelta.x / state.zoom;
        state.pan_y -= io.MouseDelta.y / state.zoom;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        GraphPinRef pin;
        const u32 node = HitTestNode(model, state, mouse.x, mouse.y);
        if (io.KeyAlt) {
            const int link = HitTestLink(model, state, mouse.x, mouse.y);
            if (link >= 0) {
                result.disconnect = true;
                result.disconnected = model.links[static_cast<usize>(link)];
            }
        } else if (HitTestPin(model, state, mouse.x, mouse.y, pin)) {
            state.dragging_link = true;
            state.link_from = pin;
        } else if (node != 0) {
            if (io.KeyCtrl) {
                if (!state.selection.erase(node)) state.selection.insert(node);
            } else if (state.selection.count(node) == 0) {
                state.selection = {node};
            }
            result.selection_changed = true;
            state.selected_comment = -1;
            state.dragging_nodes = true;
            state.drag_dx = state.drag_dy = 0.0f;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) result.double_clicked = node;
        } else if (const int corner = HitTestCommentCorner(model, state, mouse.x, mouse.y); corner >= 0) {
            state.resizing_comment = corner;
            state.selected_comment = corner;
            state.drag_dx = state.drag_dy = 0.0f;
        } else if (const int title = HitTestCommentTitle(model, state, mouse.x, mouse.y); title >= 0) {
            // Selecting a comment box replaces the node selection; dragging
            // it takes the nodes wholly inside it along.
            state.dragging_comment = title;
            state.selected_comment = title;
            if (!state.selection.empty()) {
                state.selection.clear();
            }
            result.selection_changed = true;
            state.drag_dx = state.drag_dy = 0.0f;
            state.comment_nodes.clear();
            const GraphCommentView& c = model.comments[static_cast<usize>(title)];
            for (const GraphNodeView& n : model.nodes) {
                const NodeLayout l = LayoutNode(n, state.font_size);
                if (n.x >= c.x && n.y >= c.y && n.x + l.width <= c.x + c.width && n.y + l.height <= c.y + c.height) {
                    state.comment_nodes.insert(n.id);
                }
            }
        } else {
            state.selected_comment = -1;
            state.box_selecting = true;
            state.box_x0 = state.ScreenToCanvasX(mouse.x);
            state.box_y0 = state.ScreenToCanvasY(mouse.y);
            if (!io.KeyCtrl && !state.selection.empty()) {
                state.selection.clear();
                result.selection_changed = true;
            }
        }
    }
    if ((state.dragging_nodes || state.dragging_comment >= 0 || state.resizing_comment >= 0) &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        state.drag_dx += io.MouseDelta.x / state.zoom;
        state.drag_dy += io.MouseDelta.y / state.zoom;
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (state.dragging_link) {
            GraphPinRef target;
            if (HitTestPin(model, state, mouse.x, mouse.y, target)) {
                if (target.node != state.link_from.node && target.output != state.link_from.output) {
                    result.connect = true;
                    result.connect_from = state.link_from.output ? state.link_from : target;
                    result.connect_to = state.link_from.output ? target : state.link_from;
                }
            } else if (hovered) {
                result.open_palette = true;
                result.palette_from_pin = true;
                result.palette_from = state.link_from;
                result.palette_x = state.ScreenToCanvasX(mouse.x);
                result.palette_y = state.ScreenToCanvasY(mouse.y);
            }
        }
        if (state.dragging_nodes && (state.drag_dx != 0.0f || state.drag_dy != 0.0f)) {
            for (u32 selected : state.selection) {
                if (const GraphNodeView* n = model.Find(selected)) {
                    result.moved.push_back({selected, {n->x + state.drag_dx, n->y + state.drag_dy}});
                }
            }
        }
        if (state.dragging_comment >= 0 && static_cast<usize>(state.dragging_comment) < model.comments.size() &&
            (state.drag_dx != 0.0f || state.drag_dy != 0.0f)) {
            const GraphCommentView& c = model.comments[static_cast<usize>(state.dragging_comment)];
            result.comments_changed.push_back(
                {static_cast<usize>(state.dragging_comment), c.x + state.drag_dx, c.y + state.drag_dy, c.width, c.height});
            for (u32 inside : state.comment_nodes) {
                if (const GraphNodeView* n = model.Find(inside)) {
                    result.moved.push_back({inside, {n->x + state.drag_dx, n->y + state.drag_dy}});
                }
            }
        }
        if (state.resizing_comment >= 0 && static_cast<usize>(state.resizing_comment) < model.comments.size() &&
            (state.drag_dx != 0.0f || state.drag_dy != 0.0f)) {
            const GraphCommentView& c = model.comments[static_cast<usize>(state.resizing_comment)];
            result.comments_changed.push_back({static_cast<usize>(state.resizing_comment), c.x, c.y,
                                               std::max(60.0f, c.width + state.drag_dx),
                                               std::max(40.0f, c.height + state.drag_dy)});
        }
        if (state.box_selecting) {
            const float x0 = std::min(state.box_x0, state.ScreenToCanvasX(mouse.x));
            const float x1 = std::max(state.box_x0, state.ScreenToCanvasX(mouse.x));
            const float y0 = std::min(state.box_y0, state.ScreenToCanvasY(mouse.y));
            const float y1 = std::max(state.box_y0, state.ScreenToCanvasY(mouse.y));
            if (x1 - x0 > 1.0f || y1 - y0 > 1.0f) {
                for (const GraphNodeView& n : model.nodes) {
                    const NodeLayout l = LayoutNode(n, state.font_size);
                    if (n.x < x1 && n.x + l.width > x0 && n.y < y1 && n.y + l.height > y0) state.selection.insert(n.id);
                }
                result.selection_changed = true;
            }
        }
        state.dragging_link = state.dragging_nodes = state.box_selecting = false;
        state.dragging_comment = state.resizing_comment = -1;
        state.comment_nodes.clear();
        state.drag_dx = state.drag_dy = 0.0f;
    }
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f) &&
        HitTestNode(model, state, mouse.x, mouse.y) == 0) {
        result.open_palette = true;
        result.palette_x = state.ScreenToCanvasX(mouse.x);
        result.palette_y = state.ScreenToCanvasY(mouse.y);
    }
    result.mouse_x = state.ScreenToCanvasX(mouse.x);
    result.mouse_y = state.ScreenToCanvasY(mouse.y);
    if (hovered) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            if (!state.selection.empty()) {
                result.deleted.assign(state.selection.begin(), state.selection.end());
                state.selection.clear();
            } else if (state.selected_comment >= 0) {
                result.deleted_comment = state.selected_comment;
                state.selected_comment = -1;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) state.request_fit = true;
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F) && !state.selection.empty()) {
            state.request_fit = state.request_fit_selection = true;
        }
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && !state.selection.empty()) result.comment_selection = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
            result.open_palette = true;
            result.palette_x = result.mouse_x;
            result.palette_y = result.mouse_y;
        }
        if (io.KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_A)) {
                for (const GraphNodeView& n : model.nodes) state.selection.insert(n.id);
                result.selection_changed = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_C) && !state.selection.empty()) result.copy = true;
            if (ImGui::IsKeyPressed(ImGuiKey_X) && !state.selection.empty()) result.cut = true;
            if (ImGui::IsKeyPressed(ImGuiKey_V)) result.paste = true;
            if (ImGui::IsKeyPressed(ImGuiKey_D) && !state.selection.empty()) result.duplicate = true;
        }
    }

    // --- Drawing ---------------------------------------------------------------
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), kColBackground);
    const float grid = 32.0f * state.zoom;
    if (grid > 6.0f) {
        for (float x = std::fmod(-state.pan_x * state.zoom, grid); x < size.x; x += grid)
            draw->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y), kColGrid);
        for (float y = std::fmod(-state.pan_y * state.zoom, grid); y < size.y; y += grid)
            draw->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y), kColGrid);
    }
    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

    auto offset = [&](const GraphNodeView& n) {
        const bool moving = (state.dragging_nodes && state.selection.count(n.id) != 0) ||
                            (state.dragging_comment >= 0 && state.comment_nodes.count(n.id) != 0);
        return ImVec2(n.x + (moving ? state.drag_dx : 0.0f), n.y + (moving ? state.drag_dy : 0.0f));
    };
    auto pin_pos = [&](const GraphNodeView& n, const NodeLayout& l, bool output, usize i) {
        const ImVec2 at = offset(n);
        return ImVec2(state.CanvasToScreenX(at.x + (output ? l.width : 0.0f)),
                      state.CanvasToScreenY(at.y + (output ? l.output_y[i] : l.input_y[i])));
    };
    auto pin_of = [&](u32 node_id, const std::string& name, bool output, ImVec2& at) {
        const GraphNodeView* n = model.Find(node_id);
        usize i = 0;
        if (n == nullptr || FindPin(*n, name, output, &i) == nullptr) return false;
        at = pin_pos(*n, LayoutNode(*n, state.font_size), output, i);
        return true;
    };

    ImFont* font = ImGui::GetFont();
    for (usize i = 0; i < model.comments.size(); ++i) {
        const GraphCommentView& c = model.comments[i];
        float x = c.x, y = c.y, w = c.width, h = c.height;
        if (state.dragging_comment == static_cast<int>(i)) x += state.drag_dx, y += state.drag_dy;
        if (state.resizing_comment == static_cast<int>(i)) {
            w = std::max(60.0f, w + state.drag_dx);
            h = std::max(40.0f, h + state.drag_dy);
        }
        const ImVec2 p0(state.CanvasToScreenX(x), state.CanvasToScreenY(y));
        const ImVec2 p1(state.CanvasToScreenX(x + w), state.CanvasToScreenY(y + h));
        const float bar = (state.font_size + 8.0f) * state.zoom;
        draw->AddRectFilled(p0, p1, c.color, 4.0f * state.zoom);
        draw->AddRectFilled(p0, ImVec2(p1.x, p0.y + bar), (c.color & 0x00FFFFFFu) | 0xA0000000u, 4.0f * state.zoom,
                            ImDrawFlags_RoundCornersTop);
        draw->AddText(font, state.font_size * state.zoom, ImVec2(p0.x + kPad * state.zoom, p0.y + 4.0f * state.zoom), kColText,
                      c.text.c_str());
        draw->AddRect(p0, p1, state.selected_comment == static_cast<int>(i) ? kColSelected : IM_COL32(0, 0, 0, 90),
                      4.0f * state.zoom, 0, (state.selected_comment == static_cast<int>(i) ? 2.0f : 1.0f) * state.zoom);
        const float g = 10.0f * state.zoom;
        draw->AddTriangleFilled(ImVec2(p1.x - g, p1.y), ImVec2(p1.x, p1.y - g), p1, IM_COL32(200, 200, 200, 120));
    }

    for (const GraphLinkView& l : model.links) {
        ImVec2 a, b, c1, c2;
        if (!pin_of(l.from_node, l.from_pin, true, a) || !pin_of(l.to_node, l.to_pin, false, b)) continue;
        WirePoints(a, b, state.zoom, c1, c2);
        const float thickness = (2.0f + 3.0f * l.glow) * state.zoom;
        if (l.glow > 0.0f) draw->AddBezierCubic(a, c1, c2, b, IM_COL32(255, 255, 255, static_cast<int>(120 * l.glow)), thickness * 2.5f);
        draw->AddBezierCubic(a, c1, c2, b, l.color, thickness);
    }

    for (const GraphNodeView& n : model.nodes) {
        const NodeLayout l = LayoutNode(n, state.font_size);
        const ImVec2 at = offset(n);
        const ImVec2 p0(state.CanvasToScreenX(at.x), state.CanvasToScreenY(at.y));
        const ImVec2 p1(p0.x + l.width * state.zoom, p0.y + l.height * state.zoom);
        const float round = (n.pure ? 10.0f : 5.0f) * state.zoom;
        draw->AddRectFilled(p0, p1, kColNodeBody, round);
        draw->AddRectFilled(p0, ImVec2(p1.x, p0.y + l.header * state.zoom), n.header_color, round, ImDrawFlags_RoundCornersTop);
        const float text_size = state.font_size * state.zoom;
        draw->AddText(font, text_size, ImVec2(p0.x + kPad * state.zoom, p0.y + 4.0f * state.zoom), kColText, n.title.c_str());
        ImU32 border = kColNodeBorder;
        float border_w = 1.0f;
        if (!n.error.empty()) border = kColError, border_w = 2.0f;
        if (state.selection.count(n.id)) border = kColSelected, border_w = 2.0f;
        if (n.highlighted) border = kColHighlight, border_w = 3.0f;
        draw->AddRect(p0, p1, border, round, 0, border_w * state.zoom);
        if (n.breakpoint) draw->AddCircleFilled(ImVec2(p0.x, p0.y), 6.0f * state.zoom, kColError);
        if (!n.comment.empty()) {
            draw->AddText(font, text_size, ImVec2(p0.x, p0.y - text_size - 4.0f * state.zoom), IM_COL32(200, 200, 200, 255),
                          n.comment.c_str());
        }
        for (bool output : {false, true}) {
            const auto& pins = output ? n.outputs : n.inputs;
            for (usize i = 0; i < pins.size(); ++i) {
                const GraphPinView& p = pins[i];
                const ImVec2 c = pin_pos(n, l, output, i);
                const float r = kPinRadius * state.zoom;
                if (p.exec) {
                    const ImVec2 t0(c.x - r, c.y - r), t1(c.x + r, c.y), t2(c.x - r, c.y + r);
                    if (p.connected) draw->AddTriangleFilled(t0, t1, t2, p.color);
                    else draw->AddTriangle(t0, t1, t2, p.color, 1.5f * state.zoom);
                } else if (p.array) {
                    const float q = r * 0.45f;
                    for (int gx = -1; gx <= 1; ++gx)
                        for (int gy = -1; gy <= 1; ++gy)
                            draw->AddRectFilled(ImVec2(c.x + gx * q * 2 - q * 0.8f, c.y + gy * q * 2 - q * 0.8f),
                                                ImVec2(c.x + gx * q * 2 + q * 0.8f, c.y + gy * q * 2 + q * 0.8f), p.color);
                } else if (p.connected) {
                    draw->AddCircleFilled(c, r, p.color);
                } else {
                    draw->AddCircle(c, r, p.color, 0, 1.5f * state.zoom);
                }
                if (!p.label.empty()) {
                    const float w = TextWidth(p.label, state.font_size) * state.zoom;
                    const float x = output ? c.x - r - 4.0f * state.zoom - w : c.x + r + 4.0f * state.zoom;
                    draw->AddText(font, text_size, ImVec2(x, c.y - text_size * 0.5f), kColText, p.label.c_str());
                }
            }
        }
        if (!n.error.empty() && hovered && HitTestNode(model, state, mouse.x, mouse.y) == n.id) {
            ImGui::SetTooltip("%s", n.error.c_str());
        }
    }

    if (state.dragging_link) {
        ImVec2 a;
        if (pin_of(state.link_from.node, state.link_from.pin, state.link_from.output, a)) {
            ImVec2 from = state.link_from.output ? a : mouse, to = state.link_from.output ? mouse : a, c1, c2;
            WirePoints(from, to, state.zoom, c1, c2);
            draw->AddBezierCubic(from, c1, c2, to, IM_COL32(255, 255, 255, 180), 2.0f * state.zoom);
        }
    }
    if (state.box_selecting) {
        const ImVec2 b0(state.CanvasToScreenX(state.box_x0), state.CanvasToScreenY(state.box_y0));
        draw->AddRectFilled(ImVec2(std::min(b0.x, mouse.x), std::min(b0.y, mouse.y)),
                            ImVec2(std::max(b0.x, mouse.x), std::max(b0.y, mouse.y)), kColBox);
        draw->AddRect(ImVec2(std::min(b0.x, mouse.x), std::min(b0.y, mouse.y)),
                      ImVec2(std::max(b0.x, mouse.x), std::max(b0.y, mouse.y)), kColBoxBorder);
    }
    draw->PopClipRect();
    ImGui::EndChild();
    ImGui::PopID();
    return result;
}

} // namespace aether::editor
