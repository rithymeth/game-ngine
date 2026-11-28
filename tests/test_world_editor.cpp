#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"
#include "test_framework.h"
#include "world/world_panels.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <vector>

#if defined(__linux__)
#include <signal.h>
#include <sys/resource.h>
#endif

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

AETHER_TEST(WorldEditor_PartitionMarkerKeepsLocalPrecisionAtLargeOrigins) {
    HeadlessImGui ui;
    RegisterStreamingComponents();
    World game;
    game.CreateEntity(Transform{Vec3(96, 0, 48), Quaternion::Identity()}, StreamingSource{0.0f, true});
    constexpr f32 far = 1073741824.0f; // one float step at this scale exceeds a cell
    WorldIndex index;
    index.settings.origin = Vec3(far, 0, -far);
    index.settings.cell_size = 64.0f;
    for (i32 x = 0; x < 3; ++x) index.cells.push_back({{x, 0}, 1, {}, {}});
    WorldStreamer streamer(game, std::move(index), {});
    streamer.SetOriginOffset({far, 0, -far});
    WorldPartitionPanel panel(streamer, &game);

    ui.Frame([&] {
        panel.Draw();
        const ImVec2 map_min = ImGui::GetItemRectMin();
        ImVec2 low(1e6f, 1e6f), high(-1e6f, -1e6f);
        usize vertices = 0;
        for (const ImDrawVert& vertex : ImGui::GetWindowDrawList()->VtxBuffer) {
            if (vertex.col != IM_COL32(80, 200, 255, 255)) continue; // enabled source marker
            low.x = std::min(low.x, vertex.pos.x);
            low.y = std::min(low.y, vertex.pos.y);
            high.x = std::max(high.x, vertex.pos.x);
            high.y = std::max(high.y, vertex.pos.y);
            ++vertices;
        }
        CHECK(vertices > 0);
        const ImVec2 center((low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f);
        CHECK(std::fabs(center.x - (map_min.x + 36.0f)) < 1.0f);
        CHECK(std::fabs(center.y - (map_min.y + 18.0f)) < 1.0f);
        CHECK(panel.CellAt(center.x, center.y) == CellCoord{1, 0});
    });
}

AETHER_TEST(WorldEditor_ParentedPartitionMarkerMatchesRuntimeCell) {
    HeadlessImGui ui;
    RegisterStreamingComponents();
    World game;
    const EntityGuid rig{1, 1}, mount{1, 2};
    game.CreateEntity(IdComponent{rig}, Transform{Vec3(99, 0, 0), Quaternion::Identity()});
    game.CreateEntity(IdComponent{mount}, Parent{rig}, Transform{Vec3(12, 0, 0), Quaternion::Identity()});
    game.CreateEntity(Parent{mount}, Transform{Vec3(5, 0, 32), Quaternion::Identity()}, StreamingSource{0.0f, true});
    GuidIndex guids;
    guids.Rebuild(game);

    constexpr f32 far = 1073741824.0f;
    WorldIndex index;
    index.settings.origin = Vec3(far, 0, -far);
    index.settings.cell_size = 64.0f;
    for (i32 x = 0; x < 3; ++x) index.cells.push_back({{x, 0}, 0, {}, {}});
    const World empty;
    const auto scene = SaveSceneToMemory(empty);
    WorldStreamer streamer(game, std::move(index), [&](const CellCoord&, std::vector<u8>& bytes) {
        bytes = scene;
        return true;
    }, &guids);
    streamer.SetOriginOffset({far, 0, -far});
    streamer.Update();
    CHECK(streamer.LoadedCells() == std::vector<CellCoord>{{1, 0}});

    WorldPartitionPanel panel(streamer, &game);
    ui.Frame([&] {
        panel.Draw();
        ImVec2 low(1e6f, 1e6f), high(-1e6f, -1e6f);
        usize vertices = 0;
        for (const ImDrawVert& vertex : ImGui::GetWindowDrawList()->VtxBuffer) {
            if (vertex.col != IM_COL32(80, 200, 255, 255)) continue;
            low.x = std::min(low.x, vertex.pos.x);
            low.y = std::min(low.y, vertex.pos.y);
            high.x = std::max(high.x, vertex.pos.x);
            high.y = std::max(high.y, vertex.pos.y);
            ++vertices;
        }
        CHECK(vertices > 0);
        CHECK(panel.CellAt((low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f) == CellCoord{1, 0});
    });
}

AETHER_TEST(WorldEditor_PartitionMapHandlesWidelySeparatedCells) {
    HeadlessImGui ui;
    World game;
    WorldIndex index;
    constexpr i32 low = std::numeric_limits<i32>::min() + 1;
    constexpr i32 high = std::numeric_limits<i32>::max();
    index.cells.push_back({{low, low}, 1, {}, {}});
    index.cells.push_back({{high, high}, 1, {}, {}});
    WorldStreamer streamer(game, std::move(index), {});
    WorldPartitionPanel panel(streamer);
    ui.Frame([&] {
        panel.Draw();
        const ImVec2 start = ImGui::GetItemRectMin();
        CHECK(panel.CellAt(start.x + 12.0f, start.y + 12.0f) == CellCoord{low, low});
        CHECK(panel.CellAt(start.x + 36.0f, start.y + 12.0f) == CellCoord{low + 1, low});
        CHECK(!panel.CellAt(start.x - 1.0f, start.y + 12.0f));
        CHECK(!panel.CellAt(std::numeric_limits<f32>::max(), start.y));
    });
}

// Stage 6: terrain and foliage are assets. Save writes one, Load reads it back, and Dirty says whether what is on
// screen is on disk.

#if defined(__linux__)
AETHER_TEST(WorldEditor_TerrainFailedSavePreservesHeightmapAssets) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "aether_terrain_write_failure";
    fs::create_directories(dir);
    const auto original = dir / "Hills.aterrain";
    const auto other = dir / "Backup.aterrain";
    const auto read = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };

    TerrainEditDocument doc = FlatTerrain();
    CHECK(doc.SaveAs(other) && doc.SaveAs(original));
    const auto original_before = read(original);
    const auto other_before = read(other);
    CHECK(!original_before.empty() && !other_before.empty());
    doc.brush.radius = 3.0f;
    doc.brush.strength = 1.0f;
    doc.BeginStroke();
    doc.Dab(10.0f, 10.0f);
    const auto pending = doc.Data().heightmap.heights;
    CHECK(doc.InStroke() && H(doc, 10, 10) > 0.0f);

    struct rlimit previous_limit{};
    struct sigaction previous_signal{}, ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    const bool read_limit = getrlimit(RLIMIT_FSIZE, &previous_limit) == 0;
    CHECK(read_limit);
    if (!read_limit) return;
    const bool changed_signal = sigaction(SIGXFSZ, &ignore, &previous_signal) == 0;
    CHECK(changed_signal);
    if (!changed_signal) return;
    const struct rlimit blocked{0, previous_limit.rlim_max};
    const bool limited = setrlimit(RLIMIT_FSIZE, &blocked) == 0;
    std::string save_error, copy_error;
    const bool saved = limited && doc.Save(&save_error);
    const bool copied = limited && doc.SaveAs(other, &copy_error);
    const bool restored = !limited || setrlimit(RLIMIT_FSIZE, &previous_limit) == 0;
    const bool signal_restored = sigaction(SIGXFSZ, &previous_signal, nullptr) == 0;
    CHECK(limited && restored && signal_restored);

    CHECK(!saved && !save_error.empty() && !copied && !copy_error.empty());
    CHECK(read(original) == original_before && read(other) == other_before);
    CHECK(doc.Path() == original && doc.Dirty() && doc.CanUndo() && doc.Data().heightmap.heights == pending && !doc.InStroke());
    CHECK(doc.Save() && !doc.Dirty());
    TerrainEditDocument reloaded = FlatTerrain();
    CHECK(reloaded.Load(original) && reloaded.Data().heightmap.heights == pending && read(other) == other_before);
    CHECK(std::distance(fs::directory_iterator(dir), fs::directory_iterator{}) == 2);
    fs::remove_all(dir);
}
#endif

