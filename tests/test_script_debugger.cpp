#include "aether/scene/components.h"
#include "aether/script/completion.h"
#include "aether/script/debugger.h"
#include "aether/script/luau_host.h"
#include "test_framework.h"

#include <algorithm>

using namespace aether;
using namespace aether::script;

struct CompletionMover {
    f32 speed = 2.0f;
    Vec3 target{0.0f, 0.0f, 0.0f};
    Transform offset;
    f32 Boost(f32 amount) { return speed += amount; }
};

AETHER_REFLECT(CompletionMover, 1,
    AETHER_FIELD(speed), AETHER_FIELD(target), AETHER_FIELD(offset),
    AETHER_METHOD(Boost, Fn_BlueprintCallable, {"amount"})
)

namespace {

LuauHost::Options DebugOptions() {
    LuauHost::Options options;
    options.allow_debug = true;
    return options;
}

// Lines 1..: the numbers below refer to these.
const char* kCounter = R"(local function add(a, b)
    local sum = a + b
    return sum
end

function Run(n)
    local total = 0
    for i = 1, n do
        total = add(total, i)
    end
    local label = tostring(total)
    return total
end
)";

const DebugVariable* FindLocal(const DebugFrame& frame, const std::string& name) {
    for (const DebugVariable& v : frame.locals) {
        if (v.name == name) {
            return &v;
        }
    }
    return nullptr;
}

bool Has(const CompletionResult& r, const std::string& label, CompletionKind kind) {
    return std::any_of(r.items.begin(), r.items.end(),
                       [&](const CompletionItem& i) { return i.label == label && i.kind == kind; });
}
bool HasLabel(const CompletionResult& r, const std::string& label) {
    return std::any_of(r.items.begin(), r.items.end(), [&](const CompletionItem& i) { return i.label == label; });
}

// Completes at the '|' in `text`.
CompletionResult At(const std::string& text) {
    const usize cursor = text.find('|');
    std::string source = text;
    source.erase(cursor, 1);
    return CompleteScript(source, cursor);
}

} // namespace

AETHER_TEST(ScriptDebugger_BreakpointStopsWithLocalsAndFrames) {
    LuauHost host(DebugOptions());
    ScriptDebugger debugger(host);
    // Set before the chunk is loaded: resolved on load.
    AETHER_CHECK(debugger.SetBreakpoint("counter", 3) == 3);

    std::vector<DebugStop> stops;
    debugger.SetHandler([&](const DebugStop& stop) {
        stops.push_back(stop);
        return DebugAction::Continue;
    });
    AETHER_CHECK(host.Run(kCounter, "counter").ok);
    ScriptResult r = host.Call("Run", {3.0});
    AETHER_CHECK(r.ok && std::get<f64>(r.values[0]) == 6.0);

    // Once per call of add, not once per instruction on the line.
    AETHER_CHECK(stops.size() == 3);
    AETHER_CHECK(debugger.StopCount() == 3);
    const DebugStop& second = stops[1];
    AETHER_CHECK(second.reason == StopReason::Breakpoint && second.chunk == "counter" && second.line == 3);
    AETHER_CHECK(second.frames.size() >= 2);
    const DebugFrame& inner = second.frames[0];
    AETHER_CHECK(inner.function == "add" && inner.line == 3);
    const DebugVariable* a = FindLocal(inner, "a");
    const DebugVariable* sum = FindLocal(inner, "sum");
    AETHER_CHECK(a != nullptr && a->value == "1" && a->type == "number"); // total after i = 1
    AETHER_CHECK(sum != nullptr && sum->value == "3");
    const DebugFrame& outer = second.frames[1];
    AETHER_CHECK(outer.function == "Run" && outer.line == 9);
    const DebugVariable* i = FindLocal(outer, "i");
    AETHER_CHECK(i != nullptr && i->value == "2");
    AETHER_CHECK(FindLocal(outer, "n") != nullptr);
    for (const DebugVariable& v : outer.locals) {
        AETHER_CHECK(v.name[0] != '('); // no temporaries
    }

    // Cleared: runs through.
    AETHER_CHECK(debugger.ClearBreakpoint("counter", 3));
    AETHER_CHECK(debugger.Breakpoints("counter").empty());
    stops.clear();
    AETHER_CHECK(host.Call("Run", {3.0}).ok);
    AETHER_CHECK(stops.empty());
}

