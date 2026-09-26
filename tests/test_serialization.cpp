#include "aether/scene/serialization.h"
#include "test_framework.h"

#include <filesystem>
#include <string>

using namespace aether;

namespace {

struct Position { f32 x = 0, y = 0, z = 0; };
struct Health { i32 value = 100; };

} // namespace

AETHER_TEST(Serialization_RoundTripsComponentsAcrossWorlds) {
    std::string path = (std::filesystem::temp_directory_path() / "aether_test_scene.aesc").string();

    World saved_world;
    saved_world.CreateEntity(Position{1, 2, 3}, Health{50});
    saved_world.CreateEntity(Position{4, 5, 6});
    saved_world.CreateEntity(Health{7});

    AETHER_CHECK(SaveScene(saved_world, path));

    World loaded_world;
    AETHER_CHECK(LoadScene(loaded_world, path));
    AETHER_CHECK(loaded_world.EntityCount() == 3);

    int with_both = 0;
    loaded_world.ForEach<Position, Health>([&](Position& p, Health& h) {
        ++with_both;
        AETHER_CHECK_NEAR(p.x, 1.0f, 1e-6);
        AETHER_CHECK_NEAR(p.y, 2.0f, 1e-6);
        AETHER_CHECK_NEAR(p.z, 3.0f, 1e-6);
        AETHER_CHECK(h.value == 50);
    });
    AETHER_CHECK(with_both == 1);

    int position_count = 0;
    loaded_world.ForEach<Position>([&](Position&) { ++position_count; });
    AETHER_CHECK(position_count == 2); // the {Position,Health} entity + the Position-only entity

    int health_count = 0;
    i32 lone_health_value = 0;
    loaded_world.ForEach<Health>([&](Health& h) {
        ++health_count;
        if (h.value != 50) {
            lone_health_value = h.value;
        }
    });
    AETHER_CHECK(health_count == 2); // the {Position,Health} entity + the Health-only entity
    AETHER_CHECK(lone_health_value == 7);

    std::filesystem::remove(path);
}

AETHER_TEST(Serialization_RejectsMissingFile) {
    World world;
    AETHER_CHECK(!LoadScene(world, "this_scene_file_should_not_exist_98765.aesc"));
}

// ---------------------------------------------------------------------------
// Phase 6: reflected components, format v2, legacy v1 files, JSON scenes
// ---------------------------------------------------------------------------

#include "aether/platform/filesystem.h"
#include "aether/reflection/reflection.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <typeinfo>
#include <utility>

namespace scene_test {

struct Stamina {
    f32 current = 50.0f;
    f32 regen = 1.5f;
    std::string label = "stamina";
};

// Plain data that was saved raw by v1 files and is reflected now.
struct LegacyPos {
    f32 x = 0, y = 0, z = 0;
};

// Never reflected.
struct RawTag {
    u32 bits = 0;
};

template <int N>
struct Many {
    i32 value = N;
};

} // namespace scene_test

AETHER_REFLECT(scene_test::Stamina, 1, AETHER_FIELD(current), AETHER_FIELD(regen), AETHER_FIELD(label))
AETHER_REFLECT(scene_test::LegacyPos, 1, AETHER_FIELD(x), AETHER_FIELD(y), AETHER_FIELD(z))

