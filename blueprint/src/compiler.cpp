#include "aether/blueprint/compiler.h"

#include "aether/reflection/reflection.h"

#include <set>

namespace aether::bp {

namespace {

bool Unsupported(const PinType& type) {
    return type.is_array || type.type == ValueType::Struct || type.type == ValueType::Wildcard ||
           type.type == ValueType::None;
}

Bank BankOf(const PinType& type) { return type.type == ValueType::String ? Bank::String : Bank::Value; }

Reg RegFromValue(const Value& value, const PinType& type) {
    if (const bool* b = std::get_if<bool>(&value)) return Reg::Bool(*b);
    if (const i32* i = std::get_if<i32>(&value)) return type.type == ValueType::Float ? Reg::Float(static_cast<f32>(*i)) : Reg::Int(*i);
    if (const f32* f = std::get_if<f32>(&value)) return type.type == ValueType::Int ? Reg::Int(static_cast<i32>(*f)) : Reg::Float(*f);
    if (const Vec3* v = std::get_if<Vec3>(&value)) return Reg::Vector(*v);
    if (const Quaternion* q = std::get_if<Quaternion>(&value)) return Reg::Quat(*q);
    if (type.type == ValueType::Entity) return Reg::EntityOf(kNullEntity);
    return Reg{};
}

struct BinaryOps {
    const char* prefix;
    Op int_op, float_op, vec_op, bool_op, string_op, entity_op;
};

// Math families -> typed opcodes (Nop: that operand type isn't offered).
const BinaryOps kBinary[] = {
    {"Math.Add:", Op::AddI, Op::AddF, Op::AddV, Op::Nop, Op::Nop, Op::Nop},
    {"Math.Subtract:", Op::SubI, Op::SubF, Op::SubV, Op::Nop, Op::Nop, Op::Nop},
    {"Math.Multiply:", Op::MulI, Op::MulF, Op::MulV, Op::Nop, Op::Nop, Op::Nop},
    {"Math.Divide:", Op::DivI, Op::DivF, Op::Nop, Op::Nop, Op::Nop, Op::Nop},
    {"Math.Modulo:", Op::ModI, Op::Nop, Op::Nop, Op::Nop, Op::Nop, Op::Nop},
    {"Math.Min:", Op::MinI, Op::MinF, Op::Nop, Op::Nop, Op::Nop, Op::Nop},
    {"Math.Max:", Op::MaxI, Op::MaxF, Op::Nop, Op::Nop, Op::Nop, Op::Nop},
    {"Math.Less:", Op::LtI, Op::LtF, Op::Nop, Op::Nop, Op::Nop, Op::Nop},
    {"Math.LessEqual:", Op::LeI, Op::LeF, Op::Nop, Op::Nop, Op::Nop, Op::Nop},
    {"Math.Greater:", Op::GtI, Op::GtF, Op::Nop, Op::Nop, Op::Nop, Op::Nop},
    {"Math.GreaterEqual:", Op::GeI, Op::GeF, Op::Nop, Op::Nop, Op::Nop, Op::Nop},
    {"Math.Equal:", Op::EqI, Op::EqF, Op::Nop, Op::EqB, Op::EqS, Op::EqE},
    {"Math.NotEqual:", Op::NeI, Op::NeF, Op::Nop, Op::NeB, Op::NeS, Op::NeE},
};

class FunctionCompiler {
public:
    FunctionCompiler(const Graph& graph, usize graph_index, u32 function_index,
                     const std::map<NodeId, NodeSignature>& sigs, CompiledBlueprint& out, CompiledFunction& fn,
                     ValidationResult& diagnostics, std::map<std::pair<usize, NodeId>, u16>& slots)
        : graph_(graph), graph_index_(graph_index), function_index_(function_index), slots_(slots), sigs_(sigs), out_(out), fn_(fn), diagnostics_(diagnostics) {}

    void CompileEvent(const Node& event) {
        const NodeSignature& sig = sigs_.at(event.id);
        for (const PinDesc& pin : sig.pins) {
            if (pin.dir == PinDir::Out && !pin.type.IsExec()) {
                fn_.params.push_back(OutputReg(event.id, pin));
                fn_.param_types.push_back(pin.type);
            }
        }
        Chain(event.id, "then");
        Emit({Op::Ret}, event.id);
        EmitResumeBlocks();
    }

