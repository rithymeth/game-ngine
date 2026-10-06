#include "test_framework.h"

#ifdef AETHER_TEST_HAS_INTERACTION

#include "aether/assets/asset_guid.h"
#include "aether/ecs/world.h"
#include "aether/interaction/interaction_library.h"
#include "aether/interaction/interaction_system.h"
#include "aether/loc/localization.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/reflection/registry.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>

// Phase 30 step 7b (§30.8): interactables, focus, using them, and the faces.

using namespace aether;
using namespace aether::interact;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

struct Rig {
    World world;
    gas::AttributeSystem attrs{world};
    gas::EffectLibrary effects;
    gas::EffectSystem fx{world, attrs, effects};
    gas::AbilityLibrary abilities;
    gas::AbilitySystem ab{world, attrs, fx, abilities, effects};
    InteractionSystem sys{world, &fx, &ab};
    Entity hero;

    Entity Make(const Vec3& at, Interactable i = {}) {
        return world.CreateEntity(Transform{at, Quaternion::Identity()}, std::move(i));
    }
    Rig() {
        gas::GameplayEffect heal;
        heal.name = "Heal";
        heal.modifiers = {{"Health", gas::GameplayEffect::Op::Add, 10}};
        effects.Register(heal);
        gas::GameplayAbility open;
        open.name = "OpenDoor";
        abilities.Register(open);
        hero = world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()});
        attrs.Define(hero, "Health", 50, 0, 100);
    }
};

} // namespace

AETHER_TEST(Interaction_ChecksRangeEnabledTagsAndState) {
    Rig r;
    Interactable i;
    i.range = 2.0f;
    i.required_tags = {"Item.Key"};
    i.blocked_tags = {"State.Stunned"};
    const Entity door = r.Make(Vec3(1, 0, 0), i);
    CHECK(r.sys.Check(r.hero, door) == Reason::MissingTag);
    r.world.AddComponent<gas::TagContainer>(r.hero, gas::TagContainer{});
    r.world.GetComponent<gas::TagContainer>(r.hero)->Add(gas::GameplayTag::Make("Item.Key.Gold")); // hierarchical match
    CHECK(r.sys.Check(r.hero, door) == Reason::None);
    r.world.GetComponent<gas::TagContainer>(r.hero)->Add(gas::GameplayTag::Make("State.Stunned"));
    CHECK(r.sys.Check(r.hero, door) == Reason::BlockedTag);
    r.world.GetComponent<gas::TagContainer>(r.hero)->Remove(gas::GameplayTag::Make("State.Stunned"));
    r.world.GetComponent<Transform>(r.hero)->position = Vec3(-5, 0, 0);
    CHECK(r.sys.Check(r.hero, door) == Reason::OutOfRange && r.sys.Check(r.hero, door, Vec3(1, 0, 0)) == Reason::None); // from another position
    r.world.GetComponent<Transform>(r.hero)->position = Vec3(0, 0, 0);
    CHECK(r.sys.SetEnabled(door, false) && r.sys.Check(r.hero, door) == Reason::Disabled && r.sys.SetEnabled(door, true));
    CHECK(r.sys.Check(r.hero, r.hero) == Reason::NotInteractable && r.sys.Check(r.hero, Entity{}) == Reason::DeadEntity && r.sys.Check(Entity{}, door) == Reason::DeadEntity);
    CHECK(!r.sys.SetEnabled(r.hero, true) && !r.sys.Reset(r.hero));
    // A range of 0 has no limit.
    Interactable far;
    far.range = 0.0f;
    CHECK(r.sys.Check(r.hero, r.Make(Vec3(900, 0, 0), far)) == Reason::None);
}

