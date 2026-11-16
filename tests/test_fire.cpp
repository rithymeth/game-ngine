#include "test_framework.h"

#ifdef AETHER_TEST_HAS_FIRE

#include "aether/assets/asset_guid.h"
#include "aether/ecs/world.h"
#include "aether/fire/fire.h"
#include "aether/fire/fire_kit.h"
#include "aether/fire/fire_system.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/player/user_paths.h"
#include "aether/renderer/components.h"
#include "aether/save/save_system.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/vfx/particle_system.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>

// The fire kit: a Flammable catches from a burning neighbour (nearer sooner), burns on its fuel, is put out by wetness, burns away or is consumed, and carries a
// flame and a light while it burns. Checked on a bare world, then in the player with a script.

using namespace aether;
using namespace aether::fire;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

constexpr f32 kDt = 1.0f / 30.0f;

bool Near(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }

struct Sim {
    World world;
    FireSystem system{world};

    Sim() { RegisterFireComponents(); }
    Entity Thing(const Vec3& at, const Flammable& f = Flammable{}) { return world.CreateEntity(Transform{at, Quaternion::Identity()}, f); }
    void Run(f32 seconds) {
        for (f32 t = 0.0f; t < seconds; t += kDt) system.Advance(kDt);
    }
    // The seconds until it is alight, or -1 if it is not by `limit`.
    f32 TimeToBurn(Entity e, f32 limit) {
        for (f32 t = 0.0f; t < limit; t += kDt) {
            if (system.IsBurning(e)) return t;
            system.Advance(kDt);
        }
        return system.IsBurning(e) ? limit : -1.0f;
    }
};

Flammable Quick(f32 fuel = 4.0f) {
    Flammable f;
    f.fuel = fuel;
    f.light_intensity = 0.0f; // no flame entities unless a test asks
    return f;
}

} // namespace

AETHER_TEST(Fire_ComponentRegistersAndRoundTrips) {
    RegisterFireComponents();
    CHECK(FindComponentIdByName("Flammable") != kInvalidComponentId);
    Flammable f;
    f.fuel = 3.5f;
    f.consume = true;
    f.wetness = 0.25f;
    f.light_intensity = 33.0f;
    std::strncpy(f.flame_effect, "Fx/Flame.avfx", sizeof(f.flame_effect) - 1);
    Flammable back;
    CHECK(reflect::FromJson(back, reflect::ToJson(f)) && Near(back.fuel, 3.5f, 1e-5f) && back.consume && Near(back.wetness, 0.25f, 1e-6f) &&
          Near(back.light_intensity, 33.0f, 1e-4f) && std::string(back.flame_effect) == "Fx/Flame.avfx");
}

AETHER_TEST(Fire_ALitThingBurnsItsFuelAndThenStaysBurntOrIsConsumed) {
    Sim s;
    const Entity kept = s.Thing(Vec3(0, 0, 0), Quick(4.0f));
    Flammable gone = Quick(4.0f);
    gone.consume = true;
    const Entity eaten = s.Thing(Vec3(50, 0, 0), gone);
    CHECK(s.system.IsBurning(kept) == false && Near(s.system.FuelLeft(kept), 4.0f, 1e-5f));
    CHECK(s.system.Ignite(kept) && s.system.Ignite(eaten));
    CHECK(!s.system.Ignite(kept)); // already burning
    CHECK(s.system.BurningCount() == 2 && s.system.IsBurning(kept));
    s.Run(2.0f);
    CHECK(s.system.IsBurning(kept) && Near(s.system.FuelLeft(kept), 2.0f, 0.1f));
    s.Run(2.5f);
    CHECK(!s.system.IsBurning(kept) && s.system.IsBurnt(kept) && s.system.FuelLeft(kept) == 0.0f);
    CHECK(s.world.IsAlive(kept));    // burnt, but still there
    CHECK(!s.world.IsAlive(eaten));  // consumed
    CHECK(!s.system.Ignite(kept));   // nothing left to burn
    CHECK(s.system.BurningCount() == 0);
}

