#pragma once

#include "aether/core/base.h"
#include "aether/math/quaternion.h"
#include "aether/math/vec.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace aether::reflect {
struct TypeInfo;
}

namespace aether::bp {

// Blueprint pin and variable types (Phase 12, docs/design/PHASE_SPECS.md
// §12.2; colors and shapes in docs/design/BLUEPRINT_NODES.md).

enum class PinKind : u8 { Exec, Data };
enum class PinDir : u8 { In, Out };

enum class ValueType : u8 {
    None,
    Bool,
    Int,   // i32
    Float, // f32
    String,
    Vec3,
    Quat,
    Entity,
    Struct,   // a reflected struct (`reflected` says which)
    Wildcard, // takes the type of whatever is connected
};

struct PinType {
    PinKind kind = PinKind::Data;
    ValueType type = ValueType::None;
    const reflect::TypeInfo* reflected = nullptr; // Struct only
    bool is_array = false;

    static PinType Exec() { return {PinKind::Exec, ValueType::None, nullptr, false}; }
    static PinType Of(ValueType type) { return {PinKind::Data, type, nullptr, false}; }
    static PinType StructOf(const reflect::TypeInfo& type) { return {PinKind::Data, ValueType::Struct, &type, false}; }
    static PinType ArrayOf(PinType element) {
        element.is_array = true;
        return element;
    }

    bool IsExec() const { return kind == PinKind::Exec; }
    bool operator==(const PinType& o) const {
        return kind == o.kind && type == o.type && reflected == o.reflected && is_array == o.is_array;
    }
    bool operator!=(const PinType& o) const { return !(*this == o); }
};

// "exec", "bool", "int", "float", "string", "Vec3", "Quat", "Entity",
// "Wildcard", a reflected struct's declared name, or "Array<T>".
std::string TypeName(const PinType& type);
std::optional<PinType> ParseType(std::string_view name);
// The pin type a reflected C++ type appears as (f32 -> float, Vec3, a
// reflected struct, std::vector<T> -> Array<T>), or nullopt if it can't be
// used in a Blueprint.
std::optional<PinType> PinTypeOf(const reflect::TypeInfo& type);

// A literal value: a pin's default or a variable's. Entities, structs and
// arrays have no literal form (their default is "none"/empty).
using Value = std::variant<std::monostate, bool, i32, f32, std::string, Vec3, Quaternion>;

Value DefaultValue(const PinType& type);
nlohmann::json ValueToJson(const Value& value);
// Reads a value of `type`; false if `json` doesn't hold one (numbers
// convert between int and float).
bool ValueFromJson(const nlohmann::json& json, const PinType& type, Value& out);
std::string ValueText(const Value& value); // for messages: "false", "1.5", "\"hi\"", "(0, 1, 0)"

// Whether an output of type `from` can feed an input of type `to`.
enum class Compat : u8 {
    No,
    Yes,
    Convert,      // an implicit conversion node is inserted (int -> float, anything -> string)
    ConvertLossy, // float -> int: allowed with warning BP104
};
Compat CanConnect(const PinType& from, const PinType& to);

} // namespace aether::bp
