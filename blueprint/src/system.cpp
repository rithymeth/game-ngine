#include "aether/blueprint/system.h"

#include "aether/core/log.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"

namespace aether::bp {

void BlueprintInstance::SetOverride(const std::string& name, const nlohmann::json& value) {
    for (ScriptProperty& p : overrides) {
        if (p.name == name) {
            p.value = value.dump();
            return;
        }
    }
    overrides.push_back({name, value.dump()});
}

nlohmann::json BlueprintInstance::OverridesJson() const {
    nlohmann::json j = nlohmann::json::object();
    for (const ScriptProperty& p : overrides) {
        nlohmann::json value = nlohmann::json::parse(p.value, nullptr, false);
        if (!value.is_discarded()) j[p.name] = std::move(value);
    }
    return j;
}

void RegisterBlueprintComponents() { (void)GetComponentId<BlueprintInstance>(); }

BlueprintSystem::BlueprintSystem(World& world, BlueprintVM::Options options) : world_(world), vm_(world, options) {
    RegisterBlueprintComponents();
}

std::shared_ptr<const CompiledBlueprint> BlueprintSystem::Get(const assets::AssetGuid& guid) {
    if (auto it = cache_.find(guid); it != cache_.end()) return it->second;
    Blueprint blueprint;
    std::string name = "Blueprint";
    std::shared_ptr<const CompiledBlueprint> compiled;
    if (!loader_ || !loader_(guid, blueprint, name)) {
        errors_.push_back({guid, name, {{"BP007", Severity::Error, "", 0, "", "The Blueprint asset couldn't be loaded."}}});
        AETHER_LOG_ERROR("Blueprint", "Couldn't load Blueprint asset %s", assets::ToString(guid).c_str());
    } else {
        CompileResult result = CompileBlueprint(blueprint);
        compiled = result.blueprint;
        if (!result.Ok()) {
            CompileError error{guid, name, {}};
            for (const Diagnostic& d : result.diagnostics.diagnostics) {
                if (d.severity == Severity::Error) {
                    error.diagnostics.push_back(d);
                    AETHER_LOG_ERROR("Blueprint", "%s: %s %s (graph %s, node %u)", name.c_str(), d.code.c_str(),
                                     d.message.c_str(), d.graph.c_str(), d.node);
                }
            }
            errors_.push_back(std::move(error));
        }
    }
    cache_[guid] = compiled;
    return compiled;
}

void BlueprintSystem::Start(Entity entity) {
    const BlueprintInstance* component = world_.GetComponent<BlueprintInstance>(entity);
    if (component == nullptr || !component->blueprint.IsSet()) return;
    if (vm_.IsAttached(entity)) return; // spawned by a Blueprint: attached already
    if (std::shared_ptr<const CompiledBlueprint> compiled = Get(component->blueprint.guid)) {
        vm_.Attach(entity, std::move(compiled), component->OverridesJson());
    }
}

void BlueprintSystem::Stop(Entity entity) {
    if (!vm_.IsAttached(entity)) return;
    vm_.Dispatch(entity, "Event.EndPlay");
    vm_.Detach(entity);
}

void BlueprintSystem::Register(Lifecycle& lifecycle) {
    LifecycleCallbacks callbacks;
    callbacks.on_create = [this](Entity e) { Start(e); };
    callbacks.on_enable = [this](Entity e) { vm_.SetEnabled(e, true); };
    callbacks.on_start = [this](Entity e) { vm_.Dispatch(e, "Event.BeginPlay"); };
    callbacks.on_disable = [this](Entity e) { vm_.SetEnabled(e, false); };
    callbacks.on_destroy = [this](Entity e) { Stop(e); };
    lifecycle.Register<BlueprintInstance>(std::move(callbacks));

    GuidIndex& guids = lifecycle.Guids();
    vm_.SetGuidIndex(&guids);
    // Spawn Blueprint: the entity is attached now (so the spawner can use it
    // straight away); the lifecycle starts it (BeginPlay) at its next sync.
    vm_.SetSpawnHandler([this, &guids](const assets::AssetGuid& blueprint, const Transform& transform,
                                       const nlohmann::json& exposed) {
        std::shared_ptr<const CompiledBlueprint> compiled = Get(blueprint);
        if (!compiled) return kNullEntity;
        BlueprintInstance instance;
        instance.blueprint.guid = blueprint;
        const Entity e = world_.CreateEntity(IdComponent{NewEntityGuid()}, transform, std::move(instance));
        guids.Add(world_.GetComponent<IdComponent>(e)->guid, e);
        vm_.Attach(e, std::move(compiled), exposed);
        return e;
    });
    // EndPlay here too: the lifecycle only reports entities it's already
    // tracking (one spawned this frame isn't yet).
    vm_.SetDestroyHandler([this, &lifecycle](Entity e) {
        Stop(e);
        lifecycle.Destroy(e);
    });
}

} // namespace aether::bp
