#include "test_framework.h"
#include "aether/pak/pak.h"
#include "aether/assets/asset_database.h"
#include "aether/assets/png_writer.h"
#include "aether/project/project.h"
#include "core/editor_document.h"
#include "host/imgui_window_input.h"
#include "packaging/build_window.h"
#include "packaging/new_project_panel.h"
#include "sprite2d/tilemap_document.h"
#include "sprite2d/tilemap_panel.h"
#include "workspace/editor_workspace.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string_view>

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

AETHER_TEST(TilemapEditor_RevertedStrokePreservesRevisionAndRedoAcrossLayers) {
    TilemapEditDocument doc = Doc();
    const usize top = doc.AddLayer("Top");
    doc.brush = 3;
    doc.Apply(5, 4);
    CHECK(doc.Undo() && doc.CanRedo());
    const u64 revision = doc.Revision();
    const bool dirty = doc.dirty();

    doc.BeginStroke();
    doc.active_layer = 0;
    doc.Apply(5, 4);
    doc.active_layer = top;
    doc.Apply(5, 4);
    doc.tool = TileTool::Erase;
    doc.Apply(5, 4);
    doc.active_layer = 0;
    doc.Apply(5, 4);
    doc.EndStroke();

    CHECK(doc.Revision() == revision);
    CHECK(doc.dirty() == dirty);
    CHECK(doc.Redo());
    CHECK(doc.Map().Get(top, 5, 4) == 3 && doc.Map().Get(0, 5, 4) == kEmpty);
}

AETHER_TEST(TilemapEditor_LiveStrokeIsDirtyOnlyWhileCellsDiffer) {
    TilemapEditDocument doc = Doc();
    const auto revision = doc.Revision();
    doc.brush = 3;
    doc.BeginStroke();
    doc.Apply(-1, 0);
    CHECK(!doc.dirty());
    doc.Apply(1, 1);
    doc.Apply(2, 1);
    doc.Apply(2, 1); // Repainting the same tile must not count twice.
    CHECK(doc.dirty() && doc.Revision() == revision && !doc.CanUndo());
    doc.tool = TileTool::Erase;
    doc.Apply(1, 1);
    CHECK(doc.dirty()); // One changed tile remains.
    doc.Apply(2, 1);
    CHECK(!doc.dirty());
    doc.EndStroke();
    CHECK(!doc.dirty() && doc.Revision() == revision && !doc.CanUndo());
}

AETHER_TEST(TilemapEditor_StrokeWithLeadingNoOpsUndoesItsFirstRealEdit) {
    auto doc = Doc();
    const auto revision = doc.Revision();
    doc.BeginStroke();
    doc.tool = TileTool::Erase;
    doc.Apply(0, 0); // already empty
    doc.active_layer = 99;
    doc.tool = TileTool::Paint;
    doc.brush = 4;
    doc.Apply(0, 0); // invalid layer
    doc.active_layer = 0;
    doc.Apply(-1, 0);
    CHECK(!doc.dirty() && !doc.CanUndo() && doc.Revision() == revision);

    doc.Apply(1, 2); // first real change, after the no-op samples
    doc.Apply(1, 3);
    doc.tool = TileTool::Erase;
    doc.Apply(1, 2); // leave one change in the stroke
    doc.EndStroke();
    CHECK(doc.dirty() && doc.Map().Get(0, 1, 2) == kEmpty && doc.Map().Get(0, 1, 3) == 4);
    CHECK(doc.Undo() && !doc.dirty() && !doc.CanUndo() && doc.Map().Get(0, 1, 3) == kEmpty);
    CHECK(doc.Redo() && doc.Map().Get(0, 1, 3) == 4);
}

AETHER_TEST(TilemapEditor_SparseStrokeSurvivesReorderAndResize) {
    TilemapData map;
    map.Resize(64, 48);
    map.AddLayer("Floor");
    map.AddLayer("Decor");
    map.Set(0, 60, 44, 11);
    TilemapEditDocument doc(std::move(map), Doc().GetTileset());

    doc.BeginStroke();
    doc.brush = 14;
    doc.Apply(60, 44);
    doc.active_layer = 1;
    doc.brush = 22;
    doc.Apply(2, 3);
    doc.brush = 23;
    doc.Apply(2, 3);
    doc.EndStroke();
    const auto revision = doc.Revision();
    CHECK(doc.MoveLayer(1, 0));
    CHECK(doc.Resize(16, 12));
    CHECK(doc.Map().Get(0, 2, 3) == 23 && doc.Map().Get(1, 60, 44) == kEmpty);

    CHECK(doc.Undo() && doc.Map().Get(1, 60, 44) == 14);
    CHECK(doc.Undo() && doc.active_layer == 1 && doc.Map().Get(1, 2, 3) == 23);
    CHECK(doc.Undo() && doc.Map().Get(0, 60, 44) == 11 && doc.Map().Get(1, 2, 3) == kEmpty);
    CHECK(!doc.CanUndo() && !doc.dirty() && doc.Revision() > revision);
    CHECK(doc.Redo() && doc.Map().Get(1, 2, 3) == 23);
    CHECK(doc.Redo() && doc.active_layer == 0 && doc.Map().Get(1, 60, 44) == 14);
    CHECK(doc.Redo() && doc.Map().width == 16 && doc.Map().Get(0, 2, 3) == 23 && !doc.CanRedo());
}

AETHER_TEST(TilemapEditor_DenseStrokeRestoresOriginalsAndPreservesRedoWhenReverted) {
    TilemapData map;
    map.Resize(96, 72);
    map.AddLayer("Floor");
    map.AddLayer("Decor");
    map.Set(0, 5, 5, 4);
    TilemapEditDocument doc(std::move(map), Doc().GetTileset());
    doc.BeginStroke();
    doc.active_layer = 1;
    doc.brush = 12;
    doc.Apply(7, 7); // Early sparse write in a separate layer.
    doc.active_layer = 0;
    doc.brush = 9;
    doc.Rectangle(0, 0, 55, 50); // Promotes the active stroke to a snapshot.
    CHECK(doc.Undo() && !doc.CanUndo() && doc.CanRedo() && !doc.dirty());
    CHECK(doc.Map().Get(0, 5, 5) == 4 && doc.Map().Get(0, 50, 50) == kEmpty && doc.Map().Get(1, 7, 7) == kEmpty);
    const auto revision = doc.Revision();

    doc.BeginStroke();
    for (i32 y = 0; y < 30; ++y) for (i32 x = 0; x < 30; ++x) doc.Apply(x, y);
    CHECK(doc.dirty());
    doc.tool = TileTool::Erase;
    for (i32 y = 0; y < 30; ++y) for (i32 x = 0; x < 30; ++x)
        if (x != 5 || y != 5) doc.Apply(x, y);
    doc.tool = TileTool::Paint;
    doc.brush = 4;
    doc.Apply(5, 5);
    CHECK(!doc.dirty());
    doc.EndStroke();
    CHECK(doc.Revision() == revision && !doc.CanUndo() && doc.CanRedo());
    CHECK(doc.Redo() && !doc.CanRedo() && doc.Map().Get(0, 5, 5) == 9 &&
          doc.Map().Get(0, 50, 50) == 9 && doc.Map().Get(1, 7, 7) == 12);
}

