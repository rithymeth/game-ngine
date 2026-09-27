#include "aether/blueprint/compiler.h"

#include "aether/reflection/reflection.h"

#include <algorithm>
#include <set>

namespace aether::bp {

namespace {

bool Unsupported(const PinType& type) {
    return type.type == ValueType::Struct || type.type == ValueType::Wildcard || type.type == ValueType::None;
}

Bank BankOf(const PinType& type) {
    if (type.is_array) return Bank::Array;
    return type.type == ValueType::String ? Bank::String : Bank::Value;
}

Op MoveOp(Bank bank) { return bank == Bank::Array ? Op::MoveA : bank == Bank::String ? Op::MoveS : Op::Move; }

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
        if (BankOf(type) == Bank::Array) return {Bank::Array, fn_.array_regs++};
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
        if (dst.bank == Bank::Array) {
            Emit({Op::NewA, dst.index, 0, static_cast<u16>(type.type)}, node);
        } else if (dst.bank == Bank::String) {
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
            const Op get = dst.bank == Bank::Array ? Op::GetVarA : dst.bank == Bank::String ? Op::GetVarS : Op::GetVar;
            Emit({get, dst.index, 0, 0, var->slot.index}, id);
            return SetPure(id, "value", dst);
        }
        if (type.rfind("Comp.Get:", 0) == 0) {
            const RegRef value = Alloc(out_type("value"), id);
            EmitField(id, sig, Op::GetField, Input(id, "target"), value);
            return SetPure(id, "value", value);
        }
        if (type.rfind("Call.Native:", 0) == 0) return EmitNative(id, sig, true);
        if (type.rfind("Call.Self:", 0) == 0) return EmitCall(id, sig, out_.function_index.at(type.substr(10)), true);
        if (EvaluateMore(id, sig)) return;
        Error("BP012", id, "'" + sig.title + "' can't run yet.");
    }

    RegRef Const(i32 value, NodeId node) { return LoadValue(value, PinType::Of(ValueType::Int), node); }
    RegRef ConstF(f32 value, NodeId node) { return LoadValue(value, PinType::Of(ValueType::Float), node); }

    // Pure array nodes.
    bool EvaluateArray(NodeId id, const NodeSignature& sig, const std::string& type) {
        auto is = [&](const char* op) { return type.rfind(std::string("Array.") + op + ":", 0) == 0; };
        const PinType i32t = PinType::Of(ValueType::Int), boolt = PinType::Of(ValueType::Bool);
        if (is("Make")) {
            const PinDesc& out = *sig.Find("array", PinDir::Out);
            const RegRef dst = LoadValue({}, out.type, id); // NEWA
            for (const PinDesc& pin : sig.pins) {
                if (pin.dir != PinDir::In) continue;
                const RegRef item = Input(id, pin.name);
                Emit({Op::PushA, dst.index, item.index}, id);
            }
            SetPure(id, "array", dst);
            return true;
        }
        const RegRef array = Input(id, "array");
        if (is("Length") || is("LastIndex")) {
            RegRef dst = Alloc(i32t, id);
            Emit({Op::LenA, dst.index, array.index}, id);
            if (is("LastIndex")) {
                const RegRef one = Const(1, id), last = Alloc(i32t, id);
                Emit({Op::SubI, last.index, dst.index, one.index}, id);
                dst = last;
            }
            SetPure(id, "result", dst);
            return true;
        }
        if (is("Get")) {
            const RegRef index = Input(id, "index");
            const RegRef dst = Alloc(sig.Find("item", PinDir::Out)->type, id);
            Emit({Op::GetA, dst.index, array.index, index.index}, id);
            SetPure(id, "item", dst);
            return true;
        }
        if (is("IsValidIndex")) {
            const RegRef index = Input(id, "index");
            const RegRef dst = Alloc(boolt, id);
            Emit({Op::ValidIdxA, dst.index, array.index, index.index}, id);
            SetPure(id, "result", dst);
            return true;
        }
        if (is("Find") || is("Contains")) {
            const RegRef item = Input(id, "item");
            const RegRef found = Alloc(i32t, id);
            Emit({Op::FindA, found.index, array.index, item.index}, id);
            if (is("Find")) {
                SetPure(id, "index", found);
            } else {
                const RegRef none = Const(-1, id), dst = Alloc(boolt, id);
                Emit({Op::NeI, dst.index, found.index, none.index}, id);
                SetPure(id, "result", dst);
            }
            return true;
        }
        return false;
    }

