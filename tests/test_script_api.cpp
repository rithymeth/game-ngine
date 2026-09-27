#include "aether/input/actions.h"
#include "aether/scene/components.h"
#include "aether/script/script_system.h"
#include "test_framework.h"

#include <unordered_map>

using namespace aether;
using namespace aether::script;

namespace {

const char* kListener = R"(
local Listener = {}
Listener.hp = 100
function Listener:OnStart()
    self.ticks = 0
    self.pings = 0
    GameEvents:Connect(function() self.pings += 1 end)   -- owned by this entity
    Timer.Every(0.5, function() self.ticks += 1 end)       -- owned too
end
function Listener:OnHit(amount) self.hp -= amount; return self.hp end
return Listener
)";

struct Fixture {
    World world;
    GuidIndex guids;
    LuauHost host;
    assets::AssetGuid listener = assets::NewAssetGuid();

    Fixture() {
        (void)GetComponentId<ScriptComponent>();
        (void)GetComponentId<Transform>();
    }
    ScriptSystem::SourceLoader Loader() {
        return [this](const assets::AssetGuid& guid, std::string& source, std::string& name) {
            if (guid != listener) {
                return false;
            }
            source = kListener;
            name = "Listener.luau";
            return true;
        };
    }
    Entity Make() {
        ScriptComponent component;
        component.script.guid = listener;
        Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, std::move(component));
        guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
        return e;
    }
    f64 Number(const std::string& expression) {
        ScriptResult r = host.Run("return " + expression);
        return r.ok && std::holds_alternative<f64>(r.values[0]) ? std::get<f64>(r.values[0]) : -1.0;
    }
};

f64 AsNumber(const ScriptValue& v) { return std::holds_alternative<f64>(v) ? std::get<f64>(v) : -1.0; }

} // namespace

AETHER_TEST(ScriptApi_EventsConnectFireDisconnect) {
    Fixture f;
    ScriptSystem scripts(f.host, f.world, f.guids, f.Loader());
    ScriptResult r = f.host.Run(R"(
        local e = Event.new()
        local got = {}
        local c = e:Connect(function(x, y) table.insert(got, x + (y or 0)) end)
        e:Fire(1)
        e:Fire(2, 10)
        local was = c:IsConnected()
        assert(c:Disconnect() and not c:Disconnect())
        e:Fire(3)
        return #got, got[2], was, c:IsConnected()
    )");
    AETHER_CHECK(r.ok && AsNumber(r.values[0]) == 2.0 && AsNumber(r.values[1]) == 12.0);
    AETHER_CHECK(std::get<bool>(r.values[2]) && !std::get<bool>(r.values[3]));

    // A failing handler is reported; the others still run, including one
    // that disconnects itself while the event is firing.
    r = f.host.Run(R"(
        local e = Event.new()
        local count = 0
        e:Connect(function() error("handler blew up") end)
        local self_removing
        self_removing = e:Connect(function() count += 1; self_removing:Disconnect() end)
        e:Connect(function() count += 10 end)
        e:Fire()
        e:Fire()
        return count
    )");
    AETHER_CHECK(r.ok && AsNumber(r.values[0]) == 21.0);
    AETHER_CHECK(scripts.Errors().size() == 2 && scripts.Errors()[0].find("handler blew up") != std::string::npos);
    AETHER_CHECK(!f.host.Run("Event.new():Connect(42)").ok); // needs a function
}

