#pragma once

#include "aether/reflection/type_info.h"

#include <string_view>
#include <vector>

namespace aether::reflect {

// Process-wide index of every reflected type, by TypeId and by declared name.
//
// Types register themselves the first time their TypeInfo is built — either
// on first use of Reflect<T>(), or at static-initialization time via the
// registrar AETHER_REFLECT/AETHER_ENUM emit, so Find("Transform") works even
// if nothing has called Reflect<Transform>() yet. (One caveat inherited from
// static registration in general: a type reflected only inside a static
// library translation unit that nothing links against never registers. Types
// reflected in headers the program includes are always fine.)
class TypeRegistry {
public:
    // Idempotent for the same TypeInfo. A *different* TypeInfo with the same
    // name/TypeId (two types with one declared name) is rejected with an error
    // log; the first registration wins.
    static void Register(const TypeInfo& info);

    static const TypeInfo* Find(TypeId id);
    static const TypeInfo* Find(std::string_view name);

    // Snapshot of every registered type, in registration order.
    static std::vector<const TypeInfo*> AllTypes();
};

} // namespace aether::reflect