AETHER_TEST(Fire_FireSpreadsToWhatIsNearAndNearerCatchesSooner) {
    Sim s;
    Flammable f = Quick(60.0f);
    const Entity source = s.Thing(Vec3(0, 0, 0), f);
    const Entity close = s.Thing(Vec3(1.0f, 0, 0), f);
    const Entity mid = s.Thing(Vec3(2.0f, 0, 0), f);
    const Entity far_one = s.Thing(Vec3(2.9f, 0, 0), f);
    const Entity out_of_reach = s.Thing(Vec3(20.0f, 0, 0), f);
    CHECK(s.system.Ignite(source));
    f32 t_close = -1, t_mid = -1, t_far = -1;
    for (f32 t = 0.0f; t < 40.0f; t += kDt) {
        s.system.Advance(kDt);
        if (t_close < 0 && s.system.IsBurning(close)) t_close = t;
        if (t_mid < 0 && s.system.IsBurning(mid)) t_mid = t;
        if (t_far < 0 && s.system.IsBurning(far_one)) t_far = t;
    }
    CHECK(t_close > 0 && t_mid > 0 && t_far > 0);
    CHECK(t_close < t_mid && t_mid < t_far); // nearer, sooner
    CHECK(!s.system.IsBurning(out_of_reach) && s.system.Heat(out_of_reach) == 0.0f);
}

AETHER_TEST(Fire_HeatFadesWhenTheFireGoesOutBeforeItCatches) {
    Sim s;
    const Entity source = s.Thing(Vec3(0, 0, 0), Quick(60.0f));
    const Entity neighbour = s.Thing(Vec3(1.5f, 0, 0), Quick(60.0f));
    CHECK(s.system.Ignite(source));
    s.Run(1.5f);
    CHECK(!s.system.IsBurning(neighbour) && s.system.Heat(neighbour) > 0.2f);
    CHECK(s.system.Extinguish(source));
    CHECK(!s.system.Extinguish(source)); // already out
    const f32 hot = s.system.Heat(neighbour);
    s.Run(1.0f);
    CHECK(s.system.Heat(neighbour) < hot);
    s.Run(6.0f);
    CHECK(s.system.Heat(neighbour) == 0.0f && !s.system.IsBurning(neighbour));
}

AETHER_TEST(Fire_WetnessSlowsCatchingAndPutsABurningThingOut) {
    Sim s;
    Flammable dry = Quick(60.0f), damp = Quick(60.0f), soaked = Quick(60.0f);
    damp.wetness = 0.5f;
    soaked.wetness = 0.9f;
    const Entity source = s.Thing(Vec3(0, 0, 0), Quick(60.0f));
    const Entity a = s.Thing(Vec3(1.0f, 0, 0), dry);
    const Entity b = s.Thing(Vec3(-1.0f, 0, 0), damp);
    const Entity c = s.Thing(Vec3(0, 0, 1.0f), soaked);
    CHECK(s.system.Ignite(source));
    f32 ta = -1, tb = -1;
    for (f32 t = 0.0f; t < 40.0f; t += kDt) {
        s.system.Advance(kDt);
        if (ta < 0 && s.system.IsBurning(a)) ta = t;
        if (tb < 0 && s.system.IsBurning(b)) tb = t;
    }
    CHECK(ta > 0 && tb > ta);                  // the damp one catches, later
    CHECK(!s.system.IsBurning(c) && s.system.Heat(c) == 0.0f); // the soaked one never does
    CHECK(!s.system.Ignite(c));                // not even by hand

    // A burning thing that is wetted goes out, and keeps the fuel it had.
    CHECK(s.system.IsBurning(a));
    const f32 before = s.system.FuelLeft(a);
    CHECK(s.system.SetWetness(a, 0.8f));
    s.Run(0.2f);
    CHECK(!s.system.IsBurning(a) && !s.system.IsBurnt(a));
    CHECK(s.system.FuelLeft(a) <= before && s.system.FuelLeft(a) > 0.0f); // the fuel it had is kept
    // WetAll wets everything, and dries it again.
    CHECK(s.system.WetAll(1.0f) == 4);
    CHECK(!s.system.Ignite(a));
    CHECK(s.system.WetAll(0.0f) == 4 && s.system.Ignite(a)); // dry, it can catch again
}

