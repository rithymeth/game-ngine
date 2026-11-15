#include "test_framework.h"

#ifdef AETHER_TEST_HAS_TIMEOFDAY

#include "aether/assets/asset_guid.h"
#include "aether/ecs/world.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/scene/components.h"
#include "aether/timeofday/timeofday_kit.h"
#include "aether/timeofday/world_clock.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <limits>

// The time-of-day kit: the game's world clock (an hour and a day count that advance with the frame, a phase of the day and a sunlight level).

using namespace aether;
using namespace aether::tod;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {
bool Near(f64 a, f64 b, f64 eps = 1e-6) { return std::fabs(a - b) < eps; }
} // namespace

AETHER_TEST(TimeOfDay_ClockAdvancesWrapsAndCountsDays) {
    WorldClock clock; // 8:00 on day 0, a game day lasting 1200 real seconds
    CHECK(WorldClock::Active() == &clock);
    CHECK(Near(clock.Hour(), 8.0) && clock.Day() == 0 && clock.CurrentPhase() == Phase::Day && clock.TimeText() == "08:00");

    clock.Advance(600.0); // half a day: 20:00
    CHECK(Near(clock.Hour(), 20.0) && clock.Day() == 0 && clock.CurrentPhase() == Phase::Dusk);
    clock.Advance(600.0); // 8:00 on the next day
    CHECK(Near(clock.Hour(), 8.0) && clock.Day() == 1 && Near(clock.TotalHours(), 32.0));
    clock.Advance(1200.0 * 5.0 + 300.0); // five days and a quarter more: it does not loop per day
    CHECK(clock.Day() == 6 && Near(clock.Hour(), 14.0));

    // Not a positive number: nothing moves.
    const f64 before = clock.TotalHours();
    clock.Advance(0.0);
    clock.Advance(-5.0);
    clock.Advance(std::numeric_limits<f64>::quiet_NaN());
    clock.Advance(std::numeric_limits<f64>::infinity());
    CHECK(clock.TotalHours() == before);

    // The phases and their edges.
    const auto phase_at = [&](f64 hour) { CHECK(clock.SetHour(hour)); return clock.CurrentPhase(); };
    CHECK(phase_at(4.99) == Phase::Night && phase_at(5.0) == Phase::Dawn && phase_at(7.99) == Phase::Dawn && phase_at(8.0) == Phase::Day);
    CHECK(phase_at(17.99) == Phase::Day && phase_at(18.0) == Phase::Dusk && phase_at(20.99) == Phase::Dusk && phase_at(21.0) == Phase::Night);
    CHECK(phase_at(0.0) == Phase::Night && clock.IsNight() && std::string(PhaseName(Phase::Dusk)) == "dusk");

    // Sunlight: dark at midnight, full at noon, half at the quarter marks.
    CHECK(clock.SetHour(0.0) && Near(clock.Sunlight(), 0.0));
    CHECK(clock.SetHour(12.0) && Near(clock.Sunlight(), 1.0));
    CHECK(clock.SetHour(6.0) && Near(clock.Sunlight(), 0.5) && clock.SetHour(18.0) && Near(clock.Sunlight(), 0.5));

    // The setters: the hour wraps, and what makes no sense is refused with nothing changed.
    CHECK(clock.SetHour(-1.0) && Near(clock.Hour(), 23.0) && clock.SetHour(25.5) && Near(clock.Hour(), 1.5) && clock.TimeText() == "01:30");
    CHECK(!clock.SetHour(std::numeric_limits<f64>::quiet_NaN()) && !clock.SetHour(std::numeric_limits<f64>::infinity()) && Near(clock.Hour(), 1.5));
    CHECK(clock.SetDay(3) && clock.Day() == 3 && !clock.SetDay(-1) && clock.Day() == 3);
    CHECK(!clock.SetDayLength(0.5) && !clock.SetDayLength(2.0e6) && !clock.SetDayLength(std::numeric_limits<f64>::quiet_NaN()) && Near(clock.DayLength(), 1200.0));
    CHECK(!clock.SetScale(-1.0) && !clock.SetScale(1001.0) && !clock.SetScale(std::numeric_limits<f64>::quiet_NaN()) && Near(clock.Scale(), 1.0));

    // Speed: a shorter day and a scale both make the hours go by faster; a scale of 0 pauses.
    CHECK(clock.SetHour(0.0) && clock.SetDayLength(24.0) && clock.SetScale(1.0));
    clock.Advance(3.0); // one game hour per real second
    CHECK(Near(clock.Hour(), 3.0));
    CHECK(clock.SetScale(2.0));
    clock.Advance(1.0);
    CHECK(Near(clock.Hour(), 5.0));
    CHECK(clock.SetScale(0.0));
    clock.Advance(100.0);
    CHECK(Near(clock.Hour(), 5.0));
}

