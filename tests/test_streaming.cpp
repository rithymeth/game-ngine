#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"
#include "aether/streaming/streamer.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <random>
#include <set>

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

namespace {
// A manual-step executor: tasks queue until the test runs them, standing in for a worker thread.
struct ManualExecutor {
    std::vector<std::function<void()>> tasks;
    WorldStreamer::Executor Get() {
        return [this](std::function<void()> t) { tasks.push_back(std::move(t)); };
    }
    void RunAll() {
        auto run = std::move(tasks);
        tasks.clear();
        for (auto& t : run) t();
    }
};
} // namespace

AETHER_TEST(Streaming_AsyncReadsApplyOnUpdate) {
    Level level;
    PartitionSettings s;
    s.cell_size = 64.0f;
    const PartitionResult p = PartitionWorld(level.world, s);
    World game;
    CHECK(LoadSceneFromMemory(game, p.persistent));
    WorldStreamer streamer(game, p.index, [&](const CellCoord& c, std::vector<u8>& bytes) {
        const auto it = p.cells.find(c);
        if (it == p.cells.end()) return false;
        bytes = it->second;
        return true;
    });
    ManualExecutor exec;
    streamer.SetExecutor(exec.Get());
    streamer.Update();
    // The read is requested but nothing is in the world until it finishes and Update applies it.
    CHECK(streamer.State({0, 0}) == CellState::Unloaded && exec.tasks.size() == 1 && streamer.InFlight() == 1);
    CHECK(!HasEntityAt(game, 10.0f));
    exec.RunAll();
    streamer.Update();
    CHECK(streamer.State({0, 0}) == CellState::Loaded && HasEntityAt(game, 10.0f));
    CHECK(streamer.InFlight() == 0);
}

AETHER_TEST(Streaming_AsyncDropsCellsNoLongerWanted) {
    Level level;
    PartitionSettings s;
    s.cell_size = 64.0f;
    const PartitionResult p = PartitionWorld(level.world, s);
    World game;
    CHECK(LoadSceneFromMemory(game, p.persistent));
    WorldStreamer streamer(game, p.index, [&](const CellCoord& c, std::vector<u8>& bytes) {
        const auto it = p.cells.find(c);
        if (it == p.cells.end()) return false;
        bytes = it->second;
        return true;
    });
    ManualExecutor exec;
    streamer.SetExecutor(exec.Get());
    streamer.Update(); // requests cell 0
    Entity player = kNullEntity;
    game.ForEachArchetype([&](Archetype& a) {
        if (a.Mask().test(GetComponentId<StreamingSource>())) player = a.EntityArray(0)[0];
    });
    game.GetComponent<Transform>(player)->position = Vec3(1000, 0, 0); // far from every cell
    exec.RunAll();
    streamer.Update();
    CHECK(streamer.State({0, 0}) == CellState::Unloaded && streamer.LoadedCells().empty());
    CHECK(streamer.InFlight() == 0);
}

AETHER_TEST(Streaming_AsyncEntityBudgetSpreadsApplies) {
    Level level;
    PartitionSettings s;
    s.cell_size = 64.0f;
    const PartitionResult p = PartitionWorld(level.world, s);
    World game;
    CHECK(LoadSceneFromMemory(game, p.persistent));
    WorldStreamer streamer(game, p.index, [&](const CellCoord& c, std::vector<u8>& bytes) {
        const auto it = p.cells.find(c);
        if (it == p.cells.end()) return false;
        bytes = it->second;
        return true;
    });
    Entity player = kNullEntity;
    game.ForEachArchetype([&](Archetype& a) {
        if (a.Mask().test(GetComponentId<StreamingSource>())) player = a.EntityArray(0)[0];
    });
    game.GetComponent<Transform>(player)->position = Vec3(100, 0, 0);
    game.GetComponent<StreamingSource>(player)->radius = 400.0f; // every cell wanted
    ManualExecutor exec;
    streamer.SetExecutor(exec.Get());
    streamer.max_loads_per_update = 8;
    streamer.max_entities_per_update = 1; // one cell's worth per Update
    usize previous = 0;
    bool spread = true;
    for (int i = 0; i < 20; ++i) {
        exec.RunAll();
        streamer.Update();
        const usize now = streamer.LoadedCells().size();
        spread = spread && now - previous <= 1; // each cell holds one entity, so one cell per Update
        previous = now;
    }
    CHECK(spread);
    CHECK(streamer.LoadedCells().size() == p.index.cells.size());
}

