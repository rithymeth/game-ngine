#include "test_framework.h"

#include "aether/assets/asset_guid.h"
#include "aether/gameplay/ability_system.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/scene/components.h"
#include "aether/script/script_system.h"

// Phase 30 step 5 (§30.5): the Luau tables for tags, attributes, effects and
// abilities, and the methods a script hears.

#if AETHER_TEST_HAS_SCRIPTING

#include <nlohmann/json.hpp>

#include <filesystem>

using namespace aether;
using namespace aether::gas;
using namespace aether::script;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

const char* kHero = R"(
local Hero = {}
Hero.heard = 0
function Hero:Run()
    local e = self.entity
    self.defined = Attributes.Define(e, 'Health', 100, 0, 100)
    Attributes.AddBase(e, 'Health', -30)
    self.hp = Attributes.Get(e, 'Health', -1)
    self.max = Attributes.GetMax(e, 'Health', -1)
    self.has = Attributes.Has(e, 'Health') and not Attributes.Has(e, 'Mana')
    self.tag = GameplayTags.Matches('Damage.Fire.Burning', 'Damage.Fire') and not GameplayTags.MatchesExact('Damage.Fire.Burning', 'Damage.Fire')
    self.depth = GameplayTags.GetDepth('A.B.C')
    self.effect = Effects.Apply(e, 'Buff', e)
    self.buffed = Attributes.Get(e, 'Health', -1)
    self.effect_active = Effects.HasActive(e, 'Buff')
    self.effect_count = Effects.GetActiveCount(e)
    self.granted = Abilities.Grant(e, 'Dash')
    self.can = Abilities.CanActivate(e, 'Dash')
    self.handle = Abilities.TryActivate(e, 'Dash')
    self.active = Abilities.IsActive(e, 'Dash')
    self.ended = Abilities.End(self.handle)
    self.removed = Effects.Remove(self.effect)
    self.after = Attributes.Get(e, 'Health', -1)
end
function Hero:OnAttributeChanged(name, old, new) self.heard += 1; self.last = name end
function Hero:OnAbilityActivated(ability, handle) self.activated = ability end
return Hero
)";

// Counts what it hears in attributes, which the test reads back through the player's attribute system.
const char* kListener = R"(
local Listener = {}
function Listener:OnAttributeChanged(name, old, new)
    if name == 'Health' then Attributes.AddBase(self.entity, 'Seen', 1) end
end
function Listener:OnEffectApplied(effect, handle) Attributes.AddBase(self.entity, 'Applied', 1) end
return Listener
)";

struct Rig {
    World world;
    GuidIndex guids;
    LuauHost host;
    AttributeSystem attrs{world};
    EffectLibrary effects;
    EffectSystem fx{world, attrs, effects};
    AbilityLibrary abilities;
    AbilitySystem ab{world, attrs, fx, abilities, effects};
    assets::AssetGuid hero = assets::NewAssetGuid();

    Rig() {
        (void)GetComponentId<ScriptComponent>();
        GameplayEffect buff;
        buff.name = "Buff";
        buff.duration_policy = GameplayEffect::Duration::Infinite;
        buff.modifiers = {{"Health", GameplayEffect::Op::Add, 20}};
        effects.Register(buff);
        GameplayAbility dash;
        dash.name = "Dash";
        abilities.Register(dash);
    }
    ScriptSystem::SourceLoader Loader() {
        return [this](const assets::AssetGuid& guid, std::string& source, std::string& name) {
            if (guid != hero) return false;
            source = kHero;
            name = "Hero.luau";
            return true;
        };
    }
    Entity Make() {
        ScriptComponent component;
        component.script.guid = hero;
        Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, std::move(component));
        guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
        return e;
    }
};

f64 Num(const ScriptValue& v) { return std::holds_alternative<f64>(v) ? std::get<f64>(v) : -999.0; }
bool Flag(const ScriptValue& v) { return std::holds_alternative<bool>(v) && std::get<bool>(v); }
std::string Str(const ScriptValue& v) { return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string("<none>"); }

} // namespace