AETHER_TEST(Fire_ABurningThingCarriesAFlameAndALightThatFollowItAndFadeAway) {
    Sim s;
    Flammable f = Quick(60.0f);
    std::strncpy(f.flame_effect, "Fx/Flame.avfx", sizeof(f.flame_effect) - 1);
    f.light_intensity = 20.0f;
    f.flame_offset = Vec3(0, 0.5f, 0);
    const Entity thing = s.Thing(Vec3(2, 1, 3), f);
    CHECK(s.system.FlameOf(thing).IsNull());
    CHECK(s.system.Ignite(thing));
    const Entity flame = s.system.FlameOf(thing);
    CHECK(!flame.IsNull() && s.world.IsAlive(flame));
    if (flame.IsNull()) return;
    const ParticleSystem* ps = s.world.GetComponent<ParticleSystem>(flame);
    const PointLight* light = s.world.GetComponent<PointLight>(flame);
    CHECK(ps != nullptr && ps->asset == "Fx/Flame.avfx" && ps->auto_activate);
    CHECK(light != nullptr && Near(light->intensity, 20.0f, 1e-4f) && !light->cast_shadows);
    CHECK(Near(s.world.GetComponent<Transform>(flame)->position.y, 1.5f, 1e-4f));
    // It follows the thing.
    s.world.GetComponent<Transform>(thing)->position = Vec3(10, 1, 3);
    s.Run(0.1f);
    CHECK(Near(s.world.GetComponent<Transform>(flame)->position.x, 10.0f, 1e-4f));
    // Put out: the particles are told to stop, the light goes, and the entity is let go a few seconds later.
    CHECK(s.system.Extinguish(thing));
    CHECK(s.system.FlameOf(thing).IsNull() && s.world.IsAlive(flame));
    CHECK(s.world.GetComponent<PointLight>(flame)->intensity == 0.0f);
    s.Run(1.0f);
    CHECK(s.world.IsAlive(flame));
    s.Run(3.0f);
    CHECK(!s.world.IsAlive(flame));
    // A thing that asks for neither effect has no flame entity at all.
    const Entity plain = s.Thing(Vec3(30, 0, 0), Quick(5.0f));
    CHECK(s.system.Ignite(plain) && s.system.FlameOf(plain).IsNull() && s.system.IsBurning(plain));
}

AETHER_TEST(Fire_IgniteNearAndTheRefusals) {
    Sim s;
    const Entity a = s.Thing(Vec3(0, 0, 0), Quick(30.0f));
    const Entity b = s.Thing(Vec3(2, 0, 0), Quick(30.0f));
    const Entity c = s.Thing(Vec3(9, 0, 0), Quick(30.0f));
    Flammable off = Quick(30.0f);
    off.enabled = false;
    const Entity d = s.Thing(Vec3(1, 0, 0), off);
    CHECK(s.system.IgniteNear(Vec3(0, 0, 0), 3.0f) == 2); // a and b; not the far one or the disabled one
    CHECK(s.system.IsBurning(a) && s.system.IsBurning(b) && !s.system.IsBurning(c) && !s.system.IsBurning(d));
    CHECK(s.system.IgniteNear(Vec3(0, 0, 0), 3.0f) == 0); // already alight
    CHECK(s.system.IgniteNear(Vec3(std::numeric_limits<f32>::quiet_NaN(), 0, 0), 3.0f) == 0);
    CHECK(s.system.IgniteNear(Vec3(0, 0, 0), -1.0f) == 0);
    const Entity plain = s.world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()});
    CHECK(!s.system.Ignite(plain) && !s.system.Ignite(Entity{}) && !s.system.Extinguish(plain) && !s.system.IsBurning(plain));
    CHECK(!s.system.SetWetness(plain, 0.5f) && !s.system.SetWetness(a, std::numeric_limits<f32>::quiet_NaN()));
    CHECK(s.system.WetAll(std::numeric_limits<f32>::quiet_NaN()) == 0);
    CHECK(s.system.SetWetness(a, 7.0f) && s.world.GetComponent<Flammable>(a)->wetness == 1.0f); // held to 1
    s.system.Advance(std::numeric_limits<f32>::quiet_NaN()); // a step that is not a number changes nothing
    CHECK(s.system.Advance(-1.0f) == s.system.BurningCount());
}