AETHER_TEST(TilemapEditor_DenseStrokeLeavesSparseAndUntouchedLayerStorageInPlace) {
    TilemapData map;
    map.Resize(48, 48);
    map.AddLayer("Dense");
    map.AddLayer("Sparse");
    map.AddLayer("Untouched");
    TilemapEditDocument doc(std::move(map), {});
    const auto* sparse = doc.Map().layers[1].cells.data();
    const auto* untouched = doc.Map().layers[2].cells.data();
    doc.BeginStroke();
    doc.tool = TileTool::Fill;
    doc.brush = 4;
    doc.Apply(0, 0); // Promotes the first layer.
    doc.active_layer = 1;
    doc.tool = TileTool::Paint;
    doc.brush = 8;
    doc.Apply(2, 3);
    doc.EndStroke();
    CHECK(doc.Undo() && !doc.dirty());
    CHECK(doc.Map().Get(0, 47, 47) == kEmpty && doc.Map().Get(1, 2, 3) == kEmpty);
    CHECK(doc.Map().layers[1].cells.data() == sparse && doc.Map().layers[2].cells.data() == untouched);
    CHECK(doc.Redo() && doc.Map().Get(0, 47, 47) == 4 && doc.Map().Get(1, 2, 3) == 8);

    const auto* first = doc.Map().layers[0].cells.data();
    doc.BeginStroke();
    doc.active_layer = 0;
    doc.tool = TileTool::Fill;
    doc.brush = 6;
    doc.Apply(0, 0);
    doc.brush = 4;
    doc.Apply(0, 0); // This layer has no remaining changes.
    doc.active_layer = 1;
    doc.tool = TileTool::Paint;
    doc.brush = 9;
    doc.Apply(2, 3);
    doc.EndStroke();
    CHECK(doc.Undo() && doc.Map().Get(1, 2, 3) == 8);
    CHECK(doc.Map().layers[0].cells.data() == first);
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

AETHER_TEST(TilemapEditor_FloodRegionDoesNotCrossDiagonalOrLayerBoundaries) {
    TilemapData map;
    map.Resize(4, 4);
    map.AddLayer("Open");
    map.AddLayer("Pattern");
    map.Fill(1, 0, 0, 3, 3, 1);
    map.Fill(1, 0, 0, 1, 1, 2);
    map.Set(1, 2, 2, 2); // Same tile, but only diagonally connected.
    TilemapEditDocument doc(std::move(map), {});

    doc.active_layer = 1;
    auto region = doc.FloodRegion(0, 0);
    std::sort(region.begin(), region.end());
    CHECK(region == std::vector<std::pair<i32, i32>>{{0, 0}, {0, 1}, {1, 0}, {1, 1}});
    doc.tool = TileTool::Fill;
    doc.brush = 3;
    doc.Apply(0, 0);
    CHECK(doc.Map().Get(1, 1, 1) == 3 && doc.Map().Get(1, 2, 2) == 2);
    CHECK(doc.Map().Get(1, 1, 2) == 1 && doc.Map().Get(0, 0, 0) == kEmpty);
    CHECK(doc.Undo() && doc.Map().Get(1, 1, 1) == 2 && !doc.CanUndo());
    doc.active_layer = doc.Map().layers.size();
    CHECK(doc.FloodRegion(0, 0).empty());
}

AETHER_TEST(TilemapEditor_FloodPaintMatchesRegionThroughNarrowConnections) {
    TilemapData map;
    map.Resize(71, 57);
    map.AddLayer("Unchanged");
    map.AddLayer("Pattern");
    const i32 grass = AutotileCell(0);
    map.Fill(1, 0, 0, 70, 56, grass);
    // Alternating teeth force multiple runs per row and many reconnections.
    for (i32 y = 1; y < 56; y += 2) {
        for (i32 x = 1; x < 70; x += 3) map.Set(1, x, y, 1);
    }
    map.Fill(1, 4, 4, 10, 10, 1);
    map.Fill(1, 5, 5, 9, 9, grass); // A region of the same value inside a closed ring.
    TilemapEditDocument doc(std::move(map), {});
    doc.active_layer = 1;
    const auto original = doc.Map().layers[1].cells;
    auto expected = original;
    const auto region = doc.FloodRegion(0, 0);
    CHECK(region.size() > 2500);
    for (const auto& [x, y] : region) expected[static_cast<usize>(y) * 71 + x] = kEmpty;
    doc.tool = TileTool::Fill;
    doc.brush = kEmpty;
    doc.Apply(0, 0);
    CHECK(doc.Map().layers[1].cells == expected);
    CHECK(doc.Map().Get(1, 7, 7) == grass && doc.Map().Get(0, 70, 56) == kEmpty);
    CHECK(doc.Undo() && doc.Map().layers[1].cells == original && !doc.CanUndo());
    CHECK(doc.Redo() && doc.Map().layers[1].cells == expected);
}

AETHER_TEST(TilemapEditor_DenseRectangleKeepsRedoAfterAClippedNetZeroStroke) {
    TilemapData map;
    map.Resize(72, 48);
    map.AddLayer("Ground");
    map.Fill(0, 0, 0, 35, 47, 4);
    const auto initial = map.layers[0].cells;
    TilemapEditDocument doc(std::move(map), {});
    doc.brush = 9;
    doc.Apply(0, 47);
    CHECK(doc.Undo() && !doc.dirty() && doc.CanRedo());
    const auto revision = doc.Revision();

    doc.BeginStroke();
    doc.brush = 4;
    for (i32 y = 0; y < 48; ++y) for (i32 x = 36; x < 72; ++x) doc.Apply(x, y);
    CHECK(doc.dirty());
    doc.tool = TileTool::Erase;
    doc.Rectangle(99, 99, 36, -5); // Bulk restore; clipped edges and reversed corners.
    CHECK(doc.Map().layers[0].cells == initial && !doc.dirty());
    CHECK(doc.Revision() == revision && !doc.CanUndo() && doc.CanRedo());
    CHECK(doc.Redo() && doc.Map().Get(0, 0, 47) == 9);

    doc.tool = TileTool::Rectangle;
    doc.brush = 4;
    doc.Rectangle(0, 0, 71, 47); // Mixes matching and changed cells.
    CHECK(doc.Map().Get(0, 0, 47) == 4 && doc.Map().Get(0, 71, 47) == 4);
    CHECK(doc.Undo() && doc.Map().Get(0, 0, 47) == 9 && doc.Map().Get(0, 71, 47) == kEmpty);

    TilemapData truncated;
    truncated.Resize(6, 8);
    truncated.AddLayer("Short");
    truncated.layers[0].cells.resize(33); // Part of the sixth row is missing.
    TilemapEditDocument short_doc(std::move(truncated), {});
    short_doc.brush = 8;
    short_doc.Rectangle(0, 0, 5, 7);
    CHECK(short_doc.Map().layers[0].cells.size() == 33);
    CHECK(short_doc.Map().Get(0, 2, 5) == 8 && short_doc.Map().Get(0, 3, 5) == kEmpty);
    CHECK(short_doc.Undo() && short_doc.Map().Get(0, 2, 5) == kEmpty);
}

AETHER_TEST(TilemapEditor_DenseFillCountsNewAndRevertedCellsInTheSameRun) {
    TilemapData map;
    map.Resize(90, 28);
    map.AddLayer("Ground");
    map.Fill(0, 0, 0, 29, 27, 1);
    map.Fill(0, 30, 0, 59, 27, 2);
    map.Fill(0, 60, 0, 89, 27, 3);
    const auto initial = map.layers[0].cells;
    TilemapEditDocument doc(std::move(map), {});
    doc.BeginStroke();
    doc.brush = 3;
    for (i32 y = 0; y < 28; ++y) for (i32 x = 0; x < 60; ++x) doc.Apply(x, y);
    CHECK(doc.dirty());
    doc.tool = TileTool::Fill;
    doc.brush = 1;
    doc.Apply(0, 0); // Restores the left third and changes the previously untouched right.
    CHECK(doc.dirty() && doc.Map().Get(0, 89, 27) == 1);
    doc.tool = TileTool::Paint;
    for (i32 y = 0; y < 28; ++y) for (i32 x = 30; x < 90; ++x) {
        doc.brush = x < 60 ? 2 : 3;
        doc.Apply(x, y);
    }
    CHECK(!doc.dirty() && doc.Map().layers[0].cells == initial);
    doc.EndStroke();
    CHECK(!doc.CanUndo() && !doc.dirty());
}

AETHER_TEST(TilemapEditor_FloodPaintRemainsOneStrokeWithPaintAndRevert) {
    TilemapEditDocument doc = Doc();
    doc.BeginStroke();
    doc.brush = 8;
    doc.Apply(1, 1);
    doc.tool = TileTool::Fill;
    doc.brush = 4;
    doc.Apply(0, 0);
    CHECK(doc.Map().Get(0, 1, 1) == 8 && doc.Map().Get(0, 5, 4) == 4);
    doc.brush = kEmpty;
    doc.Apply(1, 1);
    doc.Apply(5, 4);
    doc.EndStroke();
    CHECK(!doc.CanUndo() && !doc.dirty());
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

AETHER_TEST(TilemapEditor_TilesetHistoryKeepsPaintedTileStorage) {
    TilemapEditDocument doc = Doc();
    doc.BeginStroke();
    doc.brush = 7;
    doc.Apply(0, 0);
    // End the stroke before recording the independent tileset edit.
    const usize rule = doc.AddAutotile("Grass");
    CHECK(doc.SetAutotileTile(rule, kNorth, 3));
    CHECK(doc.FillEmptyAutotileMasks(rule, 2));
    doc.SetSolid(7, true);
    const auto* cells = doc.Map().layers[0].cells.data();
    CHECK(doc.GetTileset().IsSolid(7));
    CHECK(doc.Undo() && !doc.GetTileset().IsSolid(7));
    CHECK(doc.Undo() && rule < doc.GetTileset().autotiles.size() && doc.GetTileset().autotiles[rule].tiles[0] == kEmpty);
    CHECK(doc.Undo() && rule < doc.GetTileset().autotiles.size() && doc.GetTileset().autotiles[rule].tiles[kNorth] == kEmpty);
    CHECK(doc.Undo() && doc.GetTileset().autotiles.empty());
    CHECK(doc.Map().Get(0, 0, 0) == 7 && doc.Map().layers[0].cells.data() == cells);
    CHECK(doc.Undo() && !doc.dirty() && doc.Map().Get(0, 0, 0) == kEmpty);
    CHECK(doc.Redo() && doc.Redo() && doc.Redo() && doc.Redo() && doc.Redo());
    CHECK(doc.GetTileset().IsSolid(7) && doc.Map().Get(0, 0, 0) == 7);
    CHECK(doc.Map().layers[0].cells.data() == cells);
}

AETHER_TEST(TilemapEditor_AppendingAutotileKeepsExistingTilesetStorage) {
    TilemapData map;
    map.Resize(2, 2);
    Tileset tiles;
    tiles.SetSolid(4095, true);
    const std::string existing_name(128, 'a');
    const std::string added_name(128, 'b');
    tiles.AddAutotile(existing_name, Autotile{}.tiles);
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    const auto* flags = doc.GetTileset().solid.data();
    const auto* existing = doc.GetTileset().autotiles[0].name.data();

    const usize added = doc.AddAutotile(added_name);
    CHECK(added == 1 && doc.dirty());
    CHECK(doc.GetTileset().autotiles[added].tiles == Autotile{}.tiles);
    const auto* added_storage = doc.GetTileset().autotiles[added].name.data();
    CHECK(doc.Undo() && !doc.dirty() && doc.GetTileset().autotiles.size() == 1);
    CHECK(doc.GetTileset().solid.data() == flags && doc.GetTileset().IsSolid(4095));
    CHECK(doc.GetTileset().autotiles[0].name.data() == existing);
    CHECK(doc.Redo() && doc.dirty() && doc.GetTileset().autotiles.size() == 2);
    CHECK(doc.GetTileset().autotiles[added].name == added_name);
    CHECK(doc.GetTileset().autotiles[added].name.data() == added_storage);

    CHECK(doc.RenameAutotile(added, "Renamed") && doc.SetAutotileTile(added, kNorth, 3));
    doc.SetSolid(1, true); // exercise mixed tileset history
    CHECK(doc.Undo() && !doc.GetTileset().IsSolid(1));
    CHECK(doc.Undo() && doc.GetTileset().autotiles[added].tiles[kNorth] == kEmpty);
    CHECK(doc.Undo() && doc.GetTileset().autotiles[added].name == added_name);
    CHECK(doc.Undo() && doc.GetTileset().autotiles.size() == 1 && !doc.dirty());
    CHECK(doc.Redo() && doc.Redo() && doc.Redo() && doc.Redo());
    CHECK(doc.GetTileset().IsSolid(1) && doc.GetTileset().IsSolid(4095));
    CHECK(doc.GetTileset().autotiles[added].name == "Renamed");
    CHECK(doc.GetTileset().autotiles[added].tiles[kNorth] == 3);
    CHECK(doc.Undo() && doc.Undo() && doc.Undo() && doc.Undo());
    CHECK(doc.AddAutotile("Replacement") == added && !doc.CanRedo());
    CHECK(doc.GetTileset().autotiles[added].name == "Replacement");
}

AETHER_TEST(TilemapEditor_SolidHistoryRestoresByteAndTrimmedLength) {
    TilemapData map;
    map.Resize(2, 2);
    Tileset tiles;
    const std::vector<u8> original{0, 7, 0, 0, 0, 0, 255, 0, 0, 0};
    tiles.solid = original;
    const std::string name(128, 'a');
    tiles.AddAutotile(name, Autotile{}.tiles);
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    const auto* untouched_rule = doc.GetTileset().autotiles[0].name.data();
    doc.BeginStroke();
    doc.brush = 4;
    doc.Apply(1, 1);
    doc.SetSolid(1, false);
    CHECK((doc.GetTileset().solid == std::vector<u8>{0, 0, 0, 0, 0, 0, 255}));
    doc.SetSolid(6, false);
    CHECK(doc.GetTileset().solid.empty());
    doc.SetSolid(8191, true);
    CHECK(doc.GetTileset().solid.size() == 8192 && doc.GetTileset().solid.back() == 1);

    CHECK(doc.Undo() && doc.GetTileset().solid.empty());
    CHECK((doc.Undo() && doc.GetTileset().solid == std::vector<u8>{0, 0, 0, 0, 0, 0, 255}));
    CHECK(doc.Undo() && doc.GetTileset().solid == original);
    CHECK(doc.GetTileset().autotiles[0].name.data() == untouched_rule);
    CHECK(doc.Map().Get(0, 1, 1) == 4 && doc.Undo());
    CHECK(doc.Map().Get(0, 1, 1) == kEmpty && !doc.dirty());

    CHECK(doc.Redo() && doc.Redo() && doc.Redo() && doc.Redo());
    CHECK(doc.Map().Get(0, 1, 1) == 4 && doc.dirty());
    CHECK(doc.GetTileset().solid.size() == 8192 && doc.GetTileset().IsSolid(8191));
    CHECK(!doc.GetTileset().IsSolid(1) && !doc.GetTileset().IsSolid(6));
    CHECK(doc.GetTileset().autotiles[0].name.data() == untouched_rule);
    CHECK(doc.Undo());
    doc.SetSolid(2, true);
    CHECK((doc.GetTileset().solid == std::vector<u8>{0, 0, 1}) && !doc.CanRedo());
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

AETHER_TEST(TilemapEditor_AddLayerUndoAndRedoRestoreTheBrushTarget) {
    TilemapEditDocument doc = Doc();
    doc.AddLayer("Walls");
    doc.AddLayer("Canopy");
    doc.active_layer = 0;

    const usize added = doc.AddLayer("Decor");
    CHECK(added == 3 && doc.active_layer == added);
    CHECK(doc.Undo());
    CHECK(doc.Map().layers.size() == 3 && doc.active_layer == 0);
    CHECK(doc.Redo());
    CHECK(doc.Map().layers.size() == 4 && doc.active_layer == added);
    doc.brush = 4;
    doc.Apply(0, 0);
    CHECK(doc.Map().Get(added, 0, 0) == 4 && doc.Map().Get(2, 0, 0) == kEmpty);
}

AETHER_TEST(TilemapEditor_RemovingLayerKeepsTheBrushOnTheSameLayer) {
    TilemapEditDocument doc = Doc();
    doc.AddLayer("Walls");
    doc.AddLayer("Canopy");
    doc.active_layer = 1;

    // Deleting a layer underneath Walls shifts its index, not the brush target.
    CHECK(doc.RemoveLayer(0));
    CHECK(doc.active_layer == 0 && doc.Map().layers[0].name == "Walls");
    CHECK(doc.Undo());
    CHECK(doc.active_layer == 1 && doc.Map().layers[1].name == "Walls");
    CHECK(doc.Redo());
    CHECK(doc.active_layer == 0 && doc.Map().layers[0].name == "Walls");
    doc.brush = 4;
    doc.Apply(0, 0);
    CHECK(doc.Map().Get(0, 0, 0) == 4 && doc.Map().Get(1, 0, 0) == kEmpty);

    // Deleting the selected top layer selects its neighbor; Undo selects the restored layer.
    doc.active_layer = 1;
    CHECK(doc.RemoveLayer(1) && doc.active_layer == 0);
    CHECK(doc.Undo());
    CHECK(doc.active_layer == 1 && doc.Map().layers[1].name == "Canopy");
    CHECK(doc.Redo() && doc.active_layer == 0);
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

AETHER_TEST(TilemapEditor_RejectsSavingBothAssetsToOneDestination) {
    TilemapEditDocument doc = Doc();
    const auto dir = std::filesystem::temp_directory_path() / "aether_tilemap_save_alias";
    std::filesystem::create_directories(dir / "nested");
    const auto map_path = dir / "map.atilemap", tileset_path = dir / "set.atileset";
    const auto unused = dir / "unused.atilemap";
    std::string error;
    CHECK(!doc.Save(unused, unused, &error) && error.find("distinct files") != std::string::npos);
    CHECK(!std::filesystem::exists(unused) && doc.TilemapFile().empty() && doc.TilesetFile().empty());

    if (!doc.Save(map_path, tileset_path, &error)) { CHECK(false); return; }
    const auto read = [](const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>{in}, {});
    };
    const auto map_before = read(map_path), tileset_before = read(tileset_path);
    CHECK(!map_before.empty() && !tileset_before.empty());
    doc.brush = 5;
    doc.Apply(2, 2);
    const auto alias = dir / "nested" / ".." / map_path.filename();
    CHECK(!doc.Save(map_path, alias, &error) && error.find("distinct files") != std::string::npos);
    CHECK(read(map_path) == map_before && read(tileset_path) == tileset_before);
    CHECK(doc.dirty() && doc.TilemapFile() == map_path && doc.TilesetFile() == tileset_path);
}

AETHER_TEST(TilemapEditor_SaveStagesEachAssetBeforeReplacingOldBytes) {
    TilemapEditDocument doc = Doc();
    doc.brush = 5;
    doc.Apply(2, 2);
    doc.SetSolid(5, true);
    const auto dir = std::filesystem::temp_directory_path() / "aether_tilemap_atomic";
    const auto map_path = dir / "a.atilemap", set_path = dir / "a.atileset";
    std::string error;
    if (!doc.Save(map_path, set_path, &error)) { CHECK(false); return; }
    TilemapData changed = doc.Map();
    changed.Set(0, 2, 2, 6);
    Tileset tiles = doc.GetTileset();
    tiles.SetSolid(6, true);
    for (const auto& target : {map_path, set_path}) {
        const auto temp = std::filesystem::path(target.string() + ".tmp");
        std::filesystem::create_directory(temp);
        std::ofstream(temp / "keep") << "an occupied staging path";
        const bool saved = target == map_path ? SaveTilemap(target, changed, &error) : SaveTileset(target, tiles, &error);
        CHECK(!saved && !error.empty());
        CHECK(std::filesystem::is_regular_file(temp / "keep"));
    }
    TilemapData original;
    Tileset original_set;
    CHECK(LoadTilemap(map_path, original, &error) && original.Get(0, 2, 2) == 5);
    CHECK(LoadTileset(set_path, original_set, &error) && original_set.IsSolid(5) && !original_set.IsSolid(6));
    for (const auto& target : {map_path, set_path}) {
        const auto temp = std::filesystem::path(target.string() + ".tmp");
        std::filesystem::remove(temp / "keep");
        std::filesystem::remove(temp);
    }
    CHECK(SaveTilemap(map_path, changed, &error) && LoadTilemap(map_path, original, &error) && original.Get(0, 2, 2) == 6);
    CHECK(SaveTileset(set_path, tiles, &error) && LoadTileset(set_path, original_set, &error) && original_set.IsSolid(6));
}

AETHER_TEST(TilemapEditor_SecondAssetStageFailureDoesNotCommitMap) {
    TilemapEditDocument doc = Doc();
    doc.brush = 5;
    doc.Apply(2, 2);
    const auto dir = std::filesystem::temp_directory_path() / "aether_tilemap_pair_stage";
    const auto map_path = dir / "a.atilemap", set_path = dir / "a.atileset";
    std::string error;
    if (!doc.Save(map_path, set_path, &error)) { CHECK(false); return; }
    doc.brush = 6;
    doc.Apply(2, 2);
    doc.SetSolid(6, true);
    std::ofstream(dir / "occupied") << "not a directory";
    CHECK(!doc.Save(map_path, dir / "occupied" / "new.atileset", &error) && error.find("new.atileset") != std::string::npos);
    CHECK(doc.dirty() && doc.Map().Get(0, 2, 2) == 6 && doc.GetTileset().IsSolid(6));
    CHECK(doc.TilemapFile() == map_path && doc.TilesetFile() == set_path);
    TilemapData map;
    Tileset tiles;
    CHECK(LoadTilemap(map_path, map, &error) && map.Get(0, 2, 2) == 5);
    CHECK(LoadTileset(set_path, tiles, &error) && !tiles.IsSolid(6));
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        CHECK(entry.path().filename().string().find(".aether-save-") == std::string::npos);
    CHECK(doc.Save(&error) && !doc.dirty());
    CHECK(LoadTilemap(map_path, map, &error) && map.Get(0, 2, 2) == 6);
    CHECK(LoadTileset(set_path, tiles, &error) && tiles.IsSolid(6));
}

AETHER_TEST(TilemapEditor_FillsUnsetAutotileMasksAsOneEdit) {
    TilemapData map;
    map.Resize(2, 2);
    map.AddLayer("Ground");
    Tileset tiles = Doc().GetTileset();
    std::array<i32, 16> variants;
    variants.fill(kEmpty);
    variants[kEast] = 7;
    const auto rule = tiles.AddAutotile("Moss", variants);
    map.Set(0, 0, 0, AutotileCell(rule));
    CHECK(ResolveTile(map, tiles, 0, 0, 0) == kEmpty);
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    CHECK(!doc.FillEmptyAutotileMasks(1, 4) && !doc.FillEmptyAutotileMasks(rule, 16));
    CHECK(!doc.FillEmptyAutotileMasks(rule, kEmpty) && !doc.dirty() && !doc.CanUndo());
    CHECK(doc.FillEmptyAutotileMasks(rule, 4));
    CHECK(ResolveTile(doc.Map(), doc.GetTileset(), 0, 0, 0) == 4);
    for (usize mask = 0; mask < 16; ++mask)
        CHECK(doc.GetTileset().autotiles[rule].tiles[mask] == (mask == kEast ? 7 : 4));
    const u64 revision = doc.Revision();
    CHECK(!doc.FillEmptyAutotileMasks(rule, 3) && doc.Revision() == revision);
    CHECK(doc.Undo() && !doc.CanUndo() && doc.GetTileset().autotiles[rule].tiles == variants);
    CHECK(doc.Redo() && ResolveTile(doc.Map(), doc.GetTileset(), 0, 0, 0) == 4);
    const auto root = std::filesystem::temp_directory_path() / "tilemap-fill-masks";
    if (!doc.Save(root / "Moss.atilemap", root / "Moss.atileset")) { CHECK(false); return; }
    Tileset saved;
    if (!LoadTileset(root / "Moss.atileset", saved) || saved.autotiles.size() != 1) { CHECK(false); return; }
    CHECK(saved.autotiles[0].tiles == doc.GetTileset().autotiles[rule].tiles);
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

AETHER_TEST(TilemapEditor_ChoosingAnAutotileLeavesEraseMode) {
    HeadlessImGui ui;
    auto doc = Doc();
    const usize ground = doc.AddAutotile("Ground");
    doc.brush = 3;
    doc.Apply(0, 0);
    doc.brush = kEmpty;
    doc.tool = TileTool::Erase;
    TilemapPanel panel(doc);
    ImGuiWindow* palette = nullptr;
    ImVec2 canvas_origin;
    const auto draw = [&] {
        const ImGuiID palette_id = ImGui::GetID("##left");
        const ImGuiWindow* parent = ImGui::GetCurrentWindowRead();
        panel.Draw();
        for (ImGuiWindow* child : parent->DC.ChildWindows)
            if (child->ChildId == palette_id) palette = child;
        const auto& padding = ImGui::GetStyle().WindowPadding;
        canvas_origin = {ImGui::GetItemRectMin().x + padding.x + 8.0f,
                         ImGui::GetItemRectMax().y - padding.y - 8.0f};
    };
    auto& io = ImGui::GetIO();
    const auto click = [&] {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        ui.Frame(draw);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        ui.Frame(draw);
    };
    const auto click_palette_item = [&](ImGuiID item) {
        // Find the actual interactive row without baking in a font or panel layout.
        const ImRect clip = palette->InnerClipRect;
        for (f32 y = clip.Min.y + 2.0f; y < clip.Max.y; y += 4.0f) {
            io.AddMousePosEvent(clip.Min.x + 16.0f, y);
            ui.Frame(draw);
            if (ImGui::GetHoveredID() == item) {
                click();
                return true;
            }
        }
        return false;
    };

    ui.Frame(draw);
    if (!palette) { CHECK(false); return; }
    const u64 revision = doc.Revision();
    const ImGuiID ground_id = ImHashStr("##autotile", 0, palette->GetID(1000));
    CHECK(click_palette_item(ground_id));
    CHECK(doc.tool == TileTool::Paint && doc.brush == AutotileCell(ground));
    CHECK(doc.Revision() == revision); // selecting a rule is not an edit
    io.AddMousePosEvent(canvas_origin.x + panel.zoom * 0.5f, canvas_origin.y - panel.zoom * 0.5f);
    ui.Frame(draw);
    click();
    CHECK(doc.Map().Get(0, 0, 0) == AutotileCell(ground));
    CHECK(doc.Undo() && doc.Map().Get(0, 0, 0) == 3);

    doc.tool = TileTool::Erase;
    CHECK(click_palette_item(palette->GetID("Add autotile")));
    CHECK(doc.GetTileset().autotiles.size() == 2);
    CHECK(doc.tool == TileTool::Paint && doc.brush == AutotileCell(1));
    CHECK(doc.Undo() && doc.GetTileset().autotiles.size() == 1);
}

AETHER_TEST(TilemapEditor_AutotileNamesRenderLiterallyAndSelectTheRightRule) {
    const auto exercise_palette = [](const char* first, const char* second, bool select) {
        HeadlessImGui ui;
        auto doc = Doc();
        const usize first_rule = doc.AddAutotile(first);
        const usize second_rule = doc.AddAutotile(second);
        doc.brush = kEmpty; // keep the separate rule editor out of the comparison
        const u64 revision = doc.Revision();
        TilemapPanel panel(doc);
        ImGuiWindow* palette = nullptr;
        const auto draw = [&] {
            const ImGuiID palette_id = ImGui::GetID("##left");
            const ImGuiWindow* parent = ImGui::GetCurrentWindowRead();
            panel.Draw();
            for (ImGuiWindow* child : parent->DC.ChildWindows)
                if (child->ChildId == palette_id) palette = child;
        };
        ui.Frame(draw);
        const int vertices = ImGui::GetDrawData()->TotalVtxCount;
        if (!select) return vertices;
        if (!palette) { CHECK(false); return vertices; }

        auto& io = ImGui::GetIO();
        const auto click_rule = [&](usize index) {
            // Use the stable widget ID to locate the real selectable row.
            const ImGuiID id = ImHashStr("##autotile", 0, palette->GetID(static_cast<int>(1000 + index)));
            const ImRect clip = palette->InnerClipRect;
            for (f32 y = clip.Min.y + 2.0f; y < clip.Max.y; y += 4.0f) {
                io.AddMousePosEvent(clip.Min.x + 19.0f, y);
                ui.Frame(draw);
                if (ImGui::GetHoveredID() != id) continue;
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                ui.Frame(draw);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                ui.Frame(draw);
                return true;
            }
            return false;
        };
        CHECK(click_rule(second_rule) && doc.brush == AutotileCell(second_rule));
        CHECK(click_rule(first_rule) && doc.brush == AutotileCell(first_rule));
        CHECK(doc.Revision() == revision && doc.Map().Get(0, 0, 0) == kEmpty);
        return vertices;
    };

    const int prefixes = exercise_palette("Water", "Water", false);
    // Only the suffix characters change. If ImGui treats them as widget IDs
    // rather than authored text, they submit no glyphs at all.
    const int with_markers = exercise_palette("Water##Deep", "Water###Shallow", true);
    CHECK(with_markers > prefixes);
}

AETHER_TEST(TilemapEditor_InvalidPickedTileCannotBecomeAnAutotileVariant) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(6, 5);
    map.AddLayer("Ground");
    map.Set(0, 0, 0, 6); // outside the authored six-tile grid
    map.Set(0, 1, 0, 5); // last valid tile
    Tileset tiles;
    tiles.texture = assets::NewAssetGuid();
    tiles.texture_width = 48;
    tiles.texture_height = 32;
    auto variants = Autotile().tiles;
    constexpr u8 mask = kNorth | kEast;
    variants[mask] = 4;
    const usize river = tiles.AddAutotile("River", variants);
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    TilemapPanel panel(doc);
    doc.tool = TileTool::Pick;
    doc.brush = 5;

    ImGuiWindow* palette = nullptr;
    ImVec2 canvas_origin;
    const auto draw = [&] {
        const auto palette_id = ImGui::GetID("##left");
        const auto* parent = ImGui::GetCurrentWindowRead();
        panel.Draw();
        for (auto* child : parent->DC.ChildWindows)
            if (child->ChildId == palette_id) palette = child;
        const auto& padding = ImGui::GetStyle().WindowPadding;
        canvas_origin = {ImGui::GetItemRectMin().x + padding.x + 8.0f,
                         ImGui::GetItemRectMax().y - padding.y - 8.0f};
    };
    auto& io = ImGui::GetIO();
    const auto move = [&](ImVec2 p) {
        io.AddMousePosEvent(p.x, p.y);
        ui.Frame(draw);
    };
    const auto click = [&] {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        ui.Frame(draw);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        ui.Frame(draw);
    };
    ui.Frame(draw);
    if (!palette) { CHECK(false); return; }
    const ImGuiID mask_id = ImHashStr("NE-- 4", 0, palette->GetID(2000 + mask));
    const ImRect clip = palette->InnerClipRect;
    std::optional<ImVec2> mask_button;
    for (f32 y = clip.Min.y + 3.0f; y < clip.Max.y && !mask_button; y += 6.0f) {
        for (f32 x = clip.Min.x + 8.0f; x < clip.Max.x; x += 16.0f) {
            move({x, y});
            if (ImGui::GetHoveredID() == mask_id) { mask_button = ImVec2(x, y); break; }
        }
    }
    if (!mask_button) { CHECK(false); return; }
    const auto pick = [&](i32 x) {
        move({canvas_origin.x + (x + 0.5f) * panel.zoom, canvas_origin.y - 0.5f * panel.zoom});
        click();
    };
    const u64 revision = doc.Revision();
    pick(0);
    CHECK(doc.brush == 6);
    move(*mask_button);
    click();
    CHECK(doc.GetTileset().autotiles[river].tiles == variants && doc.Revision() == revision &&
          !doc.CanUndo() && !doc.dirty());
    CHECK(!doc.SetAutotileTile(river, mask, 6) && doc.Revision() == revision);

    pick(1);
    CHECK(doc.brush == 5);
    move(*mask_button);
    click();
    CHECK(doc.GetTileset().autotiles[river].tiles[mask] == 5 && doc.dirty());
    CHECK(doc.Undo() && doc.GetTileset().autotiles[river].tiles == variants && !doc.CanUndo() && !doc.dirty());
    CHECK(doc.SetAutotileTile(river, mask, kEmpty) && doc.GetTileset().autotiles[river].tiles[mask] == kEmpty);
}

AETHER_TEST(TilemapEditor_ClearsOneAutotileMaskWithNoValidConcreteBrush) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(6, 5);
    map.AddLayer("Ground");
    Tileset tiles;
    tiles.texture = assets::NewAssetGuid();
    tiles.texture_width = tiles.texture_height = 32;
    std::array<i32, 16> variants{};
    variants[kNorth] = 3;
    variants[kEast] = 2;
    const auto rule = tiles.AddAutotile("River", variants);
    map.Set(0, 0, 0, AutotileCell(rule));
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    doc.brush = AutotileCell(rule);
    TilemapPanel panel(doc);
    ImGuiWindow* palette = nullptr;
    const auto draw = [&] {
        const auto* parent = ImGui::GetCurrentWindowRead();
        const auto id = ImGui::GetID("##left");
        panel.Draw();
        for (ImGuiWindow* child : parent->DC.ChildWindows)
            if (child->ChildId == id) palette = child;
    };
    const auto point_at_mask = [&] {
        if (!palette) return false;
        auto& io = ImGui::GetIO();
        const auto id = ImHashStr("N--- 3", 0, palette->GetID(2000 + kNorth));
        const auto clip = palette->InnerClipRect;
        for (f32 y = clip.Min.y + 3; y < clip.Max.y; y += 6) {
            for (f32 x = clip.Min.x + 8; x < clip.Max.x; x += 16) {
                io.AddMousePosEvent(x, y);
                ui.Frame(draw);
                if (ImGui::GetHoveredID() == id) return true;
            }
        }
        return false;
    };
    const auto click = [&](ImGuiMouseButton button) {
        ImGui::GetIO().AddMouseButtonEvent(button, true);
        ui.Frame(draw);
        ImGui::GetIO().AddMouseButtonEvent(button, false);
        ui.Frame(draw);
    };
    for (const i32 brush : {AutotileCell(rule), 99}) {
        doc.brush = brush;
        const auto revision = doc.Revision();
        ui.Frame(draw);
        if (!point_at_mask()) { CHECK(false); return; }
        click(ImGuiMouseButton_Left);
        CHECK(doc.Revision() == revision && doc.GetTileset().autotiles[rule].tiles == variants);
        click(ImGuiMouseButton_Right);
        auto cleared = variants;
        cleared[kNorth] = kEmpty;
        CHECK(doc.GetTileset().autotiles[rule].tiles == cleared && doc.Revision() == revision + 1);
        CHECK(doc.brush == brush && doc.Map().Get(0, 0, 0) == AutotileCell(rule));
        CHECK(doc.Undo() && !doc.dirty() && doc.GetTileset().autotiles[rule].tiles == variants);
    }
}

AETHER_TEST(TilemapEditor_OutOfGridBrushCannotDefineSolidTiles) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(6, 5);
    map.AddLayer("Ground");
    map.Set(0, 0, 0, 6); // stale paint after shrinking a six-tile sheet
    map.Set(0, 1, 0, 5);
    Tileset tiles;
    tiles.texture = assets::NewAssetGuid();
    tiles.texture_width = 48;
    tiles.texture_height = 32;
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    doc.tool = TileTool::Pick;
    TilemapPanel panel(doc);

    ImGuiWindow* palette = nullptr;
    ImVec2 canvas_origin;
    const auto draw = [&] {
        const auto palette_id = ImGui::GetID("##left");
        const auto* parent = ImGui::GetCurrentWindowRead();
        panel.Draw();
        for (auto* child : parent->DC.ChildWindows)
            if (child->ChildId == palette_id) palette = child;
        const auto& padding = ImGui::GetStyle().WindowPadding;
        canvas_origin = {ImGui::GetItemRectMin().x + padding.x + 8.0f,
                         ImGui::GetItemRectMax().y - padding.y - 8.0f};
    };
    auto& io = ImGui::GetIO();
    const auto move = [&](ImVec2 p) {
        io.AddMousePosEvent(p.x, p.y);
        ui.Frame(draw);
    };
    const auto click = [&] {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        ui.Frame(draw);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        ui.Frame(draw);
    };
    ui.Frame(draw);
    if (!palette) { CHECK(false); return; }
    const ImGuiID solid_id = palette->GetID("Tile blocks movement");
    const ImRect clip = palette->InnerClipRect;
    std::optional<ImVec2> solid_checkbox;
    for (f32 y = clip.Min.y + 2.0f; y < clip.Max.y; y += 4.0f) {
        move({clip.Min.x + 16.0f, y});
        if (ImGui::GetHoveredID() == solid_id) { solid_checkbox = io.MousePos; break; }
    }
    if (!solid_checkbox) { CHECK(false); return; }
    const auto pick = [&](i32 x) {
        move({canvas_origin.x + (x + 0.5f) * panel.zoom, canvas_origin.y - 0.5f * panel.zoom});
        click();
    };
    const auto revision = doc.Revision();
    pick(0);
    CHECK(doc.brush == 6);
    move(*solid_checkbox);
    click();
    CHECK(!doc.GetTileset().IsSolid(6) && doc.Revision() == revision && !doc.CanUndo());
    doc.SetSolid(6, true);
    CHECK(doc.GetTileset().solid.empty() && doc.Revision() == revision);

    pick(1);
    CHECK(doc.brush == 5);
    move(*solid_checkbox);
    click();
    CHECK(doc.GetTileset().IsSolid(5) && doc.dirty());
    CHECK(doc.Undo() && doc.GetTileset().solid.empty() && !doc.dirty() && !doc.CanUndo());

    auto stale_tiles = doc.GetTileset();
    stale_tiles.SetSolid(6, true);
    TilemapEditDocument stale(doc.Map(), std::move(stale_tiles));
    stale.SetSolid(6, false);
    CHECK(!stale.GetTileset().IsSolid(6) && stale.Undo() && stale.GetTileset().IsSolid(6));

    auto paint_only = Doc();
    paint_only.SetSolid(100, true); // texture-free numbered tiles keep working
    CHECK(paint_only.GetTileset().IsSolid(100));
}

AETHER_TEST(TilemapEditor_WheelZoomKeepsTheHoveredCellUnderTheCursor) {
    HeadlessImGui ui;
    auto doc = Doc();
    doc.brush = 3;
    TilemapPanel panel(doc);
    ImVec2 bottom_left;
    const auto draw = [&] {
        panel.Draw();
        const auto& padding = ImGui::GetStyle().WindowPadding;
        bottom_left = {ImGui::GetItemRectMin().x + padding.x + 8,
                       ImGui::GetItemRectMax().y - padding.y - 8};
    };
    ui.Frame(draw);
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(bottom_left.x + 5.5f * panel.zoom, bottom_left.y - 4.5f * panel.zoom);
    ui.Frame(draw);
    const f32 starting_zoom = panel.zoom;
    const auto wheel = [&](f32 delta) {
        io.AddMouseWheelEvent(0.0f, delta);
        ui.Frame(draw);
    };
    const auto click = [&] {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        ui.Frame(draw);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        ui.Frame(draw);
    };
    const u64 revision = doc.Revision();
    wheel(1.0f);
    wheel(1.0f);
    const f32 two_notches = panel.zoom;
    CHECK(std::abs(two_notches - starting_zoom * 1.15f * 1.15f) < 0.01f);
    wheel(-2.0f);
    CHECK(std::abs(panel.zoom - starting_zoom) < 0.01f);
    wheel(2.0f);
    CHECK(std::abs(panel.zoom - two_notches) < 0.01f);
    wheel(-2.0f);
    wheel(0.25f);
    CHECK(std::abs(panel.zoom - starting_zoom * std::pow(1.15f, 0.25f)) < 0.01f);
    CHECK(doc.Revision() == revision && !doc.dirty() && !doc.CanUndo());
    click();
    CHECK(doc.Map().Get(0, 5, 4) == 3 && doc.Map().Get(0, 4, 3) == kEmpty);
    CHECK(doc.Undo() && doc.Map().Get(0, 5, 4) == kEmpty && !doc.CanUndo());

    wheel(-0.25f);
    CHECK(std::abs(panel.zoom - starting_zoom) < 0.01f);
    wheel(100.0f);
    CHECK(panel.zoom == 64.0f);
    wheel(-100.0f);
    CHECK(panel.zoom == 6.0f && !doc.dirty() && !doc.CanUndo() && doc.CanRedo());
    click();
    CHECK(doc.Map().Get(0, 5, 4) == 3);
}

AETHER_TEST(TilemapEditor_ZoomSliderKeepsTheFocusedCellInView) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(128, 96);
    map.AddLayer("Ground");
    TilemapEditDocument doc(std::move(map), {});
    doc.brush = 3;
    TilemapPanel panel(doc);
    ImVec2 canvas_center;
    ImGuiID zoom_id = 0;
    ImVec2 toolbar;
    f32 canvas_top = 0;
    const auto draw = [&] {
        const auto* parent = ImGui::GetCurrentWindowRead();
        const auto id = ImGui::GetID("##canvas");
        zoom_id = ImGui::GetID("Zoom");
        toolbar = ImGui::GetCursorScreenPos();
        panel.Draw();
        canvas_top = ImGui::GetItemRectMin().y;
        for (const auto* child : parent->DC.ChildWindows) {
            if (child->ChildId != id) continue;
            const auto start = child->DC.CursorStartPos;
            canvas_center = {(start.x + child->WorkRect.Max.x) * 0.5f, (start.y + child->WorkRect.Max.y) * 0.5f};
        }
    };
    auto& io = ImGui::GetIO();
    const auto move = [&](f32 x, f32 y) { io.AddMousePosEvent(x, y); ui.Frame(draw); };
    const auto click = [&] {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        ui.Frame(draw);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        ui.Frame(draw);
    };
    CHECK(panel.FocusCell(0, 90, 70));
    ui.Frame(draw);
    bool found = false;
    for (f32 y = toolbar.y + 4; !found && y < canvas_top; y += 8) {
        for (f32 x = toolbar.x + 5; x < toolbar.x + 950; x += 12) {
            move(x, y);
            if (ImGui::GetHoveredID() != zoom_id) continue;
            click();
            found = true;
            break;
        }
    }
    CHECK(found && panel.zoom != 24.0f && !doc.dirty() && !doc.CanUndo());
    move(canvas_center.x, canvas_center.y);
    click();
    CHECK(doc.Map().Get(0, 90, 70) == 3 && doc.CanUndo());
    CHECK(doc.Map().Get(0, 89, 70) == kEmpty && doc.Map().Get(0, 91, 70) == kEmpty);
    CHECK(doc.Undo() && !doc.dirty());
}

