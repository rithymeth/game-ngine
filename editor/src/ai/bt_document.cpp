#include "ai/bt_document.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace aether::editor {

using nlohmann::json;
using namespace aether::ai;

namespace {
bool Fail(std::string* error, const std::string& m) {
    if (error != nullptr) *error = m;
    return false;
}
bool Within(const BtPath& path, const BtPath& ancestor) {
    return path.size() >= ancestor.size() && std::equal(ancestor.begin(), ancestor.end(), path.begin());
}
json* NodeAt(json& root, const BtPath& path) {
    json* n = &root;
    for (usize i : path) {
        if (!n->contains("children") || i >= (*n)["children"].size()) return nullptr;
        n = &(*n)["children"][i];
    }
    return n;
}
} // namespace

std::string BtProblemPath(const BehaviorTreeAsset& tree, const BtPath& path) {
    std::string out = "root";
    const BtNode* n = &tree.root;
    for (usize i : path) {
        if (i >= n->children.size()) break;
        n = &n->children[i];
        out += "/" + n->Label() + "[" + std::to_string(i) + "]";
    }
    return out;
}

std::vector<BtNodeType> BtCompositeTypes() { return {BtNodeType::Selector, BtNodeType::Sequence, BtNodeType::Parallel}; }
std::vector<BtNodeType> BtTaskTypes() {
    return {BtNodeType::Wait, BtNodeType::MoveTo, BtNodeType::SetBlackboard, BtNodeType::ClearBlackboard, BtNodeType::RunBlueprint,
            BtNodeType::RunLuau, BtNodeType::Log, BtNodeType::Succeed, BtNodeType::Fail};
}

BehaviorTreeDocument::BehaviorTreeDocument() {
    tree_.name = "BehaviorTree";
    tree_.root.type = BtNodeType::Selector;
    BtNode wait;
    wait.type = BtNodeType::Wait;
    tree_.root.children.push_back(wait);
}

BehaviorTreeDocument::BehaviorTreeDocument(BehaviorTreeAsset tree, std::filesystem::path path) : tree_(std::move(tree)), path_(std::move(path)) {}

bool BehaviorTreeDocument::Load(const std::filesystem::path& path, std::string* error) {
    std::ifstream in(path);
    if (!in) return Fail(error, "can't open " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    const json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded()) return Fail(error, path.string() + " isn't JSON");
    BehaviorTreeAsset t;
    if (!LoadBehaviorTree(j, t, error)) return false;
    tree_ = std::move(t);
    path_ = path;
    history_.Clear();
    dirty_ = false;
    ++revision_;
    return true;
}

bool BehaviorTreeDocument::Save(std::string* error) {
    if (path_.empty()) return Fail(error, "no file yet: Save As first");
    return SaveAs(path_, error);
}

bool BehaviorTreeDocument::SaveAs(const std::filesystem::path& path, std::string* error) {
    std::ofstream out(path);
    if (!out) return Fail(error, "can't write " + path.string());
    out << Text() << "\n";
    path_ = path;
    dirty_ = false;
    return true;
}

std::string BehaviorTreeDocument::Name() const {
    if (!path_.empty()) return path_.stem().string();
    return tree_.name.empty() ? "Untitled" : tree_.name;
}

const BtNode* BehaviorTreeDocument::Node(const BtPath& path) const {
    const BtNode* n = &tree_.root;
    for (usize i : path) {
        if (i >= n->children.size()) return nullptr;
        n = &n->children[i];
    }
    return n;
}

BtNode* BehaviorTreeDocument::Mutable(BehaviorTreeAsset& t, const BtPath& path) const {
    BtNode* n = &t.root;
    for (usize i : path) {
        if (i >= n->children.size()) return nullptr;
        n = &n->children[i];
    }
    return n;
}

void BehaviorTreeDocument::Edit(const std::string& label, const std::function<void(BehaviorTreeAsset&)>& change, const std::string& merge_key) {
    history_.Record(label, Snapshot(), merge_key);
    change(tree_);
    dirty_ = true;
    ++revision_;
}

void BehaviorTreeDocument::Restore(const json& snapshot) {
    BehaviorTreeAsset t;
    if (LoadBehaviorTree(snapshot, t)) tree_ = std::move(t);
    dirty_ = true;
    ++revision_;
}

bool BehaviorTreeDocument::Undo() {
    json restore;
    if (!history_.Undo(Snapshot(), restore)) return false;
    Restore(restore);
    return true;
}

bool BehaviorTreeDocument::Redo() {
    json restore;
    if (!history_.Redo(Snapshot(), restore)) return false;
    Restore(restore);
    return true;
}

