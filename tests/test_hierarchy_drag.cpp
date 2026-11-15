// Dragging several entities in the hierarchy (parity Stage 4): a drag from a selected row carries the whole selection, a drop
// reparents all of it as one undo step, and a carried entity whose ancestor is carried too keeps its own parent. Headless.

#include "world/scene_panel.h"
#include "test_framework.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::editor;

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400, 900);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    template <typename F>
    void Frame(F&& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1400, 900));
        ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

// The Scene tool, with a probe row above it whose drag carries a payload nobody takes: carrying it down the hierarchy finds where each
// row is without dropping anything.
struct DragRig {
    HeadlessImGui imgui;
    ScenePanel panel;
    DragRig() { Step(ImVec2(0, 0), false); }
    void Step(const ImVec2& mouse, bool down) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(0, down);
        imgui.Frame([&] {
            ImGui::SetCursorScreenPos(ImVec2(10, 10));
            ImGui::Button("probe", ImVec2(120, 24));
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload("TEST_PROBE", "x", 2);
                ImGui::EndDragDropSource();
            }
            ImGui::SetCursorScreenPos(ImVec2(10, 50));
            panel.Draw();
        });
    }
    // The y of the middle of each drop target, top to bottom: the heading, then each row in the order drawn.
    std::vector<f32> Targets() {
        Step(ImVec2(20, 20), false);
        Step(ImVec2(20, 20), true);
        Step(ImVec2(50, 20), true);
        std::vector<f32> found;
        f32 last_top = -1.0f;
        for (f32 y = 60.0f; y < 330.0f; y += 3.0f) {
            Step(ImVec2(80.0f, y), true);
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            if (g.DragDropTargetId == 0 || g.DragDropTargetRect.Min.y == last_top) continue;
            last_top = g.DragDropTargetRect.Min.y;
            found.push_back((g.DragDropTargetRect.Min.y + g.DragDropTargetRect.Max.y) * 0.5f);
        }
        Step(ImVec2(80.0f, 400.0f), false); // let the probe go where nothing takes it
        Step(ImVec2(80.0f, 400.0f), false);
        return found;
    }
};

bool Has(const std::vector<Entity>& set, Entity entity) { return std::find(set.begin(), set.end(), entity) != set.end(); }

} // namespace

AETHER_TEST(HierarchyDrag_ReparentingManyIsOneUndoStep) {
    ScenePanel panel;
    SceneDocument& document = panel.Document();
    const Entity target = document.CreateEntity("Target");
    const Entity a = document.CreateEntity("Crate A");
    const Entity b = document.CreateEntity("Crate B");
    const usize before = document.Commands().UndoCount();
    const std::vector<Entity> carried{a, b};
    AETHER_CHECK(panel.ReparentEntities(carried, target) == 2);
    AETHER_CHECK(document.ParentOf(a) == target && document.ParentOf(b) == target);
    AETHER_CHECK(document.Commands().UndoCount() == before + 1);
    AETHER_CHECK(document.Commands().UndoLabel() == "Parent 2 entities");
    AETHER_CHECK(panel.ReparentEntities(carried, target) == 0); // already there: nothing to undo
    AETHER_CHECK(document.Commands().UndoCount() == before + 1);

    AETHER_CHECK(document.Undo());
    AETHER_CHECK(document.ParentOf(a).IsNull() && document.ParentOf(b).IsNull());
    AETHER_CHECK(document.Redo());
    AETHER_CHECK(panel.ReparentEntities(carried, kNullEntity) == 2);
    AETHER_CHECK(document.Commands().UndoLabel() == "Move 2 entities to the root");
    AETHER_CHECK(document.ParentOf(a).IsNull() && document.ParentOf(b).IsNull());
}

AETHER_TEST(HierarchyDrag_ACarriedChildKeepsItsCarriedParent) {
    ScenePanel panel;
    SceneDocument& document = panel.Document();
    const Entity target = document.CreateEntity("Target");
    const Entity parent = document.CreateEntity("Rack");
    const Entity child = document.CreateEntity("Shelf", SceneEntityKind::Empty, {}, parent);
    const Entity grandchild = document.CreateEntity("Box", SceneEntityKind::Empty, {}, child);
    const std::vector<Entity> carried{grandchild, parent, child};
    AETHER_CHECK(panel.ReparentEntities(carried, target) == 1); // only the top of the carried subtree moves
    AETHER_CHECK(document.ParentOf(parent) == target);
    AETHER_CHECK(document.ParentOf(child) == parent && document.ParentOf(grandchild) == child); // the subtree arrives intact
}

