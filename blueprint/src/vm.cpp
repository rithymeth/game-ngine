#include "aether/blueprint/vm.h"

#include "aether/core/log.h"
#include "aether/ecs/world.h"
#include "aether/reflection/reflection.h"
#include "aether/assets/asset_guid.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace aether::bp {

namespace {

Reg RegOf(const VmValue& value, const PinType& type) {
    if (const bool* b = std::get_if<bool>(&value)) return Reg::Bool(*b);
    if (const i32* i = std::get_if<i32>(&value)) return type.type == ValueType::Float ? Reg::Float(static_cast<f32>(*i)) : Reg::Int(*i);
    if (const f32* f = std::get_if<f32>(&value)) return type.type == ValueType::Int ? Reg::Int(static_cast<i32>(*f)) : Reg::Float(*f);
    if (const Vec3* v = std::get_if<Vec3>(&value)) return Reg::Vector(*v);
    if (const Quaternion* q = std::get_if<Quaternion>(&value)) return Reg::Quat(*q);
    if (const Entity* e = std::get_if<Entity>(&value)) return Reg::EntityOf(*e);
    return type.type == ValueType::Entity ? Reg::EntityOf(kNullEntity) : Reg{};
}

VmValue ValueOf(const Reg& r, const PinType& type) {
    switch (type.type) {
    case ValueType::Bool: return r.b;
    case ValueType::Int: return r.i;
    case ValueType::Float: return r.f;
    case ValueType::Vec3: return r.AsVec3();
    case ValueType::Quat: return r.AsQuat();
    case ValueType::Entity: return r.AsEntity();
    default: return std::monostate{};
    }
}

std::string Text(const Reg& r, ValueType type) {
    char buffer[96];
    switch (type) {
    case ValueType::Bool: return r.b ? "true" : "false";
    case ValueType::Int: return std::to_string(r.i);
    case ValueType::Float:
        std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(r.f));
        return buffer;
    case ValueType::Vec3:
        std::snprintf(buffer, sizeof(buffer), "(%g, %g, %g)", static_cast<double>(r.v[0]),
                      static_cast<double>(r.v[1]), static_cast<double>(r.v[2]));
        return buffer;
    case ValueType::Quat:
        std::snprintf(buffer, sizeof(buffer), "(%g, %g, %g, %g)", static_cast<double>(r.v[0]),
                      static_cast<double>(r.v[1]), static_cast<double>(r.v[2]), static_cast<double>(r.v[3]));
        return buffer;
    case ValueType::Entity: {
        const Entity e = r.AsEntity();
        if (e.IsNull()) return "None";
        std::snprintf(buffer, sizeof(buffer), "Entity(%uv%u)", e.index, e.generation);
        return buffer;
    }
    default: return "";
    }
}

reflect::Any AnyOf(const Reg& r, const std::string& s, const PinType& type) {
    switch (type.type) {
    case ValueType::Bool: return reflect::Any(r.b);
    case ValueType::Int: return reflect::Any(r.i);
    case ValueType::Float: return reflect::Any(r.f);
    case ValueType::String: return reflect::Any(s);
    case ValueType::Vec3: return reflect::Any(r.AsVec3());
    case ValueType::Quat: return reflect::Any(r.AsQuat());
    case ValueType::Entity: return reflect::Any(r.AsEntity());
    default: return {};
    }
}

void StoreAny(const reflect::Any& any, const PinType& type, Reg& r, std::string& s) {
    switch (type.type) {
    case ValueType::Bool: if (auto* v = any.TryGet<bool>()) r = Reg::Bool(*v); break;
    case ValueType::Int: if (auto* v = any.TryGet<i32>()) r = Reg::Int(*v); break;
    case ValueType::Float: if (auto* v = any.TryGet<f32>()) r = Reg::Float(*v); break;
    case ValueType::String: if (auto* v = any.TryGet<std::string>()) s = *v; break;
    case ValueType::Vec3: if (auto* v = any.TryGet<Vec3>()) r = Reg::Vector(*v); break;
    case ValueType::Quat: if (auto* v = any.TryGet<Quaternion>()) r = Reg::Quat(*v); break;
    case ValueType::Entity: if (auto* v = any.TryGet<Entity>()) r = Reg::EntityOf(*v); break;
    default: break;
    }
}

// Items compare by their bytes (registers are zero-filled, so unused lanes match).
i32 FindValue(const ArrayValue& array, const Reg& item) {
    for (usize i = 0; i < array.values.size(); ++i) {
        if (std::memcmp(&array.values[i], &item, sizeof(Reg)) == 0) return static_cast<i32>(i);
    }
    return -1;
}

i32 FindText(const ArrayValue& array, const std::string& item) {
    for (usize i = 0; i < array.texts.size(); ++i) {
        if (array.texts[i] == item) return static_cast<i32>(i);
    }
    return -1;
}

} // namespace

BlueprintVM::BlueprintVM(World& world, Options options)
    : world_(world), options_(options), rng_(options.random_seed != 0 ? options.random_seed : std::random_device{}()) {
    frames_.resize(static_cast<usize>(options_.max_call_depth) + 1);
    print_ = [](Entity, const std::string& text) { AETHER_LOG_INFO("Blueprint", "%s", text.c_str()); };
}

BlueprintVM::Instance* BlueprintVM::Find(Entity e) {
    auto it = instances_.find(Key(e));
    return it != instances_.end() && !it->second->detached ? it->second.get() : nullptr;
}

const BlueprintVM::Instance* BlueprintVM::Find(Entity e) const { return const_cast<BlueprintVM*>(this)->Find(e); }

bool BlueprintVM::Attach(Entity entity, std::shared_ptr<const CompiledBlueprint> blueprint,
                         const nlohmann::json& overrides) {
    if (!blueprint || !world_.IsAlive(entity)) {
        return false;
    }
    if (depth_ > 0 && instances_.count(Key(entity)) != 0) {
        return false; // can't replace an instance while scripts run
    }
    auto instance = std::make_unique<Instance>();
    instance->entity = entity;
    instance->vars.resize(blueprint->value_vars);
    instance->states.resize(blueprint->state_slots);
    instance->svars.resize(blueprint->string_vars);
    instance->avars.resize(blueprint->array_vars);
    for (const CompiledVariable& var : blueprint->variables) {
        Reg value = var.default_value;
        std::string text = var.default_string;
        if ((var.flags & (Var_InstanceEditable | Var_ExposeOnSpawn)) && overrides.is_object() && overrides.contains(var.name)) {
            Value parsed;
            if (ValueFromJson(overrides[var.name], var.type, parsed)) {
                if (const std::string* s = std::get_if<std::string>(&parsed)) {
                    text = *s;
                } else {
                    VmValue v;
                    std::visit([&](const auto& x) {
                        using X = std::decay_t<decltype(x)>;
                        if constexpr (!std::is_same_v<X, std::string>) v = x;
                    }, parsed);
                    value = RegOf(v, var.type);
                }
            }
        }
        if (var.slot.bank == Bank::Array) {
            ArrayValue& array = instance->avars[var.slot.index];
            array.type = var.type.type;
            array.strings = var.type.type == ValueType::String;
        } else if (var.slot.bank == Bank::String) instance->svars[var.slot.index] = std::move(text);
        else instance->vars[var.slot.index] = value;
    }
    instance->blueprint = std::move(blueprint);
    const u64 key = Key(entity);
    if (instances_.count(key) == 0) {
        order_.push_back(key);
    } else {
        latent_.erase(std::remove_if(latent_.begin(), latent_.end(), [&](const LatentAction& a) { return a.owner == key; }),
                      latent_.end());
    }
    instances_[key] = std::move(instance);
    return true;
}

void BlueprintVM::Detach(Entity entity) {
    auto it = instances_.find(Key(entity));
    if (it == instances_.end()) return;
    const u64 key = Key(entity);
    // Its dispatchers go, and so do its bindings to others'.
    for (auto b = bindings_.begin(); b != bindings_.end();) {
        if (b->first.first == key) {
            b = bindings_.erase(b);
            continue;
        }
        auto& list = b->second;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const auto& l) { return l.first == key; }), list.end());
        ++b;
    }
    latent_.erase(std::remove_if(latent_.begin(), latent_.end(), [&](const LatentAction& a) { return a.owner == key; }),
                  latent_.end()); // an entity's pending actions go with it
    if (depth_ > 0) {
        it->second->detached = true; // it may be running: remove when the outermost run ends
        detached_keys_.push_back(it->first);
        return;
    }
    instances_.erase(it);
    order_.erase(std::remove(order_.begin(), order_.end(), Key(entity)), order_.end());
}