std::optional<BtPath> BehaviorTreeDocument::AddNode(const BtPath& parent, BtNodeType type, std::optional<usize> index, std::string* error) {
    const BtNode* p = Node(parent);
    if (p == nullptr) return Fail(error, "no such node"), std::nullopt;
    if (!IsComposite(p->type)) return Fail(error, std::string("a ") + BtNodeTypeName(p->type) + " can't have children"), std::nullopt;
    const usize at = std::min(index.value_or(p->children.size()), p->children.size());
    Edit(std::string("Add ") + BtNodeTypeName(type), [&](BehaviorTreeAsset& t) {
        BtNode n;
        n.type = type;
        if (type == BtNodeType::Log) n.event = "Hello";
        BtNode* q = Mutable(t, parent);
        q->children.insert(q->children.begin() + static_cast<std::ptrdiff_t>(at), std::move(n));
    });
    BtPath out = parent;
    out.push_back(at);
    return out;
}

bool BehaviorTreeDocument::RemoveNode(const BtPath& path, std::string* error) {
    if (path.empty()) return Fail(error, "the root can't be removed");
    if (Node(path) == nullptr) return Fail(error, "no such node");
    Edit("Remove node", [&](BehaviorTreeAsset& t) {
        BtNode* p = Mutable(t, BtPath(path.begin(), path.end() - 1));
        p->children.erase(p->children.begin() + static_cast<std::ptrdiff_t>(path.back()));
    });
    return true;
}

std::optional<BtPath> BehaviorTreeDocument::MoveNode(const BtPath& path, const BtPath& parent, usize index, std::string* error) {
    if (path.empty()) return Fail(error, "the root can't be moved"), std::nullopt;
    if (Node(path) == nullptr || Node(parent) == nullptr) return Fail(error, "no such node"), std::nullopt;
    if (Within(parent, path)) return Fail(error, "a node can't go under itself"), std::nullopt;
    if (!IsComposite(Node(parent)->type)) return Fail(error, std::string("a ") + BtNodeTypeName(Node(parent)->type) + " can't have children"), std::nullopt;
    // Where the parent will be once the node is out.
    BtPath target = parent;
    const BtPath from_parent(path.begin(), path.end() - 1);
    if (target.size() > from_parent.size() && Within(target, from_parent) && target[from_parent.size()] > path.back()) --target[from_parent.size()];
    const usize count = Node(parent)->children.size() - (from_parent == parent ? 1 : 0);
    const usize at = std::min(index, count);
    if (from_parent == parent && at == path.back()) {
        BtPath same = path;
        return same; // nothing changes
    }
    Edit("Move node", [&](BehaviorTreeAsset& t) {
        BtNode* src = Mutable(t, from_parent);
        BtNode moving = std::move(src->children[path.back()]);
        src->children.erase(src->children.begin() + static_cast<std::ptrdiff_t>(path.back()));
        BtNode* dst = Mutable(t, target);
        dst->children.insert(dst->children.begin() + static_cast<std::ptrdiff_t>(at), std::move(moving));
    });
    target.push_back(at);
    return target;
}

std::optional<BtPath> BehaviorTreeDocument::DuplicateNode(const BtPath& path, std::string* error) {
    if (path.empty()) return Fail(error, "the root can't be duplicated"), std::nullopt;
    if (Node(path) == nullptr) return Fail(error, "no such node"), std::nullopt;
    Edit("Duplicate node", [&](BehaviorTreeAsset& t) {
        BtNode* p = Mutable(t, BtPath(path.begin(), path.end() - 1));
        BtNode copy = p->children[path.back()];
        p->children.insert(p->children.begin() + static_cast<std::ptrdiff_t>(path.back() + 1), std::move(copy));
    });
    BtPath out = path;
    ++out.back();
    return out;
}

bool BehaviorTreeDocument::SetNodeType(const BtPath& path, BtNodeType type, std::string* error) {
    const BtNode* n = Node(path);
    if (n == nullptr) return Fail(error, "no such node");
    if (!IsComposite(type) && !n->children.empty()) return Fail(error, "it has children, so it stays a composite");
    if (n->type == type) return true;
    Edit("Change node type", [&](BehaviorTreeAsset& t) { Mutable(t, path)->type = type; });
    return true;
}

json BehaviorTreeDocument::NodeJson(const BtPath& path) const {
    json root = SaveBehaviorTree(tree_)["root"];
    json* n = NodeAt(root, path);
    if (n == nullptr) return nullptr;
    json out = *n;
    out.erase("children");
    return out;
}

bool BehaviorTreeDocument::EditJson(const std::string& label, const BtPath& path, const std::function<bool(json&, std::string&)>& change, std::string* error,
                                    const std::string& merge_key) {
    json all = SaveBehaviorTree(tree_);
    json* n = NodeAt(all["root"], path);
    if (n == nullptr) return Fail(error, "no such node");
    std::string why;
    if (!change(*n, why)) return Fail(error, why);
    BehaviorTreeAsset t;
    if (!LoadBehaviorTree(all, t, &why)) return Fail(error, why);
    Edit(label, [&](BehaviorTreeAsset& a) { a = std::move(t); }, merge_key);
    return true;
}