AETHER_TEST(ScriptApi_TimersRunInGameTime) {
    Fixture f;
    ScriptSystem scripts(f.host, f.world, f.guids, f.Loader());
    AETHER_CHECK(f.host.Run(R"(
        once, every = 0, 0
        Timer.After(0.5, function() once += 1 end)
        repeating = Timer.Every(0.25, function() every += 1 end)
        Timer.After(0.1, function() error("timer failed") end)
    )").ok);
    AETHER_CHECK(scripts.TimerCount() == 3);
    scripts.Tick(0.2f);
    AETHER_CHECK(f.Number("once") == 0.0 && f.Number("every") == 0.0 && scripts.Errors().size() == 1);
    scripts.Tick(0.3f); // 0.5 s in
    AETHER_CHECK(f.Number("once") == 1.0 && f.Number("every") == 2.0);
    scripts.Tick(1.0f); // a long frame: the repeating timer catches up
    AETHER_CHECK(f.Number("once") == 1.0 && f.Number("every") == 6.0);
    AETHER_CHECK(f.host.Run("assert(repeating:Cancel()); assert(not repeating:Cancel())").ok);
    scripts.Tick(1.0f);
    AETHER_CHECK(f.Number("every") == 6.0 && scripts.TimerCount() == 0);
    AETHER_CHECK(!f.host.Run("Timer.Every(0, function() end)").ok);
}

AETHER_TEST(ScriptApi_ScriptConnectionsEndWithTheirEntity) {
    Fixture f;
    Lifecycle life(f.world, f.guids);
    ScriptSystem scripts(f.host, f.world, f.guids, f.Loader());
    scripts.Register(life);
    AETHER_CHECK(f.host.Run("GameEvents = Event.new(); outside = 0; GameEvents:Connect(function() outside += 1 end)").ok);
    Entity a = f.Make();
    Entity b = f.Make();
    life.BeginPlay();
    AETHER_CHECK(scripts.ConnectionCount() == 3 && scripts.TimerCount() == 2);
    f.host.Run("GameEvents:Fire()");
    scripts.Tick(1.0f);
    AETHER_CHECK(AsNumber(scripts.GetField(a, "pings")) == 1.0 && AsNumber(scripts.GetField(b, "ticks")) == 2.0);

    // Destroying a disconnects what it connected, and stops its timer; b and
    // the top-level connection carry on.
    life.Destroy(a);
    AETHER_CHECK(scripts.ConnectionCount() == 2 && scripts.TimerCount() == 1);
    f.host.Run("GameEvents:Fire()");
    AETHER_CHECK(AsNumber(scripts.GetField(b, "pings")) == 2.0 && f.Number("outside") == 2.0);
    AETHER_CHECK(scripts.Errors().empty());

    // Entity events: a method called from C++ (e.g. physics hits).
    AETHER_CHECK(scripts.SendEvent(b, "OnHit", {30.0}) && AsNumber(scripts.GetField(b, "hp")) == 70.0);
    AETHER_CHECK(!scripts.SendEvent(b, "OnNothing") && !scripts.SendEvent(a, "OnHit", {1.0}));
    life.EndPlay();
    AETHER_CHECK(scripts.ConnectionCount() == 1 && scripts.TimerCount() == 0);
}

AETHER_TEST(ScriptApi_InputFromScripts) {
    Fixture f;
    ScriptSystem scripts(f.host, f.world, f.guids, f.Loader());
    // Without an input system everything reads as released.
    AETHER_CHECK(f.host.Run("assert(not Input.IsTriggered('Jump')); assert(Input.GetAxis1D('Move') == 0)").ok);

    input::InputSystem input;
    input::InputMappingContext context;
    context.name = "OnFoot";
    input::InputBinding jump;
    jump.action = "Jump";
    jump.key = input::Key::Space;
    input::InputTrigger pressed;
    pressed.type = input::TriggerType::Pressed;
    jump.triggers = {pressed};
    input::InputBinding right;
    right.action = "Move";
    right.key = input::Key::D;
    context.bindings = {jump, right};
    input.AddContext(context);
    scripts.BindInput(&input);

    AETHER_CHECK(f.host.Run(R"(
        jumps, released = 0, 0
        Input.OnTriggered("Jump"):Connect(function(value) jumps += value.x end)
        Input.OnCompleted("Jump"):Connect(function() released += 1 end)
        function ReadMove() return Input.GetAxis2D("Move").x, Input.IsTriggered("Move") end
    )").ok);
    input::InputState state;
    state.SetButton(input::Key::Space, true);
    state.SetButton(input::Key::D, true);
    input.Update(state, 0.1f);
    ScriptResult move = f.host.Call("ReadMove");
    AETHER_CHECK(move.ok && AsNumber(move.values[0]) == 1.0 && std::get<bool>(move.values[1]));
    input.Update(state, 0.1f); // still held: Pressed has completed
    AETHER_CHECK(f.Number("jumps") == 1.0 && f.Number("released") == 1.0);

    scripts.BindInput(nullptr);
    state.SetButton(input::Key::Space, false);
    input.Update(state, 0.1f);
    state.SetButton(input::Key::Space, true);
    input.Update(state, 0.1f);
    AETHER_CHECK(f.Number("jumps") == 1.0); // unbound: no more events reach scripts
}
