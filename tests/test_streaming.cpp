#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"
#include "aether/streaming/streamer.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

// Phase 21 step 6: large worlds - partitioning a scene into grid cells (and
// a persistent scene), the index and files, streaming cells in and out
// around sources with hysteresis, budgets and pins, and the floating origin.

using namespace aether;
using namespace aether::streaming;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

Transform At(f32 x, f32 z) { return Transform{Vec3(x, 0, z), Quaternion::Identity()}; }

// Entities: a crate at x = 10, 70 and 200 along the X axis (cells 0, 1, 3 at 64 m),
// a lamp parented to the far crate, a level-rules entity with no Transform,
// an AlwaysLoaded tower out at 500, and the player (a streaming source).
struct Level {
    World world;
    GuidIndex guids;
    Entity crate0, crate1, crate3, lamp, rules, tower, player;
    Level() {
        RegisterStreamingComponents();
        crate0 = world.CreateEntity(IdComponent{NewEntityGuid()}, At(10, 5));
        crate1 = world.CreateEntity(IdComponent{NewEntityGuid()}, At(70, 5));
        crate3 = world.CreateEntity(IdComponent{NewEntityGuid()}, At(200, 5));
        lamp = world.CreateEntity(IdComponent{NewEntityGuid()}, At(1, 1), Parent{world.GetComponent<IdComponent>(crate3)->guid});
        rules = world.CreateEntity(IdComponent{NewEntityGuid()});
        tower = world.CreateEntity(IdComponent{NewEntityGuid()}, At(500, 5), AlwaysLoaded{});
        player = world.CreateEntity(IdComponent{NewEntityGuid()}, At(0, 0), StreamingSource{60.0f, true});
        guids.Rebuild(world);
    }
};

usize Count(const World& world) { return world.EntityCount(); }

bool HasEntityAt(World& world, f32 x) {
    bool found = false;
    world.ForEachArchetype([&](Archetype& a) {
        if (!a.Mask().test(GetComponentId<Transform>())) return;
        for (usize c = 0; c < a.ChunkCount(); ++c)
            for (usize i = 0; i < a.ChunkEntityCount(c); ++i)
                if (std::fabs(world.GetComponent<Transform>(a.EntityArray(c)[i])->position.x - x) < 1e-3f) found = true;
    });
    return found;
}

} // namespace

AETHER_TEST(Streaming_Cells) {
    PartitionSettings s;
    s.cell_size = 64.0f;
    CHECK(CellOf(Vec3(10, 0, 5), s) == CellCoord{0, 0});
    CHECK(CellOf(Vec3(64, 0, 5), s) == CellCoord{1, 0});
    CHECK(CellOf(Vec3(-0.5f, 0, -70), s) == CellCoord{-1, -2});
    s.origin = Vec3(-32, 0, -32);
    CHECK(CellOf(Vec3(0, 0, 0), s) == CellCoord{0, 0});
    Vec3 lo, hi;
    CellBounds({2, -1}, s, lo, hi);
    CHECK(lo.x == 96.0f && hi.x == 160.0f && lo.z == -96.0f && hi.z == -32.0f);
    CHECK(CellName({-1, 2}) == "-1_2");
    CHECK((CellCoord{0, 1} < CellCoord{1, 0}));
}

AETHER_TEST(Streaming_Partition) {
    Level level;
    PartitionSettings s;
    s.cell_size = 64.0f;
    const PartitionResult p = PartitionWorld(level.world, s);
    CHECK(p.cells.size() == 3);
    CHECK(p.index.cells.size() == 3);
    CHECK(p.index.cells[0].coord == CellCoord{0, 0} && p.index.cells[2].coord == CellCoord{3, 0});
    const CellInfo* far = p.index.Find({3, 0});
    CHECK(far != nullptr && far->entity_count == 2); // the crate and its lamp
    CHECK(far != nullptr && far->min.x == 200.0f);
    CHECK(p.index.Find({2, 0}) == nullptr);
    CHECK(p.index.persistent_count == 3); // the rules, the tower and the player
    // A cell's scene loads back with its entities and their components.
    World cell;
    CHECK(LoadSceneFromMemory(cell, p.cells.at({3, 0})));
    CHECK(Count(cell) == 2);
    GuidIndex g;
    g.Rebuild(cell);
    bool lamp_found = false;
    cell.ForEachArchetype([&](Archetype& a) {
        if (!a.Mask().test(GetComponentId<Parent>())) return;
        Entity e = a.EntityArray(0)[0];
        lamp_found = !GetParent(cell, g, e).IsNull(); // its parent came with it
    });
    CHECK(lamp_found);
    World persistent;
    CHECK(LoadSceneFromMemory(persistent, p.persistent));
    CHECK(Count(persistent) == 3);
    CHECK(HasEntityAt(persistent, 500.0f));
    // The index round-trips; bad ones are refused.
    WorldIndex back;
    std::string error;
    CHECK(LoadWorldIndex(SaveWorldIndex(p.index), back, &error));
    CHECK(back.cells.size() == 3 && back.persistent_count == 3 && back.settings.cell_size == 64.0f && back.Find({1, 0})->entity_count == 1);
    nlohmann::json bad = SaveWorldIndex(p.index);
    bad["cell_size"] = 0;
    CHECK(!LoadWorldIndex(bad, back, &error));
    CHECK(!LoadWorldIndex(nlohmann::json::object(), back, &error));
    // Copying an entity copies its components.
    World other;
    const Entity copy = CopyEntity(level.world, level.crate1, other);
    CHECK(other.GetComponent<Transform>(copy)->position.x == 70.0f);
    CHECK(other.GetComponent<IdComponent>(copy)->guid == level.world.GetComponent<IdComponent>(level.crate1)->guid);
}

