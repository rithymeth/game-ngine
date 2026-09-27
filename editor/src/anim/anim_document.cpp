#include "anim/anim_document.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <set>

namespace aether::editor {

using anim::AnimGraph;
using anim::AnimNode;
using anim::AnimNodeKind;
using anim::CompareOp;
using anim::VarType;
using nlohmann::json;

namespace {

// Whether a comparison works on a variable type (as ValidateAnimGraph checks).
bool OpFits(CompareOp op, VarType type) {
    const bool equality = op == CompareOp::Equal || op == CompareOp::NotEqual;
    switch (type) {
    case VarType::Trigger: return op == CompareOp::Triggered;
    case VarType::Bool: return op == CompareOp::IsTrue || op == CompareOp::IsFalse || equality;
    default: return op != CompareOp::IsTrue && op != CompareOp::IsFalse && op != CompareOp::Triggered;
    }
}

// Removes nodes and unsets everything that used them.
void EraseNodes(AnimGraph& g, const std::set<u32>& doomed) {
    g.nodes.erase(std::remove_if(g.nodes.begin(), g.nodes.end(), [&](const AnimNode& n) { return doomed.count(n.id) != 0; }), g.nodes.end());
    for (AnimNode& n : g.nodes) {
        for (u32& in : n.inputs) {
            if (doomed.count(in)) in = 0;
        }
    }
    if (doomed.count(g.output)) g.output = 0;
    for (anim::StateMachine& m : g.machines) {
        for (anim::AnimState& s : m.states) {
            if (doomed.count(s.pose)) s.pose = 0;
        }
    }
}

u32 NextNodeId(const AnimGraph& g) {
    u32 id = 1;
    for (const AnimNode& n : g.nodes) id = std::max(id, n.id + 1);
    return id;
}

std::string FreeName(const std::string& base, const std::function<bool(const std::string&)>& taken) {
    std::string name = base;
    for (int n = 1; taken(name); ++n) name = base + "_" + std::to_string(n);
    return name;
}

} // namespace

AnimGraphDocument::AnimGraphDocument(AnimGraph graph, std::filesystem::path path) : graph_(std::move(graph)), path_(std::move(path)) {}

bool AnimGraphDocument::Load(const std::filesystem::path& path, std::string* error) {
    std::ifstream file(path);
    if (!file) {
        if (error != nullptr) *error = "can't read " + path.string();
        return false;
    }
    const json j = json::parse(file, nullptr, false);
    AnimGraph g;
    if (j.is_discarded() || !anim::AnimGraphFromJson(j, g, error)) {
        if (error != nullptr && j.is_discarded()) *error = path.string() + " isn't valid JSON";
        return false;
    }
    graph_ = std::move(g);
    path_ = path;
    dirty_ = false;
    history_.Clear();
    ++revision_;
    return true;
}

bool AnimGraphDocument::Save(std::string* error) {
    if (path_.empty()) {
        if (error != nullptr) *error = "the graph has no file yet; use Save As";
        return false;
    }
    return SaveAs(path_, error);
}

bool AnimGraphDocument::SaveAs(const std::filesystem::path& path, std::string* error) {
    std::ofstream file(path);
    if (!file) {
        if (error != nullptr) *error = "can't write " + path.string();
        return false;
    }
    file << anim::AnimGraphToJson(graph_).dump(2) << "\n";
    path_ = path;
    dirty_ = false;
    return true;
}

std::string AnimGraphDocument::Name() const { return path_.empty() ? "Untitled" : path_.stem().string(); }

void AnimGraphDocument::SetAssets(anim::AnimAssets assets, std::vector<std::string> clip_names, std::vector<std::string> blend_space_names) {
    assets_ = std::move(assets);
    has_assets_ = true;
    clip_names_ = std::move(clip_names);
    blend_space_names_ = std::move(blend_space_names);
    ++revision_;
}

void AnimGraphDocument::Edit(const std::string& label, const std::function<void(AnimGraph&)>& change, const std::string& merge_key) {
    history_.Record(label, anim::AnimGraphToJson(graph_), merge_key);
    change(graph_);
    dirty_ = true;
    ++revision_;
}

void AnimGraphDocument::Restore(const json& state) {
    AnimGraph g;
    anim::AnimGraphFromJson(state, g);
    graph_ = std::move(g);
    dirty_ = true;
    ++revision_;
}

bool AnimGraphDocument::Undo() {
    json restore;
    if (!history_.Undo(anim::AnimGraphToJson(graph_), restore)) return false;
    Restore(restore);
    return true;
}

bool AnimGraphDocument::Redo() {
    json restore;
    if (!history_.Redo(anim::AnimGraphToJson(graph_), restore)) return false;
    Restore(restore);
    return true;
}

void AnimGraphDocument::CancelEdit() {
    json restore;
    if (history_.Cancel(restore)) Restore(restore);
}

const std::vector<anim::AnimDiagnostic>& AnimGraphDocument::Diagnostics() const {
    if (diagnosed_ != revision_) {
        diagnostics_ = anim::ValidateAnimGraph(graph_, has_assets_ ? &assets_ : nullptr);
        diagnosed_ = revision_;
    }
    return diagnostics_;
}

usize AnimGraphDocument::ErrorCount() const {
    const auto& d = Diagnostics();
    return static_cast<usize>(std::count_if(d.begin(), d.end(), [](const anim::AnimDiagnostic& x) { return x.error; }));
}

anim::StateMachine* AnimGraphDocument::Machine(AnimGraph& g, const std::string& name) {
    for (anim::StateMachine& m : g.machines) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

// --- Variables -------------------------------------------------------------------------------

bool AnimGraphDocument::IsValidName(const std::string& name) {
    if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ') return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x20 && c != 0x7F; });
}

std::string AnimGraphDocument::AddVariable(VarType type) {
    const std::string name = FreeName("NewVar", [&](const std::string& n) { return graph_.FindVariable(n) != nullptr; });
    Edit("Add variable", [&](AnimGraph& g) { g.variables.push_back({name, type, 0.0f}); });
    return name;
}

bool AnimGraphDocument::RenameVariable(const std::string& from, const std::string& to, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error != nullptr) *error = m;
        return false;
    };
    if (graph_.FindVariable(from) == nullptr) return fail("no variable named '" + from + "'");
    if (from == to) return true;
    if (!IsValidName(to)) return fail("'" + to + "' isn't a valid name");
    if (graph_.FindVariable(to) != nullptr) return fail("there's already a variable named '" + to + "'");
    Edit("Rename variable", [&](AnimGraph& g) {
        for (anim::AnimVariable& v : g.variables) {
            if (v.name == from) v.name = to;
        }
        for (AnimNode& n : g.nodes) {
            if (n.variable == from) n.variable = to;
            if (n.variable_y == from) n.variable_y = to;
        }
        for (anim::StateMachine& m : g.machines) {
            for (anim::Transition& t : m.transitions) {
                for (anim::Condition& c : t.conditions) {
                    if (c.variable == from) c.variable = to;
                }
            }
        }
    });
    return true;
}