AETHER_TEST(Fire_ADisabledOrDestroyedThingIsForgottenAndARunRepeats) {
    Sim s;
    Flammable f = Quick(30.0f);
    f.light_intensity = 10.0f;
    const Entity a = s.Thing(Vec3(0, 0, 0), f);
    CHECK(s.system.Ignite(a));
    const Entity flame = s.system.FlameOf(a);
    s.world.GetComponent<Flammable>(a)->enabled = false;
    s.Run(0.2f);
    CHECK(!s.system.IsBurning(a) && s.system.BurningCount() == 0);
    s.Run(4.0f);
    CHECK(!s.world.IsAlive(flame)); // its flame went with it
    // The same scene twice gives the same fire.
    const auto run = [] {
        Sim t;
        std::vector<Entity> row;
        for (int i = 0; i < 8; ++i) row.push_back(t.Thing(Vec3(static_cast<f32>(i) * 1.3f, 0, static_cast<f32>(i % 3)), Quick(6.0f)));
        t.system.Ignite(row[0]);
        std::vector<f32> heat;
        for (int k = 0; k < 200; ++k) t.system.Advance(kDt);
        for (const Entity e : row) heat.push_back(t.system.Heat(e) + (t.system.IsBurnt(e) ? 100.0f : 0.0f) + (t.system.IsBurning(e) ? 10.0f : 0.0f));
        return heat;
    };
    CHECK(run() == run());
}

AETHER_TEST(Fire_TheKitRegistersTheComponentAndHoldsTheSystemInARuntime) {
    const std::unique_ptr<kit::IKit> kit = MakeFireKit();
    CHECK(std::string(kit->Name()) == "Fire" && kit->Deps().empty());
    const std::vector<kit::KitStage> stages = kit->Stages();
    CHECK(stages.size() == 1 && stages[0].name == "Player.Fire" && stages[0].after.size() == 1 && stages[0].after[0] == "Player.Update");
    World world;
    kit::KitRuntimeContext context;
    CHECK(kit->CreateRuntime(context) == nullptr); // no world, no runtime
    context.world = &world;
    std::unique_ptr<kit::IKitRuntime> runtime = kit->CreateRuntime(context);
    CHECK(runtime != nullptr);
    if (!runtime) return;
    kit->RegisterComponents();
    CHECK(FindComponentIdByName("Flammable") != kInvalidComponentId);
    FireSystem* system = static_cast<FireSystem*>(runtime->Find("FireSystem"));
    CHECK(system != nullptr && runtime->Find("Nothing") == nullptr && FireSystem::Active() == system);
    std::string installed;
    kit::KitScriptApiContext api;
    api.install = [&](const std::string& name) { installed = name; };
    kit->InstallScriptApi(&api);
    CHECK(installed == "Fire");
    // The frame stage advances it.
    Flammable f = Quick(3.0f);
    const Entity e = world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, f);
    CHECK(system->Ignite(e));
    const SystemDesc desc = MakeFireSystem(system);
    CHECK(desc.name == "Player.Fire" && desc.main_thread_only && desc.run);
    FrameContext frame;
    frame.dt = 1.0f;
    for (int i = 0; i < 4; ++i) desc.run(world, frame);
    CHECK(system->IsBurnt(e));
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer) {
    return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", importer}};
}

} // namespace

