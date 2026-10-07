// Engine benchmarks (Phase 38 steps 2, 3 and 4): ECS iteration and structural changes, the job system, scene
// save and load, and texture cooking.
#include "aether/bench/bench.h"
#include "aether/cook/texture_cook.h"
#include "aether/ecs/world.h"
#include "aether/job/job_system.h"
#include "aether/scene/components.h"
#include "aether/scene/serialization.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

using namespace aether;

namespace {

struct Velocity {
    Vec3 value;
};
struct QueryMarker {};

struct EcsState {
    World world;
    std::vector<Entity> entities;
};
EcsState& Ecs() {
    static EcsState s;
    return s;
}

struct QueryState {
    World world;
    usize matches = 0;
};
QueryState& Query() {
    static QueryState s;
    return s;
}

void Spin(void* data) {
    auto* x = static_cast<u64*>(data);
    for (int i = 0; i < 200; ++i) *x = *x * 6364136223846793005ull + 1442695040888963407ull;
}

World& SceneWorld() {
    static World world;
    return world;
}

struct JobState {
    std::unique_ptr<JobSystem> jobs;
    std::vector<u64> data = std::vector<u64>(1024, 1);
    std::vector<JobDecl> decls;
};
JobState& Jobs() {
    static JobState s;
    return s;
}

} // namespace

