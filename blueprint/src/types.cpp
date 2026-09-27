#include "aether/blueprint/types.h"

#include "aether/reflection/reflection.h"

#include <cstdio>

namespace aether::bp {

using nlohmann::json;

std::string TypeName(const PinType& type) {
    if (type.IsExec()) {
        return "exec";
    }
    std::string name;
    switch (type.type) {
    case ValueType::None: name = "none"; break;
    case ValueType::Bool: name = "bool"; break;
    case ValueType::Int: name = "int"; break;
    case ValueType::Float: name = "float"; break;
    case ValueType::String: name = "string"; break;
    case ValueType::Vec3: name = "Vec3"; break;
    case ValueType::Quat: name = "Quat"; break;
    case ValueType::Entity: name = "Entity"; break;
    case ValueType::Wildcard: name = "Wildcard"; break;
    case ValueType::Struct: name = type.reflected != nullptr ? type.reflected->name : "struct"; break;
    }
    return type.is_array ? "Array<" + name + ">" : name;
}

std::optional<PinType> ParseType(std::string_view name) {
    if (name.size() > 7 && name.substr(0, 6) == "Array<" && name.back() == '>') {
        std::optional<PinType> element = ParseType(name.substr(6, name.size() - 7));
        if (!element || element->IsExec() || element->is_array) {
            return std::nullopt;
        }
        return PinType::ArrayOf(*element);
    }
    if (name == "exec") return PinType::Exec();
    if (name == "bool") return PinType::Of(ValueType::Bool);
    if (name == "int") return PinType::Of(ValueType::Int);
    if (name == "float") return PinType::Of(ValueType::Float);
    if (name == "string") return PinType::Of(ValueType::String);
    if (name == "Vec3") return PinType::Of(ValueType::Vec3);
    if (name == "Quat") return PinType::Of(ValueType::Quat);
    if (name == "Entity") return PinType::Of(ValueType::Entity);
    if (name == "Wildcard") return PinType::Of(ValueType::Wildcard);
    if (const reflect::TypeInfo* type = reflect::TypeRegistry::Find(name)) {
        if (type->kind == reflect::TypeKind::Struct) {
            return PinType::StructOf(*type);
        }
    }
    return std::nullopt;
}

std::optional<PinType> PinTypeOf(const reflect::TypeInfo& type) {
    using reflect::Reflect;
    if (&type == &Reflect<bool>()) return PinType::Of(ValueType::Bool);
    if (&type == &Reflect<i32>()) return PinType::Of(ValueType::Int);
    if (&type == &Reflect<f32>()) return PinType::Of(ValueType::Float);
    if (&type == &Reflect<std::string>()) return PinType::Of(ValueType::String);
    if (&type == &Reflect<Vec3>()) return PinType::Of(ValueType::Vec3);
    if (&type == &Reflect<Quaternion>()) return PinType::Of(ValueType::Quat);
    if (&type == &Reflect<Entity>()) return PinType::Of(ValueType::Entity);
    if (type.kind == reflect::TypeKind::Array && type.element != nullptr) {
        std::optional<PinType> element = PinTypeOf(*type.element);
        if (element && !element->is_array) {
            return PinType::ArrayOf(*element);
        }
        return std::nullopt;
    }
    if (type.kind == reflect::TypeKind::Struct) {
        return PinType::StructOf(type);
    }
    return std::nullopt;
}

Value DefaultValue(const PinType& type) {
    if (type.IsExec() || type.is_array) {
        return std::monostate{};
    }
    switch (type.type) {
    case ValueType::Bool: return false;
    case ValueType::Int: return i32{0};
    case ValueType::Float: return 0.0f;
    case ValueType::String: return std::string();
    case ValueType::Vec3: return Vec3{};
    case ValueType::Quat: return Quaternion{};
    default: return std::monostate{};
    }
}

json ValueToJson(const Value& value) {
    if (const bool* b = std::get_if<bool>(&value)) return *b;
    if (const i32* i = std::get_if<i32>(&value)) return *i;
    if (const f32* f = std::get_if<f32>(&value)) return *f;
    if (const std::string* s = std::get_if<std::string>(&value)) return *s;
    if (const Vec3* v = std::get_if<Vec3>(&value)) return json::array({v->x, v->y, v->z});
    if (const Quaternion* q = std::get_if<Quaternion>(&value)) return json::array({q->x, q->y, q->z, q->w});
    return nullptr;
}

bool ValueFromJson(const json& j, const PinType& type, Value& out) {
    if (type.IsExec() || type.is_array) {
        return j.is_null();
    }
    auto numbers = [&](usize count, f32* dst) {
        if (!j.is_array() || j.size() != count) return false;
        for (usize i = 0; i < count; ++i) {
            if (!j[i].is_number()) return false;
            dst[i] = j[i].get<f32>();
        }
        return true;
    };
    switch (type.type) {
    case ValueType::Bool:
        if (!j.is_boolean()) return false;
        out = j.get<bool>();
        return true;
    case ValueType::Int:
        if (!j.is_number()) return false;
        out = j.is_number_integer() ? j.get<i32>() : static_cast<i32>(j.get<f64>());
        return true;
    case ValueType::Float:
        if (!j.is_number()) return false;
        out = j.get<f32>();
        return true;
    case ValueType::String:
        if (!j.is_string()) return false;
        out = j.get<std::string>();
        return true;
    case ValueType::Vec3: {
        f32 v[3];
        if (!numbers(3, v)) return false;
        out = Vec3{v[0], v[1], v[2]};
        return true;
    }
    case ValueType::Quat: {
        f32 q[4];
        if (!numbers(4, q)) return false;
        out = Quaternion{q[0], q[1], q[2], q[3]};
        return true;
    }
    default:
        out = std::monostate{};
        return j.is_null();
    }
}

std::string ValueText(const Value& value) {
    char buffer[96];
    if (const bool* b = std::get_if<bool>(&value)) return *b ? "true" : "false";
    if (const i32* i = std::get_if<i32>(&value)) return std::to_string(*i);
    if (const f32* f = std::get_if<f32>(&value)) {
        std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(*f));
        return buffer;
    }
    if (const std::string* s = std::get_if<std::string>(&value)) return "\"" + *s + "\"";
    if (const Vec3* v = std::get_if<Vec3>(&value)) {
        std::snprintf(buffer, sizeof(buffer), "(%g, %g, %g)", static_cast<double>(v->x), static_cast<double>(v->y),
                      static_cast<double>(v->z));
        return buffer;
    }
    if (const Quaternion* q = std::get_if<Quaternion>(&value)) {
        std::snprintf(buffer, sizeof(buffer), "(%g, %g, %g, %g)", static_cast<double>(q->x),
                      static_cast<double>(q->y), static_cast<double>(q->z), static_cast<double>(q->w));
        return buffer;
    }
    return "none";
}

Compat CanConnect(const PinType& from, const PinType& to) {
    if (from.IsExec() || to.IsExec()) {
        return from.IsExec() && to.IsExec() ? Compat::Yes : Compat::No;
    }
    if (from.type == ValueType::Wildcard || to.type == ValueType::Wildcard) {
        return Compat::Yes; // resolved to the connected type
    }
    if (from == to) {
        return Compat::Yes;
    }
    if (from.is_array || to.is_array) {
        return Compat::No;
    }
    if (from.type == ValueType::Int && to.type == ValueType::Float) {
        return Compat::Convert;
    }
    if (from.type == ValueType::Float && to.type == ValueType::Int) {
        return Compat::ConvertLossy;
    }
    if (to.type == ValueType::String && from.type != ValueType::Struct) {
        return Compat::Convert; // bool, int, float, Vec3, Quat, Entity print as text
    }
    return Compat::No;
}

} // namespace aether::bp