void BlueprintVM::Finish() {
    RemoveDetached();
    ProcessDestroys();
}

void BlueprintVM::ProcessDestroys() {
    if (destroying_) return; // EndPlay dispatched below finishes here too
    destroying_ = true;
    while (!pending_destroy_.empty()) {
        std::vector<Entity> batch;
        batch.swap(pending_destroy_);
        for (Entity e : batch) {
            if (!world_.IsAlive(e)) continue;
            if (destroy_) {
                destroy_(e);
                continue;
            }
            if (IsAttached(e)) {
                Dispatch(e, "Event.EndPlay");
                Detach(e);
            }
            world_.DestroyEntity(e);
        }
    }
    RemoveDetached();
    destroying_ = false;
}

Transform* BlueprintVM::TransformOf(Instance& instance, const CompiledFunction& fn, NodeId node, Entity target) {
    Transform* t = world_.IsAlive(target) ? world_.GetComponent<Transform>(target) : nullptr;
    if (t == nullptr) {
        Warn("BP201", instance, fn, node,
             "Used the transform of an entity that is gone or has no Transform in '" + fn.name +
                 "'. Use Is Valid first.");
    }
    return t;
}

void BlueprintVM::RemoveDetached() {
    // Only the instances detached while running are waiting: this runs after
    // every outermost dispatch, so it must not scan every instance.
    if (detached_keys_.empty()) return;
    std::vector<u64> keys;
    keys.swap(detached_keys_);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    for (u64 key : keys) {
        auto it = instances_.find(key);
        if (it != instances_.end() && it->second->detached) instances_.erase(it);
    }
    order_.erase(std::remove_if(order_.begin(), order_.end(),
                                [&](u64 key) {
                                    return std::binary_search(keys.begin(), keys.end(), key) && instances_.count(key) == 0;
                                }),
                 order_.end());
}

bool BlueprintVM::IsAttached(Entity entity) const { return Find(entity) != nullptr; }

BlueprintVM::Frame& BlueprintVM::AcquireFrame(const CompiledFunction& fn, u32 depth) {
    Frame& frame = frames_[depth];
    frame.r.assign(fn.value_regs, Reg{});
    if (frame.s.size() < fn.string_regs) frame.s.resize(fn.string_regs);
    for (u16 i = 0; i < fn.string_regs; ++i) frame.s[i].clear();
    if (frame.a.size() < fn.array_regs) frame.a.resize(fn.array_regs);
    for (u16 i = 0; i < fn.array_regs; ++i) frame.a[i] = ArrayValue{};
    return frame;
}

bool BlueprintVM::Dispatch(Entity entity, std::string_view event, std::span<const VmValue> args) {
    Instance* instance = Find(entity);
    if (instance == nullptr) return false;
    const CompiledBlueprint& bp = *instance->blueprint;
    auto it = bp.events.find(event);
    if (it == bp.events.end()) return false;
    return Invoke(*instance, it->second, args);
}

bool BlueprintVM::Invoke(Instance& instance_ref, u32 function, std::span<const VmValue> args) {
    Instance* instance = &instance_ref;
    const Entity entity = instance->entity;
    const CompiledFunction& fn = instance->blueprint->functions[function];
    if (depth_ >= frames_.size()) {
        errors_.push_back({"BP203", entity, fn.name, 0, "Blueprint events nested too deeply."});
        return false;
    }
    const bool outermost = depth_ == 0;
    if (outermost) budget_left_ = options_.instruction_budget;
    Frame& frame = AcquireFrame(fn, depth_);
    for (usize i = 0; i < fn.params.size() && i < args.size(); ++i) {
        const RegRef p = fn.params[i];
        if (p.bank == Bank::Array) {
            continue; // arrays can't be passed in from C++ yet
        } else if (p.bank == Bank::String) {
            if (const std::string* s = std::get_if<std::string>(&args[i])) frame.s[p.index] = *s;
        } else {
            frame.r[p.index] = RegOf(args[i], fn.param_types[i]);
        }
    }
    const bool ok = Run(*instance, function, frame, depth_);
    if (outermost) Finish();
    return ok;
}

void BlueprintVM::SetEnabled(Entity entity, bool enabled) {
    if (Instance* instance = Find(entity)) instance->enabled = enabled;
}

void BlueprintVM::ResumeDue() {
    // Take the due actions out first: resuming may start new ones.
    std::vector<LatentAction> due;
    for (auto it = latent_.begin(); it != latent_.end();) {
        auto owner = instances_.find(it->owner);
        const bool ready = it->wake_frame != 0 ? frame_ >= it->wake_frame : time_ >= it->wake_time;
        if (owner == instances_.end() || owner->second->detached || !world_.IsAlive(owner->second->entity)) {
            it = latent_.erase(it); // the owner is gone: dropped
        } else if (ready && owner->second->enabled) {
            due.push_back(std::move(*it));
            it = latent_.erase(it);
        } else {
            ++it;
        }
    }
    std::sort(due.begin(), due.end(), [](const LatentAction& a, const LatentAction& b) {
        return a.wake_time != b.wake_time ? a.wake_time < b.wake_time : a.order < b.order;
    });
    for (LatentAction& action : due) {
        auto owner = instances_.find(action.owner);
        if (owner == instances_.end() || owner->second->detached) continue;
        Instance& instance = *owner->second;
        instance.states[action.slot].pending = false;
        const CompiledFunction& fn = instance.blueprint->functions[action.function];
        budget_left_ = options_.instruction_budget;
        Frame& frame = AcquireFrame(fn, 0);
        frame.r = std::move(action.r);
        for (usize i = 0; i < action.s.size(); ++i) frame.s[i] = std::move(action.s[i]);
        for (usize i = 0; i < action.a.size(); ++i) frame.a[i] = std::move(action.a[i]);
        Run(instance, action.function, frame, 0, action.resume_pc);
        Finish();
    }
}

bool BlueprintVM::CallPredicate(Instance& instance, u32 function, const ArrayValue& array, usize a, const usize* b,
                                u32 depth, bool& result) {
    const CompiledFunction& callee = instance.blueprint->functions[function];
    if (depth + 1 >= frames_.size()) {
        errors_.push_back({"BP203", instance.entity, callee.name, 0,
                           "'" + callee.name + "' was called more than " + std::to_string(options_.max_call_depth) +
                               " levels deep. Check for endless recursion."});
        return false;
    }
    Frame& frame = AcquireFrame(callee, depth + 1);
    for (usize k = 0; k < callee.params.size() && k < (b != nullptr ? 2u : 1u); ++k) {
        const usize item = k == 0 ? a : *b;
        const RegRef p = callee.params[k];
        if (p.bank == Bank::String) frame.s[p.index] = array.texts[item];
        else frame.r[p.index] = array.values[item];
    }
    if (!Run(instance, function, frame, depth + 1)) return false;
    result = !callee.results.empty() && frame.r[callee.results[0].index].b;
    return true;
}

void BlueprintVM::StartLatent(Instance& instance, u32 function, const Instr& in, Frame& frame) {
    const CompiledBlueprint& bp = *instance.blueprint;
    NodeState& state = instance.states[in.a];
    const LatentKind kind = static_cast<LatentKind>(in.c);
    const u64 owner = Key(instance.entity);
    const f32 duration = kind == LatentKind::NextTick ? 0.0f : std::max(0.0f, frame.r[in.b].f);
    if (state.pending) {
        if (kind != LatentKind::RetriggerableDelay) return; // calling again while waiting is ignored
        for (LatentAction& action : latent_) {
            if (action.owner == owner && action.slot == in.a) {
                action.wake_time = time_ + duration; // restart the timer (with the new frame)
                action.r = frame.r;
                action.s.assign(frame.s.begin(), frame.s.begin() + bp.functions[function].string_regs);
                action.a.assign(frame.a.begin(), frame.a.begin() + bp.functions[function].array_regs);
                return;
            }
        }
    }
    LatentAction action;
    action.owner = owner;
    action.function = function;
    action.resume_pc = bp.latents[static_cast<usize>(in.d)].resume_pc;
    action.slot = in.a;
    action.node = bp.latents[static_cast<usize>(in.d)].node;
    action.wake_time = time_ + duration;
    action.wake_frame = kind == LatentKind::NextTick ? frame_ + 1 : 0;
    action.order = latent_order_++;
    action.r = frame.r;
    action.s.assign(frame.s.begin(), frame.s.begin() + bp.functions[function].string_regs);
    action.a.assign(frame.a.begin(), frame.a.begin() + bp.functions[function].array_regs);
    latent_.push_back(std::move(action));
    state.pending = true;
}