bool BehaviorTreeDocument::SetNodeField(const BtPath& path, const std::string& key, const json& value, std::string* error, const std::string& merge_key) {
    if (key == "type" || key == "children" || key == "decorators" || key == "services") return Fail(error, "'" + key + "' has its own editor");
    return EditJson("Set " + key, path, [&](json& n, std::string&) {
        n[key] = value;
        return true;
    }, error, merge_key);
}

bool BehaviorTreeDocument::AddDecorator(const BtPath& path, BtDecoratorType type) {
    if (Node(path) == nullptr) return false;
    Edit("Add decorator", [&](BehaviorTreeAsset& t) {
        BtDecorator d;
        d.type = type;
        if (type == BtDecoratorType::BlackboardCondition && !t.blackboard.keys.empty()) d.key = t.blackboard.keys.front().name;
        Mutable(t, path)->decorators.push_back(std::move(d));
    });
    return true;
}

bool BehaviorTreeDocument::RemoveDecorator(const BtPath& path, usize index) {
    const BtNode* n = Node(path);
    if (n == nullptr || index >= n->decorators.size()) return false;
    Edit("Remove decorator", [&](BehaviorTreeAsset& t) {
        auto& ds = Mutable(t, path)->decorators;
        ds.erase(ds.begin() + static_cast<std::ptrdiff_t>(index));
    });
    return true;
}

bool BehaviorTreeDocument::MoveDecorator(const BtPath& path, usize from, usize to) {
    const BtNode* n = Node(path);
    if (n == nullptr || from >= n->decorators.size() || to >= n->decorators.size()) return false;
    if (from == to) return true;
    Edit("Move decorator", [&](BehaviorTreeAsset& t) {
        auto& ds = Mutable(t, path)->decorators;
        BtDecorator d = std::move(ds[from]);
        ds.erase(ds.begin() + static_cast<std::ptrdiff_t>(from));
        ds.insert(ds.begin() + static_cast<std::ptrdiff_t>(to), std::move(d));
    });
    return true;
}

json BehaviorTreeDocument::DecoratorJson(const BtPath& path, usize index) const {
    const json n = NodeJson(path);
    return n.contains("decorators") && index < n["decorators"].size() ? n["decorators"][index] : json();
}

bool BehaviorTreeDocument::SetDecoratorField(const BtPath& path, usize index, const std::string& key, const json& value, std::string* error,
                                             const std::string& merge_key) {
    return EditJson("Set decorator " + key, path, [&](json& n, std::string& why) {
        if (!n.contains("decorators") || index >= n["decorators"].size()) return why = "no such decorator", false;
        n["decorators"][index][key] = value;
        return true;
    }, error, merge_key);
}

bool BehaviorTreeDocument::AddService(const BtPath& path, BtServiceType type) {
    if (Node(path) == nullptr) return false;
    Edit("Add service", [&](BehaviorTreeAsset& t) {
        BtService s;
        s.type = type;
        if (type != BtServiceType::DistanceTo) s.event = "Update";
        Mutable(t, path)->services.push_back(std::move(s));
    });
    return true;
}

bool BehaviorTreeDocument::RemoveService(const BtPath& path, usize index) {
    const BtNode* n = Node(path);
    if (n == nullptr || index >= n->services.size()) return false;
    Edit("Remove service", [&](BehaviorTreeAsset& t) {
        auto& ss = Mutable(t, path)->services;
        ss.erase(ss.begin() + static_cast<std::ptrdiff_t>(index));
    });
    return true;
}

json BehaviorTreeDocument::ServiceJson(const BtPath& path, usize index) const {
    const json n = NodeJson(path);
    return n.contains("services") && index < n["services"].size() ? n["services"][index] : json();
}

bool BehaviorTreeDocument::SetServiceField(const BtPath& path, usize index, const std::string& key, const json& value, std::string* error,
                                           const std::string& merge_key) {
    return EditJson("Set service " + key, path, [&](json& n, std::string& why) {
        if (!n.contains("services") || index >= n["services"].size()) return why = "no such service", false;
        n["services"][index][key] = value;
        return true;
    }, error, merge_key);
}

std::string BehaviorTreeDocument::UniqueKeyName(const std::string& base) const {
    if (tree_.blackboard.Find(base) == nullptr) return base;
    for (int i = 1;; ++i) {
        const std::string name = base + std::to_string(i);
        if (tree_.blackboard.Find(name) == nullptr) return name;
    }
}

bool BehaviorTreeDocument::AddKey(const std::string& name, BlackboardType type, std::string* error) {
    if (name.empty()) return Fail(error, "a key needs a name");
    if (tree_.blackboard.Find(name) != nullptr) return Fail(error, "there's already a key '" + name + "'");
    Edit("Add key", [&](BehaviorTreeAsset& t) { t.blackboard.Add(name, type); });
    return true;
}