AETHER_TEST(WorldEditor_TerrainSavesAndLoads) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_world_editor_terrain_io";
    std::filesystem::remove_all(dir);
    const std::filesystem::path file = dir / "Hills.aterrain";

    TerrainEditDocument doc = FlatTerrain();
    CHECK(doc.Name() == "Terrain");
    CHECK(!doc.Dirty());
    std::string error;
    CHECK(!doc.Save(&error)); // no file yet
    CHECK(!error.empty());

    doc.tool = TerrainTool::Raise;
    doc.brush.radius = 3.0f;
    doc.brush.strength = 1.0f;
    doc.brush.falloff = 0.5f;
    doc.raise_height = 0.5f;
    doc.BeginStroke();
    doc.Dab(10.0f, 10.0f);
    CHECK(doc.InStroke());
    error.clear();
    CHECK(doc.SaveAs(file, &error)); // ends the stroke first
    CHECK(!doc.InStroke());

    TerrainEditDocument loaded = FlatTerrain();
    error.clear();
    CHECK(loaded.Load(file, &error));
    CHECK(error.empty());
    CHECK(!loaded.Dirty());
    CHECK(!doc.Dirty());
    CHECK(doc.Name() == "Hills");
    CHECK(loaded.Path() == file);
    CHECK(!loaded.CanUndo()); // a load isn't an edit
    CHECK(loaded.Data().heightmap.width == doc.Data().heightmap.width);
    CHECK(loaded.Data().heightmap.heights == doc.Data().heightmap.heights);
    CHECK(loaded.Data().chunk_count_x == doc.Data().chunk_count_x);
    CHECK(loaded.Data().chunk_count_z == doc.Data().chunk_count_z);
    CHECK(loaded.Splatmap().width == doc.Splatmap().width);
    CHECK(loaded.Splatmap().height == doc.Splatmap().height);
    CHECK(loaded.Splatmap().pixels == doc.Splatmap().pixels);
    CHECK(loaded.Settings().chunk_world_size == doc.Settings().chunk_world_size);
    CHECK(loaded.Settings().verts_per_chunk == doc.Settings().verts_per_chunk);
    CHECK(loaded.Settings().lod_distance == doc.Settings().lod_distance);
    CHECK(loaded.Data().layers.size() == doc.Data().layers.size());
    CHECK(loaded.Data().layers.back().name == "Rock"); // the default layer plus the one FlatTerrain adds
    CHECK(loaded.Data().layers.back().roughness == 0.9f);
    CHECK(H(loaded, 10, 10) > 0.0f); // the stroke is in the file
    CHECK(H(loaded, 10, 10) == H(doc, 10, 10));

    // A dab after saving dirties it again, and Save goes to the same file.
    doc.DabOnce(40.0f, 40.0f);
    CHECK(doc.Dirty());
    CHECK(doc.Save(&error));
    CHECK(!doc.Dirty());
    CHECK(H(doc, 40, 40) > 0.0f);
}