AETHER_TEST(GameplayScript_TablesActOnTheSystems) {
    Rig r;
    ScriptSystem scripts(r.host, r.world, r.guids, r.Loader());
    Lifecycle life(r.world, r.guids);
    scripts.Register(life);
    const Entity e = r.Make();
    life.BeginPlay();
    CHECK(scripts.SendEvent(e, "Run"));
    CHECK(scripts.Errors().empty());
    CHECK(Flag(scripts.GetField(e, "defined")) && Num(scripts.GetField(e, "hp")) == 70.0 && Num(scripts.GetField(e, "max")) == 100.0 && Flag(scripts.GetField(e, "has")));
    CHECK(Flag(scripts.GetField(e, "tag")) && Num(scripts.GetField(e, "depth")) == 3.0);
    CHECK(Num(scripts.GetField(e, "effect")) > 0.0 && Num(scripts.GetField(e, "buffed")) == 90.0 && Flag(scripts.GetField(e, "effect_active")) && Num(scripts.GetField(e, "effect_count")) == 1.0);
    CHECK(Flag(scripts.GetField(e, "granted")) && Flag(scripts.GetField(e, "can")) && Num(scripts.GetField(e, "handle")) > 0.0 && Flag(scripts.GetField(e, "active")) && Flag(scripts.GetField(e, "ended")));
    CHECK(Flag(scripts.GetField(e, "removed")) && Num(scripts.GetField(e, "after")) == 70.0);
    // Wrong-typed arguments are script errors.
    CHECK(!r.host.Run("return Attributes.Get(5, 'x')").ok && !r.host.Run("return Effects.Apply(nil, 'x')").ok && !r.host.Run("return GameplayTags.IsValid(1)").ok && !r.host.Run("return Abilities.End('x')").ok);
    life.EndPlay();
}

AETHER_TEST(GameplayScript_WithoutSystemsTheTablesAreInert) {
    LuauHost host;
    World world;
    GuidIndex guids;
    ScriptSystem scripts(host, world, guids, [](const assets::AssetGuid&, std::string&, std::string&) { return false; });
    const ScriptResult r = host.Run("return GameplayTags.IsValid('A.B'), GameplayTags.GetParent('A.B'), Abilities.End(3), Effects.Remove(3), Abilities.TryActivate and true");
    CHECK(r.ok && r.values.size() == 5 && Flag(r.values[0]) && Str(r.values[1]) == "A" && !Flag(r.values[2]) && !Flag(r.values[3]) && Flag(r.values[4]));
}

AETHER_TEST(GameplayScript_PlayerSendsEventsToTheOwnersScript) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_gameplay_script_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const assets::AssetGuid script_guid = assets::NewAssetGuid();
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array()}};
    const auto asset = [](const assets::AssetGuid& g, const char* path, const char* importer) {
        return nlohmann::json{{"guid", assets::ToString(g)}, {"path", path}, {"importer", importer}};
    };
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()},
                                     {"assets", nlohmann::json::array({asset(assets::NewAssetGuid(), "Scenes/start.ascene", "Scene"), asset(script_guid, "Scripts/Hero.luau", "Script"),
                                                                        asset(assets::NewAssetGuid(), "Effects/buff.aeffect", "GameplayEffect")})},
                                     {"files", nlohmann::json::array()}};
    const nlohmann::json buff = {{"name", "Buff"}, {"duration_policy", "infinite"}, {"modifiers", nlohmann::json::array({{{"attribute", "Health"}, {"op", "add"}, {"magnitude", 5}}})}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump(2));
    writer.Add("Content/Scenes/start.ascene", scene.dump(2));
    writer.Add("Content/Scripts/Hero.luau", std::string(kListener));
    writer.Add("Content/Effects/buff.aeffect", buff.dump(2));
    const std::filesystem::path file = dir / "Game.apak";
    std::string error;
    CHECK(writer.Write(file.string(), &error));
    player::GamePackage package;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    CHECK(game.LoadStartupScene(&error));
    ScriptComponent component;
    component.script.guid = script_guid;
    const Entity e = game.GetWorld().CreateEntity(IdComponent{NewEntityGuid()}, std::move(component));
    game.BeginPlay();
    AttributeSystem* attrs = AttributeSystem::Active();
    EffectSystem* fx = EffectSystem::Active();
    CHECK(attrs != nullptr && fx != nullptr && game.ScriptErrors().empty());
    if (!attrs || !fx) return;
    attrs->Define(e, "Seen", 0);
    attrs->Define(e, "Applied", 0);
    attrs->Define(e, "Health", 50, 0, 100);
    fx->Apply(e, "Buff");
    game.Tick(0.016f); // Player.Effects and Player.Attributes hand the changes to the script
    CHECK(game.ScriptErrors().empty());
    CHECK(attrs->Get(e, "Seen") == 2.0f && attrs->Get(e, "Applied") == 1.0f); // Health appeared, then the buff moved it; the buff was applied
}

#endif
