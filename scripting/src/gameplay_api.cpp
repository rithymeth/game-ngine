#include "aether/script/gameplay_api.h"

#include "aether/gameplay/ability_library.h"
#include "aether/gameplay/attribute_system.h"
#include "aether/gameplay/effect_library.h"
#include "aether/gameplay/tag_library.h"

namespace aether::script {

namespace {

using namespace aether::gas;

bool NeedString(NativeCall& c, usize i, const char* table, const char* what) {
    if (c.IsString(i)) return true;
    c.Fail(std::string(table) + ": " + what + " must be a string");
    return false;
}
bool NeedEntity(NativeCall& c, usize i, const char* table, const char* what) {
    if (c.IsEntity(i)) return true;
    c.Fail(std::string(table) + ": " + what + " must be an entity");
    return false;
}
bool NeedNumber(NativeCall& c, usize i, const char* table, const char* what) {
    if (c.IsNumber(i)) return true;
    c.Fail(std::string(table) + ": " + what + " must be a number");
    return false;
}

} // namespace

void InstallGameplayApi(LuauHost& host) { InstallGameplayApi(host, nullptr); }

void InstallGameplayApi(LuauHost& host, gas::AttributeSystem* attributes) {
    // GameplayTags: pure string functions.
    const auto tags = [&](const char* name, NativeFunction fn) { host.RegisterNative("GameplayTags", name, std::move(fn)); };
    tags("IsValid", [](NativeCall& c) {
        if (NeedString(c, 0, "GameplayTags", "the tag")) c.Return(GameplayTags::IsValid(c.String(0)));
    });
    tags("Matches", [](NativeCall& c) {
        if (NeedString(c, 0, "GameplayTags", "the tag") && NeedString(c, 1, "GameplayTags", "the parent")) c.Return(GameplayTags::Matches(c.String(0), c.String(1)));
    });
    tags("MatchesExact", [](NativeCall& c) {
        if (NeedString(c, 0, "GameplayTags", "the tag") && NeedString(c, 1, "GameplayTags", "the other tag")) c.Return(GameplayTags::MatchesExact(c.String(0), c.String(1)));
    });
    tags("GetParent", [](NativeCall& c) {
        if (NeedString(c, 0, "GameplayTags", "the tag")) c.Return(GameplayTags::GetParent(c.String(0)));
    });
    tags("GetDepth", [](NativeCall& c) {
        if (NeedString(c, 0, "GameplayTags", "the tag")) c.Return(static_cast<f64>(GameplayTags::GetDepth(c.String(0))));
    });

    // A player host owns its world; only standalone hosts fall back to the process-wide active system.
    const auto system = [attributes] { return attributes ? attributes : AttributeSystem::Active(); };
    const auto attrs = [&](const char* name, NativeFunction fn) { host.RegisterNative("Attributes", name, std::move(fn)); };
    using Getter = f32 (AttributeSystem::*)(Entity, const std::string&, f32) const;
    const auto getter = [&](const char* name, Getter get) {
        attrs(name, [get, system](NativeCall& c) {
            if (!NeedEntity(c, 0, "Attributes", "the entity") || !NeedString(c, 1, "Attributes", "the name")) return;
            const f32 fallback = static_cast<f32>(c.Number(2, 0.0));
            const auto* current = system();
            c.Return(static_cast<f64>(current ? (current->*get)(c.EntityArg(0), c.String(1), fallback) : fallback));
        });
    };
    getter("Get", &AttributeSystem::Get);
    getter("GetBase", &AttributeSystem::GetBase);
    getter("GetMin", &AttributeSystem::GetMin);
    getter("GetMax", &AttributeSystem::GetMax);
    attrs("Has", [system](NativeCall& c) {
        if (!NeedEntity(c, 0, "Attributes", "the entity") || !NeedString(c, 1, "Attributes", "the name")) return;
        const auto* current = system();
        c.Return(current && current->Has(c.EntityArg(0), c.String(1)));
    });
    attrs("Define", [system](NativeCall& c) {
        if (!NeedEntity(c, 0, "Attributes", "the entity") || !NeedString(c, 1, "Attributes", "the name") || !NeedNumber(c, 2, "Attributes", "the base")) return;
        auto* current = system();
        c.Return(current && current->Define(c.EntityArg(0), c.String(1), static_cast<f32>(c.Number(2)),
                                           static_cast<f32>(c.Number(3, -3.0e38)), static_cast<f32>(c.Number(4, 3.0e38))));
    });
    attrs("SetBase", [system](NativeCall& c) {
        if (!NeedEntity(c, 0, "Attributes", "the entity") || !NeedString(c, 1, "Attributes", "the name") || !NeedNumber(c, 2, "Attributes", "the value")) return;
        auto* current = system();
        c.Return(current && current->SetBase(c.EntityArg(0), c.String(1), static_cast<f32>(c.Number(2))));
    });
    attrs("AddBase", [system](NativeCall& c) {
        if (!NeedEntity(c, 0, "Attributes", "the entity") || !NeedString(c, 1, "Attributes", "the name") || !NeedNumber(c, 2, "Attributes", "the amount")) return;
        auto* current = system();
        c.Return(current && current->AddBase(c.EntityArg(0), c.String(1), static_cast<f32>(c.Number(2))));
    });

    // Effects.
    const auto fx = [&](const char* name, NativeFunction fn) { host.RegisterNative("Effects", name, std::move(fn)); };
    fx("Apply", [](NativeCall& c) {
        if (!NeedEntity(c, 0, "Effects", "the target") || !NeedString(c, 1, "Effects", "the effect")) return;
        const Entity source = c.IsEntity(2) ? c.EntityArg(2) : Entity{};
        c.Return(static_cast<f64>(Effects::ApplyEffect(c.EntityArg(0), c.String(1), source)));
    });
    fx("Remove", [](NativeCall& c) {
        if (NeedNumber(c, 0, "Effects", "the handle")) c.Return(Effects::RemoveEffect(static_cast<i32>(c.Number(0))));
    });
    fx("RemoveByTag", [](NativeCall& c) {
        if (NeedEntity(c, 0, "Effects", "the target") && NeedString(c, 1, "Effects", "the tag")) c.Return(static_cast<f64>(Effects::RemoveEffectsByTag(c.EntityArg(0), c.String(1))));
    });
    fx("HasActive", [](NativeCall& c) {
        if (NeedEntity(c, 0, "Effects", "the target") && NeedString(c, 1, "Effects", "the effect")) c.Return(Effects::HasActiveEffect(c.EntityArg(0), c.String(1)));
    });
    fx("GetActiveCount", [](NativeCall& c) {
        if (NeedEntity(c, 0, "Effects", "the target")) c.Return(static_cast<f64>(Effects::GetActiveEffectCount(c.EntityArg(0))));
    });

    // Abilities.
    const auto ab = [&](const char* name, NativeFunction fn) { host.RegisterNative("Abilities", name, std::move(fn)); };
    using OwnerFn = bool (*)(const Entity&, const std::string&);
    const auto owner_bool = [&](const char* name, OwnerFn fn) {
        ab(name, [fn](NativeCall& c) {
            if (NeedEntity(c, 0, "Abilities", "the owner") && NeedString(c, 1, "Abilities", "the ability")) c.Return(fn(c.EntityArg(0), c.String(1)));
        });
    };
    owner_bool("Grant", &Abilities::GrantAbility);
    owner_bool("Revoke", &Abilities::RevokeAbility);
    owner_bool("CanActivate", &Abilities::CanActivateAbility);
    owner_bool("IsActive", &Abilities::IsAbilityActive);
    owner_bool("IsGranted", &Abilities::IsAbilityGranted);
    ab("TryActivate", [](NativeCall& c) {
        if (NeedEntity(c, 0, "Abilities", "the owner") && NeedString(c, 1, "Abilities", "the ability")) c.Return(static_cast<f64>(Abilities::TryActivateAbility(c.EntityArg(0), c.String(1))));
    });
    using HandleFn = bool (*)(i32);
    const auto handle_bool = [&](const char* name, HandleFn fn) {
        ab(name, [fn](NativeCall& c) {
            if (NeedNumber(c, 0, "Abilities", "the handle")) c.Return(fn(static_cast<i32>(c.Number(0))));
        });
    };
    handle_bool("Commit", &Abilities::CommitAbility);
    handle_bool("End", &Abilities::EndAbility);
    handle_bool("Cancel", &Abilities::CancelAbility);
}

} // namespace aether::script
