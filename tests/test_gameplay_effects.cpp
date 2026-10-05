#include "test_framework.h"

#include "aether/assets/asset_guid.h"
#include "aether/ecs/world.h"
#include "aether/gameplay/effect_library.h"
#include "aether/gameplay/effect_system.h"
#include "aether/gameplay/tag_container.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/reflection/registry.h"
#include "aether/scene/components.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>

// Phase 30 step 3a (§30.3): gameplay effects, their modifier math, stacking,
// periodic ticks, tags and JSON.

using namespace aether;
using namespace aether::gas;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {
bool Near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

struct Rig {
    World world;
    AttributeSystem attrs{world};
    EffectLibrary lib;
    EffectSystem fx{world, attrs, lib};
    Entity e;
    Rig() {
        e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
        attrs.Define(e, "Health", 100, 0, 200);
        attrs.Define(e, "Speed", 10);
    }
    void Reg(GameplayEffect g) { std::string err; CHECK(lib.Register(std::move(g), &err)); }
};

GameplayEffect Make(const char* name, GameplayEffect::Duration d, f32 dur, std::vector<GameplayEffect::Modifier> mods) {
    GameplayEffect g;
    g.name = name;
    g.duration_policy = d;
    g.duration = dur;
    g.modifiers = std::move(mods);
    return g;
}
Entity world_entity(player::Game& game) { return game.GetWorld().CreateEntity(Transform{Vec3(), Quaternion::Identity()}); }
using Op = GameplayEffect::Op;
using Dur = GameplayEffect::Duration;
} // namespace

AETHER_TEST(Effect_InstantChangesBaseAndFiresEvents) {
    Rig r;
    r.Reg(Make("Heal", Dur::Instant, 0, {{"Health", Op::Add, -30}}));
    r.Reg(Make("Double", Dur::Instant, 0, {{"Speed", Op::Multiply, 2}}));
    r.Reg(Make("Set", Dur::Instant, 0, {{"Speed", Op::Override, 3}}));
    r.attrs.ClearEvents();
    CHECK(r.fx.Apply(r.e, "Heal").status == EffectSystem::Status::Instant);
    CHECK(Near(r.attrs.GetBase(r.e, "Health"), 70) && Near(r.attrs.Get(r.e, "Health"), 70) && r.attrs.Events().size() == 1);
    r.fx.Apply(r.e, "Double");
    CHECK(Near(r.attrs.GetBase(r.e, "Speed"), 20));
    r.fx.Apply(r.e, "Set");
    CHECK(Near(r.attrs.GetBase(r.e, "Speed"), 3) && r.fx.ActiveCount(r.e) == 0);
    CHECK(r.fx.Apply(r.e, "Nope").status == EffectSystem::Status::Rejected);
}

AETHER_TEST(Effect_TimedModifiesCurrentThenExpires) {
    Rig r;
    r.Reg(Make("Buff", Dur::Timed, 2, {{"Speed", Op::Add, 5}}));
    const auto res = r.fx.Apply(r.e, "Buff");
    CHECK(res.status == EffectSystem::Status::Applied && res.handle != 0);
    CHECK(Near(r.attrs.Get(r.e, "Speed"), 15) && Near(r.attrs.GetBase(r.e, "Speed"), 10)); // base untouched
    r.fx.Update(1.0f);
    CHECK(Near(r.attrs.Get(r.e, "Speed"), 15) && r.fx.HasEffect(r.e, "Buff"));
    r.fx.Update(1.0f);
    CHECK(Near(r.attrs.Get(r.e, "Speed"), 10) && !r.fx.HasEffect(r.e, "Buff"));
    CHECK(r.fx.Events().size() == 2 && r.fx.Events()[0].applied && !r.fx.Events()[1].applied);
}

AETHER_TEST(Effect_InfiniteLastsUntilRemoved) {
    Rig r;
    r.Reg(Make("Aura", Dur::Infinite, 0, {{"Speed", Op::Add, 5}}));
    const auto res = r.fx.Apply(r.e, "Aura");
    r.fx.Update(100.0f);
    CHECK(Near(r.attrs.Get(r.e, "Speed"), 15));
    CHECK(r.fx.Remove(res.handle) && Near(r.attrs.Get(r.e, "Speed"), 10) && !r.fx.Remove(res.handle));
}

