#pragma once

#include "aether/blueprint/types.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace aether::bp {

// The saved form of a Blueprint (Phase 12 step 1, docs/design/PHASE_SPECS.md
// §12.2). Pins aren't stored: they come from the node type (nodes.h).

using NodeId = u32; // unique within a graph; 0 = none

struct PinRef {
    NodeId node = 0;
    std::string pin;
    bool operator==(const PinRef& o) const { return node == o.node && pin == o.pin; }
};

struct Link {
    PinRef from; // an output
    PinRef to;   // an input
    bool operator==(const Link& o) const { return from == o.from && to == o.to; }
};

struct Node {
    NodeId id = 0;
    std::string type;  // stable type ID: "Flow.Branch", "Var.Get:IsOpen", "Call.Native:Health.Heal"
    float x = 0.0f, y = 0.0f;
    nlohmann::json config = nlohmann::json::object();   // node-type settings (Sequence's count, a custom event's name)
    nlohmann::json defaults = nlohmann::json::object(); // input pin name -> value, for inputs the user changed
    std::string comment;
};

enum VariableFlags : u32 {
    Var_None = 0,
    Var_InstanceEditable = 1u << 0, // editable per placed instance in the Details panel
    Var_ExposeOnSpawn = 1u << 1,    // a pin on Spawn nodes
};

struct Variable {
    std::string name;
    PinType type = PinType::Of(ValueType::Float);
    Value default_value;
    u32 flags = Var_None;
    std::string tooltip;
    std::string category;
};

enum class GraphKind : u8 { EventGraph, Function, Macro };

// A comment box drawn behind nodes (the editor's C key). Moving it moves the
// nodes inside it; it has no effect on compiling.
struct CommentBox {
    std::string text;
    float x = 0.0f, y = 0.0f, width = 200.0f, height = 100.0f;
    u32 color = 0x40FFFFFF; // ImGui order (ABGR), usually translucent
};

struct Graph {
    std::string name;
    GraphKind kind = GraphKind::EventGraph;
    std::vector<Node> nodes;
    std::vector<Link> links;
    // Function graphs: the signature (Function.Entry's outputs, Function.Return's inputs).
    // Macro graphs: the same, and pins may be exec ("type": "exec").
    std::vector<Variable> inputs;
    std::vector<Variable> outputs;
    bool pure = false; // a pure function: called without exec pins
    std::vector<CommentBox> comments;

    Node* Find(NodeId id);
    const Node* Find(NodeId id) const;
    NodeId NextId() const; // one past the highest ID in use
};

// An event dispatcher (BLUEPRINT_NODES.md §10): a multicast event other
// Blueprints bind their Custom Events to.
struct Dispatcher {
    std::string name;
    std::vector<Variable> params;
};

struct Blueprint {
    std::string parent = "native:Entity"; // a native base class, or another Blueprint's asset GUID
    std::vector<Variable> variables;
    std::vector<Graph> graphs;
    std::vector<Dispatcher> dispatchers;
    std::vector<std::string> interfaces; // Blueprint Interfaces this class implements

    const Dispatcher* FindDispatcher(std::string_view name) const;
    bool Implements(std::string_view interface_name) const;

    const Variable* FindVariable(std::string_view name) const;
    const Graph* FindGraph(std::string_view name) const;
    Graph* FindGraph(std::string_view name);
};

// .abp files: {"$type": "Blueprint", "$version": 1, "parent": ..., "variables":
// [...], "graphs": [...]}, the shape in docs/ROADMAP_DETAILS.md §A.4.
nlohmann::json BlueprintToJson(const Blueprint& blueprint);
bool BlueprintFromJson(const nlohmann::json& json, Blueprint& out, std::string* error = nullptr);
bool SaveBlueprint(const Blueprint& blueprint, const std::filesystem::path& path, std::string* error = nullptr);
bool LoadBlueprint(const std::filesystem::path& path, Blueprint& out, std::string* error = nullptr);

// Builds graphs in code (tests, samples, the editor's paste).
class GraphBuilder {
public:
    explicit GraphBuilder(Graph& graph) : graph_(graph) {}

    NodeId Add(const std::string& type, nlohmann::json config = nlohmann::json::object(), float x = 0.0f,
               float y = 0.0f);
    GraphBuilder& Connect(NodeId from, const std::string& from_pin, NodeId to, const std::string& to_pin);
    GraphBuilder& Default(NodeId node, const std::string& pin, nlohmann::json value);

private:
    Graph& graph_;
};

} // namespace aether::bp