    void CompileFunction() {
        const Node* entry = nullptr;
        const Node* ret = nullptr;
        for (const Node& n : graph_.nodes) {
            if (n.type == "Function.Entry" && entry == nullptr) entry = &n;
            if (n.type == "Function.Return" && ret == nullptr) ret = &n;
        }
        for (const Variable& v : graph_.outputs) {
            fn_.results.push_back(Alloc(v.type, entry != nullptr ? entry->id : 0));
            fn_.result_types.push_back(v.type);
        }
        if (entry == nullptr) {
            Emit({Op::Ret}, 0);
            return;
        }
        const NodeSignature& sig = sigs_.at(entry->id);
        for (const PinDesc& pin : sig.pins) {
            if (pin.dir == PinDir::Out && !pin.type.IsExec()) {
                fn_.params.push_back(OutputReg(entry->id, pin));
                fn_.param_types.push_back(pin.type);
            }
        }
        if (graph_.pure) {
            if (ret != nullptr) EmitReturn(*ret);
            else Emit({Op::Ret}, entry->id);
            return;
        }
        Chain(entry->id, "then");
        Emit({Op::Ret}, entry->id);
    }

private:
    // --- Emission helpers -------------------------------------------------
    i32 Here() const { return static_cast<i32>(fn_.code.size()); }
    usize Emit(Instr instr, NodeId node) {
        fn_.code.push_back(instr);
        fn_.node_of.push_back(node);
        return fn_.code.size() - 1;
    }

    void Error(const char* code, NodeId node, const std::string& message) {
        for (const Diagnostic& d : diagnostics_.diagnostics) {
            if (d.code == code && d.graph == graph_.name && d.node == node) return; // once per node
        }
        diagnostics_.diagnostics.push_back({code, Severity::Error, graph_.name, node, "", message});
        diagnostics_.errors++;
    }

    RegRef Alloc(const PinType& type, NodeId node) {
        if (Unsupported(type)) {
            const std::string name = TypeName(type);
            Error("BP012", node, "'" + Title(node) + "' uses a " + name + " value; Blueprints can't run " + name +
                                     " values yet.");
        }
        if (BankOf(type) == Bank::String) return {Bank::String, fn_.string_regs++};
        return {Bank::Value, fn_.value_regs++};
    }

    std::string Title(NodeId node) const {
        auto it = sigs_.find(node);
        return it != sigs_.end() ? it->second.title : "node " + std::to_string(node);
    }

    const Node& NodeOf(NodeId id) const { return *graph_.Find(id); }
    const NodeSignature& Sig(NodeId id) const { return sigs_.at(id); }

    const Link* LinkInto(NodeId node, const std::string& pin) const {
        for (const Link& l : graph_.links) {
            if (l.to.node == node && l.to.pin == pin) return &l;
        }
        return nullptr;
    }
    const Link* ExecFrom(NodeId node, const std::string& pin) const {
        for (const Link& l : graph_.links) {
            if (l.from.node == node && l.from.pin == pin) return &l;
        }
        return nullptr;
    }

    RegRef LoadValue(const Value& value, const PinType& type, NodeId node) {
        const RegRef dst = Alloc(type, node);
        if (dst.bank == Bank::String) {
            const std::string* s = std::get_if<std::string>(&value);
            out_.string_constants.push_back(s != nullptr ? *s : std::string());
            Emit({Op::LoadKS, dst.index, 0, 0, static_cast<i32>(out_.string_constants.size() - 1)}, node);
        } else {
            out_.constants.push_back(RegFromValue(value, type));
            Emit({Op::LoadK, dst.index, 0, 0, static_cast<i32>(out_.constants.size() - 1)}, node);
        }
        return dst;
    }

    RegRef Convert(RegRef src, const PinType& from, const PinType& to, NodeId node) {
        switch (CanConnect(from, to)) {
        case Compat::Yes: return src;
        case Compat::ConvertLossy: {
            const RegRef dst = Alloc(to, node);
            Emit({Op::FloatToInt, dst.index, src.index}, node);
            return dst;
        }
        case Compat::Convert: {
            const RegRef dst = Alloc(to, node);
            if (to.type == ValueType::String) {
                Emit({Op::ToString, dst.index, src.index, static_cast<u16>(from.type)}, node);
            } else {
                Emit({Op::IntToFloat, dst.index, src.index}, node);
            }
            return dst;
        }
        case Compat::No: break;
        }
        return src; // validation already reported it
    }