AETHER_TEST(TilemapEditor_ResetViewRestoresOriginWithoutChangingHistory) {
    HeadlessImGui ui;
    auto doc = Doc();
    TilemapPanel panel(doc);
    doc.brush = 3;
    ImGuiID reset_id = 0;
    ImVec2 toolbar, canvas_min, bottom_left;
    const auto draw = [&] {
        toolbar = ImGui::GetCursorScreenPos();
        reset_id = ImGui::GetID("Reset view");
        panel.Draw();
        canvas_min = ImGui::GetItemRectMin();
        const auto padding = ImGui::GetStyle().WindowPadding;
        bottom_left = {canvas_min.x + padding.x + 8.0f, ImGui::GetItemRectMax().y - padding.y - 8.0f};
    };
    ui.Frame(draw);
    const f32 starting_zoom = panel.zoom;
    const ImVec2 cell{bottom_left.x + 1.5f * starting_zoom, bottom_left.y - 1.5f * starting_zoom};
    auto& io = ImGui::GetIO();
    const auto move = [&](ImVec2 where) {
        io.AddMousePosEvent(where.x, where.y);
        ui.Frame(draw);
    };
    const auto button = [&](ImGuiMouseButton which, bool down) {
        io.AddMouseButtonEvent(which, down);
        ui.Frame(draw);
    };
    const auto click = [&] { button(ImGuiMouseButton_Left, true); button(ImGuiMouseButton_Left, false); };
    const u64 revision = doc.Revision();
    move(cell);
    io.AddMouseWheelEvent(0.0f, 2.0f);
    ui.Frame(draw);
    CHECK(panel.zoom > starting_zoom);
    button(ImGuiMouseButton_Middle, true);
    move({cell.x + 300.0f, cell.y - 120.0f});
    button(ImGuiMouseButton_Middle, false);

    move(cell);
    click();
    CHECK(doc.Revision() == revision && !doc.CanUndo()); // the map was panned away

    bool found = false;
    for (f32 y = toolbar.y + 2.0f; !found && y < canvas_min.y; y += 4.0f) {
        for (f32 x = toolbar.x + 4.0f; x < toolbar.x + 850.0f; x += 14.0f) {
            move({x, y});
            if (ImGui::GetHoveredID() == reset_id) { found = true; break; }
        }
    }
    if (!found) { CHECK(false); return; }
    click();
    CHECK(panel.zoom == starting_zoom && doc.Revision() == revision && !doc.dirty());
    CHECK(!doc.CanUndo() && !doc.CanRedo() && doc.brush == 3);
    move(cell);
    click();
    CHECK(doc.Map().Get(0, 1, 1) == 3);
    CHECK(doc.Undo() && !doc.dirty() && !doc.CanUndo());
}

AETHER_TEST(TilemapEditor_FastPointerDragsPaintAndEraseEveryCellAsOneStroke) {
    HeadlessImGui ui;
    TilemapEditDocument doc = Doc();
    TilemapPanel panel(doc);
    doc.brush = 3;
    ImVec2 bottom_left;
    const auto draw = [&] {
        panel.Draw();
        const ImVec2 corner = {ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y};
        const ImVec2 padding = ImGui::GetStyle().WindowPadding;
        bottom_left = ImVec2(corner.x + padding.x + 8, corner.y - padding.y - 8);
    };
    ui.Frame(draw);
    ImGuiIO& io = ImGui::GetIO();
    const auto move = [&](i32 x, i32 y) {
        io.AddMousePosEvent(bottom_left.x + (static_cast<f32>(x) + 0.5f) * panel.zoom,
                            bottom_left.y - (static_cast<f32>(y) + 0.5f) * panel.zoom);
        ui.Frame(draw);
    };
    const auto button = [&](ImGuiMouseButton which, bool down) {
        io.AddMouseButtonEvent(which, down);
        ui.Frame(draw);
    };
    const auto row = [&](i32 value) {
        for (i32 x = 0; x < 6; ++x) CHECK(doc.Map().Get(0, x, 1) == value);
    };

    move(0, 1);
    button(ImGuiMouseButton_Left, true);
    move(5, 1); // only the beginning and end arrive as mouse events
    button(ImGuiMouseButton_Left, false);
    CHECK(doc.Map().Get(0, 0, 1) == 3 && doc.Map().Get(0, 5, 1) == 3);
    row(3);
    CHECK(doc.Undo() && !doc.CanUndo());
    row(kEmpty);
    CHECK(doc.Redo());
    row(3);

    button(ImGuiMouseButton_Right, true);
    move(0, 1);
    button(ImGuiMouseButton_Right, false);
    row(kEmpty);
    CHECK(doc.Undo());
    row(3);

    // Crossing empty space outside the map must not draw a connector on re-entry.
    move(0, 3);
    button(ImGuiMouseButton_Left, true);
    move(6, 3);
    move(5, 3);
    button(ImGuiMouseButton_Left, false);
    CHECK(doc.Map().Get(0, 0, 3) == 3 && doc.Map().Get(0, 5, 3) == 3);
    for (i32 x = 1; x < 5; ++x) CHECK(doc.Map().Get(0, x, 3) == kEmpty);
}

AETHER_TEST(TilemapEditor_FreehandIncludesMotionQueuedBeforeButtonRelease) {
    HeadlessImGui ui;
    TilemapEditDocument doc = Doc();
    TilemapPanel panel(doc);
    doc.brush = 3;
    ImVec2 bottom_left;
    const auto draw = [&] {
        panel.Draw();
        const ImVec2 corner = {ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y};
        const ImVec2 padding = ImGui::GetStyle().WindowPadding;
        bottom_left = ImVec2(corner.x + padding.x + 8, corner.y - padding.y - 8);
    };
    ui.Frame(draw);
    ImGuiIO& io = ImGui::GetIO();
    const auto queue_pos = [&](i32 x, i32 y = 1) {
        io.AddMousePosEvent(bottom_left.x + (static_cast<f32>(x) + 0.5f) * panel.zoom,
                            bottom_left.y - (static_cast<f32>(y) + 0.5f) * panel.zoom);
    };
    const auto drag = [&](i32 start, i32 end, ImGuiMouseButton button) {
        queue_pos(start);
        ui.Frame(draw);
        io.AddMouseButtonEvent(button, true);
        ui.Frame(draw);
        queue_pos(end); // Motion and release can be picked up on the same frame.
        io.AddMouseButtonEvent(button, false);
        ui.Frame(draw);
        CHECK(!ImGui::IsMouseDown(button));
    };
    const auto row_is = [&](i32 value) {
        for (i32 x = 0; x < 6; ++x) CHECK(doc.Map().Get(0, x, 1) == value);
    };

    drag(0, 5, ImGuiMouseButton_Left);
    row_is(3);
    CHECK(doc.Undo() && !doc.CanUndo());
    row_is(kEmpty);
    CHECK(doc.Redo());
    row_is(3);

    doc.tool = TileTool::Rectangle;
    drag(5, 0, ImGuiMouseButton_Right);
    row_is(kEmpty);
    CHECK(doc.tool == TileTool::Rectangle && doc.Undo());
    row_is(3);

    doc.tool = TileTool::Paint;
    queue_pos(0, 2);
    ui.Frame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    ui.Frame(draw);
    queue_pos(7, 2); // Remember that the stroke left the map.
    ui.Frame(draw);
    queue_pos(5, 2);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    ui.Frame(draw);
    CHECK(doc.Map().Get(0, 0, 2) == 3 && doc.Map().Get(0, 5, 2) == 3);
    for (i32 x = 1; x < 5; ++x) CHECK(doc.Map().Get(0, x, 2) == kEmpty);
    CHECK(doc.Undo());

    queue_pos(0, 3);
    ui.Frame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    ui.Frame(draw);
    queue_pos(5, 3);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    io.AddFocusEvent(false); // Never complete a segment on a focus-loss release.
    ui.Frame(draw);
    CHECK(doc.Map().Get(0, 0, 3) == 3 && doc.Map().Get(0, 5, 3) == kEmpty);
    CHECK(doc.Undo() && doc.Map().Get(0, 0, 3) == kEmpty);
}

AETHER_TEST(TilemapEditor_FreehandDoesNotSwitchButtonsMidStroke) {
    HeadlessImGui ui;
    auto doc = Doc();
    TilemapPanel panel(doc);
    doc.brush = 3;
    ImVec2 origin;
    const auto draw = [&] {
        panel.Draw();
        const auto pad = ImGui::GetStyle().WindowPadding;
        origin = {ImGui::GetItemRectMin().x + pad.x + 8, ImGui::GetItemRectMax().y - pad.y - 8};
    };
    auto& io = ImGui::GetIO();
    const auto move = [&](i32 x, i32 y) {
        io.AddMousePosEvent(origin.x + (x + 0.5f) * panel.zoom, origin.y - (y + 0.5f) * panel.zoom);
        ui.Frame(draw);
    };
    const auto button = [&](ImGuiMouseButton which, bool down) {
        io.AddMouseButtonEvent(which, down);
        ui.Frame(draw);
    };
    const auto left = ImGuiMouseButton_Left;
    const auto right = ImGuiMouseButton_Right;
    ui.Frame(draw);

    move(0, 1);
    button(left, true);
    move(2, 1);
    button(right, true);
    move(4, 1);
    button(left, false);
    move(5, 1); // The still-held right button must not acquire a new stroke.
    button(right, false);
    for (i32 x = 0; x <= 4; ++x) CHECK(doc.Map().Get(0, x, 1) == 3);
    CHECK(doc.Map().Get(0, 5, 1) == kEmpty);
    CHECK(doc.Undo() && !doc.CanUndo() && doc.Redo());

    move(4, 1);
    button(right, true);
    move(3, 1);
    button(left, true);
    move(0, 1);
    button(right, false);
    move(0, 2); // Nor may the held left button start painting.
    button(left, false);
    for (i32 x = 0; x < 6; ++x) CHECK(doc.Map().Get(0, x, 1) == kEmpty);
    CHECK(doc.Map().Get(0, 0, 2) == kEmpty && doc.Undo());
    for (i32 x = 0; x <= 4; ++x) CHECK(doc.Map().Get(0, x, 1) == 3);
}

AETHER_TEST(TilemapEditor_MiddlePanRequiresItsOwnCanvasGesture) {
    HeadlessImGui ui;
    auto doc = Doc();
    TilemapPanel panel(doc);
    doc.brush = 3;
    ImVec2 origin;
    const auto draw = [&] {
        panel.Draw();
        const auto pad = ImGui::GetStyle().WindowPadding;
        origin = {ImGui::GetItemRectMin().x + pad.x + 8, ImGui::GetItemRectMax().y - pad.y - 8};
    };
    auto& io = ImGui::GetIO();
    const auto queue_pos = [&](i32 x, i32 y) {
        io.AddMousePosEvent(origin.x + (x + 0.5f) * panel.zoom, origin.y - (y + 0.5f) * panel.zoom);
    };
    const auto move = [&](i32 x, i32 y) { queue_pos(x, y); ui.Frame(draw); };
    const auto button = [&](ImGuiMouseButton which, bool down) {
        io.AddMouseButtonEvent(which, down);
        ui.Frame(draw);
    };
    const auto left = ImGuiMouseButton_Left, middle = ImGuiMouseButton_Middle;
    ui.Frame(draw);

    move(0, 1);
    button(left, true);
    button(middle, true); // A secondary button cannot pan during a paint stroke.
    move(3, 1);
    button(left, false);
    button(middle, false);
    for (i32 x = 0; x < 4; ++x) CHECK(doc.Map().Get(0, x, 1) == 3);
    CHECK(doc.Undo() && !doc.dirty());

    move(1, 1);
    button(middle, true);
    queue_pos(3, 1); // Last motion and release can queue together.
    io.AddMouseButtonEvent(middle, false);
    ui.Frame(draw);
    CHECK(!doc.dirty()); // Panning never creates an edit.
    move(2, 0); // The board moved right two tiles, so this is now cell (0, 0).
    button(left, true);
    button(left, false);
    CHECK(doc.Map().Get(0, 0, 0) == 3 && doc.Map().Get(0, 2, 0) == kEmpty);
}

AETHER_TEST(TilemapEditor_FreehandDragsMustStartOnTheMap) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(6, 5);
    map.AddLayer("Ground");
    map.Set(0, 3, 2, 2);
    TilemapEditDocument doc(std::move(map), Doc().GetTileset());
    TilemapPanel panel(doc);
    doc.brush = 3;
    ImVec2 bottom_left;
    const auto draw = [&] {
        panel.Draw();
        const ImVec2 corner = {ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y};
        const ImVec2 padding = ImGui::GetStyle().WindowPadding;
        bottom_left = ImVec2(corner.x + padding.x + 8, corner.y - padding.y - 8);
    };
    ui.Frame(draw);
    ImGuiIO& io = ImGui::GetIO();
    const auto move = [&](i32 x) {
        io.AddMousePosEvent(bottom_left.x + (static_cast<f32>(x) + .5f) * panel.zoom,
                            bottom_left.y - 2.5f * panel.zoom);
        ui.Frame(draw);
    };
    const auto drag = [&](ImGuiMouseButton button, i32 target) {
        move(7); // Blank canvas, beyond the edge of the map.
        io.AddMouseButtonEvent(button, true);
        ui.Frame(draw);
        move(target);
        io.AddMouseButtonEvent(button, false);
        ui.Frame(draw);
    };
    const auto revision = doc.Revision();
    drag(ImGuiMouseButton_Left, 1);
    CHECK(doc.Map().Get(0, 1, 2) == kEmpty);
    drag(ImGuiMouseButton_Right, 3);
    CHECK(doc.Map().Get(0, 3, 2) == 2);
    CHECK(doc.Revision() == revision && !doc.dirty() && !doc.CanUndo());
}

AETHER_TEST(TilemapEditor_FillAndPickUseOnlyTheClickedCell) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(6, 5);
    map.AddLayer("Ground");
    for (i32 y = 0; y < 5; ++y)
        for (i32 x = 3; x < 6; ++x) map.Set(0, x, y, 2);
    TilemapEditDocument doc(std::move(map), Doc().GetTileset());
    TilemapPanel panel(doc);
    doc.tool = TileTool::Fill;
    doc.brush = 3;
    ImVec2 bottom_left;
    const auto draw = [&] {
        panel.Draw();
        const ImVec2 corner = {ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y};
        const ImVec2 padding = ImGui::GetStyle().WindowPadding;
        bottom_left = ImVec2(corner.x + padding.x + 8, corner.y - padding.y - 8);
    };
    ui.Frame(draw);
    ImGuiIO& io = ImGui::GetIO();
    const auto move = [&](i32 x, i32 y) {
        io.AddMousePosEvent(bottom_left.x + (static_cast<f32>(x) + 0.5f) * panel.zoom,
                            bottom_left.y - (static_cast<f32>(y) + 0.5f) * panel.zoom);
        ui.Frame(draw);
    };
    const auto left = [&](bool down) {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        ui.Frame(draw);
    };
    move(7, 2); // Canvas but outside the map.
    left(true);
    move(1, 2);
    left(false);
    CHECK(doc.Map().Get(0, 1, 2) == kEmpty && !doc.CanUndo());
    if (doc.CanUndo()) doc.Undo();

    move(1, 2);
    left(true);
    move(4, 2); // A different flood region under the still-held button.
    left(false);
    CHECK(doc.Map().Get(0, 0, 0) == 3 && doc.Map().Get(0, 4, 2) == 2);
    CHECK(doc.Undo() && !doc.CanUndo() && doc.Map().Get(0, 1, 2) == kEmpty);

    doc.tool = TileTool::Pick;
    doc.brush = 3;
    move(4, 2);
    left(true);
    move(1, 2);
    left(false);
    CHECK(doc.brush == 2 && !doc.CanUndo() && doc.CanRedo());
    doc.brush = 5;
    move(7, 2);
    left(true);
    move(4, 2);
    left(false);
    CHECK(doc.brush == 5 && !doc.CanUndo() && doc.CanRedo());
}