    // Math, select, strings and conversions (Phase 12 step 4).
    bool EvaluateMore(NodeId id, const NodeSignature& sig) {
        const std::string& type = NodeOf(id).type;
        auto out_type = [&](const std::string& pin) { return sig.Find(pin, PinDir::Out)->type; };
        auto emit = [&](Op op, std::vector<RegRef> in, const char* out = "result", u16 c_immediate = 0,
                        bool use_immediate = false) {
            const RegRef dst = Alloc(out_type(out), id);
            Instr instr{op, dst.index};
            if (in.size() > 0) instr.b = in[0].index;
            if (use_immediate) instr.c = c_immediate;
            else if (in.size() > 1) instr.c = in[1].index;
            if (in.size() > 2) instr.d = in[2].index;
            Emit(instr, id);
            SetPure(id, out, dst);
            return dst;
        };
        static const std::pair<const char*, MathFn> kMathFns[] = {
            {"Math.Sin", MathFn::Sin},   {"Math.Cos", MathFn::Cos},   {"Math.Tan", MathFn::Tan},
            {"Math.Asin", MathFn::Asin}, {"Math.Acos", MathFn::Acos}, {"Math.Atan", MathFn::Atan},
            {"Math.Sqrt", MathFn::Sqrt}, {"Math.Exp", MathFn::Exp},   {"Math.Log", MathFn::Log},
            {"Math.Frac", MathFn::Frac}, {"Math.DegreesToRadians", MathFn::DegToRad},
            {"Math.RadiansToDegrees", MathFn::RadToDeg}};
        for (const auto& [name, fn] : kMathFns) {
            if (type == name) return emit(Op::MathF, {Input(id, "a")}, "result", static_cast<u16>(fn), true), true;
        }
        static const std::pair<const char*, RoundMode> kRounding[] = {
            {"Math.Floor", RoundMode::Floor}, {"Math.Ceil", RoundMode::Ceil},
            {"Math.Round", RoundMode::Round}, {"Math.Truncate", RoundMode::Truncate}};
        for (const auto& [name, mode] : kRounding) {
            if (type == name) return emit(Op::RoundF, {Input(id, "a")}, "result", static_cast<u16>(mode), true), true;
        }
        if (type == "Math.Atan2") return emit(Op::Atan2F, {Input(id, "y"), Input(id, "x")}), true;
        if (type == "Math.Power") return emit(Op::PowF, {Input(id, "base"), Input(id, "exponent")}), true;
        if (type == "Math.Negate:int") return emit(Op::NegI, {Input(id, "a")}), true;
        if (type == "Math.Abs:int") return emit(Op::AbsI, {Input(id, "a")}), true;
        if (type == "Math.Clamp:int") {
            return emit(Op::ClampI, {Input(id, "value"), Input(id, "min"), Input(id, "max")}), true;
        }
        if (type == "Math.NearlyEqual:float") {
            return emit(Op::NearEqF, {Input(id, "a"), Input(id, "b"), Input(id, "tolerance")}), true;
        }
        if (type == "Math.MapRange:float") {
            // out_min + clamp((value - in_min) / (in_max - in_min), 0, 1) * (out_max - out_min)
            const PinType f = PinType::Of(ValueType::Float);
            const RegRef v = Input(id, "value"), in_min = Input(id, "in_min"), in_max = Input(id, "in_max");
            const RegRef out_min = Input(id, "out_min"), out_max = Input(id, "out_max");
            const RegRef num = Alloc(f, id), den = Alloc(f, id), t = Alloc(f, id), ct = Alloc(f, id);
            Emit({Op::SubF, num.index, v.index, in_min.index}, id);
            Emit({Op::SubF, den.index, in_max.index, in_min.index}, id);
            Emit({Op::DivF, t.index, num.index, den.index}, id);
            const RegRef zero = ConstF(0.0f, id), one = ConstF(1.0f, id);
            Emit({Op::ClampF, ct.index, t.index, zero.index, one.index}, id);
            const RegRef dst = Alloc(f, id);
            Emit({Op::LerpF, dst.index, out_min.index, out_max.index, ct.index}, id);
            SetPure(id, "result", dst);
            return true;
        }
        if (type == "Math.RandomFloatInRange") return emit(Op::RandF, {Input(id, "min"), Input(id, "max")}), true;
        if (type == "Math.RandomIntInRange") return emit(Op::RandI, {Input(id, "min"), Input(id, "max")}), true;
        if (type == "Math.RandomBool") return emit(Op::RandB, {}), true;

        if (type.rfind("Flow.Select:", 0) == 0) {
            // dst = option[index], or the type's default when out of range.
            const PinDesc& ret = *sig.Find("return", PinDir::Out);
            const RegRef index = Input(id, "index");
            std::vector<RegRef> options;
            for (const PinDesc& pin : sig.pins) {
                if (pin.dir == PinDir::In && pin.name.rfind("option ", 0) == 0) options.push_back(Input(id, pin.name));
            }
            const RegRef dst = LoadValue(DefaultValue(ret.type), ret.type, id);
            const Op move = MoveOp(dst.bank);
            std::vector<usize> to_end;
            for (usize i = 0; i < options.size(); ++i) {
                const RegRef k = Const(static_cast<i32>(i), id);
                const RegRef hit = Alloc(PinType::Of(ValueType::Bool), id);
                Emit({Op::EqI, hit.index, index.index, k.index}, id);
                const usize skip = Emit({Op::JmpF, 0, hit.index}, id);
                Emit({move, dst.index, options[i].index}, id);
                to_end.push_back(Emit({Op::Jmp}, id));
                fn_.code[skip].d = Here();
            }
            for (usize j : to_end) fn_.code[j].d = Here();
            SetPure(id, "return", dst);
            return true;
        }

        // Strings.
        if (type == "String.Append") {
            RegRef acc = Input(id, "a");
            for (const PinDesc& pin : sig.pins) {
                if (pin.dir != PinDir::In || pin.name == "a") continue;
                const RegRef next = Input(id, pin.name);
                const RegRef dst = Alloc(PinType::Of(ValueType::String), id);
                Emit({Op::ConcatS, dst.index, acc.index, next.index}, id);
                acc = dst;
            }
            SetPure(id, "result", acc);
            return true;
        }
        if (type == "Text.Format") {
            // Literal pieces and placeholders, concatenated in order.
            const std::string format = NodeOf(id).config.value("format", "");
            const PinType str = PinType::Of(ValueType::String);
            RegRef acc = LoadValue(std::string(), str, id);
            std::string literal;
            auto flush = [&] {
                if (literal.empty()) return;
                const RegRef piece = LoadValue(literal, str, id);
                const RegRef dst = Alloc(str, id);
                Emit({Op::ConcatS, dst.index, acc.index, piece.index}, id);
                acc = dst;
                literal.clear();
            };
            for (usize i = 0; i < format.size(); ++i) {
                const char c = format[i];
                if ((c == '{' || c == '}') && i + 1 < format.size() && format[i + 1] == c) {
                    literal.push_back(c); // "{{" and "}}"
                    ++i;
                    continue;
                }
                const usize end = c == '{' ? format.find('}', i + 1) : std::string::npos;
                if (end == std::string::npos || end == i + 1) {
                    literal.push_back(c);
                    continue;
                }
                flush();
                const RegRef value = Input(id, format.substr(i + 1, end - i - 1));
                const RegRef dst = Alloc(str, id);
                Emit({Op::ConcatS, dst.index, acc.index, value.index}, id);
                acc = dst;
                i = end;
            }
            flush();
            SetPure(id, "result", acc);
            return true;
        }
        if (type == "String.Length") return emit(Op::LenS, {Input(id, "text")}), true;
        if (type == "String.IsEmpty") return emit(Op::EmptyS, {Input(id, "text")}), true;
        if (type == "String.Contains") {
            return emit(Op::ContainsS, {Input(id, "text"), Input(id, "substring"), Input(id, "ignore_case")}), true;
        }
        static const std::pair<const char*, StrFnKind> kStrFns[] = {
            {"String.ToUpper", StrFnKind::Upper}, {"String.ToLower", StrFnKind::Lower}, {"String.Trim", StrFnKind::Trim}};
        for (const auto& [name, fn] : kStrFns) {
            if (type == name) return emit(Op::StrFn, {Input(id, "text")}, "result", static_cast<u16>(fn), true), true;
        }
        if (type == "String.ToInt" || type == "String.ToFloat") {
            const RegRef text = Input(id, "text");
            const RegRef value = Alloc(out_type("result"), id), ok = Alloc(out_type("success"), id);
            Emit({type == "String.ToInt" ? Op::ParseI : Op::ParseF, value.index, text.index, ok.index}, id);
            SetPure(id, "result", value);
            SetPure(id, "success", ok);
            return true;
        }
        if (type.rfind("Array.", 0) == 0) return EvaluateArray(id, sig, type);
        static const std::pair<const char*, Op> kEntityGets[] = {
            {"Entity.GetLocation", Op::GetLoc}, {"Entity.GetRotation", Op::GetRot},
            {"Entity.GetWorldLocation", Op::WorldLoc}, {"Entity.GetParent", Op::GetParentOp}};
        for (const auto& [name, op] : kEntityGets) {
            if (type == name) {
                const PinDesc& out = *std::find_if(sig.pins.begin(), sig.pins.end(),
                                                   [](const PinDesc& p) { return p.dir == PinDir::Out; });
                const RegRef target = Input(id, "target");
                const RegRef dst = Alloc(out.type, id);
                Emit({op, dst.index, target.index}, id);
                SetPure(id, out.name, dst);
                return true;
            }
        }
        if (type == "Quat.RotateVector") return emit(Op::QuatRotate, {Input(id, "rotation"), Input(id, "vector")}), true;
        if (type == "Quat.FromAxisAngle") return emit(Op::QuatAxisAngle, {Input(id, "axis"), Input(id, "degrees")}), true;
        if (type == "Quat.Multiply") return emit(Op::QuatMul, {Input(id, "a"), Input(id, "b")}), true;
        if (type == "Entity.HasTag") return emit(Op::HasTagOp, {Input(id, "target"), Input(id, "tag")}), true;
        if (type == "Entity.FindWithTag") return emit(Op::FindTagOp, {Input(id, "tag")}, "entities"), true;
        if (type.rfind("Interface.Implements:", 0) == 0) {
            const RegRef target = Input(id, "target");
            const RegRef dst = Alloc(PinType::Of(ValueType::Bool), id);
            out_.names.push_back(type.substr(21));
            Emit({Op::ImplementsOp, dst.index, target.index, 0, static_cast<i32>(out_.names.size() - 1)}, id);
            SetPure(id, "result", dst);
            return true;
        }
        if (type == "World.GameTime") return emit(Op::GameTime, {}, "seconds"), true;
        if (type == "World.DeltaSeconds") return emit(Op::DeltaTime, {}, "seconds"), true;
        if (type.rfind("Conv.ToString:", 0) == 0) {
            const PinDesc& in = *sig.Find("value", PinDir::In);
            const RegRef value = Input(id, "value");
            const RegRef dst = Alloc(PinType::Of(ValueType::String), id);
            Emit({Op::ToString, dst.index, value.index, static_cast<u16>(in.type.type)}, id);
            SetPure(id, "result", dst);
            return true;
        }
        return false;
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
            Emit({MoveOp(dst.bank), dst.index, v.index}, node.id);
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
        static const std::pair<const char*, std::pair<Op, const char*>> kEntitySets[] = {
            {"Entity.SetLocation", {Op::SetLoc, "location"}}, {"Entity.SetRotation", {Op::SetRot, "rotation"}},
            {"Entity.AddOffset", {Op::AddOffset, "offset"}},   {"Entity.AddTag", {Op::AddTagOp, "tag"}},
            {"Entity.RemoveTag", {Op::RemoveTagOp, "tag"}},    {"Entity.AttachTo", {Op::AttachOp, "parent"}},
            {"Entity.Detach", {Op::DetachOp, nullptr}},        {"Entity.Destroy", {Op::DestroyOp, nullptr}}};
        for (const auto& [name, op] : kEntitySets) {
            if (type != name) continue;
            const RegRef target = Input(id, "target");
            const u16 value = op.second != nullptr ? Input(id, op.second).index : 0;
            Emit({op.first, 0, target.index, value}, id);
            return Chain(id, "then");
        }
        if (type.rfind("Dispatch.Call:", 0) == 0 || type.rfind("Interface.Call:", 0) == 0) {
            const bool dispatcher = type.rfind("Dispatch.Call:", 0) == 0;
            std::vector<TypedReg> args;
            RegRef target;
            for (const PinDesc& pin : sig.pins) {
                if (pin.dir != PinDir::In || pin.type.IsExec()) continue;
                if (pin.name == "target") target = Input(id, pin.name);
                else args.push_back({Input(id, pin.name), pin.type});
            }
            if (dispatcher) {
                out_.dispatcher_calls.push_back({type.substr(14), target, std::move(args)});
                Emit({Op::CallDispatcher, 0, 0, 0, static_cast<i32>(out_.dispatcher_calls.size() - 1)}, id);
            } else {
                out_.interface_calls.push_back({type.substr(15), target, std::move(args)});
                Emit({Op::InterfaceCall, 0, 0, 0, static_cast<i32>(out_.interface_calls.size() - 1)}, id);
            }
            return Chain(id, "then");
        }
        if (type.rfind("Dispatch.Bind:", 0) == 0 || type.rfind("Dispatch.Unbind:", 0) == 0 ||
            type.rfind("Dispatch.UnbindAll:", 0) == 0) {
            DispatcherBind bind;
            bind.mode = type.rfind("Dispatch.Bind:", 0) == 0     ? DispatcherBind::Mode::Bind
                        : type.rfind("Dispatch.Unbind:", 0) == 0 ? DispatcherBind::Mode::Unbind
                                                                 : DispatcherBind::Mode::UnbindAll;
            bind.dispatcher = type.substr(type.find(':') + 1);
            bind.event = NodeOf(id).config.value("event", "");
            bind.target = Input(id, "target");
            out_.dispatcher_binds.push_back(std::move(bind));
            Emit({Op::BindDispatcher, 0, 0, 0, static_cast<i32>(out_.dispatcher_binds.size() - 1)}, id);
            return Chain(id, "then");
        }
        if (type == "Entity.Spawn") {
            const RegRef location = Input(id, "location"), rotation = Input(id, "rotation");
            const RegRef spawned = OutputReg(id, *sig.Find("spawned", PinDir::Out));
            out_.spawn_assets.push_back(NodeOf(id).config.value("blueprint", ""));
            Emit({Op::SpawnOp, spawned.index, location.index, rotation.index,
                  static_cast<i32>(out_.spawn_assets.size() - 1)}, id);
            return Chain(id, "then");
        }
        if (type.rfind("Flow.ForEach:", 0) == 0) {
            // Iterates a copy: changing the array in the body doesn't affect the loop.
            const RegRef source = Input(id, "array");
            const RegRef array = Alloc(sig.Find("array", PinDir::In)->type, id);
            Emit({Op::MoveA, array.index, source.index}, id);
            const RegRef index = OutputReg(id, *sig.Find("index", PinDir::Out));
            const RegRef element = OutputReg(id, *sig.Find("element", PinDir::Out));
            const PinType i32t = PinType::Of(ValueType::Int);
            Emit({Op::Move, index.index, Const(0, id).index}, id);
            const RegRef one = Const(1, id), length = Alloc(i32t, id);
            Emit({Op::LenA, length.index, array.index}, id);
            const i32 top = Here();
            const RegRef more = Alloc(PinType::Of(ValueType::Bool), id);
            Emit({Op::LtI, more.index, index.index, length.index}, id);
            const usize exit = Emit({Op::JmpF, 0, more.index}, id);
            Emit({Op::GetA, element.index, array.index, index.index}, id);
            Chain(id, "loop_body");
            Emit({Op::AddI, index.index, index.index, one.index}, id);
            Emit({Op::Jmp, 0, 0, 0, top}, id);
            fn_.code[exit].d = Here();
            return Chain(id, "completed");
        }
        if (type.rfind("Array.", 0) == 0) {
            // Changes an array variable in place (validation made sure it is one).
            const Link* link = LinkInto(id, "array");
            const CompiledVariable* var = out_.FindVariable(NodeOf(link->from.node).type.substr(8));
            const i32 slot = var->slot.index;
            auto is = [&](const char* op) { return type.rfind(std::string("Array.") + op + ":", 0) == 0; };
            auto item = [&] { return Input(id, "item").index; };
            auto index = [&] { return Input(id, "index").index; };
            if (is("Add") || is("AddUnique")) {
                const u16 value = item();
                const RegRef dst = OutputReg(id, *sig.Find("index", PinDir::Out));
                Emit({is("Add") ? Op::VarAdd : Op::VarAddUnique, dst.index, value, 0, slot}, id);
            } else if (is("Insert")) {
                const u16 value = item(), at = index();
                Emit({Op::VarInsert, 0, value, at, slot}, id);
            } else if (is("RemoveIndex")) {
                Emit({Op::VarRemoveAt, 0, 0, index(), slot}, id);
            } else if (is("RemoveItem")) {
                const u16 value = item();
                const RegRef dst = OutputReg(id, *sig.Find("removed", PinDir::Out));
                Emit({Op::VarRemoveItem, dst.index, value, 0, slot}, id);
            } else if (is("Clear")) {
                Emit({Op::VarClear, 0, 0, 0, slot}, id);
            } else if (is("Set")) {
                const u16 at = index(), value = item();
                Emit({Op::VarSetAt, 0, value, at, slot}, id);
            } else if (is("Reverse")) {
                Emit({Op::VarReverse, 0, 0, 0, slot}, id);
            } else {
                Error("BP012", id, "'" + sig.title + "' can't run yet.");
                return;
            }
            return Chain(id, "then");
        }
        if (type == "Flow.ForLoop" || type == "Flow.ForLoopWithBreak") {
            const bool breakable = type == "Flow.ForLoopWithBreak";
            const PinType b = PinType::Of(ValueType::Bool);
            const PinDesc break_flag{"$break", PinDir::Out, b, {}, Pin_None};
            if (entry == "break") {
                const RegRef yes = LoadValue(true, b, id);
                Emit({Op::Move, OutputReg(id, break_flag).index, yes.index}, id);
                return; // the loop checks the flag after this iteration's body
            }
            const RegRef first = Input(id, "first"), last = Input(id, "last");
            const RegRef index = OutputReg(id, *sig.Find("index", PinDir::Out));
            Emit({Op::Move, index.index, first.index}, id);
            const RegRef one = Const(1, id);
            RegRef stop;
            if (breakable) {
                stop = OutputReg(id, break_flag);
                Emit({Op::Move, stop.index, LoadValue(false, b, id).index}, id);
            }
            const i32 top = Here();
            const RegRef more = Alloc(b, id);
            Emit({Op::LeI, more.index, index.index, last.index}, id);
            const usize exit = Emit({Op::JmpF, 0, more.index}, id);
            Chain(id, "loop_body");
            usize broke = 0;
            if (breakable) {
                const usize go_on = Emit({Op::JmpF, 0, stop.index}, id);
                broke = Emit({Op::Jmp}, id);
                fn_.code[go_on].d = Here();
            }
            Emit({Op::AddI, index.index, index.index, one.index}, id);
            Emit({Op::Jmp, 0, 0, 0, top}, id);
            fn_.code[exit].d = Here();
            if (breakable) fn_.code[broke].d = Here();
            return Chain(id, "completed");
        }
        if (type == "Flow.WhileLoop") {
            const i32 top = Here();
            pure_.clear(); // the condition is evaluated again every iteration
            evaluated_.clear();
            const RegRef cond = Input(id, "condition");
            const usize exit = Emit({Op::JmpF, 0, cond.index}, id);
            Chain(id, "loop_body");
            Emit({Op::Jmp, 0, 0, 0, top}, id);
            fn_.code[exit].d = Here();
            return Chain(id, "completed");
        }
        if (type == "Flow.SwitchInt" || type == "Flow.SwitchString") {
            const bool strings = type == "Flow.SwitchString";
            const RegRef selection = Input(id, "selection");
            std::vector<usize> to_end;
            for (const PinDesc& pin : sig.pins) {
                if (pin.dir != PinDir::Out || pin.name == "default") continue;
                const RegRef k = strings ? LoadValue(pin.name, PinType::Of(ValueType::String), id)
                                         : Const(std::stoi(pin.name), id);
                const RegRef hit = Alloc(PinType::Of(ValueType::Bool), id);
                Emit({strings ? Op::EqS : Op::EqI, hit.index, selection.index, k.index}, id);
                const usize skip = Emit({Op::JmpF, 0, hit.index}, id);
                Chain(id, pin.name);
                to_end.push_back(Emit({Op::Jmp}, id));
                fn_.code[skip].d = Here();
            }
            Chain(id, "default");
            for (usize j : to_end) fn_.code[j].d = Here();
            return;
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
            const Op set = v.bank == Bank::Array ? Op::SetVarA : v.bank == Bank::String ? Op::SetVarS : Op::SetVar;
            Emit({set, 0, v.index, 0, var->slot.index}, id);
            const RegRef out = OutputReg(id, *sig.Find("value", PinDir::Out));
            Emit({MoveOp(v.bank), out.index, v.index}, id);
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
            Emit({MoveOp(value.bank), out.index, value.index}, id);
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

    out->interfaces = blueprint.interfaces;
    for (const Variable& v : blueprint.variables) {
        CompiledVariable var;
        var.name = v.name;
        var.type = v.type;
        var.flags = v.flags;
        if (BankOf(v.type) == Bank::Array) {
            var.slot = {Bank::Array, out->array_vars++};
        } else if (BankOf(v.type) == Bank::String) {
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