    // The register holding an impure node's output (allocated on first use).
    RegRef OutputReg(NodeId node, const PinDesc& pin) {
        const auto key = std::make_pair(node, pin.name);
        auto it = outputs_.find(key);
        if (it != outputs_.end()) return it->second;
        return outputs_.emplace(key, Alloc(pin.type, node)).first->second;
    }

    RegRef Output(const PinRef& from) {
        const NodeSignature& sig = Sig(from.node);
        if (sig.IsPure()) {
            EvaluatePure(from.node);
            auto it = pure_.find({from.node, from.pin});
            if (it != pure_.end()) return it->second;
            return Alloc(sig.Find(from.pin, PinDir::Out)->type, from.node);
        }
        return OutputReg(from.node, *sig.Find(from.pin, PinDir::Out));
    }

    RegRef Input(NodeId node, const std::string& name) {
        const PinDesc* pin = Sig(node).Find(name, PinDir::In);
        if (const Link* link = LinkInto(node, name)) {
            const PinDesc* from = Sig(link->from.node).Find(link->from.pin, PinDir::Out);
            return Convert(Output(link->from), from->type, pin->type, node);
        }
        if (pin->flags & Pin_Self) {
            const RegRef dst = Alloc(pin->type, node);
            Emit({Op::Self, dst.index}, node);
            return dst;
        }
        Value value = pin->default_value;
        const Node& n = NodeOf(node);
        if (n.defaults.contains(name)) ValueFromJson(n.defaults[name], pin->type, value);
        return LoadValue(value, pin->type, node);
    }

    void SetPure(NodeId node, const std::string& pin, RegRef reg) { pure_[{node, pin}] = reg; }

