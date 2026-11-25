#include "test_framework.h"
#include "aether/pak/pak.h"
#include "packaging/build_window.h"
#include "packaging/new_project_panel.h"
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
    build->launch_args = "--headless --frames 2";
    if (!build->Launch(&error)) {
        CHECK(false);
        return;
    }
    int code = -1;
    CHECK(build->Game().Wait(&code) && code == 0);
}