AETHER_TEST(Streaming_StreamsAroundSources) {
    Level level;
    PartitionSettings s;
    s.cell_size = 64.0f;
    const PartitionResult p = PartitionWorld(level.world, s);
    // The running game: the persistent scene, then cells stream in.
    World game;
    CHECK(LoadSceneFromMemory(game, p.persistent));
    GuidIndex guids;
    guids.Rebuild(game);
    WorldStreamer streamer(game, p.index, [&](const CellCoord& c, std::vector<u8>& bytes) {
        const auto it = p.cells.find(c);
        if (it == p.cells.end()) return false;
        bytes = it->second;
        return true;
    }, &guids);
    streamer.Update();
    // The player at the origin with a 60 m radius: cell 0 (0..64) loads; cell 1 (64..128) is 64 m away, so it doesn't.
    CHECK(streamer.State({0, 0}) == CellState::Loaded);
    CHECK(streamer.State({1, 0}) == CellState::Unloaded);
    CHECK(streamer.EntitiesIn({0, 0}).size() == 1);
    CHECK(HasEntityAt(game, 10.0f));
    CHECK(streamer.Events().size() == 1 && streamer.Events()[0].loaded);
    // Walk to x = 150: cells 1 (22 m away) and 3 (42 m away) come in; cell 0 is 86 m away, beyond 60 x 1.25 = 75, so it goes.
    Entity player = kNullEntity;
    game.ForEachArchetype([&](Archetype& a) {
        if (a.Mask().test(GetComponentId<StreamingSource>())) player = a.EntityArray(0)[0];
    });
    game.GetComponent<Transform>(player)->position = Vec3(150, 0, 0);
    streamer.Update();
    CHECK(streamer.State({1, 0}) == CellState::Loaded && streamer.State({3, 0}) == CellState::Loaded);
    CHECK(streamer.State({0, 0}) == CellState::Unloaded); // 86 m out: beyond 75
    CHECK(!HasEntityAt(game, 10.0f));
    // The lamp found its parent through the GUID index.
    for (Entity e : streamer.EntitiesIn({3, 0}))
        if (game.GetComponent<Parent>(e) != nullptr) CHECK(!GetParent(game, guids, e).IsNull());
    // Hysteresis: between 60 and 75 m a cell isn't loaded, but a loaded one isn't unloaded either.
    game.GetComponent<Transform>(player)->position = Vec3(130, 0, 0); // 66 m from cell 0
    streamer.Update();
    CHECK(streamer.State({0, 0}) == CellState::Unloaded);
    game.GetComponent<Transform>(player)->position = Vec3(110, 0, 0); // 46 m: loads
    streamer.Update();
    CHECK(streamer.State({0, 0}) == CellState::Loaded);
    game.GetComponent<Transform>(player)->position = Vec3(130, 0, 0); // 66 m: kept (within 75)
    streamer.Update();
    CHECK(streamer.State({0, 0}) == CellState::Loaded);
    // Entities gameplay destroyed meanwhile are fine; spawned ones aren't touched.
    game.DestroyEntity(streamer.EntitiesIn({0, 0})[0]);
    const Entity spawned = game.CreateEntity(At(20, 5));
    game.GetComponent<Transform>(player)->position = Vec3(400, 0, 0);
    streamer.Update();
    CHECK(streamer.LoadedCells().empty());
    CHECK(game.IsAlive(spawned));
    CHECK(Count(game) == 3 + 1); // the persistent three and the spawned one
}

