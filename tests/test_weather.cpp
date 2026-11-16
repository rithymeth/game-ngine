#include "test_framework.h"

#ifdef AETHER_TEST_HAS_WEATHER

#include "aether/assets/asset_guid.h"
#include "aether/ecs/world.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/scene/components.h"
#include "aether/weather/weather_kit.h"
#include "aether/weather/weather_system.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <vector>

// The weather kit: the game's weather as numbers (clouds, rain, snow, fog, wind with gusts, lightning, wet ground) that blend from one kind to the next, by hand or
// by themselves from a seed.

using namespace aether;
using namespace aether::weather;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f64 a, f64 b, f64 eps = 1e-4) { return std::fabs(a - b) < eps; }

// Runs `seconds` of weather in steps of `dt`.
void Run(WeatherSystem& w, f64 seconds, f64 dt = 1.0 / 60.0) {
    const long steps = static_cast<long>(std::lround(seconds / dt));
    for (long i = 0; i < steps; ++i) w.Advance(dt);
}

} // namespace

AETHER_TEST(Weather_KindsHaveNamesAndSensibleNumbers) {
    for (u32 i = 0; i < kWeatherKindCount; ++i) {
        const WeatherKind kind = static_cast<WeatherKind>(i);
        WeatherKind back;
        CHECK(ParseWeather(WeatherName(kind), back) && back == kind);
        const WeatherParams p = ProfileOf(kind);
        CHECK(p.clouds >= 0 && p.clouds <= 1 && p.rain >= 0 && p.rain <= 1 && p.snow >= 0 && p.snow <= 1 && p.fog >= 0 && p.fog <= 1);
        CHECK(p.light_scale > 0 && p.light_scale <= 1 && p.wind >= 0 && p.lightning >= 0);
    }
    WeatherKind k = WeatherKind::Fog;
    CHECK(ParseWeather("STORM", k) && k == WeatherKind::Storm && ParseWeather("Partly_Cloudy", k) && k == WeatherKind::PartlyCloudy);
    CHECK(!ParseWeather("hail", k) && !ParseWeather("", k) && k == WeatherKind::PartlyCloudy); // unchanged on failure
    CHECK(std::string(WeatherName(static_cast<WeatherKind>(200))).empty());
    // Only the storm has lightning, only snow has snow, only the rains rain, fog is the thickest.
    CHECK(ProfileOf(WeatherKind::Storm).lightning > 0 && ProfileOf(WeatherKind::Rain).lightning == 0 && ProfileOf(WeatherKind::Snow).snow > 0);
    CHECK(ProfileOf(WeatherKind::Rain).rain > 0 && ProfileOf(WeatherKind::Storm).rain > ProfileOf(WeatherKind::Rain).rain && ProfileOf(WeatherKind::Clear).rain == 0);
    CHECK(ProfileOf(WeatherKind::Fog).fog > ProfileOf(WeatherKind::Storm).fog && ProfileOf(WeatherKind::Clear).light_scale > ProfileOf(WeatherKind::Storm).light_scale);
}

AETHER_TEST(Weather_BlendsSmoothlyAndAnInterruptedBlendCarriesOn) {
    WeatherSystem w;
    CHECK(WeatherSystem::Active() == &w && w.Current() == WeatherKind::Clear && w.Target() == WeatherKind::Clear && Near(w.Blend(), 1.0));
    CHECK(w.Set(WeatherKind::Rain, 10.0) && w.Target() == WeatherKind::Rain && w.Current() == WeatherKind::Clear && Near(w.Blend(), 0.0));
    CHECK(Near(w.Rain(), 0.0));
    Run(w, 2.5); // a quarter of the way in time is only 0.156 of the way in rain: a smooth step starts slowly (a straight line would be 0.25)
    CHECK(Near(w.Rain(), 0.6 * 0.15625, 6e-3) && Near(w.Blend(), 0.25, 4e-3));
    Run(w, 2.5);
    CHECK(Near(w.Blend(), 0.5, 4e-3) && Near(w.Rain(), 0.3, 4e-3) && w.Current() == WeatherKind::Clear);
    f32 previous = w.Rain();
    for (int i = 0; i < 10; ++i) { // it only rises, a little at a time
        Run(w, 0.4);
        CHECK(w.Rain() >= previous && w.Rain() - previous < 0.2f);
        previous = w.Rain();
    }
    Run(w, 2.0);
    CHECK(w.Current() == WeatherKind::Rain && Near(w.Blend(), 1.0) && Near(w.Rain(), 0.6) && Near(w.Clouds(), 0.9) && Near(w.LightScale(), 0.4));

    // Interrupted halfway to fog: the numbers carry on from where they were, with no jump.
    CHECK(w.Set(WeatherKind::Fog, 10.0));
    Run(w, 4.0);
    const WeatherParams mid = w.Params();
    CHECK(mid.rain > 0.0f && mid.rain < 0.6f && mid.fog > 0.3f);
    CHECK(w.Set(WeatherKind::Clear, 10.0));
    CHECK(Near(w.Rain(), mid.rain) && Near(w.Fog(), mid.fog)); // right after the new Set: unchanged
    Run(w, 0.05);               // and a couple of steps on it is still where it was, give or take the first moments of a slow start: no jump back to fog or forward to clear
    CHECK(std::fabs(w.Rain() - mid.rain) < 0.01 && std::fabs(w.Fog() - mid.fog) < 0.01);
    Run(w, 20.0);
    CHECK(w.Current() == WeatherKind::Clear && Near(w.Rain(), 0.0) && Near(w.Fog(), 0.0) && Near(w.Clouds(), 0.05));
    // At once.
    CHECK(w.Set(WeatherKind::Storm, 0.0) && w.Current() == WeatherKind::Storm && Near(w.Rain(), 1.0) && Near(w.Blend(), 1.0));
}

