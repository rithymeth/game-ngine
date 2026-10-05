#include "aether/scene/components.h"
#include "test_framework.h"
#include "world/world_panels.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <functional>

// Phase 21 step 7: the world-building editors - terrain sculpting and
// painting (strokes, region undo, dirty chunks, the brush cursor), foliage
// painting, spline editing, and the panels with the partition map, drawn
// headless.

using namespace aether;
using namespace aether::editor;
using namespace aether::terrain;
using namespace aether::streaming;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGui::GetIO().ConfigMacOSXBehaviors = false; // tests press Ctrl on every platform
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1200, 800);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1200, 800));
        ImGui::Begin("World", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

// A flat 63 x 63 terrain (2 x 2 chunks of 32), with a 64 x 64 splatmap of the first layer.
TerrainEditDocument FlatTerrain() {
    Heightmap hm;
    hm.width = hm.height = 63;
    hm.cell_size = 1.0f;
    hm.heights.assign(63u * 63u, 0.0f);
    TerrainSettings settings;
    settings.verts_per_chunk = 32;
    settings.chunk_world_size = 31.0f;
    TerrainData data;
    InitTerrainData(data, hm, settings);
    data.layers.push_back(TerrainLayer{"Rock", Vec3(0.5f, 0.5f, 0.5f), 0.0f, 0.9f, 1.0f});
    std::vector<SplatmapLayer> layers(2);
    return TerrainEditDocument(data, settings, BuildSplatmap(64, layers, {}));
}

f32 H(const TerrainEditDocument& d, u32 x, u32 z) { return d.Data().heightmap.GetHeight(x, z); }

} // namespace

AETHER_TEST(WorldEditor_TerrainStrokes) {
    TerrainEditDocument doc = FlatTerrain();
    CHECK(doc.WorldWidth() == 62.0f && doc.Data().chunk_count_x == 2);
    doc.brush.radius = 3.0f;
    doc.brush.strength = 1.0f;
    doc.brush.falloff = 0.5f;
    doc.raise_height = 0.5f;
    // A stroke of three dabs: one undo step.
    doc.BeginStroke();
    doc.Dab(10, 10);
    doc.Dab(10, 10);
    doc.Dab(12, 10);
    doc.EndStroke();
    CHECK(H(doc, 10, 10) > 0.9f);
    CHECK(H(doc, 30, 30) == 0.0f);
    CHECK(doc.UndoDepth() == 1);
    // Only the first chunk (and nothing else) waits to rebuild.
    auto dirty = doc.TakeDirtyChunks();
    CHECK(dirty.size() == 1 && dirty.count({0, 0}) == 1);
    CHECK(doc.TakeDirtyChunks().empty());
    // Undo restores exactly; redo reapplies; both re-mark the chunk.
    const f32 raised = H(doc, 10, 10);
    CHECK(doc.Undo());
    CHECK(H(doc, 10, 10) == 0.0f && H(doc, 12, 10) == 0.0f);
    CHECK(doc.TakeDirtyChunks().count({0, 0}) == 1);
    CHECK(doc.Redo() && H(doc, 10, 10) == raised);
    // A dab on the shared edge (x = 31) dirties both chunks along it.
    doc.TakeDirtyChunks();
    doc.DabOnce(31, 10);
    dirty = doc.TakeDirtyChunks();
    CHECK(dirty.count({0, 0}) == 1 && dirty.count({1, 0}) == 1 && dirty.count({0, 1}) == 0);
    // Lower, flatten and smooth.
    doc.tool = TerrainTool::Lower;
    doc.DabOnce(50, 50);
    CHECK(H(doc, 50, 50) < 0.0f);
    doc.tool = TerrainTool::Flatten;
    const f32 before = H(doc, 12, 10);
    doc.DabOnce(10, 10); // toward the (raised) center: the slope nearby rises
    CHECK(H(doc, 12, 10) >= before);
    // Smoothing works on slopes (a plateau averages to itself): the bump's flank moves.
    doc.tool = TerrainTool::Smooth;
    const f32 flank = H(doc, 33, 10);
    doc.brush.radius = 1.0f;
    doc.DabOnce(33, 10);
    CHECK(H(doc, 33, 10) != flank);
    doc.brush.radius = 3.0f;
    // A stroke that changes nothing isn't an undo step.
    const usize depth = doc.UndoDepth();
    doc.tool = TerrainTool::Raise;
    doc.brush.strength = 0.0f;
    doc.DabOnce(40, 40);
    CHECK(doc.UndoDepth() == depth);
    // Painting changes the splatmap (and undoes).
    doc.tool = TerrainTool::Paint;
    doc.brush.strength = 1.0f;
    doc.brush.layer_index = 1;
    const auto splat_before = doc.Splatmap().pixels;
    doc.DabOnce(31, 31);
    CHECK(doc.Splatmap().pixels != splat_before);
    CHECK(doc.Splatmap().GetWeights(0.5f, 0.5f)[1] > 0.5f);
    CHECK(doc.Undo() && doc.Splatmap().pixels == splat_before);
    // The cursor follows the ground (a ring inside the raised, flattened plateau).
    doc.brush.radius = 1.5f;
    const auto ring = doc.CursorRing(10, 10, 16);
    CHECK(ring.size() == 16);
    CHECK(std::all_of(ring.begin(), ring.end(), [](const Vec3& p) { return p.y > 0.05f; })); // on the raised ground
    CHECK(std::fabs(std::hypot(ring[0].x - 10.0f, ring[0].z - 10.0f) - 1.5f) < 1e-4f);
}