void BlueprintVM::Tick(f32 delta_seconds) {
    last_delta_ = delta_seconds;
    time_ += delta_seconds;
    ++frame_;
    ResumeDue();
    const VmValue dt = delta_seconds;
    const std::vector<u64> order = order_; // stable while events attach or detach
    for (u64 key : order) {
        auto it = instances_.find(key);
        if (it == instances_.end() || it->second->detached) continue;
        if (!world_.IsAlive(it->second->entity)) {
            Detach(it->second->entity); // destroyed behind the VM's back
            continue;
        }
        if (!it->second->enabled) continue;
        const auto& events = it->second->blueprint->events;
        const auto tick = events.find(std::string_view("Event.Tick"));
        if (tick == events.end()) continue;
        Invoke(*it->second, tick->second, std::span<const VmValue>(&dt, 1));
    }
}

void BlueprintVM::BeginPlay() {
    const std::vector<u64> order = order_;
    for (u64 key : order) {
        auto it = instances_.find(key);
        if (it != instances_.end() && !it->second->detached) Dispatch(it->second->entity, "Event.BeginPlay");
    }
}

VmValue BlueprintVM::GetVariable(Entity entity, std::string_view name) const {
    const Instance* instance = Find(entity);
    if (instance == nullptr) return std::monostate{};
    const CompiledVariable* var = instance->blueprint->FindVariable(name);
    if (var == nullptr) return std::monostate{};
    if (var->slot.bank == Bank::Array) return std::monostate{}; // see GetArray
    if (var->slot.bank == Bank::String) return instance->svars[var->slot.index];
    return ValueOf(instance->vars[var->slot.index], var->type);
}

bool BlueprintVM::SetVariable(Entity entity, std::string_view name, const VmValue& value) {
    Instance* instance = Find(entity);
    if (instance == nullptr) return false;
    const CompiledVariable* var = instance->blueprint->FindVariable(name);
    if (var == nullptr) return false;
    if (var->slot.bank == Bank::Array) return false; // see SetArray
    if (var->slot.bank == Bank::String) {
        const std::string* s = std::get_if<std::string>(&value);
        if (s == nullptr) return false;
        instance->svars[var->slot.index] = *s;
    } else {
        if (std::holds_alternative<std::string>(value) || std::holds_alternative<std::monostate>(value)) return false;
        instance->vars[var->slot.index] = RegOf(value, var->type);
    }
    return true;
}

std::vector<VmValue> BlueprintVM::GetArray(Entity entity, std::string_view name) const {
    std::vector<VmValue> items;
    const Instance* instance = Find(entity);
    const CompiledVariable* var = instance != nullptr ? instance->blueprint->FindVariable(name) : nullptr;
    if (var == nullptr || var->slot.bank != Bank::Array) return items;
    const ArrayValue& array = instance->avars[var->slot.index];
    const PinType element = PinType::Of(var->type.type);
    if (array.strings) {
        for (const std::string& text : array.texts) items.push_back(text);
    } else {
        for (const Reg& r : array.values) items.push_back(ValueOf(r, element));
    }
    return items;
}

bool BlueprintVM::SetArray(Entity entity, std::string_view name, const std::vector<VmValue>& items) {
    Instance* instance = Find(entity);
    const CompiledVariable* var = instance != nullptr ? instance->blueprint->FindVariable(name) : nullptr;
    if (var == nullptr || var->slot.bank != Bank::Array) return false;
    ArrayValue& array = instance->avars[var->slot.index];
    array.Clear();
    const PinType element = PinType::Of(var->type.type);
    for (const VmValue& item : items) {
        if (array.strings) {
            const std::string* text = std::get_if<std::string>(&item);
            if (text == nullptr) return false;
            array.texts.push_back(*text);
        } else {
            array.values.push_back(RegOf(item, element));
        }
    }
    return true;
}

std::vector<VmValue> BlueprintVM::ArgsOf(const std::vector<TypedReg>& args, const Frame& frame) const {
    std::vector<VmValue> values(args.size()); // filled in place (GCC mis-warns on moved-in variants)
    for (usize i = 0; i < args.size(); ++i) {
        const TypedReg& arg = args[i];
        if (arg.reg.bank == Bank::String) values[i] = frame.s[arg.reg.index];
        else if (arg.reg.bank != Bank::Array) values[i] = ValueOf(frame.r[arg.reg.index], arg.type); // arrays don't cross yet
    }
    return values;
}

void BlueprintVM::SetDebugHandler(std::function<BpAction(const BpStop&)> handler) {
    debug_handler_ = std::move(handler);
    debugging_ = static_cast<bool>(debug_handler_) || tracing_;
    if (!debug_handler_) step_ = StepMode::None;
}

void BlueprintVM::SetTraceEnabled(bool enabled, usize capacity) {
    tracing_ = enabled;
    trace_capacity_ = std::max<usize>(1, capacity);
    debugging_ = static_cast<bool>(debug_handler_) || tracing_;
}

std::vector<BpTraceEvent> BlueprintVM::TakeTrace() {
    std::vector<BpTraceEvent> out;
    out.swap(trace_);
    return out;
}

bool BlueprintVM::SetBreakpoint(const CompiledBlueprint& blueprint, const std::string& graph, NodeId node, bool enabled) {
    bool exists = false;
    for (const CompiledFunction& fn : blueprint.functions) {
        if (fn.graph != graph) continue;
        for (const auto& [pc, entered] : fn.entries) exists |= entered == node;
    }
    const auto key = std::make_tuple(&blueprint, graph, node);
    breakpoints_.erase(std::remove(breakpoints_.begin(), breakpoints_.end(), key), breakpoints_.end());
    if (enabled && exists) breakpoints_.push_back(key);
    return exists;
}

void BlueprintVM::DebugHook(Instance& instance, u32 function, usize pc) {
    const CompiledBlueprint& bp = *instance.blueprint;
    const CompiledFunction& fn = bp.functions[function];
    auto first = std::lower_bound(fn.entries.begin(), fn.entries.end(), std::make_pair(static_cast<u32>(pc), NodeId{0}));
    auto last = first;
    while (last != fn.entries.end() && last->first == pc) ++last;
    if (first == last) return;
    if (!debug_stack_.empty()) debug_stack_.back().node = first->second;

    if (tracing_) {
        for (auto it = first; it != last; ++it) {
            if (trace_.size() >= trace_capacity_) trace_.erase(trace_.begin());
            trace_.push_back({instance.entity, fn.graph, it->second, frame_});
        }
    }
    if (!debug_handler_ || in_handler_) return;
    if (!debug_filter_.IsNull() && instance.entity != debug_filter_) return;

    const usize depth = debug_stack_.size();
    bool stop = false;
    BpStopReason reason = BpStopReason::Step;
    for (auto it = first; it != last && !stop; ++it) {
        stop = std::find(breakpoints_.begin(), breakpoints_.end(), std::make_tuple(&bp, fn.graph, it->second)) !=
               breakpoints_.end();
        if (stop) {
            reason = BpStopReason::Breakpoint;
            debug_stack_.back().node = it->second;
        }
    }
    if (!stop) {
        switch (step_) {
        case StepMode::None: break;
        case StepMode::Pause: stop = true; reason = BpStopReason::Pause; break;
        case StepMode::Into: stop = true; break;
        case StepMode::Over: stop = depth <= step_depth_; break;
        case StepMode::Out: stop = depth < step_depth_; break;
        }
    }
    if (!stop) return;

    ++debug_stops_;
    const BpStop info = DescribeStop(reason, instance);
    in_handler_ = true;
    const BpAction action = debug_handler_(info);
    in_handler_ = false;
    step_depth_ = depth;
    switch (action) {
    case BpAction::Continue: step_ = StepMode::None; break;
    case BpAction::StepInto: step_ = StepMode::Into; break;
    case BpAction::StepOver: step_ = StepMode::Over; break;
    case BpAction::StepOut: step_ = StepMode::Out; break;
    }
}

