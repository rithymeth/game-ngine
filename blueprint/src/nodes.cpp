#include "aether/blueprint/nodes.h"
#include "aether/debug/debug_draw.h" // its functions are nodes (Phase 23)

#include "aether/assets/asset_guid.h"
#include "aether/reflection/reflection.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

namespace aether::bp {

using nlohmann::json;

namespace {

// ---------------------------------------------------------------------------
// Pin helpers
// ---------------------------------------------------------------------------

PinDesc ExecIn(const std::string& name = "exec") { return {name, PinDir::In, PinType::Exec(), {}, Pin_None}; }
PinDesc ExecOut(const std::string& name = "then") { return {name, PinDir::Out, PinType::Exec(), {}, Pin_None}; }
// The flags are a PinFlags, not an integer, so a literal default such as
// 0.2f can't be taken for flags by the overload below it.
PinDesc In(const std::string& name, PinType type, PinFlags flags = Pin_None) {
    return {name, PinDir::In, type, DefaultValue(type), flags};
}
PinDesc In(const std::string& name, PinType type, Value value, PinFlags flags = Pin_None) {
    return {name, PinDir::In, type, std::move(value), flags};
}
PinDesc Out(const std::string& name, PinType type) { return {name, PinDir::Out, type, {}, Pin_None}; }

PinType T(ValueType type) { return PinType::Of(type); }

NodeSignature Sig(std::string title, std::string category, NodeKind kind, std::vector<PinDesc> pins) {
    NodeSignature s;
    s.title = std::move(title);
    s.category = std::move(category);
    s.kind = kind;
    s.pins = std::move(pins);
    return s;
}

std::optional<NodeSignature> Fail(NodeError& error, const char* code, std::string message) {
    error.code = code;
    error.message = std::move(message);
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

struct Family {
    NodeFactory factory;
    FamilyLister lister;
};

struct Registry {
    std::map<std::string, NodeFactory> exact;
    std::map<std::string, Family> families; // by prefix
};

struct InterfaceRegistry {
    std::mutex mutex;
    std::map<std::string, BlueprintInterface, std::less<>> interfaces;
};
InterfaceRegistry& Interfaces() {
    static InterfaceRegistry registry;
    return registry;
}
std::vector<std::string> InterfaceNames() {
    InterfaceRegistry& r = Interfaces();
    std::lock_guard<std::mutex> lock(r.mutex);
    std::vector<std::string> names;
    for (const auto& [name, iface] : r.interfaces) names.push_back(name);
    return names;
}

void RegisterBuiltins(Registry& r);

Registry& GetRegistry() {
    static Registry registry;
    static std::once_flag once;
    std::call_once(once, [] { RegisterBuiltins(registry); });
    return registry;
}

// ---------------------------------------------------------------------------
// Built-in node types
// ---------------------------------------------------------------------------

void AddEvent(Registry& r, const std::string& id, const std::string& title, std::vector<PinDesc> outputs) {
    r.exact[id] = [id, title, outputs](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        std::vector<PinDesc> pins{ExecOut()};
        pins.insert(pins.end(), outputs.begin(), outputs.end());
        NodeSignature s = Sig(title, "Events", NodeKind::Event, std::move(pins));
        s.event_key = id;
        return s;
    };
}

std::optional<PinType> OperandType(std::string_view suffix, const std::vector<ValueType>& allowed) {
    std::optional<PinType> type = ParseType(suffix);
    if (!type || type->IsExec() || type->is_array ||
        std::find(allowed.begin(), allowed.end(), type->type) == allowed.end()) {
        return std::nullopt;
    }
    return type;
}

void AddMathFamily(Registry& r, const std::string& prefix, const std::string& title,
                   std::initializer_list<ValueType> allowed, bool returns_bool) {
    const std::vector<ValueType> types(allowed);
    r.families[prefix] = {
        [title, types, returns_bool](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const std::optional<PinType> type = OperandType(c.suffix, types);
            if (!type) {
                return Fail(error, "BP007", "'" + title + "' doesn't take " + std::string(c.suffix) + " values.");
            }
            const PinType result = returns_bool ? T(ValueType::Bool) : *type;
            return Sig(title, "Math|" + TypeName(*type), NodeKind::Pure,
                       {In("a", *type), In("b", *type), Out("result", result)});
        },
        [prefix, title, types](const Blueprint&) {
            std::vector<PaletteEntry> entries;
            for (ValueType t : types) {
                const std::string name = TypeName(T(t));
                entries.push_back({prefix + name, title + " (" + name + ")", "Math|" + name});
            }
            return entries;
        }};
}

void AddPure(Registry& r, const std::string& id, const std::string& title, const std::string& category,
             std::vector<PinDesc> pins) {
    r.exact[id] = [title, category, pins](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig(title, category, NodeKind::Pure, pins);
    };
}

std::vector<PinDesc> ParamsFrom(const std::vector<Variable>& vars, PinDir dir) {
    std::vector<PinDesc> pins;
    for (const Variable& v : vars) {
        pins.push_back({v.name, dir, v.type, dir == PinDir::In ? v.default_value : Value{}, Pin_None});
    }
    return pins;
}

// Custom event parameters from config: [{"name": "amount", "type": "float"}].
bool CustomParams(const json& config, std::vector<Variable>& out, std::string& error) {
    for (const json& p : config.value("params", json::array())) {
        Variable v;
        v.name = p.value("name", "");
        std::optional<PinType> type = ParseType(p.value("type", ""));
        if (v.name.empty() || !type || type->IsExec()) {
            error = "a parameter has no name or an unknown type";
            return false;
        }
        v.type = *type;
        v.default_value = DefaultValue(v.type);
        out.push_back(std::move(v));
    }
    return true;
}

const Node* FindCustomEvent(const Blueprint& bp, std::string_view name) {
    for (const Graph& g : bp.graphs) {
        for (const Node& n : g.nodes) {
            if (n.type == "Event.Custom" && n.config.value("name", "") == name) return &n;
        }
    }
    return nullptr;
}

// "Health.Heal" -> the reflected type and member name (type names may contain "::").
bool SplitMember(std::string_view suffix, const reflect::TypeInfo*& type, std::string& member) {
    const usize dot = suffix.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= suffix.size()) {
        return false;
    }
    type = reflect::TypeRegistry::Find(suffix.substr(0, dot));
    member = std::string(suffix.substr(dot + 1));
    return type != nullptr && type->kind == reflect::TypeKind::Struct;
}

// Component methods go under Components|<type>; static functions (function
// libraries such as Audio's) under their type's name.
static std::string NativeCategory(const reflect::TypeInfo& type, const reflect::FunctionInfo& fn) {
    return fn.HasFlag(reflect::Fn_Static) ? std::string(type.name) : std::string("Components|") + type.name;
}

void RegisterBuiltins(Registry& r) {
    const PinType kBool = T(ValueType::Bool), kInt = T(ValueType::Int), kFloat = T(ValueType::Float),
                  kString = T(ValueType::String), kVec3 = T(ValueType::Vec3), kEntity = T(ValueType::Entity);

    // --- Events (BLUEPRINT_NODES.md §1) ------------------------------------
    AddEvent(r, "Event.BeginPlay", "Event BeginPlay", {});
    AddEvent(r, "Event.EndPlay", "Event EndPlay", {});
    AddEvent(r, "Event.Tick", "Event Tick", {Out("delta_seconds", kFloat)});
    AddEvent(r, "Event.FixedTick", "Event FixedTick", {Out("fixed_delta", kFloat)});
    AddEvent(r, "Event.OnTriggerEnter", "Event OnTriggerEnter", {Out("other", kEntity)});
    AddEvent(r, "Event.OnTriggerExit", "Event OnTriggerExit", {Out("other", kEntity)});
    AddEvent(r, "Event.OnCollisionBegin", "Event OnCollisionBegin", {Out("other", kEntity)});
    AddEvent(r, "Event.OnCollisionEnd", "Event OnCollisionEnd", {Out("other", kEntity)});
    AddEvent(r, "Event.OnCollisionStay", "Event OnCollisionStay", {Out("other", kEntity)});
    // Character movement (Phase 13 step 5). The mode is 0 walking, 1 falling, 2 flying, 3 swimming.
    AddEvent(r, "Event.OnLanded", "Event OnLanded", {Out("impact_speed", kFloat)});
    AddEvent(r, "Event.OnJumped", "Event OnJumped", {});
    AddEvent(r, "Event.OnMovementModeChanged", "Event OnMovementModeChanged", {Out("new_mode", kInt)});
    // Animation (Phase 16 step 4): notifies from clips and montages, state
    // machine changes, and montage progress, from the Animator.
    AddEvent(r, "Event.OnAnimNotify", "Event OnAnimNotify", {Out("name", kString)});
    AddEvent(r, "Event.OnAnimNotifyBegin", "Event OnAnimNotifyBegin", {Out("name", kString)});
    AddEvent(r, "Event.OnAnimNotifyEnd", "Event OnAnimNotifyEnd", {Out("name", kString)});
    AddEvent(r, "Event.OnAnimStateChanged", "Event OnAnimStateChanged",
             {Out("machine", kString), Out("from", kString), Out("to", kString)});
    AddEvent(r, "Event.OnMontageStarted", "Event OnMontageStarted", {Out("montage", kString)});
    AddEvent(r, "Event.OnMontageSectionChanged", "Event OnMontageSectionChanged", {Out("montage", kString), Out("section", kString)});
    AddEvent(r, "Event.OnMontageBlendingOut", "Event OnMontageBlendingOut", {Out("montage", kString), Out("interrupted", kBool)});
    AddEvent(r, "Event.OnMontageEnded", "Event OnMontageEnded", {Out("montage", kString), Out("interrupted", kBool)});
    // Audio (Phase 17 step 4): an AudioSource's cue ended by itself.
    AddEvent(r, "Event.OnAudioFinished", "Event OnAudioFinished", {Out("cue", kString)});
    // Save games (Phase 28 step 4): an async Save Game to Slot finished.
    AddEvent(r, "Event.OnSaveFinished", "Event OnSaveFinished", {Out("slot", kString), Out("success", kBool)});
    // Gameplay (Phase 30 step 2): one of an entity's attributes changed its current value.
    AddEvent(r, "Event.OnAttributeChanged", "Event OnAttributeChanged", {Out("name", kString), Out("old", kFloat), Out("new", kFloat)});
    // Gameplay (Phase 30 step 3): a lasting effect started or ended on an entity.
    AddEvent(r, "Event.OnEffectApplied", "Event OnEffectApplied", {Out("effect", kString), Out("handle", kInt)});
    AddEvent(r, "Event.OnEffectRemoved", "Event OnEffectRemoved", {Out("effect", kString), Out("handle", kInt)});
    // Gameplay (Phase 30 step 4): an ability started, ended (or was cancelled) or couldn't activate.
    AddEvent(r, "Event.OnAbilityActivated", "Event OnAbilityActivated", {Out("ability", kString), Out("handle", kInt)});
    AddEvent(r, "Event.OnAbilityEnded", "Event OnAbilityEnded", {Out("ability", kString), Out("handle", kInt), Out("cancelled", kBool)});
    AddEvent(r, "Event.OnAbilityFailed", "Event OnAbilityFailed", {Out("ability", kString), Out("reason", kString)});
    // Inventory kit (Phase 30 step 7a): items added, removed, equipped, unequipped and used.
    AddEvent(r, "Event.OnItemAdded", "Event OnItemAdded", {Out("item", kString), Out("count", kInt)});
    AddEvent(r, "Event.OnItemRemoved", "Event OnItemRemoved", {Out("item", kString), Out("count", kInt)});
    AddEvent(r, "Event.OnItemEquipped", "Event OnItemEquipped", {Out("item", kString), Out("slot", kString)});
    AddEvent(r, "Event.OnItemUnequipped", "Event OnItemUnequipped", {Out("item", kString), Out("slot", kString)});
    AddEvent(r, "Event.OnItemUsed", "Event OnItemUsed", {Out("item", kString)});
    // Interaction kit (Phase 30 step 7b): something was used (heard by the target), or couldn't be (heard by the user).
    AddEvent(r, "Event.OnInteract", "Event OnInteract", {Out("interactor", kEntity)});
    AddEvent(r, "Event.OnInteractFailed", "Event OnInteractFailed", {Out("target", kEntity), Out("reason", kString)});
    // Quests kit (Phase 30 step 7c): a quest started, advanced, completed or failed.
    AddEvent(r, "Event.OnQuestStarted", "Event OnQuestStarted", {Out("quest", kString)});
    AddEvent(r, "Event.OnQuestProgress", "Event OnQuestProgress", {Out("quest", kString), Out("objective", kString), Out("progress", kInt), Out("required", kInt)});
    AddEvent(r, "Event.OnQuestCompleted", "Event OnQuestCompleted", {Out("quest", kString)});
    AddEvent(r, "Event.OnQuestFailed", "Event OnQuestFailed", {Out("quest", kString)});
    // VFX (Phase 19 step 4): a ParticleSystem's effect is over.
    AddEvent(r, "Event.OnParticleSystemFinished", "Event OnParticleSystemFinished", {Out("asset", kString)});
    // Navigation (Phase 20 step 3): a NavAgent's move ended (arrived, or failed).
    AddEvent(r, "Event.OnMoveCompleted", "Event OnMoveCompleted", {Out("success", kBool)});
    // AI perception (Phase 20 step 5): an AIPerception sensed (or lost sight of) an actor, or forgot it.
    AddEvent(r, "Event.OnTargetPerceived", "Event OnTargetPerceived", {Out("actor", kEntity), Out("sense", kString), Out("sensed", kBool)});
    AddEvent(r, "Event.OnTargetForgotten", "Event OnTargetForgotten", {Out("actor", kEntity)});
    // UI (Phase 18 step 4): a Widget Blueprint's controls and animations.
    AddEvent(r, "Event.OnWidgetClicked", "Event OnWidgetClicked", {Out("widget", kString)});
    AddEvent(r, "Event.OnWidgetValueChanged", "Event OnWidgetValueChanged", {Out("widget", kString), Out("value", kFloat)});
    AddEvent(r, "Event.OnWidgetCheckChanged", "Event OnWidgetCheckChanged", {Out("widget", kString), Out("checked", kBool)});
    AddEvent(r, "Event.OnWidgetTextCommitted", "Event OnWidgetTextCommitted", {Out("widget", kString), Out("text", kString)});
    AddEvent(r, "Event.OnWidgetSelectionChanged", "Event OnWidgetSelectionChanged", {Out("widget", kString), Out("index", kInt)});
    AddEvent(r, "Event.OnWidgetAnimationFinished", "Event OnWidgetAnimationFinished", {Out("animation", kString)});
    r.exact["Event.Custom"] = [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        const std::string name = c.node.config.value("name", "");
        if (name.empty()) return Fail(error, "BP007", "A Custom Event needs a name.");
        std::vector<Variable> params;
        std::string message;
        if (!CustomParams(c.node.config, params, message)) return Fail(error, "BP007", "Custom Event '" + name + "': " + message + ".");
        std::vector<PinDesc> pins{ExecOut()};
        for (PinDesc& p : ParamsFrom(params, PinDir::Out)) pins.push_back(std::move(p));
        NodeSignature s = Sig(name, "Events", NodeKind::Event, std::move(pins));
        s.event_key = "Event.Custom:" + name;
        return s;
    };
    r.families["Call.Custom:"] = {
        [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const Node* event = FindCustomEvent(c.blueprint, c.suffix);
            if (event == nullptr) {
                return Fail(error, "BP004", "The custom event '" + std::string(c.suffix) + "' no longer exists.");
            }
            std::vector<Variable> params;
            std::string message;
            CustomParams(event->config, params, message);
            std::vector<PinDesc> pins{ExecIn(), ExecOut()};
            for (PinDesc& p : ParamsFrom(params, PinDir::In)) pins.push_back(std::move(p));
            return Sig("Call " + std::string(c.suffix), "Events", NodeKind::Impure, std::move(pins));
        },
        [](const Blueprint& bp) {
            std::vector<PaletteEntry> entries;
            for (const Graph& g : bp.graphs)
                for (const Node& n : g.nodes)
                    if (n.type == "Event.Custom" && !n.config.value("name", "").empty()) {
                        const std::string name = n.config.value("name", "");
                        entries.push_back({"Call.Custom:" + name, "Call " + name, "Events"});
                    }
            return entries;
        }};

    // --- Event dispatchers (§10) -------------------------------------------
    auto dispatcher_lister = [](const std::string& prefix, const std::string& verb) {
        return [prefix, verb](const Blueprint& bp) {
            std::vector<PaletteEntry> entries;
            for (const Dispatcher& d : bp.dispatchers) entries.push_back({prefix + d.name, verb + " " + d.name, "Event Dispatchers"});
            return entries;
        };
    };
    auto find_dispatcher = [](const NodeContext& c, NodeError& error) -> const Dispatcher* {
        const Dispatcher* d = c.blueprint.FindDispatcher(c.suffix);
        if (d == nullptr) Fail(error, "BP004", "The event dispatcher '" + std::string(c.suffix) + "' no longer exists.");
        return d;
    };
    r.families["Dispatch.Call:"] = {
        [=](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const Dispatcher* d = find_dispatcher(c, error);
            if (d == nullptr) return std::nullopt;
            std::vector<PinDesc> pins{ExecIn(), In("target", kEntity, Pin_Self)};
            for (PinDesc& p : ParamsFrom(d->params, PinDir::In)) pins.push_back(std::move(p));
            pins.push_back(ExecOut());
            return Sig("Call " + d->name, "Event Dispatchers", NodeKind::Impure, std::move(pins));
        },
        dispatcher_lister("Dispatch.Call:", "Call")};
    // Bind/Unbind: config {"event": "<Custom Event on this Blueprint>"}, whose
    // parameters must match the dispatcher's (BP014).
    auto bind_family = [=](const std::string& verb, bool needs_event) -> Family {
        const std::string prefix = "Dispatch." + verb + ":";
        return {[=](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
                    // Usually another class's dispatcher: bound by name. When
                    // this Blueprint declares one of that name, the handler must match it.
                    const Dispatcher* d = c.blueprint.FindDispatcher(c.suffix);
                    const std::string name(c.suffix);
                    if (name.empty()) return Fail(error, "BP007", verb + " needs a dispatcher name.");
                    if (needs_event) {
                        const std::string event = c.node.config.value("event", "");
                        const Node* handler = FindCustomEvent(c.blueprint, event);
                        if (handler == nullptr) {
                            return Fail(error, "BP004", verb + " " + name + ": the Custom Event '" + event + "' doesn't exist.");
                        }
                        std::vector<Variable> params;
                        std::string message;
                        CustomParams(handler->config, params, message);
                        bool same = d == nullptr || params.size() == d->params.size();
                        for (usize i = 0; d != nullptr && same && i < params.size(); ++i) {
                            same = params[i].type == d->params[i].type;
                        }
                        if (!same) {
                            return Fail(error, "BP014", "The Custom Event '" + event + "' doesn't take the same parameters as '" +
                                                            name + "', so it can't be bound to it.");
                        }
                    }
                    return Sig(verb + " " + name, "Event Dispatchers", NodeKind::Impure,
                               {ExecIn(), In("target", kEntity, Pin_Self), ExecOut()});
                },
                dispatcher_lister(prefix, verb)};
    };
    r.families["Dispatch.Bind:"] = bind_family("Bind", true);
    r.families["Dispatch.Unbind:"] = bind_family("Unbind", true);
    r.families["Dispatch.UnbindAll:"] = bind_family("UnbindAll", false);

    // --- Interfaces (§12.1, §8) ----------------------------------------------
    auto find_interface_fn = [](std::string_view suffix, const BlueprintInterface*& iface, NodeError& error)
        -> const InterfaceFunction* {
        const usize dot = suffix.rfind('.');
        iface = dot == std::string_view::npos ? nullptr : FindBlueprintInterface(suffix.substr(0, dot));
        const InterfaceFunction* fn = iface != nullptr ? iface->Find(suffix.substr(dot + 1)) : nullptr;
        if (fn == nullptr) Fail(error, "BP004", "The interface function '" + std::string(suffix) + "' no longer exists.");
        return fn;
    };
    r.families["Event.Interface:"] = {
        [=](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const BlueprintInterface* iface = nullptr;
            const InterfaceFunction* fn = find_interface_fn(c.suffix, iface, error);
            if (fn == nullptr) return std::nullopt;
            if (!c.blueprint.Implements(iface->name)) {
                return Fail(error, "BP015", "This Blueprint doesn't implement '" + iface->name +
                                                "'. Add it to the Class Settings' interfaces first.");
            }
            std::vector<PinDesc> pins{ExecOut()};
            for (PinDesc& p : ParamsFrom(fn->params, PinDir::Out)) pins.push_back(std::move(p));
            NodeSignature s = Sig("Event " + fn->name + " (" + iface->name + ")", "Interfaces", NodeKind::Event, std::move(pins));
            s.event_key = "Event.Interface:" + std::string(c.suffix);
            return s;
        },
        [](const Blueprint& bp) {
            std::vector<PaletteEntry> entries;
            for (const std::string& name : bp.interfaces)
                if (const BlueprintInterface* iface = FindBlueprintInterface(name))
                    for (const InterfaceFunction& fn : iface->functions)
                        entries.push_back({"Event.Interface:" + name + "." + fn.name, "Event " + fn.name, "Interfaces"});
            return entries;
        }};
    r.families["Interface.Call:"] = {
        [=](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const BlueprintInterface* iface = nullptr;
            const InterfaceFunction* fn = find_interface_fn(c.suffix, iface, error);
            if (fn == nullptr) return std::nullopt;
            std::vector<PinDesc> pins{ExecIn(), In("target", kEntity)};
            for (PinDesc& p : ParamsFrom(fn->params, PinDir::In)) pins.push_back(std::move(p));
            pins.push_back(ExecOut());
            return Sig(fn->name + " (Message)", "Interfaces", NodeKind::Impure, std::move(pins));
        },
        [](const Blueprint&) {
            std::vector<PaletteEntry> entries;
            for (const std::string& name : InterfaceNames())
                if (const BlueprintInterface* iface = FindBlueprintInterface(name))
                    for (const InterfaceFunction& fn : iface->functions)
                        entries.push_back({"Interface.Call:" + name + "." + fn.name, fn.name + " (Message)", "Interfaces"});
            return entries;
        }};
    r.families["Interface.Implements:"] = {
        [kEntity, kBool](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            if (FindBlueprintInterface(c.suffix) == nullptr) {
                return Fail(error, "BP004", "The interface '" + std::string(c.suffix) + "' no longer exists.");
            }
            return Sig("Does Implement " + std::string(c.suffix), "Interfaces", NodeKind::Pure,
                       {In("target", kEntity), Out("result", kBool)});
        },
        [](const Blueprint&) {
            std::vector<PaletteEntry> entries;
            for (const std::string& name : InterfaceNames())
                entries.push_back({"Interface.Implements:" + name, "Does Implement " + name, "Interfaces"});
            return entries;
        }};

    // --- Flow control (§2) -------------------------------------------------
    r.exact["Flow.Branch"] = [kBool](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Branch", "Flow Control", NodeKind::Impure,
                   {ExecIn(), In("condition", kBool, Pin_WarnIfUnconnected), ExecOut("true"), ExecOut("false")});
    };
    r.exact["Flow.Sequence"] = [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        const json count = c.node.config.value("count", json(2));
        if (!count.is_number_integer() || count.get<int>() < 1 || count.get<int>() > 32) {
            return Fail(error, "BP007", "Sequence needs between 1 and 32 outputs.");
        }
        std::vector<PinDesc> pins{ExecIn()};
        for (int i = 0; i < count.get<int>(); ++i) pins.push_back(ExecOut("then " + std::to_string(i)));
        return Sig("Sequence", "Flow Control", NodeKind::Impure, std::move(pins));
    };