AETHER_TEST(WorldEditor_TerrainPointer) {
    TerrainEditDocument doc = FlatTerrain();
    doc.brush.radius = 4.0f;
    TerrainToolPanel panel(doc);
    // A drag from x = 5 to 15 in 0.1 m steps: dabs a metre apart (a quarter radius), one stroke.
    for (int i = 0; i <= 100; ++i) panel.Pointer(true, 5.0f + static_cast<f32>(i) / 10.0f, 20.0f);
    panel.Pointer(false, 15.0f, 20.0f);
    CHECK(panel.Dabs() == 11);
    CHECK(doc.UndoDepth() == 1 && !doc.InStroke());
    CHECK(H(doc, 10, 20) > 0.0f);
}

AETHER_TEST(WorldEditor_Foliage) {
    FoliageLayer layer;
    layer.types.resize(1);
    layer.types[0].align_to_normal = false;
    layer.types[0].anchor_offset = 0.25f;
    layer.density = 1.0f;
    // Ground: height = x / 10.
    FoliagePaintDocument doc(layer, {}, [](f32 x, f32) { return x / 10.0f; }, 5);
    doc.radius = 3.0f;
    doc.DabOnce(Vec3(20, 0, 20));
    const usize painted = doc.Layer().instances.size();
    CHECK(painted >= 25 && painted <= 32); // about pi x 9
    for (const FoliageInstance& i : doc.Layer().instances) CHECK(std::fabs(i.position.y - (i.position.x / 10.0f + 0.25f)) < 1e-4f);
    // Erase: everything within 3 m of (20, 20) goes.
    doc.erase = true;
    doc.DabOnce(Vec3(20, 0, 20));
    CHECK(doc.Layer().instances.empty());
    CHECK(doc.Undo() && doc.Layer().instances.size() == painted);
    CHECK(doc.Redo() && doc.Layer().instances.empty());
    CHECK(doc.Undo());
    // An eraser stroke over nothing isn't an undo step.
    doc.erase = true;
    doc.DabOnce(Vec3(100, 0, 100));
    CHECK(doc.Undo() && doc.Layer().instances.empty()); // undoes the paint (the empty erase added no step)
    CHECK(doc.Redo() && doc.Layer().instances.size() == painted);
    // Types: removing one drops its instances and renumbers the rest.
    doc.erase = false;
    const usize t1 = doc.AddType(FoliageType{});
    CHECK(t1 == 1);
    CHECK(doc.CountsByType().size() == 2);
    CHECK(doc.RemoveType(0));
    CHECK(doc.Layer().types.size() == 1 && doc.Layer().instances.empty()); // they were all type 0
    CHECK(!doc.RemoveType(5));
    CHECK(doc.Undo() && doc.Layer().types.size() == 2);
    // Density and the cap.
    doc.SetDensity(-1.0f);
    CHECK(doc.Layer().density == 0.0f);
    doc.SetDensity(2.0f);
    doc.DabOnce(Vec3(50, 0, 50));
    CHECK(doc.Layer().instances.size() > painted);
    doc.SetMaxInstances(10);
    CHECK(doc.Layer().instances.size() == 10);
    doc.Clear();
    CHECK(doc.Layer().instances.empty() && doc.Undo() && doc.Layer().instances.size() == 10);
}

