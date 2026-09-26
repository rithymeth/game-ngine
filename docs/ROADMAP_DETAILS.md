# Aether Roadmap: detailed specs and appendices

Companion to [`ROADMAP.md`](ROADMAP.md). The roadmap says **what** to build
and in what order; this document goes deeper on **how**:

- **A.** On-disk file formats, with examples
- **B.** Phase 6 (reflection) build spec, ready to implement
- **C.** Blueprint VM: opcodes, frames, a worked compile example
- **D.** Writing custom Blueprint nodes in C++
- **E.** More editor UI mockups
- **F.** Keyboard shortcut map
- **G.** Effort estimates and team shape
- **H.** Risk register
- **I.** Definition of done / PR checklist
- **J.** Glossary

---

## A. File formats

Every text format is JSON, UTF-8, with 2-space indentation and **sorted
keys**, so saving an unchanged asset produces a byte-identical file and git
diffs stay small. Every file starts with a `"$type"` and `"$version"`.

### A.1 Project file: `MyGame.aproject`

```json
{
  "$type": "Project",
  "$version": 1,
  "name": "MyGame",
  "engine_version": "0.6.0",
  "startup_scene": "a3f1c2d4-0000-4000-8000-00000000c0de",
  "modules": [
    { "name": "MyGame", "type": "runtime", "path": "Source/MyGame" }
  ],
  "plugins": ["Aether.Physics", "Aether.Audio"],
  "settings": {
    "physics": { "fixed_timestep_hz": 60, "gravity": [0, -9.81, 0] },
    "input":   { "default_contexts": ["b71e...-imc-default"] },
    "layers":  ["Default", "Player", "Enemy", "Pickup", "UI"]
  }
}
```

Project folder layout:

```
MyGame/
  MyGame.aproject
  Content/          assets + .ameta sidecars (commit to git, LFS for binaries)
  Source/MyGame/    C++ game module (optional)
  Config/           per-platform settings overrides
  Saved/            layout, logs, autosaves (gitignored)
  Intermediate/     DDC, build files (gitignored)
```

### A.2 Asset sidecar: `T_Brick.png.ameta`

```json
{
  "$type": "AssetMeta",
  "$version": 1,
  "guid": "5c0a9e2e-7b1d-4c3a-9f10-2a6b8e4d1f77",
  "importer": "Texture",
  "importer_version": 3,
  "source_hash": "xxh3:9a1b2c3d4e5f6071",
  "settings": {
    "srgb": true,
    "compression": "BC7",
    "generate_mips": true,
    "max_size": 2048,
    "address_mode": "Wrap"
  },
  "labels": ["Environment", "Brick"]
}
```

The DDC key is `hash(source_hash, settings, importer_version, target platform)`.

### A.3 Scene: `Level_01.ascene`

```json
{
  "$type": "Scene",
  "$version": 1,
  "entities": [
    {
      "guid": "e1d2...-01",
      "name": "Sun",
      "components": {
        "Transform":        { "$v": 1, "position": [0, 10, 0], "rotation": [-0.38, 0, 0, 0.92], "scale": [1, 1, 1] },
        "DirectionalLight": { "$v": 1, "color": [1, 0.96, 0.9], "intensity": 5.0, "cast_shadows": true }
      }
    },
    {
      "guid": "e1d2...-02",
      "name": "Door_A",
      "prefab": {
        "source": "7a0f...-bp-door",
        "overrides": [
          { "entity": "root", "component": "Transform", "path": "position", "value": [4, 0, 2] },
          { "entity": "root", "component": "BP_Door",   "path": "open_speed", "value": 2.5 }
        ]
      }
    }
  ]
}
```

- Components are keyed by **reflected name** (see B.1), not `typeid`.
- `$v` is the component's schema version, so each component migrates
  independently.
- A prefab instance stores **only overrides**; everything else comes from
  the source asset.

### A.4 Blueprint: `BP_Door.abp` (excerpt)

```json
{
  "$type": "Blueprint",
  "$version": 1,
  "parent": "native:Entity",
  "variables": [
    { "name": "IsOpen",     "type": "bool",  "default": false, "flags": ["InstanceEditable"] },
    { "name": "open_speed", "type": "float", "default": 1.0,   "flags": ["InstanceEditable"], "meta": { "range": [0.1, 10] } }
  ],
  "graphs": [
    {
      "name": "EventGraph",
      "kind": "EventGraph",
      "nodes": [
        { "id": 1, "type": "Event.OnTriggerEnter", "pos": [0, 0] },
        { "id": 2, "type": "Flow.Branch",          "pos": [260, 0] },
        { "id": 3, "type": "Var.Get:IsOpen",       "pos": [120, 90] },
        { "id": 4, "type": "Call.Self:Open",       "pos": [520, 0] }
      ],
      "links": [
        { "from": [1, "then"],  "to": [2, "exec"] },
        { "from": [3, "value"], "to": [2, "condition"] },
        { "from": [2, "false"], "to": [4, "exec"] }
      ]
    }
  ]
}
```