AETHER_TEST(Streaming_BudgetsPinsAndFailures) {
    // A 4 x 4 field of cells, one crate each.
    World src;
    RegisterStreamingComponents();
    for (int z = 0; z < 4; ++z)
        for (int x = 0; x < 4; ++x) src.CreateEntity(IdComponent{NewEntityGuid()}, At(static_cast<f32>(x) * 64 + 32, static_cast<f32>(z) * 64 + 32));
    PartitionSettings s;
    s.cell_size = 64.0f;
    PartitionResult p = PartitionWorld(src, s);
    CHECK(p.cells.size() == 16);
    p.cells.erase({3, 3}); // its file is missing
    World game;
    game.CreateEntity(At(128, 128), StreamingSource{1000.0f, true});
    std::vector<CellCoord> order;
    WorldStreamer streamer(game, p.index, [&](const CellCoord& c, std::vector<u8>& bytes) {
        order.push_back(c);
        const auto it = p.cells.find(c);
        if (it == p.cells.end()) return false;
        bytes = it->second;
        return true;
    });
    streamer.max_loads_per_update = 3;
    streamer.Update();
    CHECK(streamer.LoadedCells().size() == 3);
    CHECK(streamer.PendingLoads() == 13);
    // Nearest first: the four around (128, 128) are all 0 m away.
    for (usize i = 0; i < 3; ++i) CHECK(order[i].x >= 1 && order[i].x <= 2 && order[i].z >= 1 && order[i].z <= 2);
    for (int i = 0; i < 10; ++i) streamer.Update();
    CHECK(streamer.LoadedCells().size() == 15);
    CHECK(streamer.State({3, 3}) == CellState::Failed);
    CHECK(streamer.Problems().size() == 1);
    CHECK(streamer.PendingLoads() == 0); // the failed cell isn't retried
    // A pinned cell stays when the source goes away; the rest unload (4 per update).
    streamer.Pin({0, 0});
    Entity src_e = kNullEntity;
    game.ForEachArchetype([&](Archetype& a) {
        if (a.Mask().test(GetComponentId<StreamingSource>())) src_e = a.EntityArray(0)[0];
    });
    game.GetComponent<StreamingSource>(src_e)->enabled = false;
    streamer.Update();
    CHECK(streamer.LoadedCells().size() == 11);
    for (int i = 0; i < 5; ++i) streamer.Update();
    CHECK(streamer.LoadedCells() == std::vector<CellCoord>{CellCoord{0, 0}});
    streamer.Unpin({0, 0});
    streamer.Update();
    CHECK(streamer.LoadedCells().empty());
}

AETHER_TEST(Streaming_FilesAndFloatingOrigin) {
    Level level;
    PartitionSettings s;
    s.cell_size = 64.0f;
    const auto dir = std::filesystem::temp_directory_path() / "aether_test_streaming";
    std::filesystem::remove_all(dir);
    std::string error;
    CHECK(SavePartition(PartitionWorld(level.world, s), dir, &error));
    CHECK(std::filesystem::exists(dir / "world.aworld") && std::filesystem::exists(dir / "persistent.aesc"));
    CHECK(std::filesystem::exists(CellFile(dir, {3, 0})));
    WorldIndex index;
    CHECK(LoadWorldIndexFile(dir, index, &error));
    CHECK(index.cells.size() == 3);
    // Saving again replaces stale cell files.
    std::filesystem::copy_file(CellFile(dir, {0, 0}), CellFile(dir, {9, 9}));
    CHECK(SavePartition(PartitionWorld(level.world, s), dir, &error));
    CHECK(!std::filesystem::exists(CellFile(dir, {9, 9})));

    World game;
    CHECK(LoadScene(game, (dir / "persistent.aesc").string()));
    WorldStreamer streamer(game, index, WorldStreamer::FileLoader(dir));
    Entity player = kNullEntity;
    game.ForEachArchetype([&](Archetype& a) {
        if (a.Mask().test(GetComponentId<StreamingSource>())) player = a.EntityArray(0)[0];
    });
    // Walk far out: the floating origin moves everything back near zero.
    FloatingOrigin origin;
    origin.threshold = 100.0f;
    origin.step = 64.0f;
    game.GetComponent<Transform>(player)->position = Vec3(190, 0, 0);
    CHECK(origin.Update(game, game.GetComponent<Transform>(player)->position));
    CHECK(origin.last_shift.x == 192.0f && origin.offset.x == 192.0);
    CHECK(std::fabs(game.GetComponent<Transform>(player)->position.x + 2.0f) < 1e-4f);
    CHECK(HasEntityAt(game, 500.0f - 192.0f)); // the tower moved too
    streamer.SetOriginOffset(origin.offset);
    streamer.Update();
    // The player is at 190 in the whole world: cell 3 (192..256) is 2 m away and loads, its entities shifted to local positions.
    CHECK(streamer.State({3, 0}) == CellState::Loaded);
    CHECK(HasEntityAt(game, 200.0f - 192.0f));
    CHECK(!origin.Update(game, game.GetComponent<Transform>(player)->position)); // near the origin now
    const streaming::WorldPosition wp = origin.ToWorld(Vec3(8, 0, 0));
    CHECK(wp.x == 200.0);
    CHECK(origin.ToLocal(wp).x == 8.0f);
    std::filesystem::remove_all(dir);
}