AETHER_TEST(ScriptDebugger_LateBreakpointsMoveToCodeAndSurviveReload) {
    LuauHost host(DebugOptions());
    ScriptDebugger debugger(host);
    AETHER_CHECK(host.Run(kCounter, "counter").ok);

    // Line 5 is blank: the breakpoint lands on the next line with code
    // (the function statement on line 6 or the first line of its body).
    const int landed = debugger.SetBreakpoint("counter", 5);
    AETHER_CHECK(landed == 6 || landed == 7);
    AETHER_CHECK(debugger.Breakpoints("counter") == std::vector<int>{landed});
    AETHER_CHECK(debugger.SetBreakpoint("counter", 500) == -1); // past the end

    int hits = 0;
    debugger.SetHandler([&](const DebugStop& stop) {
        ++hits;
        AETHER_CHECK(stop.line == 11);
        return DebugAction::Continue;
    });
    debugger.ClearBreakpoints("counter");
    AETHER_CHECK(debugger.SetBreakpoint("counter", 11) == 11);
    AETHER_CHECK(host.Call("Run", {2.0}).ok);
    AETHER_CHECK(hits == 1);

    // Reloading the chunk (hot reload) re-applies it to the new code.
    AETHER_CHECK(host.Run(kCounter, "counter").ok);
    AETHER_CHECK(host.Call("Run", {2.0}).ok);
    AETHER_CHECK(hits == 2);
}

AETHER_TEST(ScriptDebugger_StepIntoOverOut) {
    LuauHost host(DebugOptions());
    ScriptDebugger debugger(host);
    AETHER_CHECK(host.Run(kCounter, "counter").ok);
    AETHER_CHECK(debugger.SetBreakpoint("counter", 9) == 9);

    // Step Into from line 9 enters add (line 2); Step Over walks add's
    // lines; Step Out returns to Run.
    std::vector<std::pair<std::string, int>> path;
    std::vector<DebugAction> script = {DebugAction::StepInto, DebugAction::StepOver, DebugAction::StepOut,
                                       DebugAction::Continue};
    usize step = 0;
    debugger.SetHandler([&](const DebugStop& stop) {
        path.emplace_back(stop.frames.front().function, stop.line);
        if (step < script.size()) {
            const DebugAction action = script[step++];
            if (step == script.size()) {
                debugger.ClearAllBreakpoints();
            }
            return action;
        }
        return DebugAction::Continue;
    });
    AETHER_CHECK(host.Call("Run", {3.0}).ok);
    AETHER_CHECK(path.size() == 4);
    AETHER_CHECK(path[0] == std::make_pair(std::string("Run"), 9));
    AETHER_CHECK(path[1] == std::make_pair(std::string("add"), 2));
    AETHER_CHECK(path[2] == std::make_pair(std::string("add"), 3));
    AETHER_CHECK(path[3].first == "Run"); // back in the caller
    AETHER_CHECK(path[3].second == 9 || path[3].second == 8);

    // Step Over at line 7 skips over the calls to add.
    path.clear();
    step = 0;
    script = {DebugAction::StepOver, DebugAction::StepOver, DebugAction::StepOver, DebugAction::Continue};
    AETHER_CHECK(debugger.SetBreakpoint("counter", 7) == 7);
    AETHER_CHECK(host.Call("Run", {1.0}).ok);
    AETHER_CHECK(path.size() == 4);
    for (const auto& [function, line] : path) {
        AETHER_CHECK(function == "Run");
    }
    AETHER_CHECK(path[0].second == 7 && path[1].second == 8 && path[2].second == 9);

    // Pause stops at the next line that runs.
    path.clear();
    step = script.size();
    debugger.RequestPause();
    AETHER_CHECK(host.Call("Run", {1.0}).ok);
    AETHER_CHECK(path.size() == 1 && path[0] == std::make_pair(std::string("Run"), 7));
}

