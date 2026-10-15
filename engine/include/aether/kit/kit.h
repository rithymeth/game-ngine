#pragma once

// @stability: experimental
//
// IKit and KitRegistry (Phase 37, docs/design/UPGRADE_PLAN.md).
//
// A kit is an optional gameplay module (gameplay, inventory, interaction, quests, ...). IKit describes the
// kit's install hooks, stages and scene-scoped runtime; KitRegistry orders and owns the installed kits.
//
// The engine knows nothing about any kit, the scripting host or the Blueprint VM (those sit above it), so the
// hooks that touch them take opaque pointers that the layer which owns the type casts back.

#include "aether/core/base.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aether { class World; }

namespace aether::kit {

// One scheduler stage a kit adds, named like the player's stages ("Player.Inventory").
struct KitStage {
    std::string name;
    std::vector<std::string> after; // stage names that must run first
};

struct KitAsset {
    std::string importer;
    std::string path;
};

struct KitAssetDiagnostic {
    std::string path;
    std::string message;
    bool content_read_failure = false;
};

// Package-neutral access to the cooked assets a kit recognizes. The host controls
// content access and diagnostic presentation; kits own parsing and validation.
struct KitAssetContext {
    std::vector<KitAsset> assets;
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content;
    std::function<void(const KitAssetDiagnostic&)> report;
};

struct KitScriptApiContext {
    std::function<void(const std::string&)> install;
};

// Scene-scoped kit state. The host owns each object for the scene lifetime; kits
// expose selected services opaquely so the engine layer remains kit-agnostic.
class IKitRuntime {
public:
    virtual ~IKitRuntime() = default;
    virtual void* Find(const char* name) = 0;
};

struct KitRuntimeContext {
    World* world = nullptr;
    std::function<void*(const char* name)> find_service;
};

class KitRuntimeSet {
public:
    IKitRuntime* Find(const std::string& kit) const;
    template <typename T> T* Get(const std::string& kit, const char* object) const {
        IKitRuntime* runtime = Find(kit);
        return runtime ? static_cast<T*>(runtime->Find(object)) : nullptr;
    }
    void Add(const std::string& kit, std::unique_ptr<IKitRuntime> runtime);
private:
    std::vector<std::pair<std::string, std::unique_ptr<IKitRuntime>>> runtimes_;
};

class IKit {
public:
    virtual ~IKit() = default;

    // Unique, stable name ("Inventory"). Dependencies refer to it.
    virtual const char* Name() const = 0;
    // Names of the kits that must be installed (and ordered) before this one.
    virtual std::vector<std::string> Deps() const { return {}; }

    // Reflection and saved components.
    virtual void RegisterComponents() {}
    // Loads this kit's definitions from the host's package context.
    virtual void LoadAssets(const KitAssetContext& context) { (void)context; }
    // Blueprint function libraries and events.
    virtual void InstallBlueprintNodes() {}
    // Luau natives; `script_host` is the scripting layer's host, passed opaque so the engine doesn't link it.
    virtual void InstallScriptApi(void* script_host) { (void)script_host; }
    // Stages this kit adds to the frame, in the order it wants them listed.
    virtual std::vector<KitStage> Stages() const { return {}; }
    // Creates scene-scoped state after host services are ready.
    virtual std::unique_ptr<IKitRuntime> CreateRuntime(const KitRuntimeContext& context) { (void)context; return {}; }
};

class KitRegistry {
public:
    // Takes ownership. Returns false (and records an error) for a null kit or a duplicate name.
    bool Add(std::unique_ptr<IKit> kit);

    bool Has(const std::string& name) const;
    IKit* Find(const std::string& name) const;
    usize Count() const { return kits_.size(); }

    // Orders the kits so every kit comes after its dependencies; ties break alphabetically so the order
    // doesn't depend on registration order. Returns false and records errors for a missing dependency or a
    // cycle; `Order()` is then empty. Errors never throw.
    bool Resolve();
    const std::vector<IKit*>& Order() const { return order_; }
    const std::vector<std::string>& Errors() const { return errors_; }

    // Runs the install hooks over `Order()`, in dependency order.
    void RegisterComponents();
    void LoadAssets(const KitAssetContext& context);
    void InstallBlueprintNodes();
    void InstallScriptApi(void* script_host);
    KitRuntimeSet CreateRuntime(const KitRuntimeContext& context);

    // Every kit's stages in dependency order, each stage's `after` extended with the kit's dependencies'
    // last stage so a kit's stages never run before the kits it needs.
    std::vector<KitStage> Stages() const;

private:
    std::vector<std::unique_ptr<IKit>> kits_;
    std::vector<IKit*> order_;
    std::vector<std::string> errors_;
};

} // namespace aether::kit