Node types are stable string IDs (`Flow.Branch`, `Call.Native:Health.ApplyDamage`),
so renaming a C++ function needs a redirect entry (`CoreRedirects.json`)
rather than breaking every Blueprint that calls it.

### A.5 Binary (cooked) format

Cooked assets use the same reflection walk with a binary archive:
`magic "AETH"`, `u16 format version`, `u64 TypeId`, `u16 schema version`,
then fields in declaration order with no names (the schema is known at
cook time). Stored in `.apak` files: a table of contents (GUID → offset,
size, compression) followed by zstd-compressed blocks.

---

## B. Phase 6 build spec (reflection)

This is the first phase to implement, so it's specified down to files,
types and tests.

### B.1 Files

```
engine/include/aether/reflection/
  type_id.h          TypeId (u64 FNV-1a/xxh3 of declared name), TypeIdOf<T>()
  type_info.h        TypeKind, TypeInfo, FieldInfo, FunctionInfo, EventInfo, EnumInfo, Meta
  registry.h         TypeRegistry (Find by name / TypeId, ForEachType)
  any.h              Any (32-byte small buffer, heap fallback), AnyRef
  reflect_macros.h   AETHER_REFLECT / AETHER_FIELD / AETHER_ENUM / AETHER_FUNCTION
  archive.h          ArchiveWriter / ArchiveReader interfaces
  json_archive.h     JsonArchiveWriter / JsonArchiveReader (nlohmann/json, already a dependency)
  binary_archive.h   BinaryArchiveWriter / BinaryArchiveReader
  serialize.h        SerializeObject / DeserializeObject, migration hooks
engine/src/reflection/
  registry.cpp  any.cpp  json_archive.cpp  binary_archive.cpp  serialize.cpp  builtin_types.cpp
tests/
  test_reflection.cpp  test_archive.cpp
```

### B.2 Core types

```cpp
namespace aether::reflect {

enum class TypeKind : u8 { Void, Bool, Int, UInt, Float, String, Enum, Struct, Array, AssetRef, Entity };

struct Meta {                                 // optional, all editor-facing
    const char* tooltip  = nullptr;
    const char* category = nullptr;
    f64 range_min = 0, range_max = 0;         // range_min == range_max means "no range"
    bool is_color = false;
    const char* units = nullptr;              // "m", "deg", "s"
};

enum FieldFlags : u32 {
    Field_None = 0, Field_EditAnywhere = 1 << 0, Field_ReadOnly = 1 << 1,
    Field_Transient = 1 << 2, Field_BlueprintReadWrite = 1 << 3, Field_Replicated = 1 << 4,
};

struct FieldInfo {
    const char* name;
    TypeId      type;
    u32         offset;
    u32         flags;
    Meta        meta;
};

struct FunctionInfo {
    const char* name;
    TypeId      return_type;
    std::vector<std::pair<const char*, TypeId>> params;
    u32 flags;                                // Fn_BlueprintCallable, Fn_Pure, Fn_Static, Fn_Latent
    bool (*invoke)(void* self, std::span<Any> args, Any* ret);
};

struct TypeInfo {
    const char* name;                         // declared name: "Transform", "RigidBody"
    TypeId      id;
    TypeKind    kind;
    u32 size, alignment;
    u16 version = 1;
    const TypeInfo* base = nullptr;
    const TypeInfo* element = nullptr;        // Array / AssetRef element type
    std::vector<FieldInfo>    fields;
    std::vector<FunctionInfo> functions;
    std::vector<std::pair<const char*, i64>> enum_values;
    // lifecycle, reused by ComponentInfo so there's one source of truth
    void (*construct)(void*);
    void (*destruct)(void*);
    void (*copy)(void* dst, const void* src);
    void (*move)(void* dst, void* src);
    // schema migration: called when a file's version < this->version
    void (*migrate)(u16 from_version, JsonValue& data) = nullptr;
};

template <typename T> const TypeInfo& Reflect();   // specialized by AETHER_REFLECT

} // namespace aether::reflect
```

