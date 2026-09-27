# Aether Blueprints: node reference (v1 library)

This is the full list of nodes planned for the first Blueprint release
([ROADMAP.md Phase 12](../ROADMAP.md)), with each node's pins and exact
behavior. It's the spec the node implementations and their tests are
written against.

**Notation**

- `▶ name` is an exec (control flow) pin. `● name: Type` is a data pin.
- **In** and **Out** list pins top to bottom as they appear on the node.
- A node with no exec pins is **pure**: it runs when something reads its
  output, once per exec step (see
  [ROADMAP_DETAILS.md §C.3](../ROADMAP_DETAILS.md)).
- *Latent* nodes finish later; their `▶ Completed` (or similar) output
  fires on a later frame.
- Types: `bool`, `int` (i32), `float` (f32), `string`, `Text` (LocText),
  `Name` (hashed string), `Vec2`, `Vec3`, `Quat`, `Rotator` (pitch/yaw/roll
  in degrees, the editor-friendly form of a rotation), `Transform`,
  `Color`, `Entity`, `Component<T>`, `Asset<T>`, `Array<T>`, `Map<K,V>`,
  `Wildcard` (takes on the type of whatever is connected first).

**Pin colors and shapes** (shape so color isn't the only signal):

| Type | Color | Shape |
|---|---|---|
| exec | white | ▶ triangle |
| bool | `#8C1A1A` maroon | ● circle |
| int | `#1FB39C` teal | ● |
| float | `#9ADB47` green | ● |
| string / Text / Name | `#F04BC8` magenta / `#E37EAF` pink / `#C79AFF` lilac | ● |
| Vec2 / Vec3 | `#F5C542` gold | ● |
| Quat / Rotator | `#9CC7FF` light blue | ● |
| Transform | `#F28C28` orange | ● |
| Color | `#FFFFFF` with a color swatch | ● |
| Entity / Component | `#2F7CF6` blue | ● |
| Asset | `#A06CD5` purple | ● |
| struct | `#1D4E89` dark blue | ● |
| enum | `#0E6B4D` dark green | ● |
| Array | element color | ▦ grid |
| Map | key/value colors split | ◐ two-color circle |

---

## 1. Events (red header)

Events are graph entry points. Each has one `▶` output. An event can
appear only once per graph, except Custom Events, which have unique names.

| Node | Out | Fires when |
|---|---|---|
| **Event BeginPlay** | `▶` | the entity starts playing: after spawn when the game is running, or at PIE start for entities in the level. Fires after all components have been created |
| **Event EndPlay** | `▶`, `● reason: EndPlayReason` | destroyed, level unloaded, or PIE stopped |
| **Event Tick** | `▶`, `● delta_seconds: float` | every frame, after physics. Can be disabled or throttled per class (Class Settings > Tick Interval) |
| **Event FixedTick** | `▶`, `● fixed_delta: float` | every fixed step (60 Hz default), before physics. Use for forces |
| **Event OnCollisionBegin / End** | `▶`, `● other: Entity`, `● hit: HitResult` | a physics contact starts or ends (Phase 13) |
| **Event OnTriggerEnter / Exit** | `▶`, `● other: Entity` | another body enters or leaves this entity's trigger collider |
| **Event OnDamaged** | `▶`, `● amount: float`, `● instigator: Entity`, `● type: DamageType` | `Apply Damage` targets this entity |
| **Event InputAction** *(config: action)* | `▶ Started`, `▶ Triggered`, `▶ Completed`, `● value: Wildcard` | the chosen Input Action changes state (Phase 10). The value type follows the action (bool/float/Vec2/Vec3) |
| **Event OnClicked** (entity) | `▶`, `● button: MouseButton` | the entity is clicked while "Enable Click Events" is on |
| **Custom Event** *(config: name, params)* | `▶`, one `●` per parameter | called by a **Call \<name\>** node, a timer, or a delegate binding |
| **Event \<Interface function\>** | `▶`, params | an interface this Blueprint implements is called on it |

---

## 2. Flow control (gray header)

| Node | In | Out | Behavior |
|---|---|---|---|
| **Branch** | `▶`, `● condition: bool` | `▶ true`, `▶ false` | if/else. An unconnected condition defaults to false (compiler warning) |
| **Sequence** | `▶` | `▶ then 0 … then N` (add pin +) | runs each output in order. If an output starts a latent action, the next one still runs immediately |
| **Switch on Int** | `▶`, `● selection: int` | `▶ 0 … ▶ N`, `▶ default` | pins are added with +. Optional start index |
| **Switch on String / Name** | `▶`, `● selection` | one `▶` per case, `▶ default` | case-sensitive toggle in node details |
| **Switch on Enum** *(config: enum)* | `▶`, `● selection: enum` | one `▶` per enum value | pins update automatically when the enum asset changes |
| **For Loop** | `▶`, `● first: int`, `● last: int` | `▶ loop body`, `● index: int`, `▶ completed` | inclusive range. Runs synchronously in one frame |
| **For Loop with Break** | `▶`, `● first`, `● last`, `▶ break` | `▶ loop body`, `● index`, `▶ completed` | `▶ break` stops after the current iteration |
| **For Each** | `▶`, `● array: Array<T>` | `▶ loop body`, `● element: T`, `● index: int`, `▶ completed` | iterating a copy; changing the array inside the loop doesn't affect it |
| **While Loop** | `▶`, `● condition: bool` | `▶ loop body`, `▶ completed` | condition re-evaluated each iteration. Subject to the instruction budget (1M instructions per event in editor builds) |
| **Do Once** | `▶`, `▶ reset`, `● start closed: bool` | `▶ completed` | passes through the first time only, until reset |
| **Do N** | `▶ enter`, `● n: int`, `▶ reset` | `▶ exit`, `● counter: int` | passes through N times |
| **Gate** | `▶ enter`, `▶ open`, `▶ close`, `▶ toggle`, `● start closed: bool` | `▶ exit` | enter passes only while open |
| **Flip Flop** | `▶` | `▶ A`, `▶ B`, `● is A: bool` | alternates outputs |
| **MultiGate** | `▶`, `▶ reset`, `● is random: bool`, `● loop: bool`, `● start index: int` | `▶ out 0 … N` | fires one output per call, in order or random |
| **Select** *(pure)* | `● index: int\|bool\|enum`, `● option 0 … N: Wildcard` | `● return: Wildcard` | picks a value; like a ternary |

---

## 3. Latent nodes (orange header, clock icon)

All latent nodes are cancelled automatically if the owning entity is
destroyed. They can't be used inside **functions** (only in event graphs
and macros), because functions must return in the same frame. The
compiler reports an error with that explanation.