BpStop BlueprintVM::DescribeStop(BpStopReason reason, const Instance& instance) const {
    BpStop stop;
    stop.reason = reason;
    stop.entity = instance.entity;
    for (auto it = debug_stack_.rbegin(); it != debug_stack_.rend(); ++it) {
        const CompiledFunction& fn = it->instance->blueprint->functions[it->function];
        BpFrame frame;
        frame.entity = it->instance->entity;
        frame.function = fn.name;
        frame.graph = fn.graph;
        frame.node = it->node;
        for (const auto& [key, value] : fn.pin_values) {
            BpPinValue pin;
            pin.node = key.first;
            pin.pin = key.second;
            pin.type = TypeName(value.type);
            if (value.reg.bank == Bank::String) pin.value = it->frame->s[value.reg.index];
            else if (value.reg.bank == Bank::Array) pin.value = "[" + std::to_string(it->frame->a[value.reg.index].Size()) + " items]";
            else pin.value = Text(it->frame->r[value.reg.index], value.type.type);
            frame.values.push_back(std::move(pin));
        }
        stop.frames.push_back(std::move(frame));
    }
    if (!stop.frames.empty()) {
        stop.graph = stop.frames.front().graph;
        stop.node = stop.frames.front().node;
    }
    return stop;
}

void BlueprintVM::Warn(const char* code, const Instance& instance, const CompiledFunction& fn, NodeId node,
                       std::string message) {
    // Once per (function, node): a bad reference in Tick shouldn't flood the log.
    const u64 key = (static_cast<u64>(std::hash<std::string>{}(fn.name)) << 20) ^ node;
    if (warned_.emplace(key, true).second) {
        errors_.push_back({code, instance.entity, fn.name, node, std::move(message)});
        AETHER_LOG_WARN("Blueprint", "%s: %s", code, errors_.back().message.c_str());
    }
}

void* BlueprintVM::ComponentOf(Entity target, const reflect::TypeInfo* owner) {
    if (owner == nullptr || !world_.IsAlive(target)) return nullptr;
    const ComponentId id = FindComponentIdByName(owner->name);
    return id == kInvalidComponentId ? nullptr : world_.GetComponentRaw(target, id);
}

void BlueprintVM::NativeCallOp(Instance& instance, const CompiledFunction& fn, const NativeCall& call, Frame& frame,
                               NodeId node) {
    void* self = nullptr;
    if (call.has_target) {
        const Entity target = frame.r[call.target.index].AsEntity();
        self = ComponentOf(target, call.owner);
        if (self == nullptr) {
            Warn("BP201", instance, fn, node,
                 std::string("Called '") + call.function->name + "' on an entity without a " + call.owner->name +
                     " in '" + fn.name + "'. Use Is Valid first.");
            if (call.has_result) {
                Reg& r = frame.r[call.result.reg.index];
                if (call.result.reg.bank == Bank::String) frame.s[call.result.reg.index].clear();
                else r = Reg{};
            }
            return;
        }
    }
    std::vector<reflect::Any> args;
    args.reserve(call.args.size());
    for (const TypedReg& arg : call.args) {
        args.push_back(AnyOf(frame.r[arg.reg.bank == Bank::Value ? arg.reg.index : 0],
                             arg.reg.bank == Bank::String ? frame.s[arg.reg.index] : std::string(), arg.type));
    }
    reflect::Any result;
    if (!call.function->Invoke(self, args, call.has_result ? &result : nullptr)) {
        Warn("BP204", instance, fn, node, std::string("The call to '") + call.function->name + "' was refused.");
        return;
    }
    if (call.has_result) {
        const RegRef r = call.result.reg;
        Reg scratch;
        std::string text;
        StoreAny(result, call.result.type, r.bank == Bank::Value ? frame.r[r.index] : scratch,
                 r.bank == Bank::String ? frame.s[r.index] : text);
    }
}

void BlueprintVM::FieldOp(Instance& instance, const CompiledFunction& fn, const FieldAccess& access, Frame& frame,
                          NodeId node, bool write) {
    const Entity target = frame.r[access.target.index].AsEntity();
    void* component = ComponentOf(target, access.owner);
    const RegRef v = access.value.reg;
    if (component == nullptr) {
        Warn("BP201", instance, fn, node,
             std::string("Accessed '") + access.field->name + "' on an entity without a " + access.owner->name +
                 " in '" + fn.name + "'. Use Is Valid first.");
        if (!write) {
            if (v.bank == Bank::String) frame.s[v.index].clear();
            else frame.r[v.index] = Reg{};
        }
        return;
    }
    void* ptr = access.field->Ptr(component);
    Reg& r = frame.r[v.bank == Bank::Value ? v.index : 0];
    switch (access.value.type.type) {
    case ValueType::Bool: write ? void(*static_cast<bool*>(ptr) = r.b) : void(r = Reg::Bool(*static_cast<bool*>(ptr))); break;
    case ValueType::Int: write ? void(*static_cast<i32*>(ptr) = r.i) : void(r = Reg::Int(*static_cast<i32*>(ptr))); break;
    case ValueType::Float: write ? void(*static_cast<f32*>(ptr) = r.f) : void(r = Reg::Float(*static_cast<f32*>(ptr))); break;
    case ValueType::Vec3:
        write ? void(*static_cast<Vec3*>(ptr) = r.AsVec3()) : void(r = Reg::Vector(*static_cast<Vec3*>(ptr)));
        break;
    case ValueType::Quat:
        write ? void(*static_cast<Quaternion*>(ptr) = r.AsQuat()) : void(r = Reg::Quat(*static_cast<Quaternion*>(ptr)));
        break;
    case ValueType::String:
        if (write) *static_cast<std::string*>(ptr) = frame.s[v.index];
        else frame.s[v.index] = *static_cast<std::string*>(ptr);
        break;
    default: break;
    }
}

bool BlueprintVM::Call(Instance& instance, const FunctionCall& call, Frame& caller, u32 depth) {
    const CompiledBlueprint& bp = *instance.blueprint;
    const CompiledFunction& callee = bp.functions[call.function];
    if (depth + 1 >= frames_.size()) {
        errors_.push_back({"BP203", instance.entity, callee.name, 0,
                           "'" + callee.name + "' was called more than " + std::to_string(options_.max_call_depth) +
                               " levels deep. Check for endless recursion."});
        return false;
    }
    Frame& frame = AcquireFrame(callee, depth + 1);
    for (usize i = 0; i < call.args.size() && i < callee.params.size(); ++i) {
        const RegRef from = call.args[i], to = callee.params[i];
        if (to.bank == Bank::Array) frame.a[to.index] = caller.a[from.index];
        else if (to.bank == Bank::String) frame.s[to.index] = caller.s[from.index];
        else frame.r[to.index] = caller.r[from.index];
    }
    if (!Run(instance, call.function, frame, depth + 1)) return false;
    for (usize i = 0; i < call.results.size() && i < callee.results.size(); ++i) {
        const RegRef from = callee.results[i], to = call.results[i];
        if (to.bank == Bank::Array) caller.a[to.index] = frame.a[from.index];
        else if (to.bank == Bank::String) caller.s[to.index] = frame.s[from.index];
        else caller.r[to.index] = frame.r[from.index];
    }
    return true;
}