AETHER_TEST(Effect_ModifierMathAndClamp) {
    Rig r;
    r.Reg(Make("A", Dur::Infinite, 0, {{"Speed", Op::Add, 5}}));
    r.Reg(Make("B", Dur::Infinite, 0, {{"Speed", Op::Multiply, 2}}));
    r.Reg(Make("O1", Dur::Infinite, 0, {{"Speed", Op::Override, 99}}));
    r.Reg(Make("O2", Dur::Infinite, 0, {{"Speed", Op::Override, 7}}));
    r.fx.Apply(r.e, "A");
    r.fx.Apply(r.e, "B");
    CHECK(Near(r.attrs.Get(r.e, "Speed"), 30)); // (10 + 5) * 2
    r.fx.Apply(r.e, "O1");
    r.fx.Apply(r.e, "O2");
    CHECK(Near(r.attrs.Get(r.e, "Speed"), 7)); // the latest override wins
    r.Reg(Make("Big", Dur::Infinite, 0, {{"Health", Op::Add, 500}}));
    r.fx.Apply(r.e, "Big");
    CHECK(Near(r.attrs.Get(r.e, "Health"), 200) && Near(r.attrs.GetBase(r.e, "Health"), 100)); // clamped current, base kept
}

AETHER_TEST(Effect_Stacking) {
    Rig r;
    GameplayEffect none = Make("None", Dur::Infinite, 0, {{"Speed", Op::Add, 1}});
    GameplayEffect refresh = Make("Refresh", Dur::Timed, 2, {{"Speed", Op::Add, 1}});
    refresh.stacking = GameplayEffect::Stacking::Refresh;
    GameplayEffect stack = Make("Stack", Dur::Infinite, 0, {{"Speed", Op::Add, 2}});
    stack.stacking = GameplayEffect::Stacking::StackCount;
    stack.max_stacks = 3;
    r.Reg(none); r.Reg(refresh); r.Reg(stack);
    CHECK(r.fx.Apply(r.e, "None").status == EffectSystem::Status::Applied && r.fx.Apply(r.e, "None").status == EffectSystem::Status::Rejected && r.fx.ActiveCount(r.e) == 1);
    const auto h1 = r.fx.Apply(r.e, "Refresh");
    r.fx.Update(1.5f);
    const auto h2 = r.fx.Apply(r.e, "Refresh");
    CHECK(h1.handle == h2.handle);
    r.fx.Update(1.5f);
    CHECK(r.fx.HasEffect(r.e, "Refresh")); // refreshed, so still here
    r.fx.Update(1.0f);
    CHECK(!r.fx.HasEffect(r.e, "Refresh"));
    for (int i = 0; i < 5; ++i) r.fx.Apply(r.e, "Stack");
    CHECK(Near(r.attrs.Get(r.e, "Speed"), 10 + 1 + 6)); // None +1, Stack 3 x 2
    // Per-source stacks keep one instance per applier.
    Entity other = r.world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    GameplayEffect ps = Make("PerSource", Dur::Infinite, 0, {{"Speed", Op::Add, 1}});
    ps.stacking = GameplayEffect::Stacking::Refresh;
    ps.per_source = true;
    r.Reg(ps);
    r.fx.Apply(r.e, "PerSource", r.e);
    r.fx.Apply(r.e, "PerSource", other);
    r.fx.Apply(r.e, "PerSource", other);
    CHECK(r.fx.ActiveCount(r.e) == 4); // None, Stack, 2 PerSource
}

AETHER_TEST(Effect_PeriodicPoison) {
    for (const bool on_apply : {false, true}) {
        Rig r;
        GameplayEffect g = Make("Poison", Dur::Timed, 3, {{"Health", Op::Add, -5}});
        g.period = 1.0f;
        g.execute_on_apply = on_apply;
        r.Reg(g);
        r.fx.Apply(r.e, "Poison");
        for (int i = 0; i < 4; ++i) r.fx.Update(1.0f);
        CHECK(Near(r.attrs.Get(r.e, "Health"), on_apply ? 80.0f : 85.0f));
        CHECK(!r.fx.HasEffect(r.e, "Poison"));
    }
    Rig r; // small steps accumulate to the same three ticks
    GameplayEffect g = Make("Poison", Dur::Timed, 3, {{"Health", Op::Add, -5}});
    g.period = 1.0f;
    r.Reg(g);
    r.fx.Apply(r.e, "Poison");
    for (int i = 0; i < 40; ++i) r.fx.Update(0.1f);
    CHECK(Near(r.attrs.Get(r.e, "Health"), 85));
}