AETHER_TEST(TimeOfDay_TheKitAddsItsStageAndAdvancesTheClock) {
    const std::unique_ptr<kit::IKit> kit = MakeTimeOfDayKit();
    CHECK(std::string(kit->Name()) == "TimeOfDay" && kit->Deps().empty());
    const std::vector<kit::KitStage> stages = kit->Stages();
    CHECK(stages.size() == 1 && stages[0].name == "Player.TimeOfDay" && stages[0].after == std::vector<std::string>{"Player.Update"});

    WorldClock clock;
    CHECK(clock.SetHour(0.0));
    SystemDesc system = MakeTimeOfDaySystem(&clock);
    CHECK(system.name == "Player.TimeOfDay" && system.run != nullptr);
    World world;
    FrameContext frame;
    frame.dt = 60.0f; // a minute: 1.2 game hours of a 1200 second day
    system.run(world, frame);
    CHECK(Near(clock.Hour(), 1.2, 1e-5));
    MakeTimeOfDaySystem(nullptr).run(world, frame); // no clock: nothing happens, and nothing crashes
    CHECK(Near(clock.Hour(), 1.2, 1e-5));
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer) {
    return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", importer}};
}

} // namespace

// The player advances the clock every frame, and a scene change does not touch it.
AETHER_TEST(TimeOfDay_PlayerRunsTheClockThroughASceneChange) {
    if (!player::Game::HasScripting()) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_timeofday_player_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string script_guid = assets::ToString(assets::NewAssetGuid());
    ScriptComponent component;
    assets::AssetGuid parsed;
    CHECK(assets::ParseAssetGuid(script_guid, parsed));
    component.script.guid = parsed;
    const nlohmann::json a = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array({{{"components", {{"ScriptComponent", reflect::ToJson(component)}}}}})}};
    const nlohmann::json b = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array()}};
    const std::string script = "local S = {}\nfunction S:OnUpdate(dt)\n  if not self.asked then self.asked = true; Scene.Load('Scenes/b.ascene') end\nend\nreturn S\n";
    const nlohmann::json assets_json = nlohmann::json::array({AssetEntry("Scenes/a.ascene", "Scene"), AssetEntry("Scenes/b.ascene", "Scene"),
                                                              {{"guid", script_guid}, {"path", "Scripts/Swap.luau"}, {"importer", "Script"}}});
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/a.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()}, {"assets", assets_json}, {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    CHECK(writer.Add("Manifest.json", manifest.dump(2)));
    CHECK(writer.Add("Content/Scenes/a.ascene", a.dump()));
    CHECK(writer.Add("Content/Scenes/b.ascene", b.dump()));
    CHECK(writer.Add("Content/Scripts/Swap.luau", script));
    const std::filesystem::path file = dir / "Game.apak";
    CHECK(writer.Write(file.string()));
    player::GamePackage package;
    std::string error;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    CHECK(game.LoadStartupScene(&error));
    WorldClock* clock = WorldClock::Active();
    CHECK(clock != nullptr);
    if (!clock) return;
    CHECK(clock->SetHour(8.0));
    game.BeginPlay();
    for (int i = 0; i < 4; ++i) game.Tick(1.0f);
    CHECK(game.SceneName() == "Scenes/b.ascene" && game.ScriptErrors().empty());
    CHECK(WorldClock::Active() == clock);
    CHECK(Near(clock->Hour(), 8.0 + 4.0 / 1200.0 * 24.0, 1e-4)); // four seconds of the frame's dt, scene change and all
}