AETHER_TEST(Interaction_InteractAppliesActionsAndStartsCooldown) {
    Rig r;
    Interactable i;
    i.effect = "Heal";
    i.ability = "OpenDoor";
    i.cooldown = 2.0f;
    const Entity door = r.Make(Vec3(1, 0, 0), i);
    r.ab.Grant(r.hero, "OpenDoor");
    CHECK(r.sys.Interact(r.hero, door) == Reason::None);
    CHECK(Near(r.attrs.Get(r.hero, "Health"), 60) && r.ab.IsActive(r.hero, "OpenDoor"));
    CHECK(r.sys.Events().size() == 1 && r.sys.Events()[0].kind == InteractionEvent::Kind::Interacted && r.sys.Events()[0].target == door && r.sys.Events()[0].interactor == r.hero);
    r.sys.ClearEvents();
    CHECK(r.sys.Interact(r.hero, door) == Reason::Cooldown && Near(r.attrs.Get(r.hero, "Health"), 60));
    CHECK(r.sys.Events().size() == 1 && r.sys.Events()[0].kind == InteractionEvent::Kind::Failed && r.sys.Events()[0].reason == Reason::Cooldown);
    r.sys.Update(1.0f);
    CHECK(r.sys.Check(r.hero, door) == Reason::Cooldown);
    r.sys.Update(1.0f);
    CHECK(r.sys.Check(r.hero, door) == Reason::None);
    // The ability can't be activated twice (still running), so the use fails and nothing else happens.
    CHECK(r.sys.Interact(r.hero, door) == Reason::ActionFailed && Near(r.attrs.Get(r.hero, "Health"), 60));
    // An ability the user lacks fails the use too.
    Interactable locked;
    locked.ability = "Missing";
    CHECK(r.sys.Interact(r.hero, r.Make(Vec3(0, 1, 0), locked)) == Reason::ActionFailed);
}

AETHER_TEST(Interaction_OneShotAndReset) {
    Rig r;
    Interactable i;
    i.one_shot = true;
    const Entity lever = r.Make(Vec3(1, 0, 0), i);
    CHECK(r.sys.Interact(r.hero, lever) == Reason::None && r.sys.Interact(r.hero, lever) == Reason::Used);
    CHECK(r.sys.Reset(lever) && r.sys.Interact(r.hero, lever) == Reason::None);
    CHECK(r.world.GetComponent<Interactable>(lever)->used);
}

AETHER_TEST(Interaction_FocusPicksTheNearestInFront) {
    Rig r;
    Interactable near_i;
    near_i.prompt = "Open";
    Interactable far_i;
    far_i.prompt = "Pull";
    far_i.range = 10.0f;
    const Entity near_door = r.Make(Vec3(1, 0, 0), near_i);
    const Entity behind = r.Make(Vec3(-1.5f, 0, 0), near_i);
    const Entity lever = r.Make(Vec3(0, 0, 6), far_i);
    // All round: the nearest.
    FocusResult f = r.sys.Focus(r.hero, Vec3(0, 0, 0), Vec3(0, 0, 0));
    CHECK(f.target == near_door && Near(f.distance, 1.0f) && f.prompt == "Open");
    // Looking along +z: only the lever is in front (the doors are to the sides).
    f = r.sys.Focus(r.hero, Vec3(0, 0, 0), Vec3(0, 0, 1));
    CHECK(f.target == lever && f.prompt == "Pull");
    // Looking along -x: the one behind (1.5 away) is in front.
    f = r.sys.Focus(r.hero, Vec3(0, 0, 0), Vec3(-1, 0, 0));
    CHECK(f.target == behind);
    // Out of everyone's range, nothing.
    f = r.sys.Focus(r.hero, Vec3(100, 0, 0), Vec3(0, 0, 0));
    CHECK(f.target.IsNull() && f.prompt.empty());
    r.sys.SetEnabled(near_door, false);
    r.sys.SetEnabled(behind, false);
    CHECK(r.sys.Focus(r.hero, Vec3(0, 0, 0), Vec3(0, 0, 0)).target == lever); // the lever is 6 away, within its range of 10
}

AETHER_TEST(Interaction_PromptIsLocalized) {
    Rig r;
    Interactable i;
    i.prompt = "Open";
    i.prompt_key = "prompt.open";
    const Entity door = r.Make(Vec3(1, 0, 0), i);
    CHECK(r.sys.Prompt(door) == "Open"); // no localization: the fallback
    loc::Localization l;
    l.MakeActive();
    l.AddFromCsv("key,en,fr\nprompt.open,Open the door,Ouvrir la porte\n");
    CHECK(r.sys.Prompt(door) == "Open the door");
    l.SetLanguage("fr");
    CHECK(r.sys.Prompt(door) == "Ouvrir la porte" && r.sys.Prompt(r.hero).empty());
}

