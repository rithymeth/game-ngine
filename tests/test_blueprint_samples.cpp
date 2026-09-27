#include "aether/blueprint/system.h"
#include "aether/assets/asset_guid.h"
#include "aether/ecs/world.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/components.h"
#include "test_framework.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>

using namespace aether;
using namespace aether::bp;
using nlohmann::json;

// Phase 12 step 7: the sample Blueprints in assets/blueprints (BP_Door,
// BP_Coin, BP_GameMode, BP_Spinner) load, validate cleanly, and play their
// parts, and 10,000 BP_Spinner instances tick within budget.

namespace {

const char* const kSamples[] = {"BP_Door", "BP_Coin", "BP_GameMode", "BP_Spinner"};

std::string SamplePath(const std::string& name) { return std::string(AETHER_REPO_ASSETS_DIR) + "/blueprints/" + name + ".abp"; }

Blueprint LoadSample(const std::string& name) {
    Blueprint bp;
    std::string error;
    if (!LoadBlueprint(SamplePath(name), bp, &error)) std::printf("    %s: %s\n", name.c_str(), error.c_str());
    return bp;
}

// A scene: the samples as assets, a lifecycle, and a BlueprintSystem.
struct Scene {
    World world;
    GuidIndex guids;
    Lifecycle lifecycle{world, guids};
    BlueprintSystem system{world};
    std::map<std::string, assets::AssetGuid> assets;
    std::map<assets::AssetGuid, std::string> names;
    std::vector<std::string> printed;

    Scene() {
        RegisterBlueprintComponents();
        for (const char* name : kSamples) {
            const assets::AssetGuid guid = assets::NewAssetGuid();
            assets[name] = guid;
            names[guid] = name;
        }
        system.SetLoader([this](const assets::AssetGuid& guid, Blueprint& out, std::string& name) {
            auto it = names.find(guid);
            if (it == names.end()) return false;
            name = it->second;
            out = LoadSample(name);
            return true;
        });
        system.Register(lifecycle);
        system.VM().SetGuidIndex(&guids);
        system.VM().SetPrintHandler([this](Entity, const std::string& text) { printed.push_back(text); });
    }
    Entity Spawn(const std::string& blueprint, const std::vector<std::string>& tags = {}, json overrides = json::object()) {
        const Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, Transform{});
        guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
        if (!blueprint.empty()) {
            BlueprintInstance instance;
            instance.blueprint.guid = assets.at(blueprint);
            for (auto& [key, value] : overrides.items()) instance.SetOverride(key, value);
            world.AddComponent(e, instance);
        }
        for (const std::string& tag : tags) AddTag(world, e, tag);
        return e;
    }
    void Frame(f32 dt = 1.0f / 60.0f) {
        lifecycle.Update(dt);
        system.Update(dt);
    }
    VmValue Var(Entity e, const char* name) { return system.VM().GetVariable(e, name); }
};

f32 AsFloat(const VmValue& v) { return std::holds_alternative<f32>(v) ? std::get<f32>(v) : -1.0f; }
i32 AsInt(const VmValue& v) { return std::holds_alternative<i32>(v) ? std::get<i32>(v) : -1; }
bool AsBool(const VmValue& v) { return std::holds_alternative<bool>(v) && std::get<bool>(v); }

} // namespace

AETHER_TEST(BlueprintSamples_LoadValidateAndRoundTrip) {
    for (const char* name : kSamples) {
        const Blueprint bp = LoadSample(name);
        AETHER_CHECK(!bp.graphs.empty());
        const ValidationResult v = ValidateBlueprint(bp);
        for (const Diagnostic& d : v.diagnostics) {
            std::printf("    %s %s node %u: %s\n", name, d.code.c_str(), d.node, d.message.c_str());
        }
        AETHER_CHECK(v.errors == 0 && v.warnings == 0);
        AETHER_CHECK(CompileBlueprint(bp).Ok());
        AETHER_CHECK(!bp.graphs[0].comments.empty()); // each is commented, as the editor saves it

        // The files are in the editor's saved form: loading and saving gives the same JSON.
        std::ifstream file(SamplePath(name));
        const json on_disk = json::parse(file, nullptr, false);
        AETHER_CHECK(BlueprintToJson(bp) == on_disk);
    }
}