bool AnimGraphDocument::RemoveVariable(const std::string& name) {
    if (graph_.FindVariable(name) == nullptr) return false;
    Edit("Remove variable", [&](AnimGraph& g) {
        g.variables.erase(std::remove_if(g.variables.begin(), g.variables.end(), [&](const anim::AnimVariable& v) { return v.name == name; }),
                          g.variables.end());
        for (AnimNode& n : g.nodes) {
            if (n.variable == name) n.variable.clear();
            if (n.variable_y == name) n.variable_y.clear();
        }
        for (anim::StateMachine& m : g.machines) {
            for (anim::Transition& t : m.transitions) {
                t.conditions.erase(std::remove_if(t.conditions.begin(), t.conditions.end(),
                                                  [&](const anim::Condition& c) { return c.variable == name; }),
                                   t.conditions.end());
            }
        }
    });
    return true;
}

bool AnimGraphDocument::SetVariableType(const std::string& name, VarType type) {
    const anim::AnimVariable* v = graph_.FindVariable(name);
    if (v == nullptr) return false;
    if (v->type == type) return true;
    Edit("Change variable type", [&](AnimGraph& g) {
        for (anim::AnimVariable& x : g.variables) {
            if (x.name == name) x.type = type, x.default_value = 0.0f;
        }
        for (anim::StateMachine& m : g.machines) {
            for (anim::Transition& t : m.transitions) {
                t.conditions.erase(std::remove_if(t.conditions.begin(), t.conditions.end(),
                                                  [&](const anim::Condition& c) { return c.variable == name && !OpFits(c.op, type); }),
                                   t.conditions.end());
            }
        }
    });
    return true;
}

// --- Pose nodes ------------------------------------------------------------------------------