AETHER_TEST(WorldEditor_TerrainRefusesAForeignFile) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_world_editor_terrain_refuse";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    TerrainEditDocument doc = FlatTerrain();
    doc.DabOnce(10.0f, 10.0f);
    const std::vector<f32> heights = doc.Data().heightmap.heights;
    const std::vector<u8> pixels = doc.Splatmap().pixels;

    auto write = [&](const char* name, const char* text) {
        std::ofstream out(dir / name, std::ios::binary);
        out << text;
    };
    // A newer format version, an older one, a file that names another format, and a file that isn't JSON at all.
    write("Newer.aterrain", R"({"format": "aether.terrain", "version": 99})");
    write("Ancient.aterrain", R"({"format": "aether.terrain", "version": 0})");
    write("Foliage.aterrain", R"({"format": "aether.foliage", "version": 1})");
    write("Broken.aterrain", "{ not json");

    for (const char* name : {"Newer.aterrain", "Ancient.aterrain", "Foliage.aterrain", "Broken.aterrain"}) {
        std::string error;
        CHECK(!doc.Load(dir / name, &error));
        CHECK(!error.empty());
        CHECK(doc.Data().heightmap.heights == heights); // the document is unchanged
        CHECK(doc.Splatmap().pixels == pixels);
        CHECK(doc.Path().empty()); // and the failed file isn't the open one
        CHECK(doc.Dirty()); // nor was anything marked saved
    }
}

