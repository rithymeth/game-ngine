#include "world/terrain_panel.h"
#include "test_framework.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <functional>

// Phase 21: the Terrain panel - picking, sculpt and paint strokes as undo
// steps, dirty chunks, the brush cursor, and the panel drawn headless.

using namespace aether;
using namespace aether::editor;
using namespace aether::terrain;

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
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1400, 900));
        ImGui::Begin("Terrain", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

// A flat 65 x 65 terrain, 1 m cells, in 2 x 2 chunks of 32 m, with a 64 x 64 splatmap on layer 0.
struct Fixture {
    TerrainSettings settings;
    TerrainData data;
    SplatmapData splat;

    Fixture() {
        settings.chunk_world_size = 32.0f;
        settings.verts_per_chunk = 33;
        Heightmap hm;
        hm.width = hm.height = 65;
        hm.cell_size = 1.0f;
        hm.heights.assign(65u * 65u, 0.0f);
        InitTerrainData(data, hm, settings);
        data.layers.resize(3);
        splat.width = splat.height = 64;
        splat.pixels.assign(64u * 64u * 4u, 0);
        for (usize i = 0; i < splat.pixels.size(); i += 4) splat.pixels[i] = 255;
    }
};

} // namespace

AETHER_TEST(TerrainEditor_PickAndCursor) {
    Fixture f;
    TerrainPanel panel(f.data, f.splat, f.settings);
    AETHER_CHECK(!panel.HasHover());
    AETHER_CHECK(!panel.Pick(Vec3(10, 10, 10), Vec3(0, 1, 0), 100.0f));
    AETHER_CHECK(!panel.HasHover());

    AETHER_CHECK(panel.Pick(Vec3(20, 10, 30), Vec3(0, -1, 0), 100.0f));
    AETHER_CHECK_NEAR(panel.Hover().x, 20.0f, 1e-3f);
    AETHER_CHECK_NEAR(panel.Hover().y, 0.0f, 1e-3f);
    AETHER_CHECK_NEAR(panel.Hover().z, 30.0f, 1e-3f);

    panel.brush.radius = 4.0f;
    std::vector<TerrainOverlayLine> lines;
    panel.BuildCursor(lines);
    AETHER_CHECK(lines.size() == 48 * 2 + 1);
    // Every ring point is on the brush circle, hugging the (flat) ground.
    for (usize i = 0; i < 48; ++i) {
        const Vec3 d = lines[i].a - panel.Hover();
        AETHER_CHECK_NEAR(std::sqrt(d.x * d.x + d.z * d.z), 4.0f, 1e-3f);
        AETHER_CHECK_NEAR(lines[i].a.y, 0.05f, 1e-3f);
    }
    panel.show_cursor = false;
    lines.clear();
    panel.BuildCursor(lines);
    AETHER_CHECK(lines.empty());
}

AETHER_TEST(TerrainEditor_SculptStrokeUndoRedo) {
    Fixture f;
    TerrainPanel panel(f.data, f.splat, f.settings);
    panel.tool = TerrainTool::Sculpt;
    panel.brush.radius = 4.0f;
    panel.brush.strength = 1.0f;
    panel.brush.falloff = 0.0f;
    panel.brush.mode = SculptMode::Raise;

    // Strokes need a BeginStroke; an empty stroke is not an undo step.
    AETHER_CHECK(!panel.Stroke(Vec3(10, 0, 10), 0.1f));
    panel.BeginStroke();
    AETHER_CHECK(panel.InStroke() && !panel.EndStroke() && panel.UndoDepth() == 0);

    const std::vector<f32> original = f.data.heightmap.heights;
    panel.BeginStroke();
    AETHER_CHECK(panel.Stroke(Vec3(10, 0, 10), 0.1f));
    AETHER_CHECK(panel.Stroke(Vec3(11, 0, 10), 0.1f));
    AETHER_CHECK(!panel.Stroke(Vec3(500, 0, 10), 0.1f)); // off the terrain
    AETHER_CHECK(panel.EndStroke());
    AETHER_CHECK(panel.UndoDepth() == 1 && panel.RedoDepth() == 0);

    const std::vector<f32> raised = f.data.heightmap.heights;
    AETHER_CHECK(raised != original);
    AETHER_CHECK(f.data.heightmap.GetHeight(10, 10) > 0.5f);
    AETHER_CHECK(f.data.bounds_max.y > 0.5f);
    // Nothing outside the brush changed.
    AETHER_CHECK(f.data.heightmap.GetHeight(40, 40) == 0.0f);

    // A stroke at (10,10) with radius 4 stays inside chunk (0,0).
    const auto dirty = panel.TakeDirtyChunks();
    AETHER_CHECK(dirty.size() == 1 && dirty[0] == TerrainChunkCoord({0, 0}));
    AETHER_CHECK(panel.DirtyChunkCount() == 0);

    AETHER_CHECK(panel.Undo());
    AETHER_CHECK(f.data.heightmap.heights == original && f.data.bounds_max.y == 0.0f);
    AETHER_CHECK(panel.UndoDepth() == 0 && panel.RedoDepth() == 1 && !panel.Undo());
    AETHER_CHECK(panel.TakeDirtyChunks().size() == 1); // undo marks the chunk again

    AETHER_CHECK(panel.Redo());
    AETHER_CHECK(f.data.heightmap.heights == raised);
    AETHER_CHECK(!panel.Redo());

    // A new stroke drops the redo history.
    panel.Undo();
    panel.BeginStroke();
    panel.Stroke(Vec3(50, 0, 50), 0.1f);
    AETHER_CHECK(panel.EndStroke() && panel.RedoDepth() == 0);

    // Undo is refused mid-stroke.
    panel.BeginStroke();
    AETHER_CHECK(!panel.Undo());
    panel.EndStroke();
}