u32 AnimGraphDocument::AddNode(AnimNodeKind kind, f32 x, f32 y) {
    const u32 id = NextNodeId(graph_);
    Edit("Add node", [&](AnimGraph& g) {
        AnimNode n;
        n.id = id;
        n.kind = kind;
        n.x = x;
        n.y = y;
        switch (kind) {
        case AnimNodeKind::Blend:
        case AnimNodeKind::BlendByBool:
        case AnimNodeKind::BlendByInt:
        case AnimNodeKind::Layered:
        case AnimNodeKind::Additive: n.inputs = {0, 0}; break;
        case AnimNodeKind::Slot:
            n.inputs = {0};
            n.slot = "Default";
            break;
        default: break;
        }
        g.nodes.push_back(std::move(n));
        if (g.nodes.size() == 1) g.output = id; // the first node is the output until told otherwise
    });
    return id;
}

void AnimGraphDocument::DeleteNodes(const std::vector<u32>& ids) {
    const std::set<u32> doomed(ids.begin(), ids.end());
    Edit("Delete", [&](AnimGraph& g) { EraseNodes(g, doomed); });
}

bool AnimGraphDocument::ConnectPose(u32 from, u32 to, usize index, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error != nullptr) *error = m;
        return false;
    };
    const AnimNode* target = graph_.Find(to);
    if (graph_.Find(from) == nullptr || target == nullptr) return fail("That node no longer exists.");
    if (from == to) return fail("A node can't feed itself.");
    if (target->kind == AnimNodeKind::Clip || target->kind == AnimNodeKind::BlendSpace || target->kind == AnimNodeKind::StateMachine) {
        return fail("That node has no pose inputs.");
    }
    if (index >= target->inputs.size() && target->kind != AnimNodeKind::BlendByInt) return fail("That input doesn't exist.");
    // No loops: `to` mustn't already feed `from`.
    std::set<u32> seen;
    std::function<bool(u32)> reaches = [&](u32 id) {
        if (id == to) return true;
        if (!seen.insert(id).second) return false;
        const AnimNode* n = graph_.Find(id);
        if (n == nullptr) return false;
        for (u32 in : n->inputs) {
            if (reaches(in)) return true;
        }
        return false;
    };
    if (reaches(from)) return fail("That would make a loop; pose nodes can't feed themselves.");
    Edit("Connect", [&](AnimGraph& g) {
        for (AnimNode& n : g.nodes) {
            if (n.id != to) continue;
            if (index >= n.inputs.size()) n.inputs.resize(index + 1, 0);
            n.inputs[index] = from;
        }
    });
    return true;
}

bool AnimGraphDocument::SetOutput(u32 node) {
    if (graph_.Find(node) == nullptr) return false;
    if (graph_.output == node) return true;
    Edit("Set output", [&](AnimGraph& g) { g.output = node; });
    return true;
}

// --- State machines --------------------------------------------------------------------------

std::string AnimGraphDocument::AddMachine(f32 x, f32 y, u32* node) {
    const std::string name = FreeName("StateMachine", [&](const std::string& n) { return graph_.FindMachine(n) != nullptr; });
    const u32 id = NextNodeId(graph_);
    Edit("Add state machine", [&](AnimGraph& g) {
        anim::StateMachine m;
        m.name = name;
        g.machines.push_back(std::move(m));
        AnimNode n;
        n.id = id;
        n.kind = AnimNodeKind::StateMachine;
        n.machine = name;
        n.x = x;
        n.y = y;
        g.nodes.push_back(std::move(n));
        if (g.nodes.size() == 1) g.output = id;
    });
    if (node != nullptr) *node = id;
    return name;
}

bool AnimGraphDocument::RenameMachine(const std::string& from, const std::string& to, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error != nullptr) *error = m;
        return false;
    };
    if (graph_.FindMachine(from) == nullptr) return fail("no state machine named '" + from + "'");
    if (from == to) return true;
    if (!IsValidName(to)) return fail("'" + to + "' isn't a valid name");
    if (graph_.FindMachine(to) != nullptr) return fail("there's already a state machine named '" + to + "'");
    Edit("Rename state machine", [&](AnimGraph& g) {
        Machine(g, from)->name = to;
        for (AnimNode& n : g.nodes) {
            if (n.kind == AnimNodeKind::StateMachine && n.machine == from) n.machine = to;
        }
    });
    return true;
}

bool AnimGraphDocument::RemoveMachine(const std::string& name) {
    if (graph_.FindMachine(name) == nullptr) return false;
    Edit("Remove state machine", [&](AnimGraph& g) {
        g.machines.erase(std::remove_if(g.machines.begin(), g.machines.end(), [&](const anim::StateMachine& m) { return m.name == name; }),
                         g.machines.end());
        std::set<u32> runners;
        for (const AnimNode& n : g.nodes) {
            if (n.kind == AnimNodeKind::StateMachine && n.machine == name) runners.insert(n.id);
        }
        EraseNodes(g, runners);
    });
    return true;
}

