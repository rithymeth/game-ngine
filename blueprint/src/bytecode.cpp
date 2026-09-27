#include "aether/blueprint/bytecode.h"

#include "aether/reflection/reflection.h"

#include <cstdio>

namespace aether::bp {

namespace {

// Operand layout per opcode, for the disassembler: letters for a, b, c, d.
//   z = state slot, l = latent kind, L = latent resume point,
//   r = value register, s = string register, k = constant, K = string
//   constant, v = variable slot, @ = jump target, n/f/F/C = native call,
//   field access, function call, event call, i = immediate, - = unused.
struct OpInfo {
    const char* name;
    const char* layout;
};

OpInfo Info(Op op) {
    switch (op) {
    case Op::Nop: return {"NOP", "----"};
    case Op::LoadK: return {"LOADK", "r--k"};
    case Op::LoadKS: return {"LOADKS", "s--K"};
    case Op::Move: return {"MOVE", "rr--"};
    case Op::MoveS: return {"MOVES", "ss--"};
    case Op::GetVar: return {"GETVAR", "r--v"};
    case Op::SetVar: return {"SETVAR", "-r-v"};
    case Op::GetVarS: return {"GETVARS", "s--v"};
    case Op::SetVarS: return {"SETVARS", "-s-v"};
    case Op::AddI: return {"ADD_I", "rrr-"};
    case Op::SubI: return {"SUB_I", "rrr-"};
    case Op::MulI: return {"MUL_I", "rrr-"};
    case Op::DivI: return {"DIV_I", "rrr-"};
    case Op::ModI: return {"MOD_I", "rrr-"};
    case Op::MinI: return {"MIN_I", "rrr-"};
    case Op::MaxI: return {"MAX_I", "rrr-"};
    case Op::AddF: return {"ADD_F", "rrr-"};
    case Op::SubF: return {"SUB_F", "rrr-"};
    case Op::MulF: return {"MUL_F", "rrr-"};
    case Op::DivF: return {"DIV_F", "rrr-"};
    case Op::MinF: return {"MIN_F", "rrr-"};
    case Op::MaxF: return {"MAX_F", "rrr-"};
    case Op::AddV: return {"ADD_V3", "rrr-"};
    case Op::SubV: return {"SUB_V3", "rrr-"};
    case Op::MulV: return {"MUL_V3", "rrr-"};
    case Op::ScaleV: return {"SCALE_V3", "rrr-"};
    case Op::LtI: return {"CMP_LT_I", "rrr-"};
    case Op::LeI: return {"CMP_LE_I", "rrr-"};
    case Op::GtI: return {"CMP_GT_I", "rrr-"};
    case Op::GeI: return {"CMP_GE_I", "rrr-"};
    case Op::EqI: return {"CMP_EQ_I", "rrr-"};
    case Op::NeI: return {"CMP_NE_I", "rrr-"};
    case Op::LtF: return {"CMP_LT_F", "rrr-"};
    case Op::LeF: return {"CMP_LE_F", "rrr-"};
    case Op::GtF: return {"CMP_GT_F", "rrr-"};
    case Op::GeF: return {"CMP_GE_F", "rrr-"};
    case Op::EqF: return {"CMP_EQ_F", "rrr-"};
    case Op::NeF: return {"CMP_NE_F", "rrr-"};
    case Op::EqB: return {"CMP_EQ_B", "rrr-"};
    case Op::NeB: return {"CMP_NE_B", "rrr-"};
    case Op::EqE: return {"CMP_EQ_E", "rrr-"};
    case Op::NeE: return {"CMP_NE_E", "rrr-"};
    case Op::EqS: return {"CMP_EQ_S", "rss-"};
    case Op::NeS: return {"CMP_NE_S", "rss-"};
    case Op::And: return {"AND", "rrr-"};
    case Op::Or: return {"OR", "rrr-"};
    case Op::Xor: return {"XOR", "rrr-"};
    case Op::Not: return {"NOT", "rr--"};
    case Op::NegF: return {"NEG_F", "rr--"};
    case Op::AbsF: return {"ABS_F", "rr--"};
    case Op::ClampF: return {"CLAMP_F", "rrrr"};
    case Op::LerpF: return {"LERP_F", "rrrr"};
    case Op::MakeV: return {"MAKE_V3", "rrrr"};
    case Op::BreakV: return {"BREAK_V3", "rri-"};
    case Op::LenV: return {"LEN_V3", "rr--"};
    case Op::DotV: return {"DOT_V3", "rrr-"};
    case Op::CrossV: return {"CROSS_V3", "rrr-"};
    case Op::NormV: return {"NORM_V3", "rr--"};
    case Op::IntToFloat: return {"CONV_I2F", "rr--"};
    case Op::FloatToInt: return {"CONV_F2I", "rr--"};
    case Op::ToString: return {"TOSTR", "sri-"};
    case Op::Self: return {"GETSELF", "r---"};
    case Op::IsValid: return {"ISVALID", "rr--"};
    case Op::Jmp: return {"JMP", "---@"};
    case Op::JmpF: return {"JMPF", "-r-@"};
    case Op::Print: return {"PRINT", "-s--"};
    case Op::CallNative: return {"CALLN", "---n"};
    case Op::GetField: return {"GETFIELD", "---f"};
    case Op::SetField: return {"SETFIELD", "---f"};
    case Op::CallFunction: return {"CALLB", "---F"};
    case Op::CallEvent: return {"CALLE", "---C"};
    case Op::StateInit: return {"STATE_INIT", "zr--"};
    case Op::StateJmpIf: return {"STATE_JMPIF", "z-i@"};
    case Op::StateSet: return {"STATE_SET", "z-i-"};
    case Op::StateToggle: return {"STATE_TOGGLE", "z---"};
    case Op::StateGet: return {"STATE_GET", "rz--"};
    case Op::CountLess: return {"COUNT_LESS", "zr-@"};
    case Op::CountGet: return {"COUNT_GET", "rz--"};
    case Op::CountReset: return {"COUNT_RESET", "z---"};
    case Op::Latent: return {"LATENT", "zrlL"};
    case Op::Ret: return {"RET", "----"};
    }
    return {"?", "----"};
}

std::string RegName(RegRef r) { return (r.bank == Bank::String ? "s" : "r") + std::to_string(r.index); }

} // namespace

std::string Disassemble(const CompiledBlueprint& bp, const CompiledFunction& fn) {
    std::string text;
    char line[256];
    for (usize pc = 0; pc < fn.code.size(); ++pc) {
        const Instr& in = fn.code[pc];
        const OpInfo info = Info(in.op);
        std::string operands;
        const i32 values[4] = {in.a, in.b, in.c, in.d};
        for (int i = 0; i < 4; ++i) {
            const char kind = info.layout[i];
            if (kind == '-') continue;
            if (!operands.empty()) operands += ", ";
            const i32 v = values[i];
            switch (kind) {
            case 'r': operands += "r" + std::to_string(v); break;
            case 's': operands += "s" + std::to_string(v); break;
            case 'i': operands += std::to_string(v); break;
            case '@': operands += "@" + std::to_string(v); break;
            case 'z': operands += "slot" + std::to_string(v); break;
            case 'l': {
                static const char* kKinds[] = {"delay", "retrigger", "next-tick"};
                operands += v >= 0 && v < 3 ? kKinds[v] : "?";
                break;
            }
            case 'L': operands += "resume@" + std::to_string(bp.latents[static_cast<usize>(v)].resume_pc); break;
            case 'k': operands += "k" + std::to_string(v); break;
            case 'K': operands += "\"" + bp.string_constants[static_cast<usize>(v)] + "\""; break;
            case 'v': {
                std::string name = "?";
                for (const CompiledVariable& var : bp.variables) {
                    if (var.slot.index == v && (var.slot.bank == Bank::String) == (in.op == Op::GetVarS || in.op == Op::SetVarS)) {
                        name = var.name;
                    }
                }
                operands += name;
                break;
            }
            case 'n': {
                const NativeCall& call = bp.native_calls[static_cast<usize>(v)];
                operands += std::string(call.owner != nullptr ? call.owner->name : "") + "." + call.function->name + "(";
                if (call.has_target) operands += "self=" + RegName(call.target);
                for (const TypedReg& arg : call.args) {
                    if (operands.back() != '(') operands += " ";
                    operands += RegName(arg.reg);
                }
                operands += ")";
                if (call.has_result) operands += " -> " + RegName(call.result.reg);
                break;
            }
            case 'f': {
                const FieldAccess& access = bp.field_accesses[static_cast<usize>(v)];
                operands += RegName(access.target) + "." + access.owner->name + "." + access.field->name + " " +
                            (in.op == Op::GetField ? "-> " : "<- ") + RegName(access.value.reg);
                break;
            }
            case 'F':
            case 'C': {
                const FunctionCall& call = bp.calls[static_cast<usize>(v)];
                operands += bp.functions[call.function].name + "(";
                for (usize a = 0; a < call.args.size(); ++a) operands += (a > 0 ? " " : "") + RegName(call.args[a]);
                operands += ")";
                if (!call.results.empty()) {
                    operands += " ->";
                    for (RegRef r : call.results) operands += " " + RegName(r);
                }
                break;
            }
            default: break;
            }
        }
        std::snprintf(line, sizeof(line), "%04zu  %-9s %-28s ; node %u\n", pc, info.name, operands.c_str(),
                      pc < fn.node_of.size() ? fn.node_of[pc] : 0u);
        text += line;
    }
    return text;
}

} // namespace aether::bp
