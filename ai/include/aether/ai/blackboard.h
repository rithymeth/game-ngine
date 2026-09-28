#pragma once

#include "aether/core/base.h"
#include "aether/ecs/entity.h"
#include "aether/math/vec.h"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <variant>
#include <vector>

namespace aether::ai {

// Blackboards (Phase 20 step 4, docs/design/PHASE_SPECS.md §20.4): the
// typed memory a Behavior Tree reads and writes - what the AI knows (its
// target, where it heard a noise, whether it's alarmed).

enum class BlackboardType : u8 { Bool, Int, Float, String, Vector, Entity };
const char* BlackboardTypeName(BlackboardType type);
bool BlackboardTypeFromName(const std::string& name, BlackboardType& out);

// Unset is std::monostate.
using BlackboardValue = std::variant<std::monostate, bool, i32, f32, std::string, Vec3, Entity>;
bool ValueFits(const BlackboardValue& value, BlackboardType type); // unset fits any
bool ValuesEqual(const BlackboardValue& a, const BlackboardValue& b);
std::string ValueToString(const BlackboardValue& value);
// JSON: a bool, a number, a string or [x, y, z]; entities aren't saved. Null is unset.
nlohmann::json ValueToJson(const BlackboardValue& value);
bool ValueFromJson(const nlohmann::json& json, BlackboardType type, BlackboardValue& out, std::string* error = nullptr);

struct BlackboardKey {
    std::string name;
    BlackboardType type = BlackboardType::Bool;
    BlackboardValue initial; // unset by default
    std::string description;
};

// The keys a tree's blackboard has.
struct BlackboardAsset {
    std::vector<BlackboardKey> keys;
    const BlackboardKey* Find(const std::string& name) const;
    void Add(const std::string& name, BlackboardType type, BlackboardValue initial = {});
};

class Blackboard {
public:
    Blackboard() = default;
    explicit Blackboard(const BlackboardAsset& asset) { Adopt(asset); }

    // Takes on `asset`'s keys: values already set that fit stay (set before
    // the tree started), the rest start at their initial values. Without
    // keys, any name can be set (ad hoc).
    void Adopt(const BlackboardAsset& asset);
    bool HasKeys() const { return !keys_.empty(); }

    // False for an undeclared key or a value of the wrong type.
    bool Set(const std::string& key, const BlackboardValue& value);
    bool Clear(const std::string& key);
    const BlackboardValue* Get(const std::string& key) const; // null when unknown or unset
    bool IsSet(const std::string& key) const { return Get(key) != nullptr; }
    template <typename T>
    T GetAs(const std::string& key, T fallback = {}) const {
        const BlackboardValue* v = Get(key);
        if (v == nullptr) return fallback;
        if (const T* t = std::get_if<T>(v)) return *t;
        return fallback;
    }
    // A number either way (ints and floats).
    bool GetNumber(const std::string& key, f64& out) const;
    const BlackboardKey* Key(const std::string& key) const;

    // Goes up on every change; per key too (conditions watch it).
    u64 Revision() const { return revision_; }
    u64 Revision(const std::string& key) const;
    std::vector<std::string> Names() const; // set keys, sorted
    usize Count() const { return values_.size(); }

private:
    struct Entry {
        BlackboardValue value;
        u64 revision = 0;
    };
    std::vector<BlackboardKey> keys_;
    std::map<std::string, Entry> values_;
    std::map<std::string, u64> cleared_; // revisions of keys that were cleared
    u64 revision_ = 0;
};

} // namespace aether::ai