| Node | In | Out | Behavior |
|---|---|---|---|
| **Delay** | `▶`, `● duration: float` | `▶ completed` | waits in game time (paused while the game is paused). Calling again while waiting is ignored |
| **Retriggerable Delay** | `▶`, `● duration: float` | `▶ completed` | calling again restarts the timer |
| **Delay Until Next Tick** | `▶` | `▶ completed` | resumes next frame |
| **Timeline** *(asset or embedded)* | `▶ play`, `▶ play from start`, `▶ stop`, `▶ reverse`, `▶ reverse from end`, `● new time: float`, `▶ set new time` | `▶ update`, `▶ finished`, `● direction`, one `●` per curve track | curve tracks (float, Vec3, Color) and event tracks edited in an embedded curve editor. Settings: length, loop, autoplay, play rate |
| **Move Component To** | `▶ move`, `▶ stop`, `▶ return`, `● component`, `● target location: Vec3`, `● target rotation: Rotator`, `● time: float`, `● ease in/out: bool` | `▶ completed` | interpolates a component's relative transform |
| **Wait for Event** *(config: event dispatcher)* | `▶`, `● target: Entity` | `▶ completed`, dispatcher params | resumes the next time that dispatcher fires on the target |
| **Async Load Asset** | `▶`, `● asset: SoftAsset<T>` | `▶ completed`, `● loaded: Asset<T>` | loads in the background (Phase 8) |
| **AI Move To** | `▶`, `● pawn: Entity`, `● destination: Vec3`, `● target: Entity`, `● acceptance radius: float` | `▶ on success`, `▶ on fail`, `● result` | Phase 20. Uses the target if set, otherwise the destination |
| **Play Montage and Wait** | `▶`, `● mesh`, `● montage: Asset<Montage>`, `● rate: float`, `● section: Name` | `▶ on completed`, `▶ on blend out`, `▶ on interrupted`, `▶ on notify begin`, `● notify name` | Phase 16 |