### B.3 Macro shape

```cpp
// In a header, next to the struct:
AETHER_REFLECT(Transform, 1 /* version */,
    AETHER_FIELD(position, Field_EditAnywhere, { .units = "m" }),
    AETHER_FIELD(rotation, Field_EditAnywhere),
    AETHER_FIELD(scale,    Field_EditAnywhere)
)
```

`AETHER_REFLECT` expands to a specialization of `Reflect<T>()` that builds
a function-local `static TypeInfo` (thread-safe init), plus a static
registrar object in the including `.cpp` so `TypeRegistry::Find("Transform")`
works before anyone calls `Reflect<Transform>()`. `AETHER_FIELD` uses
`offsetof` and `TypeIdOf<decltype(T::field)>()`. The macros need C++20
designated initializers (`{ .units = "m" }`), which the project already
builds with.

A clang-based `tools/aether-reflect` code generator can replace the macros
later with no data-format change, because the generated `TypeInfo` is the
same.

### B.4 Integration with the existing ECS

The goal is to change the ECS as little as possible:

1. `GetComponentId<T>()` (`engine/include/aether/ecs/component.h`) keeps
   its current role. If `T` is reflected, it sets `info.name =
   Reflect<T>().name` instead of `typeid(T).name()`, and the default
   serialize/deserialize become reflection-driven. Unreflected components
   keep today's raw-byte behavior, so nothing breaks.
2. `SetComponentSerializer` stays as an override for special cases.
   `RigidBody` keeps using it until Phase 13 gives it a reflection-based
   custom field handler.
3. `kMaxComponentTypes` goes from 64 to 256. `ComponentMask` is a
   `std::bitset`, so this only makes each mask 32 bytes instead of 8.
4. `SaveScene`/`LoadScene` keep the binary format as version N+1 (with
   reflected payloads), and gain `SaveSceneJson`/`LoadSceneJson`. The
   loader still reads the current binary version, so existing `.aesc`
   files keep loading.

### B.5 Tests (using the existing `AETHER_TEST` / `AETHER_CHECK` framework)

| Test | Checks |
|---|---|
| `Reflection_FieldsEnumerateInDeclarationOrder` | names, offsets, types for a 5-field struct |
| `Reflection_GetSetThroughAny` | `FieldInfo::Get`/`Set` round-trip for bool, i32, f32, Vec3, string, enum |
| `Reflection_InvokeFunction` | `FunctionInfo::invoke` calls a real method and returns a value |
| `Reflection_RegistryFindByName` | lookup works before `Reflect<T>()` is called directly |
| `Archive_JsonRoundTrip_AllKinds` | every `TypeKind`, nested struct, array of structs |
| `Archive_BinaryRoundTrip_AllKinds` | same as above, binary |
| `Archive_AddedFieldGetsDefault` | load a file missing a new field, and it keeps its default value |
| `Archive_RemovedFieldIgnored` | a file with an extra unknown field loads without error |
| `Archive_MigrationHookRuns` | v1 file → v2 type calls `migrate(1, data)` |
| `Serialization_LegacyBinarySceneStillLoads` | a scene saved by today's code loads after the change (checked-in fixture) |
| `Serialization_NamesStableAcrossCompilers` | the scene contains `"Transform"`, not a mangled `typeid` name |

### B.6 PR breakdown

1. `TypeId`, `TypeInfo`, `TypeRegistry`, macros, and builtin types
   (bool/ints/floats/string/Vec3/Vec4/Quaternion/Mat4). Tests 1–4.
2. `Any` and function invocation thunks.
3. JSON and binary archives with `SerializeObject`/`DeserializeObject`.
   Archive tests.
4. ECS integration: reflected names, reflection-driven default
   serializers, the 256-component cap, legacy scene fixture test.
5. Reflect `Transform`, `Parent`, `ModelRenderer`, `RigidBody` (fields
   only), and `EditorCamera`.
6. Editor: generic reflected Inspector and "Add Component". Remove the
   hand-written inspector code.

---

## C. Blueprint VM

### C.1 Execution model

- **Register-based** bytecode. Each function has a fixed frame of typed
  registers (locals, temporaries and parameters) sized at compile time, so
  no allocation happens during a call.
- Registers are 16 bytes each (enough for a `Vec3` or `Quaternion` without
  boxing). Larger structs and strings live in a per-frame side arena and
  the register holds a pointer.
