#pragma once

#include "aether/reflection/type_id.h"

#include <concepts>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace aether::reflect {

// What shape of data a TypeInfo describes. Container, asset-reference and
// entity-reference kinds are added alongside the features that need them
// (see docs/ROADMAP_DETAILS.md §B).
enum class TypeKind : u8 {
    Bool,
    Int,    // signed integer of `size` bytes
    UInt,   // unsigned integer of `size` bytes
    Float,  // f32 or f64, by `size`
    String, // std::string
    FixedString, // char[N]: a NUL-terminated string in a fixed buffer of `size` bytes
    Enum,   // integer-backed enum; see TypeInfo::enum_values / underlying
    Struct, // aggregate of FieldInfo entries
    Array,  // std::vector<T>: `element` is T; accessed through the array_* thunks
};

// Optional, editor-facing field metadata. An aggregate so reflected fields can
// use designated initializers: AETHER_FIELD(speed, Field_EditAnywhere,
// { .tooltip = "Units per second", .units = "m/s" }). Designated initializers
// must appear in declaration order.
struct Meta {
    const char* tooltip = nullptr;
    const char* category = nullptr;
    f64 range_min = 0.0; // range_min == range_max means "no range"
    f64 range_max = 0.0;
    bool is_color = false;
    const char* units = nullptr; // "m", "deg", "s"

    bool HasRange() const { return range_min != range_max; }
};

enum FieldFlags : u32 {
    Field_None = 0,
    Field_EditAnywhere = 1u << 0,       // shown and editable in the Inspector
    Field_ReadOnly = 1u << 1,           // shown but not editable
    Field_Transient = 1u << 2,          // never serialized
    Field_BlueprintReadWrite = 1u << 3, // Get/Set nodes generated (Phase 12)
    Field_Replicated = 1u << 4,         // sent to clients (Phase 22)
};

struct TypeInfo;
class Any;

struct FieldInfo {
    const char* name = "";
    const TypeInfo* type = nullptr;
    u32 offset = 0;
    u32 flags = Field_None;
    Meta meta;

    bool HasFlag(FieldFlags flag) const { return (flags & flag) != 0; }

    void* Ptr(void* object) const { return static_cast<u8*>(object) + offset; }
    const void* Ptr(const void* object) const { return static_cast<const u8*>(object) + offset; }

    // Typed access; returns nullptr if T isn't this field's type.
    template <typename T>
    T* As(void* object) const;
    template <typename T>
    const T* As(const void* object) const;

    // Type-erased access (defined in any.cpp). Set copies `value` into the
    // field and returns false, leaving the field unchanged, if `value` holds a
    // different type.
    Any Get(const void* object) const;
    bool Set(void* object, const Any& value) const;
};

struct EnumValue {
    const char* name = "";
    i64 value = 0;
};

enum FunctionFlags : u32 {
    Fn_None = 0,
    Fn_BlueprintCallable = 1u << 0, // becomes a Blueprint node (Phase 12)
    Fn_Pure = 1u << 1,              // no side effects: a pure node with no exec pins
    Fn_Const = 1u << 2,             // set automatically for const member functions
    Fn_Static = 1u << 3,            // set automatically for static/free functions; no `self`
    // Network calls (Phase 22, net/rpc.h), on a component of a replicated entity.
    Fn_ServerRPC = 1u << 4,         // a client calls it; runs on the server, only for the entity's owner
    Fn_ClientRPC = 1u << 5,         // the server calls it; runs on the client that owns the entity
    Fn_MulticastRPC = 1u << 6,      // the server calls it; runs on every client that has the entity
    Fn_RpcUnreliable = 1u << 7,     // sent without the reliable channel: may be lost, never resent
};

struct ParamInfo {
    const char* name = "";
    const TypeInfo* type = nullptr;
};

// A reflected function: a member function (called on an instance of the
// owning type) or a static function. Invoke type-checks every argument
// against `params` before calling, so a bad call from a script or Blueprint
// is refused instead of reinterpreting memory.
struct FunctionInfo {
    const char* name = "";
    const TypeInfo* return_type = nullptr; // nullptr for void
    std::vector<ParamInfo> params;
    u32 flags = Fn_None;

    // Type-erased call. `args` has exactly params.size() entries, already
    // validated; a non-void result is stored into `*ret` when ret != nullptr.
    void (*thunk)(void* self, Any* args, Any* ret) = nullptr;

    bool HasFlag(FunctionFlags flag) const { return (flags & flag) != 0; }

    // Returns false (and calls nothing) if the argument count or any argument
    // type doesn't match, or if a member function is given a null `self`.
    // Arguments are passed by reference: a function taking `T&` can write
    // back into its Any argument.
    bool Invoke(void* self, std::span<Any> args, Any* ret = nullptr) const;
};

struct TypeInfo {
    const char* name = "";     // declared name: "Transform", "f32"
    TypeId id = kInvalidTypeId;
    TypeKind kind = TypeKind::Struct;
    u32 size = 0;
    u32 alignment = 0;
    u16 version = 1;           // schema version, bumped when fields change meaning
    // Struct only: serialize as a JSON array of field values in declaration
    // order ([1, 2, 3]) instead of an object. Used for small math types; set
    // by specializing detail::kSerializeAsArray<T>.
    bool serialize_as_array = false;
    // Non-null for asset references (assets::AssetRef<T>): the importer name
    // of the asset type they point at ("Model", "Texture", ...). Lets the
    // editor draw an asset picker instead of the raw GUID.
    const char* asset_type = nullptr;