AETHER_TEST(TilemapEditor_JumpProofOverlayIsReadOnlyAndKeepsTheLastValidTrace) {
    HeadlessImGui ui;
    TilemapEditDocument doc = Doc();
    TilemapPanel panel(doc);
    const auto file = std::filesystem::temp_directory_path() / "jump-proof-overlay.json";
    const auto draw = [&] { panel.Draw(); };
    const u64 rev = doc.Revision();
    ui.Frame(draw);
    const int plain_vertices = ImGui::GetDrawData()->TotalVtxCount;
    {
        std::ofstream out(file);
        out << R"({"frames":3,"first_difference_frame":2,"before_path":[[1,1,0],[2,2,0],[3,1,0]],"after_path":[[1,1,0],[2,1.5,0],[2.5,1,0]]})";
    }
    std::string error;
    CHECK(panel.LoadJumpProofOverlay(file, &error));
    CHECK(error.empty());
    ui.Frame(draw);
    const int overlay_vertices = ImGui::GetDrawData()->TotalVtxCount;
    CHECK(overlay_vertices > plain_vertices);
    CHECK(doc.Revision() == rev && !doc.dirty() && !doc.CanUndo());
    {
        std::ofstream out(file);
        out << R"({"frames":1,"first_difference_frame":2,"before_path":[[1,1,0]],"after_path":[[1,1,0]]})";
    }
    CHECK(!panel.LoadJumpProofOverlay(file, &error) && !error.empty());
    ui.Frame(draw);
    CHECK(ImGui::GetDrawData()->TotalVtxCount == overlay_vertices);
    CHECK(doc.Revision() == rev && !doc.dirty());
}

AETHER_TEST(TilemapEditor_JumpProofShowsTargetVerdictWithoutPathDivergence) {
    HeadlessImGui ui;
    TilemapEditDocument doc = Doc();
    TilemapPanel panel(doc);
    const auto root = std::filesystem::temp_directory_path();
    const auto file = root / "jump-proof-outcome.json";
    const auto capture = root / "jump-proof-outcome-ui.txt";
    const auto draw_text = [&] {
        std::filesystem::remove(capture);
        ui.Frame([&] {
            ImGui::GetStateStorage()->SetInt(ImGui::GetID("Jump Proof overlay"), 1);
            ImGui::LogToFile(0, capture.string().c_str());
            panel.Draw();
            ImGui::LogFinish();
        });
        std::ifstream in(capture);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    std::ofstream(file) << R"({"frames":1,"first_difference_frame":null,"before_status":"pass","after_status":"missed","verdict_changed":true,"before_path":[[1,1,0]],"after_path":[[1,1,0]]})";
    const u64 revision = doc.Revision();
    CHECK(panel.LoadJumpProofOverlay(file));
    auto text = draw_text();
    CHECK(text.find("Target result: pass -> missed (changed)") != std::string::npos);
    CHECK(text.find("First difference: frame") == std::string::npos);
    std::ofstream(file) << R"({"frames":1,"first_difference_frame":null,"before_status":"pass","before_path":[[1,1,0]],"after_path":[[1,1,0]]})";
    std::string error;
    CHECK(!panel.LoadJumpProofOverlay(file, &error) && !error.empty());
    CHECK(draw_text().find("Target result: pass -> missed (changed)") != std::string::npos);
    std::ofstream(file) << R"({"frames":1,"first_difference_frame":null,"before_status":"pass","after_status":"missed","verdict_changed":false,"before_path":[[1,1,0]],"after_path":[[1,1,0]]})";
    CHECK(!panel.LoadJumpProofOverlay(file, &error) && !error.empty());
    std::ofstream(file) << R"({"frames":1,"first_difference_frame":null,"before_path":[[1,1,0]],"after_path":[[1,1,0]]})";
    CHECK(panel.LoadJumpProofOverlay(file));
    CHECK(draw_text().find("Target result:") == std::string::npos);
    CHECK(doc.Revision() == revision && !doc.dirty() && !doc.CanUndo());
}

AETHER_TEST(TilemapEditor_JumpProofMarksOnlyTheMatchingChangedCell) {
    HeadlessImGui ui;
    TilemapEditDocument doc = Doc();
    TilemapPanel panel(doc);
    doc.brush = 3;
    doc.Apply(1, 1);
    const auto folder = std::filesystem::temp_directory_path() / "jump-proof-marked-map";
    const auto map = folder / "Content/Tiles/Level.atilemap";
    const auto tileset = folder / "Content/Tiles/Level.atileset";
    if (!doc.Save(map, tileset)) { CHECK(false); return; }
    const auto file = folder / "overlay.json";
    {
        std::ofstream out(file);
        out << R"({"frames":2,"first_difference_frame":2,"before_path":[[1,1,0],[2,2,0]],"after_path":[[1,1,0],[2,1.5,0]],"asset":"Content/Tiles/Level.atilemap","cell":{"layer":0,"x":1,"y":1,"before":-1,"after":3}})";
    }
    if (!panel.LoadJumpProofOverlay(file)) { CHECK(false); return; }
    const auto marked = [&] {
        ui.Frame([&] { panel.Draw(); });
        const auto* draw = ImGui::GetDrawData();
        for (const auto* list : draw->CmdLists)
            for (const auto& vertex : list->VtxBuffer)
                if (vertex.col == IM_COL32(138, 255, 133, 255)) return true;
        return false;
    };
    const u64 revision = doc.Revision();
    CHECK(marked());
    CHECK(doc.Revision() == revision && !doc.dirty());
    doc.brush = 4;
    doc.Apply(1, 1);
    CHECK(!marked()); // changed again since the package under comparison
    CHECK(doc.Undo() && marked());
    CHECK(doc.Save(folder / "Content/Tiles/Other.atilemap", tileset) && !marked());
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

AETHER_TEST(TilemapEditor_OpensTheAssetNamedByAStrictBuildWarning) {
    HeadlessImGui ui;
    NewProjectPanel starter;
    starter.template_id = "platformer_2d";
    starter.location = (std::filesystem::temp_directory_path() / "aether_warning_asset").string();
    starter.name = "WarningAsset";
    std::string error;
    if (!starter.Create(&error)) { CHECK(false); return; }
    const std::string relative = "Tiles/Level1.atilemap";
    const auto source = ProjectPaths::ForFile(starter.Created()).content / relative;
    TilemapData map;
    if (!LoadTilemap(source, map, &error)) { CHECK(false); return; }
    map.Resize(96, 88);
    const auto layer = map.AddLayer("Distant");
    map.Set(layer, 85, 78, AutotileCell(999));
    if (!SaveTilemap(source, map, &error)) { CHECK(false); return; }
    EditorWorkspace ws(starter.Created());
    auto* build = ws.PackageWindow();
    if (!build) { CHECK(false); return; }
    build->configuration = cook::BuildConfiguration::Shipping;
    build->strict_validation = true;
    build->player_path = AETHER_TEST_PLAYER_PATH;
    if (!build->StartPackage()) { CHECK(false); return; }
    build->WaitForBuild();
    CHECK(build->LastResult() == BuildPackageWindow::Result::Failed);
    const auto warnings = build->Log();
    const auto hit = std::find_if(warnings.begin(), warnings.end(), [&](const auto& line) {
        return line.warning && line.text.starts_with(relative + ": layer " + std::to_string(layer) + " cell (85, 78) uses missing autotile rule 999");
    });
    if (hit == warnings.end()) { CHECK(false); return; }
    CHECK(build->WarningAsset(*hit) == relative);
    CHECK(build->WarningAsset({false, hit->text}).empty());
    CHECK(build->WarningAsset({true, "../Tiles/Level1.atilemap: bad"}).empty());
    CHECK(build->WarningAsset({true, "Tiles/Unknown.atilemap: missing"}).empty());
    CHECK(build->WarningAsset({true, "Failed: " + hit->text}).empty());
    const auto capture = std::filesystem::temp_directory_path() / "build-warning-open-asset.txt";
    ui.Frame([&] { ImGui::LogToFile(0, capture.string().c_str()); build->Draw(); ImGui::LogFinish(); });
    std::ifstream in(capture);
    const std::string log(std::istreambuf_iterator<char>(in), {});
    CHECK(log.find("Open asset") != std::string::npos);
    if (!build->OpenWarningAsset(*hit, &error)) { CHECK(false); return; }
    CHECK(ws.Tilemap().TilemapFile() == source && ws.Tilemap().Map().Get(layer, 85, 78) == AutotileCell(999));
    CHECK(ws.Tilemap().active_layer == layer);
    CHECK(ws.FindTool("Tilemap") == static_cast<i64>(ws.Selected()));
    ui.Frame([&] { ws.DrawTool(ws.Selected()); });
    usize marks = 0;
    for (const auto* list : ImGui::GetDrawData()->CmdLists)
        for (const auto& vertex : list->VtxBuffer)
            if (vertex.col == IM_COL32(255, 96, 215, 255)) ++marks;
    CHECK(marks == 8); // The only bad cell is offscreen without the warning jump.
    CHECK(!ws.Tilemap().dirty() && !ws.Tilemap().CanUndo() && build->StagedGame().empty());
}

AETHER_TEST(TilemapEditor_SavedCheckpointFollowsUndoAndRedo) {
    auto doc = Doc();
    const auto dir = std::filesystem::temp_directory_path() / "aether_tilemap_saved_checkpoint";
    std::string error;
    doc.brush = 5;
    doc.BeginStroke();
    doc.Apply(2, 2);
    const auto revision = doc.Revision();
    if (!doc.Save(dir / "a.atilemap", dir / "a.atileset", &error)) { CHECK(false); return; }
    CHECK(!doc.dirty() && doc.Revision() > revision);
    const auto saved_revision = doc.Revision();
    CHECK(doc.Undo() && doc.dirty() && doc.Map().Get(0, 2, 2) == kEmpty);
    CHECK(doc.Redo() && !doc.dirty() && doc.Revision() > saved_revision);
    doc.SetSolid(5, true);
    doc.brush = 6;
    doc.Apply(2, 2);
    std::ofstream(dir / "occupied") << "a file, not a directory";
    CHECK(!doc.Save(dir / "a.atilemap", dir / "occupied" / "a.atileset", &error));
    CHECK(doc.dirty() && doc.Undo() && doc.dirty());
    CHECK(doc.Undo() && !doc.dirty() && !doc.GetTileset().IsSolid(5));
    const auto before_no_op = doc.Revision();
    doc.brush = 5;
    doc.Apply(2, 2);
    CHECK(!doc.dirty() && doc.Revision() == before_no_op && doc.CanRedo());
    doc.SetSolid(7, true);
    CHECK(doc.dirty() && !doc.CanRedo());
    CHECK(doc.Undo() && !doc.dirty() && !doc.GetTileset().IsSolid(7));
    CHECK(doc.Load(dir / "a.atilemap", dir / "a.atileset", &error));
    CHECK(!doc.dirty() && !doc.CanUndo() && !doc.CanRedo());
}

AETHER_TEST(TilemapEditor_PackagesAfterUndoToSavedMap) {
    NewProjectPanel starter;
    starter.template_id = "platformer_2d";
    starter.location = (std::filesystem::temp_directory_path() / "aether_platformer_tilemap_undo").string();
    starter.name = "TilemapUndo";
    std::string error;
    if (!starter.Create(&error)) { CHECK(false); return; }
    EditorWorkspace ws(starter.Created());
    auto* build = ws.PackageWindow();
    if (!build || !ws.OpenAsset("Tiles/Level1.atilemap", &error)) { CHECK(false); return; }
    build->player_path = AETHER_TEST_PLAYER_PATH;
    auto& doc = ws.Tilemap();
    doc.brush = AutotileCell(0);
    doc.Apply(20, 1);
    CHECK(doc.dirty() && !build->StartPackage());
    if (!doc.Undo() || !build->StartPackage()) { CHECK(false); return; }
    build->WaitForBuild();
    CHECK(!doc.dirty() && build->LastResult() == BuildPackageWindow::Result::Succeeded);
    pak::PakReader reader;
    if (!reader.Open((build->StagedGame().parent_path() / "Paks/Game.apak").string(), &error)) { CHECK(false); return; }
    std::string contents;
    TilemapData cooked_map;
    CHECK(reader.ReadText("Content/Tiles/Level1.atilemap", contents) &&
          TilemapFromJson(nlohmann::json::parse(contents), cooked_map) &&
          cooked_map.Get(0, 20, 1) == kEmpty);
}

AETHER_TEST(TilemapEditor_SavesPlatformerEditsBeforePackaging) {
    HeadlessImGui ui;
    NewProjectPanel starter;
    starter.template_id = "platformer_2d";
    starter.location = (std::filesystem::temp_directory_path() / "aether_platformer_tilemap").string();
    starter.name = "TilemapPlatformer";
    std::string error;
    if (!starter.Create(&error)) {
        CHECK(false);
        return;
    }
    EditorWorkspace ws(starter.Created());
    auto* build = ws.PackageWindow();
    if (!build || !ws.OpenAsset("Tiles/Level1.atilemap", &error)) {
        CHECK(false);
        return;
    }
    build->player_path = AETHER_TEST_PLAYER_PATH;
    TilemapEditDocument& doc = ws.Tilemap();
    doc.brush = AutotileCell(0);
    doc.Apply(20, 1); // extend the ground into the pit
    doc.SetSolid(1, true);
    CHECK(doc.Map().Get(0, 20, 1) == doc.brush && doc.dirty());
    const bool started = build->StartPackage();
    CHECK(!started);
    CHECK(!build->StartCook());
    if (started) {
        build->WaitForBuild();
        return;
    }

    // Use the actual Tilemap panel's Save button, not the document's save API.
    TilemapPanel panel(doc);
    ImVec2 save_position;
    const auto draw = [&] {
        const ImVec2 top = ImGui::GetCursorScreenPos();
        save_position = ImVec2(top.x + 12.0f, top.y + 8.0f);
        panel.Draw();
    };
    ui.Frame(draw);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(save_position.x, save_position.y);
    ui.Frame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    ui.Frame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    ui.Frame(draw);
    CHECK(!doc.dirty());
    TilemapData saved;
    CHECK(LoadTilemap(doc.TilemapFile(), saved, &error));
    CHECK(saved.Get(0, 20, 1) == AutotileCell(0));
    if (!build->StartPackage()) {
        CHECK(false);
        return;
    }
    build->WaitForBuild();
    CHECK(build->LastResult() == BuildPackageWindow::Result::Succeeded);
    {
        pak::PakReader reader;
        if (!reader.Open((build->StagedGame().parent_path() / "Paks/Game.apak").string(), &error)) {
            CHECK(false);
            return;
        }
        std::string contents;
        TilemapData cooked_map;
        Tileset cooked_tileset;
        CHECK(reader.ReadText("Content/Tiles/Level1.atilemap", contents) &&
              TilemapFromJson(nlohmann::json::parse(contents), cooked_map) &&
              cooked_map.Get(0, 20, 1) == AutotileCell(0));
        CHECK(reader.ReadText("Content/Tiles/Level.atileset", contents) &&
              TilesetFromJson(nlohmann::json::parse(contents), cooked_tileset) &&
              cooked_tileset.IsSolid(1));
    }
    if (!build->StartSmoke()) { CHECK(false); return; }
    build->WaitForBuild();
    CHECK(build->LastResult() == BuildPackageWindow::Result::Succeeded);
    CHECK(build->Stage() == "Headless smoke test passed (3 frames)");
    build->launch_args = "--headless --frames 2";
    if (!build->Launch(&error)) {
        CHECK(false);
        return;
    }
    int code = -1;
    CHECK(build->Game().Wait(&code) && code == 0);
}

AETHER_TEST(TilemapEditor_EditorQuitProtectsUnsavedProjectMap) {
    NewProjectPanel starter;
    starter.location = (std::filesystem::temp_directory_path() / "aether_tilemap_quit").string();
    starter.name = "TilemapQuit";
    starter.template_id = "platformer_2d";
    std::string error;
    if (!starter.Create(&error)) { CHECK(false); return; }
    EditorWorkspace ws(starter.Created());
    int quits = 0;
    ws.on_quit_confirmed = [&] { ++quits; };
    if (!ws.OpenAsset("Tiles/Level1.atilemap", &error)) { CHECK(false); return; }
    auto& doc = ws.Tilemap();
    CHECK(!ws.HasUnsavedChanges());
    doc.brush = AutotileCell(0);
    doc.BeginStroke();
    doc.Apply(20, 1); // The window can close before the brush button is released.
    CHECK(doc.dirty() && !ws.ActiveDocument().IsDirty() && !doc.CanUndo());
    CHECK(ws.HasUnsavedChanges());
    ws.RequestQuit();
    if (!ws.QuitPromptOpen() || quits != 0) { CHECK(false); return; }
    ws.CancelQuit();
    CHECK(doc.dirty() && quits == 0);
    ws.RequestQuit();

    const auto tileset = doc.TilesetFile();
    const auto held = std::filesystem::path(tileset.string() + ".fixture");
    std::filesystem::rename(tileset, held);
    std::filesystem::create_directory(tileset);
    CHECK(!ws.SaveAndQuit() && ws.QuitPromptOpen() && quits == 0 && doc.dirty());
    std::filesystem::remove(tileset);
    std::filesystem::rename(held, tileset);

    CHECK(ws.SaveAndQuit() && !ws.QuitPromptOpen() && quits == 1 && !doc.dirty());
    TilemapData saved;
    CHECK(LoadTilemap(doc.TilemapFile(), saved, &error) && saved.Get(0, 20, 1) == AutotileCell(0));
}

AETHER_TEST(TilemapEditor_ProjectSwitchProtectsAndClearsOldMap) {
    NewProjectPanel starter;
    starter.location = (std::filesystem::temp_directory_path() / "aether_tilemap_switch").string();
    starter.template_id = "platformer_2d";
    starter.name = "First Map";
    std::string error;
    if (!starter.Create(&error)) { CHECK(false); return; }
    const auto first = starter.Created();
    starter.name = "Second Map";
    if (!starter.Create(&error)) { CHECK(false); return; }
    const auto second = starter.Created();

    EditorWorkspace ws(first);
    if (!ws.OpenAsset("Tiles/Level1.atilemap", &error)) { CHECK(false); return; }
    const auto old_map = ws.Tilemap().TilemapFile();
    ws.Tilemap().brush = AutotileCell(0);
    ws.Tilemap().Apply(20, 1);
    if (ws.OpenProject(second)) { CHECK(false); return; }
    CHECK(ws.ProjectFile() == first && ws.Tilemap().TilemapFile() == old_map && ws.Tilemap().dirty());
    CHECK(ws.Tilemap().Map().Get(0, 20, 1) == AutotileCell(0));
    CHECK(ws.Tilemap().Save(&error));
    CHECK(ws.OpenProject(second) && ws.ProjectFile() == second);
    CHECK(ws.Tilemap().TilemapFile().empty() && !ws.Tilemap().dirty());
    CHECK(ws.OpenAsset("Tiles/Level1.atilemap", &error) && ws.Tilemap().TilemapFile() != old_map);
}

AETHER_TEST(TilemapEditor_AutotileMaskUndoKeepsUnrelatedTilesetStorage) {
    TilemapData map;
    map.Resize(3, 3);
    map.AddLayer("Ground");
    Tileset tiles;
    tiles.texture_width = tiles.texture_height = 32;
    tiles.SetSolid(3, true);
    std::array<i32, 16> river;
    river.fill(kEmpty);
    river[kEast] = 1;
    std::array<i32, 16> road;
    road.fill(0);
    const auto river_rule = tiles.AddAutotile("River", river);
    const auto road_rule = tiles.AddAutotile("Road", road);
    map.Set(0, 1, 1, AutotileCell(river_rule));
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    const auto* rules = doc.GetTileset().autotiles.data();
    const auto* solid = doc.GetTileset().solid.data();
    const auto assert_unchanged = [&] {
        CHECK(doc.GetTileset().autotiles.data() == rules && doc.GetTileset().solid.data() == solid);
        CHECK(doc.GetTileset().autotiles[road_rule].tiles == road && doc.GetTileset().IsSolid(3));
        CHECK(doc.Map().Get(0, 1, 1) == AutotileCell(river_rule));
    };

    CHECK(doc.SetAutotileTile(river_rule, kNorth, 3));
    auto changed = river;
    changed[kNorth] = 3;
    CHECK(doc.FillEmptyAutotileMasks(river_rule, 2));
    auto complete = changed;
    std::replace(complete.begin(), complete.end(), kEmpty, 2);
    CHECK(doc.GetTileset().autotiles[river_rule].tiles == complete);
    assert_unchanged();
    CHECK(doc.Undo() && doc.GetTileset().autotiles[river_rule].tiles == changed);
    assert_unchanged();
    CHECK(doc.Undo() && !doc.dirty() && doc.GetTileset().autotiles[river_rule].tiles == river);
    assert_unchanged();
    CHECK(doc.Redo() && doc.GetTileset().autotiles[river_rule].tiles == changed);
    CHECK(doc.Redo() && doc.GetTileset().autotiles[river_rule].tiles == complete);
    assert_unchanged();
}

AETHER_TEST(TilemapEditor_RenamesAutotileRulesWithoutChangingPaintedCells) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(6, 5);
    map.AddLayer("Ground");
    Tileset tiles;
    tiles.texture_width = tiles.texture_height = 32;
    std::array<i32, 16> variants{};
    variants[kEast] = 3;
    const auto rule = tiles.AddAutotile("River", variants);
    tiles.AddAutotile("Road", variants);
    tiles.SetSolid(3, true);
    map.Set(0, 1, 1, AutotileCell(rule));
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    doc.brush = AutotileCell(rule);
    const auto* cells = doc.Map().layers[0].cells.data();
    const auto* solid = doc.GetTileset().solid.data();
    const auto revision = doc.Revision();
    CHECK(!doc.RenameAutotile(99, "Valid") && !doc.RenameAutotile(rule, " \t") &&
          !doc.RenameAutotile(rule, "River") && !doc.CanUndo());

    TilemapPanel panel(doc);
    ImGuiWindow* palette = nullptr;
    const auto draw = [&] {
        const auto* parent = ImGui::GetCurrentWindowRead();
        const auto id = ImGui::GetID("##left");
        panel.Draw();
        for (ImGuiWindow* child : parent->DC.ChildWindows)
            if (child->ChildId == id) palette = child;
    };
    ui.Frame(draw);
    if (!palette) { CHECK(false); return; }
    const ImGuiID name_seed = palette->GetID("autotile-name");
    const int first = 0;
    const ImGuiID rule_seed = ImHashData(&first, sizeof(first), name_seed);
    const ImGuiID field = ImHashStr("Rule name", 0, rule_seed);
    const auto clip = palette->InnerClipRect;
    auto& io = ImGui::GetIO();
    bool clicked = false;
    for (f32 y = clip.Min.y + 4; y < clip.Max.y; y += 6) {
        io.AddMousePosEvent(clip.Min.x + 19, y);
        ui.Frame(draw);
        if (ImGui::GetHoveredID() != field) continue;
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        ui.Frame(draw);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        ui.Frame(draw);
        clicked = true;
        break;
    }
    if (!clicked) { CHECK(false); return; }
    io.AddInputCharactersUTF8("River ## North");
    ui.Frame(draw);
    CHECK(doc.GetTileset().autotiles[rule].name == "River" && doc.Revision() == revision);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    ui.Frame(draw);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    ui.Frame(draw);
    CHECK(doc.GetTileset().autotiles[rule].name == "River ## North" && doc.Revision() == revision + 1);
    CHECK(doc.GetTileset().autotiles[rule].tiles == variants && doc.GetTileset().autotiles[1].name == "Road");
    CHECK(doc.Map().layers[0].cells.data() == cells && doc.GetTileset().solid.data() == solid);
    CHECK(doc.Map().Get(0, 1, 1) == AutotileCell(rule) && doc.brush == AutotileCell(rule));
    CHECK(doc.Undo() && doc.GetTileset().autotiles[rule].name == "River" && !doc.dirty());
    CHECK(doc.Redo() && doc.GetTileset().autotiles[rule].name == "River ## North");
}

AETHER_TEST(TilemapEditor_RenamesLayersWithoutErasingCellsOrAcceptingBlankNames) {
    TilemapEditDocument doc = Doc();
    CHECK(!doc.RenameLayer(0, "") && !doc.RenameLayer(0, " \t") && !doc.CanUndo());
    doc.brush = 5;
    doc.Apply(2, 2);
    const auto original = doc.Map().layers.front().name;
    CHECK(doc.RenameLayer(0, "Forest Floor ## A"));
    CHECK(!doc.RenameLayer(0, "Forest Floor ## A"));
    CHECK(doc.Map().layers.front().name == "Forest Floor ## A" && doc.Map().Get(0, 2, 2) == 5);
    CHECK(doc.Undo() && doc.Map().layers.front().name == original && doc.Map().Get(0, 2, 2) == 5);
    CHECK(doc.Redo() && doc.Map().layers.front().name == "Forest Floor ## A");
    const auto dir = std::filesystem::temp_directory_path() / "tilemap_layer_names";
    CHECK(doc.Save(dir/"level.atilemap", dir/"level.atileset"));
    TilemapData saved;
    if (!LoadTilemap(dir/"level.atilemap",saved)) { CHECK(false); return; }
    CHECK(saved.layers.front().name == "Forest Floor ## A" && saved.Get(0, 2, 2) == 5);
}

AETHER_TEST(TilemapEditor_LayerPropertyHistoryKeepsTilesAndSelectionInPlace) {
    TilemapData map;
    map.Resize(4, 4);
    map.AddLayer("Floor");
    map.AddLayer("Canopy");
    TilemapEditDocument doc(std::move(map), {});
    doc.active_layer = 1;
    const auto* floor_cells = doc.Map().layers[0].cells.data();
    doc.BeginStroke();
    doc.brush = 6;
    doc.Apply(2, 2);
    // Properties on another layer close the brush stroke as a distinct edit.
    CHECK(doc.RenameLayer(0, "New Floor") && doc.SetLayerCollides(0, false));
    CHECK(doc.Undo() && doc.Map().layers[0].collides && doc.Map().layers[0].name == "New Floor");
    CHECK(doc.Undo() && doc.Map().layers[0].name == "Floor" && doc.Map().Get(1, 2, 2) == 6);
    CHECK(doc.Map().layers[0].cells.data() == floor_cells && doc.active_layer == 1);
    CHECK(doc.Undo() && doc.Map().Get(1, 2, 2) == kEmpty && !doc.dirty());
    CHECK(doc.Redo() && doc.Redo() && doc.Redo());
    CHECK(doc.Map().Get(1, 2, 2) == 6 && doc.Map().layers[0].name == "New Floor");
    CHECK(doc.Map().layers[0].cells.data() == floor_cells && doc.active_layer == 1);

    // Structural history must restore the correct layer before property undo.
    CHECK(doc.MoveLayer(0, 1) && doc.Map().layers[1].name == "New Floor");
    CHECK(doc.SetLayerCollides(1, true));
    CHECK(doc.Undo() && !doc.Map().layers[1].collides);
    CHECK(doc.Undo() && doc.Map().layers[0].name == "New Floor");
    CHECK(doc.Undo() && doc.Map().layers[0].collides && doc.Map().layers[1].name == "Canopy");
    CHECK(doc.Redo() && doc.Redo() && doc.Redo());
    CHECK(doc.Map().layers[1].name == "New Floor" && doc.Map().layers[1].collides);
}

AETHER_TEST(TilemapEditor_ReordersLayersAndKeepsTheBrushOnTheSameLayer) {
    TilemapData map;
    map.Resize(2, 2);
    map.AddLayer("Floor");
    map.AddLayer("Canopy");
    map.layers[1].collides = false;
    map.Set(0, 0, 0, 2);
    map.Set(1, 0, 0, AutotileCell(0));
    TilemapEditDocument doc(std::move(map), Doc().GetTileset());

    CHECK(!doc.MoveLayer(0, 0) && !doc.MoveLayer(0, 2) && !doc.MoveLayer(2, 0));
    CHECK(!doc.CanUndo() && !doc.dirty());
    doc.BeginStroke();
    CHECK(!doc.MoveLayer(0, 1));
    doc.EndStroke();
    CHECK(doc.MoveLayer(0, 1) && doc.active_layer == 1 && doc.dirty());
    CHECK(doc.Map().layers[1].name == "Floor" && doc.Map().Get(1, 0, 0) == 2 && doc.Map().layers[1].collides);
    CHECK(doc.Map().layers[0].name == "Canopy" && doc.Map().Get(0, 0, 0) == AutotileCell(0) && !doc.Map().layers[0].collides);
    CHECK(doc.Undo() && doc.active_layer == 0 && doc.Map().layers[0].name == "Floor" && !doc.CanUndo());
    CHECK(doc.Redo() && doc.active_layer == 1 && doc.Map().layers[1].name == "Floor");
    CHECK(doc.MoveLayer(0, 1) && doc.active_layer == 0 && doc.Map().layers[0].name == "Floor");
    CHECK(doc.MoveLayer(0, 1) && doc.active_layer == 1);
    const auto dir = std::filesystem::temp_directory_path() / "tilemap_editor_reorder";
    if (!doc.Save(dir/"map.atilemap", dir/"set.atileset")) { CHECK(false); return; }
    TilemapData saved;
    if (!LoadTilemap(dir/"map.atilemap", saved)) { CHECK(false); return; }
    CHECK(TilemapToJson(saved) == TilemapToJson(doc.Map()));
}

AETHER_TEST(TilemapEditor_LayerPresenceHistoryReusesExistingTileStorage) {
    TilemapData map;
    map.Resize(4, 4);
    map.AddLayer("Floor");
    map.AddLayer("Decor");
    map.layers[1].collides = false;
    map.Set(0, 0, 0, 3);
    map.Set(1, 0, 0, 5);
    TilemapEditDocument doc(std::move(map), {});
    const auto* floor = doc.Map().layers[0].cells.data();
    const auto* decor = doc.Map().layers[1].cells.data();
    CHECK(doc.AddLayer("Temporary") == 2 && doc.active_layer == 2);
    CHECK(doc.Undo() && doc.Map().layers.size() == 2 && doc.active_layer == 0 && !doc.dirty());
    if (doc.Map().layers.size() != 2) return;
    CHECK(doc.Map().layers[0].cells.data() == floor && doc.Map().layers[1].cells.data() == decor);
    CHECK(doc.Redo() && doc.Map().layers.size() == 3 && doc.active_layer == 2);

    CHECK(doc.RemoveLayer(1) && doc.active_layer == 1);
    CHECK(doc.DuplicateLayer(0) && doc.active_layer == 1);
    if (doc.Map().layers.size() < 2) return;
    CHECK(doc.Map().layers[1].cells.data() != floor && doc.Map().Get(1, 0, 0) == 3);
    doc.brush = 7;
    doc.Apply(0, 0);
    CHECK(doc.Undo() && doc.Undo() && doc.Undo());
    CHECK(doc.Map().layers.size() == 3 && doc.active_layer == 2);
    if (doc.Map().layers.size() != 3) return;
    CHECK(doc.Map().layers[0].cells.data() == floor && doc.Map().layers[1].cells.data() == decor);
    CHECK(doc.Map().layers[1].name == "Decor" && !doc.Map().layers[1].collides && doc.Map().Get(1, 0, 0) == 5);
    CHECK(doc.Redo() && doc.Redo() && doc.Redo());
    CHECK(doc.Map().Get(1, 0, 0) == 7 && doc.Map().Get(0, 0, 0) == 3);
    CHECK(doc.Map().layers[0].cells.data() == floor);
}

AETHER_TEST(TilemapEditor_ReorderHistoryDoesNotCopyTileStorage) {
    TilemapData map;
    map.Resize(4, 4);
    map.AddLayer("A");
    map.AddLayer("B");
    map.AddLayer("C");
    TilemapEditDocument doc(std::move(map), {});
    const auto* a_cells = doc.Map().layers[0].cells.data();
    const auto* b_cells = doc.Map().layers[1].cells.data();
    const auto* c_cells = doc.Map().layers[2].cells.data();
    doc.active_layer = 1;
    CHECK(doc.MoveLayer(0, 2));
    CHECK(doc.active_layer == 0 && doc.Map().layers[2].cells.data() == a_cells);
    doc.active_layer = 1; // An explicit selection after the move must come back on redo.
    CHECK(doc.Undo() && !doc.dirty() && doc.active_layer == 1);
    CHECK(doc.Map().layers[0].cells.data() == a_cells &&
          doc.Map().layers[1].cells.data() == b_cells &&
          doc.Map().layers[2].cells.data() == c_cells);
    doc.active_layer = 2;
    CHECK(doc.Redo() && doc.active_layer == 1);
    CHECK(doc.Map().layers[0].cells.data() == b_cells &&
          doc.Map().layers[1].cells.data() == c_cells &&
          doc.Map().layers[2].cells.data() == a_cells);
    CHECK(doc.Undo() && doc.active_layer == 2);
    CHECK(doc.MoveLayer(2, 0) && !doc.CanRedo() && doc.active_layer == 0);
    CHECK(doc.Map().layers[0].cells.data() == c_cells);
}

AETHER_TEST(TilemapEditor_DuplicatesLayerIndependentlyAboveSource) {
    TilemapData map;
    map.Resize(2, 2);
    map.AddLayer("Grass");
    map.AddLayer("Grass copy");
    map.AddLayer("Sky");
    map.layers[0].collides = false;
    map.Set(0, 1, 0, 2);
    map.Set(0, 0, 1, AutotileCell(0));
    TilemapEditDocument doc(std::move(map), Doc().GetTileset());

    CHECK(!doc.DuplicateLayer(3) && !doc.dirty() && !doc.CanUndo());
    doc.BeginStroke();
    CHECK(!doc.DuplicateLayer(0));
    doc.EndStroke();
    if (!doc.DuplicateLayer(0) || doc.Map().layers.size() != 4) { CHECK(false); return; }
    CHECK(doc.active_layer == 1 && doc.Map().layers.size() == 4);
    CHECK(doc.Map().layers[1].name == "Grass copy 2" && doc.Map().layers[2].name == "Grass copy");
    CHECK(doc.Map().layers[1].cells == doc.Map().layers[0].cells && !doc.Map().layers[1].collides);
    CHECK(doc.Undo() && doc.active_layer == 0 && doc.Map().layers.size() == 3 && !doc.CanUndo());
    CHECK(!doc.DuplicateLayer(3) && doc.CanRedo());
    CHECK(doc.Redo() && doc.active_layer == 1 && doc.Map().layers.size() == 4);

    doc.brush = 5;
    doc.Apply(1, 0);
    CHECK(doc.Map().Get(0, 1, 0) == 2 && doc.Map().Get(1, 1, 0) == 5);
    const auto dir = std::filesystem::temp_directory_path() / "tilemap_editor_duplicate";
    if (!doc.Save(dir/"map.atilemap", dir/"set.atileset")) { CHECK(false); return; }
    TilemapData saved;
    if (!LoadTilemap(dir/"map.atilemap", saved)) { CHECK(false); return; }
    CHECK(TilemapToJson(saved) == TilemapToJson(doc.Map()));
}

AETHER_TEST(TilemapEditor_HistoryShortcutsRespectFocusAndTextEditing) {
    HeadlessImGui ui;
    TilemapEditDocument doc = Doc();
    TilemapPanel panel(doc);
    doc.brush = 4;
    doc.Apply(1, 1);
    doc.brush = 5;
    doc.Apply(2, 1);
    char text[32] = "note";
    bool focus_text = false;
    bool has_text_focus = false;
    bool focus_other = false;
    const auto draw = [&] {
        if (focus_text) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("Text field", text, sizeof(text));
        has_text_focus = ImGui::IsItemActive();
        panel.Draw();
        if (focus_other) ImGui::SetNextWindowFocus();
        ImGui::Begin("Other tool");
        ImGui::End();
        focus_text = focus_other = false;
    };
    ImGuiIO& io = ImGui::GetIO();
    const auto press = [&](ImGuiKey key, bool shift = false, bool hold = false) {
        const ImGuiKey physical_mod = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        io.AddKeyEvent(physical_mod, true);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
        io.AddKeyEvent(key, true);
        ui.Frame(draw);
        if (hold) {
            io.DeltaTime = 0.5f;
            ui.Frame(draw);
            ui.Frame(draw);
            io.DeltaTime = 1.0f / 60.0f;
        }
        io.AddKeyEvent(key, false);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
        io.AddKeyEvent(physical_mod, false);
        ui.Frame(draw);
    };
    ui.Frame(draw);
    ui.Frame([&] { ImGui::SetWindowFocus(); draw(); });
    ui.Frame(draw);
    press(ImGuiKey_Z, false, true);
    CHECK(doc.Map().Get(0, 1, 1) == 4 && doc.Map().Get(0, 2, 1) == kEmpty);
    press(ImGuiKey_Z, true);
    CHECK(doc.Map().Get(0, 2, 1) == 5);
    press(ImGuiKey_Z);
    press(ImGuiKey_Y);
    CHECK(doc.Map().Get(0, 2, 1) == 5);

    focus_other = true;
    ui.Frame(draw);
    ui.Frame(draw);
    press(ImGuiKey_Z);
    CHECK(doc.Map().Get(0, 2, 1) == 5);

    focus_text = true;
    ui.Frame([&] { ImGui::SetWindowFocus(); draw(); });
    ui.Frame(draw);
    CHECK(has_text_focus);
    press(ImGuiKey_Z);
    CHECK(doc.Map().Get(0, 2, 1) == 5);

    io.AddKeyEvent(ImGuiKey_Escape, true);
    ui.Frame(draw);
    io.AddKeyEvent(ImGuiKey_Escape, false);
    ui.Frame(draw);
    CHECK(!has_text_focus);
    io.ConfigMacOSXBehaviors = true;
    ui.Frame(draw);
    press(ImGuiKey_Z);
    CHECK(doc.Map().Get(0, 2, 1) == kEmpty);
    press(ImGuiKey_Z, true);
    CHECK(doc.Map().Get(0, 2, 1) == 5);
}

AETHER_TEST(TilemapEditor_SaveShortcutWritesBothAssetsAndKeepsFailedEdits) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("tilemap-save-shortcut-" + assets::ToString(assets::NewAssetGuid()));
    const auto map_file = dir / "Map.atilemap";
    const auto tileset_file = dir / "Ground.atileset";
    HeadlessImGui ui;
    auto doc = Doc();
    TilemapPanel panel(doc);
    bool focus_other = false;
    const auto draw = [&] {
        panel.Draw();
        if (focus_other) ImGui::SetNextWindowFocus();
        ImGui::Begin("Other tool");
        ImGui::End();
        focus_other = false;
    };
    ImGuiIO& io = ImGui::GetIO();
    const auto save_key = [&] {
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_S, true);
        ui.Frame(draw);
        io.AddKeyEvent(ImGuiKey_S, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        ui.Frame(draw);
    };

    doc.brush = 3;
    doc.Apply(1, 1);
    ui.Frame(draw);
    save_key();
    CHECK(doc.dirty() && doc.TilemapFile().empty());
    if (!doc.Save(map_file, tileset_file)) { CHECK(false); return; }
    doc.brush = 4;
    doc.Apply(1, 1);
    doc.SetSolid(4, true);

    focus_other = true;
    ui.Frame(draw);
    ui.Frame(draw);
    save_key();
    CHECK(doc.dirty());

    ui.Frame([&] { ImGui::SetWindowFocus(); draw(); });
    ui.Frame(draw);
    CHECK(fs::remove(tileset_file) && fs::create_directory(tileset_file));
    save_key();
    TilemapData reloaded;
    CHECK(doc.dirty() && LoadTilemap(map_file, reloaded) && reloaded.Get(0, 1, 1) == 3);
    CHECK(fs::remove(tileset_file));
    save_key();
    Tileset tiles;
    CHECK(!doc.dirty() && LoadTilemap(map_file, reloaded) && reloaded.Get(0, 1, 1) == 4);
    CHECK(LoadTileset(tileset_file, tiles) && tiles.IsSolid(4));
    CHECK(doc.CanUndo());
    fs::remove_all(dir);
}