---

## 4. Variables and data

| Node | Pins | Behavior |
|---|---|---|
| **Get \<Variable\>** *(pure)* | Out `● value` | reads a Blueprint variable. For another entity: `● target: Entity` input appears |
| **Set \<Variable\>** | In `▶`, `● value`; Out `▶`, `● value` (the new value) | writes the variable. If the variable is marked Replicated, it's only honored on the server (Phase 22) |
| **Get / Set Local** | as above | variables that only exist inside a function call |
| **Make \<Struct\>** *(pure)* | In one `●` per field; Out `● struct` | builds a struct value |
| **Break \<Struct\>** *(pure)* | In `● struct`; Out one `●` per field | pins can be hidden in node details. Pins on other nodes can also be "split" directly (right-click > Split Struct Pin) |
| **Set Members in \<Struct\>** | In `▶`, `● struct (ref)`, chosen fields; Out `▶` | changes only the chosen fields |
| **Make Literal** *(pure)* | In the literal; Out `● value` | for int/float/bool/string/Name/Text |
| **Is Valid** *(pure and exec versions)* | In `● object`; Out `● return: bool` or `▶ is valid` / `▶ is not valid` | checks that an entity, component or asset is alive/loaded |

**Promote to Variable** (right-click a pin) creates a variable of the pin's
type and a Set node wired to it.

---

## 5. Math (green header for pure nodes)

All math nodes are pure. Operators with the same shape (`+`, `−`, `×`,
`÷`) accept `int`, `float`, `Vec2`, `Vec3` and mixed forms (`Vec3 × float`);
the node picks the right typed opcode once pins are connected. Pins can be
added (`A + B + C + …`) with +.

| Group | Nodes |
|---|---|
| Arithmetic | Add, Subtract, Multiply, Divide (int divide truncates; divide by zero returns 0 and logs a warning in editor builds), Modulo, Negate, Abs, Sign, Power, Sqrt, Exp, Log |
| Comparison | `==`, `!=` (float version has an `● error tolerance` pin), `<`, `<=`, `>`, `>=`, Nearly Equal, In Range |
| Boolean | AND, OR, XOR, NOT, NAND |
| Rounding | Floor, Ceil, Round, Truncate, Frac |
| Range | Min, Max, Clamp, Map Range (clamped / unclamped), Normalize to Range, Wrap |
| Interpolation | Lerp, Ease (function: linear, sine, quad, cubic, expo, in/out/in-out), FInterp To / VInterp To / RInterp To (frame-rate independent, `● delta time`, `● speed`), Spring Interp |
| Trig | Sin, Cos, Tan, Asin, Acos, Atan, Atan2 (degrees and radians versions), Degrees ↔ Radians |
| Random | Random Float, Random Float in Range, Random Int in Range, Random Bool, Random Bool with Weight, Random Unit Vector, Random Point in Box, Random Point in Sphere, **Random Stream** versions of all (seeded, reproducible) |
| Vector | Make/Break Vec3, Length, Length Squared, Distance, Normalize (`● tolerance`), Dot, Cross, Project on Vector, Project on Plane, Reflect, Rotate Vector (by Rotator/Quat), Get Forward/Right/Up Vector (from Rotator), Direction Between, Clamp Length |
| Rotation | Make/Break Rotator, Combine Rotators, Invert, Find Look-at Rotation, Delta Rotator, Rotator ↔ Quat, Slerp |
| Transform | Make/Break Transform, Compose, Inverse, Transform Location/Direction, Inverse Transform Location/Direction |
| **Math Expression** | In: one `●` per variable found in the expression; Out `● result` | type an expression such as `a * b + sin(c) * 2`; the node creates pins for `a`, `b`, `c` and compiles to the same opcodes as separate nodes |

---

## 6. Entity and world