    std::vector<FieldInfo> fields;       // Struct only, declaration order
    std::vector<FunctionInfo> functions; // Struct only, declaration order
    std::vector<EnumValue> enum_values;  // Enum only
    const TypeInfo* underlying = nullptr; // Enum only: the integer type backing it
    const TypeInfo* element = nullptr;    // Array only: the element type

    // Array only. `array_element` returns element `index` (< size) of a live
    // array; `array_resize` default-constructs new elements.
    usize (*array_size)(const void* array) = nullptr;
    void (*array_resize)(void* array, usize size) = nullptr;
    void* (*array_element)(void* array, usize index) = nullptr;
    const void* ArrayElement(const void* array, usize index) const {
        return array_element(const_cast<void*>(array), index);
    }

    // Lifecycle thunks. `construct` default-constructs into uninitialized
    // memory (nullptr if T isn't default-constructible); the *_construct
    // variants construct `dst` (uninitialized) from a live `src`.
    void (*construct)(void* dst) = nullptr;
    void (*destruct)(void* object) = nullptr;
    void (*copy_construct)(void* dst, const void* src) = nullptr;
    void (*move_construct)(void* dst, void* src) = nullptr;
    void (*copy_assign)(void* dst, const void* src) = nullptr; // both live objects

    const FieldInfo* FindField(std::string_view field_name) const {
        for (const FieldInfo& field : fields) {
            if (field_name == field.name) {
                return &field;
            }
        }
        return nullptr;
    }

    const FunctionInfo* FindFunction(std::string_view function_name) const {
        for (const FunctionInfo& function : functions) {
            if (function_name == function.name) {
                return &function;
            }
        }
        return nullptr;
    }

    // Enum helpers. EnumName returns nullptr for a value with no named entry.
    const char* EnumName(i64 value) const {
        for (const EnumValue& entry : enum_values) {
            if (entry.value == value) {
                return entry.name;
            }
        }
        return nullptr;
    }
    bool EnumValueOf(std::string_view entry_name, i64& out_value) const {
        for (const EnumValue& entry : enum_values) {
            if (entry_name == entry.name) {
                out_value = entry.value;
                return true;
            }
        }
        return false;
    }
};

// Reflector<T>::Get() returns T's TypeInfo. There is no primary definition:
// builtin types specialize it in builtin_types.h, and AETHER_REFLECT /
// AETHER_ENUM specialize it for user types.
template <typename T>
struct Reflector;

template <typename T>
concept Reflected = requires {
    { Reflector<std::remove_cv_t<T>>::Get() } -> std::same_as<const TypeInfo&>;
};

template <typename T>
const TypeInfo& Reflect() {
    static_assert(Reflected<T>,
                  "Type is not reflected: add AETHER_REFLECT(Type, version, fields...) (or AETHER_ENUM) for it");
    return Reflector<std::remove_cv_t<T>>::Get();
}

template <typename T>
TypeId TypeIdOf() {
    return Reflect<T>().id;
}

template <typename T>
T* FieldInfo::As(void* object) const {
    return type == &Reflect<T>() ? static_cast<T*>(Ptr(object)) : nullptr;
}

template <typename T>
const T* FieldInfo::As(const void* object) const {
    return type == &Reflect<T>() ? static_cast<const T*>(Ptr(object)) : nullptr;
}

namespace detail {

// Specialize to true for small, fixed-shape value types (see
// TypeInfo::serialize_as_array). Must be visible before the type is reflected.
template <typename T>
inline constexpr bool kSerializeAsArray = false;

// Fills in everything about T that doesn't depend on its fields: identity,
// size, and lifecycle thunks.
template <typename T>
TypeInfo MakeTypeInfo(const char* declared_name, TypeKind kind, u16 version) {
    TypeInfo info;
    info.name = StripNamespace(declared_name);
    info.id = HashTypeName(info.name);
    info.kind = kind;
    info.size = static_cast<u32>(sizeof(T));
    info.alignment = static_cast<u32>(alignof(T));
    info.version = version;
    info.serialize_as_array = kSerializeAsArray<T>;
    if constexpr (std::is_default_constructible_v<T>) {
        info.construct = [](void* dst) { new (dst) T(); };
    }
    info.destruct = [](void* object) { static_cast<T*>(object)->~T(); };
    if constexpr (std::is_copy_constructible_v<T>) {
        info.copy_construct = [](void* dst, const void* src) { new (dst) T(*static_cast<const T*>(src)); };
    }
    if constexpr (std::is_move_constructible_v<T>) {
        info.move_construct = [](void* dst, void* src) { new (dst) T(std::move(*static_cast<T*>(src))); };
    }
    if constexpr (std::is_copy_assignable_v<T>) {
        info.copy_assign = [](void* dst, const void* src) { *static_cast<T*>(dst) = *static_cast<const T*>(src); };
    }
    return info;
}

template <typename FieldType>
FieldInfo MakeField(const char* name, usize offset, u32 flags = Field_None, Meta meta = {}) {
    FieldInfo field;
    field.name = name;
    field.type = &Reflect<FieldType>();
    field.offset = static_cast<u32>(offset);
    field.flags = flags;
    field.meta = meta;
    return field;
}

} // namespace detail

} // namespace aether::reflect