- One `ScriptFrame` stack per `BlueprintInstance` event dispatch; latent
  nodes save the frame to the heap and resume later (see C.4).

### C.2 Opcode table

| Opcode | Operands | Effect |
|---|---|---|
| `NOP` | | |
| `LOADK` | `dst, kidx` | `r[dst] = constants[kidx]` |
| `MOVE` | `dst, src` | `r[dst] = r[src]` |
| `GETSELF` | `dst, field` | read a field of the instance's own component/variables |
| `SETSELF` | `field, src` | write a field (fires change notifications if the field is replicated or bound in UI) |
| `GETFIELD` | `dst, obj, type, field` | read a field of another entity's component |
| `SETFIELD` | `obj, type, field, src` | write it |
| `ADD/SUB/MUL/DIV/MOD` `_I` `_F` `_V3` | `dst, a, b` | typed arithmetic (no dynamic dispatch) |
| `CMP_EQ/NE/LT/LE` `_I` `_F` | `dst, a, b` | comparisons into a bool register |
| `NOT/AND/OR` | `dst, a, b` | boolean logic |
| `CONV` | `dst, src, conv_id` | int↔float, float→string, and other implicit conversions |
| `JMP` | `offset` | unconditional jump |
| `JMPF` | `cond, offset` | jump if false (Branch) |
| `SWITCH` | `src, table_idx` | jump table (Switch on Int/Enum) |
| `CALLN` | `fn_idx, argbase, argc, dst` | call a native `FunctionInfo` |
| `CALLB` | `fn_idx, argbase, argc, dst` | call a Blueprint function (new frame) |
| `CALLI` | `iface_fn, target, argbase, argc, dst` | interface call; no-op if the target doesn't implement it |
| `CAST` | `dst, src, type` | Cast To; writes null on failure, followed by a `JMPF` for the failed pin |
| `ISVALID` | `dst, src` | entity handle still alive |
| `ARR_*` | varies | array get/set/add/remove/length |
| `LATENT` | `node_id, argbase, resume_offset` | start a latent action and suspend the frame |
| `BREAK` | `node_id` | debugger breakpoint (only emitted in debug compiles) |
| `TRACE` | `node_id` | exec-wire animation hook (editor builds only) |
| `RET` | `src` | return from function |

### C.3 Worked example: the `BP_Door` graph from A.4

Graph: `OnTriggerEnter → Branch(IsOpen) → false → Open()`.

```
; function Event_OnTriggerEnter(other: Entity)     frame: r0=other, r1=tmp_bool
0000  TRACE    1                 ; node 1 fired
0001  GETSELF  r1, IsOpen        ; pure node 3, evaluated when node 2 needs it
0002  TRACE    2
0003  JMPF     r1, +2            ; Branch: false pin -> 0006
0004  RET                        ; true pin not connected
0005  NOP
0006  TRACE    4
0007  CALLB    fn:Open, r0, 0, -
0008  RET
```

Pure nodes (`Var.Get:IsOpen`) have no exec pins. The compiler emits them
right before the first impure node that reads their output, and caches the
result for the rest of that exec step.

### C.4 Latent actions (Delay, Timeline, MoveTo)

```cpp
struct LatentAction {
    EntityGuid  owner;
    u32         function;        // which compiled function
    u32         resume_pc;       // instruction to continue from
    std::vector<u8> saved_frame; // copied registers and arena
    u32         node_id;         // for the debugger and "Retriggerable" logic
    f64         wake_time;       // Delay; other latent kinds have their own state
};
```

- `LATENT` pushes a `LatentAction` into `LatentActionManager` and returns
  from the dispatch. `BlueprintVM::Tick` checks due actions each frame and
  resumes them by restoring the frame and jumping to `resume_pc`.
- If the owner entity is destroyed, its pending actions are dropped.
- `Do Once`, `Gate` and `Retriggerable Delay` check for an existing action
  with the same `(owner, node_id)` before adding a new one.

### C.5 Performance targets and how to reach them

| Target | Technique |
|---|---|
| 10,000 instances × 20-node tick under 2 ms | typed opcodes (no `Any` in the hot path); instances grouped by class; one dispatch loop per class |
| Native call overhead under 20 ns | `CALLN` uses a direct typed thunk generated per `FunctionInfo` signature (templates), `Any` only as a fallback |
| No per-frame allocation | fixed register frames; arena reset per dispatch |
| Fast compile (under 100 ms for a 500-node graph) | incremental: only recompile graphs whose hash changed |

---

## D. Writing custom Blueprint nodes in C++