    // Stateful flow nodes: their state lives per instance (a compiler "state slot").
    r.exact["Flow.DoOnce"] = [kBool](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Do Once", "Flow Control", NodeKind::Impure,
                   {ExecIn(), ExecIn("reset"), In("start_closed", kBool), ExecOut("completed")});
    };
    r.exact["Flow.DoN"] = [kInt](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Do N", "Flow Control", NodeKind::Impure,
                   {ExecIn("enter"), In("n", kInt, i32{1}), ExecIn("reset"), ExecOut("exit"), Out("counter", kInt)});
    };
    r.exact["Flow.Gate"] = [kBool](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Gate", "Flow Control", NodeKind::Impure,
                   {ExecIn("enter"), ExecIn("open"), ExecIn("close"), ExecIn("toggle"), In("start_closed", kBool),
                    ExecOut("exit")});
    };
    r.exact["Flow.FlipFlop"] = [kBool](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Flip Flop", "Flow Control", NodeKind::Impure,
                   {ExecIn(), ExecOut("A"), ExecOut("B"), Out("is_a", kBool)});
    };

    // --- Latent nodes (§3) -------------------------------------------------
    r.exact["Latent.Delay"] = [kFloat](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Delay", "Latent", NodeKind::Latent, {ExecIn(), In("duration", kFloat, 0.2f), ExecOut("completed")});
    };
    r.exact["Latent.RetriggerableDelay"] = [kFloat](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Retriggerable Delay", "Latent", NodeKind::Latent,
                   {ExecIn(), In("duration", kFloat, 0.2f), ExecOut("completed")});
    };
    r.exact["Latent.DelayNextTick"] = [](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Delay Until Next Tick", "Latent", NodeKind::Latent, {ExecIn(), ExecOut("completed")});
    };

    // --- Variables (§4) ----------------------------------------------------
    auto variable_lister = [](const std::string& prefix, const std::string& verb) {
        return [prefix, verb](const Blueprint& bp) {
            std::vector<PaletteEntry> entries;
            for (const Variable& v : bp.variables) entries.push_back({prefix + v.name, verb + " " + v.name, "Variables"});
            return entries;
        };
    };
    r.families["Var.Get:"] = {
        [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const Variable* v = c.blueprint.FindVariable(c.suffix);
            if (v == nullptr) {
                return Fail(error, "BP005", "The variable '" + std::string(c.suffix) + "' was deleted. Replace or remove this node.");
            }
            return Sig("Get " + v->name, "Variables", NodeKind::Pure, {Out("value", v->type)});
        },
        variable_lister("Var.Get:", "Get")};
    r.families["Var.Set:"] = {
        [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const Variable* v = c.blueprint.FindVariable(c.suffix);
            if (v == nullptr) {
                return Fail(error, "BP005", "The variable '" + std::string(c.suffix) + "' was deleted. Replace or remove this node.");
            }
            return Sig("Set " + v->name, "Variables", NodeKind::Impure,
                       {ExecIn(), In("value", v->type, v->default_value), ExecOut(), Out("value", v->type)});
        },
        variable_lister("Var.Set:", "Set")};

    // Literals: config {"value": ...}.
    for (const auto& [id, type] : std::vector<std::pair<std::string, PinType>>{
             {"Literal.Bool", kBool}, {"Literal.Int", kInt}, {"Literal.Float", kFloat},
             {"Literal.String", kString}, {"Literal.Vec3", kVec3}}) {
        r.exact[id] = [type](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            Value value = DefaultValue(type);
            if (c.node.config.contains("value") && !ValueFromJson(c.node.config["value"], type, value)) {
                return Fail(error, "BP007", "The literal's value isn't a " + TypeName(type) + ".");
            }
            PinDesc out = Out("value", type);
            out.default_value = value;
            return Sig(ValueText(value), "Literals", NodeKind::Pure, {out});
        };
    }

    // --- Math (§5) ---------------------------------------------------------
    using V = ValueType;
    AddMathFamily(r, "Math.Add:", "Add", {V::Int, V::Float, V::Vec3}, false);
    AddMathFamily(r, "Math.Subtract:", "Subtract", {V::Int, V::Float, V::Vec3}, false);
    AddMathFamily(r, "Math.Multiply:", "Multiply", {V::Int, V::Float, V::Vec3}, false);
    AddMathFamily(r, "Math.Divide:", "Divide", {V::Int, V::Float}, false);
    AddMathFamily(r, "Math.Modulo:", "Modulo", {V::Int}, false);
    AddMathFamily(r, "Math.Min:", "Min", {V::Int, V::Float}, false);
    AddMathFamily(r, "Math.Max:", "Max", {V::Int, V::Float}, false);
    AddMathFamily(r, "Math.Less:", "<", {V::Int, V::Float}, true);
    AddMathFamily(r, "Math.LessEqual:", "<=", {V::Int, V::Float}, true);
    AddMathFamily(r, "Math.Greater:", ">", {V::Int, V::Float}, true);
    AddMathFamily(r, "Math.GreaterEqual:", ">=", {V::Int, V::Float}, true);
    AddMathFamily(r, "Math.Equal:", "==", {V::Bool, V::Int, V::Float, V::String, V::Entity}, true);
    AddMathFamily(r, "Math.NotEqual:", "!=", {V::Bool, V::Int, V::Float, V::String, V::Entity}, true);
    AddPure(r, "Math.And", "AND", "Math|bool", {In("a", kBool), In("b", kBool), Out("result", kBool)});
    AddPure(r, "Math.Or", "OR", "Math|bool", {In("a", kBool), In("b", kBool), Out("result", kBool)});
    AddPure(r, "Math.Xor", "XOR", "Math|bool", {In("a", kBool), In("b", kBool), Out("result", kBool)});
    AddPure(r, "Math.Not", "NOT", "Math|bool", {In("a", kBool), Out("result", kBool)});
    AddPure(r, "Math.Negate:float", "Negate (float)", "Math|float", {In("a", kFloat), Out("result", kFloat)});
    AddPure(r, "Math.Abs:float", "Abs (float)", "Math|float", {In("a", kFloat), Out("result", kFloat)});
    AddPure(r, "Math.Clamp:float", "Clamp (float)", "Math|float",
            {In("value", kFloat), In("min", kFloat), In("max", kFloat, 1.0f), Out("result", kFloat)});
    AddPure(r, "Math.Lerp:float", "Lerp (float)", "Math|float",
            {In("a", kFloat), In("b", kFloat, 1.0f), In("alpha", kFloat), Out("result", kFloat)});
    AddPure(r, "Vec3.Make", "Make Vec3", "Math|Vec3",
            {In("x", kFloat), In("y", kFloat), In("z", kFloat), Out("result", kVec3)});
    AddPure(r, "Vec3.Break", "Break Vec3", "Math|Vec3",
            {In("vector", kVec3), Out("x", kFloat), Out("y", kFloat), Out("z", kFloat)});
    AddPure(r, "Vec3.Scale", "Vec3 * float", "Math|Vec3", {In("vector", kVec3), In("scale", kFloat, 1.0f), Out("result", kVec3)});
    AddPure(r, "Vec3.Length", "Length", "Math|Vec3", {In("vector", kVec3), Out("result", kFloat)});
    AddPure(r, "Vec3.Dot", "Dot", "Math|Vec3", {In("a", kVec3), In("b", kVec3), Out("result", kFloat)});
    AddPure(r, "Vec3.Cross", "Cross", "Math|Vec3", {In("a", kVec3), In("b", kVec3), Out("result", kVec3)});
    AddPure(r, "Vec3.Normalize", "Normalize", "Math|Vec3", {In("vector", kVec3), Out("result", kVec3)});

    // More math (§5): unary float functions, rounding to int, int variants,
    // random numbers.
    for (const char* name : {"Sin", "Cos", "Tan", "Asin", "Acos", "Atan", "Sqrt", "Exp", "Log", "Frac",
                             "DegreesToRadians", "RadiansToDegrees"}) {
        AddPure(r, std::string("Math.") + name, name, "Math|float", {In("a", kFloat), Out("result", kFloat)});
    }
    AddPure(r, "Math.Atan2", "Atan2", "Math|float", {In("y", kFloat), In("x", kFloat, 1.0f), Out("result", kFloat)});
    AddPure(r, "Math.Power", "Power", "Math|float", {In("base", kFloat), In("exponent", kFloat, 1.0f), Out("result", kFloat)});
    for (const char* name : {"Floor", "Ceil", "Round", "Truncate"}) {
        AddPure(r, std::string("Math.") + name, name, "Math|float", {In("a", kFloat), Out("result", kInt)});
    }
    AddPure(r, "Math.Negate:int", "Negate (int)", "Math|int", {In("a", kInt), Out("result", kInt)});
    AddPure(r, "Math.Abs:int", "Abs (int)", "Math|int", {In("a", kInt), Out("result", kInt)});
    AddPure(r, "Math.Clamp:int", "Clamp (int)", "Math|int",
            {In("value", kInt), In("min", kInt), In("max", kInt, i32{1}), Out("result", kInt)});
    AddPure(r, "Math.NearlyEqual:float", "Nearly Equal (float)", "Math|float",
            {In("a", kFloat), In("b", kFloat), In("tolerance", kFloat, 0.0001f), Out("result", kBool)});
    AddPure(r, "Math.MapRange:float", "Map Range Clamped", "Math|float",
            {In("value", kFloat), In("in_min", kFloat), In("in_max", kFloat, 1.0f), In("out_min", kFloat),
             In("out_max", kFloat, 1.0f), Out("result", kFloat)});
    AddPure(r, "Math.RandomFloatInRange", "Random Float in Range", "Math|Random",
            {In("min", kFloat), In("max", kFloat, 1.0f), Out("result", kFloat)});
    AddPure(r, "Math.RandomIntInRange", "Random Int in Range", "Math|Random",
            {In("min", kInt), In("max", kInt, i32{1}), Out("result", kInt)});
    AddPure(r, "Math.RandomBool", "Random Bool", "Math|Random", {Out("result", kBool)});

    // --- Loops and switches (§2) --------------------------------------------
    r.exact["Flow.ForLoop"] = [kInt](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("For Loop", "Flow Control", NodeKind::Impure,
                   {ExecIn(), In("first", kInt), In("last", kInt), ExecOut("loop_body"), Out("index", kInt),
                    ExecOut("completed")});
    };
    r.exact["Flow.ForLoopWithBreak"] = [kInt](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("For Loop with Break", "Flow Control", NodeKind::Impure,
                   {ExecIn(), In("first", kInt), In("last", kInt), ExecIn("break"), ExecOut("loop_body"),
                    Out("index", kInt), ExecOut("completed")});
    };
    r.exact["Flow.WhileLoop"] = [kBool](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("While Loop", "Flow Control", NodeKind::Impure,
                   {ExecIn(), In("condition", kBool, Pin_WarnIfUnconnected), ExecOut("loop_body"), ExecOut("completed")});
    };
    r.exact["Flow.SwitchInt"] = [kInt](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        const json cases = c.node.config.value("cases", json::array({0, 1}));
        std::vector<PinDesc> pins{ExecIn(), In("selection", kInt)};
        std::set<i32> seen;
        if (!cases.is_array()) return Fail(error, "BP007", "Switch on Int needs a list of cases.");
        for (const json& k : cases) {
            if (!k.is_number_integer() || !seen.insert(k.get<i32>()).second) {
                return Fail(error, "BP007", "Switch on Int's cases must be distinct whole numbers.");
            }
            pins.push_back(ExecOut(std::to_string(k.get<i32>())));
        }
        pins.push_back(ExecOut("default"));
        return Sig("Switch on Int", "Flow Control", NodeKind::Impure, std::move(pins));
    };
    r.exact["Flow.SwitchString"] = [kString](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        const json cases = c.node.config.value("cases", json::array());
        std::vector<PinDesc> pins{ExecIn(), In("selection", kString)};
        std::set<std::string> seen;
        if (!cases.is_array()) return Fail(error, "BP007", "Switch on String needs a list of cases.");
        for (const json& k : cases) {
            if (!k.is_string() || k.get<std::string>().empty() || k.get<std::string>() == "default" ||
                !seen.insert(k.get<std::string>()).second) {
                return Fail(error, "BP007", "Switch on String's cases must be distinct, non-empty, and not 'default'.");
            }
            pins.push_back(ExecOut(k.get<std::string>()));
        }
        pins.push_back(ExecOut("default"));
        return Sig("Switch on String", "Flow Control", NodeKind::Impure, std::move(pins));
    };
    r.families["Flow.Select:"] = {
        [kInt](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const std::optional<PinType> type = ParseType(c.suffix);
            if (!type || type->IsExec() || type->is_array || type->type == ValueType::Wildcard) {
                return Fail(error, "BP007", "Select can't choose between " + std::string(c.suffix) + " values.");
            }
            const json count = c.node.config.value("count", json(2));
            if (!count.is_number_integer() || count.get<int>() < 2 || count.get<int>() > 16) {
                return Fail(error, "BP007", "Select needs between 2 and 16 options.");
            }
            std::vector<PinDesc> pins{In("index", kInt)};
            for (int i = 0; i < count.get<int>(); ++i) pins.push_back(In("option " + std::to_string(i), *type));
            pins.push_back(Out("return", *type));
            return Sig("Select", "Flow Control", NodeKind::Pure, std::move(pins));
        },
        [](const Blueprint&) {
            std::vector<PaletteEntry> entries;
            for (const char* t : {"bool", "int", "float", "string", "Vec3", "Entity"})
                entries.push_back({std::string("Flow.Select:") + t, std::string("Select (") + t + ")", "Flow Control"});
            return entries;
        }};

    // --- Arrays (§9) and For Each (§2) --------------------------------------
    // One family per operation, by element type: "Array.Add:int".
    struct ArrayOp {
        const char* name;
        const char* title;
        NodeKind kind;
        std::function<std::vector<PinDesc>(const PinType& element, const PinType& array)> pins;
    };
    const PinType kIntT = kInt, kBoolT = kBool;
    const std::vector<ArrayOp> array_ops = {
        {"Make", "Make Array", NodeKind::Pure, nullptr},
        {"Length", "Length", NodeKind::Pure,
         [=](const PinType&, const PinType& a) { return std::vector<PinDesc>{In("array", a), Out("result", kIntT)}; }},
        {"LastIndex", "Last Index", NodeKind::Pure,
         [=](const PinType&, const PinType& a) { return std::vector<PinDesc>{In("array", a), Out("result", kIntT)}; }},
        {"Get", "Get (a copy)", NodeKind::Pure,
         [=](const PinType& e, const PinType& a) {
             return std::vector<PinDesc>{In("array", a), In("index", kIntT), Out("item", e)};
         }},
        {"IsValidIndex", "Is Valid Index", NodeKind::Pure,
         [=](const PinType&, const PinType& a) {
             return std::vector<PinDesc>{In("array", a), In("index", kIntT), Out("result", kBoolT)};
         }},
        {"Find", "Find", NodeKind::Pure,
         [=](const PinType& e, const PinType& a) {
             return std::vector<PinDesc>{In("array", a), In("item", e), Out("index", kIntT)};
         }},
        {"Contains", "Contains", NodeKind::Pure,
         [=](const PinType& e, const PinType& a) {
             return std::vector<PinDesc>{In("array", a), In("item", e), Out("result", kBoolT)};
         }},
        {"Add", "Add", NodeKind::Impure,
         [=](const PinType& e, const PinType& a) {
             return std::vector<PinDesc>{ExecIn(), In("array", a, Pin_ByRef), In("item", e), ExecOut(), Out("index", kIntT)};
         }},
        {"AddUnique", "Add Unique", NodeKind::Impure,
         [=](const PinType& e, const PinType& a) {
             return std::vector<PinDesc>{ExecIn(), In("array", a, Pin_ByRef), In("item", e), ExecOut(), Out("index", kIntT)};
         }},
        {"Insert", "Insert", NodeKind::Impure,
         [=](const PinType& e, const PinType& a) {
             return std::vector<PinDesc>{ExecIn(), In("array", a, Pin_ByRef), In("item", e), In("index", kIntT), ExecOut()};
         }},
        {"RemoveIndex", "Remove Index", NodeKind::Impure,
         [=](const PinType&, const PinType& a) {
             return std::vector<PinDesc>{ExecIn(), In("array", a, Pin_ByRef), In("index", kIntT), ExecOut()};
         }},
        {"RemoveItem", "Remove Item", NodeKind::Impure,
         [=](const PinType& e, const PinType& a) {
             return std::vector<PinDesc>{ExecIn(), In("array", a, Pin_ByRef), In("item", e), ExecOut(), Out("removed", kBoolT)};
         }},
        {"Clear", "Clear", NodeKind::Impure,
         [=](const PinType&, const PinType& a) { return std::vector<PinDesc>{ExecIn(), In("array", a, Pin_ByRef), ExecOut()}; }},
        {"Set", "Set Array Elem", NodeKind::Impure,
         [=](const PinType& e, const PinType& a) {
             return std::vector<PinDesc>{ExecIn(), In("array", a, Pin_ByRef), In("index", kIntT), In("item", e), ExecOut()};
         }},
        {"Reverse", "Reverse", NodeKind::Impure,
         [=](const PinType&, const PinType& a) { return std::vector<PinDesc>{ExecIn(), In("array", a, Pin_ByRef), ExecOut()}; }},
    };
    auto element_of = [](std::string_view suffix, const std::string& title, NodeError& error) -> std::optional<PinType> {
        const std::optional<PinType> e = ParseType(suffix);
        if (!e || e->IsExec() || e->is_array || e->type == ValueType::Struct || e->type == ValueType::Wildcard) {
            Fail(error, "BP007", "'" + title + "' can't hold " + std::string(suffix) + " values.");
            return std::nullopt;
        }
        return e;
    };
    auto element_lister = [](const std::string& prefix, const std::string& title, const std::string& category) {
        return [prefix, title, category](const Blueprint&) {
            std::vector<PaletteEntry> entries;
            for (const char* t : {"bool", "int", "float", "string", "Vec3", "Entity"})
                entries.push_back({prefix + t, title + " (" + t + ")", category});
            return entries;
        };
    };
    for (const ArrayOp& op : array_ops) {
        const std::string prefix = std::string("Array.") + op.name + ":";
        const std::string title = op.title;
        const NodeKind kind = op.kind;
        const bool make = std::string(op.name) == "Make";
        auto pins = op.pins;
        r.families[prefix] = {
            [=](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
                const std::optional<PinType> e = element_of(c.suffix, title, error);
                if (!e) return std::nullopt;
                const PinType array = PinType::ArrayOf(*e);
                std::vector<PinDesc> list;
                if (make) {
                    const json count = c.node.config.value("count", json(0));
                    if (!count.is_number_integer() || count.get<int>() < 0 || count.get<int>() > 16) {
                        return Fail(error, "BP007", "Make Array takes between 0 and 16 items.");
                    }
                    for (int i = 0; i < count.get<int>(); ++i) list.push_back(In("item " + std::to_string(i), *e));
                    list.push_back(Out("array", array));
                } else {
                    list = pins(*e, array);
                }
                return Sig(title, "Array", kind, std::move(list));
            },
            element_lister(prefix, title, "Array")};
    }
    // Sort (in place) and Filter take a function of this Blueprint (config
    // "by"): Sort's "a comes before b" (a: T, b: T) -> bool, Filter's
    // (item: T) -> bool. Sort without one uses the natural order of ints,
    // floats and strings. BP017 when the function doesn't fit.
    auto check_function = [](const NodeContext& c, const PinType& element, usize params, const std::string& what,
                             NodeError& error) -> bool {
        const std::string by = c.node.config.value("by", "");
        const Graph* fn = c.blueprint.FindGraph(by);
        if (fn == nullptr || fn->kind != GraphKind::Function) {
            Fail(error, "BP004", what + " uses the function '" + by + "', which doesn't exist.");
            return false;
        }
        bool fits = fn->inputs.size() == params && fn->outputs.size() == 1 &&
                    fn->outputs[0].type == PinType::Of(ValueType::Bool);
        for (usize i = 0; fits && i < params; ++i) fits = fn->inputs[i].type == element;
        if (!fits) {
            Fail(error, "BP017", "'" + by + "' can't be used by " + what + ": it needs " +
                                     (params == 2 ? "two " + TypeName(element) + " inputs" : "one " + TypeName(element) + " input") +
                                     " and one bool output.");
        }
        return fits;
    };
    r.families["Array.Sort:"] = {
        [=](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const std::optional<PinType> e = element_of(c.suffix, "Sort", error);
            if (!e) return std::nullopt;
            if (c.node.config.contains("by")) {
                if (!check_function(c, *e, 2, "Sort", error)) return std::nullopt;
            } else if (e->type != ValueType::Int && e->type != ValueType::Float && e->type != ValueType::String) {
                return Fail(error, "BP007", "Sorting " + TypeName(*e) + " values needs a comparison function (\"by\").");
            }
            return Sig("Sort", "Array", NodeKind::Impure,
                       {ExecIn(), In("array", PinType::ArrayOf(*e), Pin_ByRef), ExecOut()});
        },
        element_lister("Array.Sort:", "Sort", "Array")};
    r.families["Array.Filter:"] = {
        [=](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const std::optional<PinType> e = element_of(c.suffix, "Filter", error);
            if (!e) return std::nullopt;
            if (!check_function(c, *e, 1, "Filter", error)) return std::nullopt;
            const PinType array = PinType::ArrayOf(*e);
            return Sig("Filter", "Array", NodeKind::Pure, {In("array", array), Out("result", array)});
        },
        element_lister("Array.Filter:", "Filter", "Array")};
    r.families["Flow.ForEach:"] = {
        [=](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const std::optional<PinType> e = element_of(c.suffix, "For Each", error);
            if (!e) return std::nullopt;
            return Sig("For Each", "Flow Control", NodeKind::Impure,
                       {ExecIn(), In("array", PinType::ArrayOf(*e)), ExecOut("loop_body"), Out("element", *e),
                        Out("index", kInt), ExecOut("completed")});
        },
        element_lister("Flow.ForEach:", "For Each", "Flow Control")};

    // --- Strings and text (§9) ----------------------------------------------
    r.exact["String.Append"] = [kString](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        const json count = c.node.config.value("count", json(2));
        if (!count.is_number_integer() || count.get<int>() < 2 || count.get<int>() > 16) {
            return Fail(error, "BP007", "Append needs between 2 and 16 inputs.");
        }
        std::vector<PinDesc> pins;
        for (int i = 0; i < count.get<int>(); ++i) pins.push_back(In(std::string(1, static_cast<char>('a' + i)), kString));
        pins.push_back(Out("result", kString));
        return Sig("Append", "String", NodeKind::Pure, std::move(pins));
    };
    AddPure(r, "String.Length", "Length", "String", {In("text", kString), Out("result", kInt)});
    AddPure(r, "String.IsEmpty", "Is Empty", "String", {In("text", kString), Out("result", kBool)});
    AddPure(r, "String.Contains", "Contains", "String",
            {In("text", kString), In("substring", kString), In("ignore_case", kBool), Out("result", kBool)});
    AddPure(r, "String.ToUpper", "To Upper", "String", {In("text", kString), Out("result", kString)});
    AddPure(r, "String.ToLower", "To Lower", "String", {In("text", kString), Out("result", kString)});
    AddPure(r, "String.Trim", "Trim", "String", {In("text", kString), Out("result", kString)});
    AddPure(r, "String.ToInt", "String to Int", "String",
            {In("text", kString), Out("result", kInt), Out("success", kBool)});
    AddPure(r, "String.ToFloat", "String to Float", "String",
            {In("text", kString), Out("result", kFloat), Out("success", kBool)});
    r.exact["Text.Format"] = [kString](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        const json format = c.node.config.value("format", json(""));
        if (!format.is_string()) return Fail(error, "BP007", "Format Text needs a format string.");
        std::vector<PinDesc> pins;
        for (const std::string& arg : ParseFormatArgs(format.get<std::string>())) {
            if (arg == "result") return Fail(error, "BP007", "Format Text can't have a {result} placeholder.");
            pins.push_back(In(arg, kString)); // anything connects: it's converted to text
        }
        pins.push_back(Out("result", kString));
        return Sig("Format Text", "String", NodeKind::Pure, std::move(pins));
    };
    r.families["Conv.ToString:"] = {
        [kString](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const std::optional<PinType> type = ParseType(c.suffix);
            if (!type || type->IsExec() || type->is_array || CanConnect(*type, PinType::Of(ValueType::String)) == Compat::No ||
                type->type == ValueType::String) {
                return Fail(error, "BP007", "There's no text form for " + std::string(c.suffix) + " values.");
            }
            return Sig("To String (" + TypeName(*type) + ")", "Conversions", NodeKind::Pure,
                       {In("value", *type), Out("result", kString)});
        },
        [](const Blueprint&) {
            std::vector<PaletteEntry> entries;
            for (const char* t : {"bool", "int", "float", "Vec3", "Quat", "Entity"})
                entries.push_back({std::string("Conv.ToString:") + t, std::string("To String (") + t + ")", "Conversions"});
            return entries;
        }};

    // --- Entity (§6) and debug (§11) ---------------------------------------
    AddPure(r, "Entity.Self", "Get Self", "Entity", {Out("self", kEntity)});
    // Transforms (local: relative to the parent, if any), tags, hierarchy,
    // lifetime and time (BLUEPRINT_NODES.md §6).
    const PinType kQuat = T(ValueType::Quat);
    AddPure(r, "Entity.GetLocation", "Get Location", "Entity|Transform",
            {In("target", kEntity, Pin_Self), Out("location", kVec3)});
    AddPure(r, "Entity.GetRotation", "Get Rotation", "Entity|Transform",
            {In("target", kEntity, Pin_Self), Out("rotation", kQuat)});
    AddPure(r, "Entity.GetWorldLocation", "Get World Location", "Entity|Transform",
            {In("target", kEntity, Pin_Self), Out("location", kVec3)});
    auto impure = [&](const std::string& id, const std::string& title, const std::string& category,
                      std::vector<PinDesc> inputs, std::vector<PinDesc> outputs = {}) {
        r.exact[id] = [=](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
            std::vector<PinDesc> pins{ExecIn()};
            pins.insert(pins.end(), inputs.begin(), inputs.end());
            pins.push_back(ExecOut());
            pins.insert(pins.end(), outputs.begin(), outputs.end());
            return Sig(title, category, NodeKind::Impure, pins);
        };
    };
    // Physics queries (Phase 13 step 4): answered by the game's physics
    // through BlueprintVM::SetPhysicsQueries. `layers` is a mask of layer
    // indices (-1 = all); triggers are never hit.
    const std::vector<PinDesc> kTraceHit{Out("hit", kBool), Out("hit_entity", kEntity), Out("location", kVec3),
                                         Out("normal", kVec3), Out("distance", kFloat)};
    impure("Physics.LineTrace", "Line Trace", "Physics",
           {In("start", kVec3), In("end", kVec3), In("layers", kInt, i32{-1}), In("ignore_self", kBool, true)}, kTraceHit);
    impure("Physics.SphereTrace", "Sphere Trace", "Physics",
           {In("start", kVec3), In("end", kVec3), In("radius", kFloat, 0.5f), In("layers", kInt, i32{-1}),
            In("ignore_self", kBool, true)},
           kTraceHit);
    impure("Physics.OverlapSphere", "Overlap Sphere", "Physics",
           {In("center", kVec3), In("radius", kFloat, 1.0f), In("layers", kInt, i32{-1}), In("ignore_self", kBool, true)},
           {Out("entities", PinType::ArrayOf(kEntity))});
    impure("Entity.SetLocation", "Set Location", "Entity|Transform",
           {In("target", kEntity, Pin_Self), In("location", kVec3)});
    impure("Entity.SetRotation", "Set Rotation", "Entity|Transform",
           {In("target", kEntity, Pin_Self), In("rotation", kQuat)});
    impure("Entity.AddOffset", "Add Offset", "Entity|Transform",
           {In("target", kEntity, Pin_Self), In("offset", kVec3)});
    AddPure(r, "Quat.RotateVector", "Rotate Vector", "Math|Rotation",
            {In("rotation", kQuat), In("vector", kVec3, Vec3{0.0f, 0.0f, 1.0f}), Out("result", kVec3)});
    AddPure(r, "Quat.FromAxisAngle", "Rotation from Axis and Angle", "Math|Rotation",
            {In("axis", kVec3, Vec3{0.0f, 1.0f, 0.0f}), In("degrees", kFloat), Out("result", kQuat)});
    AddPure(r, "Quat.Multiply", "Combine Rotations", "Math|Rotation",
            {In("a", kQuat), In("b", kQuat), Out("result", kQuat)});
    AddPure(r, "Entity.HasTag", "Has Tag", "Entity|Tags",
            {In("target", kEntity, Pin_Self), In("tag", kString), Out("result", kBool)});
    AddPure(r, "Entity.FindWithTag", "Find Entities with Tag", "Entity|Tags",
            {In("tag", kString), Out("entities", PinType::ArrayOf(kEntity))});
    impure("Entity.AddTag", "Add Tag", "Entity|Tags", {In("target", kEntity, Pin_Self), In("tag", kString)});
    impure("Entity.RemoveTag", "Remove Tag", "Entity|Tags", {In("target", kEntity, Pin_Self), In("tag", kString)});
    AddPure(r, "Entity.GetParent", "Get Parent", "Entity|Hierarchy",
            {In("target", kEntity, Pin_Self), Out("parent", kEntity)});
    impure("Entity.AttachTo", "Attach To", "Entity|Hierarchy",
           {In("target", kEntity, Pin_Self), In("parent", kEntity)});
    impure("Entity.Detach", "Detach", "Entity|Hierarchy", {In("target", kEntity, Pin_Self)});
    impure("Entity.Destroy", "Destroy Entity", "Entity", {In("target", kEntity, Pin_Self)});
    r.exact["Entity.Spawn"] = [=](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        assets::AssetGuid guid;
        if (!assets::ParseAssetGuid(c.node.config.value("blueprint", ""), guid) || guid.IsNull()) {
            return Fail(error, "BP007", "Spawn needs a Blueprint to spawn (its asset in the node's settings).");
        }
        // Expose on Spawn: the spawned Blueprint's variables the editor lists
        // in config "expose" ([{"name", "type"}]), set before its BeginPlay.
        std::vector<PinDesc> pins{ExecIn(), In("location", kVec3), In("rotation", kQuat)};
        for (const json& e : c.node.config.value("expose", json::array())) {
            const std::string name = e.value("name", "");
            const std::optional<PinType> type = ParseType(e.value("type", ""));
            if (name.empty() || name == "location" || name == "rotation" || !type || type->IsExec() ||
                type->is_array || type->type == ValueType::Struct || type->type == ValueType::Entity) {
                return Fail(error, "BP007", "Spawn can't expose '" + name + "' (a bool, number, string, Vec3 or Quat).");
            }
            pins.push_back(In(name, *type));
        }
        pins.push_back(ExecOut());
        pins.push_back(Out("spawned", kEntity));
        return Sig("Spawn Blueprint", "Entity", NodeKind::Impure, std::move(pins));
    };
    AddPure(r, "World.GameTime", "Get Game Time", "Utilities|Time", {Out("seconds", kFloat)});
    AddPure(r, "World.DeltaSeconds", "Get Delta Seconds", "Utilities|Time", {Out("seconds", kFloat)});
    AddPure(r, "Entity.IsValid", "Is Valid", "Entity", {In("entity", kEntity), Out("result", kBool)});
    r.exact["Debug.Print"] = [kString](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Print String", "Debug", NodeKind::Impure,
                   {ExecIn(), In("text", kString, std::string("Hello")), ExecOut()});
    };

    // --- Functions (§12) ---------------------------------------------------
    r.exact["Function.Entry"] = [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        if (c.graph.kind != GraphKind::Function) return Fail(error, "BP010", "Function Entry belongs in a function graph.");
        std::vector<PinDesc> pins{ExecOut()};
        for (PinDesc& p : ParamsFrom(c.graph.inputs, PinDir::Out)) pins.push_back(std::move(p));
        return Sig(c.graph.name, "Functions", NodeKind::FunctionEntry, std::move(pins));
    };
    r.exact["Function.Return"] = [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        if (c.graph.kind != GraphKind::Function) return Fail(error, "BP010", "Return Node belongs in a function graph.");
        std::vector<PinDesc> pins;
        if (!c.graph.pure) pins.push_back(ExecIn());
        for (PinDesc& p : ParamsFrom(c.graph.outputs, PinDir::In)) pins.push_back(std::move(p));
        return Sig("Return Node", "Functions", NodeKind::FunctionReturn, std::move(pins));
    };
    r.families["Call.Self:"] = {
        [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const Graph* fn = c.blueprint.FindGraph(c.suffix);
            if (fn == nullptr || fn->kind != GraphKind::Function) {
                return Fail(error, "BP004", "The function '" + std::string(c.suffix) + "' no longer exists on this Blueprint.");
            }
            std::vector<PinDesc> pins;
            if (!fn->pure) pins.push_back(ExecIn());
            for (PinDesc& p : ParamsFrom(fn->inputs, PinDir::In)) pins.push_back(std::move(p));
            if (!fn->pure) pins.push_back(ExecOut());
            for (PinDesc& p : ParamsFrom(fn->outputs, PinDir::Out)) pins.push_back(std::move(p));
            return Sig(fn->name, "Functions", fn->pure ? NodeKind::Pure : NodeKind::Impure, std::move(pins));
        },
        [](const Blueprint& bp) {
            std::vector<PaletteEntry> entries;
            for (const Graph& g : bp.graphs)
                if (g.kind == GraphKind::Function) entries.push_back({"Call.Self:" + g.name, g.name, "Functions"});
            return entries;
        }};

    // --- Macros (§12) --------------------------------------------------------
    // A macro graph's tunnels: Macro.Inputs' outputs are the macro's inputs,
    // Macro.Outputs' inputs its outputs. Instances ("Macro:Name") are inlined
    // by the compiler, so none of these ever reach the VM.
    auto macro_pins = [](const std::vector<Variable>& vars, PinDir dir) {
        std::vector<PinDesc> pins;
        for (const Variable& v : vars) {
            if (v.type.IsExec()) pins.push_back({v.name, dir, PinType::Exec(), {}, Pin_None});
            else pins.push_back({v.name, dir, v.type, dir == PinDir::In ? v.default_value : Value{}, Pin_None});
        }
        return pins;
    };
    r.exact["Macro.Inputs"] = [=](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        if (c.graph.kind != GraphKind::Macro) return Fail(error, "BP010", "Macro inputs belong in a macro graph.");
        return Sig("Inputs", "Macros", NodeKind::Tunnel, macro_pins(c.graph.inputs, PinDir::Out));
    };
    r.exact["Macro.Outputs"] = [=](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        if (c.graph.kind != GraphKind::Macro) return Fail(error, "BP010", "Macro outputs belong in a macro graph.");
        return Sig("Outputs", "Macros", NodeKind::Tunnel, macro_pins(c.graph.outputs, PinDir::In));
    };
    r.families["Macro:"] = {
        [=](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const Graph* macro = c.blueprint.FindGraph(c.suffix);
            if (macro == nullptr || macro->kind != GraphKind::Macro) {
                return Fail(error, "BP004", "The macro '" + std::string(c.suffix) + "' no longer exists.");
            }
            std::vector<PinDesc> pins = macro_pins(macro->inputs, PinDir::In);
            for (PinDesc& p : macro_pins(macro->outputs, PinDir::Out)) pins.push_back(std::move(p));
            const bool has_exec = std::any_of(pins.begin(), pins.end(), [](const PinDesc& p) { return p.type.IsExec(); });
            return Sig(macro->name, "Macros", has_exec ? NodeKind::Impure : NodeKind::Pure, std::move(pins));
        },
        [](const Blueprint& bp) {
            std::vector<PaletteEntry> entries;
            for (const Graph& g : bp.graphs)
                if (g.kind == GraphKind::Macro) entries.push_back({"Macro:" + g.name, g.name, "Macros"});
            return entries;
        }};

    // --- Reflection: native calls and component fields (§12.3) -------------
    r.families["Call.Native:"] = {
        [kEntity](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const reflect::TypeInfo* type = nullptr;
            std::string name;
            const reflect::FunctionInfo* fn = nullptr;
            if (SplitMember(c.suffix, type, name)) fn = type->FindFunction(name);
            if (fn == nullptr || !fn->HasFlag(reflect::Fn_BlueprintCallable)) {
                const std::string owner = type != nullptr ? type->name : std::string(c.suffix.substr(0, c.suffix.rfind('.')));
                return Fail(error, "BP004", "The function '" + name + "' no longer exists on '" + owner +
                                                "'. It may have been renamed; check CoreRedirects.");
            }
            const bool pure = fn->HasFlag(reflect::Fn_Pure);
            std::vector<PinDesc> pins;
            if (!pure) pins.push_back(ExecIn());
            if (!fn->HasFlag(reflect::Fn_Static)) pins.push_back(In("target", kEntity, Pin_Self));
            for (const reflect::ParamInfo& param : fn->params) {
                std::optional<PinType> pt = param.type != nullptr ? PinTypeOf(*param.type) : std::nullopt;
                if (!pt) return Fail(error, "BP007", std::string("'") + fn->name + "' takes a parameter Blueprints can't pass.");
                pins.push_back(In(param.name, *pt));
            }
            if (!pure) pins.push_back(ExecOut());
            if (fn->return_type != nullptr) {
                std::optional<PinType> pt = PinTypeOf(*fn->return_type);
                if (!pt) return Fail(error, "BP007", std::string("'") + fn->name + "' returns a type Blueprints can't use.");
                pins.push_back(Out("return", *pt));
            }
            NodeSignature s = Sig(fn->name, NativeCategory(*type, *fn), pure ? NodeKind::Pure : NodeKind::Impure, std::move(pins));
            s.owner = type;
            s.function = fn;
            return s;
        },
        [](const Blueprint&) {
            std::vector<PaletteEntry> entries;
            for (const reflect::TypeInfo* type : reflect::TypeRegistry::AllTypes())
                for (const reflect::FunctionInfo& fn : type->functions)
                    if (fn.HasFlag(reflect::Fn_BlueprintCallable))
                        entries.push_back({std::string("Call.Native:") + type->name + "." + fn.name, fn.name, NativeCategory(*type, fn)});
            return entries;
        }};
    auto field_family = [kEntity](bool set) -> Family {
        const std::string prefix = set ? "Comp.Set:" : "Comp.Get:";
        return {[kEntity, set](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
                    const reflect::TypeInfo* type = nullptr;
                    std::string name;
                    const reflect::FieldInfo* field = nullptr;
                    if (SplitMember(c.suffix, type, name)) field = type->FindField(name);
                    if (field == nullptr || !field->HasFlag(reflect::Field_BlueprintReadWrite)) {
                        return Fail(error, "BP004", "The field '" + name + "' no longer exists on '" +
                                                        (type != nullptr ? std::string(type->name) : std::string(c.suffix)) +
                                                        "' or isn't BlueprintReadWrite.");
                    }
                    std::optional<PinType> pt = PinTypeOf(*field->type);
                    if (!pt) return Fail(error, "BP007", std::string("Blueprints can't use the type of '") + field->name + "'.");
                    NodeSignature s = set ? Sig(std::string("Set ") + field->name, std::string("Components|") + type->name,
                                                NodeKind::Impure,
                                                {ExecIn(), In("target", kEntity, Pin_Self), In("value", *pt), ExecOut(),
                                                 Out("value", *pt)})
                                          : Sig(std::string("Get ") + field->name, std::string("Components|") + type->name,
                                                NodeKind::Pure, {In("target", kEntity, Pin_Self), Out("value", *pt)});
                    s.owner = type;
                    s.field = field;
                    return s;
                },
                [prefix, set](const Blueprint&) {
                    std::vector<PaletteEntry> entries;
                    for (const reflect::TypeInfo* type : reflect::TypeRegistry::AllTypes())
                        for (const reflect::FieldInfo& field : type->fields)
                            if (field.HasFlag(reflect::Field_BlueprintReadWrite))
                                entries.push_back({prefix + type->name + "." + field.name,
                                                   std::string(set ? "Set " : "Get ") + field.name,
                                                   std::string("Components|") + type->name});
                    return entries;
                }};
    };
    r.families["Comp.Get:"] = field_family(false);
    r.families["Comp.Set:"] = field_family(true);
}

} // namespace