i32 AnimGraphDocument::AddState(const std::string& machine, f32 x, f32 y, bool conduit) {
    const anim::StateMachine* m = graph_.FindMachine(machine);
    if (m == nullptr) return -1;
    const std::string name = FreeName(conduit ? "Conduit" : "State", [&](const std::string& n) { return m->FindState(n) >= 0; });
    const u32 id = NextNodeId(graph_);
    i32 index = -1;
    Edit("Add state", [&](AnimGraph& g) {
        anim::StateMachine& sm = *Machine(g, machine);
        anim::AnimState s;
        s.name = name;
        s.conduit = conduit;
        s.x = x;
        s.y = y;
        if (!conduit) {
            AnimNode clip;
            clip.id = id;
            clip.x = x;
            clip.y = y + 400.0f; // out of the way in the pose graph
            g.nodes.push_back(clip);
            s.pose = id;
        }
        sm.states.push_back(std::move(s));
        index = static_cast<i32>(sm.states.size() - 1);
    });
    return index;
}

bool AnimGraphDocument::RenameState(const std::string& machine, const std::string& from, const std::string& to, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error != nullptr) *error = msg;
        return false;
    };
    const anim::StateMachine* m = graph_.FindMachine(machine);
    if (m == nullptr || m->FindState(from) < 0) return fail("no state named '" + from + "'");
    if (from == to) return true;
    if (!IsValidName(to)) return fail("'" + to + "' isn't a valid name");
    if (m->FindState(to) >= 0) return fail("there's already a state named '" + to + "'");
    Edit("Rename state", [&](AnimGraph& g) {
        anim::StateMachine& sm = *Machine(g, machine);
        sm.states[static_cast<usize>(sm.FindState(from))].name = to;
    });
    return true;
}

bool AnimGraphDocument::RemoveState(const std::string& machine, const std::string& state) {
    const anim::StateMachine* m = graph_.FindMachine(machine);
    const i32 index = m != nullptr ? m->FindState(state) : -1;
    if (index < 0) return false;
    Edit("Remove state", [&](AnimGraph& g) {
        anim::StateMachine& sm = *Machine(g, machine);
        sm.states.erase(sm.states.begin() + index);
        std::vector<anim::Transition> kept;
        for (anim::Transition t : sm.transitions) {
            if (t.from == index || static_cast<i32>(t.to) == index) continue;
            if (t.from > index) --t.from;
            if (static_cast<i32>(t.to) > index) --t.to;
            kept.push_back(std::move(t));
        }
        sm.transitions = std::move(kept);
        if (static_cast<i32>(sm.entry) == index) sm.entry = 0;
        else if (static_cast<i32>(sm.entry) > index) --sm.entry;
    });
    return true;
}

bool AnimGraphDocument::SetEntry(const std::string& machine, const std::string& state) {
    const anim::StateMachine* m = graph_.FindMachine(machine);
    const i32 index = m != nullptr ? m->FindState(state) : -1;
    if (index < 0) return false;
    Edit("Set entry state", [&](AnimGraph& g) { Machine(g, machine)->entry = static_cast<u32>(index); });
    return true;
}

i32 AnimGraphDocument::AddTransition(const std::string& machine, i32 from, u32 to) {
    const anim::StateMachine* m = graph_.FindMachine(machine);
    if (m == nullptr || to >= m->states.size() || from < anim::kAnyState || from >= static_cast<i32>(m->states.size())) return -1;
    if (from == static_cast<i32>(to)) return -1;
    i32 index = -1;
    Edit("Add transition", [&](AnimGraph& g) {
        anim::StateMachine& sm = *Machine(g, machine);
        anim::Transition t;
        t.from = from;
        t.to = to;
        sm.transitions.push_back(std::move(t));
        index = static_cast<i32>(sm.transitions.size() - 1);
    });
    return index;
}

bool AnimGraphDocument::RemoveTransition(const std::string& machine, usize index) {
    const anim::StateMachine* m = graph_.FindMachine(machine);
    if (m == nullptr || index >= m->transitions.size()) return false;
    Edit("Remove transition", [&](AnimGraph& g) {
        anim::StateMachine& sm = *Machine(g, machine);
        sm.transitions.erase(sm.transitions.begin() + static_cast<std::ptrdiff_t>(index));
    });
    return true;
}

} // namespace aether::editor