AETHER_TEST(TilemapEditor_CommandShortcutsUseTheRealWindowInputBridge) {
    namespace fs = std::filesystem;
    using input::Key;
    for (const bool native_command : {true, false}) {
        const auto dir = fs::temp_directory_path() / ("tilemap-command-keys-" + assets::ToString(assets::NewAssetGuid()));
        const auto map_file = dir / "Map.atilemap";
        const auto tileset_file = dir / "Ground.atileset";
        HeadlessImGui ui; // matches the shell: no global macOS modifier remapping
        auto doc = Doc();
        TilemapPanel panel(doc, native_command);
        ImGuiWindowInput bridge;
        ImGuiIO& io = ImGui::GetIO();
        const Key left_command = KeyFromGlfwKey(343);
        const Key right_command = KeyFromGlfwKey(347);
        CHECK(!io.ConfigMacOSXBehaviors && left_command == Key::LeftSuper && right_command == Key::RightSuper);
        const auto draw = [&] { panel.Draw(); };
        const auto press = [&](Key modifier, Key key, bool shift = false, bool ctrl = false) {
            const auto send = [&](bool down) {
                const auto event = [&](Key k) {
                    WindowEvent e{};
                    e.type = WindowEventType::Key;
                    e.key = k;
                    e.down = down;
                    return e;
                };
                std::vector<WindowEvent> events{event(modifier)};
                if (shift) events.push_back(event(Key::LeftShift));
                if (ctrl) events.push_back(event(Key::LeftCtrl));
                events.push_back(event(key));
                bridge.Feed(events, io);
                ui.Frame(draw);
            };
            send(true);
            send(false);
        };

        doc.brush = 3;
        doc.Apply(1, 1);
        if (!doc.Save(map_file, tileset_file)) { CHECK(false); return; }
        doc.brush = 4;
        doc.Apply(1, 1);
        ui.Frame(draw);
        ui.Frame([&] { ImGui::SetWindowFocus(); draw(); });
        ui.Frame(draw);

        press(left_command, Key::S);
        TilemapData reloaded;
        CHECK(doc.dirty() == !native_command);
        CHECK(LoadTilemap(map_file, reloaded) && reloaded.Get(0, 1, 1) == (native_command ? 4 : 3));

        press(left_command, Key::Z, false, true);
        CHECK(doc.Map().Get(0, 1, 1) == 4); // extra modifiers must not trigger ordinary shortcuts
        press(left_command, Key::Z);
        CHECK(doc.Map().Get(0, 1, 1) == (native_command ? 3 : 4));
        if (native_command) doc.Redo(); // reset the edit so Ctrl also proves it can undo on macOS
        press(Key::LeftCtrl, Key::Z);
        CHECK(doc.Map().Get(0, 1, 1) == 3);
        press(right_command, Key::Z, true);
        CHECK(doc.Map().Get(0, 1, 1) == (native_command ? 4 : 3));
        if (!native_command) press(Key::LeftCtrl, Key::Y);
        CHECK(doc.Map().Get(0, 1, 1) == 4);
        press(Key::LeftCtrl, Key::Z);
        CHECK(doc.Map().Get(0, 1, 1) == 3);
        press(Key::LeftCtrl, Key::S);
        CHECK(!doc.dirty() && LoadTilemap(map_file, reloaded) && reloaded.Get(0, 1, 1) == 3);
        fs::remove_all(dir);
    }
}

