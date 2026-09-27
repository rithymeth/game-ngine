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

enum class Bank : u8 { Value, String, Array };

// An array register or variable (Phase 12 step 4): values, or strings for
// Array<string>. Elements compare by their bytes (Find, Contains, Add Unique).
struct ArrayValue {
    ValueType type = ValueType::None; // the element type
    bool strings = false;
    std::vector<Reg> values;
    std::vector<std::string> texts;

    usize Size() const { return strings ? texts.size() : values.size(); }
    void Clear() {
        values.clear();
        texts.clear();
    }
};

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
    // Per-instance node state (Do Once, Gate, Do N, Flip Flop): a = state slot.
    StateInit,   // b = bool register: the flag's first value (once per instance)
    StateJmpIf,  // jump to d if the flag equals c
    StateSet,    // flag = c
    StateToggle, // flag = !flag
    StateGet,    // a = dst bool, b = slot
    CountLess,   // jump to d if counter >= r[b] (int), else counter++
    CountGet,    // a = dst int, b = slot
    CountReset,  // counter = 0
    // More math: a = dst, b (, c, d) = operands.
    MathF,    // c = MathFn
    Atan2F,   // b = y, c = x
    PowF,     // b = base, c = exponent
    RoundF,   // a = dst int, b = float, c = RoundMode
    NegI, AbsI,
    ClampI,   // b = value, c = min, d = max
    NearEqF,  // b, c = values, d = tolerance
    RandF,    // b = min, c = max
    RandI,    // b = min, c = max (inclusive)
    RandB,
    // Strings: s registers unless noted.
    ConcatS,   // a = dst, b, c
    LenS,      // a = dst int, b = string
    EmptyS,    // a = dst bool, b = string
    ContainsS, // a = dst bool, b = text, c = substring, d = ignore-case bool register
    StrFn,     // a = dst, b = src, c = StrFn
    ParseI,    // a = dst int, b = string, c = dst success bool
    ParseF,    // a = dst float, b = string, c = dst success bool
    // Arrays: A = array register; items are in the value or string bank to
    // match the array. Var* ops change array variable d in place.
    NewA,      // a = dst, c = element ValueType
    PushA,     // a = array, b = item
    MoveA,     // a = dst, b = src
    GetVarA,   // a = dst, d = slot (a copy)
    SetVarA,   // b = src, d = slot
    LenA,      // a = dst int, b = array
    GetA,      // a = dst item, b = array, c = index (out of range: default, BP205)
    ValidIdxA, // a = dst bool, b = array, c = index
    FindA,     // a = dst int, b = array, c = item (-1 if absent)
    VarAdd,       // a = dst index, b = item
    VarAddUnique, // a = dst index (-1 if already there), b = item
    VarInsert,    // b = item, c = index
    VarRemoveAt,  // c = index
    VarRemoveItem, // a = dst bool, b = item (the first match)
    VarClear,
    VarSetAt,     // b = item, c = index
    VarReverse,
    // Entities and the world (BP201 when the entity is gone or lacks the
    // component). b = entity register unless noted.
    GetLoc,       // a = dst Vec3
    SetLoc,       // c = Vec3
    AddOffset,    // c = Vec3
    GetRot,       // a = dst Quat
    SetRot,       // c = Quat
    WorldLoc,     // a = dst Vec3 (through the parent chain)
    QuatRotate,   // a = dst Vec3, b = Quat, c = Vec3
    QuatAxisAngle, // a = dst Quat, b = axis, c = degrees
    QuatMul,      // a = dst, b, c
    HasTagOp,     // a = dst bool, c = tag string
    AddTagOp,     // c = tag string
    RemoveTagOp,  // c = tag string
    FindTagOp,    // a = dst Array<Entity>, b = tag string
    GetParentOp,  // a = dst entity
    AttachOp,     // c = parent entity
    DetachOp,
    DestroyOp,    // deferred until the running event finishes; EndPlay first
    SpawnOp,      // a = dst entity, b = location, c = rotation, d = spawn index
    GameTime,     // a = dst float
    DeltaTime,    // a = dst float
    // Event dispatchers and interfaces: d = table index.
    CallDispatcher, // runs every Custom Event bound to the target's dispatcher
    BindDispatcher, // Bind / Unbind / Unbind All (DispatcherBind::mode)
    InterfaceCall,  // runs the target's interface event, if it implements it
    ImplementsOp,   // a = dst bool, b = target, d = name index
    SortVar, // d = sort index: sorts an array variable in place
    FilterA, // a = dst array, b = source array, d = sort index (its function)
    // Latent actions (§C.4): a = state slot, b = duration register, c =
    // LatentKind, d = latent index. Starts the action and carries on; its
    // "completed" code runs later from the latent's resume point.
    Latent,
    Ret,
};

