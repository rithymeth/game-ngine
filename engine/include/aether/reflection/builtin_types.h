#pragma once

#include "aether/math/math.h"
#include "aether/reflection/reflect_macros.h"

#include <cstring>
#include <string>
#include <vector>

// Reflection for the engine's builtin value types: primitives, std::string,
// and the math types. Declared names are the engine's own spellings ("f32",
// "i32", "string"), which is what appears in scene files and the editor.

#define AETHER_REFLECT_PRIMITIVE(Type, Name, Kind)                                                \
    template <>                                                                                   \
    struct aether::reflect::Reflector<Type> {                                                     \
        static const ::aether::reflect::TypeInfo& Get() {                                         \
            static const ::aether::reflect::TypeInfo info =                                       \
                ::aether::reflect::detail::MakeTypeInfo<Type>(Name, ::aether::reflect::TypeKind::Kind, 1); \
            static const bool registered = (::aether::reflect::TypeRegistry::Register(info), true); \
            (void)registered;                                                                     \
            return info;                                                                          \
        }                                                                                         \
        static inline const bool kStaticRegistration = (Get(), true);                            \
    };

AETHER_REFLECT_PRIMITIVE(bool, "bool", Bool)
AETHER_REFLECT_PRIMITIVE(aether::i8, "i8", Int)
AETHER_REFLECT_PRIMITIVE(aether::i16, "i16", Int)
AETHER_REFLECT_PRIMITIVE(aether::i32, "i32", Int)
AETHER_REFLECT_PRIMITIVE(aether::i64, "i64", Int)
AETHER_REFLECT_PRIMITIVE(aether::u8, "u8", UInt)
AETHER_REFLECT_PRIMITIVE(aether::u16, "u16", UInt)
AETHER_REFLECT_PRIMITIVE(aether::u32, "u32", UInt)
AETHER_REFLECT_PRIMITIVE(aether::u64, "u64", UInt)
AETHER_REFLECT_PRIMITIVE(aether::f32, "f32", Float)
AETHER_REFLECT_PRIMITIVE(aether::f64, "f64", Float)
AETHER_REFLECT_PRIMITIVE(std::string, "string", String)

// Arrays: std::vector<T> of any reflected T (including structs and other
// arrays). Declared name "Array<T>", e.g. "Array<string>". std::vector<bool>
// isn't supported — its elements aren't addressable; use std::vector<u8>.
template <typename T>
struct aether::reflect::Reflector<std::vector<T>> {
    static_assert(!std::is_same_v<T, bool>, "std::vector<bool> can't be reflected; use std::vector<u8>");
    static const ::aether::reflect::TypeInfo& Get() {
        static const std::string declared_name = std::string("Array<") + Reflect<T>().name + ">";
        static const ::aether::reflect::TypeInfo info = [] {
            using namespace ::aether::reflect;
            using Self = std::vector<T>;
            TypeInfo type_info = detail::MakeTypeInfo<Self>("", TypeKind::Array, 1);
            type_info.name = declared_name.c_str();
            type_info.id = HashTypeName(type_info.name);
            type_info.element = &Reflect<T>();
            type_info.array_size = [](const void* array) { return static_cast<const Self*>(array)->size(); };
            type_info.array_resize = [](void* array, usize size) { static_cast<Self*>(array)->resize(size); };
            type_info.array_element = [](void* array, usize index) -> void* {
                return &(*static_cast<Self*>(array))[index];
            };
            return type_info;
        }();
        static const bool registered = (::aether::reflect::TypeRegistry::Register(info), true);
        (void)registered;
        return info;
    }
};

// Fixed-size character buffers (`char name[64]`), for plain-data components
// that must stay trivially copyable. Declared name "char[N]". Serialized as a
// string; loading a string that doesn't fit is truncated with a warning.
template <aether::usize N>
struct aether::reflect::Reflector<char[N]> {
    static const ::aether::reflect::TypeInfo& Get() {
        static const std::string declared_name = "char[" + std::to_string(N) + "]";
        static const ::aether::reflect::TypeInfo info = [] {
            using namespace ::aether::reflect;
            TypeInfo type_info;
            type_info.name = declared_name.c_str();
            type_info.id = HashTypeName(type_info.name);
            type_info.kind = TypeKind::FixedString;
            type_info.size = static_cast<u32>(N);
            type_info.alignment = 1;
            type_info.construct = [](void* dst) { std::memset(dst, 0, N); };
            type_info.destruct = [](void*) {};
            type_info.copy_construct = [](void* dst, const void* src) { std::memcpy(dst, src, N); };
            type_info.move_construct = [](void* dst, void* src) { std::memcpy(dst, src, N); };
            type_info.copy_assign = [](void* dst, const void* src) { std::memcpy(dst, src, N); };
            return type_info;
        }();
        static const bool registered = (::aether::reflect::TypeRegistry::Register(info), true);
        (void)registered;
        return info;
    }
};

template <>
inline constexpr bool aether::reflect::detail::kSerializeAsArray<aether::Vec3> = true;
template <>
inline constexpr bool aether::reflect::detail::kSerializeAsArray<aether::Vec4> = true;
template <>
inline constexpr bool aether::reflect::detail::kSerializeAsArray<aether::Quaternion> = true;
template <>
inline constexpr bool aether::reflect::detail::kSerializeAsArray<aether::Mat4> = true;

AETHER_REFLECT(aether::Vec3, 1, AETHER_FIELD(x), AETHER_FIELD(y), AETHER_FIELD(z))
AETHER_REFLECT(aether::Vec4, 1, AETHER_FIELD(x), AETHER_FIELD(y), AETHER_FIELD(z), AETHER_FIELD(w))
AETHER_REFLECT(aether::Quaternion, 1, AETHER_FIELD(x), AETHER_FIELD(y), AETHER_FIELD(z), AETHER_FIELD(w))

// Mat4 stores `Vec4 cols[4]`; fixed-size arrays aren't a reflected kind (yet),
// so its columns are exposed as four Vec4 fields at their array offsets.
template <>
struct aether::reflect::Reflector<aether::Mat4> {
    static const ::aether::reflect::TypeInfo& Get() {
        static const ::aether::reflect::TypeInfo info = [] {
            using namespace ::aether::reflect;
            TypeInfo type_info = detail::MakeTypeInfo<aether::Mat4>("Mat4", TypeKind::Struct, 1);
            const char* names[4] = {"col0", "col1", "col2", "col3"};
            for (usize i = 0; i < 4; ++i) {
                type_info.fields.push_back(detail::MakeField<aether::Vec4>(
                    names[i], offsetof(aether::Mat4, cols) + i * sizeof(aether::Vec4)));
            }
            return type_info;
        }();
        static const bool registered = (::aether::reflect::TypeRegistry::Register(info), true);
        (void)registered;
        return info;
    }
    static inline const bool kStaticRegistration = (Get(), true);
};