#if AETHER_TEST_HAS_SCRIPTING
// The real player: two flammables in a scene, a script lights one (Fire.Ignite), the fire spreads to the other through the frame's Fire stage, and the script
// reads it (IsBurning, Burning, FuelLeft), then wets everything (WetAll) and sees the fires go out.
AETHER_TEST(Fire_PlayerSpreadsFireAndAScriptLightsAndWetsIt) {
    if (!player::Game::HasScripting()) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_fire_player_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string script_guid = assets::ToString(assets::NewAssetGuid());
    ScriptComponent component;
    assets::AssetGuid parsed;
    CHECK(assets::ParseAssetGuid(script_guid, parsed));
    component.script.guid = parsed;
    Flammable f;
    f.fuel = 60.0f;
    f.light_intensity = 0.0f;
    const auto at = [](f32 x, f32 y, f32 z) { return reflect::ToJson(Transform{Vec3(x, y, z), Quaternion::Identity()}); };
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array({
        {{"components", {{"Transform", at(0, 0, 0)}, {"Flammable", reflect::ToJson(f)}, {"ScriptComponent", reflect::ToJson(component)}}}},
        {{"components", {{"Transform", at(1, 0, 0)}, {"Flammable", reflect::ToJson(f)}}}}})}};
    const std::string script =
        "local S = {}\nfunction S:OnCreate()\n  self.t = 0\nend\n"
        "function S:OnUpdate(dt)\n  self.t = self.t + dt\n"
        "  if self.t > 0.2 and not self.lit then\n    self.lit = true\n    SaveGames.SetNumber('lit', Fire.Ignite(self.entity) and 1 or 0)\n  end\n"
        "  SaveGames.SetNumber('burning', Fire.Burning())\n"
        "  SaveGames.SetNumber('self', Fire.IsBurning(self.entity) and 1 or 0)\n"
        "  SaveGames.SetNumber('fuel', Fire.FuelLeft(self.entity))\n"
        "  if self.t > 12 and not self.wet then\n    self.wet = true\n    SaveGames.SetNumber('burning_before_rain', Fire.Burning())\n    SaveGames.SetNumber('wetted', Fire.WetAll(1.0))\n  end\nend\nreturn S\n";
    const nlohmann::json assets_json = nlohmann::json::array({AssetEntry("Scenes/start.ascene", "Scene"), {{"guid", script_guid}, {"path", "Scripts/Fire.luau"}, {"importer", "Script"}}});
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()}, {"assets", assets_json}, {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    CHECK(writer.Add("Manifest.json", manifest.dump(2)));
    CHECK(writer.Add("Content/Scenes/start.ascene", scene.dump()));
    CHECK(writer.Add("Content/Scripts/Fire.luau", script));
    const std::filesystem::path file = dir / "Game.apak";
    CHECK(writer.Write(file.string()));
    player::GamePackage package;
    std::string error;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    game.SetUserPaths(player::ResolveUserPaths("Demo", dir / "user"));
    CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    CHECK(game.Saves() != nullptr);
    if (!game.Saves()) return;
    for (int i = 0; i < 60 * 11; ++i) game.Tick(1.0f / 60.0f);
    CHECK(game.ScriptErrors().empty());
    const save::SaveBag& bag = game.Saves()->Bag();
    CHECK(bag.GetInt("lit") == 1 && bag.GetInt("self") == 1);
    CHECK(bag.GetInt("burning") == 2);                              // it spread to the one a metre away
    CHECK(bag.GetFloat("fuel") < 60.0f && bag.GetFloat("fuel") > 40.0f);
    for (int i = 0; i < 60 * 3; ++i) game.Tick(1.0f / 60.0f);
    CHECK(game.ScriptErrors().empty());
    CHECK(bag.GetInt("burning_before_rain") == 2 && bag.GetInt("wetted") == 2);
    CHECK(bag.GetInt("burning") == 0 && bag.GetInt("self") == 0);   // the rain put both out
}
#endif

#endif