AETHER_TEST(Interaction_ComponentRoundTripsAndLibraryActsOnTheSystem) {
    Interactable i;
    i.prompt = "Talk";
    i.required_tags = {"State.Alive"};
    i.one_shot = true;
    i.used = true;
    i.remaining = 1.5f;
    const reflect::Json j = reflect::ToJson(i);
    Interactable back;
    CHECK(reflect::FromJson(back, j) && back.prompt == "Talk" && back.required_tags.size() == 1 && back.one_shot && back.used && Near(back.remaining, 1.5f));
    const reflect::TypeInfo* type = reflect::TypeRegistry::Find("Interaction");
    CHECK(type != nullptr && type->functions.size() == 7 && reflect::TypeRegistry::Find("Interactable") != nullptr);
    World world;
    const Entity e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    CHECK(InteractionSystem::Active() == nullptr);
    CHECK(Interaction::FindInteractable(e, Vec3(), Vec3()).IsNull() && !Interaction::Interact(e, e) && !Interaction::CanInteract(e, e) && Interaction::GetInteractionPrompt(e).empty());
    Rig r;
    CHECK(InteractionSystem::Active() == &r.sys);
    const Entity door = r.Make(Vec3(1, 0, 0));
    CHECK(Interaction::FindInteractable(r.hero, Vec3(), Vec3()) == door && Interaction::GetInteractionPrompt(door) == "Interact" && Interaction::GetPromptFor(r.hero, door) == "Interact");
    CHECK(Interaction::CanInteract(r.hero, door) && Interaction::SetInteractable(door, false) && !Interaction::CanInteract(r.hero, door) && Interaction::GetPromptFor(r.hero, door).empty());
    CHECK(Interaction::SetInteractable(door, true) && Interaction::Interact(r.hero, door) && Interaction::ResetInteractable(door));
    CHECK(std::string(ReasonName(Reason::OutOfRange)) == "out_of_range");
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer, const assets::AssetGuid& guid) {
    return {{"guid", assets::ToString(guid)}, {"path", path}, {"importer", importer}};
}

std::filesystem::path MakePackage(const char* dir_name, const assets::AssetGuid& script_guid, const std::string& script) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / dir_name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array()}};
    nlohmann::json assets_json = nlohmann::json::array({AssetEntry("Scenes/start.ascene", "Scene", assets::NewAssetGuid())});
    if (!script.empty()) assets_json.push_back(AssetEntry("Scripts/Door.luau", "Script", script_guid));
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()}, {"assets", assets_json}, {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump(2));
    writer.Add("Content/Scenes/start.ascene", scene.dump(2));
    if (!script.empty()) writer.Add("Content/Scripts/Door.luau", script);
    const std::filesystem::path file = dir / "Game.apak";
    std::string error;
    writer.Write(file.string(), &error);
    return file;
}

} // namespace

AETHER_TEST(Interaction_PlayerRunsTheSystemAndCooldowns) {
    const std::filesystem::path file = MakePackage("aether_interaction_player_tests", assets::AssetGuid{}, "");
    std::string error;
    player::GamePackage package;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    CHECK(game.LoadStartupScene(&error));
    InteractionSystem* sys = InteractionSystem::Active();
    CHECK(sys != nullptr);
    if (!sys) return;
    World& w = game.GetWorld();
    const Entity hero = w.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    Interactable i;
    i.cooldown = 0.05f;
    const Entity door = w.CreateEntity(Transform{Vec3(1, 0, 0), Quaternion::Identity()}, i);
    game.BeginPlay();
    CHECK(sys->Interact(hero, door) == Reason::None && sys->Check(hero, door) == Reason::Cooldown);
    game.Tick(0.016f);
    CHECK(sys->Events().empty()); // Player.Interaction took them
    for (int n = 0; n < 5; ++n) game.Tick(0.016f);
    CHECK(sys->Check(hero, door) == Reason::None); // the player's own tick counted the cooldown down
}