AETHER_TEST(TerrainEditor_StrokeOnAChunkSeamMarksBothChunks) {
    Fixture f;
    TerrainPanel panel(f.data, f.splat, f.settings);
    panel.brush.radius = 3.0f;
    panel.BeginStroke();
    panel.Stroke(Vec3(32, 0, 10), 0.1f); // x = 32 is the seam between chunk 0 and 1
    panel.EndStroke();
    const auto dirty = panel.TakeDirtyChunks();
    AETHER_CHECK(dirty.size() == 2 && dirty[0] == TerrainChunkCoord({0, 0}) && dirty[1] == TerrainChunkCoord({1, 0}));
}

AETHER_TEST(TerrainEditor_SculptModes) {
    Fixture f;
    f.data.heightmap.heights[20u * 65u + 20u] = 5.0f; // a spike
    TerrainPanel panel(f.data, f.splat, f.settings);
    panel.brush.radius = 4.0f;
    panel.brush.strength = 1.0f;
    panel.brush.falloff = 0.0f;

    panel.brush.mode = SculptMode::Smooth;
    panel.BeginStroke();
    panel.Stroke(Vec3(20, 0, 20), 0.1f);
    panel.EndStroke();
    AETHER_CHECK(f.data.heightmap.GetHeight(20, 20) < 5.0f); // smoothed down

    panel.brush.mode = SculptMode::Flatten;
    panel.BeginStroke();
    panel.Stroke(Vec3(20, 0, 20), 0.1f);
    panel.EndStroke();
    AETHER_CHECK(panel.UndoDepth() == 2);

    // Lower: negative strength.
    Fixture g;
    g.data.heightmap.heights.assign(65u * 65u, 2.0f);
    TerrainPanel lower(g.data, g.splat, g.settings);
    lower.brush.radius = 3.0f;
    lower.brush.strength = -1.0f;
    lower.brush.falloff = 0.0f;
    lower.BeginStroke();
    lower.Stroke(Vec3(30, 0, 30), 0.1f);
    lower.EndStroke();
    AETHER_CHECK(g.data.heightmap.GetHeight(30, 30) < 2.0f);
    AETHER_CHECK(g.data.bounds_min.y < 2.0f);
}

AETHER_TEST(TerrainEditor_PaintStrokeUndoRedo) {
    Fixture f;
    TerrainPanel panel(f.data, f.splat, f.settings);
    panel.tool = TerrainTool::Paint;
    panel.brush.radius = 4.0f;
    panel.brush.strength = 1.0f;
    panel.brush.falloff = 0.0f;
    panel.brush.layer_index = 1;
    const std::vector<u8> original = f.splat.pixels;

    panel.BeginStroke();
    AETHER_CHECK(panel.Stroke(Vec3(32, 0, 32), 0.1f));
    AETHER_CHECK(panel.EndStroke());
    AETHER_CHECK(f.splat.GetWeights(0.5f, 0.5f)[1] > 0.5f);
    AETHER_CHECK(f.splat.GetWeights(0.05f, 0.05f)[0] > 0.99f); // far corner untouched
    AETHER_CHECK(!panel.TakeDirtyChunks().empty());
    AETHER_CHECK(f.data.heightmap.heights == std::vector<f32>(65u * 65u, 0.0f)); // paint doesn't touch heights

    const std::vector<u8> painted = f.splat.pixels;
    AETHER_CHECK(panel.Undo() && f.splat.pixels == original);
    AETHER_CHECK(panel.Redo() && f.splat.pixels == painted);

    // A layer index beyond the terrain's layers is clamped, not out of bounds.
    panel.brush.layer_index = 9;
    panel.BeginStroke();
    AETHER_CHECK(panel.Stroke(Vec3(10, 0, 10), 0.1f));
    panel.EndStroke();
    AETHER_CHECK(f.splat.GetWeights(10.0f / 64.0f, 10.0f / 64.0f)[2] > 0.5f);

    // No splatmap: painting reports why.
    SplatmapData none;
    TerrainPanel bare(f.data, none, f.settings);
    bare.tool = TerrainTool::Paint;
    bare.BeginStroke();
    AETHER_CHECK(!bare.Stroke(Vec3(10, 0, 10), 0.1f) && bare.Status().find("splatmap") != std::string::npos);
    AETHER_CHECK(!bare.EndStroke());
}

AETHER_TEST(TerrainEditor_HistoryIsBounded) {
    Fixture f;
    TerrainPanel panel(f.data, f.splat, f.settings);
    panel.max_history = 3;
    panel.brush.radius = 2.0f;
    for (int i = 0; i < 6; ++i) {
        panel.BeginStroke();
        panel.Stroke(Vec3(10.0f + static_cast<f32>(i) * 8.0f, 0, 10), 0.1f);
        panel.EndStroke();
    }
    AETHER_CHECK(panel.UndoDepth() == 3);
}

AETHER_TEST(TerrainEditor_PanelDrawsHeadless) {
    Fixture f;
    TerrainPanel panel(f.data, f.splat, f.settings);
    HeadlessImGui ui;
    ui.Frame([&] { panel.Draw(); });
    panel.brush.mode = SculptMode::Noise;
    ui.Frame([&] { panel.Draw(); });
    panel.brush.mode = SculptMode::Erode;
    ui.Frame([&] { panel.Draw(); });
    panel.tool = TerrainTool::Paint;
    panel.Pick(Vec3(20, 10, 30), Vec3(0, -1, 0), 100.0f);
    panel.BeginStroke();
    panel.Stroke(panel.Hover(), 0.1f);
    panel.EndStroke();
    ui.Frame([&] { panel.Draw(); });
}