AETHER_TEST(Weather_RefusesWhatItCannotUse) {
    WeatherSystem w;
    const f64 nan = std::numeric_limits<f64>::quiet_NaN();
    CHECK(!w.Set(WeatherKind::Rain, nan) && !w.Set(WeatherKind::Rain, -1.0) && !w.Set(WeatherKind::Rain, 4000.0) && !w.Set(WeatherKind::Rain, std::numeric_limits<f64>::infinity()));
    CHECK(!w.Set(static_cast<WeatherKind>(99), 1.0));
    CHECK(w.Target() == WeatherKind::Clear); // nothing changed
    CHECK(!w.SetMeanDuration(0.5) && !w.SetMeanDuration(2.0e6) && !w.SetMeanDuration(nan) && Near(w.MeanDuration(), 300.0) && w.SetMeanDuration(60.0) && Near(w.MeanDuration(), 60.0));
    CHECK(!w.SetWindDirection(nan) && !w.SetWindDirection(std::numeric_limits<f64>::infinity()) && Near(w.WindDirection(), 0.0));
    CHECK(w.SetWindDirection(-90.0) && Near(w.WindDirection(), 270.0) && w.SetWindDirection(725.0) && Near(w.WindDirection(), 5.0)); // wraps into 0..360
    // Advance ignores what is not a time, and a huge one is held to an hour.
    w.Advance(nan);
    w.Advance(-5.0);
    w.Advance(0.0);
    CHECK(Near(w.Time(), 0.0));
    w.Advance(1.0e12);
    CHECK(Near(w.Time(), 3600.0, 0.1));
}

AETHER_TEST(Weather_ChangesByItselfFromASeedAndRepeatsExactly) {
    const auto history = [](u64 seed, bool cold) {
        WeatherSystem w;
        w.SetSeed(seed);
        w.SetCold(cold);
        w.SetMeanDuration(120.0);
        w.SetAuto(true);
        std::vector<int> kinds;
        WeatherKind last = w.Target();
        for (int i = 0; i < 360 * 300; ++i) { // six hours: 21600 s in steps of 0.2
            w.Advance(1.0 / 5.0);
            if (w.Target() != last) {
                last = w.Target();
                kinds.push_back(static_cast<int>(last));
            }
        }
        return kinds;
    };
    const std::vector<int> a = history(42, false), b = history(42, false), c = history(43, false);
    CHECK(a.size() > 40 && a == b); // six hours at a mean of two minutes: dozens of changes, the same every time
    CHECK(a != c);                  // another seed, another weather
    // Never the same kind twice in a row, never snow without the cold; and in the cold rain and storms become snow.
    for (usize i = 0; i < a.size(); ++i) {
        CHECK(a[i] != static_cast<int>(WeatherKind::Snow));
        if (i > 0) CHECK(a[i] != a[i - 1]);
    }
    std::map<int, int> counts;
    for (const int k : a) ++counts[k];
    CHECK(counts[static_cast<int>(WeatherKind::Clear)] > 0 && counts[static_cast<int>(WeatherKind::Rain)] > 0 && counts[static_cast<int>(WeatherKind::Overcast)] > 0);
    const std::vector<int> cold = history(42, true);
    bool any_snow = false;
    for (const int k : cold) {
        CHECK(k != static_cast<int>(WeatherKind::Rain) && k != static_cast<int>(WeatherKind::Storm));
        any_snow = any_snow || k == static_cast<int>(WeatherKind::Snow);
    }
    CHECK(any_snow);
}

