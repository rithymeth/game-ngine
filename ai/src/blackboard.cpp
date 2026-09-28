#include "aether/ai/blackboard.h"

#include <algorithm>
#include <cstdio>

namespace aether::ai {

namespace {
constexpr const char* kTypeNames[] = {"Bool", "Int", "Float", "String", "Vector", "Entity"};
bool Fail(std::string* error, const std::string& m) {
    if (error != nullptr) *error = m;
    return false;
}
} // namespace

const char* BlackboardTypeName(BlackboardType type) { return kTypeNames[static_cast<usize>(type)]; }

bool BlackboardTypeFromName(const std::string& name, BlackboardType& out) {
    for (usize i = 0; i < std::size(kTypeNames); ++i) {
        if (name == kTypeNames[i]) {
            out = static_cast<BlackboardType>(i);
            return true;
        }
    }
    return false;
}

bool ValueFits(const BlackboardValue& v, BlackboardType type) {
    switch (type) {
    case BlackboardType::Bool: return std::holds_alternative<std::monostate>(v) || std::holds_alternative<bool>(v);
    case BlackboardType::Int: return std::holds_alternative<std::monostate>(v) || std::holds_alternative<i32>(v);
    case BlackboardType::Float: return std::holds_alternative<std::monostate>(v) || std::holds_alternative<f32>(v);
    case BlackboardType::String: return std::holds_alternative<std::monostate>(v) || std::holds_alternative<std::string>(v);
    case BlackboardType::Vector: return std::holds_alternative<std::monostate>(v) || std::holds_alternative<Vec3>(v);
    case BlackboardType::Entity: return std::holds_alternative<std::monostate>(v) || std::holds_alternative<Entity>(v);
    }
    return false;
}

bool ValuesEqual(const BlackboardValue& a, const BlackboardValue& b) {
    if (a.index() != b.index()) return false;
    if (const Vec3* p = std::get_if<Vec3>(&a)) {
        const Vec3& q = std::get<Vec3>(b);
        return p->x == q.x && p->y == q.y && p->z == q.z;
    }
    return std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, Vec3>) return false;
            else return x == std::get<T>(b);
        },
        a);
}

std::string ValueToString(const BlackboardValue& v) {
    char buf[96];
    if (std::holds_alternative<std::monostate>(v)) return "(unset)";
    if (const bool* b = std::get_if<bool>(&v)) return *b ? "true" : "false";
    if (const i32* i = std::get_if<i32>(&v)) return std::to_string(*i);
    if (const f32* f = std::get_if<f32>(&v)) {
        std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(*f));
        return buf;
    }
    if (const std::string* s = std::get_if<std::string>(&v)) return *s;
    if (const Vec3* p = std::get_if<Vec3>(&v)) {
        std::snprintf(buf, sizeof(buf), "(%g, %g, %g)", static_cast<double>(p->x), static_cast<double>(p->y), static_cast<double>(p->z));
        return buf;
    }
    const Entity e = std::get<Entity>(v);
    return e.IsNull() ? "(no entity)" : "entity " + std::to_string(e.index);
}

nlohmann::json ValueToJson(const BlackboardValue& v) {
    if (const bool* b = std::get_if<bool>(&v)) return *b;
    if (const i32* i = std::get_if<i32>(&v)) return *i;
    if (const f32* f = std::get_if<f32>(&v)) return *f;
    if (const std::string* s = std::get_if<std::string>(&v)) return *s;
    if (const Vec3* p = std::get_if<Vec3>(&v)) return nlohmann::json::array({p->x, p->y, p->z});
    return nullptr;
}

bool ValueFromJson(const nlohmann::json& j, BlackboardType type, BlackboardValue& out, std::string* error) {
    if (j.is_null()) {
        out = std::monostate{};
        return true;
    }
    switch (type) {
    case BlackboardType::Bool:
        if (!j.is_boolean()) return Fail(error, "a Bool is true or false");
        out = j.get<bool>();
        return true;
    case BlackboardType::Int:
        if (!j.is_number_integer()) return Fail(error, "an Int is a whole number");
        out = j.get<i32>();
        return true;
    case BlackboardType::Float:
        if (!j.is_number()) return Fail(error, "a Float is a number");
        out = j.get<f32>();
        return true;
    case BlackboardType::String:
        if (!j.is_string()) return Fail(error, "a String is text");
        out = j.get<std::string>();
        return true;
    case BlackboardType::Vector:
        if (!j.is_array() || j.size() != 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number()) return Fail(error, "a Vector is [x, y, z]");
        out = Vec3(j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>());
        return true;
    case BlackboardType::Entity: return Fail(error, "an Entity can't have a saved value");
    }
    return false;
}

const BlackboardKey* BlackboardAsset::Find(const std::string& name) const {
    for (const BlackboardKey& k : keys)
        if (k.name == name) return &k;
    return nullptr;
}

void BlackboardAsset::Add(const std::string& name, BlackboardType type, BlackboardValue initial) {
    keys.push_back({name, type, std::move(initial), {}});
}

void Blackboard::Adopt(const BlackboardAsset& asset) {
    keys_ = asset.keys;
    std::map<std::string, Entry> kept;
    for (const BlackboardKey& k : keys_) {
        auto it = values_.find(k.name);
        if (it != values_.end() && ValueFits(it->second.value, k.type)) {
            kept[k.name] = it->second;
        } else if (!std::holds_alternative<std::monostate>(k.initial)) {
            kept[k.name] = {k.initial, ++revision_};
        }
    }
    values_ = std::move(kept);
    ++revision_;
}

const BlackboardKey* Blackboard::Key(const std::string& key) const {
    for (const BlackboardKey& k : keys_)
        if (k.name == key) return &k;
    return nullptr;
}

bool Blackboard::Set(const std::string& key, const BlackboardValue& value) {
    if (std::holds_alternative<std::monostate>(value)) return Clear(key);
    if (!keys_.empty()) {
        const BlackboardKey* k = Key(key);
        if (k == nullptr || !ValueFits(value, k->type)) return false;
    }
    Entry& e = values_[key];
    if (e.revision != 0 && ValuesEqual(e.value, value)) return true; // unchanged: conditions don't re-run
    e.value = value;
    e.revision = ++revision_;
    return true;
}

bool Blackboard::Clear(const std::string& key) {
    if (!keys_.empty() && Key(key) == nullptr) return false;
    auto it = values_.find(key);
    if (it == values_.end()) return true;
    values_.erase(it);
    cleared_[key] = ++revision_;
    return true;
}

const BlackboardValue* Blackboard::Get(const std::string& key) const {
    auto it = values_.find(key);
    return it == values_.end() ? nullptr : &it->second.value;
}

bool Blackboard::GetNumber(const std::string& key, f64& out) const {
    const BlackboardValue* v = Get(key);
    if (v == nullptr) return false;
    if (const i32* i = std::get_if<i32>(v)) return out = *i, true;
    if (const f32* f = std::get_if<f32>(v)) return out = *f, true;
    return false;
}

u64 Blackboard::Revision(const std::string& key) const {
    auto it = values_.find(key);
    if (it != values_.end()) return it->second.revision;
    auto c = cleared_.find(key);
    return c == cleared_.end() ? 0 : c->second;
}

std::vector<std::string> Blackboard::Names() const {
    std::vector<std::string> out;
    for (const auto& [k, e] : values_) out.push_back(k);
    return out;
}

} // namespace aether::ai
