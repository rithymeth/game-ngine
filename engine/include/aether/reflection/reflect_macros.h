#pragma once

#include "aether/reflection/any.h"
#include "aether/reflection/registry.h"
#include "aether/reflection/type_info.h"

#include <cstddef>
#include <initializer_list>

namespace aether::reflect::detail {

// One entry in an AETHER_REFLECT member list: a field or a function.
struct MemberDecl {
    MemberDecl(FieldInfo f) : field(std::move(f)) {}                          // NOLINT: implicit by design
    MemberDecl(FunctionInfo f) : function(std::move(f)), is_function(true) {} // NOLINT: implicit by design
    FieldInfo field;
    FunctionInfo function;
    bool is_function = false;
};

inline void AddMembers(TypeInfo& info, std::initializer_list<MemberDecl> members) {
    for (const MemberDecl& member : members) {
        if (member.is_function) {
            info.functions.push_back(member.function);
        } else {
            info.fields.push_back(member.field);
        }
    }
}

} // namespace aether::reflect::detail

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
// must itself be reflected (builtins are; see builtin_types.h). Functions go in
// the same list with AETHER_METHOD (below); fields and functions may be mixed
// in any order.
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
                detail::AddMembers(type_info, {__VA_ARGS__});                                     \
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

// AETHER_METHOD(member_function, flags, {"param", "names"}) — flags and
// parameter names are optional. Works for const, non-const and static member
// functions; the function must not be overloaded.
#define AETHER_METHOD(Function, ...) detail::MakeMethod<&Self::Function>(#Function, ##__VA_ARGS__)

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
