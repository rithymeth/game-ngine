#pragma once

#include "aether/reflection/registry.h"
#include "aether/reflection/type_info.h"

#include <cstddef>

// Reflection declaration macros. Use them at *global* namespace scope (they
// specialize aether::reflect::Reflector<T>), after the type's definition,
// with the type's fully qualified name:
//
//     namespace game { struct Health { f32 current = 100; f32 max = 100; bool invulnerable = false; }; }
//
//     AETHER_REFLECT(game::Health, 1,
//         AETHER_FIELD(current, Field_EditAnywhere, { .tooltip = "Current HP", .range_min = 0, .range_max = 1000 }),
//         AETHER_FIELD(max, Field_EditAnywhere),
//         AETHER_FIELD(invulnerable)
//     )
//
// The declared name is the unqualified type name ("Health"); the second
// argument is the schema version. AETHER_FIELD takes the member name, then
// optional FieldFlags and an optional Meta initializer. Every field's type
// must itself be reflected (builtins are; see builtin_types.h).
//
//     enum class game::Team : u8 { Red, Blue };
//     AETHER_ENUM(game::Team, 1, AETHER_ENUM_VALUE(Red), AETHER_ENUM_VALUE(Blue))

#define AETHER_REFLECT(Type, Version, ...)                                                        \
    template <>                                                                                   \
    struct aether::reflect::Reflector<Type> {                                                     \
        static const ::aether::reflect::TypeInfo& Get() {                                         \
            static const ::aether::reflect::TypeInfo info = [] {                                  \
                using namespace ::aether::reflect;                                                \
                using Self = Type;                                                                \
                TypeInfo type_info = detail::MakeTypeInfo<Self>(#Type, TypeKind::Struct, Version); \
                type_info.fields = {__VA_ARGS__};                                                 \
                return type_info;                                                                 \
            }();                                                                                  \
            static const bool registered = (::aether::reflect::TypeRegistry::Register(info), true); \
            (void)registered;                                                                     \
            return info;                                                                          \
        }                                                                                         \
        static inline const bool kStaticRegistration = (Get(), true);                            \
    };

#define AETHER_FIELD(Member, ...)                                                                 \
    detail::MakeField<decltype(Self::Member)>(#Member, offsetof(Self, Member), ##__VA_ARGS__)

#define AETHER_ENUM(Type, Version, ...)                                                           \
    template <>                                                                                   \
    struct aether::reflect::Reflector<Type> {                                                     \
        static const ::aether::reflect::TypeInfo& Get() {                                         \
            static const ::aether::reflect::TypeInfo info = [] {                                  \
                using namespace ::aether::reflect;                                                \
                using Self = Type;                                                                \
                TypeInfo type_info = detail::MakeTypeInfo<Self>(#Type, TypeKind::Enum, Version);  \
                type_info.underlying = &Reflect<std::underlying_type_t<Self>>();                  \
                type_info.enum_values = {__VA_ARGS__};                                            \
                return type_info;                                                                 \
            }();                                                                                  \
            static const bool registered = (::aether::reflect::TypeRegistry::Register(info), true); \
            (void)registered;                                                                     \
            return info;                                                                          \
        }                                                                                         \
        static inline const bool kStaticRegistration = (Get(), true);                            \
    };

#define AETHER_ENUM_VALUE(Value) EnumValue{#Value, static_cast<::aether::i64>(Self::Value)}