AETHER_TEST(HierarchyDrag_AnEntityCannotGoUnderItsOwnChild) {
    ScenePanel panel;
    SceneDocument& document = panel.Document();
    const Entity parent = document.CreateEntity("Rack");
    const Entity child = document.CreateEntity("Shelf", SceneEntityKind::Empty, {}, parent);
    const Entity loose = document.CreateEntity("Crate");
    const usize before = document.Commands().UndoCount();

    const std::vector<Entity> only_parent{parent};
    AETHER_CHECK(panel.ReparentEntities(only_parent, child) == 0);
    AETHER_CHECK(document.ParentOf(parent).IsNull() && document.Commands().UndoCount() == before);
    AETHER_CHECK(panel.Status().find("cannot go under itself") != std::string::npos);
    AETHER_CHECK(!panel.Reparent(parent, parent));

    // The rest of a drag still lands when one of it cannot.
    const std::vector<Entity> mixed{parent, loose};
    AETHER_CHECK(panel.ReparentEntities(mixed, child) == 1);
    AETHER_CHECK(document.ParentOf(loose) == child && document.ParentOf(parent).IsNull());
    AETHER_CHECK(panel.Status().find("the rest moved") != std::string::npos);
    AETHER_CHECK(document.Commands().UndoCount() == before + 1);
    AETHER_CHECK(document.Undo() && document.ParentOf(loose).IsNull());
}

AETHER_TEST(HierarchyDrag_TheDragSetIsTheSelectionOnlyFromASelectedRow) {
    ScenePanel panel;
    SceneDocument& document = panel.Document();
    const Entity a = document.CreateEntity("Crate A");
    const Entity b = document.CreateEntity("Crate B");
    const Entity c = document.CreateEntity("Lamp");
    document.SetSelection({a, b});
    const std::vector<Entity> from_selected = panel.DragSet(b);
    AETHER_CHECK(from_selected.size() == 2 && Has(from_selected, a) && Has(from_selected, b));
    const std::vector<Entity> from_other = panel.DragSet(c);
    AETHER_CHECK(from_other.size() == 1 && from_other[0] == c);
}

AETHER_TEST(HierarchyDrag_ARealDragOfASelectedRowCarriesTheSelection) {
    DragRig rig;
    SceneDocument& document = rig.panel.Document();
    // Digits sort before letters, so these are the first three rows whatever else the scene holds.
    const Entity a = document.CreateEntity("0 Crate A");
    const Entity b = document.CreateEntity("0 Crate B");
    const Entity target = document.CreateEntity("0 Target");
    document.Commands().Clear();
    const std::vector<f32> rows = rig.Targets();
    AETHER_CHECK(rows.size() >= 4); // the heading, then the three
    if (rows.size() < 4) return;
    const f32 row_a = rows[1], row_target = rows[3];

    document.SetSelection({a, b});
    rig.Step(ImVec2(80.0f, row_a), false);
    rig.Step(ImVec2(80.0f, row_a), true);
    AETHER_CHECK(document.Selection().size() == 2); // pressing a selected row keeps the selection, so the press can drag it
    rig.Step(ImVec2(120.0f, row_a), true);
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    AETHER_CHECK(payload != nullptr && payload->DataSize == static_cast<int>(2 * sizeof(Entity)));
    rig.Step(ImVec2(80.0f, row_target), true);
    rig.Step(ImVec2(80.0f, row_target), true);
    rig.Step(ImVec2(80.0f, row_target), false);
    AETHER_CHECK(document.ParentOf(a) == target && document.ParentOf(b) == target);
    AETHER_CHECK(document.Commands().UndoCount() == 1);
    AETHER_CHECK(document.Selection().size() == 2); // a drag is not a click: the selection stays
    rig.Step(ImVec2(80.0f, 400.0f), false);
    AETHER_CHECK(document.Undo() && document.ParentOf(a).IsNull() && document.ParentOf(b).IsNull());
}

AETHER_TEST(HierarchyDrag_AClickOnASelectedRowSelectsJustIt) {
    DragRig rig;
    SceneDocument& document = rig.panel.Document();
    const Entity a = document.CreateEntity("0 Crate A");
    const Entity b = document.CreateEntity("0 Crate B");
    const std::vector<f32> rows = rig.Targets();
    AETHER_CHECK(rows.size() >= 3);
    if (rows.size() < 3) return;

    document.SetSelection({a, b});
    rig.Step(ImVec2(80.0f, rows[2]), false);
    rig.Step(ImVec2(80.0f, rows[2]), true);
    AETHER_CHECK(document.Selection().size() == 2);
    rig.Step(ImVec2(80.0f, rows[2]), false); // let go without moving: a click
    AETHER_CHECK(document.Selection().size() == 1 && document.IsSelected(b) && !document.IsSelected(a));
    AETHER_CHECK(document.ParentOf(b).IsNull());
}