AETHER_TEST(WorldEditor_FoliageSavesAndLoads) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_world_editor_foliage_io";
    std::filesystem::remove_all(dir);
    const std::filesystem::path file = dir / "Forest.afoliage";

    FoliageLayer layer;
    layer.name = "Forest";
    layer.types.push_back(FoliageType{});
    FoliageType& type = layer.types[0];
    type.scale_min = 0.5f;
    type.scale_max = 1.5f;
    type.rotation_range = 3.0f;
    type.align_to_normal = false;
    type.anchor_offset = 0.25f;
    layer.density = 0.5f;
    layer.max_instances = 1000;
    DensityMap density;
    density.width = density.height = 32;
    density.cells.assign(32u * 32u, 255);
    FoliagePaintDocument doc(std::move(layer), std::move(density), [](f32, f32) { return 2.0f; }, 7);
    CHECK(!doc.Dirty());
    std::string error;
    CHECK(!doc.Save(&error)); // no file yet
    CHECK(!error.empty());

    doc.DabOnce({16.0f, 0.0f, 16.0f});
    CHECK(doc.Dirty());
    CHECK(!doc.Layer().instances.empty());
    CHECK(doc.SaveAs(file, &error));
    CHECK(!doc.Dirty());
    CHECK(doc.Name() == "Forest");
    CHECK(doc.Layer().seed == 7); // the file records the seed painting uses

    FoliagePaintDocument loaded(FoliageLayer{}, DensityMap{}, {}, 7);
    error.clear();
    CHECK(loaded.Load(file, &error));
    CHECK(error.empty());
    CHECK(!loaded.Dirty());
    CHECK(!loaded.CanUndo());
    CHECK(loaded.Path() == file);
    CHECK(loaded.Layer().name == "Forest");
    CHECK(loaded.Layer().seed == 7);
    CHECK(loaded.Layer().types.size() == 1);
    CHECK(loaded.Layer().types[0].scale_min == 0.5f);
    CHECK(loaded.Layer().types[0].scale_max == 1.5f);
    CHECK(loaded.Layer().types[0].rotation_range == 3.0f);
    CHECK(!loaded.Layer().types[0].align_to_normal);
    CHECK(loaded.Layer().types[0].anchor_offset == 0.25f);
    CHECK(loaded.Density().width == 32);
    CHECK(loaded.Density().cells == doc.Density().cells);
    CHECK(loaded.Layer().instances.size() == doc.Layer().instances.size());
    bool same = true;
    for (usize i = 0; i < doc.Layer().instances.size() && same; ++i) {
        const FoliageInstance& a = doc.Layer().instances[i];
        const FoliageInstance& b = loaded.Layer().instances[i];
        same = a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z &&
               a.normal.x == b.normal.x && a.normal.y == b.normal.y && a.normal.z == b.normal.z &&
               a.scale.x == b.scale.x && a.scale.y == b.scale.y && a.scale.z == b.scale.z &&
               a.rotation.x == b.rotation.x && a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z &&
               a.rotation.w == b.rotation.w && a.type_index == b.type_index;
    }
    CHECK(same);
    CHECK(doc.Layer().instances[0].position.y == 2.25f); // the ground plus the anchor

    // A refused file leaves the document alone: an instance naming a type that isn't there.
    {
        std::ofstream out(dir / "Bad.afoliage", std::ios::binary);
        out << R"({"format":"aether.foliage","version":1,"layer":{"name":"Bad","types":[],)"
               R"("instances":[{"position":[0,0,0],"normal":[0,1,0],"scale":[1,1,1],"rotation":[0,0,0,1],"type_index":3}]}})";
    }
    const usize before = loaded.Layer().instances.size();
    CHECK(!loaded.Load(dir / "Bad.afoliage", &error));
    CHECK(!error.empty());
    CHECK(loaded.Layer().instances.size() == before);
    CHECK(loaded.Layer().name == "Forest");
    CHECK(loaded.Path() == file);
}