    // --- Pure nodes -----------------------------------------------------------
    void EvaluatePure(NodeId id) {
        if (!evaluated_.insert(id).second) return;
        const Node& node = NodeOf(id);
        const NodeSignature& sig = Sig(id);
        const std::string& type = node.type;
        auto out_type = [&](const std::string& pin) { return sig.Find(pin, PinDir::Out)->type; };
        auto binary = [&](Op op) {
            const RegRef a = Input(id, "a"), b = Input(id, "b");
            const RegRef dst = Alloc(out_type("result"), id);
            Emit({op, dst.index, a.index, b.index}, id);
            SetPure(id, "result", dst);
        };
        auto unary = [&](Op op, const char* in) {
            const RegRef a = Input(id, in);
            const RegRef dst = Alloc(out_type("result"), id);
            Emit({op, dst.index, a.index}, id);
            SetPure(id, "result", dst);
        };

        for (const BinaryOps& ops : kBinary) {
            if (type.rfind(ops.prefix, 0) != 0) continue;
            const PinType operand = sig.Find("a", PinDir::In)->type;
            Op op = Op::Nop;
            switch (operand.type) {
            case ValueType::Int: op = ops.int_op; break;
            case ValueType::Float: op = ops.float_op; break;
            case ValueType::Vec3: op = ops.vec_op; break;
            case ValueType::Bool: op = ops.bool_op; break;
            case ValueType::String: op = ops.string_op; break;
            case ValueType::Entity: op = ops.entity_op; break;
            default: break;
            }
            if (op == Op::Nop) {
                Error("BP012", id, "'" + sig.title + "' can't run on " + TypeName(operand) + " values.");
                return;
            }
            binary(op);
            return;
        }
        if (type == "Math.And") return binary(Op::And);
        if (type == "Math.Or") return binary(Op::Or);
        if (type == "Math.Xor") return binary(Op::Xor);
        if (type == "Math.Not") return unary(Op::Not, "a");
        if (type == "Math.Negate:float") return unary(Op::NegF, "a");
        if (type == "Math.Abs:float") return unary(Op::AbsF, "a");
        if (type == "Math.Clamp:float" || type == "Math.Lerp:float") {
            const bool clamp = type == "Math.Clamp:float";
            const RegRef x = Input(id, clamp ? "value" : "a"), y = Input(id, clamp ? "min" : "b"),
                         z = Input(id, clamp ? "max" : "alpha");
            const RegRef dst = Alloc(out_type("result"), id);
            Emit({clamp ? Op::ClampF : Op::LerpF, dst.index, x.index, y.index, z.index}, id);
            return SetPure(id, "result", dst);
        }
        if (type == "Vec3.Make") {
            const RegRef x = Input(id, "x"), y = Input(id, "y"), z = Input(id, "z");
            const RegRef dst = Alloc(out_type("result"), id);
            Emit({Op::MakeV, dst.index, x.index, y.index, z.index}, id);
            return SetPure(id, "result", dst);
        }
        if (type == "Vec3.Break") {
            const RegRef v = Input(id, "vector");
            const char* names[] = {"x", "y", "z"};
            for (u16 i = 0; i < 3; ++i) {
                const RegRef dst = Alloc(out_type(names[i]), id);
                Emit({Op::BreakV, dst.index, v.index, i}, id);
                SetPure(id, names[i], dst);
            }
            return;
        }
        if (type == "Vec3.Scale") {
            const RegRef v = Input(id, "vector"), s = Input(id, "scale");
            const RegRef dst = Alloc(out_type("result"), id);
            Emit({Op::ScaleV, dst.index, v.index, s.index}, id);
            return SetPure(id, "result", dst);
        }
        if (type == "Vec3.Length") return unary(Op::LenV, "vector");
        if (type == "Vec3.Normalize") return unary(Op::NormV, "vector");
        if (type == "Vec3.Dot") return binary(Op::DotV);
        if (type == "Vec3.Cross") return binary(Op::CrossV);
        if (type == "Entity.Self") {
            const RegRef dst = Alloc(out_type("self"), id);
            Emit({Op::Self, dst.index}, id);
            return SetPure(id, "self", dst);
        }
        if (type == "Entity.IsValid") return unary(Op::IsValid, "entity");
        if (type.rfind("Literal.", 0) == 0) {
            const PinDesc& out = sig.pins.front();
            return SetPure(id, "value", LoadValue(out.default_value, out.type, id));
        }
        if (type.rfind("Var.Get:", 0) == 0) {
            const CompiledVariable* var = out_.FindVariable(type.substr(8));
            const RegRef dst = Alloc(var->type, id);
            Emit({dst.bank == Bank::String ? Op::GetVarS : Op::GetVar, dst.index, 0, 0, var->slot.index}, id);
            return SetPure(id, "value", dst);
        }
        if (type.rfind("Comp.Get:", 0) == 0) {
            const RegRef value = Alloc(out_type("value"), id);
            EmitField(id, sig, Op::GetField, Input(id, "target"), value);
            return SetPure(id, "value", value);
        }
        if (type.rfind("Call.Native:", 0) == 0) return EmitNative(id, sig, true);
        if (type.rfind("Call.Self:", 0) == 0) return EmitCall(id, sig, out_.function_index.at(type.substr(10)), true);
        Error("BP012", id, "'" + sig.title + "' can't run yet.");
    }

    void EmitField(NodeId id, const NodeSignature& sig, Op op, RegRef target, RegRef value) {
        FieldAccess access;
        access.owner = sig.owner;
        access.field = sig.field;
        access.target = target;
        access.value = {value, sig.Find("value", op == Op::GetField ? PinDir::Out : PinDir::In)->type};
        out_.field_accesses.push_back(access);
        Emit({op, 0, 0, 0, static_cast<i32>(out_.field_accesses.size() - 1)}, id);
    }

    void EmitNative(NodeId id, const NodeSignature& sig, bool pure) {
        NativeCall call;
        call.function = sig.function;
        call.owner = sig.owner;
        for (const PinDesc& pin : sig.pins) {
            if (pin.dir != PinDir::In || pin.type.IsExec()) continue;
            if (pin.name == "target" && (pin.flags & Pin_Self)) {
                call.has_target = true;
                call.target = Input(id, pin.name);
            } else {
                call.args.push_back({Input(id, pin.name), pin.type});
            }
        }
        if (const PinDesc* ret = sig.Find("return", PinDir::Out)) {
            call.has_result = true;
            call.result = {pure ? Alloc(ret->type, id) : OutputReg(id, *ret), ret->type};
            if (pure) SetPure(id, "return", call.result.reg);
        }
        out_.native_calls.push_back(std::move(call));
        Emit({Op::CallNative, 0, 0, 0, static_cast<i32>(out_.native_calls.size() - 1)}, id);
    }

