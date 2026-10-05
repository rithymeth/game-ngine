#include "test_framework.h"

#include "aether/ecs/world.h"
#include "aether/gameplay/ability_system.h"
#include "aether/gameplay/tag_container.h"
#include "aether/scene/components.h"

#include <cmath>

// Phase 30 step 4a (§30.4): abilities, their activation checks, cost and
// cooldown through effects, cancel and block by tags, and JSON.

using namespace aether;
using namespace aether::gas;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {
bool Near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }
GameplayTag T(const char* n) { return GameplayTag::Make(n); }

struct Rig {
    World world;
    AttributeSystem attrs{world};
    EffectLibrary effects;
    EffectSystem fx{world, attrs, effects};
    AbilityLibrary lib;
    AbilitySystem ab{world, attrs, fx, lib, effects};
    Entity e;
    Rig() {
        GameplayEffect cost;
        cost.name = "ManaCost";
        cost.modifiers = {{"Mana", GameplayEffect::Op::Add, -20}};
        GameplayEffect cd;
        cd.name = "FireCD";
        cd.duration_policy = GameplayEffect::Duration::Timed;
        cd.duration = 3;
        cd.granted_tags = {T("Cooldown.Fire")};
        CHECK(effects.Register(cost) && effects.Register(cd));
        GameplayAbility fireball;
        fireball.name = "Fireball";
        fireball.tags = {T("Ability.Fire")};
        fireball.cost = "ManaCost";
        fireball.cooldown = "FireCD";
        fireball.activation_owned_tags = {T("State.Casting")};
        fireball.max_duration = 1.0f;
        CHECK(lib.Register(fireball));
        e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
        attrs.Define(e, "Mana", 50, 0, 100);
        ab.Grant(e, "Fireball");
    }
    void Reg(GameplayAbility a) { std::string err; CHECK(lib.Register(std::move(a), &err)); }
    bool HasTag(const char* t) { return world.HasComponent<TagContainer>(e) && world.GetComponent<TagContainer>(e)->HasTag(T(t)); }
};
} // namespace

AETHER_TEST(Ability_ActivateCommitsCostAndCooldown) {
    Rig r;
    CHECK(r.ab.CanActivate(r.e, "Fireball") == FailReason::None);
    const auto res = r.ab.TryActivate(r.e, "Fireball");
    CHECK(res.handle != 0 && res.reason == FailReason::None && r.ab.IsActive(r.e, "Fireball"));
    CHECK(Near(r.attrs.Get(r.e, "Mana"), 30) && r.HasTag("Cooldown.Fire") && r.HasTag("State.Casting"));
    CHECK(r.ab.Events().size() == 1 && r.ab.Events()[0].kind == AbilityEvent::Kind::Activated);
    CHECK(r.ab.End(res.handle) && !r.ab.IsActive(r.e, "Fireball") && !r.HasTag("State.Casting") && !r.ab.End(res.handle));
    CHECK(r.ab.TryActivate(r.e, "Fireball").reason == FailReason::Cooldown); // still cooling down
    r.fx.Update(3.0f);
    CHECK(!r.HasTag("Cooldown.Fire") && r.ab.TryActivate(r.e, "Fireball").handle != 0 && Near(r.attrs.Get(r.e, "Mana"), 10));
}

AETHER_TEST(Ability_EveryFailReason) {
    Rig r;
    CHECK(r.ab.CanActivate(kNullEntity, "Fireball") == FailReason::DeadOwner);
    CHECK(r.ab.CanActivate(r.e, "Nope") == FailReason::UnknownAbility);
    GameplayAbility other;
    other.name = "Other";
    other.activation_required = TagQuery::All({T("State.Alive")});
    other.activation_blocked = TagQuery::Any({T("State.Stunned")});
    r.Reg(other);
    CHECK(r.ab.CanActivate(r.e, "Other") == FailReason::NotGranted);
    r.ab.Grant(r.e, "Other");
    CHECK(r.ab.CanActivate(r.e, "Other") == FailReason::MissingRequired);
    r.world.AddComponent<TagContainer>(r.e, TagContainer{});
    r.world.GetComponent<TagContainer>(r.e)->Add(T("State.Alive"));
    CHECK(r.ab.CanActivate(r.e, "Other") == FailReason::None);
    r.world.GetComponent<TagContainer>(r.e)->Add(T("State.Stunned"));
    CHECK(r.ab.CanActivate(r.e, "Other") == FailReason::Blocked);
    r.world.GetComponent<TagContainer>(r.e)->Remove(T("State.Stunned"));
    CHECK(r.ab.TryActivate(r.e, "Other").handle != 0 && r.ab.CanActivate(r.e, "Other") == FailReason::AlreadyActive);
    r.attrs.SetBase(r.e, "Mana", 10);
    CHECK(r.ab.CanActivate(r.e, "Fireball") == FailReason::CannotAfford);
}