// Phase 42.5: a cell hook fires on load and unload, so the navmesh, audio and
// gameplay data can follow the streamed geometry.
AETHER_TEST(Streaming_CellHookFollowsLoadsAndUnloads) {
    World src;
    RegisterStreamingComponents();
    src.CreateEntity(IdComponent{NewEntityGuid()}, At(32, 32));
    PartitionSettings s;
    s.cell_size = 64.0f;
    PartitionResult p = PartitionWorld(src, s);
    CHECK(p.cells.size() == 1);
    World game;
    const Entity player = game.CreateEntity(At(32, 32), StreamingSource{30.0f, true});
    WorldStreamer streamer(game, p.index, [&](const CellCoord& c, std::vector<u8>& bytes) {
        const auto it = p.cells.find(c);
        if (it == p.cells.end()) return false;
        bytes = it->second;
        return true;
    });
    std::vector<std::pair<CellCoord, bool>> calls;
    streamer.SetCellHook([&](const CellCoord& c, bool loaded) { calls.emplace_back(c, loaded); });

    streamer.Update();
    CHECK(streamer.State({0, 0}) == CellState::Loaded);
    CHECK(calls.size() == 1 && calls[0].second && calls[0].first.x == 0 && calls[0].first.z == 0);

    // Away from every cell: it unloads and the hook says so.
    game.GetComponent<Transform>(player)->position = Vec3(2000, 0, 0);
    streamer.Update();
    CHECK(streamer.State({0, 0}) == CellState::Unloaded);
    CHECK(calls.size() == 2 && !calls[1].second);
}

namespace {
// Deliberately brute-force reference: the pre-spatial-query selection rule.
// Compare requests, priorities, hysteresis and budgets across many frames,
// not just the final set of loaded cells.
struct ReferenceStreamer {
    const WorldIndex& index;
    std::set<CellCoord> loaded, failed, pinned, missing;
    std::vector<CellCoord> requests;
    std::vector<StreamingEvent> events;
    usize pending = 0;

    explicit ReferenceStreamer(const WorldIndex& world_index, std::set<CellCoord> missing_cells = {})
        : index(world_index), missing(std::move(missing_cells)) {}

    void Update(World& world, const std::vector<Entity>& players, const streaming::WorldPosition& offset,
                usize load_budget, usize unload_budget, f32 margin) {
        requests.clear();
        events.clear();
        const auto nearness = [&](const CellCoord& cell) {
            if (pinned.count(cell) != 0) return 0.0f;
            f32 best = 1e30f;
            for (Entity e : players) {
                const StreamingSource* source = world.GetComponent<StreamingSource>(e);
                const Transform* transform = world.GetComponent<Transform>(e);
                if (source == nullptr || transform == nullptr || !source->enabled) continue;
                const f64 x = offset.x + transform->position.x, z = offset.z + transform->position.z;
                const f64 size = index.settings.cell_size;
                const f64 x0 = index.settings.origin.x + cell.x * size, z0 = index.settings.origin.z + cell.z * size;
                const f64 dx = std::max({x0 - x, 0.0, x - (x0 + size)});
                const f64 dz = std::max({z0 - z, 0.0, z - (z0 + size)});
                const f32 d = static_cast<f32>(std::sqrt(dx * dx + dz * dz));
                best = std::min(best, source->radius > 0.0f ? d / source->radius : (d <= 0.0f ? 0.0f : 1e30f));
            }
            return best;
        };
        usize unloads = 0;
        for (auto it = loaded.begin(); it != loaded.end() && unloads < unload_budget;) {
            if (nearness(*it) > margin) {
                events.push_back({*it, false, 0});
                it = loaded.erase(it);
                ++unloads;
            } else ++it;
        }
        std::vector<std::pair<f32, CellCoord>> wanted;
        for (const CellInfo& info : index.cells) {
            if (loaded.count(info.coord) != 0 || failed.count(info.coord) != 0) continue;
            const f32 n = nearness(info.coord);
            if (n <= 1.0f) wanted.push_back({n, info.coord});
        }
        for (const CellCoord& cell : pinned)
            if (index.Find(cell) == nullptr && loaded.count(cell) == 0 && failed.count(cell) == 0)
                wanted.push_back({0.0f, cell});
        std::sort(wanted.begin(), wanted.end());
        pending = wanted.size() > load_budget ? wanted.size() - load_budget : 0;
        for (usize i = 0; i < std::min(load_budget, wanted.size()); ++i) {
            const CellCoord cell = wanted[i].second;
            requests.push_back(cell);
            if (missing.count(cell) != 0) failed.insert(cell);
            else {
                loaded.insert(cell);
                events.push_back({cell, true, 0});
            }
        }
    }
};

CellInfo QueryCell(const CellCoord& coord) {
    CellInfo info;
    info.coord = coord;
    return info;
}

WorldIndex QueryTestIndex() {
    WorldIndex index;
    index.settings.origin = Vec3(-32, 0, 16);
    for (i32 x = -40; x <= 40; ++x)
        for (i32 z = -40; z <= 40; ++z)
            if ((x * x + z * z) % 7 != 3) index.cells.push_back(QueryCell({x, z}));
    return index;
}
} // namespace