| Node | In | Out | Behavior |
|---|---|---|---|
| **Spawn Prefab** *(config: class)* | `▶`, `● class: Asset<Prefab\|Blueprint>`, `● transform: Transform`, `● owner: Entity`, plus one pin per variable marked *Expose on Spawn* | `▶`, `● spawned: Entity` | variables are set before BeginPlay runs |
| **Destroy Entity** | `▶`, `● target: Entity` (default self) | `▶` | destroyed at the end of the frame; EndPlay fires first |
| **Get Self** *(pure)* | — | `● self: Entity` | |
| **Get / Add / Remove Component** *(config: type)* | `▶` (not for Get), `● target: Entity` | `▶`, `● component` | Get is pure and returns null (fails Is Valid) when missing |
| **Has Component** *(pure)* | `● target`, type | `● return: bool` | |
| **Get / Set World Location, Rotation, Scale, Transform** | `● target`, value, (`● sweep: bool`, `● teleport: bool` on Set) | `▶`, value / `● hit` | sweep stops at the first physics hit |
| **Add World / Local Offset, Rotation** | as above | as above | relative moves |
| **Attach To** | `▶`, `● child`, `● parent`, `● socket: Name`, `● rule: Keep World \| Keep Relative \| Snap` | `▶` | uses the `Parent` component |
| **Detach** | `▶`, `● target`, `● keep world transform: bool` | `▶` | |
| **Find Entities with Tag** | `● tag: Name` | `● entities: Array<Entity>` | pure. Uses a tag index, not a full scan |
| **Find Entities of Class** | `● class` | `● entities: Array<Entity>` | pure. Warns if used in Tick (slow pattern) |
| **Get Player Pawn / Controller / Camera** | `● player index: int` | `● entity` | pure. Phase 9 framework |
| **Get Game Mode** | — | `● game mode: Entity` | pure |
| **Set Timer by Event / by Function Name** | `▶`, `● event: Delegate` (or `● name`), `● time: float`, `● looping: bool` | `▶`, `● handle: TimerHandle` | |
| **Clear / Pause / Unpause Timer** | `▶`, `● handle` | `▶` | |
| **Get Game Time / Real Time / Delta Seconds** | — | `● seconds: float` | pure |
| **Set Game Paused / Set Time Dilation** | `▶`, value | `▶` | |
| **Open Level** | `▶`, `● level: Asset<Scene>`, `● options: string` | `▶` | unloads the current scene(s) |
| **Load / Unload Level Additive** *(latent)* | `▶`, `● level`, `● make visible: bool` | `▶ completed` | streaming (Phase 9/21) |
| **Quit Game** | `▶` | — | stops PIE in the editor |

---

## 7. Physics (Phase 13)

| Node | In | Out |
|---|---|---|
| **Line Trace (single / multi)** | `● start: Vec3`, `● end: Vec3`, `● layers: LayerMask`, `● ignore self: bool`, `● ignore: Array<Entity>`, `● draw debug: None \| One Frame \| Duration` | `● hit: bool`, `● result: HitResult` (or `Array<HitResult>`) |
| **Sphere / Box / Capsule Trace** | as above plus shape size | as above |
| **Overlap Sphere / Box** | `● center`, size, `● layers` | `● entities: Array<Entity>` |
| **Break HitResult** *(pure)* | `● hit` | `● entity`, `● location`, `● normal`, `● distance`, `● physics material`, `● bone name` |
| **Add Force / Add Impulse / Add Torque** | `▶`, `● target`, `● vector: Vec3`, `● at location (optional)`, `● is velocity change: bool` | `▶` |
| **Set / Get Linear Velocity, Angular Velocity** | target, value | |
| **Set Simulate Physics / Set Gravity Enabled / Set Collision Enabled** | `▶`, `● target`, `● value: bool` | `▶` |
| **Launch Character** | `▶`, `● character`, `● velocity`, `● override XY`, `● override Z` | `▶` |

---

## 8. Casting, types and conversion (blue-gray header)