const InterfaceFunction* BlueprintInterface::Find(std::string_view function) const {
    for (const InterfaceFunction& fn : functions) {
        if (fn.name == function) return &fn;
    }
    return nullptr;
}

void RegisterBlueprintInterface(BlueprintInterface interface_) {
    InterfaceRegistry& r = Interfaces();
    std::lock_guard<std::mutex> lock(r.mutex);
    const std::string name = interface_.name;
    r.interfaces[name] = std::move(interface_);
}

const BlueprintInterface* FindBlueprintInterface(std::string_view name) {
    InterfaceRegistry& r = Interfaces();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.interfaces.find(name);
    return it != r.interfaces.end() ? &it->second : nullptr; // entries are never removed, so the pointer stays valid
}

std::vector<std::string> ParseFormatArgs(std::string_view format) {
    std::vector<std::string> args;
    for (usize i = 0; i < format.size(); ++i) {
        if (format[i] == '{' && i + 1 < format.size() && format[i + 1] == '{') {
            ++i;
            continue;
        }
        if (format[i] != '{') continue;
        const usize end = format.find('}', i + 1);
        if (end == std::string_view::npos) break;
        const std::string name(format.substr(i + 1, end - i - 1));
        if (!name.empty() && std::find(args.begin(), args.end(), name) == args.end()) args.push_back(name);
        i = end;
    }
    return args;
}