namespace {

std::string TempScenePath(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

std::vector<u8> ReadAll(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(file), {});
}

void PutU32(std::vector<u8>& out, u32 v) {
    const u8* b = reinterpret_cast<const u8*>(&v);
    out.insert(out.end(), b, b + 4);
}

void PutU64(std::vector<u8>& out, u64 v) {
    const u8* b = reinterpret_cast<const u8*>(&v);
    out.insert(out.end(), b, b + 8);
}

void PutRecordV1(std::vector<u8>& out, const std::string& name, const void* data, usize size) {
    PutU32(out, static_cast<u32>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
    PutU32(out, static_cast<u32>(size));
    const u8* b = static_cast<const u8*>(data);
    out.insert(out.end(), b, b + size);
}

template <int... N>
void CreateManyTypes(World& world, std::integer_sequence<int, N...>) {
    (GetComponentId<scene_test::Many<N>>(), ...);
    Entity e = world.CreateEntity(scene_test::Many<79>{});
    world.AddComponent(e, scene_test::Many<70>{});
    (void)e;
}

} // namespace

AETHER_TEST(Serialization_ReflectedComponentsUseDeclaredNames) {
    const ComponentInfo& info = GetComponentInfo(GetComponentId<scene_test::Stamina>());
    AETHER_CHECK(std::string(info.name) == "Stamina");
    AETHER_CHECK(info.encoding == ComponentEncoding::Reflected);
    AETHER_CHECK(info.reflected == &reflect::Reflect<scene_test::Stamina>());
    // Legacy alias: files written before Stamina was reflected used its typeid name.
    AETHER_CHECK(FindComponentIdByName(typeid(scene_test::Stamina).name()) == GetComponentId<scene_test::Stamina>());

    const ComponentInfo& raw = GetComponentInfo(GetComponentId<scene_test::RawTag>());
    AETHER_CHECK(raw.encoding == ComponentEncoding::Raw && raw.reflected == nullptr);

    std::string path = TempScenePath("aether_test_reflected.aesc");
    World world;
    scene_test::Stamina stamina;
    stamina.current = 12.5f;
    stamina.label = "long enough label to live on the heap, not in the SSO buffer";
    world.CreateEntity(stamina, scene_test::RawTag{0xABCDu});
    AETHER_CHECK(SaveScene(world, path));

    std::vector<u8> bytes = ReadAll(path);
    std::string as_text(bytes.begin(), bytes.end());
    AETHER_CHECK(as_text.find("Stamina") != std::string::npos);
    AETHER_CHECK(as_text.find(typeid(scene_test::Stamina).name()) == std::string::npos);

    World loaded;
    AETHER_CHECK(LoadScene(loaded, path));
    int count = 0;
    loaded.ForEach<scene_test::Stamina, scene_test::RawTag>([&](scene_test::Stamina& s, scene_test::RawTag& t) {
        ++count;
        AETHER_CHECK(s.current == 12.5f && s.regen == 1.5f);
        AETHER_CHECK(s.label == "long enough label to live on the heap, not in the SSO buffer");
        AETHER_CHECK(t.bits == 0xABCDu);
    });
    AETHER_CHECK(count == 1);
    std::filesystem::remove(path);
}

AETHER_TEST(Serialization_LegacyV1SceneStillLoads) {
    // Byte-for-byte the layout the v1 writer produced: magic, version 1,
    // entity count, then per entity a u64 mask and one {name, size, bytes}
    // record per set bit, with components named by typeid and stored raw.
    GetComponentId<scene_test::LegacyPos>();
    GetComponentId<scene_test::RawTag>();

    std::vector<u8> file = {'A', 'E', 'S', 'C'};
    PutU32(file, 1);
    PutU32(file, 2);

    scene_test::LegacyPos pos{1.0f, 2.0f, 3.0f};
    scene_test::RawTag tag{77};
    PutU64(file, 0b101); // two set bits -> two records (bit positions don't matter to the loader)
    PutRecordV1(file, typeid(scene_test::LegacyPos).name(), &pos, sizeof(pos));
    PutRecordV1(file, typeid(scene_test::RawTag).name(), &tag, sizeof(tag));

    scene_test::LegacyPos pos2{-4.0f, 0.5f, 9.0f};
    PutU64(file, 0b1);
    PutRecordV1(file, typeid(scene_test::LegacyPos).name(), &pos2, sizeof(pos2));

    std::string path = TempScenePath("aether_test_legacy_v1.aesc");
    AETHER_CHECK(fs::WriteFileBytes(path, file.data(), file.size()));

    World world;
    AETHER_CHECK(LoadScene(world, path));
    AETHER_CHECK(world.EntityCount() == 2);

    int with_tag = 0;
    world.ForEach<scene_test::LegacyPos, scene_test::RawTag>([&](scene_test::LegacyPos& p, scene_test::RawTag& t) {
        ++with_tag;
        AETHER_CHECK(p.x == 1.0f && p.y == 2.0f && p.z == 3.0f);
        AETHER_CHECK(t.bits == 77);
    });
    AETHER_CHECK(with_tag == 1);
    int positions = 0;
    bool saw_second = false;
    world.ForEach<scene_test::LegacyPos>([&](scene_test::LegacyPos& p) {
        ++positions;
        saw_second = saw_second || (p.x == -4.0f && p.z == 9.0f);
    });
    AETHER_CHECK(positions == 2 && saw_second);

    // Re-saving writes v2 with the reflected name, and that loads too.
    std::string resaved = TempScenePath("aether_test_legacy_resaved.aesc");
    AETHER_CHECK(SaveScene(world, resaved));
    std::vector<u8> bytes = ReadAll(resaved);
    u32 version = 0;
    std::memcpy(&version, bytes.data() + 4, 4);
    AETHER_CHECK(version == 2);
    World again;
    AETHER_CHECK(LoadScene(again, resaved));
    AETHER_CHECK(again.EntityCount() == 2);

    std::filesystem::remove(path);
    std::filesystem::remove(resaved);
}

AETHER_TEST(Serialization_SupportsMoreThan64ComponentTypes) {
    World world;
    CreateManyTypes(world, std::make_integer_sequence<int, 80>{});
    AETHER_CHECK(GetComponentId<scene_test::Many<79>>() >= 64);

    std::string path = TempScenePath("aether_test_many_types.aesc");
    AETHER_CHECK(SaveScene(world, path));
    World loaded;
    AETHER_CHECK(LoadScene(loaded, path));
    int count = 0;
    loaded.ForEach<scene_test::Many<79>, scene_test::Many<70>>([&](scene_test::Many<79>& a, scene_test::Many<70>& b) {
        ++count;
        AETHER_CHECK(a.value == 79 && b.value == 70);
    });
    AETHER_CHECK(count == 1);
    std::filesystem::remove(path);
}

AETHER_TEST(Serialization_SavingIsDeterministic) {
    World world;
    for (int i = 0; i < 20; ++i) {
        scene_test::Stamina s;
        s.current = static_cast<f32>(i);
        if (i % 3 == 0) {
            world.CreateEntity(s);
        } else if (i % 3 == 1) {
            world.CreateEntity(s, scene_test::RawTag{static_cast<u32>(i)});
        } else {
            world.CreateEntity(scene_test::RawTag{static_cast<u32>(i)});
        }
    }
    std::string a = TempScenePath("aether_test_det_a.aesc");
    std::string b = TempScenePath("aether_test_det_b.aesc");
    AETHER_CHECK(SaveScene(world, a));
    AETHER_CHECK(SaveScene(world, b));
    AETHER_CHECK(ReadAll(a) == ReadAll(b));

    // Loading then saving again reproduces the same bytes.
    World loaded;
    AETHER_CHECK(LoadScene(loaded, a));
    std::string c = TempScenePath("aether_test_det_c.aesc");
    AETHER_CHECK(SaveScene(loaded, c));
    AETHER_CHECK(ReadAll(a) == ReadAll(c));

    std::filesystem::remove(a);
    std::filesystem::remove(b);
    std::filesystem::remove(c);
}

AETHER_TEST(SceneJson_RoundTripsReflectedComponents) {
    World world;
    scene_test::Stamina s;
    s.current = 0.1f;
    s.label = "hero";
    world.CreateEntity(s, scene_test::LegacyPos{1, 2, 3});
    world.CreateEntity(scene_test::RawTag{5}); // unreflected only: left out of JSON with a warning
    world.CreateEntity(scene_test::LegacyPos{7, 8, 9}, scene_test::RawTag{6});

    std::string path = TempScenePath("aether_test_scene.ascene");
    AETHER_CHECK(SaveSceneJson(world, path));

    std::vector<u8> bytes = ReadAll(path);
    std::string text(bytes.begin(), bytes.end());
    AETHER_CHECK(text.find("\"$type\": \"Scene\"") != std::string::npos);
    AETHER_CHECK(text.find("\"Stamina\"") != std::string::npos);
    AETHER_CHECK(text.find("\"current\": 0.1") != std::string::npos);
    AETHER_CHECK(text.find("RawTag") == std::string::npos);

    World loaded;
    AETHER_CHECK(LoadSceneJson(loaded, path));
    AETHER_CHECK(loaded.EntityCount() == 2);
    int both = 0;
    loaded.ForEach<scene_test::Stamina, scene_test::LegacyPos>([&](scene_test::Stamina& st, scene_test::LegacyPos& p) {
        ++both;
        AETHER_CHECK(st.current == 0.1f && st.label == "hero" && st.regen == 1.5f);
        AETHER_CHECK(p.y == 2.0f);
    });
    AETHER_CHECK(both == 1);
    int positions = 0;
    loaded.ForEach<scene_test::LegacyPos>([&](scene_test::LegacyPos&) { ++positions; });
    AETHER_CHECK(positions == 2);

    // Saving what was loaded reproduces the same file.
    std::string resaved = TempScenePath("aether_test_scene_resaved.ascene");
    AETHER_CHECK(SaveSceneJson(loaded, resaved));
    AETHER_CHECK(ReadAll(resaved) == bytes);

    std::filesystem::remove(path);
    std::filesystem::remove(resaved);
}

AETHER_TEST(SceneJson_ToleratesUnknownComponentsAndRejectsNonScenes) {
    std::string path = TempScenePath("aether_test_scene_unknown.ascene");
    const char* text = R"({
      "$type": "Scene", "$version": 1,
      "entities": [
        { "components": { "Stamina": { "$v": 1, "current": 3, "future_field": true },
                          "SomeComponentFromTheFuture": { "x": 1 } } },
        "not an entity"
      ]
    })";
    AETHER_CHECK(fs::WriteFileBytes(path, text, std::strlen(text)));
    World world;
    AETHER_CHECK(LoadSceneJson(world, path));
    AETHER_CHECK(world.EntityCount() == 1);
    world.ForEach<scene_test::Stamina>([&](scene_test::Stamina& s) {
        AETHER_CHECK(s.current == 3.0f && s.label == "stamina");
    });

    const char* not_scene = R"({"$type": "Prefab", "$version": 1, "entities": []})";
    AETHER_CHECK(fs::WriteFileBytes(path, not_scene, std::strlen(not_scene)));
    World other;
    AETHER_CHECK(!LoadSceneJson(other, path));
    AETHER_CHECK(fs::WriteFileBytes(path, "garbage", 7));
    AETHER_CHECK(!LoadSceneJson(other, path));
    std::filesystem::remove(path);
}
