#include "aether/ai/behavior_tree.h"

#include <set>

namespace aether::ai {

using nlohmann::json;

namespace {

constexpr const char* kNodeNames[] = {"Selector", "Sequence", "Parallel", "Wait", "MoveTo", "SetBlackboard", "ClearBlackboard",
                                      "RunBlueprint", "RunLuau", "Log", "Succeed", "Fail"};
constexpr const char* kDecoratorNames[] = {"BlackboardCondition", "Cooldown", "Loop", "TimeLimit", "Inverter", "ForceSuccess", "ForceFailure"};
constexpr const char* kAbortNames[] = {"None", "Self", "LowerPriority", "Both"};
constexpr const char* kCompareNames[] = {"IsSet", "IsNotSet", "Equal", "NotEqual", "Less", "LessEqual", "Greater", "GreaterEqual"};
constexpr const char* kServiceNames[] = {"Blueprint", "Luau", "DistanceTo"};

template <typename E, usize N>
bool FromName(const char* const (&names)[N], const std::string& name, E& out) {
    for (usize i = 0; i < N; ++i) {
        if (name == names[i]) {
            out = static_cast<E>(i);
            return true;
        }
    }
    return false;
}

bool Fail(std::string* error, const std::string& m) {
    if (error != nullptr) *error = m;
    return false;
}

BlackboardType KeyType(const BlackboardAsset& bb, const std::string& key, bool& found) {
    const BlackboardKey* k = bb.Find(key);
    found = k != nullptr;
    return k != nullptr ? k->type : BlackboardType::Bool;
}

json SaveDecorator(const BtDecorator& d) {
    json j = {{"type", kDecoratorNames[static_cast<usize>(d.type)]}};
    switch (d.type) {
    case BtDecoratorType::BlackboardCondition:
        j["key"] = d.key;
        j["op"] = kCompareNames[static_cast<usize>(d.op)];
        if (!std::holds_alternative<std::monostate>(d.value)) j["value"] = ValueToJson(d.value);
        if (d.abort != BtAbort::None) j["abort"] = kAbortNames[static_cast<usize>(d.abort)];
        break;
    case BtDecoratorType::Cooldown:
    case BtDecoratorType::TimeLimit: j["seconds"] = d.seconds; break;
    case BtDecoratorType::Loop: j["count"] = d.count; break;
    default: break;
    }
    return j;
}

json SaveNode(const BtNode& n) {
    json j = {{"type", BtNodeTypeName(n.type)}};
    if (!n.name.empty()) j["name"] = n.name;
    if (!n.decorators.empty()) {
        json& ds = j["decorators"] = json::array();
        for (const BtDecorator& d : n.decorators) ds.push_back(SaveDecorator(d));
    }
    if (!n.services.empty()) {
        json& ss = j["services"] = json::array();
        for (const BtService& s : n.services) {
            json sj = {{"type", kServiceNames[static_cast<usize>(s.type)]}, {"interval", s.interval}};
            if (!s.event.empty()) sj["event"] = s.event;
            if (!s.key.empty()) sj["key"] = s.key;
            if (!s.out_key.empty()) sj["out_key"] = s.out_key;
            ss.push_back(sj);
        }
    }
    switch (n.type) {
    case BtNodeType::Parallel:
        if (n.succeed_on_one) j["succeed_on_one"] = true;
        break;
    case BtNodeType::Wait:
        j["seconds"] = n.seconds;
        if (n.deviation != 0.0f) j["deviation"] = n.deviation;
        break;
    case BtNodeType::MoveTo:
        j["key"] = n.key;
        j["acceptance"] = n.acceptance;
        break;
    case BtNodeType::SetBlackboard:
        j["key"] = n.key;
        j["value"] = ValueToJson(n.value);
        break;
    case BtNodeType::ClearBlackboard: j["key"] = n.key; break;
    case BtNodeType::RunBlueprint:
    case BtNodeType::RunLuau:
    case BtNodeType::Log: j["event"] = n.event; break;
    default: break;
    }
    if (IsComposite(n.type)) {
        json& cs = j["children"] = json::array();
        for (const BtNode& c : n.children) cs.push_back(SaveNode(c));
    }
    return j;
}

bool LoadValue(const json& j, const BlackboardAsset& bb, const std::string& key, BlackboardValue& out, const std::string& where, std::string* error) {
    if (!j.contains("value")) return true;
    bool found = false;
    const BlackboardType type = KeyType(bb, key, found);
    if (!found) return Fail(error, where + ": the blackboard has no key '" + key + "'");
    std::string why;
    if (!ValueFromJson(j["value"], type, out, &why)) return Fail(error, where + ": value: " + why);
    return true;
}

bool LoadNode(const json& j, const BlackboardAsset& bb, BtNode& out, const std::string& where, std::string* error) {
    if (!j.is_object()) return Fail(error, where + ": a node is an object");
    if (!FromName(kNodeNames, j.value("type", std::string()), out.type)) return Fail(error, where + ": unknown node type '" + j.value("type", std::string()) + "'");
    const std::string here = where + "/" + std::string(BtNodeTypeName(out.type));
    try {
        out.name = j.value("name", std::string());
        if (j.contains("decorators")) {
            for (const json& dj : j.at("decorators")) {
                BtDecorator d;
                if (!FromName(kDecoratorNames, dj.value("type", std::string()), d.type))
                    return Fail(error, here + ": unknown decorator '" + dj.value("type", std::string()) + "'");
                d.key = dj.value("key", std::string());
                if (dj.contains("op") && !FromName(kCompareNames, dj.at("op").get<std::string>(), d.op))
                    return Fail(error, here + ": unknown comparison '" + dj.at("op").get<std::string>() + "'");
                if (dj.contains("abort") && !FromName(kAbortNames, dj.at("abort").get<std::string>(), d.abort))
                    return Fail(error, here + ": unknown abort '" + dj.at("abort").get<std::string>() + "'");
                d.seconds = dj.value("seconds", 1.0f);
                d.count = dj.value("count", 0);
                if (!LoadValue(dj, bb, d.key, d.value, here, error)) return false;
                out.decorators.push_back(std::move(d));
            }
        }
        if (j.contains("services")) {
            for (const json& sj : j.at("services")) {
                BtService s;
                if (!FromName(kServiceNames, sj.value("type", std::string()), s.type))
                    return Fail(error, here + ": unknown service '" + sj.value("type", std::string()) + "'");
                s.interval = sj.value("interval", 0.5f);
                s.event = sj.value("event", std::string());
                s.key = sj.value("key", std::string());
                s.out_key = sj.value("out_key", std::string());
                out.services.push_back(std::move(s));
            }
        }
        out.succeed_on_one = j.value("succeed_on_one", false);
        out.seconds = j.value("seconds", 1.0f);
        out.deviation = j.value("deviation", 0.0f);
        out.key = j.value("key", std::string());
        out.acceptance = j.value("acceptance", 0.5f);
        out.event = j.value("event", std::string());
        if (out.type == BtNodeType::SetBlackboard && !LoadValue(j, bb, out.key, out.value, here, error)) return false;
        if (j.contains("children")) {
            const json& cs = j.at("children");
            if (!cs.is_array()) return Fail(error, here + ": children is a list");
            for (usize i = 0; i < cs.size(); ++i) {
                BtNode c;
                if (!LoadNode(cs[i], bb, c, here + "[" + std::to_string(i) + "]", error)) return false;
                out.children.push_back(std::move(c));
            }
        }
    } catch (const json::exception& e) {
        return Fail(error, here + ": " + e.what());
    }
    return true;
}

struct Checker {
    const BehaviorTreeAsset& tree;
    std::vector<BtProblem> problems;