AETHER_TEST(BlueprintSamples_DoorOpensForThePlayerOnly) {
    Scene s;
    const Entity door = s.Spawn("BP_Door", {}, {{"OpenAngle", 90.0}, {"Speed", 90.0}});
    const Entity player = s.Spawn("", {"Player"}), crate = s.Spawn("", {"Crate"});
    s.lifecycle.BeginPlay();
    s.Frame();
    AETHER_CHECK(s.system.CompileErrors().empty());

    // Something that isn't the player doesn't open it.
    const VmValue crate_arg[] = {crate};
    AETHER_CHECK(s.system.VM().Dispatch(door, "Event.OnTriggerEnter", crate_arg));
    AETHER_CHECK(!AsBool(s.Var(door, "IsOpen")));

    // The player does: it swings at 90 degrees per second, up to 90.
    const VmValue player_arg[] = {player};
    AETHER_CHECK(s.system.VM().Dispatch(door, "Event.OnTriggerEnter", player_arg));
    AETHER_CHECK(AsBool(s.Var(door, "IsOpen")));
    for (int i = 0; i < 30; ++i) s.Frame();
    AETHER_CHECK(std::fabs(AsFloat(s.Var(door, "Angle")) - 45.0f) < 0.1f);
    for (int i = 0; i < 60; ++i) s.Frame();
    AETHER_CHECK(AsFloat(s.Var(door, "Angle")) == 90.0f);
    const Quaternion open = s.world.GetComponent<Transform>(door)->rotation;
    const Quaternion expected = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 3.14159265f / 2.0f);
    AETHER_CHECK(std::fabs(open.x - expected.x) + std::fabs(open.y - expected.y) + std::fabs(open.z - expected.z) +
                     std::fabs(open.w - expected.w) < 1e-3f);

    // Leaving closes it, back to 0 and no further.
    AETHER_CHECK(s.system.VM().Dispatch(door, "Event.OnTriggerExit", player_arg));
    for (int i = 0; i < 120; ++i) s.Frame();
    AETHER_CHECK(!AsBool(s.Var(door, "IsOpen")) && AsFloat(s.Var(door, "Angle")) == 0.0f);
    AETHER_CHECK(s.system.VM().Errors().empty());
}

AETHER_TEST(BlueprintSamples_CoinsScoreUntilTheGameModeSaysYouWin) {
    Scene s;
    const Entity mode = s.Spawn("BP_GameMode");
    std::vector<Entity> coins;
    for (int i = 0; i < 11; ++i) coins.push_back(s.Spawn("BP_Coin", {"Coin"}));
    const Entity big = s.Spawn("BP_Coin", {"Coin"}, {{"Value", 5}});
    const Entity player = s.Spawn("", {"Player"}), crate = s.Spawn("", {"Crate"});
    s.lifecycle.BeginPlay();
    s.Frame();
    AETHER_CHECK(s.system.CompileErrors().empty() && s.printed.empty());

    const VmValue player_arg[] = {player}, crate_arg[] = {crate};
    AETHER_CHECK(s.system.VM().Dispatch(coins[0], "Event.OnTriggerEnter", crate_arg)); // not collected
    AETHER_CHECK(s.world.IsAlive(coins[0]) && AsInt(s.Var(mode, "Score")) == 0);

    // Nine coins: the score counts, each coin goes away, no win yet.
    for (int i = 0; i < 9; ++i) {
        AETHER_CHECK(s.system.VM().Dispatch(coins[static_cast<usize>(i)], "Event.OnTriggerEnter", player_arg));
        AETHER_CHECK(!s.world.IsAlive(coins[static_cast<usize>(i)]));
    }
    AETHER_CHECK(AsInt(s.Var(mode, "Score")) == 9);
    AETHER_CHECK(s.printed.size() == 9 && s.printed.front() == "Score: 1" && s.printed.back() == "Score: 9");
    AETHER_CHECK(!s.system.VM().Dispatch(coins[0], "Event.OnTriggerEnter", player_arg)); // gone

    // The tenth wins, once; more coins keep scoring.
    AETHER_CHECK(s.system.VM().Dispatch(coins[9], "Event.OnTriggerEnter", player_arg));
    AETHER_CHECK(s.printed.size() == 11 && s.printed[9] == "Score: 10" && s.printed[10] == "You win");
    AETHER_CHECK(s.system.VM().Dispatch(big, "Event.OnTriggerEnter", player_arg));
    AETHER_CHECK(AsInt(s.Var(mode, "Score")) == 15 && s.printed.back() == "Score: 15");
    AETHER_CHECK(std::count(s.printed.begin(), s.printed.end(), "You win") == 1);
    AETHER_CHECK(s.world.IsAlive(coins[10]));
    s.Frame();
    AETHER_CHECK(s.system.VM().Errors().empty());
}

AETHER_TEST(BlueprintSamples_TenThousandSpinnersTick) {
    Scene s;
    constexpr int kCount = 10'000;
    std::vector<Entity> spinners;
    spinners.reserve(kCount);
    for (int i = 0; i < kCount; ++i) spinners.push_back(s.Spawn("BP_Spinner"));
    s.lifecycle.BeginPlay();
    s.Frame();
    AETHER_CHECK(s.system.VM().InstanceCount() == kCount && s.system.CompileErrors().empty());
    AETHER_CHECK(LoadSample("BP_Spinner").graphs[0].nodes.size() == 20);

    constexpr int kFrames = 60;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) s.system.Update(1.0f / 30.0f); // Tick only: what the benchmark measures
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kFrames;
    std::printf("    10,000 BP_Spinner instances (20-node Tick): %.3f ms per frame\n", ms);

    // 61 ticks at 180 deg/s: 360 degrees after 60, so one lap each.
    for (Entity e : {spinners.front(), spinners[kCount / 2], spinners.back()}) {
        AETHER_CHECK(AsInt(s.Var(e, "Laps")) == 1);
        AETHER_CHECK(AsFloat(s.Var(e, "Angle")) < 12.0f);
        AETHER_CHECK(std::fabs(s.world.GetComponent<Transform>(e)->position.y) <= 0.25f + 1e-4f);
    }
    AETHER_CHECK(s.system.VM().Errors().empty());
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    // The ROADMAP target is 2 ms on the target machine; CI machines vary,
    // so the test only catches large regressions. aether_bp_bench measures.
    AETHER_CHECK(ms < 20.0);
#endif
}