AETHER_TEST(Weather_TheAutoTimingKeepsToTheMeanAndTheWeatherIsTheSameAtAnyFrameRate) {
    WeatherSystem w;
    w.SetSeed(7);
    w.SetMeanDuration(100.0);
    w.SetAuto(true);
    // The time between changes is 0.5 to 1.5 times the mean.
    f64 last = 0.0;
    std::vector<f64> gaps;
    WeatherKind kind = w.Target();
    for (int i = 0; i < 40000; ++i) {
        w.Advance(0.1);
        if (w.Target() != kind) {
            kind = w.Target();
            gaps.push_back(w.Time() - last);
            last = w.Time();
        }
    }
    CHECK(gaps.size() > 20);
    for (usize i = 1; i < gaps.size(); ++i) CHECK(gaps[i] >= 49.0 && gaps[i] <= 151.0);

    // The same seed run at 30 and at 144 frames per second gives the same weather and the same strikes.
    const auto at = [](f64 dt) {
        WeatherSystem s;
        s.SetSeed(99);
        s.SetMeanDuration(60.0);
        s.SetAuto(true);
        s.Set(WeatherKind::Storm, 5.0);
        Run(s, 600.0, dt);
        return std::vector<f64>{static_cast<f64>(static_cast<int>(s.Target())), static_cast<f64>(s.StrikeCount()), s.Rain(), s.Wetness(), static_cast<f64>(s.WindSpeed())};
    };
    const std::vector<f64> slow = at(1.0 / 30.0), fast = at(1.0 / 144.0), odd = at(0.0173);
    for (usize i = 0; i < slow.size(); ++i) CHECK(Near(slow[i], fast[i], 0.02) && Near(slow[i], odd[i], 0.02));
}

AETHER_TEST(Weather_WindHasAGustAndAnAimedDirection) {
    WeatherSystem w;
    CHECK(w.Set(WeatherKind::Storm, 0.0));
    f64 lo = 1e9, hi = -1e9, total = 0.0;
    int n = 0;
    for (int i = 0; i < 6000; ++i) { // a minute and a half of storm wind
        w.Advance(1.0 / 60.0);
        const f64 speed = w.WindSpeed();
        CHECK(speed >= 0.0);
        lo = std::min(lo, speed);
        hi = std::max(hi, speed);
        total += speed;
        ++n;
    }
    CHECK(hi - lo > 8.0);                       // gusts: it swells and drops by a good part of its 18 m/s
    CHECK(std::fabs(total / n - 18.0) < 3.0);   // around the steady wind
    CHECK(w.Set(WeatherKind::Clear, 0.0));
    Run(w, 30.0);
    const f64 calm = w.WindSpeed();
    CHECK(calm < 4.5);                          // clear weather is gentle (2 m/s steady, a gust of most half as much again)
    // The direction: 0 towards +Z, 90 towards +X, and the vector has the speed's length.
    CHECK(w.SetWindDirection(0.0));
    Vec3 v = w.WindVector();
    CHECK(Near(v.x, 0.0, 1e-4) && v.z >= 0.0 && Near(v.Length(), w.WindSpeed(), 1e-4) && Near(v.y, 0.0));
    CHECK(w.SetWindDirection(90.0));
    v = w.WindVector();
    CHECK(v.x >= 0.0 && Near(v.z, 0.0, 1e-3) && Near(v.Length(), w.WindSpeed(), 1e-3));
    CHECK(w.SetWindDirection(180.0));
    CHECK(w.WindVector().z <= 0.0);
}