const PinDesc* NodeSignature::Find(std::string_view pin, PinDir dir) const {
    for (const PinDesc& p : pins) {
        if (p.dir == dir && p.name == pin) return &p;
    }
    return nullptr;
}

const PinDesc* NodeSignature::Find(std::string_view pin) const {
    for (const PinDesc& p : pins) {
        if (p.name == pin) return &p;
    }
    return nullptr;
}

void RegisterNodeType(const std::string& id, NodeFactory factory) { GetRegistry().exact[id] = std::move(factory); }

void RegisterNodeFamily(const std::string& prefix, NodeFactory factory, FamilyLister lister) {
    GetRegistry().families[prefix] = {std::move(factory), std::move(lister)};
}

std::optional<NodeSignature> ResolveNode(const Blueprint& blueprint, const Graph& graph, const Node& node,
                                         NodeError* error) {
    Registry& r = GetRegistry();
    NodeError local;
    NodeError& e = error != nullptr ? *error : local;
    if (auto it = r.exact.find(node.type); it != r.exact.end()) {
        return it->second(NodeContext{blueprint, graph, node, {}}, e);
    }
    const usize colon = node.type.find(':');
    if (colon != std::string::npos) {
        if (auto it = r.families.find(node.type.substr(0, colon + 1)); it != r.families.end()) {
            return it->second.factory(
                NodeContext{blueprint, graph, node, std::string_view(node.type).substr(colon + 1)}, e);
        }
    }
    return Fail(e, "BP007", "Unknown node type '" + node.type + "'.");
}