bool BlueprintVM::Run(Instance& instance, u32 function, Frame& frame, u32 depth, usize start_pc) {
    const CompiledBlueprint& bp = *instance.blueprint;
    const CompiledFunction& fn = bp.functions[function];
    const Instr* code = fn.code.data();
    const u32 saved_depth = depth_;
    depth_ = depth + 1;
    struct Restore {
        u32& depth;
        u32 value;
        ~Restore() { depth = value; }
    } restore{depth_, saved_depth};

    Reg* r = frame.r.data();
    std::string* s = frame.s.data();
    ArrayValue* arrays = frame.a.data();
    usize pc = start_pc;
    const bool debugging = debugging_;
    if (debugging) debug_stack_.push_back({&instance, function, &frame, 0});
    struct PopDebug {
        std::vector<DebugFrame>& stack;
        bool active;
        ~PopDebug() {
            if (active && !stack.empty()) stack.pop_back();
        }
    } pop_debug{debug_stack_, debugging};
    for (;;) {
        if (options_.instruction_budget != 0) {
            if (budget_left_ == 0) {
                const NodeId node = pc < fn.node_of.size() ? fn.node_of[pc] : 0;
                errors_.push_back({"BP202", instance.entity, fn.name, node,
                                   "'" + fn.name + "' ran more than " + std::to_string(options_.instruction_budget) +
                                       " instructions and was stopped. Check for an infinite loop near node " +
                                       std::to_string(node) + "."});
                AETHER_LOG_ERROR("Blueprint", "BP202: %s", errors_.back().message.c_str());
                return false;
            }
            --budget_left_;
        }
        ++instructions_;
        if (debugging && fn.is_entry[pc]) DebugHook(instance, function, pc);
        const Instr& in = code[pc++];
        switch (in.op) {
        case Op::Nop: break;
        case Op::LoadK: r[in.a] = bp.constants[static_cast<usize>(in.d)]; break;
        case Op::LoadKS: s[in.a] = bp.string_constants[static_cast<usize>(in.d)]; break;
        case Op::Move: r[in.a] = r[in.b]; break;
        case Op::MoveS: s[in.a] = s[in.b]; break;
        case Op::GetVar: r[in.a] = instance.vars[static_cast<usize>(in.d)]; break;
        case Op::SetVar: instance.vars[static_cast<usize>(in.d)] = r[in.b]; break;
        case Op::GetVarS: s[in.a] = instance.svars[static_cast<usize>(in.d)]; break;
        case Op::SetVarS: instance.svars[static_cast<usize>(in.d)] = s[in.b]; break;

        case Op::AddI: r[in.a] = Reg::Int(static_cast<i32>(static_cast<u32>(r[in.b].i) + static_cast<u32>(r[in.c].i))); break;
        case Op::SubI: r[in.a] = Reg::Int(static_cast<i32>(static_cast<u32>(r[in.b].i) - static_cast<u32>(r[in.c].i))); break;
        case Op::MulI: r[in.a] = Reg::Int(static_cast<i32>(static_cast<u32>(r[in.b].i) * static_cast<u32>(r[in.c].i))); break;
        case Op::DivI: // divide by zero gives 0 (BLUEPRINT_NODES.md §5)
            r[in.a] = Reg::Int(r[in.c].i == 0 || (r[in.b].i == INT32_MIN && r[in.c].i == -1) ? 0 : r[in.b].i / r[in.c].i);
            break;
        case Op::ModI:
            r[in.a] = Reg::Int(r[in.c].i == 0 || (r[in.b].i == INT32_MIN && r[in.c].i == -1) ? 0 : r[in.b].i % r[in.c].i);
            break;
        case Op::MinI: r[in.a] = Reg::Int(std::min(r[in.b].i, r[in.c].i)); break;
        case Op::MaxI: r[in.a] = Reg::Int(std::max(r[in.b].i, r[in.c].i)); break;
        case Op::AddF: r[in.a] = Reg::Float(r[in.b].f + r[in.c].f); break;
        case Op::SubF: r[in.a] = Reg::Float(r[in.b].f - r[in.c].f); break;
        case Op::MulF: r[in.a] = Reg::Float(r[in.b].f * r[in.c].f); break;
        case Op::DivF: r[in.a] = Reg::Float(r[in.c].f == 0.0f ? 0.0f : r[in.b].f / r[in.c].f); break;
        case Op::MinF: r[in.a] = Reg::Float(std::min(r[in.b].f, r[in.c].f)); break;
        case Op::MaxF: r[in.a] = Reg::Float(std::max(r[in.b].f, r[in.c].f)); break;
        case Op::AddV: r[in.a] = Reg::Vector(r[in.b].AsVec3() + r[in.c].AsVec3()); break;
        case Op::SubV: r[in.a] = Reg::Vector(r[in.b].AsVec3() - r[in.c].AsVec3()); break;
        case Op::MulV: {
            const Vec3 x = r[in.b].AsVec3(), y = r[in.c].AsVec3();
            r[in.a] = Reg::Vector({x.x * y.x, x.y * y.y, x.z * y.z});
            break;
        }
        case Op::ScaleV: r[in.a] = Reg::Vector(r[in.b].AsVec3() * r[in.c].f); break;

        case Op::LtI: r[in.a] = Reg::Bool(r[in.b].i < r[in.c].i); break;
        case Op::LeI: r[in.a] = Reg::Bool(r[in.b].i <= r[in.c].i); break;
        case Op::GtI: r[in.a] = Reg::Bool(r[in.b].i > r[in.c].i); break;
        case Op::GeI: r[in.a] = Reg::Bool(r[in.b].i >= r[in.c].i); break;
        case Op::EqI: r[in.a] = Reg::Bool(r[in.b].i == r[in.c].i); break;
        case Op::NeI: r[in.a] = Reg::Bool(r[in.b].i != r[in.c].i); break;
        case Op::LtF: r[in.a] = Reg::Bool(r[in.b].f < r[in.c].f); break;
        case Op::LeF: r[in.a] = Reg::Bool(r[in.b].f <= r[in.c].f); break;
        case Op::GtF: r[in.a] = Reg::Bool(r[in.b].f > r[in.c].f); break;
        case Op::GeF: r[in.a] = Reg::Bool(r[in.b].f >= r[in.c].f); break;
        case Op::EqF: r[in.a] = Reg::Bool(r[in.b].f == r[in.c].f); break;
        case Op::NeF: r[in.a] = Reg::Bool(r[in.b].f != r[in.c].f); break;
        case Op::EqB: r[in.a] = Reg::Bool(r[in.b].b == r[in.c].b); break;
        case Op::NeB: r[in.a] = Reg::Bool(r[in.b].b != r[in.c].b); break;
        case Op::EqE: r[in.a] = Reg::Bool(r[in.b].AsEntity() == r[in.c].AsEntity()); break;
        case Op::NeE: r[in.a] = Reg::Bool(r[in.b].AsEntity() != r[in.c].AsEntity()); break;
        case Op::EqS: r[in.a] = Reg::Bool(s[in.b] == s[in.c]); break;
        case Op::NeS: r[in.a] = Reg::Bool(s[in.b] != s[in.c]); break;
        case Op::And: r[in.a] = Reg::Bool(r[in.b].b && r[in.c].b); break;
        case Op::Or: r[in.a] = Reg::Bool(r[in.b].b || r[in.c].b); break;
        case Op::Xor: r[in.a] = Reg::Bool(r[in.b].b != r[in.c].b); break;
        case Op::Not: r[in.a] = Reg::Bool(!r[in.b].b); break;
        case Op::NegF: r[in.a] = Reg::Float(-r[in.b].f); break;
        case Op::AbsF: r[in.a] = Reg::Float(std::fabs(r[in.b].f)); break;
        case Op::ClampF: {
            const f32 lo = r[in.c].f, hi = r[static_cast<usize>(in.d)].f;
            r[in.a] = Reg::Float(std::max(lo, std::min(hi, r[in.b].f)));
            break;
        }
        case Op::LerpF: {
            const f32 t = r[static_cast<usize>(in.d)].f;
            r[in.a] = Reg::Float(r[in.b].f + (r[in.c].f - r[in.b].f) * t);
            break;
        }
        case Op::MakeV: r[in.a] = Reg::Vector({r[in.b].f, r[in.c].f, r[static_cast<usize>(in.d)].f}); break;
        case Op::BreakV: r[in.a] = Reg::Float(r[in.b].v[std::min<u16>(in.c, 2)]); break;
        case Op::LenV: r[in.a] = Reg::Float(r[in.b].AsVec3().Length()); break;
        case Op::DotV: r[in.a] = Reg::Float(r[in.b].AsVec3().Dot(r[in.c].AsVec3())); break;
        case Op::CrossV: r[in.a] = Reg::Vector(r[in.b].AsVec3().Cross(r[in.c].AsVec3())); break;
        case Op::NormV: {
            const Vec3 v = r[in.b].AsVec3();
            const f32 length = v.Length();
            r[in.a] = Reg::Vector(length > 1e-8f ? v * (1.0f / length) : Vec3{});
            break;
        }
        case Op::IntToFloat: r[in.a] = Reg::Float(static_cast<f32>(r[in.b].i)); break;
        case Op::FloatToInt: {
            const f32 f = r[in.b].f;
            r[in.a] = Reg::Int(std::isfinite(f) && std::fabs(f) < 2147483520.0f ? static_cast<i32>(f) : 0);
            break;
        }
        case Op::ToString: s[in.a] = Text(r[in.b], static_cast<ValueType>(in.c)); break;
        case Op::Self: r[in.a] = Reg::EntityOf(instance.entity); break;
        case Op::IsValid: r[in.a] = Reg::Bool(world_.IsAlive(r[in.b].AsEntity())); break;
        case Op::Jmp: pc = static_cast<usize>(in.d); break;
        case Op::JmpF:
            if (!r[in.b].b) pc = static_cast<usize>(in.d);
            break;
        case Op::Print:
            if (print_) print_(instance.entity, s[in.b]);
            break;
        case Op::Draw: {
            const DrawInfo& d = bp.debug_draws[static_cast<usize>(in.d)];
            if (debug_draw_) {
                DebugDrawCall call;
                call.kind = static_cast<DebugDrawCall::Kind>(d.kind);
                call.from = r[d.a.index].AsVec3();
                switch (d.kind) {
                    case DrawInfo::Kind::Line: call.to = r[d.b.index].AsVec3(); break;
                    case DrawInfo::Kind::Box: call.size = r[d.b.index].AsVec3(); break;
                    case DrawInfo::Kind::Sphere:
                    case DrawInfo::Kind::Point: call.to.x = r[d.b.index].f; break; // radius / cross half-size
                    case DrawInfo::Kind::Text: call.text = s[d.text.index]; break;
                }
                call.color = static_cast<u32>(r[d.color.index].i);
                call.duration = r[d.duration.index].f;
                debug_draw_(instance.entity, call);
            }
            break;
        }
        case Op::CallNative:
            NativeCallOp(instance, fn, bp.native_calls[static_cast<usize>(in.d)], frame, fn.node_of[pc - 1]);
            break;
        case Op::GetField:
        case Op::SetField:
            FieldOp(instance, fn, bp.field_accesses[static_cast<usize>(in.d)], frame, fn.node_of[pc - 1],
                    in.op == Op::SetField);
            break;
        case Op::CallFunction:
        case Op::CallEvent:
            if (!Call(instance, bp.calls[static_cast<usize>(in.d)], frame, depth)) return false;
            break;
        case Op::StateInit: {
            NodeState& st = instance.states[in.a];
            if (!st.init) {
                st.init = true;
                st.flag = r[in.b].b;
            }
            break;
        }
        case Op::StateJmpIf:
            if (instance.states[in.a].flag == (in.c != 0)) pc = static_cast<usize>(in.d);
            break;
        case Op::StateSet:
            instance.states[in.a].init = true;
            instance.states[in.a].flag = in.c != 0;
            break;
        case Op::StateToggle:
            instance.states[in.a].init = true;
            instance.states[in.a].flag = !instance.states[in.a].flag;
            break;
        case Op::StateGet: r[in.a] = Reg::Bool(instance.states[in.b].flag); break;
        case Op::CountLess: {
            NodeState& st = instance.states[in.a];
            if (st.counter >= r[in.b].i) pc = static_cast<usize>(in.d);
            else ++st.counter;
            break;
        }
        case Op::CountGet: r[in.a] = Reg::Int(instance.states[in.b].counter); break;
        case Op::CountReset: instance.states[in.a].counter = 0; break;
        case Op::Latent: StartLatent(instance, function, in, frame); break;
        case Op::SortVar: {
            const SortInfo& info = bp.sorts[static_cast<usize>(in.d)];
            const ArrayValue items = instance.avars[static_cast<usize>(info.slot)]; // a copy: the comparator may read it
            std::vector<usize> order(items.Size());
            for (usize i = 0; i < order.size(); ++i) order[i] = i;
            bool ok = true;
            // Merge sort by hand: a Blueprint comparator may be inconsistent,
            // which std::sort can't tolerate. Ties keep their order.
            auto less = [&](usize x, usize y) {
                if (!ok) return false;
                if (info.function < 0) {
                    if (items.strings) return items.texts[x] < items.texts[y];
                    return items.type == ValueType::Float ? items.values[x].f < items.values[y].f
                                                          : items.values[x].i < items.values[y].i;
                }
                bool before = false;
                ok = CallPredicate(instance, static_cast<u32>(info.function), items, x, &y, depth, before);
                return before;
            };
            std::vector<usize> scratch(order.size());
            for (usize width = 1; width < order.size() && ok; width *= 2) {
                for (usize lo = 0; lo < order.size(); lo += 2 * width) {
                    const usize mid = std::min(lo + width, order.size()), hi = std::min(lo + 2 * width, order.size());
                    usize i = lo, j = mid, k = lo;
                    while (i < mid && j < hi) scratch[k++] = less(order[j], order[i]) ? order[j++] : order[i++];
                    while (i < mid) scratch[k++] = order[i++];
                    while (j < hi) scratch[k++] = order[j++];
                }
                order.swap(scratch);
            }
            if (!ok) return false; // the comparator failed (its error is recorded)
            ArrayValue& target = instance.avars[static_cast<usize>(info.slot)];
            target.Clear();
            for (usize i : order) {
                if (items.strings) target.texts.push_back(items.texts[i]);
                else target.values.push_back(items.values[i]);
            }
            break;
        }
        case Op::FilterA: {
            const SortInfo& info = bp.sorts[static_cast<usize>(in.d)];
            const ArrayValue items = arrays[in.b];
            ArrayValue kept;
            kept.type = items.type;
            kept.strings = items.strings;
            for (usize i = 0; i < items.Size(); ++i) {
                bool keep = false;
                if (!CallPredicate(instance, static_cast<u32>(info.function), items, i, nullptr, depth, keep)) return false;
                if (!keep) continue;
                if (items.strings) kept.texts.push_back(items.texts[i]);
                else kept.values.push_back(items.values[i]);
            }
            arrays[in.a] = std::move(kept); // callees run in the next frame: this one's banks stay put
            break;
        }
        case Op::CallDispatcher: {
            const DispatcherCall& call = bp.dispatcher_calls[static_cast<usize>(in.d)];
            const Entity target = r[call.target.index].AsEntity();
            auto it = bindings_.find({Key(target), call.dispatcher});
            if (it == bindings_.end()) break;
            const std::vector<VmValue> args = ArgsOf(call.args, frame);
            const auto listeners = it->second; // a handler may bind or unbind
            for (const auto& [listener, event] : listeners) {
                auto l = instances_.find(listener);
                if (l != instances_.end() && !l->second->detached) Dispatch(l->second->entity, "Event.Custom:" + event, args);
            }
            break;
        }
        case Op::BindDispatcher: {
            const DispatcherBind& bind = bp.dispatcher_binds[static_cast<usize>(in.d)];
            const auto key = std::make_pair(Key(r[bind.target.index].AsEntity()), bind.dispatcher);
            auto& list = bindings_[key];
            const std::pair<u64, std::string> me{Key(instance.entity), bind.event};
            if (bind.mode == DispatcherBind::Mode::Bind) {
                if (std::find(list.begin(), list.end(), me) == list.end()) list.push_back(me);
            } else if (bind.mode == DispatcherBind::Mode::Unbind) {
                list.erase(std::remove(list.begin(), list.end(), me), list.end());
            } else {
                list.clear();
            }
            break;
        }
        case Op::InterfaceCall: {
            const InterfaceCallInfo& call = bp.interface_calls[static_cast<usize>(in.d)];
            const Entity target = r[call.target.index].AsEntity();
            const Instance* other = Find(target);
            const std::string key = "Event.Interface:" + call.key;
            if (other != nullptr && other->blueprint->events.count(key) != 0) {
                Dispatch(target, key, ArgsOf(call.args, frame)); // not implemented: nothing happens
            }
            break;
        }
        case Op::ImplementsOp: {
            const Instance* other = Find(r[in.b].AsEntity());
            const std::string& name = bp.names[static_cast<usize>(in.d)];
            r[in.a] = Reg::Bool(other != nullptr && std::find(other->blueprint->interfaces.begin(),
                                                              other->blueprint->interfaces.end(),
                                                              name) != other->blueprint->interfaces.end());
            break;
        }
        case Op::GetLoc:
        case Op::GetRot: {
            const Transform* t = TransformOf(instance, fn, fn.node_of[pc - 1], r[in.b].AsEntity());
            if (in.op == Op::GetLoc) r[in.a] = Reg::Vector(t != nullptr ? t->position : Vec3{});
            else r[in.a] = Reg::Quat(t != nullptr ? t->rotation : Quaternion{});
            break;
        }
        case Op::SetLoc:
        case Op::AddOffset:
        case Op::SetRot: {
            Transform* t = TransformOf(instance, fn, fn.node_of[pc - 1], r[in.b].AsEntity());
            if (t == nullptr) break;
            if (in.op == Op::SetLoc) t->position = r[in.c].AsVec3();
            else if (in.op == Op::AddOffset) t->position = t->position + r[in.c].AsVec3();
            else t->rotation = r[in.c].AsQuat();
            break;
        }
        case Op::WorldLoc: {
            const Entity target = r[in.b].AsEntity();
            if (guids_ != nullptr && world_.IsAlive(target)) {
                r[in.a] = Reg::Vector(WorldPosition(world_, *guids_, target));
            } else {
                const Transform* t = TransformOf(instance, fn, fn.node_of[pc - 1], target);
                r[in.a] = Reg::Vector(t != nullptr ? t->position : Vec3{});
            }
            break;
        }
        case Op::QuatRotate: {
            // v' = v + 2w(q x v) + 2 q x (q x v)
            const Quaternion q = r[in.b].AsQuat();
            const Vec3 v = r[in.c].AsVec3(), u{q.x, q.y, q.z};
            const Vec3 t = u.Cross(v) * 2.0f;
            r[in.a] = Reg::Vector(v + t * q.w + u.Cross(t));
            break;
        }
        case Op::QuatAxisAngle: {
            const Vec3 axis = r[in.b].AsVec3();
            r[in.a] = Reg::Quat(axis.LengthSq() > 1e-12f
                                    ? Quaternion::FromAxisAngle(axis, r[in.c].f * 0.017453292519943295f)
                                    : Quaternion{});
            break;
        }
        case Op::QuatMul: r[in.a] = Reg::Quat(r[in.b].AsQuat() * r[in.c].AsQuat()); break;
        case Op::HasTagOp: {
            const Entity target = r[in.b].AsEntity();
            r[in.a] = Reg::Bool(world_.IsAlive(target) && HasTag(world_, target, s[in.c]));
            break;
        }
        case Op::AddTagOp:
        case Op::RemoveTagOp: {
            const Entity target = r[in.b].AsEntity();
            if (!world_.IsAlive(target)) {
                Warn("BP201", instance, fn, fn.node_of[pc - 1],
                     "Changed the tags of an entity that is gone in '" + fn.name + "'. Use Is Valid first.");
            } else if (in.op == Op::AddTagOp) {
                AddTag(world_, target, s[in.c]);
            } else {
                RemoveTag(world_, target, s[in.c]);
            }
            break;
        }
        case Op::FindTagOp: {
            ArrayValue& array = arrays[in.a];
            array = ArrayValue{};
            array.type = ValueType::Entity;
            for (Entity e : FindEntitiesWithTag(world_, s[in.b])) array.values.push_back(Reg::EntityOf(e));
            break;
        }
        case Op::GetParentOp: {
            const Entity target = r[in.b].AsEntity();
            r[in.a] = Reg::EntityOf(guids_ != nullptr && world_.IsAlive(target) ? GetParent(world_, *guids_, target)
                                                                               : kNullEntity);
            break;
        }
        case Op::AttachOp: {
            const Entity child = r[in.b].AsEntity(), parent = r[in.c].AsEntity();
            const IdComponent* id = world_.IsAlive(parent) ? world_.GetComponent<IdComponent>(parent) : nullptr;
            if (guids_ == nullptr || !world_.IsAlive(child) || id == nullptr ||
                WouldCreateCycle(world_, *guids_, child, parent)) {
                Warn("BP201", instance, fn, fn.node_of[pc - 1],
                     "Attach To in '" + fn.name + "' needs two live entities (the parent with an ID), and can't "
                     "make an entity its own ancestor.");
                break;
            }
            const EntityGuid guid = id->guid;
            if (Parent* p = world_.GetComponent<Parent>(child)) p->parent = guid;
            else world_.AddComponent(child, Parent{guid});
            break;
        }
        case Op::DetachOp: {
            const Entity target = r[in.b].AsEntity();
            if (world_.IsAlive(target) && world_.GetComponent<Parent>(target) != nullptr) {
                world_.RemoveComponent<Parent>(target);
            }
            break;
        }
        case Op::DestroyOp: {
            const Entity target = r[in.b].AsEntity();
            if (world_.IsAlive(target) &&
                std::find(pending_destroy_.begin(), pending_destroy_.end(), target) == pending_destroy_.end()) {
                pending_destroy_.push_back(target);
            }
            break;
        }
        case Op::TraceOp: {
            const TraceInfo& t = bp.traces[static_cast<usize>(in.d)];
            const bool overlap = t.kind == TraceInfo::Kind::OverlapSphere;
            const u32 layers = static_cast<u32>(r[t.layers.index].i);
            const Entity ignore = r[t.ignore_self.index].b ? instance.entity : kNullEntity;
            const f32 radius = t.kind == TraceInfo::Kind::Line ? 0.0f : std::max(0.0f, r[t.radius.index].f);
            const bool available = overlap ? static_cast<bool>(queries_.overlap_sphere) : static_cast<bool>(queries_.trace);
            if (!available) {
                Warn("BP207", instance, fn, fn.node_of[pc - 1],
                     "A physics query in '" + fn.name + "' has no physics to ask (no physics scene is connected).");
            }
            if (overlap) {
                ArrayValue& array = arrays[t.entities.index];
                array = ArrayValue{};
                array.type = ValueType::Entity;
                if (available) {
                    for (Entity e : queries_.overlap_sphere(r[t.start.index].AsVec3(), radius, layers, ignore)) {
                        array.values.push_back(Reg::EntityOf(e));
                    }
                }
            } else {
                PhysicsHit hit;
                if (available) hit = queries_.trace(r[t.start.index].AsVec3(), r[t.end.index].AsVec3(), radius, layers, ignore);
                r[t.hit.index] = Reg::Bool(hit.hit);
                r[t.entity.index] = Reg::EntityOf(hit.entity);
                r[t.location.index] = Reg::Vector(hit.location);
                r[t.normal.index] = Reg::Vector(hit.normal);
                r[t.distance.index] = Reg::Float(hit.distance);
            }
            break;
        }
        case Op::SpawnOp: {
            assets::AssetGuid guid;
            Entity spawned = kNullEntity;
            if (!spawn_) {
                Warn("BP206", instance, fn, fn.node_of[pc - 1],
                     "Spawn Blueprint in '" + fn.name + "' has nothing to spawn with (no spawner is set up).");
            } else if (const SpawnInfo& info = bp.spawns[static_cast<usize>(in.d)]; assets::ParseAssetGuid(info.asset, guid)) {
                Transform transform;
                transform.position = r[in.b].AsVec3();
                transform.rotation = r[in.c].AsQuat();
                nlohmann::json exposed = nlohmann::json::object();
                for (const auto& [name, value] : info.exposed) {
                    if (value.reg.bank == Bank::String) {
                        exposed[name] = s[value.reg.index];
                    } else {
                        const Reg& v = r[value.reg.index];
                        switch (value.type.type) {
                        case ValueType::Bool: exposed[name] = v.b; break;
                        case ValueType::Int: exposed[name] = v.i; break;
                        case ValueType::Float: exposed[name] = v.f; break;
                        case ValueType::Vec3: exposed[name] = {v.v[0], v.v[1], v.v[2]}; break;
                        case ValueType::Quat: exposed[name] = {v.v[0], v.v[1], v.v[2], v.v[3]}; break;
                        default: break;
                        }
                    }
                }
                spawned = spawn_(guid, transform, exposed);
            }
            r[in.a] = Reg::EntityOf(spawned);
            break;
        }
        case Op::GameTime: r[in.a] = Reg::Float(static_cast<f32>(time_)); break;
        case Op::DeltaTime: r[in.a] = Reg::Float(last_delta_); break;
        case Op::NewA:
            arrays[in.a] = ArrayValue{};
            arrays[in.a].type = static_cast<ValueType>(in.c);
            arrays[in.a].strings = arrays[in.a].type == ValueType::String;
            break;
        case Op::PushA:
            if (arrays[in.a].strings) arrays[in.a].texts.push_back(s[in.b]);
            else arrays[in.a].values.push_back(r[in.b]);
            break;
        case Op::MoveA: arrays[in.a] = arrays[in.b]; break;
        case Op::GetVarA: arrays[in.a] = instance.avars[static_cast<usize>(in.d)]; break;
        case Op::SetVarA: {
            ArrayValue& var = instance.avars[static_cast<usize>(in.d)];
            var.values = arrays[in.b].values;
            var.texts = arrays[in.b].texts;
            break;
        }
        case Op::LenA: r[in.a] = Reg::Int(static_cast<i32>(arrays[in.b].Size())); break;
        case Op::GetA: {
            const ArrayValue& array = arrays[in.b];
            const i32 index = r[in.c].i;
            if (index >= 0 && static_cast<usize>(index) < array.Size()) {
                if (array.strings) s[in.a] = array.texts[static_cast<usize>(index)];
                else r[in.a] = array.values[static_cast<usize>(index)];
            } else {
                if (array.strings) s[in.a].clear();
                else r[in.a] = array.type == ValueType::Entity ? Reg::EntityOf(kNullEntity) : Reg{};
                Warn("BP205", instance, fn, fn.node_of[pc - 1],
                     "Index " + std::to_string(index) + " is out of range (the array has " +
                         std::to_string(array.Size()) + " items) in '" + fn.name + "'. Check Is Valid Index first.");
            }
            break;
        }
        case Op::ValidIdxA: {
            const i32 index = r[in.c].i;
            r[in.a] = Reg::Bool(index >= 0 && static_cast<usize>(index) < arrays[in.b].Size());
            break;
        }
        case Op::FindA: {
            const ArrayValue& array = arrays[in.b];
            r[in.a] = Reg::Int(array.strings ? FindText(array, s[in.c]) : FindValue(array, r[in.c]));
            break;
        }
        case Op::VarAdd:
        case Op::VarAddUnique: {
            ArrayValue& array = instance.avars[static_cast<usize>(in.d)];
            if (in.op == Op::VarAddUnique &&
                (array.strings ? FindText(array, s[in.b]) : FindValue(array, r[in.b])) >= 0) {
                r[in.a] = Reg::Int(-1);
                break;
            }
            r[in.a] = Reg::Int(static_cast<i32>(array.Size()));
            if (array.strings) array.texts.push_back(s[in.b]);
            else array.values.push_back(r[in.b]);
            break;
        }
        case Op::VarInsert: {
            ArrayValue& array = instance.avars[static_cast<usize>(in.d)];
            const i32 index = std::clamp(r[in.c].i, 0, static_cast<i32>(array.Size()));
            if (array.strings) array.texts.insert(array.texts.begin() + index, s[in.b]);
            else array.values.insert(array.values.begin() + index, r[in.b]);
            break;
        }
        case Op::VarRemoveAt: {
            ArrayValue& array = instance.avars[static_cast<usize>(in.d)];
            const i32 index = r[in.c].i;
            if (index < 0 || static_cast<usize>(index) >= array.Size()) break; // nothing to remove
            if (array.strings) array.texts.erase(array.texts.begin() + index);
            else array.values.erase(array.values.begin() + index);
            break;
        }
        case Op::VarRemoveItem: {
            ArrayValue& array = instance.avars[static_cast<usize>(in.d)];
            const i32 index = array.strings ? FindText(array, s[in.b]) : FindValue(array, r[in.b]);
            if (index >= 0) {
                if (array.strings) array.texts.erase(array.texts.begin() + index);
                else array.values.erase(array.values.begin() + index);
            }
            r[in.a] = Reg::Bool(index >= 0);
            break;
        }
        case Op::VarClear: instance.avars[static_cast<usize>(in.d)].Clear(); break;
        case Op::VarSetAt: {
            ArrayValue& array = instance.avars[static_cast<usize>(in.d)];
            const i32 index = r[in.c].i;
            if (index < 0 || static_cast<usize>(index) >= array.Size()) {
                Warn("BP205", instance, fn, fn.node_of[pc - 1],
                     "Index " + std::to_string(index) + " is out of range (the array has " +
                         std::to_string(array.Size()) + " items) in '" + fn.name + "'. Check Is Valid Index first.");
                break;
            }
            if (array.strings) array.texts[static_cast<usize>(index)] = s[in.b];
            else array.values[static_cast<usize>(index)] = r[in.b];
            break;
        }
        case Op::VarReverse: {
            ArrayValue& array = instance.avars[static_cast<usize>(in.d)];
            std::reverse(array.values.begin(), array.values.end());
            std::reverse(array.texts.begin(), array.texts.end());
            break;
        }
        case Op::MathF: {
            const f32 x = r[in.b].f;
            f32 y = 0.0f;
            switch (static_cast<MathFn>(in.c)) {
            case MathFn::Sin: y = std::sin(x); break;
            case MathFn::Cos: y = std::cos(x); break;
            case MathFn::Tan: y = std::tan(x); break;
            case MathFn::Asin: y = std::asin(std::clamp(x, -1.0f, 1.0f)); break;
            case MathFn::Acos: y = std::acos(std::clamp(x, -1.0f, 1.0f)); break;
            case MathFn::Atan: y = std::atan(x); break;
            case MathFn::Sqrt: y = x > 0.0f ? std::sqrt(x) : 0.0f; break;
            case MathFn::Exp: y = std::exp(x); break;
            case MathFn::Log: y = x > 0.0f ? std::log(x) : 0.0f; break;
            case MathFn::Frac: y = x - std::floor(x); break;
            case MathFn::DegToRad: y = x * 0.017453292519943295f; break;
            case MathFn::RadToDeg: y = x * 57.29577951308232f; break;
            }
            r[in.a] = Reg::Float(y);
            break;
        }
        case Op::Atan2F: r[in.a] = Reg::Float(std::atan2(r[in.b].f, r[in.c].f)); break;
        case Op::PowF: {
            const f32 y = std::pow(r[in.b].f, r[in.c].f);
            r[in.a] = Reg::Float(std::isfinite(y) ? y : 0.0f);
            break;
        }
        case Op::RoundF: {
            const f32 x = r[in.b].f;
            f32 y = x;
            switch (static_cast<RoundMode>(in.c)) {
            case RoundMode::Floor: y = std::floor(x); break;
            case RoundMode::Ceil: y = std::ceil(x); break;
            case RoundMode::Round: y = std::round(x); break;
            case RoundMode::Truncate: y = std::trunc(x); break;
            }
            r[in.a] = Reg::Int(std::isfinite(y) && std::fabs(y) < 2147483520.0f ? static_cast<i32>(y) : 0);
            break;
        }
        case Op::NegI: r[in.a] = Reg::Int(static_cast<i32>(0u - static_cast<u32>(r[in.b].i))); break;
        case Op::AbsI: r[in.a] = Reg::Int(r[in.b].i == INT32_MIN ? INT32_MAX : std::abs(r[in.b].i)); break;
        case Op::ClampI: {
            const i32 lo = r[in.c].i, hi = r[static_cast<usize>(in.d)].i;
            r[in.a] = Reg::Int(std::max(lo, std::min(hi, r[in.b].i)));
            break;
        }
        case Op::NearEqF:
            r[in.a] = Reg::Bool(std::fabs(r[in.b].f - r[in.c].f) <= std::fabs(r[static_cast<usize>(in.d)].f));
            break;
        case Op::RandF: {
            const f32 lo = r[in.b].f, hi = r[in.c].f;
            r[in.a] = Reg::Float(lo + (hi - lo) * std::uniform_real_distribution<f32>(0.0f, 1.0f)(rng_));
            break;
        }
        case Op::RandI: {
            const i32 lo = std::min(r[in.b].i, r[in.c].i), hi = std::max(r[in.b].i, r[in.c].i);
            r[in.a] = Reg::Int(std::uniform_int_distribution<i32>(lo, hi)(rng_));
            break;
        }
        case Op::RandB: r[in.a] = Reg::Bool((rng_() & 1u) != 0); break;
        case Op::ConcatS: s[in.a] = s[in.b] + s[in.c]; break;
        case Op::LenS: r[in.a] = Reg::Int(static_cast<i32>(s[in.b].size())); break;
        case Op::EmptyS: r[in.a] = Reg::Bool(s[in.b].empty()); break;
        case Op::ContainsS: {
            if (r[static_cast<usize>(in.d)].b) {
                std::string text = s[in.b], sub = s[in.c];
                for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                for (char& ch : sub) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                r[in.a] = Reg::Bool(text.find(sub) != std::string::npos);
            } else {
                r[in.a] = Reg::Bool(s[in.b].find(s[in.c]) != std::string::npos);
            }
            break;
        }
        case Op::StrFn: {
            std::string text = s[in.b];
            switch (static_cast<StrFnKind>(in.c)) {
            case StrFnKind::Upper:
                for (char& ch : text) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                break;
            case StrFnKind::Lower:
                for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                break;
            case StrFnKind::Trim: {
                const usize first = text.find_first_not_of(" \t\r\n");
                const usize last = text.find_last_not_of(" \t\r\n");
                text = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
                break;
            }
            }
            s[in.a] = std::move(text);
            break;
        }
        case Op::ParseI:
        case Op::ParseF: {
            const std::string& text = s[in.b];
            const char* begin = text.c_str();
            char* end = nullptr;
            bool ok = false;
            if (in.op == Op::ParseI) {
                const long long v = std::strtoll(begin, &end, 10);
                ok = end != begin && *end == '\0' && v >= INT32_MIN && v <= INT32_MAX;
                r[in.a] = Reg::Int(ok ? static_cast<i32>(v) : 0);
            } else {
                const f32 v = std::strtof(begin, &end);
                ok = end != begin && *end == '\0' && std::isfinite(v);
                r[in.a] = Reg::Float(ok ? v : 0.0f);
            }
            r[in.c] = Reg::Bool(ok);
            break;
        }
        case Op::Ret: return true;
        }
    }
}

} // namespace aether::bp