AETHER_TEST(WorldEditor_Splines) {
    SplineEditDocument doc;
    CHECK(doc.AddPoint(Vec3(0, 0, 0)) == 0);
    CHECK(doc.AddPoint(Vec3(10, 0, 0)) == 1);
    CHECK(doc.AddPoint(Vec3(20, 0, 10)) == 2);
    CHECK(doc.selected == 2u);
    const auto inserted = doc.InsertAfter(0);
    CHECK(inserted == 1u && doc.Get().PointCount() == 4);
    CHECK(doc.Get().Points()[1].position.x == 5.0f);
    CHECK(!doc.InsertAfter(3)); // nothing after the last
    // A drag is one undo step.
    for (int i = 1; i <= 10; ++i) CHECK(doc.MovePoint(1, Vec3(5, 0, static_cast<f32>(i)), true));
    CHECK(doc.Get().Points()[1].position.z == 10.0f);
    CHECK(doc.Undo() && doc.Get().Points()[1].position.z == 0.0f);
    CHECK(doc.Redo() && doc.Get().Points()[1].position.z == 10.0f);
    CHECK(doc.SetWidth(1, 4.0f) && doc.Get().Points()[1].width == 4.0f && !doc.SetWidth(1, -1.0f));
    CHECK(doc.SetRoll(1, 15.0f) && doc.Get().Points()[1].roll == 15.0f);
    CHECK(doc.Undo() && doc.Get().Points()[1].roll == 0.0f); // and the width stays
    CHECK(doc.Get().Points()[1].width == 4.0f);
    // Picking the nearest within a radius.
    CHECK(doc.PickPoint(Vec3(9.5f, 3, 0.4f), 1.0f) == 2u);
    CHECK(!doc.PickPoint(Vec3(50, 0, 50), 1.0f));
    // What the viewport draws.
    CHECK(doc.CurvePoints(8).size() == 3u * 8u + 1u);
    std::vector<Vec3> left, right;
    doc.RoadEdges(4, left, right);
    CHECK(left.size() == 3u * 4u + 1u && right.size() == left.size());
    CHECK(std::hypot(left[0].x - right[0].x, left[0].z - right[0].z) > 0.5f);
    // Removing keeps the selection valid.
    doc.selected = 3;
    CHECK(doc.RemovePoint(3) && doc.selected == 2u);
    CHECK(!doc.RemovePoint(9));
    // The viewport: Ctrl+click adds, a click picks, a drag moves.
    SplinePanel panel(doc);
    panel.Pointer(true, true, Vec3(30, 0, 30));
    panel.Pointer(false, false, Vec3(30, 0, 30));
    CHECK(doc.Get().PointCount() == 4 && doc.selected == 3u);
    panel.Pointer(true, false, Vec3(0.2f, 0, 0.1f));
    CHECK(doc.selected == 0u);
    panel.Pointer(true, false, Vec3(-3, 0, 0));
    panel.Pointer(true, false, Vec3(-4, 0, 0));
    panel.Pointer(false, false, Vec3(-4, 0, 0));
    CHECK(doc.Get().Points()[0].position.x == -4.0f);
    CHECK(doc.Undo() && doc.Get().Points()[0].position.x == 0.0f); // the drag, in one step
}

AETHER_TEST(WorldEditor_PanelsHeadless) {
    HeadlessImGui ui;
    TerrainEditDocument terrain_doc = FlatTerrain();
    TerrainToolPanel terrain_panel(terrain_doc);
    FoliageLayer layer;
    layer.types.resize(1);
    FoliagePaintDocument foliage_doc(layer);
    FoliagePanel foliage_panel(foliage_doc);
    SplineEditDocument spline_doc;
    spline_doc.AddPoint(Vec3());
    spline_doc.AddPoint(Vec3(10, 0, 0));
    SplinePanel spline_panel(spline_doc);
    // A partitioned world: cells (0, 0) and (2, 1).
    World src;
    RegisterStreamingComponents();
    src.CreateEntity(Transform{Vec3(10, 0, 10), Quaternion::Identity()});
    src.CreateEntity(Transform{Vec3(140, 0, 70), Quaternion::Identity()});
    PartitionSettings ps;
    ps.cell_size = 64.0f;
    const PartitionResult part = PartitionWorld(src, ps);
    World game;
    game.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, StreamingSource{30.0f, true});
    WorldStreamer streamer(game, part.index, [&](const CellCoord& c, std::vector<u8>& bytes) {
        const auto it = part.cells.find(c);
        if (it == part.cells.end()) return false;
        bytes = it->second;
        return true;
    });
    streamer.Update();
    CHECK(streamer.State({0, 0}) == CellState::Loaded);
    WorldPartitionPanel map(streamer, &game);
    for (TerrainTool t : {TerrainTool::Raise, TerrainTool::Paint}) {
        terrain_doc.tool = t;
        ui.Frame([&] {
            terrain_panel.Draw();
            foliage_panel.Draw();
            spline_panel.Draw();
            map.Draw();
        });
    }
    // The map covers cells x 0..2, z 0..1; clicking one pins it.
    ui.Frame([&] { map.Draw(); });
    const auto first = map.CellAt(0, 0);
    CHECK(!first); // (0, 0) on screen is the window's corner, not the map
    std::optional<CellCoord> found;
    for (f32 y = 0; y < 800 && !found; y += 4)
        for (f32 x = 0; x < 600 && !found; x += 4)
            if (auto c = map.CellAt(x, y); c && *c == CellCoord{2, 1}) found = c;
    CHECK(found.has_value());
    CHECK(!map.IsPinned({2, 1}));
    map.TogglePin({2, 1});
    CHECK(map.IsPinned({2, 1}));
    streamer.Update();
    CHECK(streamer.State({2, 1}) == CellState::Loaded);
    map.TogglePin({2, 1});
    streamer.Update();
    CHECK(streamer.State({2, 1}) == CellState::Unloaded);
    ui.Frame([&] { map.Draw(); });
}