AETHER_TEST(Ability_UnaffordableChangesNothing) {
    Rig r;
    r.attrs.SetBase(r.e, "Mana", 5);
    r.ab.ClearEvents();
    const auto res = r.ab.TryActivate(r.e, "Fireball");
    CHECK(res.handle == 0 && res.reason == FailReason::CannotAfford);
    CHECK(Near(r.attrs.Get(r.e, "Mana"), 5) && !r.HasTag("Cooldown.Fire") && !r.HasTag("State.Casting") && r.ab.ActiveCount(r.e) == 0);
    CHECK(r.ab.Events().size() == 1 && r.ab.Events()[0].kind == AbilityEvent::Kind::Failed && r.ab.Events()[0].reason == FailReason::CannotAfford);
}

AETHER_TEST(Ability_DeferredCommit) {
    Rig r;
    GameplayAbility a = *r.lib.Find("Fireball");
    a.name = "Charged";
    a.commit_on_activate = false;
    a.max_duration = 0;
    r.Reg(a);
    r.ab.Grant(r.e, "Charged");
    const auto res = r.ab.TryActivate(r.e, "Charged");
    CHECK(res.handle != 0 && Near(r.attrs.Get(r.e, "Mana"), 50) && !r.HasTag("Cooldown.Fire")); // not paid yet
    CHECK(r.ab.Commit(res.handle) && Near(r.attrs.Get(r.e, "Mana"), 30) && r.HasTag("Cooldown.Fire"));
    CHECK(r.ab.Commit(res.handle) && Near(r.attrs.Get(r.e, "Mana"), 30)); // idempotent
    r.attrs.SetBase(r.e, "Mana", 0);
    CHECK(!r.ab.Commit(9999));
}

AETHER_TEST(Ability_CancelAndBlockByTags) {
    Rig r;
    GameplayAbility channel;
    channel.name = "Channel";
    channel.tags = {T("Ability.Channel")};
    r.Reg(channel);
    GameplayAbility dash;
    dash.name = "Dash";
    dash.tags = {T("Ability.Move.Dash")};
    dash.cancel_abilities_with_tags = TagQuery::Any({T("Ability.Channel")});
    r.Reg(dash);
    GameplayAbility stun;
    stun.name = "Stunned";
    stun.tags = {T("Ability.Debuff")};
    stun.block_abilities_with_tags = TagQuery::Any({T("Ability")}); // hierarchical: every ability
    r.Reg(stun);
    for (const char* n : {"Channel", "Dash", "Stunned"}) r.ab.Grant(r.e, n);
    const auto ch = r.ab.TryActivate(r.e, "Channel");
    CHECK(ch.handle != 0 && r.ab.IsActive(r.e, "Channel"));
    r.ab.ClearEvents();
    CHECK(r.ab.TryActivate(r.e, "Dash").handle != 0 && !r.ab.IsActive(r.e, "Channel"));
    CHECK(r.ab.Events().size() == 2 && r.ab.Events()[0].kind == AbilityEvent::Kind::Cancelled && r.ab.Events()[1].kind == AbilityEvent::Kind::Activated);
    const auto st = r.ab.TryActivate(r.e, "Stunned");
    CHECK(st.handle != 0 && r.ab.CanActivate(r.e, "Fireball") == FailReason::BlockedByAbility);
    CHECK(r.ab.End(st.handle) && r.ab.CanActivate(r.e, "Fireball") == FailReason::None); // the block goes with it
    CHECK(r.ab.CancelByTag(r.e, T("Ability.Move")) == 1 && !r.ab.IsActive(r.e, "Dash") && r.ab.CancelByTag(r.e, T("Ability.Move")) == 0);
}

AETHER_TEST(Ability_OwnedTagsStackWithEffectTags) {
    Rig r;
    GameplayEffect shield;
    shield.name = "Shield";
    shield.duration_policy = GameplayEffect::Duration::Infinite;
    shield.granted_tags = {T("State.Casting")};
    r.effects.Register(shield);
    const auto fx = r.fx.Apply(r.e, "Shield");
    const auto res = r.ab.TryActivate(r.e, "Fireball");
    CHECK(r.world.GetComponent<TagContainer>(r.e)->Count(T("State.Casting")) == 2);
    r.ab.End(res.handle);
    CHECK(r.HasTag("State.Casting")); // the effect still grants it
    r.fx.Remove(fx.handle);
    CHECK(!r.HasTag("State.Casting"));
}

