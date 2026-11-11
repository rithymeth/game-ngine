#include "aether/ecs/world.h"
#include "aether/renderer/components.h"
#include "aether/scene/components.h"
#include "aether/scene/serialization.h"
#include "../player/src/frame_lights.h"
#include "test_framework.h"

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>

using namespace aether;
using namespace aether::player;

// The Wind component and what the scene shader gets of it: defaults for a world with none, the first one used, bad values cleaned, and the scene file.

namespace {
FrameData Frame(const World& world) {
    GuidIndex guids;
    return BuildFrameData(world, guids, Vec3(0, 0, 0));
}
} // namespace

AETHER_TEST(Wind_AWorldWithNoWindHasTheOldBreeze) {
    RegisterRenderComponents();
    World world;
    AETHER_CHECK(FindWind(world) == nullptr);
    const FrameData data = Frame(world);
    const f32 radians = 27.0f * 3.14159265f / 180.0f;
    AETHER_CHECK(std::fabs(data.wind[0] - std::cos(radians)) < 1e-5f && std::fabs(data.wind[1] - std::sin(radians)) < 1e-5f);
    AETHER_CHECK(data.wind[2] == 1.0f && data.wind[3] == 1.0f && std::fabs(data.wind_gusts[0] - 0.4f) < 1e-6f);
    // The defaults of the component are the same breeze.
    const Wind defaults;
    AETHER_CHECK(defaults.direction_degrees == 27.0f && defaults.strength == 1.0f && defaults.gust_speed == 1.0f && std::fabs(defaults.gustiness - 0.4f) < 1e-6f);
}

AETHER_TEST(Wind_TheFirstWindSetsTheFrameDataAndBadValuesAreCleaned) {
    RegisterRenderComponents();
    World world;
    world.CreateEntity(Wind{90.0f, 2.5f, 0.5f, 0.8f});
    world.CreateEntity(Wind{0.0f, 4.0f, 4.0f, 1.0f}); // a second one is ignored
    AETHER_CHECK(FindWind(world) != nullptr && FindWind(world)->strength == 2.5f);
    const FrameData data = Frame(world);
    AETHER_CHECK(std::fabs(data.wind[0]) < 1e-5f && std::fabs(data.wind[1] - 1.0f) < 1e-5f); // toward +Z
    AETHER_CHECK(data.wind[2] == 2.5f && data.wind[3] == 0.5f && std::fabs(data.wind_gusts[0] - 0.8f) < 1e-6f);
    AETHER_CHECK(std::fabs(data.wind[0] * data.wind[0] + data.wind[1] * data.wind[1] - 1.0f) < 1e-5f); // a unit direction

    // Out-of-range values are clamped; not-a-number takes the default.
    World wild;
    wild.CreateEntity(Wind{540.0f, 100.0f, 0.0f, 5.0f});
    const FrameData clamped = Frame(wild);
    AETHER_CHECK(std::fabs(clamped.wind[0] + 1.0f) < 1e-5f && std::fabs(clamped.wind[1]) < 1e-4f); // 540 degrees is 180: toward -X
    AETHER_CHECK(clamped.wind[2] == 4.0f && clamped.wind[3] == 0.1f && clamped.wind_gusts[0] == 1.0f);
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    World broken;
    broken.CreateEntity(Wind{nan, nan, nan, nan});
    const FrameData safe = Frame(broken);
    AETHER_CHECK(std::isfinite(safe.wind[0]) && std::isfinite(safe.wind[1]) && safe.wind[2] == 1.0f && safe.wind[3] == 1.0f && std::fabs(safe.wind_gusts[0] - 0.4f) < 1e-6f);
    World calm;
    calm.CreateEntity(Wind{27.0f, 0.0f, 1.0f, 0.4f});
    AETHER_CHECK(Frame(calm).wind[2] == 0.0f); // calm is a strength of zero, not a default
}

