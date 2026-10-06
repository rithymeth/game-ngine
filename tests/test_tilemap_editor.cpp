#include "test_framework.h"
#include "sprite2d/tilemap_document.h"
#include "sprite2d/tilemap_panel.h"
#include "workspace/editor_workspace.h"

#include <imgui.h>

#include <filesystem>
#include <fstream>
#include <functional>

// The tilemap editor (Phase 26 step 5, §26.6): painting, flood fill, layers,
// autotile rules and undo, and the panel drawing headless.

using namespace aether;
using namespace aether::editor;
using namespace aether::sprite2d;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigMacOSXBehaviors = false;
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400, 900);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1300, 800));
        ImGui::Begin("Test");
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

TilemapEditDocument Doc() {
    TilemapData map;
    map.Resize(6, 5);
    map.AddLayer("A");
    Tileset ts;
    ts.texture_width = ts.texture_height = 64;
    ts.tile_width = ts.tile_height = 16;
    return TilemapEditDocument(std::move(map), std::move(ts));
}

} // namespace

AETHER_TEST(TilemapEditor_PaintEraseAndStrokesUndoAsOne) {
    TilemapEditDocument doc = Doc();
    doc.brush = 3;
    doc.tool = TileTool::Paint;
    doc.BeginStroke();
    for (i32 x = 0; x < 4; ++x) doc.Apply(x, 1);
    doc.EndStroke();
    CHECK(doc.Map().Get(0, 0, 1) == 3 && doc.Map().Get(0, 3, 1) == 3);
    CHECK(doc.CanUndo() && doc.dirty());
    doc.tool = TileTool::Erase;
    doc.Apply(1, 1); // a single click is its own stroke
    CHECK(doc.Map().Get(0, 1, 1) == kEmpty);
    CHECK(doc.Undo() && doc.Map().Get(0, 1, 1) == 3);
    CHECK(doc.Undo() && doc.Map().Get(0, 0, 1) == kEmpty && doc.Map().Get(0, 3, 1) == kEmpty); // the whole stroke
    CHECK(!doc.Undo());
    CHECK(doc.Redo() && doc.Map().Get(0, 3, 1) == 3);
    // A stroke that changes nothing records nothing.
    doc.tool = TileTool::Paint;
    doc.BeginStroke();
    doc.Apply(0, 1); // already 3
    doc.EndStroke();
    CHECK(doc.Redo() && doc.Map().Get(0, 1, 1) == kEmpty); // the redo stack survived
    doc.Apply(-5, 99); // outside: nothing
    CHECK(doc.Map().Get(0, 0, 0) == kEmpty);
}

AETHER_TEST(TilemapEditor_RectangleFillAndPick) {
    TilemapEditDocument doc = Doc();
    doc.brush = 7;
    doc.tool = TileTool::Rectangle;
    doc.Rectangle(3, 3, 1, 0); // corners in any order
    for (i32 y = 0; y <= 3; ++y) {
        for (i32 x = 1; x <= 3; ++x) CHECK(doc.Map().Get(0, x, y) == 7);
    }
    CHECK(doc.Map().Get(0, 0, 0) == kEmpty && doc.Map().Get(0, 4, 0) == kEmpty);
    doc.tool = TileTool::Erase;
    doc.Rectangle(2, 1, 2, 2);
    CHECK(doc.Map().Get(0, 2, 1) == kEmpty && doc.Map().Get(0, 2, 2) == kEmpty && doc.Map().Get(0, 2, 3) == 7);
    CHECK(doc.Undo() && doc.Map().Get(0, 2, 1) == 7);

    // Flood fill the empty region round the block: 6x5 - 12 cells.
    doc.tool = TileTool::Fill;
    doc.brush = 9;
    CHECK(doc.FloodRegion(0, 0).size() == 30 - 12);
    doc.Apply(0, 0);
    CHECK(doc.Map().Get(0, 0, 0) == 9 && doc.Map().Get(0, 5, 4) == 9 && doc.Map().Get(0, 2, 2) == 7);
    doc.Apply(0, 0); // filling with what is already there changes nothing
    CHECK(doc.Undo() && doc.Map().Get(0, 0, 0) == kEmpty && doc.Map().Get(0, 2, 2) == 7);
    CHECK(doc.FloodRegion(-1, 0).empty());

    doc.tool = TileTool::Pick;
    doc.Apply(2, 2);
    CHECK(doc.brush == 7);
}

