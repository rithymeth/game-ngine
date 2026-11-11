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
    AETHER_CHECK(offsetof(FrameData, wind) == 1232 && offsetof(FrameData, wind_gusts) == 1248 && sizeof(FrameData) == 1264);
    const std::string hlsl = FrameDataHlsl();
    const usize at_time = hlsl.find("float4 time;"), at_wind = hlsl.find("float4 wind;"), at_gusts = hlsl.find("float4 wind_gusts;");
    AETHER_CHECK(at_time != std::string::npos && at_wind != std::string::npos && at_gusts != std::string::npos && at_time < at_wind && at_wind < at_gusts);
}