AETHER_TEST(Wind_IsASceneComponentAndTheLayoutKeepsInStepWithTheShader) {
    RegisterRenderComponents();
    World world;
    world.CreateEntity(Wind{145.0f, 3.0f, 2.0f, 0.25f});
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "aether_wind_scene.ascene";
    AETHER_CHECK(SaveSceneJson(world, file.string()));
    World loaded;
    AETHER_CHECK(LoadSceneJson(loaded, file.string()));
    const Wind* wind = FindWind(loaded);
    AETHER_CHECK(wind != nullptr && wind->direction_degrees == 145.0f && wind->strength == 3.0f && wind->gust_speed == 2.0f && wind->gustiness == 0.25f);
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    // The CPU struct and the HLSL one have the wind in the same place: the last members, a float4 each after the time.
    AETHER_CHECK(offsetof(FrameData, wind) == 1232 && offsetof(FrameData, wind_gusts) == 1248 && sizeof(FrameData) == 1408);
    const std::string hlsl = FrameDataHlsl();
    const usize at_time = hlsl.find("float4 time;"), at_wind = hlsl.find("float4 wind;"), at_gusts = hlsl.find("float4 wind_gusts;");
    AETHER_CHECK(at_time != std::string::npos && at_wind != std::string::npos && at_gusts != std::string::npos && at_time < at_wind && at_wind < at_gusts);
}

AETHER_TEST(WindZone_AnObjectTakesTheZoneItIsMostInsideAndTheFrameDataCarriesTheTable) {
    World world;
    GuidIndex guids;
    world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, WindZone{10.0f, 90.0f, 3.0f, 2.0f, 0.5f});
    world.CreateEntity(Transform{Vec3(100, 0, 0), Quaternion::Identity()}, WindZone{10.0f, 180.0f, 0.0f, 1.0f, 0.0f});
    const std::vector<WindZoneSite> zones = CollectWindZones(world, guids);
    AETHER_CHECK(zones.size() == 2);
    if (zones.size() != 2) return;
    AETHER_CHECK(zones[1].center.x == 100.0f);
    AETHER_CHECK(WindZoneBitsAt(zones, Vec3(0, 0, 0)) == (1u | (31u << 3)));        // the core of the first
    AETHER_CHECK(WindZoneBitsAt(zones, Vec3(50, 0, 0)) == 0);                       // between them: the world's wind
    AETHER_CHECK(WindZoneBitsAt(zones, Vec3(100, 0, 7)) == (2u | (31u << 3)));      // the second has no fade
    const u32 faded = WindZoneBitsAt(zones, Vec3(7.5f, 0, 0));                      // half way through the first one's fade
    AETHER_CHECK((faded & 7u) == 1u && (faded >> 3) > 10u && (faded >> 3) < 21u);
    AETHER_CHECK(WindZoneBitsAt({}, Vec3(0, 0, 0)) == 0);
    const FrameData data = BuildFrameData(world, guids, Vec3(0, 0, 0));
    AETHER_CHECK(std::fabs(data.wind_zones[0][1] - 1.0f) < 1e-5f && data.wind_zones[0][2] == 3.0f && data.wind_zones[0][3] == 2.0f);
    AETHER_CHECK(data.wind_zones[1][2] == 0.0f);
    // Only seven are used.
    World crowded;
    for (int i = 0; i < 10; ++i) crowded.CreateEntity(Transform{Vec3(static_cast<f32>(i) * 50.0f, 0, 0), Quaternion::Identity()}, WindZone{});
    AETHER_CHECK(CollectWindZones(crowded, guids).size() == kMaxWindZones);
}