Most nodes come from reflection automatically. For a node with special
behavior (variable pin count, custom compile, custom UI), implement
`INodeType`:

```cpp
class FormatTextNode final : public bp::INodeType {
public:
    const char* Id() const override       { return "Text.Format"; }
    const char* Title() const override    { return "Format Text"; }
    const char* Category() const override { return "String"; }

    // Pins can depend on node config: one input pin per {name} in the format string.
    void BuildPins(const bp::NodeConfig& cfg, bp::PinBuilder& pins) const override {
        pins.Input("format", bp::Type::String());
        for (auto& arg : ParseFormatArgs(cfg.GetString("format")))
            pins.Input(arg, bp::Type::Wildcard());     // resolved when connected
        pins.Output("result", bp::Type::String());
    }

    // Lower to IR; the compiler turns IR into bytecode.
    void Compile(bp::NodeCompileContext& ctx) const override {
        auto args = ctx.InputsAsStrings(1);            // auto-inserts CONV for non-strings
        ctx.EmitNativeCall("Text.FormatImpl", ctx.Input("format"), args, ctx.Output("result"));
    }

    // Optional custom body drawn inside the node in the editor.
    void DrawBody(bp::NodeDrawContext& ui) const override;
};

AETHER_REGISTER_NODE(FormatTextNode);
```

Game modules register nodes the same way, so plugins can add whole node
libraries.

---

## E. More editor UI mockups

### E.1 Project browser (first thing the editor shows)

```
┌ Aether ────────────────────────────────────────────────────────────────────┐
│  AETHER                                                        v0.6.0      │
├───────────────────┬────────────────────────────────────────────────────────┤
│ ▸ Recent Projects │  RECENT                                     🔍 search  │
│   New Project     │  ┌────────┐ ┌────────┐ ┌────────┐                      │
│   Open…           │  │ [img]  │ │ [img]  │ │ [img]  │                      │
│   Learn           │  │ MyGame │ │ Proto2 │ │ Demo   │                      │
│   Settings        │  │ 2h ago │ │ 3d ago │ │ 1w ago │                      │
│                   │  └────────┘ └────────┘ └────────┘                      │
│                   │  NEW PROJECT FROM TEMPLATE                             │
│                   │  [Blank] [First Person] [Third Person] [2D Platformer] │
│                   │  [Top Down] [Vehicle] [VR]                             │
│                   │  Name: [MyNewGame      ]  Location: [D:\Games\ ] [📁]  │
│                   │  Starter content ☑   Scripting: (•) Blueprint ( ) C++  │
│                   │                                       [Create Project] │
└───────────────────┴────────────────────────────────────────────────────────┘
```

### E.2 Project Settings

```
┌ Project Settings ──────────────────────────────────────────────────────────┐
│ 🔍 search all settings                                                     │
├──────────────────┬─────────────────────────────────────────────────────────┤
│ ▾ Project        │ PHYSICS                                                 │
│   Description    │   Fixed timestep (Hz)    [ 60      ]                    │
│   Maps & Modes   │   Gravity                [ 0 ][ -9.81 ][ 0 ]            │
│   Packaging      │   Default friction       ━━━●━━━━━ 0.6                  │
│ ▾ Engine         │ COLLISION MATRIX                                        │
│   Input          │            Def  Ply  Enm  Pck  UI                       │
│   Physics  ◄     │   Default  ☑    ☑    ☑    ☑    ☐                        │
│   Rendering      │   Player        ☐    ☑    ☑    ☐                        │
│   Audio          │   Enemy              ☐    ☐    ☐                        │
│   Localization   │   Pickup                  ☐    ☐                        │
│ ▾ Platforms      │   UI                           ☐                        │
│   Windows        │                                                         │
│   Linux          │ [Reset section to defaults]      Saved to Config/ ✓     │
│ ▾ Plugins        │                                                         │
└──────────────────┴─────────────────────────────────────────────────────────┘
```

Settings pages are just reflected structs drawn by the generic Inspector,
so a plugin adds its own page by registering a settings type.

### E.3 Animation state machine editor