    void Add(const char* code, const std::string& path, const std::string& m, bool warning = false) { problems.push_back({code, path, m, warning}); }

    const BlackboardKey* Key(const std::string& key, const std::string& path, const char* what) {
        const BlackboardKey* k = tree.blackboard.Find(key);
        if (k == nullptr) Add("BT003", path, std::string(what) + " uses the key '" + key + "', which the blackboard doesn't have");
        return k;
    }
    void Place(const std::string& key, const std::string& path, const char* what) {
        if (const BlackboardKey* k = Key(key, path, what); k != nullptr && k->type != BlackboardType::Vector && k->type != BlackboardType::Entity)
            Add("BT009", path, std::string(what) + ": '" + key + "' is a " + BlackboardTypeName(k->type) + "; it needs a Vector or an Entity");
    }

    void Node(const BtNode& n, const BtNode* parent, const std::string& path) {
        const bool composite = IsComposite(n.type);
        if (composite && n.children.empty()) Add("BT001", path, std::string(BtNodeTypeName(n.type)) + " has no children");
        if (!composite && !n.children.empty()) Add("BT002", path, "a task can't have children");
        if (n.type == BtNodeType::Parallel && n.children.size() == 1) Add("BT005", path, "a Parallel with one child runs it alone", true);
        for (const BtDecorator& d : n.decorators) {
            if (d.type == BtDecoratorType::BlackboardCondition) {
                if (const BlackboardKey* k = Key(d.key, path, "a condition")) {
                    const bool number = k->type == BlackboardType::Int || k->type == BlackboardType::Float;
                    const bool ordered = d.op == BtCompare::Less || d.op == BtCompare::LessEqual || d.op == BtCompare::Greater || d.op == BtCompare::GreaterEqual;
                    const bool valued = d.op != BtCompare::IsSet && d.op != BtCompare::IsNotSet;
                    if (ordered && !number) Add("BT004", path, std::string("'") + kCompareNames[static_cast<usize>(d.op)] + "' needs a number, and '" + d.key + "' is a " + BlackboardTypeName(k->type));
                    else if (valued && k->type != BlackboardType::Entity && std::holds_alternative<std::monostate>(d.value))
                        Add("BT004", path, "the condition on '" + d.key + "' has no value to compare with");
                    else if (!ValueFits(d.value, k->type)) Add("BT004", path, "the condition's value doesn't fit '" + d.key + "' (a " + BlackboardTypeName(k->type) + ")");
                }
                if ((d.abort == BtAbort::LowerPriority || d.abort == BtAbort::Both) && (parent == nullptr || parent->type != BtNodeType::Selector))
                    Add("BT006", path, "aborting lower priorities only works under a Selector", true);
            }
            if ((d.type == BtDecoratorType::Cooldown || d.type == BtDecoratorType::TimeLimit) && d.seconds < 0.0f) Add("BT007", path, "a time can't be negative");
            if (d.type == BtDecoratorType::Loop && d.count < 0) Add("BT007", path, "a loop count can't be negative");
        }
        for (const BtService& s : n.services) {
            if (s.interval <= 0.0f) Add("BT007", path, "a service's interval must be more than 0");
            if ((s.type == BtServiceType::Blueprint || s.type == BtServiceType::Luau) && s.event.empty()) Add("BT008", path, "the service has no event to run");
            if (s.type == BtServiceType::DistanceTo) {
                Place(s.key, path, "Distance To");
                if (const BlackboardKey* k = Key(s.out_key, path, "Distance To"); k != nullptr && k->type != BlackboardType::Float)
                    Add("BT004", path, "Distance To writes a Float; '" + s.out_key + "' is a " + BlackboardTypeName(k->type));
            }
        }
        switch (n.type) {
        case BtNodeType::Wait:
            if (n.seconds < 0.0f || n.deviation < 0.0f) Add("BT007", path, "a wait can't be negative");
            break;
        case BtNodeType::MoveTo: Place(n.key, path, "Move To"); break;
        case BtNodeType::SetBlackboard:
            if (const BlackboardKey* k = Key(n.key, path, "Set Blackboard"); k != nullptr && !ValueFits(n.value, k->type))
                Add("BT004", path, "the value doesn't fit '" + n.key + "' (a " + BlackboardTypeName(k->type) + ")");
            break;
        case BtNodeType::ClearBlackboard: Key(n.key, path, "Clear Blackboard"); break;
        case BtNodeType::RunBlueprint:
        case BtNodeType::RunLuau:
            if (n.event.empty()) Add("BT008", path, "the task has no event to run");
            break;
        default: break;
        }
        for (usize i = 0; i < n.children.size(); ++i) Node(n.children[i], &n, path + "/" + n.children[i].Label() + "[" + std::to_string(i) + "]");
    }
};

} // namespace

const char* BtNodeTypeName(BtNodeType type) { return kNodeNames[static_cast<usize>(type)]; }
bool IsComposite(BtNodeType type) { return type == BtNodeType::Selector || type == BtNodeType::Sequence || type == BtNodeType::Parallel; }

json SaveBehaviorTree(const BehaviorTreeAsset& tree) {
    json keys = json::array();
    for (const BlackboardKey& k : tree.blackboard.keys) {
        json kj = {{"name", k.name}, {"type", BlackboardTypeName(k.type)}};
        if (!std::holds_alternative<std::monostate>(k.initial)) kj["initial"] = ValueToJson(k.initial);
        if (!k.description.empty()) kj["description"] = k.description;
        keys.push_back(kj);
    }
    return {{"version", 1}, {"name", tree.name}, {"blackboard", keys}, {"root", SaveNode(tree.root)}};
}

bool LoadBehaviorTree(const json& j, BehaviorTreeAsset& out, std::string* error) {
    if (!j.is_object() || !j.contains("root")) return Fail(error, "not a behavior tree (no root)");
    if (j.value("version", 1) > 1) return Fail(error, "made by a newer version");
    BehaviorTreeAsset t;
    t.name = j.value("name", std::string());
    if (j.contains("blackboard")) {
        for (const json& kj : j.at("blackboard")) {
            BlackboardKey k;
            k.name = kj.value("name", std::string());
            if (!BlackboardTypeFromName(kj.value("type", std::string()), k.type)) return Fail(error, "blackboard key '" + k.name + "': unknown type '" + kj.value("type", std::string()) + "'");
            std::string why;
            if (kj.contains("initial") && !ValueFromJson(kj.at("initial"), k.type, k.initial, &why)) return Fail(error, "blackboard key '" + k.name + "': " + why);
            k.description = kj.value("description", std::string());
            t.blackboard.keys.push_back(std::move(k));
        }
    }
    if (!LoadNode(j.at("root"), t.blackboard, t.root, "root", error)) return false;
    out = std::move(t);
    return true;
}

std::vector<BtProblem> ValidateBehaviorTree(const BehaviorTreeAsset& tree) {
    Checker c{tree, {}};
    std::set<std::string> names;
    for (const BlackboardKey& k : tree.blackboard.keys) {
        if (!names.insert(k.name).second) c.Add("BT010", "blackboard", "the key '" + k.name + "' is declared twice");
    }
    c.Node(tree.root, nullptr, "root");
    return c.problems;
}

} // namespace aether::ai