AETHER_TEST(Streaming_SpatialQueryMatchesBruteForceAcrossFrames) {
    RegisterStreamingComponents();
    const WorldIndex index = QueryTestIndex();
    World game;
    std::vector<Entity> players;
    for (int i = 0; i < 4; ++i) players.push_back(game.CreateEntity(At(0, 0), StreamingSource{80, true}));
    // A source without a Transform cannot select cells.
    players.push_back(game.CreateEntity(StreamingSource{100000, true}));
    World empty;
    const std::vector<u8> bytes = SaveSceneToMemory(empty);
    ReferenceStreamer reference(index, {{0, 0}, {-2, 1}});
    std::vector<CellCoord> requests;
    WorldStreamer streamer(game, index, [&](const CellCoord& cell, std::vector<u8>& out) {
        requests.push_back(cell);
        if (reference.missing.count(cell) != 0) return false;
        out = bytes;
        return true;
    });
    std::mt19937 random(1729);
    const f32 radii[] = {0, -1, 16, 64, 80, 200, 400, 65536,
        std::numeric_limits<f32>::infinity(), std::numeric_limits<f32>::quiet_NaN()};
    for (int frame = 0; frame < 100; ++frame) {
        const streaming::WorldPosition offset{frame % 3 == 0 ? 1024.0 : 0.0, 0, frame % 5 == 0 ? -2048.0 : 0.0};
        for (usize s = 0; s < 4; ++s) {
            const i32 x = static_cast<i32>(random() % 81) - 40;
            const i32 z = static_cast<i32>(random() % 81) - 40;
            // Exact grid lines, then cell centers, including negative coords;
            // two sources overlap to exercise minimum normalized priorities.
            Vec3 position(index.settings.origin.x + x * 64.0f, 0, index.settings.origin.z + z * 64.0f);
            if (frame % 2 != 0) position = position + Vec3(32, 0, 32);
            if (s == 1) position = game.GetComponent<Transform>(players[0])->position;
            game.GetComponent<Transform>(players[s])->position = position;
            StreamingSource* source = game.GetComponent<StreamingSource>(players[s]);
            source->radius = radii[(frame + s) % 10];
            source->enabled = frame % 13 != 0 && (s != 3 || frame % 3 == 0);
        }
        const CellCoord pin = frame % 2 == 0 ? CellCoord{30, -30} : CellCoord{100, -100};
        reference.pinned.clear();
        reference.pinned.insert(pin);
        streamer.Unpin({30, -30});
        streamer.Unpin({100, -100});
        streamer.Pin(pin);
        streamer.SetOriginOffset(offset);
        streamer.max_loads_per_update = static_cast<usize>(frame % 7);
        streamer.max_unloads_per_update = static_cast<usize>(frame % 5);
        streamer.unload_margin = frame % 2 == 0 ? 1.25f : 1.5f;
        reference.Update(game, players, offset, streamer.max_loads_per_update,
                         streamer.max_unloads_per_update, streamer.unload_margin);
        requests.clear();
        streamer.Update();
        CHECK(requests == reference.requests);
        CHECK(streamer.LoadedCells() == std::vector<CellCoord>(reference.loaded.begin(), reference.loaded.end()));
        CHECK(streamer.PendingLoads() == reference.pending);
        CHECK(streamer.Events().size() == reference.events.size());
        for (usize i = 0; i < std::min(streamer.Events().size(), reference.events.size()); ++i) {
            CHECK(streamer.Events()[i].cell == reference.events[i].cell);
            CHECK(streamer.Events()[i].loaded == reference.events[i].loaded);
        }
    }
}