AETHER_TEST(Weather_LightningOnlyComesInAStormAndThunderIsLate) {
    WeatherSystem clear;
    Run(clear, 600.0);
    CHECK(clear.StrikeCount() == 0 && Near(clear.Lightning(), 0.0) && clear.ThunderDelay() == 0.0 && clear.SecondsSinceStrike() > 1.0e9); // ten quiet minutes

    WeatherSystem storm;
    storm.SetSeed(5);
    CHECK(storm.Set(WeatherKind::Storm, 0.0));
    u32 seen = 0;
    f32 peak = 0.0f;
    bool decayed = false;
    for (int i = 0; i < 600 * 60; ++i) { // ten minutes at 6 strikes a minute: about 60
        const u32 before = storm.StrikeCount();
        storm.Advance(1.0 / 60.0);
        if (storm.StrikeCount() != before) {
            ++seen;
            CHECK(Near(storm.Lightning(), 1.0, 1e-5) && Near(storm.SecondsSinceStrike(), 0.0));
            // Thunder comes after the flash: 0.3 to 3 km at the speed of sound.
            CHECK(storm.ThunderDelay() >= 300.0 / 343.0 - 1e-6 && storm.ThunderDelay() <= 3000.0 / 343.0 + 1e-6);
            peak = std::max(peak, storm.Lightning());
        }
        if (seen > 0 && storm.SecondsSinceStrike() > 1.5 && storm.SecondsSinceStrike() < 2.0) decayed = decayed || storm.Lightning() < 0.01f; // the flash is long gone
    }
    CHECK(storm.StrikeCount() == seen && seen >= 30 && seen <= 100 && Near(peak, 1.0, 1e-5) && decayed);
    // The same seed strikes the same number of times; the storm leaving ends the strikes.
    WeatherSystem again;
    again.SetSeed(5);
    again.Set(WeatherKind::Storm, 0.0);
    Run(again, 600.0);
    CHECK(again.StrikeCount() == storm.StrikeCount());
    CHECK(storm.Set(WeatherKind::Clear, 0.0));
    const u32 before = storm.StrikeCount();
    Run(storm, 600.0);
    CHECK(storm.StrikeCount() == before);
}

AETHER_TEST(Weather_TheGroundGetsWetInRainAndDriesOutSlowly) {
    WeatherSystem w;
    CHECK(Near(w.Wetness(), 0.0));
    CHECK(w.Set(WeatherKind::Storm, 0.0));
    Run(w, 15.0); // full rain soaks in 30 s: half wet at 15
    CHECK(Near(w.Wetness(), 0.5, 0.03));
    Run(w, 30.0);
    CHECK(Near(w.Wetness(), 1.0, 1e-4)); // held at 1
    CHECK(w.Set(WeatherKind::Clear, 0.0));
    Run(w, 150.0); // dries out over 300 s: half dry at 150
    CHECK(Near(w.Wetness(), 0.5, 0.03));
    Run(w, 200.0);
    CHECK(Near(w.Wetness(), 0.0, 1e-4)); // not below 0
    // Snow wets the ground only a little, and a drizzle too light to count neither wets nor dries.
    WeatherSystem s;
    s.Set(WeatherKind::Snow, 0.0);
    Run(s, 30.0);
    CHECK(s.Wetness() > 0.05f && s.Wetness() < 0.4f);
}

AETHER_TEST(Weather_TheKitAddsItsStageAndAdvancesTheWeather) {
    const std::unique_ptr<kit::IKit> kit = MakeWeatherKit();
    CHECK(std::string(kit->Name()) == "Weather" && kit->Deps().empty());
    const std::vector<kit::KitStage> stages = kit->Stages();
    CHECK(stages.size() == 1 && stages[0].name == "Player.Weather" && stages[0].after == std::vector<std::string>{"Player.Update"});
    WeatherSystem w;
    w.Set(WeatherKind::Rain, 0.0);
    SystemDesc system = MakeWeatherSystem(&w);
    CHECK(system.name == "Player.Weather" && system.run != nullptr);
    World world;
    FrameContext frame;
    frame.dt = 15.0f;
    system.run(world, frame);
    CHECK(Near(w.Time(), 15.0, 0.05) && w.Wetness() > 0.1f);
    MakeWeatherSystem(nullptr).run(world, frame); // no weather: nothing happens, and nothing crashes
    CHECK(Near(w.Time(), 15.0, 0.05));
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer) {
    return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", importer}};
}

} // namespace

// The weather is the game's, not the scene's: it keeps going through a scene change.
AETHER_TEST(Weather_PlayerRunsTheWeatherThroughASceneChange) {
    if (!player::Game::HasScripting()) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_weather_player_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string script_guid = assets::ToString(assets::NewAssetGuid());
    ScriptComponent component;
    assets::AssetGuid parsed;
    CHECK(assets::ParseAssetGuid(script_guid, parsed));
    component.script.guid = parsed;
    const nlohmann::json a = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array({{{"components", {{"ScriptComponent", reflect::ToJson(component)}}}}})}};
    const nlohmann::json b = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array()}};
    const std::string script = "local S = {}\nfunction S:OnUpdate(dt)\n  if not self.asked then self.asked = true; Weather.Set('storm', 0); Scene.Load('Scenes/b.ascene') end\nend\nreturn S\n";
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
    WeatherSystem* weather = WeatherSystem::Active();
    CHECK(weather != nullptr);
    if (!weather) return;
    game.BeginPlay();
    for (int i = 0; i < 4; ++i) game.Tick(1.0f);
    CHECK(game.SceneName() == "Scenes/b.ascene" && game.ScriptErrors().empty());
    CHECK(WeatherSystem::Active() == weather && weather->Current() == WeatherKind::Storm && Near(weather->Rain(), 1.0));
    CHECK(Near(weather->Time(), 4.0, 0.1) && weather->Wetness() > 0.05f); // four seconds of the frame's dt, scene change and all
}