```
┌ ABP_Hero › Locomotion (State Machine) ────────────────────────────────────┐
│ [Compile ✓] [💾]  Preview: [Hero ▾]  Speed ━━━●━━ 320   IsFalling ☐      │
├────────────────┬──────────────────────────────────────────┬───────────────┤
│  PREVIEW       │        ┌───────┐                          │ TRANSITION    │
│  (character    │  ●───▶ │ Idle  │ ───Speed>10───▶ ┌──────┐ │ Idle → Walk   │
│   animating)   │        └───────┘ ◀──Speed<5──── │ Walk │ │ Rule:         │
│                │            │                     └──────┘ │  Speed > 10   │
│ VARIABLES      │     IsFalling                       │    │ Blend: 0.2 s  │
│  Speed  float  │            ▼                  Speed>300  │ Curve: Ease ▾ │
│  IsFalling bool│        ┌────────┐                   ▼    │               │
│                │        │ InAir  │             ┌──────┐   │               │
│                │        └────────┘             │ Run  │   │               │
│                │  (active state glows during PIE)└─────┘   │               │
└────────────────┴──────────────────────────────────────────┴───────────────┘
```

### E.4 Behavior tree editor

```
┌ BT_Guard ─────────────────────────────────────────────────────────────────┐
│ [💾] Blackboard: [BB_Guard ▾]   Debug: [Guard_2 ▾]  ⏸                     │
├──────────────────────────────────────────────────────┬────────────────────┤
│                     ┌────────┐                       │ BLACKBOARD         │
│                     │  ROOT  │                       │  TargetActor Entity│
│                     └───┬────┘                       │  PatrolIndex int   │
│                   ┌─────┴──────┐                     │  LastSeen   Vec3   │
│                   │ ? Selector │                     │                    │
│                   └─┬───────┬──┘                     │ DETAILS            │
│      ┌──────────────┘       └────────────┐           │ Decorator:         │
│ ┌────┴──────────────┐          ┌─────────┴───────┐   │  Blackboard        │
│ │◇ Has TargetActor  │          │ → Sequence      │   │  Key: TargetActor  │
│ │ → Sequence        │          │  Patrol         │   │  Is Set            │
│ │   MoveTo Target   │          │   MoveTo Point  │   │  Abort: Both ▾     │
│ │   Attack          │          │   Wait 2s       │   │                    │
│ └───────────────────┘          └─────────────────┘   │                    │
│  (running branch outlined green, failed red, during PIE)                  │
└──────────────────────────────────────────────────────┴────────────────────┘
```

### E.5 Profiler

```
┌ Profiler ─────────────────────────────────────────────────────────────────┐
│ ● Live  [⏸ Capture] [💾 Save .trace]  Target: Editor PIE ▾   Frame 18234  │
├───────────────────────────────────────────────────────────────────────────┤
│ Frame time (ms)  16.6 ┤▁▂▁▁▂▁▁▃█▂▁▁▁▂▁▁▁▂▁▁  (click a spike to inspect)    │
├───────────────────────────────────────────────────────────────────────────┤
│ Main    |■■ Input|■■■■■■ Gameplay ■■■■■■|■■■ BP Tick ■■■|■■ Physics sync  │
│ Worker1 |   ■■■■■■■■ Jolt Step ■■■■■■■■   |■■ Anim eval ■■|                │
│ Worker2 |   ■■■■■■ Jolt Step ■■■■■■       |■■■ Anim ■■■■■■|                │
│ Render  |■■ Extract ■■|■■■■■■■■ Record passes ■■■■■■■■|■ Submit           │
│ GPU     |■ Depth|■■ Shadows ■■|■■■■■ Opaque ■■■■■|■ Post|■ UI|            │
├──────────────────────────────┬────────────────────────────────────────────┤
│ TOP ZONES (self ms)          │ COUNTERS                                   │
│ BlueprintVM::Tick     2.10   │ Draw calls 1,204   Triangles 2.1M          │
│ Physics::Step         1.80   │ Entities 12,480    BP instances 3,112      │
│ Renderer::Shadows     1.40   │ Memory: Textures 812 MB  Meshes 210 MB     │
└──────────────────────────────┴────────────────────────────────────────────┘
```

### E.6 Package window

```
┌ Package Project ──────────────────────────────────────────────────────────┐
│ Platform      (•) Windows  ( ) Linux  ( ) Android                         │
│ Configuration [ Shipping ▾ ]   Architecture [ x64 ▾ ]                     │
│ Maps          ☑ MainMenu   ☑ Level_01   ☑ Level_02   ☐ TestMap            │
│ Output        [ D:\Builds\MyGame ]  [📁]                                  │
│ Icon [🖼 choose]  Splash [🖼 choose]  Version [ 1.0.0 ]                    │
│ Compression   ☑ zstd    Encrypt paks ☐     Include debug symbols ☐        │
├───────────────────────────────────────────────────────────────────────────┤
│ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░  Cooking textures… 812 / 1,340                   │
│ [log] Cooked M_Rock (0.12 s)                                              │
│ [log] Compiled 214 shader permutations (DXIL)                             │
│                                   [Cancel]   [Package]   [Package & Run]  │
└───────────────────────────────────────────────────────────────────────────┘
```