bool BehaviorTreeDocument::RemoveKey(const std::string& name) {
    if (tree_.blackboard.Find(name) == nullptr) return false;
    Edit("Remove key", [&](BehaviorTreeAsset& t) {
        auto& ks = t.blackboard.keys;
        ks.erase(std::remove_if(ks.begin(), ks.end(), [&](const BlackboardKey& k) { return k.name == name; }), ks.end());
    });
    return true;
}

namespace {
void EachNode(BtNode& n, const std::function<void(BtNode&)>& fn) {
    fn(n);
    for (BtNode& c : n.children) EachNode(c, fn);
}
void EachNodePath(const BtNode& n, BtPath& path, const std::function<void(const BtNode&, const BtPath&)>& fn) {
    fn(n, path);
    for (usize i = 0; i < n.children.size(); ++i) {
        path.push_back(i);
        EachNodePath(n.children[i], path, fn);
        path.pop_back();
    }
}
} // namespace

bool BehaviorTreeDocument::RenameKey(const std::string& from, const std::string& to, std::string* error) {
    if (tree_.blackboard.Find(from) == nullptr) return Fail(error, "no key '" + from + "'");
    if (to.empty()) return Fail(error, "a key needs a name");
    if (from == to) return true;
    if (tree_.blackboard.Find(to) != nullptr) return Fail(error, "there's already a key '" + to + "'");
    Edit("Rename key", [&](BehaviorTreeAsset& t) {
        for (BlackboardKey& k : t.blackboard.keys)
            if (k.name == from) k.name = to;
        EachNode(t.root, [&](BtNode& n) {
            if (n.key == from) n.key = to;
            for (BtDecorator& d : n.decorators)
                if (d.key == from) d.key = to;
            for (BtService& s : n.services) {
                if (s.key == from) s.key = to;
                if (s.out_key == from) s.out_key = to;
            }
        });
    });
    return true;
}

bool BehaviorTreeDocument::SetKeyType(const std::string& name, BlackboardType type) {
    const BlackboardKey* k = tree_.blackboard.Find(name);
    if (k == nullptr) return false;
    if (k->type == type) return true;
    Edit("Change key type", [&](BehaviorTreeAsset& t) {
        for (BlackboardKey& key : t.blackboard.keys) {
            if (key.name != name) continue;
            key.type = type;
            key.initial = {};
        }
        EachNode(t.root, [&](BtNode& n) {
            if (n.key == name && !ValueFits(n.value, type)) n.value = {};
            for (BtDecorator& d : n.decorators)
                if (d.key == name && !ValueFits(d.value, type)) d.value = {};
        });
    });
    return true;
}

bool BehaviorTreeDocument::SetKeyInitial(const std::string& name, const json& value, std::string* error) {
    const BlackboardKey* k = tree_.blackboard.Find(name);
    if (k == nullptr) return Fail(error, "no key '" + name + "'");
    BlackboardValue v;
    if (!ValueFromJson(value, k->type, v, error)) return false;
    Edit("Set initial value", [&](BehaviorTreeAsset& t) {
        for (BlackboardKey& key : t.blackboard.keys)
            if (key.name == name) key.initial = v;
    }, "initial:" + name);
    return true;
}

bool BehaviorTreeDocument::SetKeyDescription(const std::string& name, const std::string& text) {
    if (tree_.blackboard.Find(name) == nullptr) return false;
    Edit("Set description", [&](BehaviorTreeAsset& t) {
        for (BlackboardKey& key : t.blackboard.keys)
            if (key.name == name) key.description = text;
    }, "description:" + name);
    return true;
}

std::vector<BtPath> BehaviorTreeDocument::KeyUsers(const std::string& name) const {
    std::vector<BtPath> out;
    BtPath path;
    EachNodePath(tree_.root, path, [&](const BtNode& n, const BtPath& p) {
        bool uses = n.key == name;
        for (const BtDecorator& d : n.decorators) uses = uses || d.key == name;
        for (const BtService& s : n.services) uses = uses || s.key == name || s.out_key == name;
        if (uses) out.push_back(p);
    });
    return out;
}

const std::vector<BtProblem>& BehaviorTreeDocument::Diagnostics() const {
    if (diagnostics_revision_ != revision_) {
        diagnostics_ = ValidateBehaviorTree(tree_);
        diagnostics_revision_ = revision_;
    }
    return diagnostics_;
}

usize BehaviorTreeDocument::ErrorCount() const {
    const auto& ds = Diagnostics();
    return static_cast<usize>(std::count_if(ds.begin(), ds.end(), [](const BtProblem& p) { return !p.warning; }));
}

} // namespace aether::editor
