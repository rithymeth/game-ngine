#pragma once

#include "aether/blueprint/graph.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aether::reflect {
struct FunctionInfo;
struct FieldInfo;
} // namespace aether::reflect

namespace aether::bp {

// Node types and their pins (Phase 12 step 1). A node's pins are worked out
// from its type ID, its config and the Blueprint (variables, functions,
// custom events), so they're never saved and never go stale.

enum class NodeKind : u8 {
    Event,          // a graph entry point: exec output(s) only
    Impure,         // exec in, exec out(s)
    Pure,           // no exec pins; runs when its output is read
    Latent,         // impure, finishes on a later frame (step 3)
    FunctionEntry,  // a function graph's entry
    FunctionReturn, // a function graph's exit
};

enum PinFlags : u32 {
    Pin_None = 0,
    Pin_WarnIfUnconnected = 1u << 0, // BP101 when left unconnected (Branch's condition)
    Pin_Self = 1u << 1,              // an Entity input that defaults to the Blueprint's own entity
    Pin_ByRef = 1u << 2,             // an array the node changes: must come from a Get <variable> (BP013)
};

struct PinDesc {
    std::string name;
    PinDir dir = PinDir::In;
    PinType type;
    Value default_value; // inputs: used when unconnected
    u32 flags = Pin_None;
};

struct NodeSignature {
    std::string title;    // "Branch", "Get IsOpen", "Heal"
    std::string category; // palette category: "Flow Control", "Math|Float"
    NodeKind kind = NodeKind::Impure;
    std::vector<PinDesc> pins;
    // Native calls and component fields (the compiler uses these in step 2).
    const reflect::TypeInfo* owner = nullptr;
    const reflect::FunctionInfo* function = nullptr;
    const reflect::FieldInfo* field = nullptr;
    // Events: which events count as the same for BP006 ("Event.Tick", or
    // "Event.Custom:Open").
    std::string event_key;

    const PinDesc* Find(std::string_view pin, PinDir dir) const;
    const PinDesc* Find(std::string_view pin) const; // either direction
    bool IsPure() const { return kind == NodeKind::Pure; }
    bool IsEvent() const { return kind == NodeKind::Event; }
};

// What a node type gets to build its signature.
struct NodeContext {
    const Blueprint& blueprint;
    const Graph& graph;
    const Node& node;
    std::string_view suffix; // for families: what follows the prefix ("IsOpen" in "Var.Get:IsOpen")
};

struct NodeError {
    std::string code; // BP004 (function gone), BP005 (variable gone), BP007 (unknown type or bad config)
    std::string message;
};

// Returns the signature, or nullopt with `error` set.
using NodeFactory = std::function<std::optional<NodeSignature>(const NodeContext& context, NodeError& error)>;

struct PaletteEntry {
    std::string id;
    std::string title;
    std::string category;
};
// Lists a family's members for a Blueprint (for the palette).
using FamilyLister = std::function<std::vector<PaletteEntry>(const Blueprint& blueprint)>;

// Custom node types (docs/ROADMAP_DETAILS.md §D): game modules add node
// libraries the same way the built-ins are registered. `id` is exact
// ("Flow.Branch"); a family's `prefix` ends with ':' ("Var.Get:").
void RegisterNodeType(const std::string& id, NodeFactory factory);
void RegisterNodeFamily(const std::string& prefix, NodeFactory factory, FamilyLister lister = {});

std::optional<NodeSignature> ResolveNode(const Blueprint& blueprint, const Graph& graph, const Node& node,
                                         NodeError* error = nullptr);

// Palette entries (for the editor's node search): every exact node type,
// plus the family members this Blueprint offers (Get/Set per variable, a
// call per function graph and custom event, native calls per reflected
// BlueprintCallable function, Get/Set per BlueprintReadWrite field), sorted
// by category then title.
std::vector<PaletteEntry> ListNodeTypes(const Blueprint& blueprint);

// Blueprint Interfaces (§12.1): named sets of functions any Blueprint can
// implement (as "Event.Interface:Name.Function" events). Calling one on an
// entity that doesn't implement it does nothing. Registered by the engine
// or a game module (and by interface assets, later).
struct InterfaceFunction {
    std::string name;
    std::vector<Variable> params;
};
struct BlueprintInterface {
    std::string name; // "Interactable"
    std::vector<InterfaceFunction> functions;
    const InterfaceFunction* Find(std::string_view function) const;
};
void RegisterBlueprintInterface(BlueprintInterface interface_);
const BlueprintInterface* FindBlueprintInterface(std::string_view name);

// Format Text's placeholders, in order of first use: "Hi {name}, {n} left"
// -> {"name", "n"}. "{{" and "}}" are literal braces.
std::vector<std::string> ParseFormatArgs(std::string_view format);

} // namespace aether::bp