| Node | In | Out | Behavior |
|---|---|---|---|
| **Cast To \<Class\>** | `▶`, `● object` | `▶ success`, `▶ failed`, `● as <Class>` | Pure version available (no exec pins; returns null on failure) |
| **Does Implement Interface** *(pure)* | `● object`, interface | `● return: bool` | |
| **\<Interface function\> (message)** | `▶`, `● target`, params | `▶`, return | does nothing if the target doesn't implement it (no error) |
| **Conversions** | auto-inserted as small "dot" nodes when connecting compatible pins: int→float, float→int (truncate, with warning), any→string, Name↔string, Vec3→string, bool→string, enum→string/Name, Rotator↔Quat | | |
| **To Text / Format Text** | `● format: Text` with `{name}` placeholders | `● result: Text` | one input pin per placeholder (see [ROADMAP_DETAILS.md §D](../ROADMAP_DETAILS.md)) |

---

## 9. Strings, text and containers

| Group | Nodes |
|---|---|
| String | Append (+ pins), Len, Contains, Starts/Ends With, Find Substring, Replace, Split, Join, To Upper/Lower, Trim, Left/Right/Mid, Is Empty, Parse Into Array, String → Int/Float (with `● success`) |
| Text | Format Text, Text Is Empty, Text → String, As Number (with grouping and decimals), As Percent, As Currency, As Date/Time |
| Array | Add, Add Unique, Insert, Remove Index, Remove Item, Clear, Length, Get (copy / ref), Set Array Elem, Find, Contains, Is Valid Index, Last Index, Shuffle, Sort (by float/int/string key or comparator function), Append Array, Reverse, Random Element, Filter (by predicate function) |
| Set | Add, Remove, Contains, Length, To Array, Union, Intersection, Difference |
| Map | Add, Remove, Find (`● found: bool`, `● value`), Contains, Keys, Values, Length, Clear |

Array, Set and Map nodes are generic: the element type comes from the
connected container.

---

## 10. Event dispatchers (delegates)

Event dispatchers are a Blueprint's own multicast events (for example
`OnOpened` on `BP_Door`). They're created in the *My Blueprint* panel with a
parameter list.