AETHER_TEST(Effect_TagRequirementsAndGrantedTags) {
    Rig r;
    GameplayEffect g = Make("Shield", Dur::Timed, 5, {{"Speed", Op::Add, 1}});
    g.require = TagQuery::All({GameplayTag::Make("State.Alive")});
    g.blocked = TagQuery::Any({GameplayTag::Make("State.Stunned")});
    g.granted_tags = {GameplayTag::Make("State.Shielded")};
    r.Reg(g);
    GameplayEffect h = Make("Ward", Dur::Infinite, 0, {});
    h.granted_tags = {GameplayTag::Make("State.Shielded")};
    r.Reg(h);
    CHECK(r.fx.Apply(r.e, "Shield").status == EffectSystem::Status::Rejected); // missing State.Alive
    r.world.AddComponent<TagContainer>(r.e, TagContainer{});
    TagContainer* t = r.world.GetComponent<TagContainer>(r.e);
    t->Add(GameplayTag::Make("State.Alive"));
    t->Add(GameplayTag::Make("State.Stunned"));
    CHECK(r.fx.Apply(r.e, "Shield").status == EffectSystem::Status::Rejected); // blocked
    t->Remove(GameplayTag::Make("State.Stunned"));
    CHECK(r.fx.Apply(r.e, "Shield").status == EffectSystem::Status::Applied);
    const auto ward = r.fx.Apply(r.e, "Ward");
    t = r.world.GetComponent<TagContainer>(r.e);
    CHECK(t->Count(GameplayTag::Make("State.Shielded")) == 2); // ref-counted
    r.fx.Update(5.0f); // the shield expires
    CHECK(t->Count(GameplayTag::Make("State.Shielded")) == 1);
    CHECK(r.fx.RemoveByTag(r.e, GameplayTag::Make("State")) == 1 && t->Count(GameplayTag::Make("State.Shielded")) == 0 && ward.handle != 0);
}

AETHER_TEST(Effect_RemoveOnTags) {
    Rig r;
    GameplayEffect g = Make("Stealth", Dur::Infinite, 0, {{"Speed", Op::Add, 3}});
    g.remove_on_tags = {GameplayTag::Make("State.Attacking")};
    r.Reg(g);
    r.fx.Apply(r.e, "Stealth");
    r.fx.Update(0.1f);
    CHECK(r.fx.HasEffect(r.e, "Stealth"));
    r.world.AddComponent<TagContainer>(r.e, TagContainer{});
    r.world.GetComponent<TagContainer>(r.e)->Add(GameplayTag::Make("State.Attacking"));
    r.fx.Update(0.1f);
    CHECK(!r.fx.HasEffect(r.e, "Stealth") && Near(r.attrs.Get(r.e, "Speed"), 10));
}

AETHER_TEST(Effect_MissingAttributeRejected) {
    Rig r;
    r.Reg(Make("Mana", Dur::Instant, 0, {{"Mana", Op::Add, 5}}));
    CHECK(r.fx.Apply(r.e, "Mana").status == EffectSystem::Status::Rejected);
}

AETHER_TEST(Effect_RebuildAfterReload) {
    Rig r;
    r.Reg(Make("Buff", Dur::Infinite, 0, {{"Speed", Op::Add, 5}}));
    const auto res = r.fx.Apply(r.e, "Buff");
    // A loaded world has the saved modifiers' inputs but not the derived modifiers.
    Attribute* a = r.world.GetComponent<AttributeSet>(r.e)->Find("Speed");
    a->add = 0;
    AttributeSet::RecomputeCurrent(*a);
    CHECK(Near(r.attrs.Get(r.e, "Speed"), 10));
    r.fx.Rebuild();
    CHECK(Near(r.attrs.Get(r.e, "Speed"), 15) && r.fx.Remove(res.handle) && Near(r.attrs.Get(r.e, "Speed"), 10));
}

AETHER_TEST(Effect_JsonRoundTripAndErrors) {
    GameplayEffect g = Make("Burn", Dur::Timed, 4, {{"Health", Op::Add, -2}, {"Speed", Op::Multiply, 0.5f}});
    g.period = 1;
    g.execute_on_apply = true;
    g.stacking = GameplayEffect::Stacking::StackCount;
    g.max_stacks = 3;
    g.per_source = true;
    g.require = TagQuery::All({GameplayTag::Make("State.Alive")});
    g.blocked = TagQuery::Any({GameplayTag::Make("Immune.Fire")});
    g.granted_tags = {GameplayTag::Make("State.Burning")};
    g.remove_on_tags = {GameplayTag::Make("State.Wet")};
    GameplayEffect back;
    std::string err;
    CHECK(EffectFromJson(EffectToJson(g).dump(), back, &err) && back == g);
    const auto fails = [](const char* json, const char* code) {
        GameplayEffect out;
        std::string e;
        return !EffectFromJson(json, out, &e) && e.rfind(code, 0) == 0;
    };
    CHECK(fails("nonsense", "effect.bad_json"));
    CHECK(fails("{}", "effect.name_empty"));
    CHECK(fails(R"({"name":"x","duration_policy":"timed"})", "effect.bad_duration"));
    CHECK(fails(R"({"name":"x","duration_policy":"later"})", "effect.bad_duration"));
    CHECK(fails(R"({"name":"x","modifiers":[{"attribute":"a","op":"pow","magnitude":1}]})", "effect.bad_op"));
    CHECK(fails(R"({"name":"x","modifiers":[{"attribute":"a"}]})", "effect.bad_modifier"));
    CHECK(fails(R"({"name":"x","period":1})", "effect.period_on_instant"));
    CHECK(fails(R"({"name":"x","stacking":"stack","max_stacks":0})", "effect.bad_stack"));
    CHECK(fails(R"({"name":"x","granted_tags":["bad tag!"]})", "effect.bad_tag"));
    CHECK(fails(R"({"name":"x","require":{"op":"xor"}})", "effect.bad_tag"));
}