enum class LatentKind : u8 { Delay, RetriggerableDelay, NextTick };
enum class MathFn : u8 { Sin, Cos, Tan, Asin, Acos, Atan, Sqrt, Exp, Log, Frac, DegToRad, RadToDeg };
enum class RoundMode : u8 { Floor, Ceil, Round, Truncate };
enum class StrFnKind : u8 { Upper, Lower, Trim };

struct LatentInfo {
    NodeId node = 0;
    u32 function = 0;
    u32 resume_pc = 0; // where its "completed" code starts in the function
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

// Spawn Blueprint: the asset (GUID text) and the Expose on Spawn values.
struct SpawnInfo {
    std::string asset;
    std::vector<std::pair<std::string, TypedReg>> exposed;
};

// Sort (in place, on an array variable) and Filter: the function they call.
struct SortInfo {
    i32 function = -1; // -1: Sort's natural order
    i32 slot = 0;      // Sort: the array variable
};

struct DispatcherCall {
    std::string dispatcher;
    RegRef target;
    std::vector<TypedReg> args;
};

struct DispatcherBind {
    enum class Mode : u8 { Bind, Unbind, UnbindAll } mode = Mode::Bind;
    std::string dispatcher;
    std::string event; // a Custom Event of this Blueprint (the handler)
    RegRef target;     // whose dispatcher
};

struct InterfaceCallInfo {
    std::string key; // "Interactable.Interact"
    RegRef target;
    std::vector<TypedReg> args;
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
    // Debug info (Phase 12 step 5): where each node's code starts, in pc
    // order (several nodes can start at one pc: a Sequence with no code of
    // its own, an impure node and the pure nodes it reads), and the register
    // holding each pin's value, for the debugger's value inspection.
    std::vector<u8> is_entry; // per instruction
    std::vector<std::pair<u32, NodeId>> entries;
    std::map<std::pair<NodeId, std::string>, TypedReg> pin_values;
    u16 value_regs = 0;
    u16 string_regs = 0;
    u16 array_regs = 0;
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
    u16 array_vars = 0;
    std::vector<CompiledFunction> functions;
    std::map<std::string, u32> events;    // event key ("Event.Tick", "Event.Custom:Hit") -> function
    std::map<std::string, u32> function_index; // function graph name -> function
    std::vector<NativeCall> native_calls;
    std::vector<FieldAccess> field_accesses;
    std::vector<FunctionCall> calls;
    std::vector<LatentInfo> latents;
    std::vector<SpawnInfo> spawns;
    std::vector<SortInfo> sorts;
    std::vector<DispatcherCall> dispatcher_calls;
    std::vector<DispatcherBind> dispatcher_binds;
    std::vector<InterfaceCallInfo> interface_calls;
    std::vector<std::string> names;       // interface names for ImplementsOp
    std::vector<std::string> interfaces;  // the interfaces this Blueprint implements
    u16 state_slots = 0; // per-instance node state (stateful flow nodes and latent nodes)

    const CompiledVariable* FindVariable(std::string_view name) const;
    const CompiledFunction* FindEvent(std::string_view key) const;
};

// One instruction per line: "0003  JMPF   r1, @6        ; node 2".
std::string Disassemble(const CompiledBlueprint& blueprint, const CompiledFunction& function);

} // namespace aether::bp
