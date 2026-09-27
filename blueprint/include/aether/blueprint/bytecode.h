#pragma once

#include "aether/blueprint/graph.h"
#include "aether/ecs/entity.h"

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aether::reflect {
struct FunctionInfo;
struct FieldInfo;
struct TypeInfo;
} // namespace aether::reflect

namespace aether::bp {

// Compiled Blueprints (Phase 12 step 2, docs/ROADMAP_DETAILS.md §C): typed
// register bytecode. A frame has two register banks: 16-byte value
// registers (bool, int, float, Vec3, Quat, Entity) and string registers.
// Opcodes are typed (AddF, AddI, AddV...), so the VM never inspects types
// in the hot path.

// A 16-byte register (§C.1).
struct alignas(16) Reg {
    union {
        bool b;
        i32 i;
        f32 f;
        f32 v[4]; // Vec3 in v[0..2]; Quat in v[0..3]
        u32 e[2]; // Entity {index, generation}
    };
    Reg() : v{0.0f, 0.0f, 0.0f, 0.0f} {}

    static Reg Bool(bool x) { Reg r; r.b = x; return r; }
    static Reg Int(i32 x) { Reg r; r.i = x; return r; }
    static Reg Float(f32 x) { Reg r; r.f = x; return r; }
    static Reg Vector(const Vec3& x) { Reg r; r.v[0] = x.x; r.v[1] = x.y; r.v[2] = x.z; return r; }
    static Reg Quat(const Quaternion& q) { Reg r; r.v[0] = q.x; r.v[1] = q.y; r.v[2] = q.z; r.v[3] = q.w; return r; }
    static Reg EntityOf(Entity x) { Reg r; r.e[0] = x.index; r.e[1] = x.generation; return r; }

    Vec3 AsVec3() const { return {v[0], v[1], v[2]}; }
    Quaternion AsQuat() const { return {v[0], v[1], v[2], v[3]}; }
    Entity AsEntity() const { return Entity{e[0], e[1]}; }
};
static_assert(sizeof(Reg) == 16);

enum class Bank : u8 { Value, String };

struct RegRef {
    Bank bank = Bank::Value;
    u16 index = 0;
    bool operator==(const RegRef& o) const { return bank == o.bank && index == o.index; }
};

enum class Op : u8 {
    Nop,
    LoadK,  // a = dst, d = constant
    LoadKS, // a = dst string, d = string constant
    Move,   // a = dst, b = src
    MoveS,
    GetVar,  // a = dst, d = variable slot
    SetVar,  // d = slot, b = src
    GetVarS, // string variables
    SetVarS,
    // Arithmetic: a = dst, b, c = operands.
    AddI, SubI, MulI, DivI, ModI, MinI, MaxI,
    AddF, SubF, MulF, DivF, MinF, MaxF,
    AddV, SubV, MulV,
    ScaleV, // a = dst vector, b = vector, c = float
    // Comparisons into a bool register.
    LtI, LeI, GtI, GeI, EqI, NeI,
    LtF, LeF, GtF, GeF, EqF, NeF,
    EqB, NeB, EqE, NeE,
    EqS, NeS, // b, c string registers
    And, Or, Xor, Not, // Not: a = dst, b = src
    NegF, AbsF,
    ClampF, // a = dst, b = value, c = min, d = max (a register)
    LerpF,  // a = dst, b = from, c = to, d = alpha
    MakeV,  // a = dst, b = x, c = y, d = z
    BreakV, // a = dst float, b = vector, c = component (0-2)
    LenV, DotV, CrossV, NormV,
    IntToFloat, FloatToInt, // a = dst, b = src
    ToString, // a = dst string, b = src value register, c = ValueType
    Self,     // a = dst entity
    IsValid,  // a = dst bool, b = entity
    Jmp,      // d = target instruction
    JmpF,     // b = condition, d = target
    Print,    // b = string register
    CallNative,   // d = native call index
    GetField,     // d = field access index
    SetField,     // d = field access index
    CallFunction, // d = function call index
    CallEvent,    // d = event call index (a custom event on self)
    Ret,
};

struct Instr {
    Op op = Op::Nop;
    u16 a = 0, b = 0, c = 0;
    i32 d = 0;
};

struct TypedReg {
    RegRef reg;
    PinType type;
};

// A call into C++ through reflection (CALLN).
struct NativeCall {
    const reflect::FunctionInfo* function = nullptr;
    const reflect::TypeInfo* owner = nullptr; // the component type (member functions)
    bool has_target = false;
    RegRef target; // the entity whose component is `self`
    std::vector<TypedReg> args;
    bool has_result = false;
    TypedReg result;
};

// A component field read or write (Comp.Get / Comp.Set).
struct FieldAccess {
    const reflect::TypeInfo* owner = nullptr;
    const reflect::FieldInfo* field = nullptr;
    RegRef target;
    TypedReg value;
};

// A call to one of this Blueprint's functions or custom events.
struct FunctionCall {
    u32 function = 0;            // index into CompiledBlueprint::functions
    std::vector<RegRef> args;    // the caller's registers, copied into the callee's params
    std::vector<RegRef> results; // the caller's registers the callee's results are copied into
};

struct CompiledFunction {
    std::string name;  // "Event.BeginPlay", "Event.Custom:Hit", or a function graph's name
    std::string graph; // the graph it came from
    std::vector<Instr> code;
    std::vector<NodeId> node_of; // per instruction: the node that emitted it (for errors and the debugger)
    u16 value_regs = 0;
    u16 string_regs = 0;
    std::vector<RegRef> params;  // where arguments land
    std::vector<PinType> param_types;
    std::vector<RegRef> results; // function outputs
    std::vector<PinType> result_types;
};

struct CompiledVariable {
    std::string name;
    PinType type;
    RegRef slot; // into the instance's variable banks
    Reg default_value;
    std::string default_string;
    u32 flags = 0;
};

struct CompiledBlueprint {
    std::vector<Reg> constants;
    std::vector<std::string> string_constants;
    std::vector<CompiledVariable> variables;
    u16 value_vars = 0;
    u16 string_vars = 0;
    std::vector<CompiledFunction> functions;
    std::map<std::string, u32> events;    // event key ("Event.Tick", "Event.Custom:Hit") -> function
    std::map<std::string, u32> function_index; // function graph name -> function
    std::vector<NativeCall> native_calls;
    std::vector<FieldAccess> field_accesses;
    std::vector<FunctionCall> calls;

    const CompiledVariable* FindVariable(std::string_view name) const;
    const CompiledFunction* FindEvent(std::string_view key) const;
};

// One instruction per line: "0003  JMPF   r1, @6        ; node 2".
std::string Disassemble(const CompiledBlueprint& blueprint, const CompiledFunction& function);

} // namespace aether::bp