    void EmitCall(NodeId id, const NodeSignature& sig, u32 function, bool pure, Op op = Op::CallFunction) {
        FunctionCall call;
        call.function = function;
        for (const PinDesc& pin : sig.pins) {
            if (pin.type.IsExec()) continue;
            if (pin.dir == PinDir::In) {
                call.args.push_back(Input(id, pin.name));
            } else {
                const RegRef reg = pure ? Alloc(pin.type, id) : OutputReg(id, pin);
                if (pure) SetPure(id, pin.name, reg);
                call.results.push_back(reg);
            }
        }
        out_.calls.push_back(std::move(call));
        Emit({op, 0, 0, 0, static_cast<i32>(out_.calls.size() - 1)}, id);
    }

    void EmitReturn(const Node& node) {
        pure_.clear();
        evaluated_.clear();
        for (usize i = 0; i < graph_.outputs.size(); ++i) {
            const RegRef v = Input(node.id, graph_.outputs[i].name);
            const RegRef dst = fn_.results[i];
            Emit({dst.bank == Bank::String ? Op::MoveS : Op::Move, dst.index, v.index}, node.id);
        }
        Emit({Op::Ret}, node.id);
    }

    // --- Exec flow ------------------------------------------------------------
    void Chain(NodeId node, const std::string& pin) {
        const Link* link = ExecFrom(node, pin);
        if (link == nullptr) return;
        const auto key = std::make_pair(link->to.node, link->to.pin);
        if (auto it = on_path_.find(key); it != on_path_.end()) {
            Emit({Op::Jmp, 0, 0, 0, it->second}, node); // an exec loop
            return;
        }
        on_path_[key] = Here();
        EmitNode(link->to.node, link->to.pin);
        on_path_.erase(key);
    }

    u16 Slot(NodeId node) {
        const auto key = std::make_pair(graph_index_, node);
        auto it = slots_.find(key);
        if (it != slots_.end()) return it->second;
        return slots_.emplace(key, out_.state_slots++).first->second;
    }

    // The "completed" code of each latent node, after the function's body:
    // it runs when the action finishes, on a later frame, and ends there.
    void EmitResumeBlocks() {
        while (!pending_resume_.empty()) {
            const NodeId node = pending_resume_.back();
            pending_resume_.pop_back();
            out_.latents[latent_index_.at(node)].resume_pc = static_cast<u32>(Here());
            pure_.clear();
            evaluated_.clear();
            Chain(node, "completed");
            Emit({Op::Ret}, node);
        }
    }