AETHER_TEST(Streaming_SpatialQueryKeepsTouchingBoundariesAndExtremeCoordinates) {
    RegisterStreamingComponents();
    const auto run = [](WorldIndex index, streaming::WorldPosition offset, Vec3 position, f32 radius, bool expect_cells = true) {
        World game, empty;
        const Entity player = game.CreateEntity(Transform{position, Quaternion::Identity()}, StreamingSource{radius, true});
        const std::vector<u8> bytes = SaveSceneToMemory(empty);
        std::vector<CellCoord> requests;
        WorldStreamer streamer(game, index, [&](const CellCoord& cell, std::vector<u8>& out) {
            requests.push_back(cell);
            out = bytes;
            return true;
        });
        streamer.SetOriginOffset(offset);
        streamer.max_loads_per_update = index.cells.size();
        ReferenceStreamer reference(index);
        reference.Update(game, {player}, offset, streamer.max_loads_per_update, 4, 1.25f);
        streamer.Update();
        if (expect_cells) CHECK(!reference.requests.empty());
        CHECK(requests == reference.requests);
    };
    WorldIndex index = QueryTestIndex();
    run(index, {}, Vec3(-32, 0, 16), 0); // four cells touch the corner
    run(index, {}, Vec3(-32, 0, 16), -1); // legacy non-positive radius rule
    run(index, {}, Vec3(-32, 0, 16), 64); // cells touching the exact radius
    run(index, {}, Vec3(-32, 0, 16), std::nextafter(64.0f, 0.0f));
    run(index, {1024, 0, -2048}, Vec3(-1056, 0, 2064), 64);
    for (f32 size : {0.3f, 3.7f, 127.25f}) {
        index.settings.cell_size = size;
        index.settings.origin = Vec3(0.1f, 0, -0.2f);
        const f32 boundary = index.settings.origin.x + 3 * size;
        for (f32 x : {std::nextafter(boundary, -std::numeric_limits<f32>::infinity()),
                       boundary, std::nextafter(boundary, std::numeric_limits<f32>::infinity())}) {
            run(index, {}, Vec3(x, 0, index.settings.origin.z), size);
            run(index, {0.15, 0, -0.35}, Vec3(x, 0, index.settings.origin.z), size);
        }
    }
    // Non-finite coordinates and settings must keep the original predicate,
    // never enter coordinate conversion or silently pick different cells.
    const f32 inf = std::numeric_limits<f32>::infinity();
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    run(index, {}, Vec3(inf, 0, 0), 16, false);
    run(index, {}, Vec3(nan, 0, 0), 16, false);
    run(index, {inf, 0, 0}, Vec3(), 16, false);
    run(index, {nan, 0, 0}, Vec3(), 16, false);
    WorldIndex unusual;
    unusual.cells = {QueryCell({-1, -1}), QueryCell({0, 0}), QueryCell({1, 1})};
    for (f32 size : {0.0f, -1.0f, inf, nan}) {
        unusual.settings.cell_size = size;
        run(unusual, {}, Vec3(), 16, false);
    }

    index.settings.cell_size = 64.0f;
    index.settings.origin = Vec3();
    constexpr i32 lo = std::numeric_limits<i32>::min(), hi = std::numeric_limits<i32>::max();
    index.cells.insert(index.cells.begin(), {QueryCell({lo, lo}), QueryCell({lo, hi})});
    index.cells.push_back(QueryCell({hi, lo}));
    index.cells.push_back(QueryCell({hi, hi}));
    run(index, {static_cast<f64>(lo) * 64, 0, static_cast<f64>(lo) * 64}, Vec3(32, 0, 32), 0);
    run(index, {static_cast<f64>(hi) * 64, 0, static_cast<f64>(hi) * 64}, Vec3(32, 0, 32), 0);
    // Double precision cannot distinguish these cell bounds: take the legacy
    // predicate path rather than incorrectly culling coincident cells.
    index.settings.origin = Vec3(1e20f, 0, 1e20f);
    run(index, {index.settings.origin.x, 0, index.settings.origin.z}, Vec3(), 16);
}

