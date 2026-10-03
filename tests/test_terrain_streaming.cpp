#include "aether/scene/components.h"
#include "aether/scene/serialization.h"
#include "aether/terrain/streaming.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>

using namespace aether;
using namespace aether::terrain;

namespace {

namespace stdfs = std::filesystem;

WorldPartitionSettings Settings() {
    WorldPartitionSettings s;
    s.cell_size = 100.0f;
    s.load_radius = 120.0f;
    s.unload_radius = 250.0f;
    s.max_loads_per_update = 0;
    return s;
}

// Writes a cell scene with `count` entities at the cell's center.
void WriteCell(const std::string& dir, CellCoord cell, int count) {
    World w;
    for (int i = 0; i < count; ++i) {
        Transform t;
        t.position = Vec3(static_cast<f32>(cell.x) * 100.0f + 50.0f, static_cast<f32>(i), static_cast<f32>(cell.z) * 100.0f + 50.0f);
        w.CreateEntity(t);
    }
    SaveScene(w, CellScenePath(dir, cell));
}

struct Dir {
    stdfs::path path;
    explicit Dir(const char* name) : path(stdfs::temp_directory_path() / name) {
        stdfs::remove_all(path);
        stdfs::create_directories(path);
    }
    ~Dir() { stdfs::remove_all(path); }
    std::string str() const { return path.string(); }
};

// A cell has content when its scene file exists.
CellSceneStreamer::PathFn PathsIn(const std::string& dir) {
    return [dir](CellCoord c) {
        const std::string path = CellScenePath(dir, c);
        return stdfs::exists(path) ? path : std::string();
    };
}

} // namespace

AETHER_TEST(CellStreaming_ScenePaths) {
    AETHER_CHECK(CellScenePath("maps/world", {2, -3}) == "maps/world/cell_2_-3.aesc");
    AETHER_CHECK(CellScenePath("maps/world/", {0, 0}) == "maps/world/cell_0_0.aesc");
    AETHER_CHECK(CellScenePath("", {1, 1}) == "cell_1_1.aesc");
}

AETHER_TEST(CellStreaming_LoadsAndUnloadsEntities) {
    Dir dir("aether_test_cell_streaming");
    WriteCell(dir.str(), {0, 0}, 3);
    WriteCell(dir.str(), {1, 0}, 2);
    WriteCell(dir.str(), {2, 0}, 4);
    WriteCell(dir.str(), {-1, 0}, 1);

    World world;
    world.CreateEntity(Transform{}); // pre-existing content must survive
    CellSceneStreamer streamer(world, Settings(), PathsIn(dir.str()));

    // Focus in the middle of cell (0,0): the 3x3 block in range holds (0,0), (1,0), (-1,0).
    CellStreamStats s = streamer.Update(50.0, 50.0);
    AETHER_CHECK(s.cells_loaded == 9 && s.cells_failed == 0);
    AETHER_CHECK(s.entities_created == 3 + 2 + 1);
    AETHER_CHECK(world.EntityCount() == 1 + 6);
    AETHER_CHECK(streamer.EntitiesOf({0, 0}).size() == 3 && streamer.EntitiesOf({1, 0}).size() == 2);
    AETHER_CHECK(streamer.EntitiesOf({5, 5}).empty());

    // Staying put changes nothing.
    s = streamer.Update(50.0, 50.0);
    AETHER_CHECK(s.cells_loaded == 0 && s.cells_unloaded == 0 && world.EntityCount() == 7);

    // Moving east loads (2,0); (-1,0) is still inside the unload radius.
    s = streamer.Update(170.0, 50.0);
    AETHER_CHECK(s.entities_created == 4 && s.entities_destroyed == 0);
    AETHER_CHECK(world.EntityCount() == 1 + 10);

    // Far east: (-1,0) and (0,0) pass the unload radius and their entities go.
    s = streamer.Update(360.0, 50.0);
    AETHER_CHECK(s.entities_destroyed == 3 + 1);
    AETHER_CHECK(streamer.EntitiesOf({0, 0}).empty() && streamer.EntitiesOf({-1, 0}).empty());
    AETHER_CHECK(world.EntityCount() == 1 + 6);

    // UnloadAll leaves only what the streamer didn't create.
    s = streamer.UnloadAll();
    AETHER_CHECK(s.entities_destroyed == 6 && world.EntityCount() == 1);
    AETHER_CHECK(streamer.Partition().LoadedCount() == 0);
}

AETHER_TEST(CellStreaming_CallbacksAndGameplayDestroyedEntities) {
    Dir dir("aether_test_cell_streaming_cb");
    WriteCell(dir.str(), {0, 0}, 2);

    World world;
    CellSceneStreamer streamer(world, Settings(), PathsIn(dir.str()));
    int loaded = 0, unloaded = 0;
    usize seen = 0;
    streamer.on_loaded = [&](CellCoord c, const std::vector<Entity>& e) {
        ++loaded;
        seen = e.size();
        AETHER_CHECK(c == CellCoord({0, 0}));
    };
    streamer.on_unloaded = [&](CellCoord) { ++unloaded; };
    streamer.Update(50.0, 50.0);
    AETHER_CHECK(loaded == 1 && seen == 2);

    // Gameplay destroys one of them first; unloading doesn't touch the dead handle.
    world.DestroyEntity(streamer.EntitiesOf({0, 0})[0]);
    const CellStreamStats s = streamer.Update(600.0, 600.0);
    AETHER_CHECK(unloaded >= 1 && s.entities_destroyed == 1 && world.EntityCount() == 0);
}

AETHER_TEST(CellStreaming_FailedLoadsAreNotRetriedUntilAsked) {
    Dir dir("aether_test_cell_streaming_bad");
    WriteCell(dir.str(), {1, 0}, 2);
    {
        std::ofstream broken(CellScenePath(dir.str(), {0, 0}), std::ios::binary);
        broken << "not a scene";
    }

    World world;
    int attempts = 0;
    CellSceneStreamer streamer(
        world, Settings(), PathsIn(dir.str()), [&](World& w, const std::string& path) {
            if (path.find("cell_0_0") != std::string::npos) ++attempts;
            return LoadScene(w, path);
        });

    CellStreamStats s = streamer.Update(50.0, 50.0);
    AETHER_CHECK(s.cells_failed == 1 && streamer.Failed().size() == 1 && streamer.Failed()[0] == CellCoord({0, 0}));
    AETHER_CHECK(world.EntityCount() == 2 && attempts == 1); // the good neighbor still loaded

    streamer.Update(50.0, 50.0);
    AETHER_CHECK(attempts == 1); // no retry storm

    // The file gets fixed; Retry makes the cell eligible again.
    WriteCell(dir.str(), {0, 0}, 5);
    streamer.Retry();
    AETHER_CHECK(streamer.Failed().empty());
    s = streamer.Update(50.0, 50.0);
    AETHER_CHECK(attempts == 2 && s.cells_failed == 0 && s.entities_created == 5 && world.EntityCount() == 7);
}