    void EmitNode(NodeId id, const std::string& entry) {
        pure_.clear(); // pure values are cached per exec step
        evaluated_.clear();
        const Node& node = NodeOf(id);
        const NodeSignature& sig = Sig(id);
        const std::string& type = node.type;

        if (type == "Flow.DoOnce") {
            const u16 slot = Slot(id);
            if (entry == "reset") {
                Emit({Op::StateSet, slot, 0, 0}, id);
                return;
            }
            const RegRef closed = Input(id, "start_closed");
            Emit({Op::StateInit, slot, closed.index}, id);
            const usize skip = Emit({Op::StateJmpIf, slot, 0, 1}, id); // already done (or closed)
            Emit({Op::StateSet, slot, 0, 1}, id);
            Chain(id, "completed");
            fn_.code[skip].d = Here();
            return;
        }
        if (type == "Flow.Gate") {
            const u16 slot = Slot(id); // flag = closed
            const RegRef closed = Input(id, "start_closed");
            Emit({Op::StateInit, slot, closed.index}, id);
            if (entry == "open") {
                Emit({Op::StateSet, slot, 0, 0}, id);
            } else if (entry == "close") {
                Emit({Op::StateSet, slot, 0, 1}, id);
            } else if (entry == "toggle") {
                Emit({Op::StateToggle, slot}, id);
            } else {
                const usize skip = Emit({Op::StateJmpIf, slot, 0, 1}, id);
                Chain(id, "exit");
                fn_.code[skip].d = Here();
            }
            return;
        }
        if (type == "Flow.DoN") {
            const u16 slot = Slot(id);
            if (entry == "reset") {
                Emit({Op::CountReset, slot}, id);
                return;
            }
            const RegRef n = Input(id, "n");
            const usize skip = Emit({Op::CountLess, slot, n.index}, id);
            const RegRef counter = OutputReg(id, *sig.Find("counter", PinDir::Out));
            Emit({Op::CountGet, counter.index, slot}, id);
            Chain(id, "exit");
            fn_.code[skip].d = Here();
            return;
        }
        if (type == "Flow.FlipFlop") {
            const u16 slot = Slot(id); // flag = "A" was taken last
            Emit({Op::StateToggle, slot}, id);
            const RegRef is_a = OutputReg(id, *sig.Find("is_a", PinDir::Out));
            Emit({Op::StateGet, is_a.index, slot}, id);
            const usize to_b = Emit({Op::StateJmpIf, slot, 0, 0}, id);
            Chain(id, "A");
            const usize end = Emit({Op::Jmp}, id);
            fn_.code[to_b].d = Here();
            Chain(id, "B");
            fn_.code[end].d = Here();
            return;
        }
        if (sig.kind == NodeKind::Latent) {
            LatentKind kind = LatentKind::Delay;
            if (type == "Latent.RetriggerableDelay") kind = LatentKind::RetriggerableDelay;
            else if (type == "Latent.DelayNextTick") kind = LatentKind::NextTick;
            else if (type != "Latent.Delay") {
                Error("BP012", id, "'" + sig.title + "' is a latent node that can't run yet.");
                return;
            }
            RegRef duration;
            if (kind != LatentKind::NextTick) duration = Input(id, "duration");
            auto it = latent_index_.find(id);
            if (it == latent_index_.end()) {
                it = latent_index_.emplace(id, static_cast<u32>(out_.latents.size())).first;
                out_.latents.push_back({id, function_index_, 0});
                pending_resume_.push_back(id);
            }
            Emit({Op::Latent, Slot(id), duration.index, static_cast<u16>(kind), static_cast<i32>(it->second)}, id);
            return; // what follows (a Sequence's next output) runs now; "completed" runs later
        }
        if (type == "Flow.Branch") {
            const RegRef cond = Input(id, "condition");
            const usize jump_false = Emit({Op::JmpF, 0, cond.index}, id);
            Chain(id, "true");
            const usize jump_end = Emit({Op::Jmp}, id);
            fn_.code[jump_false].d = Here();
            Chain(id, "false");
            fn_.code[jump_end].d = Here();
            return;
        }
        if (type == "Flow.Sequence") {
            for (const PinDesc& pin : sig.pins) {
                if (pin.dir == PinDir::Out && pin.type.IsExec()) Chain(id, pin.name);
            }
            return;
        }
        if (type.rfind("Var.Set:", 0) == 0) {
            const CompiledVariable* var = out_.FindVariable(type.substr(8));
            const RegRef v = Input(id, "value");
            const bool str = v.bank == Bank::String;
            Emit({str ? Op::SetVarS : Op::SetVar, 0, v.index, 0, var->slot.index}, id);
            const RegRef out = OutputReg(id, *sig.Find("value", PinDir::Out));
            Emit({str ? Op::MoveS : Op::Move, out.index, v.index}, id);
            return Chain(id, "then");
        }
        if (type == "Debug.Print") {
            const RegRef text = Input(id, "text");
            Emit({Op::Print, 0, text.index}, id);
            return Chain(id, "then");
        }
        if (type.rfind("Call.Native:", 0) == 0) {
            EmitNative(id, sig, false);
            return Chain(id, "then");
        }
        if (type.rfind("Comp.Set:", 0) == 0) {
            const RegRef target = Input(id, "target");
            const RegRef value = Input(id, "value");
            EmitField(id, sig, Op::SetField, target, value);
            const RegRef out = OutputReg(id, *sig.Find("value", PinDir::Out));
            Emit({value.bank == Bank::String ? Op::MoveS : Op::Move, out.index, value.index}, id);
            return Chain(id, "then");
        }
        if (type.rfind("Call.Self:", 0) == 0) {
            EmitCall(id, sig, out_.function_index.at(type.substr(10)), false);
            return Chain(id, "then");
        }
        if (type.rfind("Call.Custom:", 0) == 0) {
            auto it = out_.events.find("Event.Custom:" + type.substr(12));
            if (it != out_.events.end()) EmitCall(id, sig, it->second, false, Op::CallEvent);
            return Chain(id, "then");
        }
        if (type == "Function.Return") {
            return EmitReturn(node);
        }
        Error("BP012", id, "'" + sig.title + "' can't run yet.");
    }

