#pragma once

#include "aether/scene/lifecycle.h"
#include "aether/scene/script_component.h"
#include "aether/script/luau_host.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether::script {

// Runs ScriptComponents (Phase 11 step 3, docs/design/PHASE_SPECS.md §11.2).
//
// A script is a module that returns a table, its "class":
//
//   local Spin = {}
//   --@range 0 720
//   Spin.speed = 180
//   function Spin:OnUpdate(dt) ... self.entity:Get("Transform") ... end
//   return Spin
//
// Each entity with a ScriptComponent gets an instance table (class as
// metatable, `self.entity` set, the component's overridden variables copied
// in), and the class's OnCreate / OnEnable / OnStart / OnUpdate(dt) /
// OnFixedUpdate(dt) / OnLateUpdate(dt) / OnDisable / OnDestroy are called
// through Lifecycle, when defined. A script error in one callback is
// recorded (Errors()) and play goes on.
class ScriptSystem {
public:
    // Reads a script asset's source; false if it isn't available.
    using SourceLoader = std::function<bool(const assets::AssetGuid& script, std::string& source, std::string& name)>;

    // Binds `world` to `host`. Both must outlive the system.
    ScriptSystem(LuauHost& host, World& world, GuidIndex& guids, SourceLoader loader);
    ~ScriptSystem();
    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    // Registers the ScriptComponent callbacks.
    void Register(Lifecycle& lifecycle);

    // The script's class (loaded and cached on first use): its exposed
    // variables and callbacks, for the Inspector.
    ScriptClassInfo ClassInfo(const assets::AssetGuid& script);

    // Calls a method on the entity's script instance (e.g. from an event).
    ScriptResult CallMethod(Entity entity, const std::string& method, const std::vector<ScriptValue>& args = {});
    // A field of the entity's script instance (e.g. an exposed variable).
    ScriptValue GetField(Entity entity, const std::string& field);

    usize InstanceCount() const { return instances_.size(); }
    const std::vector<std::string>& Errors() const { return errors_; }
    void ClearErrors() { errors_.clear(); }

    // Loads a class from source into `host` and describes it, without
    // creating any instances. `ref` receives the class table's registry
    // reference (or LUA_NOREF on failure) when given.
    static ScriptClassInfo DescribeSource(LuauHost& host, const std::string& source, const std::string& name,
                                          int* ref = nullptr);

private:
    struct Class {
        int ref = -1;      // the class table
        int meta_ref = -1; // {__index = class} for instances
        std::string name;
        ScriptClassInfo info;
    };
    struct Instance {
        int ref = -1;
        assets::AssetGuid script;
        std::string name;
    };

    Class* LoadClass(const assets::AssetGuid& script);
    void Create(Entity entity);
    void Destroy(Entity entity);
    void Invoke(Entity entity, const char* method, const f32* dt = nullptr);
    Instance* Find(Entity entity);

    LuauHost& host_;
    World& world_;
    GuidIndex& guids_;
    SourceLoader loader_;
    std::unordered_map<assets::AssetGuid, Class> classes_;
    std::unordered_map<EntityGuid, Instance> instances_;
    std::vector<std::string> errors_;
};

} // namespace aether::script