#if AETHER_TEST_HAS_SCRIPTING
AETHER_TEST(Weather_LuauTableActsOnTheWeather) {
    World world;
    WeatherSystem weather;
    script::LuauHost host;
    GuidIndex guids;
    const assets::AssetGuid hero = assets::NewAssetGuid();
    (void)GetComponentId<ScriptComponent>();
    script::ScriptSystem scripts(host, world, guids, [&](const assets::AssetGuid& g, std::string& source, std::string& name) {
        if (g != hero) return false;
        source = "local H = {} function H:Run() self.cur0 = Weather.Current(); self.set = Weather.Set('Rain', 0); self.cur1 = Weather.Current(); self.tgt = Weather.Target(); "
                 "self.rain = Weather.Rain(); self.clouds = Weather.Clouds(); self.snow = Weather.Snow(); self.fog = Weather.Fog(); self.light = Weather.LightScale(); "
                 "self.blend = Weather.Blend(); self.bad_name = Weather.Set('hail'); self.bad_time = Weather.Set('rain', -5); self.slow = Weather.Set('fog'); "
                 "self.blend2 = Weather.Blend(); self.tgt2 = Weather.Target(); "
                 "self.dir_set = Weather.SetWindDirection(90); self.dir = Weather.WindDirection(); self.dir_bad = Weather.SetWindDirection(0/0); "
                 "local v = Weather.WindVector(); self.vx = v.x; self.vz = v.z; self.speed = Weather.WindSpeed(); "
                 "self.dur_set = Weather.SetMeanDuration(120); self.dur_bad = Weather.SetMeanDuration(0); "
                 "Weather.SetCold(true); Weather.SetAuto(true, 11); Weather.SetAuto(false); "
                 "self.strikes = Weather.Strikes(); self.since = Weather.SecondsSinceStrike(); self.thunder = Weather.ThunderDelay(); self.wet = Weather.Wetness(); self.flash = Weather.Lightning() end return H";
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
    CHECK(!host.Run("return Weather.Set(5)").ok && !host.Run("return Weather.Set('rain', 'soon')").ok && !host.Run("return Weather.SetAuto('yes')").ok &&
          !host.Run("return Weather.SetCold(nil)").ok && !host.Run("return Weather.SetWindDirection('north')").ok); // wrong types are script errors
    CHECK(scripts.SendEvent(e, "Run") && scripts.Errors().empty());
    const auto text = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string("<not a string>"); };
    const auto flag = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<bool>(v) && std::get<bool>(v); };
    const auto num = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<f64>(v) ? std::get<f64>(v) : -999.0; };
    CHECK(text("cur0") == "clear" && flag("set") && text("cur1") == "rain" && text("tgt") == "rain");
    CHECK(Near(num("rain"), 0.6, 1e-5) && Near(num("clouds"), 0.9, 1e-5) && Near(num("snow"), 0.0) && Near(num("fog"), 0.3, 1e-5) && Near(num("light"), 0.4, 1e-5) && Near(num("blend"), 1.0));
    CHECK(!flag("bad_name") && !flag("bad_time") && flag("slow") && Near(num("blend2"), 0.0) && text("tgt2") == "fog");
    CHECK(flag("dir_set") && Near(num("dir"), 90.0) && !flag("dir_bad") && Near(num("vz"), 0.0, 1e-3) && num("vx") >= 0.0 && Near(num("vx"), num("speed"), 1e-3));
    CHECK(flag("dur_set") && !flag("dur_bad") && Near(weather.MeanDuration(), 120.0) && weather.IsCold() && !weather.IsAuto() && weather.Seed() == 11);
    CHECK(Near(num("strikes"), 0.0) && num("since") > 1.0e9 && Near(num("thunder"), 0.0) && Near(num("wet"), 0.0) && Near(num("flash"), 0.0));
    life.EndPlay();
}
#endif

#endif