AETHER_TEST(TilemapEditor_StaleConcreteBrushCannotSpreadOutsideTheSheet) {
    const auto make_doc = [](bool textured) {
        TilemapData map;
        map.Resize(3, 2);
        map.AddLayer("Ground");
        map.Set(0, 0, 0, 6); // stale cell, just past a six-tile sheet
        map.Set(0, 1, 0, 5); // last valid tile
        Tileset tiles;
        tiles.texture_width = 48;
        tiles.texture_height = 32;
        if (textured) tiles.texture = assets::NewAssetGuid();
        return TilemapEditDocument(std::move(map), std::move(tiles));
    };
    auto doc = make_doc(true);
    doc.tool = TileTool::Pick;
    doc.Apply(0, 0);
    CHECK(doc.brush == 6);
    const auto revision = doc.Revision();
    doc.tool = TileTool::Paint;
    doc.Apply(2, 0);
    doc.tool = TileTool::Fill;
    doc.Apply(2, 1);
    doc.tool = TileTool::Rectangle;
    doc.Rectangle(0, 1, 2, 1);
    CHECK(doc.Revision() == revision && !doc.CanUndo() && !doc.dirty());
    CHECK(doc.Map().Get(0, 0, 0) == 6 && doc.Map().Get(0, 1, 0) == 5 &&
          doc.Map().Get(0, 2, 0) == kEmpty && doc.Map().Get(0, 2, 1) == kEmpty);
    doc.tool = TileTool::Erase;
    doc.Apply(0, 0);
    CHECK(doc.Map().Get(0, 0, 0) == kEmpty && doc.Undo());
    doc.tool = TileTool::Pick;
    doc.Apply(1, 0);
    doc.tool = TileTool::Rectangle;
    doc.Rectangle(0, 1, 2, 1);
    CHECK(doc.Map().Get(0, 2, 1) == 5);

    // Untextured practice maps intentionally accept unrestricted swatch IDs.
    auto swatches = make_doc(false);
    swatches.brush = 36;
    swatches.Apply(2, 0);
    CHECK(swatches.Map().Get(0, 2, 0) == 36 && swatches.CanUndo());
}

AETHER_TEST(TilemapEditor_MissingAutotileBrushCannotPaintAfterUndo) {
    auto doc = Doc();
    const auto rule = doc.AddAutotile("River");
    doc.brush = AutotileCell(rule);
    CHECK(doc.Undo() && doc.CanRedo() && doc.HasMissingAutotileBrush());
    const auto revision = doc.Revision();
    doc.Apply(0, 0);
    doc.tool = TileTool::Fill;
    doc.Apply(1, 0);
    doc.tool = TileTool::Rectangle;
    doc.Rectangle(2, 0, 5, 4);
    CHECK(doc.Revision() == revision && !doc.dirty() && !doc.CanUndo() && doc.CanRedo());
    for (i32 cell : doc.Map().layers[0].cells) CHECK(cell == kEmpty);
    CHECK(doc.Redo() && !doc.HasMissingAutotileBrush());
    doc.Rectangle(2, 0, 5, 4);
    CHECK(doc.Map().Get(0, 5, 4) == AutotileCell(rule));
}

AETHER_TEST(TilemapEditor_CanEraseButNotCopyMissingAutotileBrush) {
    TilemapData map;
    map.Resize(2, 1);
    map.AddLayer("Ground");
    map.Set(0, 0, 0, AutotileCell(2));
    TilemapEditDocument doc(std::move(map), {});
    doc.tool = TileTool::Pick;
    doc.Apply(0, 0);
    CHECK(doc.brush == AutotileCell(2) && doc.HasMissingAutotileBrush());
    doc.tool = TileTool::Paint;
    doc.Apply(1, 0);
    CHECK(doc.Map().Get(0, 1, 0) == kEmpty && !doc.CanUndo());
    doc.tool = TileTool::Erase;
    doc.Apply(0, 0);
    CHECK(doc.Map().Get(0, 0, 0) == kEmpty && doc.CanUndo());
    CHECK(doc.Undo() && doc.Map().Get(0, 0, 0) == AutotileCell(2));
}

AETHER_TEST(TilemapEditor_LineToolPreviewsAndCommitsOneStraightStroke) {
    HeadlessImGui ui;
    auto doc = Doc();
    TilemapPanel panel(doc);
    doc.brush = 7;
    ImVec2 origin, toolbar;
    ImGuiID line_id = 0;
    f32 canvas_top = 0;
    const auto draw = [&] {
        toolbar = ImGui::GetCursorScreenPos();
        line_id = ImGui::GetID("Line");
        panel.Draw();
        canvas_top = ImGui::GetItemRectMin().y;
        const auto padding = ImGui::GetStyle().WindowPadding;
        origin = {ImGui::GetItemRectMin().x + padding.x + 8, ImGui::GetItemRectMax().y - padding.y - 8};
    };
    auto& io = ImGui::GetIO();
    const auto move = [&](i32 x, i32 y) {
        io.AddMousePosEvent(origin.x + (x + 0.5f) * panel.zoom, origin.y - (y + 0.5f) * panel.zoom);
        ui.Frame(draw);
    };
    const auto left = [&](bool down) { io.AddMouseButtonEvent(ImGuiMouseButton_Left, down); ui.Frame(draw); };
    ui.Frame(draw);
    for (f32 y = toolbar.y + 4; doc.tool != TileTool::Line && y < canvas_top; y += 6) {
        for (f32 x = toolbar.x + 6; x < toolbar.x + 550; x += 8) {
            io.AddMousePosEvent(x, y);
            ui.Frame(draw);
            if (ImGui::GetHoveredID() != line_id) continue;
            left(true);
            left(false);
            break;
        }
    }
    if (doc.tool != TileTool::Line) { CHECK(false); return; }
    move(0, 0);
    left(true);
    move(5, 3);
    CHECK(!doc.dirty() && !doc.CanUndo() && doc.Map().Get(0, 0, 0) == kEmpty);
    io.AddKeyEvent(ImGuiKey_Escape, true);
    ui.Frame(draw);
    io.AddKeyEvent(ImGuiKey_Escape, false);
    left(false);
    CHECK(!doc.dirty() && !doc.CanUndo());

    move(0, 0);
    left(true);
    move(5, 3);
    left(false);
    const std::array<i32, 6> expected_y = {0, 1, 1, 2, 2, 3};
    for (i32 x = 0; x < 6; ++x) {
        for (i32 y = 0; y < 5; ++y)
            CHECK(doc.Map().Get(0, x, y) == (y == expected_y[x] ? 7 : kEmpty));
    }
    CHECK(doc.Undo() && !doc.dirty() && !doc.CanUndo());
    CHECK(doc.Redo() && doc.Map().Get(0, 5, 3) == 7);
    CHECK(doc.Undo());
    doc.brush = AutotileCell(1);
    move(0, 0);
    left(true);
    move(5, 3);
    left(false);
    CHECK(!doc.CanUndo() && doc.CanRedo()); // invalid brush cannot overwrite history
}

AETHER_TEST(TilemapEditor_LineToolKeepsDirectionAtMapEdges) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(6, 6);
    map.AddLayer("Ground");
    TilemapEditDocument doc(std::move(map), {});
    doc.brush = 7;
    doc.tool = TileTool::Line;
    TilemapPanel panel(doc);
    ImVec2 origin;
    const auto draw = [&] {
        panel.Draw();
        const auto pad = ImGui::GetStyle().WindowPadding;
        origin = {ImGui::GetItemRectMin().x + pad.x + 8, ImGui::GetItemRectMax().y - pad.y - 8};
    };
    auto& io = ImGui::GetIO();
    const auto move = [&](i32 x, i32 y) {
        io.AddMousePosEvent(origin.x + (x + 0.5f) * panel.zoom, origin.y - (y + 0.5f) * panel.zoom);
        ui.Frame(draw);
    };
    const auto check = [&](const std::vector<std::pair<i32, i32>>& expected) {
        for (i32 y = 0; y < 6; ++y) {
            for (i32 x = 0; x < 6; ++x) {
                const bool painted = std::find(expected.begin(), expected.end(), std::pair{x, y}) != expected.end();
                CHECK(doc.Map().Get(0, x, y) == (painted ? 7 : kEmpty));
            }
        }
    };
    const auto drag = [&](i32 x, i32 y, const std::vector<std::pair<i32, i32>>& expected) {
        move(1, 1);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        ui.Frame(draw);
        move(x, y);
        CHECK(!doc.dirty() && !doc.CanUndo());
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        ui.Frame(draw);
        check(expected);
        CHECK(doc.Undo() && !doc.dirty() && !doc.CanUndo());
    };
    ui.Frame(draw);
    drag(9, 5, {{1, 1}, {2, 2}, {3, 2}, {4, 3}, {5, 3}});
    drag(5, 9, {{1, 1}, {2, 2}, {2, 3}, {3, 4}, {3, 5}});
    drag(9, 9, {{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}});
    doc.Line(1, 1, std::numeric_limits<i32>::min(), 1);
    check({{1, 1}, {0, 1}}); // a distant endpoint still stops on the first out-of-map step
    CHECK(doc.Undo() && !doc.dirty());
}

AETHER_TEST(TilemapEditor_InterruptedShapesDoNotPaintWhenCanvasReturns) {
    HeadlessImGui ui;
    auto doc = Doc();
    TilemapPanel panel(doc);
    doc.brush = 4;
    ImVec2 origin;
    const auto draw = [&] {
        panel.Draw();
        const auto pad = ImGui::GetStyle().WindowPadding;
        origin = {ImGui::GetItemRectMin().x + pad.x + 8, ImGui::GetItemRectMax().y - pad.y - 8};
    };
    auto& io = ImGui::GetIO();
    const auto move = [&](i32 x, i32 y) {
        io.AddMousePosEvent(origin.x + (x + 0.5f) * panel.zoom, origin.y - (y + 0.5f) * panel.zoom);
        ui.Frame(draw);
    };
    const auto left = [&](bool down) { io.AddMouseButtonEvent(ImGuiMouseButton_Left, down); ui.Frame(draw); };
    ui.Frame(draw);
    for (TileTool tool : {TileTool::Line, TileTool::Rectangle}) {
        doc.tool = tool;
        move(0, 0);
        left(true);
        move(4, 1);
        CHECK(!doc.dirty());
        ui.Frame([] {}); // The workspace no longer submits the canvas.
        left(false);
        CHECK(!doc.dirty() && !doc.CanUndo() && doc.Map().Get(0, 0, 0) == kEmpty);
        if (doc.CanUndo()) doc.Undo();

        move(1, 1);
        left(true);
        move(3, 1);
        left(false);
        CHECK(doc.Map().Get(0, 1, 1) == 4 && doc.Map().Get(0, 3, 1) == 4);
        CHECK(doc.Undo() && !doc.CanUndo() && !doc.dirty());
    }
}

AETHER_TEST(TilemapEditor_RectangleDragCancelsAndClipsWithoutPartialHistory) {
    HeadlessImGui ui;
    TilemapEditDocument doc = Doc();
    TilemapPanel panel(doc);
    doc.tool = TileTool::Rectangle;
    doc.brush = 4;
    ImVec2 bottom_left;
    const auto draw = [&] {
        panel.Draw();
        const ImVec2 corner = {ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y};
        const ImVec2 padding = ImGui::GetStyle().WindowPadding;
        bottom_left = ImVec2(corner.x + padding.x + 8, corner.y - padding.y - 8);
    };
    ui.Frame(draw);
    ImGuiIO& io = ImGui::GetIO();
    const auto move = [&](i32 x, i32 y) {
        io.AddMousePosEvent(bottom_left.x + (static_cast<f32>(x) + 0.5f) * panel.zoom,
                            bottom_left.y - (static_cast<f32>(y) + 0.5f) * panel.zoom);
        ui.Frame(draw);
    };
    const auto left = [&](bool down) {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        ui.Frame(draw);
    };
    move(7, 3); // blank canvas, outside the six-column map
    left(true);
    move(2, 1);
    left(false);
    CHECK(!doc.CanUndo() && doc.Map().Get(0, 2, 1) == kEmpty);

    move(1, 1);
    left(true);
    move(3, 3);
    CHECK(!doc.CanUndo() && doc.Map().Get(0, 1, 1) == kEmpty);
    io.AddKeyEvent(ImGuiKey_Escape, true);
    ui.Frame(draw);
    io.AddKeyEvent(ImGuiKey_Escape, false);
    move(4, 4); // still held: cancellation must not start another rectangle
    left(false);
    CHECK(!doc.CanUndo() && doc.Map().Get(0, 3, 3) == kEmpty);

    move(2, 1);
    left(true);
    move(7, 3);
    left(false);
    for (i32 y = 0; y < 5; ++y) {
        for (i32 x = 0; x < 6; ++x) {
            CHECK(doc.Map().Get(0, x, y) == ((x >= 2 && y >= 1 && y <= 3) ? 4 : kEmpty));
        }
    }
    CHECK(doc.Undo() && !doc.CanUndo());
    CHECK(doc.Map().Get(0, 5, 3) == kEmpty && doc.Redo() && doc.Map().Get(0, 5, 3) == 4);
}

AETHER_TEST(TilemapEditor_RightDragErasesWithRectangleSelected) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(6, 5);
    map.AddLayer("Ground");
    for (i32 x = 0; x < 6; ++x) map.Set(0, x, 1, 3);
    TilemapEditDocument doc(std::move(map), Doc().GetTileset());
    doc.tool = TileTool::Rectangle;
    doc.brush = 7;
    TilemapPanel panel(doc);
    ImVec2 bottom_left;
    const auto draw = [&] {
        panel.Draw();
        const ImVec2 corner{ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y};
        const ImVec2 padding = ImGui::GetStyle().WindowPadding;
        bottom_left = ImVec2(corner.x + padding.x + 8, corner.y - padding.y - 8);
    };
    ui.Frame(draw);
    auto& io = ImGui::GetIO();
    const auto move = [&](i32 x, i32 y) {
        io.AddMousePosEvent(bottom_left.x + (static_cast<f32>(x) + 0.5f) * panel.zoom,
                            bottom_left.y - (static_cast<f32>(y) + 0.5f) * panel.zoom);
        ui.Frame(draw);
    };
    const auto button = [&](ImGuiMouseButton which, bool down) {
        io.AddMouseButtonEvent(which, down);
        ui.Frame(draw);
    };

    move(0, 1);
    button(ImGuiMouseButton_Right, true);
    move(5, 1);
    button(ImGuiMouseButton_Right, false);
    for (i32 x = 0; x < 6; ++x) CHECK(doc.Map().Get(0, x, 1) == kEmpty);
    CHECK(doc.tool == TileTool::Rectangle && doc.brush == 7);
    CHECK(doc.Undo() && !doc.CanUndo());
    for (i32 x = 0; x < 6; ++x) CHECK(doc.Map().Get(0, x, 1) == 3);

    move(1, 1);
    button(ImGuiMouseButton_Left, true);
    button(ImGuiMouseButton_Right, true); // No second gesture during a rectangle.
    move(3, 1);
    CHECK(doc.Map().Get(0, 3, 1) == 3 && !doc.CanUndo());
    button(ImGuiMouseButton_Left, false);
    move(5, 1); // A still-held right button does not start erasing.
    button(ImGuiMouseButton_Right, false);
    CHECK(doc.Map().Get(0, 1, 1) == 7 && doc.Map().Get(0, 3, 1) == 7);
    CHECK(doc.Map().Get(0, 4, 1) == 3 && doc.Map().Get(0, 5, 1) == 3);
    CHECK(doc.Undo() && !doc.CanUndo());
}

AETHER_TEST(TilemapEditor_WorkspaceDrawsTheAuthoredSheetInPaletteAndCanvas) {
    const auto dir = std::filesystem::temp_directory_path() / "aether_tilemap_texture_preview";
    ProjectPaths paths;
    std::string error;
    if (!CreateProject(dir, "Tiles", &paths, &error)) { CHECK(false); return; }
    const auto write_sheet = [&](u32 width, u8 value) {
        const auto png = assets::EncodePngRgba8(width, 8, std::vector<u8>(width * 8 * 4, value));
        return static_cast<bool>(std::ofstream(paths.content / "Tiles.png", std::ios::binary)
            .write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size())));
    };
    if (!write_sheet(18, 255)) { CHECK(false); return; }
    assets::AssetDatabase db(paths.content);
    db.Scan();
    const auto* texture = db.FindByPath("Tiles.png");
    if (!texture) { CHECK(false); return; }
    Tileset tiles;
    tiles.texture = texture->guid;
    tiles.texture_width = 18;
    tiles.texture_height = tiles.tile_width = tiles.tile_height = 8;
    tiles.spacing = 2;
    if (!SaveTileset(paths.content / "Tiles.atileset", tiles, &error)) { CHECK(false); return; }
    // Copy the GUID before scanning, which replaces the database's records.
    db.Scan();
    const auto* set = db.FindByPath("Tiles.atileset");
    if (!set) { CHECK(false); return; }
    TilemapData map;
    map.Resize(2, 1);
    map.AddLayer("Ground");
    map.Set(0, 0, 0, 1);
    map.tileset = set->guid;
    if (!SaveTilemap(paths.content / "Map.atilemap", map, &error)) { CHECK(false); return; }

    HeadlessImGui ui;
    EditorWorkspace ws(paths.file);
    ImVec2 reload;
    const auto draw = [&] {
        ui.Frame([&] {
            const auto top = ImGui::GetCursorScreenPos();
            reload = ImVec2(top.x + 20, top.y + 7);
            ws.DrawTool(ws.Selected());
        });
    };
    int first_red = -1;
    const auto image_quads_for_second_tile = [&] {
        first_red = -1;
        int quads = 0;
        constexpr float low = 10.0f / 18.0f;
        for (const auto* list : ImGui::GetDrawData()->CmdLists) {
            for (const auto& cmd : list->CmdBuffer) {
                const auto* tex = cmd.TexRef._TexData;
                if (!tex || tex->Width != 18 || tex->Height != 8) continue;
                if (tex->Pixels) first_red = tex->Pixels[0];
                for (unsigned int index = 0; index + 5 < cmd.ElemCount; index += 6) {
                    float u0 = 1.0f, u1 = 0.0f;
                    for (unsigned int i = 0; i < 6; ++i) {
                        const auto& v = list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + index + i]];
                        u0 = std::min(u0, v.uv.x);
                        u1 = std::max(u1, v.uv.x);
                    }
                    if (u0 == low && u1 == 1.0f) ++quads;
                }
            }
        }
        return quads;
    };
    if (!ws.OpenAsset("Map.atilemap", &error)) { CHECK(false); return; }
    draw();
    CHECK(image_quads_for_second_tile() == 2); // palette and painted cell; both skip the gutter
    CHECK(!ws.Tilemap().dirty() && ws.Tilemap().Map().Get(0, 0, 0) == 1);

    auto& doc = ws.Tilemap();
    doc.brush = 0;
    doc.Apply(1, 0);
    const auto revision = doc.Revision();
    const auto press_reload = [&] {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(reload.x, reload.y);
        draw();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        draw();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        draw();
        draw();
    };
    if (!write_sheet(18, 135)) { CHECK(false); return; }
    press_reload();
    CHECK(image_quads_for_second_tile() == 2 && first_red == 135);
    if (!write_sheet(16, 90)) { CHECK(false); return; }
    press_reload();
    CHECK(image_quads_for_second_tile() == 0);
    if (!write_sheet(18, 180)) { CHECK(false); return; }
    press_reload();
    CHECK(image_quads_for_second_tile() == 2 && first_red == 180);
    CHECK(doc.dirty() && doc.Revision() == revision && doc.CanUndo() && doc.Map().Get(0, 1, 0) == 0);
    if (!doc.Save(&error)) { CHECK(false); return; }

    // A sheet with mismatched dimensions must not leave old art visible or block editing.
    tiles.texture_width = 16;
    CHECK(SaveTileset(paths.content / "Tiles.atileset", tiles, &error));
    if (!ws.OpenAsset("Map.atilemap", &error)) { CHECK(false); return; }
    draw();
    CHECK(image_quads_for_second_tile() == 0 && !ws.Tilemap().dirty());
}