std::vector<PaletteEntry> ListNodeTypes(const Blueprint& blueprint) {
    Registry& r = GetRegistry();
    std::vector<PaletteEntry> entries;
    Graph scratch;
    scratch.kind = GraphKind::EventGraph;
    for (const auto& [id, factory] : r.exact) {
        Node node;
        node.type = id;
        NodeError error;
        if (std::optional<NodeSignature> s = factory(NodeContext{blueprint, scratch, node, {}}, error)) {
            entries.push_back({id, id == "Event.Custom" ? "Custom Event" : s->title, s->category});
        } else if (id == "Event.Custom") {
            entries.push_back({id, "Custom Event", "Events"});
        } else if (id.rfind("Function.", 0) == 0) {
            entries.push_back({id, id == "Function.Entry" ? "Function Entry" : "Return Node", "Functions"});
        }
    }
    for (const auto& [prefix, family] : r.families) {
        if (family.lister) {
            for (PaletteEntry& e : family.lister(blueprint)) entries.push_back(std::move(e));
        }
    }
    std::sort(entries.begin(), entries.end(), [](const PaletteEntry& a, const PaletteEntry& b) {
        return a.category != b.category ? a.category < b.category : a.title != b.title ? a.title < b.title : a.id < b.id;
    });
    return entries;
}

} // namespace aether::bp
