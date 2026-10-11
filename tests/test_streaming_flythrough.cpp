#include "aether/core/log.h"
#include "aether/scene/components.h"
#include "aether/scene/test_world.h"
#include "aether/streaming/streamer.h"
#include "test_framework.h"

#include <algorithm>
#include <chrono>
#include <vector>

// Phase 42 step 7: an automated flythrough of the generated test world through the async streamer, with a hitch
// budget. The budgets that decide the verdict are deterministic (entities applied in one frame, entities alive at
// once); the frame time is measured too, against a bound loose enough for a loaded CI machine.

using namespace aether;
using namespace aether::streaming;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

AETHER_TEST(Streaming_FlythroughStaysInsideTheHitchBudget) {
    World source;
    scene::TestWorldParams params;
    params.blocks_x = 48;
    params.blocks_z = 48;
    scene::TestWorldStats stats;
    CHECK(scene::GenerateTestWorld(source, params, &stats));
    // Cells are saved by GUID, so give every generated entity one.
    std::vector<Entity> entities;
    source.ForEachArchetype([&](Archetype& a) {
        for (usize c = 0; c < a.ChunkCount(); ++c) {
            Entity* e = a.EntityArray(c);
            entities.insert(entities.end(), e, e + a.ChunkEntityCount(c));
        }
    });
    for (Entity e : entities)
        if (!source.HasComponent<IdComponent>(e)) source.AddComponent(e, IdComponent{NewEntityGuid()});

    PartitionSettings ps;
    ps.cell_size = 64.0f;
    const PartitionResult p = PartitionWorld(source, ps);
    CHECK(p.index.cells.size() > 4);

    World game;
    const Entity camera = game.CreateEntity(Transform{}, StreamingSource{90.0f, true});
    WorldStreamer streamer(game, p.index, [&](const CellCoord& c, std::vector<u8>& bytes) {
        const auto it = p.cells.find(c);
        if (it == p.cells.end()) return false;
        bytes = it->second;
        return true;
    });
    struct Manual {
        std::vector<std::function<void()>> tasks;
    } manual;
    streamer.SetExecutor([&](std::function<void()> t) { manual.tasks.push_back(std::move(t)); });
    constexpr usize kEntityBudget = 400;
    streamer.max_entities_per_update = kEntityBudget;
    streamer.max_loads_per_update = 4;

    usize biggest_cell = 0;
    for (const CellInfo& info : p.index.cells) biggest_cell = std::max<usize>(biggest_cell, info.entity_count);

    usize peak_alive = 0, worst_applied = 0;
    f64 worst_ms = 0.0;
    for (int frame = 0; frame < 240; ++frame) {
        const f32 t = static_cast<f32>(frame) / 239.0f;
        game.GetComponent<Transform>(camera)->position = Vec3(t * stats.extent_x, 5.0f, t * stats.extent_z);
        for (auto& task : std::exchange(manual.tasks, {})) task();
        const usize before = game.EntityCount();
        const auto start = std::chrono::steady_clock::now();
        streamer.Update();
        const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
        worst_ms = std::max(worst_ms, ms);
        usize applied = 0;
        for (const StreamingEvent& ev : streamer.Events())
            if (ev.loaded) applied += ev.entities;
        worst_applied = std::max(worst_applied, applied);
        peak_alive = std::max(peak_alive, game.EntityCount());
        (void)before;
    }
    // The budget is soft by one cell (the check runs before each cell), so a frame never exceeds it by more.
    CHECK(worst_applied <= kEntityBudget + biggest_cell);
    // Memory: the radius bounds how much is alive, far below the whole world.
    CHECK(peak_alive < stats.entities / 2);
    CHECK(!streamer.LoadedCells().empty());
    CHECK(worst_ms < 250.0); // a hitch this long would be felt on any machine
    AETHER_LOG_INFO("Test", "flythrough: worst Update %.2f ms, worst entities applied %zu, peak alive %zu of %zu", worst_ms,
                    worst_applied, peak_alive, stats.entities);
}