AETHER_TEST(Effect_BlueprintLibraryActsOnTheActiveSystem) {
    const reflect::TypeInfo* type = reflect::TypeRegistry::Find("Effects");
    CHECK(type != nullptr && type->functions.size() == 5);
    World world;
    const Entity e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    CHECK(EffectSystem::Active() == nullptr);
    CHECK(Effects::ApplyEffect(e, "Buff", kNullEntity) == 0 && !Effects::RemoveEffect(1) && !Effects::HasActiveEffect(e, "Buff") && Effects::GetActiveEffectCount(e) == 0);
    Rig r;
    r.Reg(Make("Buff", Dur::Infinite, 0, {{"Speed", Op::Add, 5}}));
    CHECK(EffectSystem::Active() == &r.fx);
    const i32 h = Effects::ApplyEffect(r.e, "Buff", kNullEntity);
    CHECK(h > 0 && Effects::HasActiveEffect(r.e, "Buff") && Effects::GetActiveEffectCount(r.e) == 1 && Near(r.attrs.Get(r.e, "Speed"), 15));
    CHECK(Effects::RemoveEffect(h) && Effects::GetActiveEffectCount(r.e) == 0 && !Effects::RemoveEffect(h));
}

AETHER_TEST(Effect_PlayerLoadsAssetsAndTicksThem) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_gameplay_effect_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array()}};
    const auto asset = [](const char* path, const char* importer) {
        return nlohmann::json{{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", importer}};
    };
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()},
                                     {"assets", nlohmann::json::array({asset("Scenes/start.ascene", "Scene"), asset("Effects/slow.aeffect", "GameplayEffect"), asset("Effects/bad.aeffect", "GameplayEffect")})},
                                     {"files", nlohmann::json::array()}};
    const nlohmann::json slow = {{"name", "Slow"}, {"duration_policy", "timed"}, {"duration", 0.1},
                                 {"modifiers", nlohmann::json::array({{{"attribute", "Speed"}, {"op", "multiply"}, {"magnitude", 0.5}}})}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump(2));
    writer.Add("Content/Scenes/start.ascene", scene.dump(2));
    writer.Add("Content/Effects/slow.aeffect", slow.dump(2));
    writer.Add("Content/Effects/bad.aeffect", std::string("{\"name\":\"\"}"));
    const std::filesystem::path file = dir / "Game.apak";
    std::string error;
    CHECK(writer.Write(file.string(), &error));
    player::GamePackage package;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    CHECK(game.LoadStartupScene(&error));
    bool warned = false;
    for (const std::string& w : game.Warnings()) warned = warned || (w.find("bad.aeffect") != std::string::npos && w.find("effect.name_empty") != std::string::npos);
    CHECK(warned); // the broken asset is reported, the good one loads
    EffectSystem* fx = EffectSystem::Active();
    CHECK(fx != nullptr && AttributeSystem::Active() != nullptr);
    if (!fx) return;
    AttributeSystem* attrs = AttributeSystem::Active();
    const Entity e = world_entity(game);
    CHECK(attrs->Define(e, "Speed", 10));
    game.BeginPlay();
    CHECK(fx->Apply(e, "Slow").status == EffectSystem::Status::Applied && Near(attrs->Get(e, "Speed"), 5));
    game.Tick(0.016f);
    CHECK(fx->Events().empty() && Near(attrs->Get(e, "Speed"), 5)); // Player.Effects drained the applied event
    for (int i = 0; i < 8; ++i) game.Tick(0.016f);
    CHECK(!fx->HasEffect(e, "Slow") && Near(attrs->Get(e, "Speed"), 10)); // expired by the player's own tick
}