AETHER_TEST(TilemapEditor_AutotileBrushPaintsByNeighbours) {
    TilemapEditDocument doc = Doc();
    const usize at = doc.AddAutotile("Ground");
    for (int m = 0; m < 16; ++m) CHECK(doc.SetAutotileTile(at, static_cast<u8>(m), 40 + m));
    CHECK(!doc.SetAutotileTile(at, 3, 43) && !doc.SetAutotileTile(5, 0, 1) && !doc.SetAutotileTile(at, 16, 1));
    doc.brush = AutotileCell(at);
    doc.tool = TileTool::Rectangle;
    doc.Rectangle(1, 1, 3, 3);
    CHECK(ResolveTile(doc.Map(), doc.GetTileset(), 0, 2, 2) == 55); // the middle: mask 15
    CHECK(ResolveTile(doc.Map(), doc.GetTileset(), 0, 1, 1) == 40 + (kNorth | kEast));
    doc.tool = TileTool::Erase;
    doc.Apply(2, 2);
    CHECK(ResolveTile(doc.Map(), doc.GetTileset(), 0, 2, 1) == 40 + (kEast | kWest));
    CHECK(doc.Undo() && doc.Undo()); // the rectangle, then the tile rules
    CHECK(doc.GetTileset().autotiles[0].tiles[15] == 55);
}

AETHER_TEST(TilemapEditor_LayersSolidsAndResize) {
    TilemapEditDocument doc = Doc();
    CHECK(doc.Map().layers.size() == 1 && !doc.RemoveLayer(0)); // the last layer stays
    const usize top = doc.AddLayer("Top");
    CHECK(top == 1 && doc.active_layer == 1);
    doc.brush = 2;
    doc.tool = TileTool::Paint;
    doc.Apply(0, 0);
    CHECK(doc.Map().Get(1, 0, 0) == 2 && doc.Map().Get(0, 0, 0) == kEmpty);
    CHECK(doc.RenameLayer(1, "Walls") && doc.Map().layers[1].name == "Walls" && !doc.RenameLayer(1, "Walls"));
    CHECK(doc.SetLayerCollides(1, false) && !doc.Map().layers[1].collides && !doc.SetLayerCollides(1, false));
    CHECK(doc.RemoveLayer(1) && doc.active_layer == 0 && doc.Map().layers.size() == 1);
    CHECK(doc.Undo() && doc.Map().layers.size() == 2 && doc.Map().Get(1, 0, 0) == 2);

    doc.SetSolid(2, true);
    CHECK(doc.GetTileset().IsSolid(2));
    doc.SetSolid(2, true); // no change, no undo step
    CHECK(doc.Undo() && !doc.GetTileset().IsSolid(2));

    CHECK(doc.Resize(10, 8) && doc.Map().width == 10 && doc.Map().Get(1, 0, 0) == 2);
    CHECK(!doc.Resize(10, 8) && !doc.Resize(0, 3) && !doc.Resize(100000, 100000));
    CHECK(doc.Undo() && doc.Map().width == 6);
}

AETHER_TEST(TilemapEditor_SavesBothFiles) {
    TilemapEditDocument doc = Doc();
    doc.brush = 5;
    doc.Apply(2, 2);
    doc.SetSolid(5, true);
    const auto dir = std::filesystem::temp_directory_path() / "aether_tilemap_editor";
    std::string error;
    CHECK(doc.dirty());
    CHECK(doc.Save(dir / "a.atilemap", dir / "a.atileset", &error));
    CHECK(!doc.dirty());
    TilemapData map;
    Tileset ts;
    CHECK(LoadTilemap(dir / "a.atilemap", map, &error) && map.Get(0, 2, 2) == 5);
    CHECK(LoadTileset(dir / "a.atileset", ts, &error) && ts.IsSolid(5));
    {
        std::ofstream(dir / "blocker") << "a file, not a folder";
        CHECK(!doc.Save(dir / "blocker" / "a.atilemap", dir / "b.atileset", &error));
    }
}

AETHER_TEST(TilemapEditor_PanelDrawsHeadless) {
    HeadlessImGui ui;
    TilemapEditDocument doc = Doc();
    doc.AddAutotile("Ground");
    doc.brush = 1;
    TilemapPanel panel(doc);
    const u64 revision = doc.Revision();
    for (int frame = 0; frame < 3; ++frame) ui.Frame([&] { panel.Draw(); });
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    CHECK(doc.Map().Get(0, 0, 0) == kEmpty && doc.Revision() == revision); // drawing alone edits nothing
    for (TileTool t : {TileTool::Erase, TileTool::Rectangle, TileTool::Fill, TileTool::Pick}) {
        doc.tool = t;
        ui.Frame([&] { panel.Draw(); });
    }
}

AETHER_TEST(TilemapEditor_WorkspaceShowsTheSampleLevel) {
    HeadlessImGui ui;
    EditorWorkspace ws;
    const i64 tool = ws.FindTool("Tilemap");
    CHECK(tool >= 0 && std::string(ws.ToolCategory(static_cast<usize>(tool))) == "2D");
    ws.Select(static_cast<usize>(tool));
    for (int frame = 0; frame < 2; ++frame) ui.Frame([&] { ws.DrawHubContents(); });
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
}