#if AETHER_TEST_HAS_SCRIPTING
AETHER_TEST(Interaction_LuauTableActsOnTheSystem) {
    Rig r;
    script::LuauHost host;
    GuidIndex guids;
    const assets::AssetGuid script_guid = assets::NewAssetGuid();
    (void)GetComponentId<ScriptComponent>();
    script::ScriptSystem scripts(host, r.world, guids, [&](const assets::AssetGuid& g, std::string& source, std::string& name) {
        if (g != script_guid) return false;
        source = "local D = {} function D:Try(who) self.found = Interaction.Find(who, 0, 0, 0, 0, 0, 0) ~= nil; self.can = Interaction.CanInteract(who, self.entity); "
                 "self.prompt = Interaction.GetPrompt(self.entity); self.used = Interaction.Interact(who, self.entity); self.again = Interaction.Interact(who, self.entity); "
                 "self.reset = Interaction.Reset(self.entity); self.off = Interaction.SetEnabled(self.entity, false); self.none = Interaction.Find(who, 0, 0, 0, 0, 0, 0) == nil end return D";
        name = "D.luau";
        return true;
    });
    Interactable i;
    i.one_shot = true;
    i.prompt = "Open";
    ScriptComponent component;
    component.script.guid = script_guid;
    const Entity door = r.world.CreateEntity(Transform{Vec3(1, 0, 0), Quaternion::Identity()}, IdComponent{NewEntityGuid()}, std::move(component), i);
    guids.Add(r.world.GetComponent<IdComponent>(door)->guid, door);
    Lifecycle life(r.world, guids);
    scripts.Register(life);
    life.BeginPlay();
    CHECK(!host.Run("return Interaction.Interact(5, nil)").ok && !host.Run("return Interaction.Find(nil, 0, 0, 0, 0, 0, 0)").ok && !host.Run("return Interaction.SetEnabled(nil, true)").ok);
    CHECK(scripts.SendEvent(door, "Try", {script::EntityRef{r.hero}}) && scripts.Errors().empty());
    const auto flag = [&](const char* f) { const auto v = scripts.GetField(door, f); return std::holds_alternative<bool>(v) && std::get<bool>(v); };
    CHECK(flag("found") && flag("can") && flag("used") && !flag("again") && flag("reset") && flag("off") && flag("none"));
    CHECK(std::holds_alternative<std::string>(scripts.GetField(door, "prompt")) && std::get<std::string>(scripts.GetField(door, "prompt")) == "Open");
    life.EndPlay();
}

AETHER_TEST(Interaction_ScriptsHearInteractAndUseTheTable) {
    const assets::AssetGuid script_guid = assets::NewAssetGuid();
    // The door counts uses in an attribute; the user's script reports failures the same way.
    const std::string source = R"(
local Door = {}
function Door:OnInteract(who) Attributes.AddBase(who, 'Opened', 1) end
function Door:OnInteractFailed(target, reason) Attributes.AddBase(self.entity, 'Failed', 1) end
function Door:Try(who)
    self.found = Interaction.Find(who, 0, 0, 0, 0, 0, 0) ~= nil
    self.can = Interaction.CanInteract(who, self.entity)
    self.prompt = Interaction.GetPrompt(self.entity)
    self.used = Interaction.Interact(who, self.entity)
    self.again = Interaction.Interact(who, self.entity)
end
return Door
)";
    const std::filesystem::path file = MakePackage("aether_interaction_script_tests", script_guid, source);
    std::string error;
    player::GamePackage package;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    CHECK(game.LoadStartupScene(&error));
    World& w = game.GetWorld();
    const Entity hero = w.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    ScriptComponent component;
    component.script.guid = script_guid;
    Interactable i;
    i.one_shot = true;
    i.prompt = "Open";
    const Entity door = w.CreateEntity(Transform{Vec3(1, 0, 0), Quaternion::Identity()}, IdComponent{NewEntityGuid()}, std::move(component), i);
    game.BeginPlay();
    gas::AttributeSystem* attrs = gas::AttributeSystem::Active();
    InteractionSystem* sys = InteractionSystem::Active();
    CHECK(attrs != nullptr && sys != nullptr && game.ScriptErrors().empty());
    if (!attrs || !sys) return;
    attrs->Define(hero, "Opened", 0);
    // The door's own script drives the Luau table (the player's script system runs it).
    CHECK(sys->Check(hero, door) == Reason::None);
    CHECK(sys->Interact(hero, door) == Reason::None && sys->Interact(hero, door) == Reason::Used); // a use, then a failure
    game.Tick(0.016f);
    CHECK(game.ScriptErrors().empty() && Near(attrs->Get(hero, "Opened"), 1)); // the door's script heard OnInteract (who) once
}
#endif

#endif