// Moving 100k entities with a Transform and a Velocity: the everyday system loop.
AETHER_BENCH_SETUP(
    "ecs/iterate_100k", 20,
    [] {
        EcsState& s = Ecs();
        for (int i = 0; i < 100'000; ++i) s.entities.push_back(s.world.CreateEntity(Transform{Vec3(static_cast<f32>(i), 0, 0), Quaternion::Identity()}, Velocity{Vec3(1, 0, 0)}));
    },
    [] {
        Ecs().world.ForEach<Transform, Velocity>([](Transform& t, Velocity& v) { t.position = t.position + v.value * 0.016f; });
    });

// Query a component subset across two archetypes, matching half the mixed entities.
AETHER_BENCH_SETUP(
    "ecs/query_50k_of_100k", 10,
    [] {
        QueryState& s = Query();
        for (int i = 0; i < 100'000; ++i) {
            if ((i & 1) == 0) s.world.CreateEntity(Transform{}, QueryMarker{});
            else s.world.CreateEntity(Transform{});
        }
    },
    [] {
        QueryState& s = Query();
        s.matches = 0;
        s.world.ForEach<Transform, QueryMarker>([&](Transform&, QueryMarker&) { ++s.matches; });
        if (s.matches != 50'000) std::abort();
    });

// Creating and destroying 10k entities (structural changes, which move rows between chunks).
AETHER_BENCH("ecs/create_destroy_10k", 5, [] {
    World world;
    std::vector<Entity> made;
    made.reserve(10'000);
    for (int i = 0; i < 10'000; ++i) made.push_back(world.CreateEntity(Transform{}, Velocity{}));
    for (const Entity e : made) world.DestroyEntity(e);
});

// Adding and removing a component on 10k entities (moves between archetypes).
AETHER_BENCH("ecs/add_remove_component_10k", 5, [] {
    World world;
    std::vector<Entity> made;
    made.reserve(10'000);
    for (int i = 0; i < 10'000; ++i) made.push_back(world.CreateEntity(Transform{}));
    for (const Entity e : made) world.AddComponent<Velocity>(e, Velocity{});
    for (const Entity e : made) world.RemoveComponent<Velocity>(e);
});

// A batch of 1024 small jobs through the work-stealing scheduler.
AETHER_BENCH_SETUP(
    "jobs/batch_1024", 20,
    [] {
        JobState& s = Jobs();
        s.jobs = std::make_unique<JobSystem>();
        for (usize i = 0; i < s.data.size(); ++i) s.decls.push_back({&Spin, &s.data[i]});
    },
    [] {
        JobState& s = Jobs();
        JobCounter counter{0};
        s.jobs->ScheduleBatch(s.decls.data(), static_cast<u32>(s.decls.size()), counter);
        s.jobs->Wait(counter);
    });

// ---------------------------------------------------------------------------------------------------
// Scene save and load (Phase 38 step 4): 10k entities through the binary and the JSON formats.
// ---------------------------------------------------------------------------------------------------

namespace {

constexpr int kSceneEntities = 10'000;

struct SceneState {
    std::vector<u8> binary;
    std::vector<u8> json;
};
SceneState& SceneData() {
    static SceneState s;
    return s;
}

void FillSceneWorld(World& world) {
    for (int i = 0; i < kSceneEntities; ++i) {
        world.CreateEntity(Transform{Vec3(static_cast<f32>(i), static_cast<f32>(i % 97), 0), Quaternion::Identity()});
    }
}

} // namespace

AETHER_BENCH_SETUP(
    "scene/save_binary_10k", 5,
    [] { FillSceneWorld(SceneWorld()); },
    [] { SceneData().binary = SaveSceneToMemory(SceneWorld()); });

AETHER_BENCH_SETUP(
    "scene/load_binary_10k", 5,
    [] {
        World world;
        FillSceneWorld(world);
        SceneData().binary = SaveSceneToMemory(world);
    },
    [] {
        World world;
        LoadSceneFromMemory(world, SceneData().binary, "bench");
    });

AETHER_BENCH_SETUP(
    "scene/save_json_10k", 3,
    [] { FillSceneWorld(SceneWorld()); },
    [] {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "aether_bench_scene_save.json";
        SaveSceneJson(SceneWorld(), path.string());
        std::filesystem::remove(path);
    });

AETHER_BENCH_SETUP(
    "scene/load_json_10k", 3,
    [] {
        World world;
        FillSceneWorld(world);
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "aether_bench_scene_load.json";
        SaveSceneJson(world, path.string());
        std::ifstream in(path, std::ios::binary);
        SceneData().json.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        in.close();
        std::filesystem::remove(path);
    },
    [] {
        World world;
        LoadSceneJsonFromMemory(world, SceneData().json, "bench");
    });

// ---------------------------------------------------------------------------------------------------
// Texture cooking (Phase 38 step 4): block compression and the .atex container, on a synthetic image.
// ---------------------------------------------------------------------------------------------------

namespace {

// A smooth gradient with some noise, so the encoders have real work to do.
std::vector<u8> SyntheticImage(u32 size) {
    std::vector<u8> rgba(static_cast<usize>(size) * size * 4);
    u32 seed = 12345;
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            seed = seed * 1664525u + 1013904223u;
            u8* p = &rgba[(static_cast<usize>(y) * size + x) * 4];
            p[0] = static_cast<u8>(x * 255 / size);
            p[1] = static_cast<u8>(y * 255 / size);
            p[2] = static_cast<u8>((seed >> 24) & 0xFF);
            p[3] = 255;
        }
    }
    return rgba;
}

struct CookState {
    std::vector<u8> image;
    std::vector<u8> atex;
};
CookState& Cook() {
    static CookState s;
    return s;
}

} // namespace

AETHER_BENCH_SETUP(
    "cook/texture_bc7_256", 3,
    [] { Cook().image = SyntheticImage(256); },
    [] {
        cook::TextureCookSettings settings;
        settings.auto_format = false;
        settings.format = cook::TextureFormat::BC7;
        settings.quality = 2;
        cook::CookTexture(Cook().image, 256, 256, settings);
    });

AETHER_BENCH_SETUP(
    "cook/texture_bc1_512_mips", 3,
    [] { Cook().image = SyntheticImage(512); },
    [] {
        cook::TextureCookSettings settings;
        settings.auto_format = false;
        settings.format = cook::TextureFormat::BC1;
        settings.quality = 2;
        cook::CookTexture(Cook().image, 512, 512, settings);
    });

// Writing a cooked texture to its container and reading it back.
AETHER_BENCH_SETUP(
    "cook/atex_roundtrip_256", 10,
    [] {
        Cook().image = SyntheticImage(256);
        cook::TextureCookSettings settings;
        settings.auto_format = false;
        settings.format = cook::TextureFormat::BC1;
        Cook().atex = cook::SaveAtex(cook::CookTexture(Cook().image, 256, 256, settings));
        cook::CookedTexture check;
        if (!cook::LoadAtex(Cook().atex, check) || check.mips.empty()) { // can't quietly time an empty round trip
            std::fprintf(stderr, "cook/atex_roundtrip_256: the container did not load\n");
            std::abort();
        }
    },
    [] {
        cook::CookedTexture texture;
        if (!cook::LoadAtex(Cook().atex, texture)) std::abort();
        cook::SaveAtex(texture);
    });