### E.7 Viewport overlays and context menus

- **Top-left:** view mode (Lit ▾), projection (Perspective ▾ / Top / Front
  / Side), show flags (Collision, Navigation, Bounds, Lights, Audio, Grid).
- **Top-right:** camera speed slider, snapping toggles, and a screen
  percentage option (render at lower resolution for speed).
- **Bottom-left:** stats (FPS, frame ms, draw calls) when `stat` is on.
- **Right-click in viewport:** Place Actor ▸ (recent/common assets), Snap to
  Floor (End), Align to Surface, Group (Ctrl+G), Create Prefab, Focus (F),
  Select All of Same Type.
- **Drop preview:** dragging an asset shows a translucent ghost that snaps
  to surfaces before release.

---

## F. Keyboard shortcut map (defaults, all rebindable)

| Context | Shortcut | Action |
|---|---|---|
| Global | Ctrl+S / Ctrl+Shift+S | Save / Save All |
| Global | Ctrl+Z / Ctrl+Y | Undo / Redo |
| Global | Ctrl+P | Command palette |
| Global | Ctrl+Space | Toggle Content Browser drawer |
| Global | Alt+P / Esc / Shift+F1 | Play / Stop / Release mouse during PIE |
| Global | F11 | Maximize panel under the cursor |
| Viewport | W / E / R | Translate / Rotate / Scale |
| Viewport | Q | Select tool (no gizmo) |
| Viewport | Space | Cycle gizmo mode |
| Viewport | ` (backtick) | Toggle local / world space |
| Viewport | F | Frame selection |
| Viewport | End | Snap selection to floor |
| Viewport | Ctrl+D / Del | Duplicate / Delete |
| Viewport | H / Alt+H | Hide selected / unhide all |
| Viewport | RMB + WASD/QE, wheel | Fly camera, change speed |
| Viewport | Alt+LMB / Alt+RMB / MMB | Orbit / dolly / pan |
| Viewport | G | Game view (hide editor icons and gizmos) |
| Hierarchy | F2 | Rename |
| Hierarchy | Ctrl+G | Group into empty parent |
| Graphs | Tab / RMB | Node palette |
| Graphs | C | Comment around selection |
| Graphs | Q | Straighten links |
| Graphs | F9 | Toggle breakpoint |
| Graphs | F10 / F11 / Shift+F11 | Step over / into / out (while paused) |
| Graphs | Ctrl+F / Ctrl+Shift+F | Find in graph / in all Blueprints |
| Graphs | B/S/D + LMB | Place Branch / Sequence / Delay |
| Graphs | Ctrl+1–9 / Shift+1–9 | Jump to / set bookmark |
| Sequencer | Space / K / J / L | Play / pause / step back / step forward |
| Sequencer | S | Add key on selected track |

---

## G. Effort estimates and team shape

Rough sizes in **engineer-weeks** for one experienced engine programmer
working on this codebase. Treat them as relative sizes for planning, not
promises.

| Phase | Size | Est. (eng-weeks) | Can run in parallel with |
|---|---|---|---|
| 6 Reflection | M | 3–4 | — (everything waits on it) |
| 7 Editor foundation | L | 6–8 | 8 (after the reflection core) |
| 8 Asset system v2 | L | 6–8 | 7 |
| 9 Prefabs + gameplay framework | L | 5–7 | 10 |
| 10 Input | S | 2 | 9, 11 |
| 11 Scripting (Luau + native reload) | L | 6–8 | 12 graph widget |
| 12 Blueprints | XL | 12–16 | 13 |
| 13 Physics v2 | M | 4–5 | 12 |
| 14 Unified renderer | XL | 10–14 | 15–17 |
| 15 Materials | L | 5–6 | 16, 17 |
| 16 Animation | L | 8–10 | 15, 17 |
| 17 Audio | M | 3–4 | anything |
| 18 Runtime UI | L | 6–8 | 19 |
| 19 VFX | L | 5–7 | 18 |
| 20 AI | L | 5–6 | 21 |
| 21 World building | L | 6–8 | 20 |
| 22 Networking | XL | 10–14 | 23 |
| 23 Tooling | M | 3–4 | anything |
| 24 Cross-platform | L | 5–7 | 25 |
| 25 Cook + package | L | 5–6 | 24 |
| 26 Ecosystem | L | 6–8 | — |
| 27–36 | varies | 3–16 each | mostly independent |

Totals: **M1–M5 is roughly 120–160 engineer-weeks.** That's about 2.5–3
years for one person, or about a year for a team of three:

- **Engine/runtime:** reflection, ECS, scripting VM, physics, networking
- **Rendering:** renderer, materials, VFX, advanced rendering
- **Tools/editor:** editor framework, graph editors, content browser,
  UI designer

The critical path is **6 → 7 → 9 → 12**. Keep it moving before widening
into rendering and content systems.

---

## H. Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Reflection macros become painful as types grow | Medium | Medium | Keep the data model generator-friendly so `aether-reflect` (clang tooling) can replace macros without format changes |
| Blueprint VM too slow for heavy tick logic | Medium | High | Typed opcodes and typed native thunks from day one; benchmark in CI; nativization (C++ codegen) as a later escape hatch |
| Hot reload of native DLLs corrupts state | High | Medium | Only restore via reflection-serialized state (never raw pointers); disable when a type's layout changed and tell the user to restart PIE |
| Editor `main.cpp` split stalls feature work | Medium | Medium | Split incrementally, one panel per PR, with screenshot tests before and after |
| ImGui docking branch churn | Low | Medium | Pin a specific docking commit; upgrade deliberately |
| Render graph port to RHI breaks D3D12 features | Medium | High | Keep the D3D12-only path working until pixel parity is shown on both backends |
| Vulkan without validation layers hides bugs | High | High | Install the Vulkan SDK in CI (Linux has it easily); run validation layers in nightly tests |
| Asset GUID migration breaks existing scenes | Medium | Medium | One-time migration tool that generates `.ameta` for existing assets and rewrites path refs to GUIDs |
| Networking retrofitted too late | Medium | High | Put the `Replicated` flag in reflection from Phase 6, and avoid designs (global singletons, non-deterministic IDs) that block replication |
| Scope creep (M6 before M2 is solid) | High | High | Don't start M6 phases until the M2 sample game ships; the milestone table is the gate |
| Third-party license conflicts | Low | High | Permissive-only list in ROADMAP §3; review every new dependency in its PR |

---

## I. Definition of done / PR checklist

Every roadmap PR should meet this before merging:

- [ ] Builds on Windows (MSVC), and on Linux once Phase 24 lands.
- [ ] Unit tests added or updated in `tests/`, all passing.
- [ ] For UI or rendering changes: a headless screenshot run
      (`AETHER_EDITOR_MAX_FRAMES` + `AETHER_EDITOR_SCREENSHOT`), with the
      image attached to the PR.
- [ ] Any new serialized type or field is reflected and versioned, with
      an old-file load test when a schema changes.
- [ ] New editor actions go through the command stack (undoable) and are
      registered with a name in the command palette.
- [ ] No new third-party dependency unless it's permissively licensed and
      pinned (`GIT_TAG` or hash).
- [ ] README updated with a short section on what was built and **how it
      was verified**, following the existing style.
- [ ] Roadmap step ticked off (or updated if the plan changed).

---

## J. Glossary

| Term | Meaning in Aether |
|---|---|
| **Archetype** | ECS storage for all entities with the same set of components |
| **Asset GUID** | Permanent 128-bit ID of an asset; survives renames and moves |
| **Blueprint** | Visual-script asset defining an entity template plus node-graph logic |
| **Cook** | Convert editor assets into optimized, platform-specific runtime data |
| **DDC** | Derived Data Cache: stored results of expensive imports/compiles |
| **Exec pin** | White ▶ pin that carries control flow between Blueprint nodes |
| **Latent node** | A node that finishes later (Delay, MoveTo); the graph resumes when it completes |
| **LocText** | A localizable string (key plus source text) |
| **PIE** | Play-in-Editor: run the game inside the editor on a copy of the level |
| **Prefab** | Reusable entity subtree asset; instances store only overrides |
| **Pure node** | A Blueprint node without exec pins; computed on demand |
| **Reflection** | Runtime description of types, fields, and functions (Phase 6) |
| **RHI** | Rendering Hardware Interface: the D3D12/Vulkan abstraction layer |
| **Render graph** | Declares passes and resource usage; inserts barriers and aliases memory |
| **Replication** | Sending entity state from server to clients |
| **Transient resource** | GPU resource owned by the render graph for one frame |
