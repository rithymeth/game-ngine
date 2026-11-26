#include "test_framework.h"
#include "aether/pak/pak.h"
#include "aether/assets/asset_database.h"
#include "aether/assets/png_writer.h"
#include "aether/project/project.h"
#include "core/editor_document.h"
#include "packaging/build_window.h"
#include "packaging/new_project_panel.h"
#include "sprite2d/tilemap_document.h"
#include "sprite2d/tilemap_panel.h"
#include "workspace/editor_workspace.h"

#include <imgui.h>

#include <algorithm>
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