AETHER_TEST(Streaming_SpatialQueryAsyncDropsOldWindowAfterTeleport) {
    RegisterStreamingComponents();
    const WorldIndex index = QueryTestIndex();
    World game, empty;
    const Entity player = game.CreateEntity(At(0, 0), StreamingSource{80, true});
    const std::vector<u8> bytes = SaveSceneToMemory(empty);
    WorldStreamer streamer(game, index, [&](const CellCoord&, std::vector<u8>& out) {
        out = bytes;
        return true;
    });
    ManualExecutor executor;
    streamer.SetExecutor(executor.Get());
    streamer.Update();
    CHECK(streamer.InFlight() > 0 && streamer.LoadedCells().empty());
    executor.RunAll(); // results for the old window are ready
    game.GetComponent<Transform>(player)->position = Vec3(1600, 0, -1600);
    streamer.Update(); // discard those results and request the new window
    CHECK(streamer.LoadedCells().empty());
    for (int i = 0; i < 10; ++i) {
        executor.RunAll();
        streamer.Update();
    }
    ReferenceStreamer reference(index);
    reference.Update(game, {player}, {}, index.cells.size(), 4, 1.25f);
    CHECK(streamer.LoadedCells() == std::vector<CellCoord>(reference.loaded.begin(), reference.loaded.end()));
    CHECK(streamer.InFlight() == 0 && streamer.PendingLoads() == 0);
}

AETHER_TEST(Streaming_SpatialQueryAsyncPreservesOverlapAndDiscardsDepartedFailure) {
    RegisterStreamingComponents();
    const WorldIndex index = QueryTestIndex();
    World game, empty;
    const Vec3 start(-32, 0, 16);
    const Entity player = game.CreateEntity(Transform{start, Quaternion::Identity()}, StreamingSource{80, true});
    const std::vector<u8> bytes = SaveSceneToMemory(empty);
    const CellCoord pin{-100, 100}, departed{-1, -1}, overlap{0, -1};
    bool fail_departed = true;
    std::map<CellCoord, usize> reads;
    WorldStreamer streamer(game, index, [&](const CellCoord& cell, std::vector<u8>& out) {
        ++reads[cell];
        if (cell == departed && fail_departed) return false;
        out = bytes;
        return true;
    });
    streamer.Pin(pin); // absent from the index, and earlier in priority order
    ManualExecutor executor;
    streamer.SetExecutor(executor.Get());
    streamer.Update();
    CHECK(streamer.InFlight() == 4);
    game.GetComponent<Transform>(player)->position = start + Vec3(128, 0, 0);
    streamer.Update(); // old tasks still in flight; no results yet
    CHECK(streamer.LoadedCells().empty() && streamer.InFlight() == 4);
    executor.RunAll(); // overlapping + pinned results survive; departed reads do not
    streamer.Update();
    CHECK(streamer.State(pin) == CellState::Loaded);
    CHECK(streamer.State(overlap) == CellState::Loaded);
    CHECK(streamer.State(departed) == CellState::Unloaded);
    CHECK(streamer.Problems().empty()); // discarded failure does not poison the cell
    for (int i = 0; i < 12; ++i) {
        executor.RunAll();
        streamer.Update();
    }
    CHECK(reads[pin] == 1 && reads[overlap] == 1 && reads[departed] == 1);
    ReferenceStreamer reference(index);
    reference.pinned.insert(pin);
    reference.Update(game, {player}, {}, index.cells.size() + 1, 4, 1.25f);
    CHECK(streamer.LoadedCells() == std::vector<CellCoord>(reference.loaded.begin(), reference.loaded.end()));

    fail_departed = false;
    game.GetComponent<Transform>(player)->position = start;
    for (int i = 0; i < 12; ++i) {
        executor.RunAll();
        streamer.Update();
    }
    CHECK(streamer.State(departed) == CellState::Loaded && reads[departed] == 2);
    CHECK(reads[pin] == 1 && reads[overlap] == 1);
    CHECK(streamer.InFlight() == 0 && streamer.PendingLoads() == 0);
}
