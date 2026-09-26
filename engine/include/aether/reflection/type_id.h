#pragma once

#include "aether/core/base.h"

#include <string_view>

namespace aether::reflect {

// Stable identifier for a reflected type: a 64-bit FNV-1a hash of the type's
// *declared* name ("Transform", not "aether::Transform" and not whatever
// typeid(T).name() happens to produce on this compiler). Because it depends
// only on the name, the same type gets the same TypeId in every process and
// on every compiler, which is what makes it usable as an on-disk key.
//
// Two reflected types with the same declared name in different namespaces
// would collide; TypeRegistry::Register detects that and logs an error rather
// than silently letting one replace the other.
using TypeId = u64;

inline constexpr TypeId kInvalidTypeId = 0;

constexpr TypeId HashTypeName(std::string_view name) {
    u64 hash = 0xcbf29ce484222325ull; // FNV-1a 64-bit offset basis
    for (char c : name) {
        hash ^= static_cast<u8>(c);
        hash *= 0x100000001b3ull; // FNV-1a 64-bit prime
    }
    return hash;
}

// "aether::Transform" -> "Transform". Returns a pointer into the same string
// (so it stays valid for as long as the input, normally a string literal).
constexpr const char* StripNamespace(const char* qualified_name) {
    const char* last = qualified_name;
    for (const char* p = qualified_name; *p != '\0'; ++p) {
        if (p[0] == ':' && p[1] == ':') {
            last = p + 2;
        }
    }
    return last;
}

} // namespace aether::reflect
