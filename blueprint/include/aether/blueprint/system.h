#pragma once

#include "aether/assets/asset_ref.h"
#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/scene/lifecycle.h"
#include "aether/scene/script_component.h"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace aether::bp {

// On an entity that runs a Blueprint (Phase 12 step 3): which one, and the
// instance-editable variables this entity overrides (JSON values, the same
// form as ScriptComponent's properties).
struct BlueprintInstance {
    assets::AssetRef<assets::BlueprintAsset> blueprint;
    std::vector<ScriptProperty> overrides;

    void SetOverride(const std::string& name, const nlohmann::json& value);
    nlohmann::json OverridesJson() const;
};

// Registers BlueprintInstance (so scenes load it), idempotent.
void RegisterBlueprintComponents();

// Runs BlueprintInstance entities through the scene lifecycle (Phase 9):
//   entering play  -> the Blueprint is loaded and compiled (cached per asset)
//                     and the entity attached, with its overrides;
//   start          -> Event BeginPlay;
//   enable/disable -> ticking and latent actions resume or pause;
//   destroy / end of play -> Event EndPlay, then detached (pending latent
//                     actions are dropped).
// Tick runs from Update(dt), once per frame: latent actions that are due
// resume first, then Event Tick. Blueprints that fail to compile are
// reported once in CompileErrors() and their entities don't run.
class BlueprintSystem {
public:
    // Reads a Blueprint asset; false if it isn't available.
    using Loader = std::function<bool(const assets::AssetGuid& guid, Blueprint& out, std::string& name)>;

    explicit BlueprintSystem(World& world, BlueprintVM::Options options = {});

    void SetLoader(Loader loader) { loader_ = std::move(loader); }
    void Register(Lifecycle& lifecycle);
    void Update(f32 dt) { vm_.Tick(dt); }

    BlueprintVM& VM() { return vm_; }
    // Compiles (or returns the cached result for) a Blueprint asset.
    std::shared_ptr<const CompiledBlueprint> Get(const assets::AssetGuid& guid);
    // Drops the cached compile, e.g. after the .abp changed (instances keep
    // the version they started with until they're restarted).
    void Invalidate(const assets::AssetGuid& guid) { cache_.erase(guid); }

    struct CompileError {
        assets::AssetGuid blueprint;
        std::string name;
        std::vector<Diagnostic> diagnostics;
    };
    const std::vector<CompileError>& CompileErrors() const { return errors_; }

private:
    void Start(Entity entity);
    void Stop(Entity entity);

    World& world_;
    BlueprintVM vm_;
    Loader loader_;
    std::map<assets::AssetGuid, std::shared_ptr<const CompiledBlueprint>> cache_; // null = failed
    std::vector<CompileError> errors_;
};

} // namespace aether::bp

AETHER_REFLECT(aether::bp::BlueprintInstance, 1,
    AETHER_FIELD(blueprint, Field_EditAnywhere, {.tooltip = "The Blueprint (.abp) this entity runs"}),
    AETHER_FIELD(overrides, Field_ReadOnly)
)