    const Graph& graph_;
    usize graph_index_;
    u32 function_index_;
    std::map<std::pair<usize, NodeId>, u16>& slots_; // shared by the graph's functions
    std::map<NodeId, u32> latent_index_;             // latent node -> CompiledBlueprint::latents
    std::vector<NodeId> pending_resume_;             // latent nodes whose "completed" code isn't emitted yet
    const std::map<NodeId, NodeSignature>& sigs_;
    CompiledBlueprint& out_;
    CompiledFunction& fn_;
    ValidationResult& diagnostics_;
    std::map<std::pair<NodeId, std::string>, RegRef> outputs_; // impure node and entry outputs
    std::map<std::pair<NodeId, std::string>, RegRef> pure_;    // pure outputs in the current exec step
    std::set<NodeId> evaluated_;
    std::map<std::pair<NodeId, std::string>, i32> on_path_; // (node, entry pin) being emitted -> first instruction
};

} // namespace

const CompiledVariable* CompiledBlueprint::FindVariable(std::string_view name) const {
    for (const CompiledVariable& v : variables) {
        if (v.name == name) return &v;
    }
    return nullptr;
}

const CompiledFunction* CompiledBlueprint::FindEvent(std::string_view key) const {
    auto it = events.find(std::string(key));
    return it != events.end() ? &functions[it->second] : nullptr;
}

CompileResult CompileBlueprint(const Blueprint& blueprint) {
    CompileResult result;
    result.diagnostics = ValidateBlueprint(blueprint);
    if (!result.diagnostics.Ok()) {
        return result;
    }
    auto out = std::make_shared<CompiledBlueprint>();

    for (const Variable& v : blueprint.variables) {
        CompiledVariable var;
        var.name = v.name;
        var.type = v.type;
        var.flags = v.flags;
        if (BankOf(v.type) == Bank::String) {
            var.slot = {Bank::String, out->string_vars++};
            if (const std::string* s = std::get_if<std::string>(&v.default_value)) var.default_string = *s;
        } else {
            var.slot = {Bank::Value, out->value_vars++};
            var.default_value = RegFromValue(v.default_value, v.type);
        }
        out->variables.push_back(std::move(var));
    }

    // Every graph's signatures, and a function per event and function graph
    // (indices first, so calls can refer to functions compiled later).
    std::vector<std::map<NodeId, NodeSignature>> sigs(blueprint.graphs.size());
    struct Job {
        usize graph;
        const Node* event; // null for a function graph
    };
    std::vector<Job> jobs;
    for (usize g = 0; g < blueprint.graphs.size(); ++g) {
        const Graph& graph = blueprint.graphs[g];
        for (const Node& node : graph.nodes) {
            sigs[g].emplace(node.id, *ResolveNode(blueprint, graph, node));
        }
        if (graph.kind == GraphKind::Function) {
            out->function_index[graph.name] = static_cast<u32>(out->functions.size());
            CompiledFunction fn;
            fn.name = graph.name;
            fn.graph = graph.name;
            out->functions.push_back(std::move(fn));
            jobs.push_back({g, nullptr});
        } else if (graph.kind == GraphKind::EventGraph) {
            for (const Node& node : graph.nodes) {
                const NodeSignature& sig = sigs[g].at(node.id);
                if (!sig.IsEvent()) continue;
                out->events[sig.event_key] = static_cast<u32>(out->functions.size());
                CompiledFunction fn;
                fn.name = sig.event_key;
                fn.graph = graph.name;
                out->functions.push_back(std::move(fn));
                jobs.push_back({g, &node});
            }
        }
    }
    std::map<std::pair<usize, NodeId>, u16> slots;
    for (usize i = 0; i < jobs.size(); ++i) {
        const Graph& graph = blueprint.graphs[jobs[i].graph];
        FunctionCompiler compiler(graph, jobs[i].graph, static_cast<u32>(i), sigs[jobs[i].graph], *out, out->functions[i], result.diagnostics, slots);
        if (jobs[i].event != nullptr) {
            compiler.CompileEvent(*jobs[i].event);
        } else {
            compiler.CompileFunction();
        }
    }
    if (result.diagnostics.Ok()) {
        result.blueprint = std::move(out);
    }
    return result;
}

} // namespace aether::bp
