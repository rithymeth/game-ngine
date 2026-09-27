#include "aether/blueprint/vm.h"

#include "aether/core/log.h"
#include "aether/ecs/world.h"
#include "aether/reflection/reflection.h"

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
        if ((var.flags & Var_InstanceEditable) && overrides.is_object() && overrides.contains(var.name)) {
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
    latent_.erase(std::remove_if(latent_.begin(), latent_.end(), [&](const LatentAction& a) { return a.owner == key; }),
                  latent_.end()); // an entity's pending actions go with it
    if (depth_ > 0) {
        it->second->detached = true; // it may be running: remove when the outermost run ends
        return;
    }
    instances_.erase(it);
    order_.erase(std::remove(order_.begin(), order_.end(), Key(entity)), order_.end());
}

void BlueprintVM::RemoveDetached() {
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (it->second->detached) {
            const u64 key = it->first;
            it = instances_.erase(it);
            order_.erase(std::remove(order_.begin(), order_.end(), key), order_.end());
        } else {
            ++it;
        }
    }
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
    auto it = bp.events.find(std::string(event));
    if (it == bp.events.end()) return false;
    const CompiledFunction& fn = bp.functions[it->second];
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
    const bool ok = Run(*instance, it->second, frame, depth_);
    if (outermost) RemoveDetached();
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
        RemoveDetached();
    }
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
    time_ += delta_seconds;
    ++frame_;
    ResumeDue();
    const VmValue dt = delta_seconds;
    const std::vector<u64> order = order_; // stable while events attach or detach
    for (u64 key : order) {
        auto it = instances_.find(key);
        if (it == instances_.end() || it->second->detached || !it->second->enabled) continue;
        if (it->second->blueprint->events.count("Event.Tick") == 0) continue;
        Dispatch(it->second->entity, "Event.Tick", std::span<const VmValue>(&dt, 1));
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