#if AETHER_TEST_HAS_SCRIPTING
AETHER_TEST(TimeOfDay_LuauTableActsOnTheClock) {
    World world;
    WorldClock clock;
    script::LuauHost host;
    GuidIndex guids;
    const assets::AssetGuid hero = assets::NewAssetGuid();
    (void)GetComponentId<ScriptComponent>();
    script::ScriptSystem scripts(host, world, guids, [&](const assets::AssetGuid& g, std::string& source, std::string& name) {
        if (g != hero) return false;
        source = "local H = {} function H:Run() self.hour0 = TimeOfDay.GetHour(); self.text0 = TimeOfDay.GetTimeText(); self.phase0 = TimeOfDay.GetPhase(); "
                 "self.set = TimeOfDay.SetHour(23.5); self.night = TimeOfDay.IsNight(); self.phase1 = TimeOfDay.GetPhase(); self.text1 = TimeOfDay.GetTimeText(); "
                 "self.wrapped = TimeOfDay.SetHour(26); self.hour1 = TimeOfDay.GetHour(); self.bad = TimeOfDay.SetHour(0/0); "
                 "self.day_set = TimeOfDay.SetDay(7); self.day = TimeOfDay.GetDay(); self.day_bad = TimeOfDay.SetDay(-2); self.day_frac = TimeOfDay.SetDay(1.5); "
                 "self.total = TimeOfDay.GetTotalHours(); TimeOfDay.SetHour(12); self.sun = TimeOfDay.Sunlight(); "
                 "self.len_set = TimeOfDay.SetDayLength(600); self.len = TimeOfDay.GetDayLength(); self.len_bad = TimeOfDay.SetDayLength(0); "
                 "self.scale_set = TimeOfDay.SetScale(0); self.scale = TimeOfDay.GetScale(); self.scale_bad = TimeOfDay.SetScale(-1) end return H";
        name = "H.luau";
        return true;
    });
    ScriptComponent component;
    component.script.guid = hero;
    const Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, std::move(component));
    guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
    Lifecycle life(world, guids);
    scripts.Register(life);
    life.BeginPlay();
    CHECK(!host.Run("return TimeOfDay.SetHour('noon')").ok && !host.Run("return TimeOfDay.SetScale(nil)").ok && !host.Run("return TimeOfDay.SetDay({})").ok); // wrong types are script errors
    CHECK(scripts.SendEvent(e, "Run") && scripts.Errors().empty());
    const auto text = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string("<not a string>"); };
    const auto flag = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<bool>(v) && std::get<bool>(v); };
    const auto num = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<f64>(v) ? std::get<f64>(v) : -999.0; };
    CHECK(Near(num("hour0"), 8.0) && text("text0") == "08:00" && text("phase0") == "day");
    CHECK(flag("set") && flag("night") && text("phase1") == "night" && text("text1") == "23:30");
    CHECK(flag("wrapped") && Near(num("hour1"), 2.0) && !flag("bad"));
    CHECK(flag("day_set") && Near(num("day"), 7.0) && !flag("day_bad") && !flag("day_frac") && Near(num("total"), 7.0 * 24.0 + 2.0));
    CHECK(Near(num("sun"), 1.0));
    CHECK(flag("len_set") && Near(num("len"), 600.0) && !flag("len_bad") && flag("scale_set") && Near(num("scale"), 0.0) && !flag("scale_bad"));
    CHECK(Near(clock.DayLength(), 600.0) && Near(clock.Scale(), 0.0) && clock.Day() == 7);
    life.EndPlay();
}
#endif

#endif