AETHER_TEST(ScriptDebugger_ValuesArePrintedSafely) {
    LuauHost host(DebugOptions());
    ScriptDebugger debugger(host);
    AETHER_CHECK(host.Run(R"(function Probe()
    local t = setmetatable({1, 2}, {__tostring = function() error("no") end})
    local s = string.rep("h", t[1]) .. "i"
    local v = vector.create(t[1], 2, 3)
    local b = t[1] == 5
    return 1
end
)", "probe").ok);
    AETHER_CHECK(debugger.SetBreakpoint("probe", 6) == 6);
    DebugFrame frame;
    debugger.SetHandler([&](const DebugStop& stop) {
        frame = stop.frames.front();
        return DebugAction::Continue;
    });
    AETHER_CHECK(host.Call("Probe").ok);
    AETHER_CHECK(FindLocal(frame, "t") != nullptr && FindLocal(frame, "t")->value == "table [2]");
    AETHER_CHECK(FindLocal(frame, "s") != nullptr && FindLocal(frame, "s")->value == "\"hi\"");
    AETHER_CHECK(FindLocal(frame, "v") != nullptr && FindLocal(frame, "v")->value == "(1, 2, 3)");
    AETHER_CHECK(FindLocal(frame, "b") != nullptr && FindLocal(frame, "b")->value == "false");
}

AETHER_TEST(ScriptCompletion_ModulesKeywordsAndLocals) {
    CompletionResult r = At("local speed = 3\nlocal function helper(alpha, beta)\n  return al|\nend");
    AETHER_CHECK(Has(r, "alpha", CompletionKind::Variable));
    AETHER_CHECK(!HasLabel(r, "beta") && !HasLabel(r, "speed"));

    r = At("local speed = 3\nfor index, value in pairs(t) do\n  |");
    AETHER_CHECK(Has(r, "speed", CompletionKind::Variable) && Has(r, "index", CompletionKind::Variable));
    AETHER_CHECK(Has(r, "value", CompletionKind::Variable) && Has(r, "while", CompletionKind::Keyword));
    AETHER_CHECK(Has(r, "Input", CompletionKind::Module) && Has(r, "print", CompletionKind::Global));
    AETHER_CHECK(std::is_sorted(r.items.begin(), r.items.end(), [](const CompletionItem& a, const CompletionItem& b) {
        std::string la = a.label, lb = b.label;
        std::transform(la.begin(), la.end(), la.begin(), ::tolower);
        std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
        return la < lb;
    }));

    // Case-insensitive prefix; replace range covers it.
    const std::string text = "if Input.istr|";
    r = At(text);
    AETHER_CHECK(r.items.size() == 1 && r.items[0].label == "IsTriggered" && r.items[0].kind == CompletionKind::Function);
    AETHER_CHECK(r.replace_from == text.find("istr"));
    AETHER_CHECK(Has(At("Timer.|"), "After", CompletionKind::Function));
    AETHER_CHECK(Has(At("local x = math.fl|"), "floor", CompletionKind::Function));
    AETHER_CHECK(Has(At("world:|"), "Spawn", CompletionKind::Method));
    AETHER_CHECK(At("world.|").items.empty()); // world's functions are methods

    // Nothing in comments, strings, numbers or names being declared.
    AETHER_CHECK(At("-- Input.|").items.empty());
    AETHER_CHECK(At("--[[ long\n Input.| ]]").items.empty());
    AETHER_CHECK(At("print(\"Input.|\")").items.empty());
    AETHER_CHECK(At("local x = 12|").items.empty());
    AETHER_CHECK(At("local ne|").items.empty());
    AETHER_CHECK(At("local s = \"a\" .. |").items.size() > 10); // after concatenation: names
}

AETHER_TEST(ScriptCompletion_ComponentsFromReflection) {
    (void)GetComponentId<Transform>();
    (void)GetComponentId<CompletionMover>();

    // Component names inside :Get("...").
    CompletionResult r = At("local m = self.entity:Get(\"Comp|\")");
    AETHER_CHECK(Has(r, "CompletionMover", CompletionKind::Component));
    AETHER_CHECK(!HasLabel(r, "Transform"));
    AETHER_CHECK(Has(At("world:EntitiesWith('|')"), "Transform", CompletionKind::Component));
    AETHER_CHECK(At("print(\"|\")").items.empty());

    // Fields after '.', functions after ':'.
    const char* mover = "function C:OnUpdate(dt)\n  local m = self.entity:Get(\"CompletionMover\")\n  m.|";
    r = At(mover);
    AETHER_CHECK(Has(r, "speed", CompletionKind::Field) && Has(r, "target", CompletionKind::Field));
    AETHER_CHECK(!HasLabel(r, "Boost"));
    for (const CompletionItem& item : r.items) {
        if (item.label == "speed") {
            AETHER_CHECK(item.detail == "f32");
        }
    }
    r = At("function C:OnUpdate(dt)\n  local m = self.entity:Get(\"CompletionMover\")\n  m:|");
    AETHER_CHECK(r.items.size() == 1 && r.items[0].label == "Boost" && r.items[0].detail == "(amount: f32) -> f32");

    // Through locals holding entities, inline calls, and nested fields.
    AETHER_CHECK(Has(At("local e = world:Spawn()\nlocal m = e:Add(\"CompletionMover\")\nm.ta|"), "target",
                     CompletionKind::Field));
    AETHER_CHECK(Has(At("self.entity:Get(\"CompletionMover\").sp|"), "speed", CompletionKind::Field));
    AETHER_CHECK(Has(At("local e = self.entity\nlocal m = e:Get(\"CompletionMover\")\nm.target.|"), "y",
                     CompletionKind::Field));
    AETHER_CHECK(Has(At("local e = self.entity\ne:Get(\"CompletionMover\").offset.|"), "position",
                     CompletionKind::Field));
    AETHER_CHECK(At("local m = e:Get(\"Nope\")\nm.|").items.empty());

    // Events, connections, timers.
    AETHER_CHECK(Has(At("local hit = Event.new()\nhit:|"), "Connect", CompletionKind::Method));
    AETHER_CHECK(Has(At("local c = Input.OnStarted(\"Jump\"):Connect(f)\nc:|"), "Disconnect", CompletionKind::Method));
    AETHER_CHECK(Has(At("local t = Timer.After(1, f)\nt:|"), "Cancel", CompletionKind::Method));
}

AETHER_TEST(ScriptCompletion_SelfAndCallbacks) {
    const std::string cls = "local Spin = {}\nSpin.speed = 180\nSpin.label = \"x\"\n"
                            "function Spin:Jump() end\n"
                            "function Spin:OnStart() self.count = 0 end\n";
    CompletionResult r = At(cls + "function Spin:OnUpdate(dt)\n  self.|");
    AETHER_CHECK(Has(r, "speed", CompletionKind::Field) && Has(r, "label", CompletionKind::Field));
    AETHER_CHECK(Has(r, "count", CompletionKind::Field) && Has(r, "entity", CompletionKind::Field));
    r = At(cls + "function Spin:OnUpdate(dt)\n  self:|");
    AETHER_CHECK(Has(r, "Jump", CompletionKind::Method) && Has(r, "OnStart", CompletionKind::Method));
    AETHER_CHECK(Has(At(cls + "function Spin:OnUpdate(dt)\n  self.entity:|"), "Get", CompletionKind::Method));
    AETHER_CHECK(Has(At(cls + "function Spin:OnUpdate(dt)\n  |"), "self", CompletionKind::Variable));
    AETHER_CHECK(Has(At(cls + "function Spin:OnUpdate(dt)\n  |"), "dt", CompletionKind::Variable));

    r = At(cls + "function Spin:OnU|");
    AETHER_CHECK(r.items.size() == 1 && r.items[0].label == "OnUpdate" && r.items[0].kind == CompletionKind::Callback);
    AETHER_CHECK(Has(At(cls + "function Spin:|"), "OnCollisionBegin", CompletionKind::Callback));
    AETHER_CHECK(At(cls + "function Spin.|").items.empty());
}