AETHER_TEST(Ability_MaxDurationTimesOutExactly) {
    Rig r;
    r.ab.TryActivate(r.e, "Fireball");
    r.ab.ClearEvents();
    r.ab.Update(0.5f);
    CHECK(r.ab.IsActive(r.e, "Fireball") && r.ab.Events().empty());
    r.ab.Update(0.5f);
    CHECK(!r.ab.IsActive(r.e, "Fireball") && r.ab.Events().size() == 1 && r.ab.Events()[0].kind == AbilityEvent::Kind::Ended && r.ab.Events()[0].timed_out);
    CHECK(!r.HasTag("State.Casting"));
}

AETHER_TEST(Ability_RevokeCancelsAndEventsAreDeterministic) {
    const auto run = []() {
        Rig r;
        r.ab.TryActivate(r.e, "Fireball");
        r.ab.Update(0.4f);
        r.ab.Revoke(r.e, "Fireball");
        std::vector<int> stream;
        for (const AbilityEvent& ev : r.ab.Events()) stream.push_back(static_cast<int>(ev.kind) * 100 + static_cast<int>(ev.handle));
        return stream;
    };
    const std::vector<int> a = run();
    CHECK(a.size() == 2 && a == run());
    Rig r;
    r.ab.TryActivate(r.e, "Fireball");
    CHECK(r.ab.Revoke(r.e, "Fireball") && !r.ab.IsGranted(r.e, "Fireball") && !r.ab.IsActive(r.e, "Fireball") && !r.HasTag("State.Casting") && !r.ab.Revoke(r.e, "Fireball"));
    CHECK(r.ab.TryActivate(r.e, "Fireball").reason == FailReason::NotGranted);
}

AETHER_TEST(Ability_RebuildAfterReload) {
    Rig r;
    const auto res = r.ab.TryActivate(r.e, "Fireball");
    r.ab.Rebuild(); // as after loading a world: the handle index is derived
    CHECK(r.ab.End(res.handle) && r.ab.ActiveCount(r.e) == 0);
}

AETHER_TEST(Ability_JsonRoundTripAndErrors) {
    GameplayAbility a;
    a.name = "Fireball";
    a.tags = {T("Ability.Fire")};
    a.activation_required = TagQuery::All({T("State.Alive")});
    a.activation_blocked = TagQuery::Any({T("State.Stunned")});
    a.cancel_abilities_with_tags = TagQuery::Any({T("Ability.Channel")});
    a.block_abilities_with_tags = TagQuery::Any({T("Ability.Fire")});
    a.activation_owned_tags = {T("State.Casting")};
    a.cost = "ManaCost";
    a.cooldown = "FireCD";
    a.max_duration = 2;
    a.commit_on_activate = false;
    GameplayAbility back;
    std::string err;
    CHECK(AbilityFromJson(AbilityToJson(a).dump(), back, &err) && back == a);
    const auto fails = [](const char* json, const char* code) {
        GameplayAbility out;
        std::string e;
        return !AbilityFromJson(json, out, &e) && e.rfind(code, 0) == 0;
    };
    CHECK(fails("nonsense", "ability.bad_json"));
    CHECK(fails("{}", "ability.name_empty"));
    CHECK(fails(R"({"name":"x","max_duration":-1})", "ability.bad_duration"));
    CHECK(fails(R"({"name":"x","tags":["bad tag!"]})", "ability.bad_tag"));
    CHECK(fails(R"({"name":"x","activation_required":{"op":"xor"}})", "ability.bad_tag"));
    CHECK(fails(R"({"name":"x","cost":5})", "ability.bad_json"));
    // The effects an ability names are checked against the effect library.
    Rig r;
    GameplayAbility good = *r.lib.Find("Fireball");
    CHECK(AbilityLibrary::CheckEffects(good, r.effects).empty());
    good.cost = "Missing";
    CHECK(AbilityLibrary::CheckEffects(good, r.effects).rfind("ability.unknown_effect", 0) == 0);
    good.cost = "FireCD";
    CHECK(AbilityLibrary::CheckEffects(good, r.effects).rfind("ability.cost_not_instant", 0) == 0);
    good.cost = "ManaCost";
    good.cooldown = "ManaCost";
    CHECK(AbilityLibrary::CheckEffects(good, r.effects).rfind("ability.cooldown_not_timed", 0) == 0);
}
