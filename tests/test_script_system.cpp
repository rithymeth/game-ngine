#include "aether/scene/components.h"
#include "aether/scene/serialization.h"
#include "aether/script/script_system.h"
#include "test_framework.h"

#include <unordered_map>

using namespace aether;
using namespace aether::script;

namespace {

const char* kSpin = R"(
local Spin = {}
--@range 0 720
--@tooltip Degrees per second
Spin.speed = 180

Spin.enabled = true
Spin.label = "spinner"
Spin.offset = vector.create(0, 1, 0)
Spin._secret = 1       -- private: not exposed
Spin.config = {}       -- tables aren't Inspector material

function Spin:OnCreate() table.insert(events, "create:" .. self.label) end
function Spin:OnStart() self.count = 0; table.insert(events, "start:" .. self.label) end
function Spin:OnUpdate(dt)
    local t = self.entity:Get("Transform")
    t.position = t.position + vector.create(self.speed * dt, 0, 0)
    self.count += 1
end
function Spin:OnDestroy() table.insert(events, "destroy:" .. self.label) end
function Spin:Add(a, b) return a + b + self.speed end
return Spin
)";

const char* kBroken = R"(
local Broken = {}
function Broken:OnUpdate(dt) local nothing = nil; return nothing.boom end
return Broken
)";

struct Fixture {
    World world;
    GuidIndex guids;
    LuauHost host;
    std::unordered_map<assets::AssetGuid, std::pair<std::string, std::string>> scripts;
    assets::AssetGuid spin = assets::NewAssetGuid();
    assets::AssetGuid broken = assets::NewAssetGuid();

    Fixture() {
        scripts[spin] = {kSpin, "Spin.luau"};
        scripts[broken] = {kBroken, "Broken.luau"};
        (void)GetComponentId<Transform>();
        (void)GetComponentId<ScriptComponent>();
    }
    ScriptSystem::SourceLoader Loader() {
        return [this](const assets::AssetGuid& guid, std::string& source, std::string& name) {
            auto it = scripts.find(guid);
            if (it == scripts.end()) {
                return false;
            }
            source = it->second.first;
            name = it->second.second;
            return true;
        };
    }
    Entity Make(const assets::AssetGuid& script, std::vector<ScriptProperty> properties = {}) {
        ScriptComponent component;
        component.script.guid = script;
        component.properties = std::move(properties);
        Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, Transform{}, std::move(component));
        guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
        return e;
    }
    std::string Events() {
        ScriptResult r = host.Run("return table.concat(events, ',')");
        return r.ok ? std::get<std::string>(r.values[0]) : "?";
    }
};

f64 Number(const ScriptValue& v) { return std::holds_alternative<f64>(v) ? std::get<f64>(v) : -1.0; }

} // namespace

AETHER_TEST(ScriptSystem_DescribesClasses) {
    LuauHost host;
    host.Run("events = {}");
    ScriptClassInfo info = ScriptSystem::DescribeSource(host, kSpin, "Spin.luau");
    AETHER_CHECK(info.ok && info.variables.size() == 4);
    const ExposedVariable* speed = info.Find("speed");
    AETHER_CHECK(speed != nullptr && speed->kind == ExposedVariable::Kind::Number && speed->default_value == 180);
    AETHER_CHECK(speed->has_range && speed->range_min == 0.0 && speed->range_max == 720.0);
    AETHER_CHECK(speed->tooltip == "Degrees per second");
    AETHER_CHECK(info.Find("enabled")->kind == ExposedVariable::Kind::Bool && !info.Find("enabled")->has_range);
    AETHER_CHECK(info.Find("label")->default_value == "spinner");
    AETHER_CHECK(info.Find("offset")->kind == ExposedVariable::Kind::Vector && info.Find("offset")->default_value[1] == 1.0);
    AETHER_CHECK(info.Find("_secret") == nullptr && info.Find("config") == nullptr);
    AETHER_CHECK((info.callbacks == std::vector<std::string>{"OnCreate", "OnDestroy", "OnStart", "OnUpdate"}));

    ScriptClassInfo not_table = ScriptSystem::DescribeSource(host, "return 42", "Bad.luau");
    AETHER_CHECK(!not_table.ok && not_table.error.find("must return a table") != std::string::npos);
    ScriptClassInfo syntax = ScriptSystem::DescribeSource(host, "local = ", "Syntax.luau");
    AETHER_CHECK(!syntax.ok && syntax.error.find("Syntax.luau:1") != std::string::npos);
    AETHER_CHECK(MatchesKind(ExposedVariable::Kind::Vector, nlohmann::json::array({1, 2, 3})));
    AETHER_CHECK(!MatchesKind(ExposedVariable::Kind::Vector, nlohmann::json::array({1, 2})));
    AETHER_CHECK(!MatchesKind(ExposedVariable::Kind::Number, "3"));
}

