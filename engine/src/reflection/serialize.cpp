#include "aether/reflection/serialize.h"

#include "aether/reflection/converters.h"

#include "aether/core/log.h"

#include <charconv>
#include <locale>
#include <sstream>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace aether::reflect {

namespace {

// ---------------------------------------------------------------------------
// Migration registry
// ---------------------------------------------------------------------------

struct MigrationRegistry {
    std::mutex mutex;
    std::unordered_map<TypeId, MigrationFn> by_type;
};

MigrationRegistry& Migrations() {
    static MigrationRegistry registry;
    return registry;
}

MigrationFn FindMigration(const TypeInfo& type) {
    MigrationRegistry& registry = Migrations();
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto it = registry.by_type.find(type.id);
    return it != registry.by_type.end() ? it->second : nullptr;
}

// ---------------------------------------------------------------------------
// Custom JSON converters (converters.h)
// ---------------------------------------------------------------------------

struct ConverterRegistry {
    std::shared_mutex mutex; // read on every value saved/loaded, written rarely
    std::unordered_map<TypeId, std::pair<ToJsonConverter, FromJsonConverter>> by_type;
};

ConverterRegistry& Converters() {
    static ConverterRegistry registry;
    return registry;
}

const std::pair<ToJsonConverter, FromJsonConverter>* FindConverter(const TypeInfo& type) {
    // Only structs can have converters (see RegisterJsonConverter), so the
    // common scalar path never takes the lock.
    if (type.kind != TypeKind::Struct) {
        return nullptr;
    }
    ConverterRegistry& registry = Converters();
    std::shared_lock<std::shared_mutex> lock(registry.mutex);
    auto it = registry.by_type.find(type.id);
    return it != registry.by_type.end() ? &it->second : nullptr;
}

// ---------------------------------------------------------------------------
// Raw integer access by size (TypeKind::Int/UInt/Enum storage)
// ---------------------------------------------------------------------------

i64 ReadSigned(const void* ptr, u32 size) {
    switch (size) {
    case 1: { i8 v; std::memcpy(&v, ptr, 1); return v; }
    case 2: { i16 v; std::memcpy(&v, ptr, 2); return v; }
    case 4: { i32 v; std::memcpy(&v, ptr, 4); return v; }
    default: { i64 v; std::memcpy(&v, ptr, 8); return v; }
    }
}

u64 ReadUnsigned(const void* ptr, u32 size) {
    switch (size) {
    case 1: { u8 v; std::memcpy(&v, ptr, 1); return v; }
    case 2: { u16 v; std::memcpy(&v, ptr, 2); return v; }
    case 4: { u32 v; std::memcpy(&v, ptr, 4); return v; }
    default: { u64 v; std::memcpy(&v, ptr, 8); return v; }
    }
}

bool SignedFits(i64 value, u32 size) {
    if (size >= 8) return true;
    const i64 max = (i64{1} << (size * 8 - 1)) - 1;
    return value >= -max - 1 && value <= max;
}

bool UnsignedFits(u64 value, u32 size) {
    if (size >= 8) return true;
    return value <= (u64{1} << (size * 8)) - 1;
}

void WriteSigned(void* ptr, u32 size, i64 value) {
    switch (size) {
    case 1: { i8 v = static_cast<i8>(value); std::memcpy(ptr, &v, 1); break; }
    case 2: { i16 v = static_cast<i16>(value); std::memcpy(ptr, &v, 2); break; }
    case 4: { i32 v = static_cast<i32>(value); std::memcpy(ptr, &v, 4); break; }
    default: std::memcpy(ptr, &value, 8); break;
    }
}

void WriteUnsigned(void* ptr, u32 size, u64 value) {
    switch (size) {
    case 1: { u8 v = static_cast<u8>(value); std::memcpy(ptr, &v, 1); break; }
    case 2: { u16 v = static_cast<u16>(value); std::memcpy(ptr, &v, 2); break; }
    case 4: { u32 v = static_cast<u32>(value); std::memcpy(ptr, &v, 4); break; }
    default: std::memcpy(ptr, &value, 8); break;
    }
}

// The shortest decimal that reads back as exactly this f32, as a double, so
// the JSON shows 0.1 rather than 0.10000000149011612. Uses to_chars/
// from_chars, which are locale-independent (strtod is not).
double ShortestF32(f32 value) {
    if (!std::isfinite(value)) {
        return value; // written as null by the JSON encoder
    }
    char buffer[32];
    auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
    double result = value;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    std::from_chars(buffer, written.ptr, result);
#else
    // Apple's libc++ has no floating-point from_chars; a classic-locale
    // stream reads it back just as locale-independently.
    std::istringstream in(std::string(buffer, written.ptr));
    in.imbue(std::locale::classic());
    in >> result;
#endif
    return result;
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

Json SaveValue(const TypeInfo& type, const void* ptr) {
    if (const auto* converter = FindConverter(type)) {
        return converter->first(ptr);
    }
    switch (type.kind) {
    case TypeKind::Bool:
        return *static_cast<const bool*>(ptr);
    case TypeKind::Int:
        return ReadSigned(ptr, type.size);
    case TypeKind::UInt:
        return ReadUnsigned(ptr, type.size);
    case TypeKind::Float:
        return type.size == 4 ? ShortestF32(*static_cast<const f32*>(ptr)) : *static_cast<const f64*>(ptr);
    case TypeKind::String:
        return *static_cast<const std::string*>(ptr);
    case TypeKind::FixedString: {
        const char* chars = static_cast<const char*>(ptr);
        usize length = 0;
        while (length < type.size && chars[length] != '\0') {
            ++length;
        }
        return std::string(chars, length);
    }
    case TypeKind::Enum: {
        const TypeInfo& underlying = *type.underlying;
        i64 value = underlying.kind == TypeKind::UInt ? static_cast<i64>(ReadUnsigned(ptr, underlying.size))
                                                      : ReadSigned(ptr, underlying.size);
        if (const char* name = type.EnumName(value)) {
            return name;
        }
        return value; // a value with no named entry (e.g. combined flags)
    }
    case TypeKind::Array: {
        Json array = Json::array();
        for (usize i = 0, n = type.array_size(ptr); i < n; ++i) {
            array.push_back(SaveValue(*type.element, type.ArrayElement(ptr, i)));
        }
        return array;
    }
    case TypeKind::Struct: {
        if (type.serialize_as_array) {
            Json array = Json::array();
            for (const FieldInfo& field : type.fields) {
                array.push_back(SaveValue(*field.type, field.Ptr(ptr)));
            }
            return array;
        }
        Json object = Json::object();
        object["$v"] = type.version;
        for (const FieldInfo& field : type.fields) {
            if (!field.HasFlag(Field_Transient)) {
                object[field.name] = SaveValue(*field.type, field.Ptr(ptr));
            }
        }
        return object;
    }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

struct LoadContext {
    LoadReport* report;

    void Warn(const std::string& path, const std::string& message) {
        std::string text = path + ": " + message;
        if (report != nullptr) {
            report->warnings.push_back(std::move(text));
        } else {
            AETHER_LOG_WARN("Serialize", "%s", text.c_str());
        }
    }
};

const char* JsonTypeName(const Json& data) {
    return data.type_name();
}

// Returns false (after warning) when `data` has the wrong shape for `type`;
// the destination is left unchanged in that case.
bool LoadValue(const TypeInfo& type, void* ptr, const Json& data, LoadContext& ctx, const std::string& path);

bool LoadInteger(const TypeInfo& int_type, void* ptr, const Json& data, LoadContext& ctx, const std::string& path) {
    if (!data.is_number_integer()) {
        ctx.Warn(path, std::string("expected an integer, found ") + JsonTypeName(data));
        return false;
    }
    if (int_type.kind == TypeKind::UInt) {
        if (!data.is_number_unsigned() && data.get<i64>() < 0) {
            ctx.Warn(path, "negative value for an unsigned field; kept the existing value");
            return false;
        }
        u64 value = data.get<u64>();
        if (!UnsignedFits(value, int_type.size)) {
            ctx.Warn(path, "value " + std::to_string(value) + " doesn't fit in " + int_type.name + "; kept the existing value");
            return false;
        }
        WriteUnsigned(ptr, int_type.size, value);
        return true;
    }
    if (data.is_number_unsigned() && data.get<u64>() > static_cast<u64>(std::numeric_limits<i64>::max())) {
        ctx.Warn(path, std::string("value doesn't fit in ") + int_type.name + "; kept the existing value");
        return false;
    }
    i64 value = data.get<i64>();
    if (!SignedFits(value, int_type.size)) {
        ctx.Warn(path, "value " + std::to_string(value) + " doesn't fit in " + int_type.name + "; kept the existing value");
        return false;
    }
    WriteSigned(ptr, int_type.size, value);
    return true;
}

bool LoadStruct(const TypeInfo& type, void* ptr, const Json& data, LoadContext& ctx, const std::string& path) {
    if (data.is_array()) {
        if (!type.serialize_as_array) {
            ctx.Warn(path, std::string("expected an object for ") + type.name + ", found an array");
            return false;
        }
        if (data.size() != type.fields.size()) {
            ctx.Warn(path, std::string(type.name) + " expects " + std::to_string(type.fields.size()) +
                               " values, found " + std::to_string(data.size()) + "; loaded the ones present");
        }
        for (usize i = 0; i < type.fields.size() && i < data.size(); ++i) {
            const FieldInfo& field = type.fields[i];
            LoadValue(*field.type, field.Ptr(ptr), data[i], ctx, path + "." + field.name);
        }
        return true;
    }
    if (!data.is_object()) {
        ctx.Warn(path, std::string("expected an object for ") + type.name + ", found " + JsonTypeName(data));
        return false;
    }

    // Schema version: bring older data up to date before reading fields.
    const Json* source = &data;
    Json migrated;
    u16 saved_version = 1;
    if (auto it = data.find("$v"); it != data.end() && it->is_number_integer()) {
        i64 v = it->get<i64>();
        saved_version = static_cast<u16>(v < 1 ? 1 : (v > 0xFFFF ? 0xFFFF : v));
    }
    if (saved_version < type.version) {
        if (MigrationFn migrate = FindMigration(type)) {
            migrated = data;
            migrate(saved_version, migrated);
            source = &migrated;
        }
    } else if (saved_version > type.version) {
        ctx.Warn(path, std::string(type.name) + " was saved by a newer version (" + std::to_string(saved_version) +
                           " > " + std::to_string(type.version) + "); loading the fields this version knows");
    }

    for (const FieldInfo& field : type.fields) {
        if (field.HasFlag(Field_Transient)) {
            continue;
        }
        auto it = source->find(field.name);
        if (it != source->end()) {
            LoadValue(*field.type, field.Ptr(ptr), *it, ctx, path + "." + field.name);
        }
    }
    return true;
}

bool LoadValue(const TypeInfo& type, void* ptr, const Json& data, LoadContext& ctx, const std::string& path) {
    if (const auto* converter = FindConverter(type)) {
        if (!converter->second(data, ptr)) {
            ctx.Warn(path, std::string("not a valid ") + type.name + " (found " + JsonTypeName(data) +
                               "); kept the existing value");
            return false;
        }
        return true;
    }
    switch (type.kind) {
    case TypeKind::Bool:
        if (!data.is_boolean()) {
            ctx.Warn(path, std::string("expected a bool, found ") + JsonTypeName(data));
            return false;
        }
        *static_cast<bool*>(ptr) = data.get<bool>();
        return true;
    case TypeKind::Int:
    case TypeKind::UInt:
        return LoadInteger(type, ptr, data, ctx, path);
    case TypeKind::Float:
        if (!data.is_number()) {
            ctx.Warn(path, std::string("expected a number, found ") + JsonTypeName(data));
            return false;
        }
        if (type.size == 4) {
            *static_cast<f32*>(ptr) = static_cast<f32>(data.get<f64>());
        } else {
            *static_cast<f64*>(ptr) = data.get<f64>();
        }
        return true;
    case TypeKind::String:
        if (!data.is_string()) {
            ctx.Warn(path, std::string("expected a string, found ") + JsonTypeName(data));
            return false;
        }
        *static_cast<std::string*>(ptr) = data.get<std::string>();
        return true;
    case TypeKind::FixedString: {
        if (!data.is_string()) {
            ctx.Warn(path, std::string("expected a string, found ") + JsonTypeName(data));
            return false;
        }
        const std::string& text = data.get_ref<const std::string&>();
        usize length = text.size();
        if (length >= type.size) {
            length = type.size - 1; // keep room for the terminator
            ctx.Warn(path, "string of " + std::to_string(text.size()) + " bytes doesn't fit in " + type.name +
                               "; truncated to " + std::to_string(length));
        }
        char* chars = static_cast<char*>(ptr);
        std::memset(chars, 0, type.size);
        std::memcpy(chars, text.data(), length);
        return true;
    }
    case TypeKind::Enum: {
        if (data.is_string()) {
            i64 value = 0;
            const std::string& name = data.get_ref<const std::string&>();
            if (!type.EnumValueOf(name, value)) {
                ctx.Warn(path, "\"" + name + "\" isn't a value of " + type.name + "; kept the existing value");
                return false;
            }
            Json as_number = value;
            return LoadInteger(*type.underlying, ptr, as_number, ctx, path);
        }
        if (data.is_number_integer()) {
            return LoadInteger(*type.underlying, ptr, data, ctx, path);
        }
        ctx.Warn(path, std::string("expected an enum name, found ") + JsonTypeName(data));
        return false;
    }
    case TypeKind::Array: {
        if (!data.is_array()) {
            ctx.Warn(path, std::string("expected an array, found ") + JsonTypeName(data));
            return false;
        }
        // The data decides the length; each element loads tolerantly into a
        // default-constructed element, like a struct field does.
        type.array_resize(ptr, 0);
        type.array_resize(ptr, data.size());
        for (usize i = 0; i < data.size(); ++i) {
            LoadValue(*type.element, type.array_element(ptr, i), data[i], ctx, path + "[" + std::to_string(i) + "]");
        }
        return true;
    }
    case TypeKind::Struct:
        return LoadStruct(type, ptr, data, ctx, path);
    }
    return false;
}

} // namespace

Json ToJson(const TypeInfo& type, const void* object) {
    return SaveValue(type, object);
}

bool FromJson(const TypeInfo& type, void* object, const Json& data, LoadReport* report) {
    LoadContext ctx{report};
    return LoadValue(type, object, data, ctx, type.name);
}

std::string SaveJsonText(const TypeInfo& type, const void* object) {
    return ToJson(type, object).dump(2);
}

bool LoadJsonText(const TypeInfo& type, void* object, std::string_view text, LoadReport* report) {
    Json data = Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (data.is_discarded()) {
        LoadContext{report}.Warn(type.name, "not valid JSON");
        return false;
    }
    return FromJson(type, object, data, report);
}

std::vector<u8> SaveBinary(const TypeInfo& type, const void* object) {
    return Json::to_msgpack(ToJson(type, object));
}

bool LoadBinary(const TypeInfo& type, void* object, std::span<const u8> bytes, LoadReport* report) {
    Json data = Json::from_msgpack(bytes.begin(), bytes.end(), /*strict=*/true, /*allow_exceptions=*/false);
    if (data.is_discarded()) {
        LoadContext{report}.Warn(type.name, "not valid binary (MessagePack) data");
        return false;
    }
    return FromJson(type, object, data, report);
}

void RegisterJsonConverter(const TypeInfo& type, ToJsonConverter to_json, FromJsonConverter from_json) {
    AETHER_ASSERT(type.kind == TypeKind::Struct);
    ConverterRegistry& registry = Converters();
    std::unique_lock<std::shared_mutex> lock(registry.mutex);
    registry.by_type[type.id] = {to_json, from_json};
}

bool MigrateJson(const TypeInfo& type, Json& data) {
    if (!data.is_object() || type.kind != TypeKind::Struct || type.serialize_as_array) {
        return false;
    }
    u16 saved_version = 1;
    if (auto it = data.find("$v"); it != data.end() && it->is_number_integer()) {
        const i64 v = it->get<i64>();
        saved_version = static_cast<u16>(v < 1 ? 1 : (v > 0xFFFF ? 0xFFFF : v));
    }
    if (saved_version >= type.version) {
        return false;
    }
    if (MigrationFn migrate = FindMigration(type)) {
        migrate(saved_version, data);
    }
    data["$v"] = type.version;
    return true;
}

void RegisterMigration(const TypeInfo& type, MigrationFn fn) {
    MigrationRegistry& registry = Migrations();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.by_type[type.id] = fn;
}

} // namespace aether::reflect

// bytes.h
#include "aether/reflection/bytes.h"

namespace aether::reflect {

void AppendBinary(const TypeInfo& type, const void* object, std::vector<u8>& out) {
    Json::to_msgpack(ToJson(type, object), out);
}

bool ReadBinary(const TypeInfo& type, void* object, const u8* data, usize size) {
    return LoadBinary(type, object, std::span<const u8>(data, size), nullptr);
}

} // namespace aether::reflect