AETHER_TEST(WindZone_AnObjectsWindBlendsTheZonesWithTheWorlds) {
    World world;
    GuidIndex guids;
    world.CreateEntity(Wind{0.0f, 2.0f, 1.0f, 0.4f});                     // the world: east, strength 2
    world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, WindZone{10.0f, 90.0f, 2.0f, 3.0f, 0.5f}); // the zone: +Z, strength 2, gusts 3
    const FrameData data = BuildFrameData(world, guids, Vec3(0, 0, 0));
    const WindSample outside = WindForDraw(data, 0);
    AETHER_CHECK(std::fabs(outside.direction[0] - 1.0f) < 1e-5f && std::fabs(outside.strength - 2.0f) < 1e-5f && outside.gust_speed == 1.0f);
    const WindSample inside = WindForDraw(data, 1u | (31u << 3));          // the core: all the zone's
    AETHER_CHECK(std::fabs(inside.direction[1] - 1.0f) < 1e-5f && std::fabs(inside.direction[0]) < 1e-5f && std::fabs(inside.strength - 2.0f) < 1e-5f && inside.gust_speed == 3.0f);
    const WindSample half = WindForDraw(data, 1u | (16u << 3));            // between: a mix, in between in direction, and weaker where the two disagree
    AETHER_CHECK(half.direction[0] > 0.3f && half.direction[1] > 0.3f && half.strength < 2.0f && half.strength > 1.0f && half.gust_speed > 1.0f && half.gust_speed < 3.0f);
    AETHER_CHECK(WindForDraw(data, 7u).strength == 2.0f);                  // a slot with no zone behind it is the world's wind
}

// Per-instance colour variation of foliage (player/src/instance_variation.h).
#include "../player/src/instance_variation.h"
AETHER_TEST(InstanceTint_IsSteadyBoundedAndVariesWithThePlace) {
    const Vec3 here(12.3f, 4.0f, -7.7f);
    const Vec3 a = player::InstanceTint(here), b = player::InstanceTint(here);
    AETHER_CHECK(a.x == b.x && a.y == b.y && a.z == b.z); // the same place is the same tint
    const Vec3 nudged = player::InstanceTint(Vec3(here.x + 0.01f, here.y, here.z));
    AETHER_CHECK(std::fabs(nudged.y - a.y) < 1e-6f);      // a hair of movement keeps the colour (a decimetre grid)
    double sum = 0.0, low = 10.0, high = 0.0;
    usize distinct = 0;
    f32 last = -1.0f;
    for (int i = 0; i < 4000; ++i) {
        const Vec3 t = player::InstanceTint(Vec3(static_cast<f32>(i) * 1.7f, 2.0f, static_cast<f32>(i % 61) * 3.1f));
        for (const f32 c : {t.x, t.y, t.z}) { low = std::min<double>(low, c); high = std::max<double>(high, c); }
        sum += t.y;
        if (std::fabs(t.y - last) > 1e-3f) ++distinct;
        last = t.y;
    }
    AETHER_CHECK(low >= 0.78 && high <= 1.22);            // never far from the material's own colour
    AETHER_CHECK(std::fabs(sum / 4000.0 - 1.0) < 0.02);   // and no brighter or darker on average
    AETHER_CHECK(distinct > 3500);                        // really different from place to place
    AETHER_CHECK(high - low > 0.25);                      // with a visible spread
}

AETHER_TEST(SkyLight_CloudSettingsReachTheFrameDataCleaned) {
    GuidIndex guids;
    World none;
    AETHER_CHECK(BuildFrameData(none, guids, Vec3(0, 0, 0)).clouds[0] == 0.0f); // no SkyLight: no clouds
    World world;
    SkyLight sky;
    sky.cloud_coverage = 0.4f;
    sky.cloud_scale = 1.5f;
    sky.cloud_speed = 0.1f;
    world.CreateEntity(sky);
    const FrameData data = BuildFrameData(world, guids, Vec3(0, 0, 0));
    AETHER_CHECK(data.clouds[0] == 0.4f && data.clouds[1] == 0.1f && data.clouds[2] == 1.5f);
    World wild;
    SkyLight bad;
    bad.cloud_coverage = 9.0f;
    bad.cloud_scale = std::numeric_limits<f32>::quiet_NaN();
    bad.cloud_speed = -3.0f;
    wild.CreateEntity(bad);
    const FrameData clamped = BuildFrameData(wild, guids, Vec3(0, 0, 0));
    AETHER_CHECK(clamped.clouds[0] == 1.0f && clamped.clouds[1] == 0.0f && clamped.clouds[2] == 0.7f);
    AETHER_CHECK(SkyLight{}.cloud_coverage == 0.0f); // off by default
}