AETHER_TEST(ScriptSystem_RunsTheLifecycleWithPerEntityState) {
    Fixture f;
    f.host.Run("events = {}");
    Entity slow = f.Make(f.spin);
    Entity fast = f.Make(f.spin, {{"speed", "360"}, {"label", "\"fast\""}});
    Lifecycle life(f.world, f.guids);
    ScriptSystem scripts(f.host, f.world, f.guids, f.Loader());
    scripts.Register(life);

    life.BeginPlay();
    AETHER_CHECK(scripts.InstanceCount() == 2 && scripts.Errors().empty());
    AETHER_CHECK(f.Events() == "create:spinner,create:fast,start:spinner,start:fast");
    life.Update(0.5f);
    life.Update(0.5f);
    AETHER_CHECK(f.world.GetComponent<Transform>(slow)->position.x == 180.0f);
    AETHER_CHECK(f.world.GetComponent<Transform>(fast)->position.x == 360.0f);
    AETHER_CHECK(Number(scripts.GetField(slow, "count")) == 2.0 && Number(scripts.GetField(fast, "speed")) == 360.0);

    // Methods from C++ (events, later): self is the entity's instance.
    ScriptResult sum = scripts.CallMethod(fast, "Add", {1.0, 2.0});
    AETHER_CHECK(sum.ok && Number(sum.values[0]) == 363.0);
    AETHER_CHECK(!scripts.CallMethod(fast, "Nope").ok);

    // Destroyed during play: OnDestroy, and the instance is released.
    life.Destroy(fast);
    AETHER_CHECK(scripts.InstanceCount() == 1 && f.Events().find("destroy:fast") != std::string::npos);
    AETHER_CHECK(!scripts.CallMethod(fast, "Add", {1.0, 2.0}).ok);
    life.EndPlay();
    AETHER_CHECK(scripts.InstanceCount() == 0 && f.Events().find("destroy:spinner") != std::string::npos);
}

AETHER_TEST(ScriptSystem_ErrorsDontStopPlay) {
    Fixture f;
    f.host.Run("events = {}");
    Entity good = f.Make(f.spin, {{"speed", "\"very\""}, {"missing", "1"}}); // two bad overrides
    Entity bad = f.Make(f.broken);
    Entity lost = f.Make(assets::NewAssetGuid()); // no such script
    Lifecycle life(f.world, f.guids);
    ScriptSystem scripts(f.host, f.world, f.guids, f.Loader());
    scripts.Register(life);
    life.BeginPlay();
    // The bad overrides are reported and the defaults used; the missing script too.
    AETHER_CHECK(scripts.Errors().size() == 3);
    AETHER_CHECK(Number(scripts.GetField(good, "speed")) == 180.0);
    AETHER_CHECK(scripts.InstanceCount() == 2);
    (void)lost;
    scripts.ClearErrors();

    // A runtime error in one script's update: recorded with its location,
    // and every other script keeps running.
    life.Update(0.1f);
    life.Update(0.1f);
    AETHER_CHECK(scripts.Errors().size() == 2 && scripts.Errors()[0].find("Broken.luau:3") != std::string::npos);
    AETHER_CHECK(Number(scripts.GetField(good, "count")) == 2.0);
    AETHER_CHECK(f.world.IsAlive(bad));
    life.EndPlay();

    // The component saves with its overrides.
    World loaded;
    AETHER_CHECK(LoadSceneFromMemory(loaded, SaveSceneToMemory(f.world)));
    GuidIndex loaded_guids;
    loaded_guids.Rebuild(loaded);
    const Entity copy = loaded_guids.Find(loaded, f.world.GetComponent<IdComponent>(good)->guid);
    const ScriptComponent* component = loaded.GetComponent<ScriptComponent>(copy);
    AETHER_CHECK(component != nullptr && component->script.guid == f.spin && component->properties.size() == 2);
    AETHER_CHECK(component->FindProperty("speed")->value == "\"very\"");
}