AETHER_TEST(TilemapEditor_CanvasShortRowsDrawLikeEmptyPadding) {
    const auto draw = [](bool truncated, bool solo) {
        HeadlessImGui ui;
        TilemapData map;
        map.Resize(6, 5);
        map.AddLayer("Ground");
        map.Set(0, 1, 1, 3);
        map.Set(0, 2, 1, 30); // not in the concrete grid
        map.Set(0, 1, 2, AutotileCell(9)); // missing rule in the final partial row
        if (truncated) map.layers[0].cells.resize(14);
        map.AddLayer("Empty layer");
        if (truncated) map.layers[1].cells.clear();
        Tileset tiles;
        tiles.texture = assets::NewAssetGuid();
        tiles.texture_width = tiles.texture_height = 64;
        tiles.SetSolid(3, true);
        TilemapEditDocument doc(std::move(map), std::move(tiles));
        doc.active_layer = solo ? 1 : 0;
        TilemapPanel panel(doc);
        panel.solo_layer_art = solo;
        ui.Frame([&] { panel.Draw(); });
        std::vector<ImU32> colors;
        for (const auto* list : ImGui::GetDrawData()->CmdLists) {
            if (!list->_OwnerName || std::string_view(list->_OwnerName).find("##canvas") == std::string_view::npos) continue;
            for (const auto& vertex : list->VtxBuffer) colors.push_back(vertex.col);
        }
        CHECK(doc.Revision() == 1 && !doc.CanUndo());
        return colors;
    };
    constexpr ImU32 invalid = IM_COL32(255, 96, 215, 255);
    constexpr ImU32 blocked = IM_COL32(220, 40, 40, 255);
    for (bool solo : {false, true}) {
        const auto padded = draw(false, solo);
        const auto short_rows = draw(true, solo);
        CHECK(short_rows == padded);
        CHECK(std::find(short_rows.begin(), short_rows.end(), blocked) != short_rows.end());
        CHECK(std::count(short_rows.begin(), short_rows.end(), invalid) == 16);
    }
}

AETHER_TEST(TilemapEditor_CollisionOutlineSitsAboveDecorativeLayers) {
    HeadlessImGui ui;
    auto doc = Doc();
    doc.brush = 1;
    doc.Apply(1, 1);
    doc.SetSolid(1, true);
    doc.AddLayer("Decoration");
    doc.SetLayerCollides(1, false);
    doc.active_layer = 1;
    doc.brush = 2;
    doc.Apply(1, 1);
    doc.SetSolid(2, true); // solid in the palette, but this layer does not block
    TilemapPanel panel(doc);
    panel.show_grid = false;

    // Read actual canvas submission order. A swatch above the outline hides
    // its inner edge; the outline must be drawn after both cell images.
    constexpr ImU32 background = IM_COL32(30, 30, 36, 255);
    constexpr ImU32 marker = IM_COL32(220, 40, 40, 255);
    const auto draw = [&] {
        ui.Frame([&] { panel.Draw(); });
        std::vector<ImU32> colors;
        for (const auto* list : ImGui::GetDrawData()->CmdLists) {
            if (!list->_OwnerName || std::string_view(list->_OwnerName).find("##canvas") == std::string_view::npos)
                continue;
            bool inside = false;
            for (const auto& v : list->VtxBuffer) {
                if (v.col == background) inside = true;
                if (inside) colors.push_back(v.col);
            }
        }
        return colors;
    };
    panel.show_solid = false;
    const auto plain = draw();
    panel.show_solid = true;
    const u64 revision = doc.Revision();
    const auto tagged = draw();
    const auto outline = std::find(tagged.begin(), tagged.end(), marker);
    CHECK(plain.size() >= 12 && outline != tagged.end() &&
          static_cast<usize>(outline - tagged.begin()) >= plain.size());
    CHECK(doc.Revision() == revision && IsSolidAt(doc.Map(), doc.GetTileset(), 1, 1));
    const auto markers = std::count(tagged.begin(), tagged.end(), marker);
    CHECK(doc.SetLayerCollides(1, true));
    const auto both_block = draw();
    CHECK(std::count(both_block.begin(), both_block.end(), marker) == markers);
    CHECK(doc.SetLayerCollides(1, false) && doc.SetLayerCollides(0, false));
    const auto nonblocking = draw();
    CHECK(std::find(nonblocking.begin(), nonblocking.end(), marker) == nonblocking.end());
    CHECK(!IsSolidAt(doc.Map(), doc.GetTileset(), 1, 1));
}

AETHER_TEST(TilemapEditor_SoloLayerShowsOnlySelectedArtButKeepsDiagnostics) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(6, 5);
    map.AddLayer("Ground");
    map.AddLayer("Decoration");
    map.Set(0, 1, 1, 1);
    map.Set(0, 2, 2, AutotileCell(0)); // missing rule even with a swatch-only tileset
    map.Set(1, 1, 1, 2);
    map.layers[1].collides = false;
    Tileset ts;
    ts.texture_width = ts.texture_height = 32;
    ts.tile_width = ts.tile_height = 16;
    ts.SetSolid(1, true);
    TilemapEditDocument doc(std::move(map), std::move(ts));
    TilemapPanel panel(doc);
    panel.show_grid = false;
    constexpr ImU32 marker = IM_COL32(220, 40, 40, 255), invalid = IM_COL32(255, 96, 215, 255);
    const auto draw = [&] {
        ui.Frame([&] { panel.Draw(); });
        for (const auto* list : ImGui::GetDrawData()->CmdLists) {
            if (!list->_OwnerName || std::string_view(list->_OwnerName).find("##canvas") == std::string_view::npos)
                continue;
            std::vector<ImU32> colors;
            for (const auto& v : list->VtxBuffer) colors.push_back(v.col);
            return colors;
        }
        return std::vector<ImU32>{};
    };
    const auto all = draw();
    panel.solo_layer_art = true;
    const auto ground = draw();
    doc.active_layer = 1;
    const auto decor = draw();
    CHECK(!ground.empty() && !decor.empty() && ground != decor);
    CHECK(all.size() == ground.size() + 4 && all.size() == decor.size() + 4);
    for (const auto color : {marker, invalid}) {
        const auto count = [&](const auto& colors) { return std::count(colors.begin(), colors.end(), color); };
        CHECK(count(all) > 0 && count(decor) == count(all) && count(ground) == count(all));
    }
    panel.solo_layer_art = false;
    CHECK(draw() == all);
    CHECK(!doc.dirty() && !doc.CanUndo() && doc.Map().Get(0, 1, 1) == 1 && doc.Map().Get(1, 1, 1) == 2);
}

AETHER_TEST(TilemapEditor_HoverNamesTheLayerThatBlocksMovement) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(6, 5);
    map.AddLayer("Basalt");
    map.AddLayer("Roots");
    map.AddLayer("Canopy");
    map.layers[2].collides = false;
    Tileset tiles = Doc().GetTileset();
    tiles.SetSolid(2, true);
    std::array<i32, 16> variants;
    variants.fill(2);
    const auto rule = tiles.AddAutotile("Stone edge", variants);
    map.Set(0, 1, 1, AutotileCell(rule));
    map.Set(1, 1, 1, 2);
    map.Set(2, 1, 1, 2);
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    doc.active_layer = 2;
    TilemapPanel panel(doc);
    ImVec2 bottom_left;
    const auto draw = [&] {
        panel.Draw();
        const auto pad = ImGui::GetStyle().WindowPadding;
        bottom_left = ImVec2(ImGui::GetItemRectMin().x + pad.x + 8, ImGui::GetItemRectMax().y - pad.y - 8);
    };
    ui.Frame(draw);
    const auto log_hover = [&](i32 x, i32 y, const char* name) {
        const auto log = std::filesystem::temp_directory_path() / name;
        ImGui::GetIO().AddMousePosEvent(bottom_left.x + (static_cast<f32>(x) + .5f) * panel.zoom,
                                       bottom_left.y - (static_cast<f32>(y) + .5f) * panel.zoom);
        ui.Frame(draw); // let ImGui update its hovered window
        ui.Frame([&] {
            ImGui::LogToFile(0, log.string().c_str());
            draw();
            ImGui::LogFinish();
        });
        std::ifstream input(log);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const auto revision = doc.Revision();
    const auto occupied = log_hover(1, 1, "tilemap-hover-occupied.txt");
    CHECK(occupied.find("Editing: Canopy") != std::string::npos);
    CHECK(occupied.find("Blocks movement: Roots") != std::string::npos);
    CHECK(occupied.find("1 more blocking layer") != std::string::npos);
    CHECK(occupied.find("Blocks movement: Canopy") == std::string::npos);
    const auto clear = log_hover(2, 1, "tilemap-hover-clear.txt");
    CHECK(clear.find("No blocking tiles") != std::string::npos);
    CHECK(doc.Revision() == revision && !doc.dirty() && !doc.CanUndo());
}

AETHER_TEST(TilemapEditor_MarksInvalidConcretePaintThroughUpperLayers) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(6, 5);
    map.AddLayer("Ground");
    map.AddLayer("Canopy");
    Tileset tiles;
    tiles.texture = assets::NewAssetGuid();
    tiles.texture_width = tiles.texture_height = 32;
    tiles.tile_width = tiles.tile_height = 16;
    std::array<i32, 16> variants;
    variants.fill(3);
    const auto rule = tiles.AddAutotile("Edge", variants);
    map.Set(0, 1, 1, 4);
    map.Set(1, 1, 1, 3);
    map.Set(0, 2, 1, AutotileCell(rule));
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    doc.active_layer = 1;
    TilemapPanel panel(doc);
    ImVec2 bottom_left;
    const auto draw = [&] {
        panel.Draw();
        const auto pad = ImGui::GetStyle().WindowPadding;
        bottom_left = ImVec2(ImGui::GetItemRectMin().x + pad.x + 8, ImGui::GetItemRectMax().y - pad.y - 8);
    };
    const auto revision = doc.Revision();
    ui.Frame(draw);
    usize markers = 0;
    for (const auto* list : ImGui::GetDrawData()->CmdLists)
        for (const ImDrawVert& vertex : list->VtxBuffer)
            if (vertex.col == IM_COL32(255, 96, 215, 255)) ++markers;
    CHECK(markers == 8); // One opaque X: two lines, four colored vertices each.
    const auto capture = std::filesystem::temp_directory_path() / "tilemap-hover-invalid.txt";
    ImGui::GetIO().AddMousePosEvent(bottom_left.x + 1.5f * panel.zoom, bottom_left.y - 1.5f * panel.zoom);
    ui.Frame(draw);
    ui.Frame([&] { ImGui::LogToFile(0, capture.string().c_str()); draw(); ImGui::LogFinish(); });
    std::ifstream in(capture);
    const std::string log(std::istreambuf_iterator<char>(in), {});
    CHECK(log.find("Invalid paint: Ground uses tile 4 (grid has 4 tiles)") != std::string::npos);
    CHECK(doc.Revision() == revision && !doc.dirty() && !doc.CanUndo());
}

AETHER_TEST(TilemapEditor_FindsInvalidPaintAcrossTheMapWithoutAnEdit) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(100, 90);
    map.AddLayer("Ground"); map.AddLayer("Canopy");
    Tileset tiles;
    tiles.texture = assets::NewAssetGuid();
    tiles.texture_width = tiles.texture_height = 32;
    tiles.tile_width = tiles.tile_height = 16;
    map.Set(0, 93, 85, 6);
    map.Set(1, 3, 2, 4);
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    TilemapPanel panel(doc);
    const auto revision = doc.Revision();
    const auto marker_visible = [&] {
        ui.Frame([&] { panel.Draw(); });
        for (const auto* list : ImGui::GetDrawData()->CmdLists)
            for (const ImDrawVert& v : list->VtxBuffer)
                if (v.col == IM_COL32(255, 96, 215, 255)) return true;
        return false;
    };
    CHECK(panel.FindNextInvalidPaint() && doc.active_layer == 0 && marker_visible());
    CHECK(panel.FindNextInvalidPaint() && doc.active_layer == 1 && marker_visible());
    CHECK(panel.FindNextInvalidPaint() && doc.active_layer == 0);
    CHECK(doc.Revision() == revision && !doc.dirty() && !doc.CanUndo());
    doc.brush = 0;
    doc.Apply(93, 85);
    CHECK(panel.FindNextInvalidPaint() && doc.active_layer == 1);
    doc.Apply(3, 2);
    CHECK(!panel.FindNextInvalidPaint());
}

AETHER_TEST(TilemapEditor_FocusAndPaintTargetsExactLongAxisCell) {
    constexpr i32 axis_limit = 16'777'216;
    for (const auto& [width, height] : {std::pair{axis_limit, 1}, std::pair{1, axis_limit}, std::pair{31, 17}}) {
        HeadlessImGui ui;
        TilemapData map;
        map.Resize(width, height);
        map.AddLayer("Ground");
        TilemapEditDocument doc(std::move(map), {});
        doc.brush = 3;
        TilemapPanel panel(doc);
        ImVec2 center;
        bool canvas_found = false;
        const auto draw = [&] {
            const auto* parent = ImGui::GetCurrentWindowRead();
            const auto id = ImGui::GetID("##canvas");
            panel.Draw();
            for (const auto* child : parent->DC.ChildWindows) {
                if (child->ChildId != id) continue;
                const auto start = child->DC.CursorStartPos;
                center = {(start.x + child->WorkRect.Max.x) * 0.5f, (start.y + child->WorkRect.Max.y) * 0.5f};
                canvas_found = true;
            }
        };
        auto& io = ImGui::GetIO();
        for (i32 delta : {18, 19, 20, 21}) {
            const i32 x = width == axis_limit ? width - delta : width / 2;
            const i32 y = height == axis_limit ? height - delta : height / 2;
            CHECK(panel.FocusCell(0, x, y));
            ui.Frame(draw);
            if (!canvas_found) { CHECK(false); return; }
            io.AddMousePosEvent(center.x, center.y);
            ui.Frame(draw);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            ui.Frame(draw);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            ui.Frame(draw);
            CHECK(doc.Map().Get(0, x, y) == 3);
            CHECK(doc.Map().Get(0, x + 1, y) == kEmpty && doc.Map().Get(0, x - 1, y) == kEmpty);
            CHECK(doc.Map().Get(0, x, y + 1) == kEmpty && doc.Map().Get(0, x, y - 1) == kEmpty);
            CHECK(doc.Undo() && !doc.dirty());
        }
    }
}

AETHER_TEST(TilemapEditor_GoToCellCentersDistantCoordinatesWithoutEditing) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(128, 96);
    map.AddLayer("Ground");
    map.AddLayer("Far");
    map.Set(1, 90, 70, AutotileCell(9));
    TilemapEditDocument doc(std::move(map), {});
    doc.active_layer = 1;
    TilemapPanel panel(doc);
    ImGuiWindow* window = nullptr;
    const auto draw = [&] { window = ImGui::GetCurrentWindowRead(); panel.Draw(); };
    ui.Frame(draw);
    auto& io = ImGui::GetIO();
    const auto click_item = [&](ImGuiWindow& target, const char* label, f32 height) {
        const ImGuiID id = target.GetID(label);
        const auto clip = target.InnerClipRect;
        for (f32 y = clip.Min.y + 4; y < std::min(clip.Min.y + height, clip.Max.y); y += 8) {
            for (f32 x = clip.Min.x + 6; x < clip.Max.x; x += 14) {
                io.AddMousePosEvent(x, y);
                ui.Frame(draw);
                if (ImGui::GetHoveredID() != id) continue;
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                ui.Frame(draw);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                ui.Frame(draw);
                return true;
            }
        }
        return false;
    };
    const auto visible = [] {
        for (const auto* list : ImGui::GetDrawData()->CmdLists)
            for (const auto& vertex : list->VtxBuffer)
                if (vertex.col == IM_COL32(255, 96, 215, 255)) return true;
        return false;
    };
    const auto enter = [&](ImGuiWindow& modal, const char* label, const char* value) {
        if (!click_item(modal, label, 200)) { CHECK(false); return; }
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_A, true);
        ui.Frame(draw);
        io.AddKeyEvent(ImGuiKey_A, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        io.AddInputCharactersUTF8(value);
        ui.Frame(draw);
    };
    const auto open = [&]() -> ImGuiWindow* {
        if (!click_item(*window, "Go to cell...", 110)) return nullptr;
        ui.Frame(draw);
        return ImGui::FindWindowByName("Go to tile cell");
    };
    CHECK(!visible());
    const auto revision = doc.Revision();
    auto* modal = open();
    if (!modal) { CHECK(false); return; }
    enter(*modal, "X", "128");
    click_item(*modal, "Go", 200); // x == width is out of bounds
    ui.Frame([&] { draw(); CHECK(ImGui::IsPopupOpen("Go to tile cell")); });
    CHECK(click_item(*modal, "Cancel", 200) && !visible());

    modal = open();
    if (!modal) { CHECK(false); return; }
    enter(*modal, "X", "90");
    enter(*modal, "Y", "70");
    CHECK(click_item(*modal, "Go", 200) && visible());
    CHECK(doc.active_layer == 1 && doc.Revision() == revision && !doc.dirty() && !doc.CanUndo());
}

AETHER_TEST(TilemapEditor_FindInvalidPaintUsesBothEncodingBoundaries) {
    for (bool textured : {false, true}) {
        HeadlessImGui ui;
        Tileset tiles;
        tiles.texture_width = tiles.texture_height = 32;
        if (textured) tiles.texture = assets::NewAssetGuid(); // tiles 0 through 3
        std::array<i32, 16> variants{};
        tiles.AddAutotile("Grass", variants);
        tiles.AddAutotile("River", variants);

        TilemapData map;
        map.Resize(5, 1);
        map.AddLayer("Valid edges");
        map.Set(0, 0, 0, 0);
        map.Set(0, 1, 0, textured ? 3 : std::numeric_limits<i32>::max());
        map.Set(0, 2, 0, AutotileCell(1)); // last existing rule
        map.Set(0, 3, 0, AutotileCell(0));
        map.AddLayer("Missing rule");
        map.Set(1, 0, 0, std::numeric_limits<i32>::min());
        map.AddLayer("Outside grid");
        map.Set(2, 0, 0, 4);
        TilemapEditDocument doc(std::move(map), std::move(tiles));
        TilemapPanel panel(doc);
        CHECK(panel.FindNextInvalidPaint() && doc.active_layer == 1);
        doc.tool = TileTool::Erase;
        doc.Apply(0, 0);
        if (textured) {
            CHECK(panel.FindNextInvalidPaint() && doc.active_layer == 2);
            doc.Apply(0, 0);
        }
        CHECK(!panel.FindNextInvalidPaint());
    }
}

AETHER_TEST(TilemapEditor_FindInvalidPaintCyclesWithinOneLayer) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(6, 5);
    map.AddLayer("Ground");
    map.Set(0, 1, 1, 4);
    map.Set(0, 4, 3, 5);
    Tileset tiles;
    tiles.texture = assets::NewAssetGuid();
    tiles.texture_width = tiles.texture_height = 32;
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    TilemapPanel panel(doc);
    const auto find = [&](int step, std::string_view hit) {
        CHECK(panel.FindNextInvalidPaint());
        const auto capture = std::filesystem::temp_directory_path() /
                             ("tilemap-find-invalid-cycle-" + std::to_string(step) + ".txt");
        ui.Frame([&] {
            ImGui::LogToFile(0, capture.string().c_str());
            panel.Draw();
            ImGui::LogFinish();
        });
        std::ifstream in(capture);
        const std::string text(std::istreambuf_iterator<char>{in}, {});
        CHECK(text.find(hit) != std::string::npos);
    };
    const auto revision = doc.Revision();
    find(1, "Ground (1, 1), tile 4");
    find(2, "Ground (4, 3), tile 5");
    find(3, "Ground (1, 1), tile 4");
    CHECK(doc.Revision() == revision && !doc.dirty() && !doc.CanUndo());

    doc.brush = 0;
    doc.Apply(1, 1);
    find(4, "Ground (4, 3), tile 5");
    find(5, "Ground (4, 3), tile 5"); // wrap with only one hit left
}

AETHER_TEST(TilemapEditor_FindsMissingAutotileRulesEvenWithoutTexture) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(95, 88);
    map.AddLayer("Ground");
    map.AddLayer("Canopy");
    Tileset tiles;
    std::array<i32, 16> variants;
    variants.fill(kEmpty);
    CHECK(tiles.AddAutotile("Intentional empty", variants) == 0);
    map.Set(0, 2, 2, AutotileCell(0));
    map.Set(1, 85, 78, AutotileCell(1));
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    TilemapPanel panel(doc);
    const auto revision = doc.Revision();
    const auto capture = std::filesystem::temp_directory_path() / "tilemap-missing-rule-search.txt";
    CHECK(panel.FindNextInvalidPaint() && doc.active_layer == 1);
    ui.Frame([&] { ImGui::LogToFile(0, capture.string().c_str()); panel.Draw(); ImGui::LogFinish(); });
    usize markers = 0;
    for (const auto* list : ImGui::GetDrawData()->CmdLists)
        for (const ImDrawVert& vertex : list->VtxBuffer)
            if (vertex.col == IM_COL32(255, 96, 215, 255)) ++markers;
    CHECK(markers == 8);
    std::ifstream in(capture);
    const std::string log(std::istreambuf_iterator<char>(in), {});
    CHECK(log.find("Invalid paint: Canopy (85, 78), missing autotile rule 1") != std::string::npos);
    CHECK(doc.Revision() == revision && !doc.dirty() && !doc.CanUndo());
    doc.brush = kEmpty;
    doc.Apply(85, 78);
    CHECK(!panel.FindNextInvalidPaint());
}

AETHER_TEST(TilemapEditor_FindsInvalidVariantsInUnusedAutotileRules) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(6, 5);
    map.AddLayer("Ground");
    map.Set(0, 0, 0, AutotileCell(0));
    Tileset tiles;
    tiles.texture = assets::NewAssetGuid();
    tiles.texture_width = 48;
    tiles.texture_height = 32;
    tiles.AddAutotile("Intentional empty", Autotile().tiles);
    auto variants = Autotile().tiles;
    variants[3] = 6;
    const auto river = tiles.AddAutotile("River", variants); // no painted cell references it
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    TilemapPanel panel(doc);
    doc.brush = 5;
    const u64 revision = doc.Revision();

    const auto capture = [&](const char* name) {
        const auto file = std::filesystem::temp_directory_path() / name;
        ui.Frame([&] { ImGui::LogToFile(0, file.string().c_str()); panel.Draw(); ImGui::LogFinish(); });
        std::ifstream in(file);
        return std::string(std::istreambuf_iterator<char>{in}, {});
    };
    CHECK(!panel.FindNextInvalidPaint()); // no canvas cell to focus
    const auto found = capture("tilemap-invalid-variant-found.txt");
    CHECK(found.find("Invalid autotile: River, mask 3 uses tile 6 (grid has 6 tiles)") != std::string::npos);
    CHECK(found.find("Editing masks: River") != std::string::npos);
    CHECK(found.find("No invalid painted tiles") == std::string::npos);
    CHECK(doc.Revision() == revision && doc.brush == 5 && !doc.CanUndo());

    CHECK(doc.SetAutotileTile(river, 3, 5));
    const auto stale = capture("tilemap-invalid-variant-stale.txt");
    CHECK(stale.find("Invalid autotile:") == std::string::npos);
    CHECK(!panel.FindNextInvalidPaint());
    const auto clean = capture("tilemap-invalid-variant-clean.txt");
    CHECK(clean.find("Invalid autotile:") == std::string::npos);
    CHECK(clean.find("No invalid painted tiles") != std::string::npos);
}