| Node | In | Out |
|---|---|---|
| **Call \<Dispatcher\>** | `▶`, `● target`, params | `▶` |
| **Bind Event to \<Dispatcher\>** | `▶`, `● target`, `● event: Delegate` (drag from a Custom Event's red square) | `▶` |
| **Unbind / Unbind All** | `▶`, `● target`, (`● event`) | `▶` |
| **Assign \<Dispatcher\>** | `▶`, `● target` | `▶`, plus creates and binds a matching Custom Event in one step |

---

## 11. Debug and utility

| Node | In | Out | Notes |
|---|---|---|---|
| **Print String** | `▶`, `● text: string`, `● to screen: bool`, `● to log: bool`, `● color`, `● duration: float`, `● key: Name` (same key replaces the previous message) | `▶` | stripped from Shipping builds |
| **Draw Debug Line / Arrow / Sphere / Box / Point / String** | `▶`, shape params, `● color`, `● duration`, `● thickness` | `▶` | stripped from Shipping builds |
| **Breakpoint** | `▶` | `▶` | pauses PIE when a debugger is attached (same as F9 on any node) |
| **Assert** | `▶`, `● condition: bool`, `● message: string` | `▶` | logs an error with the node link and pauses PIE (editor) when false |
| **Reroute** | any one pin | same pin | wire organization; double-click a wire to add one |
| **Comment** | — | — | colored box grouping nodes, with a title; moving it moves its contents |

---

## 12. Functions, macros and graphs

| Node | Behavior |
|---|---|
| **Function Entry** / **Return Node** | inside a function graph; pins match the function's inputs and outputs. Multiple Return nodes are allowed |
| **Call \<Function\>** | calls a function on self or on a `● target`. Pure functions (marked *Pure* in details) have no exec pins; *Const* functions can't change variables |
| **Call Parent: \<Function\>** | calls the parent class's version inside an override |
| **Macro Instance** | inlines a macro graph. Macros can have several exec inputs/outputs and use latent nodes; they have no local variables of their own beyond "local" macro variables |
| **Collapsed Graph** | a group of nodes collapsed into one node for tidiness, with no semantic change |

---

## 13. Compiler errors and warnings (message catalog)

| Code | Level | Message (shown on the node and in Messages) |
|---|---|---|
| BP001 | error | "Pin '{pin}' needs a {expected}, but it's connected to a {actual}." |
| BP002 | error | "'{node}' is a latent node and can't be used in a function. Move it to the Event Graph or a macro." |
| BP003 | error | "Pure nodes form a loop: {chain}. A value can't depend on itself." |
| BP004 | error | "The function '{fn}' no longer exists on '{class}'. It may have been renamed; check CoreRedirects." |
| BP005 | error | "The variable '{var}' was deleted. Replace or remove this node." |
| BP006 | error | "Event '{event}' appears more than once in this graph." |
| BP007 | error | "Unknown node type '{type}'." (also a node whose settings are invalid, such as a Sequence with 0 outputs) |
| BP008 | error | "'{node}' has no pin '{pin}'." / "'{pin}' is an output; links go from an output to an input." / "The default for '{pin}' isn't a {type}." |
| BP009 | error | "Input '{pin}' has {n} links; a data input takes one." / "Exec output '{pin}' has {n} links; it can only run one thing. Use a Sequence." |
| BP010 | error | "'{node}' is an event and belongs in the Event Graph." (and Function Entry/Return outside a function graph) |
| BP011 | error | "The function '{fn}' needs exactly one Function Entry node (it has {n})." |
| BP012 | error | "'{node}' uses a {type} value; Blueprints can't run {type} values yet." (types and nodes the compiler doesn't lower yet: structs, arrays, latent nodes before Phase 12 step 3) |
| BP013 | error | "'{node}' changes an array, so its '{pin}' pin must be connected to an array variable (a Get node)." |
| BP014 | error | "The Custom Event '{event}' doesn't take the same parameters as '{dispatcher}', so it can't be bound to it." |
| BP015 | error | "This Blueprint doesn't implement '{interface}'. Add it to the Class Settings' interfaces first." |
| BP016 | error | "The macro '{macro}' ends up containing itself here. A macro can't be inlined into itself." |
| BP101 | warning | "Input '{pin}' isn't connected and will use its default value ({default})." |
| BP102 | warning | "This node's output isn't used, and it has no side effects. It can be removed." |
| BP103 | warning | "'{node}' in Event Tick runs every frame and searches the whole world. Consider caching the result in BeginPlay." |
| BP104 | warning | "Float to int conversion truncates (1.9 becomes 1). Use Round, Floor or Ceil to be explicit." |
| BP105 | warning | "Code after this node is unreachable." |
| BP201 | runtime | "Accessed '{field}' on an invalid entity in '{node}'. Use Is Valid first." (logged once per node per PIE session, execution continues with a default value) |
| BP202 | runtime | "'{event}' ran more than {budget} instructions and was stopped. Check for an infinite loop near '{node}'." |
| BP203 | runtime | "'{fn}' was called more than {depth} levels deep. Check for endless recursion." |
| BP204 | runtime | "The call to '{fn}' was refused." (the native function rejected its arguments) |
| BP205 | runtime | "Index {i} is out of range (the array has {n} items) in '{event}'. Check Is Valid Index first." (Get returns the default; Set does nothing; logged once per node) |
| BP206 | runtime | "Spawn Blueprint in '{event}' has nothing to spawn with (no spawner is set up)." (the VM runs without a BlueprintSystem) |

---

## 14. Test plan for the node library

- **One unit test per node** in `tests/test_blueprint_nodes.cpp`: builds a
  tiny graph in code (`GraphBuilder`), compiles it, runs it on a headless
  world, and checks the output. For example: `ForLoop(1, 3)` produces
  bodies with index 1, 2, 3 then `completed`.
- **Latent tests** step the world clock manually (`world.AdvanceTime(0.5)`)
  and check resume order.
- **Compiler tests** for every error code in §13: the invalid graph must
  produce exactly that code, attached to the right node.
- **Golden bytecode tests** for a handful of graphs (like the `BP_Door`
  example), so opcode changes are noticed in review.
- **Fuzz test:** random valid graphs from the node library must compile,
  and running them must not crash (with the instruction budget as the
  loop guard).