AETHER_TEST(TilemapEditor_CollisionDiagnosticsIgnoreNumberedTilesets) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(2, 2);
    Tileset tiles;
    tiles.texture_width = tiles.texture_height = 32;
    tiles.SetSolid(4, true);
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    TilemapPanel panel(doc);
    CHECK(!panel.FindNextInvalidPaint());
    const auto file = std::filesystem::temp_directory_path() / "tilemap-numbered-flag-diagnostics.txt";
    ui.Frame([&] {
        ImGui::LogToFile(0, file.string().c_str());
        panel.Draw();
        ImGui::LogFinish();
    });
    std::ifstream in(file);
    const std::string log(std::istreambuf_iterator<char>{in}, {});
    CHECK(log.find("No invalid painted tiles") != std::string::npos);
    CHECK(log.find("Clear invalid collision flag") == std::string::npos);
    CHECK(doc.GetTileset().IsSolid(4) && !doc.dirty());
}

AETHER_TEST(TilemapEditor_CanJumpToInvalidMaskWhileInvalidPaintRemains) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(4, 4);
    map.AddLayer("Ground");
    map.Set(0, 2, 2, 70); // A separate invalid painted tile must remain untouched.
    Tileset tiles;
    tiles.texture = assets::NewAssetGuid();
    tiles.texture_width = tiles.texture_height = 64;
    for (usize i = 0; i < 512; ++i) {
        auto masks = Autotile{}.tiles;
        if (i == 510) masks[15] = 99;
        tiles.AddAutotile("Canal bank " + std::to_string(i), masks);
    }
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    TilemapPanel panel(doc);
    doc.brush = 3;
    ImGuiWindow* palette = nullptr;
    ImGuiID jump_id = 0;
    const auto draw = [&] {
        ui.Frame([&] {
            jump_id = ImGui::GetID("Go to invalid autotile mask");
            const ImGuiID palette_id = ImGui::GetID("##left");
            const auto* parent = ImGui::GetCurrentWindowRead();
            panel.Draw();
            for (ImGuiWindow* child : parent->DC.ChildWindows)
                if (child->ChildId == palette_id) palette = child;
        });
    };
    CHECK(panel.FindNextInvalidPaint()); // The normal search still goes to the canvas.
    draw();
    if (!palette) { CHECK(false); return; }
    CHECK(palette->Scroll.y == 0.0f);
    auto& io = ImGui::GetIO();
    bool clicked = false;
    for (f32 y = 45; !clicked && y < 240; y += 4) {
        for (f32 x = 14; x < 400; x += 20) {
            io.AddMousePosEvent(x, y);
            draw();
            if (ImGui::GetHoveredID() != jump_id) continue;
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            draw();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            draw();
            clicked = true;
            break;
        }
    }
    draw();
    CHECK(clicked && palette->Scroll.y > 7000.0f);
    CHECK(doc.Map().Get(0, 2, 2) == 70 && doc.GetTileset().autotiles[510].tiles[15] == 99);
    CHECK(doc.brush == 3 && !doc.dirty() && !doc.CanUndo());
}

AETHER_TEST(TilemapEditor_FindingAnInvalidMaskScrollsItIntoView) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(4, 4);
    Tileset tiles;
    tiles.texture = assets::NewAssetGuid();
    tiles.texture_width = 64;
    tiles.texture_height = 64;
    for (usize i = 0; i < 512; ++i) {
        auto masks = Autotile{}.tiles;
        if (i == 510) masks[15] = 99; // out of the 16-tile sheet
        tiles.AddAutotile("Canal bank " + std::to_string(i), masks);
    }
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    TilemapPanel panel(doc);
    doc.brush = 3;
    ImGuiWindow* palette = nullptr;
    const auto draw = [&] {
        ui.Frame([&] {
            const ImGuiID palette_id = ImGui::GetID("##left");
            const auto* parent = ImGui::GetCurrentWindowRead();
            panel.Draw();
            for (ImGuiWindow* child : parent->DC.ChildWindows)
                if (child->ChildId == palette_id) palette = child;
        });
    };
    draw();
    if (!palette) { CHECK(false); return; }
    CHECK(palette->Scroll.y == 0.0f);
    const u64 revision = doc.Revision();
    CHECK(!panel.FindNextInvalidPaint()); // only the tileset needs a repair
    draw();
    draw(); // Apply the palette's requested scroll target.
    CHECK(palette->Scroll.y > 7000.0f);

    auto& io = ImGui::GetIO();
    const ImGuiID mask_id = ImHashStr("NESW 99", 0, palette->GetID(2015));
    const ImRect clip = palette->InnerClipRect;
    bool mask_visible = false;
    for (f32 y = clip.Min.y + 2.0f; !mask_visible && y < clip.Max.y; y += 4.0f) {
        for (f32 x = clip.Min.x + 8.0f; x < clip.Max.x; x += 12.0f) {
            io.AddMousePosEvent(x, y);
            draw();
            if (ImGui::GetHoveredID() == mask_id) { mask_visible = true; break; }
        }
    }
    CHECK(mask_visible && doc.Revision() == revision && doc.brush == 3 && !doc.CanUndo());
    ImGui::SetScrollY(palette, 0.0f);
    draw();
    draw();
    CHECK(palette->Scroll.y == 0.0f); // Don't fight later manual scrolling.
}

AETHER_TEST(TilemapEditor_LargeMapOnlyDrawsVisibleGridLines) {
    HeadlessImGui ui;
    auto doc = Doc();
    if (!doc.Resize(512, 256)) { CHECK(false); return; }
    doc.brush = 2;
    doc.Apply(0, 0);
    doc.brush = 5;
    doc.Apply(511, 255);
    const u64 revision = doc.Revision();
    TilemapPanel panel(doc);
    ui.Frame([&] { panel.Draw(); });
    usize grid_vertices = 0;
    for (const auto* list : ImGui::GetDrawData()->CmdLists) {
        for (const ImDrawVert& vertex : list->VtxBuffer)
            if (vertex.col == IM_COL32(255, 255, 255, 30)) ++grid_vertices;
    }
    CHECK(grid_vertices > 0 && grid_vertices < 600);
    CHECK(doc.Revision() == revision && doc.Map().Get(0, 0, 0) == 2 && doc.Map().Get(0, 511, 255) == 5);
}

AETHER_TEST(TilemapEditor_LargePaletteSubmitsOnlyVisibleRows) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(4, 4);
    map.AddLayer("Ground");
    Tileset tiles;
    tiles.texture_width = tiles.texture_height = 512;
    tiles.tile_width = tiles.tile_height = 8;
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    TilemapPanel panel(doc);
    const auto draw = [&] { ui.Frame([&] { panel.Draw(); }); };
    const auto swatch_vertices = [] {
        usize count = 0;
        for (const auto* list : ImGui::GetDrawData()->CmdLists)
            for (const auto& v : list->VtxBuffer)
                if (v.col == IM_COL32(30, 30, 36, 255)) ++count;
        return count;
    };
    draw();
    CHECK(swatch_vertices() > 40 && swatch_vertices() < 1300);
    ui.Frame([&] {
        ImGui::SetNextWindowScroll(ImVec2(-1.0f, 4000.0f));
        panel.Draw();
    });
    draw();
    CHECK(swatch_vertices() > 40 && swatch_vertices() < 1300);
    CHECK(doc.GetTileset().TileCount() == 4096 && !doc.dirty() && !doc.CanUndo());
}

AETHER_TEST(TilemapEditor_CanSelectAnAutotileAtTheEndOfALargeScrolledList) {
    HeadlessImGui ui;
    TilemapData map;
    map.Resize(4, 4);
    Tileset tiles;
    for (usize i = 0; i < 512; ++i)
        tiles.AddAutotile("Canal ## bank " + std::to_string(i), Autotile{}.tiles);
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    TilemapPanel panel(doc);
    ImGuiWindow* palette = nullptr;
    const auto draw = [&] {
        ui.Frame([&] {
            const ImGuiID palette_id = ImGui::GetID("##left");
            const ImGuiWindow* parent = ImGui::GetCurrentWindowRead();
            panel.Draw();
            for (ImGuiWindow* child : parent->DC.ChildWindows)
                if (child->ChildId == palette_id) palette = child;
        });
    };
    auto& io = ImGui::GetIO();
    const auto click_rule = [&](usize rule) {
        const ImGuiID id = ImHashStr("##autotile", 0, palette->GetID(static_cast<int>(1000 + rule)));
        const ImRect clip = palette->InnerClipRect;
        for (f32 y = clip.Min.y + 2.0f; y < clip.Max.y; y += 4.0f) {
            io.AddMousePosEvent(clip.Min.x + 25.0f, y);
            draw();
            if (ImGui::GetHoveredID() != id) continue;
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            draw();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            draw();
            return true;
        }
        return false;
    };
    draw();
    if (!palette) { CHECK(false); return; }
    draw(); // Let the child compute its contents and maximum scroll.
    ImGui::SetScrollY(palette, palette->ScrollMax.y);
    draw();
    const auto revision = doc.Revision();
    CHECK(palette->Scroll.y > 7000.0f && click_rule(511));
    CHECK(doc.brush == AutotileCell(511) && doc.Revision() == revision && !doc.CanUndo());

    // Adding a multiline name invalidates the uniform row layout.
    CHECK(doc.RenameAutotile(508, "Upper bank\nLower bank"));
    draw();
    ImGui::SetScrollY(palette, palette->ScrollMax.y);
    draw();
    CHECK(click_rule(508) && doc.brush == AutotileCell(508));
    CHECK(doc.Undo());
    draw();
    CHECK(click_rule(510) && doc.brush == AutotileCell(510));
}

AETHER_TEST(TilemapEditor_ResizeDialogOpensWithoutChangingTheMap) {
    HeadlessImGui ui;
    auto doc = Doc();
    TilemapPanel panel(doc);
    ImVec2 resize_pos;
    bool open = false;
    const auto draw = [&] {
        panel.Draw();
        open = ImGui::IsPopupOpen("Resize tilemap");
    };
    ui.Frame([&] {
        const auto top = ImGui::GetCursorScreenPos();
        resize_pos = ImVec2(top.x + 18.0f, top.y + 8.0f);
        draw();
    });
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(resize_pos.x, resize_pos.y);
    ui.Frame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    ui.Frame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    ui.Frame(draw);
    CHECK(open);
    io.AddKeyEvent(ImGuiKey_Escape, true);
    ui.Frame(draw);
    CHECK(!open);
    CHECK(doc.Map().width == 6 && doc.Map().height == 5 && !doc.dirty() && !doc.CanUndo());
}

AETHER_TEST(TilemapEditor_ExistingOversizedMapsCanBeCroppedWithoutGrowingAnAxis) {
    TilemapData map;
    constexpr u32 width = 4096;
    constexpr u32 height = 1026; // just above the editor's usual 4 Mi-cell budget
    map.Resize(width, height);
    map.AddLayer("Imported ground");
    map.Set(0, 6, 0, 2);
    map.Set(0, 6, height - 2, 3);
    map.Set(0, 6, height - 1, 4);
    TilemapEditDocument doc(std::move(map), Doc().GetTileset());

    CHECK(!doc.CanResize(width, height + 1));
    CHECK(!doc.CanResize(width + 1, height - 1)); // lower area alone does not permit a wider map
    CHECK(!doc.Resize(width, height + 1) && !doc.CanUndo() && !doc.dirty());
    CHECK(doc.CanResize(width - 1, height)); // a width-only crop is allowed too
    if (!doc.Resize(width, height - 1)) { CHECK(false); return; }
    CHECK(doc.Map().width == width && doc.Map().height == height - 1 && doc.dirty());
    CHECK(doc.Map().Get(0, 6, 0) == 2 && doc.Map().Get(0, 6, height - 2) == 3 &&
          doc.Map().Get(0, 6, height - 1) == kEmpty);
    CHECK(!doc.CanResize(width, height)); // cannot regrow past the budget through Resize
    CHECK(doc.Undo() && !doc.dirty() && doc.Map().height == height && doc.Map().Get(0, 6, height - 1) == 4);
    CHECK(doc.Redo() && doc.dirty() && doc.Map().height == height - 1);
}

AETHER_TEST(TilemapEditor_GeneralResizeReusesOriginalStorageAndKeepsTilesetHistory) {
    TilemapData map;
    map.Resize(5, 4);
    map.AddLayer("Ground");
    map.Set(0, 3, 2, 8);
    map.Set(0, 4, 3, 9);
    map.AddLayer("Short decorative layer");
    map.layers[1].collides = false;
    map.layers[1].cells.resize(7);
    map.Set(1, 1, 1, 4);
    Tileset tiles;
    tiles.SetSolid(3, true);
    TilemapEditDocument doc(std::move(map), std::move(tiles));
    doc.active_layer = 1;
    const auto original = TilemapToJson(doc.Map());
    const auto original_buffer = reinterpret_cast<std::uintptr_t>(doc.Map().layers[0].cells.data());
    const auto flags = doc.GetTileset().solid.data();
    auto expected = doc.Map();
    expected.Resize(4, 3);

    CHECK(doc.Resize(4, 3) && doc.active_layer == 1);
    CHECK(TilemapToJson(doc.Map()) == TilemapToJson(expected));
    CHECK(doc.GetTileset().solid.data() == flags && doc.GetTileset().IsSolid(3));
    CHECK(doc.Undo() && !doc.dirty() && doc.active_layer == 1);
    CHECK(TilemapToJson(doc.Map()) == original);
    CHECK(reinterpret_cast<std::uintptr_t>(doc.Map().layers[0].cells.data()) == original_buffer);
    CHECK(doc.GetTileset().solid.data() == flags);
    CHECK(doc.Redo() && TilemapToJson(doc.Map()) == TilemapToJson(expected));

    doc.SetSolid(7, true);
    CHECK(doc.Undo() && !doc.GetTileset().IsSolid(7) && doc.Map().width == 4);
    CHECK(doc.Undo() && TilemapToJson(doc.Map()) == original && !doc.dirty());
    CHECK(doc.Redo() && doc.Map().width == 4 && !doc.GetTileset().IsSolid(7));
    CHECK(doc.Redo() && doc.GetTileset().IsSolid(7) && doc.Map().Get(1, 1, 1) == 4);
}

AETHER_TEST(TilemapEditor_WideResizeKeepsRowEdgesAndPadsNewCells) {
    TilemapData map;
    map.Resize(384, 5);
    map.AddLayer("Ground");
    map.AddLayer("Short decoration");
    map.layers[1].collides = false;
    map.Set(0, 383, 0, 3);
    map.Set(0, 0, 1, 5);
    map.Set(0, 256, 3, 8);
    map.Set(0, 257, 3, 9);
    map.Set(0, 0, 4, AutotileCell(0));
    map.Set(1, 11, 1, 7);
    map.layers[1].cells.resize(396);
    auto wide = map;
    wide.Resize(416, 6);
    auto cropped = wide;
    cropped.Resize(257, 4);
    TilemapEditDocument doc(std::move(map), {});
    doc.active_layer = 1;
    const auto old = TilemapToJson(doc.Map());
    const auto* old_storage = doc.Map().layers[0].cells.data();

    CHECK(doc.Resize(416, 6));
    CHECK(TilemapToJson(doc.Map()) == TilemapToJson(wide) && doc.active_layer == 1);
    CHECK(!doc.Map().layers[1].collides && doc.Map().layers[1].name == "Short decoration");
    CHECK(doc.Resize(257, 4));
    CHECK(TilemapToJson(doc.Map()) == TilemapToJson(cropped) && doc.Map().Get(0, 256, 3) == 8);
    CHECK(doc.Undo() && TilemapToJson(doc.Map()) == TilemapToJson(wide));
    CHECK(doc.Undo() && !doc.dirty() && TilemapToJson(doc.Map()) == old);
    CHECK(doc.Map().layers[0].cells.data() == old_storage && doc.active_layer == 1);
    CHECK(doc.Redo() && doc.Redo() && TilemapToJson(doc.Map()) == TilemapToJson(cropped));
}

AETHER_TEST(TilemapEditor_HeightResizeRestoresOnlyCroppedRows) {
    TilemapData map;
    map.Resize(4, 4);
    map.AddLayer("Ground");
    map.AddLayer("Upper");
    map.Set(0, 1, 1, 3);
    map.Set(0, 3, 3, 5);
    map.Set(1, 2, 2, AutotileCell(0));
    TilemapEditDocument doc(std::move(map), {});
    const auto ground = reinterpret_cast<std::uintptr_t>(doc.Map().layers[0].cells.data());
    doc.active_layer = 1;
    CHECK(doc.Resize(4, 2) && doc.Map().height == 2);
    CHECK(doc.Map().Get(0, 1, 1) == 3 && reinterpret_cast<std::uintptr_t>(doc.Map().layers[0].cells.data()) == ground);
    CHECK(doc.Undo() && !doc.dirty() && doc.Map().height == 4 && doc.active_layer == 1);
    CHECK(doc.Map().Get(0, 3, 3) == 5 && doc.Map().Get(1, 2, 2) == AutotileCell(0));
    CHECK(reinterpret_cast<std::uintptr_t>(doc.Map().layers[0].cells.data()) == ground);
    CHECK(doc.Redo() && doc.Map().height == 2);

    CHECK(doc.Resize(4, 3) && doc.Map().Get(1, 2, 2) == kEmpty);
    doc.brush = 8;
    doc.Apply(2, 2);
    CHECK(doc.Map().Get(1, 2, 2) == 8);
    CHECK(doc.Undo() && doc.Undo() && doc.Map().height == 2);
    CHECK(doc.Redo() && doc.Map().height == 3 && doc.Map().Get(1, 2, 2) == kEmpty);
    CHECK(doc.Redo() && doc.Map().Get(1, 2, 2) == 8 && doc.Map().Get(0, 1, 1) == 3);
}

AETHER_TEST(TilemapEditor_DeepHeightCropMovesOldRowsIntoHistory) {
    TilemapData map;
    map.Resize(256, 128);
    map.AddLayer("Floor");
    map.AddLayer("Trim");
    map.layers[1].collides = false;
    map.AddLayer("Short");
    map.Set(2, 0, 0, 17);
    map.layers[2].cells.resize(1);
    map.Set(0, 255, 20, 4);
    map.Set(0, 255, 29, 5);
    map.Set(1, 0, 127, AutotileCell(2));
    TilemapEditDocument doc(std::move(map), {});
    doc.active_layer = 1;
    const auto* original_floor = doc.Map().layers[0].cells.data();
    const auto* original_trim = doc.Map().layers[1].cells.data();
    CHECK(doc.Resize(256, 24) && doc.active_layer == 1 && doc.dirty());
    CHECK(doc.Map().Get(0, 255, 20) == 4 && doc.Map().Get(0, 255, 29) == kEmpty);
    CHECK(!doc.Map().layers[1].collides && doc.Map().Get(2, 0, 0) == 17 &&
          doc.Map().layers[2].cells.size() == 256 * 24 && doc.Map().Get(2, 255, 23) == kEmpty);
    const auto* smaller_floor = doc.Map().layers[0].cells.data();
    CHECK(smaller_floor != original_floor);
    CHECK(doc.Undo() && !doc.dirty());
    CHECK(doc.Map().height == 128 && doc.Map().layers[0].cells.data() == original_floor &&
          doc.Map().layers[1].cells.data() == original_trim);
    CHECK(doc.Map().Get(0, 255, 29) == 5 && doc.Map().Get(1, 0, 127) == AutotileCell(2));
    CHECK(doc.Map().layers[2].cells == std::vector<i32>{17});
    CHECK(doc.Redo() && doc.Map().height == 24 && doc.Map().layers[0].cells.data() == smaller_floor &&
          doc.Map().Get(0, 255, 20) == 4);
}

AETHER_TEST(TilemapEditor_ResizePreservesOverlapAndUndoRestoresCroppedLayers) {
    auto doc = Doc();
    doc.RenameLayer(0, "Ground");
    doc.brush = 2;
    doc.Apply(1, 1);
    doc.Apply(5, 4);
    doc.AddLayer("Decor");
    doc.SetLayerCollides(1, false);
    doc.brush = 7;
    doc.Apply(5, 4);
    CHECK(doc.Resize(3, 3));
    CHECK(doc.Map().width == 3 && doc.Map().height == 3 && doc.Map().Get(0, 1, 1) == 2);
    if (doc.Map().layers.size() != 2) { CHECK(false); return; }
    CHECK(doc.Map().layers[1].name == "Decor" && !doc.Map().layers[1].collides && doc.Map().Get(1, 5, 4) == kEmpty);
    CHECK(doc.Undo() && doc.Map().width == 6 && doc.Map().Get(0, 5, 4) == 2 && doc.Map().Get(1, 5, 4) == 7);
    CHECK(doc.Redo() && doc.Map().width == 3 && doc.Resize(7, 6));
    CHECK(doc.Map().Get(0, 1, 1) == 2 && doc.Map().Get(0, 5, 4) == kEmpty && doc.Map().Get(1, 6, 5) == kEmpty);
}
