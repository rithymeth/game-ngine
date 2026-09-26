#pragma once

#include "aether/reflection/type_info.h"

#include <nlohmann/json.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aether::reflect {

using Json = nlohmann::json;

// Reflection-driven serialization. One JSON document model backs both
// archives:
//
// - JSON text (SaveJsonText/LoadJsonText): what scenes, prefabs and assets
//   use during development. Output is deterministic: keys sorted, 2-space
//   indent, and f32 values written in their shortest round-trip form (0.1,
//   not 0.10000000149011612), so saving an unchanged object gives a
//   byte-identical file.
// - Binary (SaveBinary/LoadBinary): the same document encoded as
//   MessagePack. It's smaller and faster to parse, and keeps field names, so
//   it's exactly as tolerant of schema changes as the JSON form. The compact,
//   name-free cooked format from docs/ROADMAP_DETAILS.md §A.5 is separate
//   and comes with the cooker.
//
// Value encoding by TypeKind: bool/int/uint/float/string as the matching JSON
// scalar; enums as their value's name (loading also accepts a number);
// structs as an object with a "$v" schema-version entry plus one entry per
// non-transient field, or as a positional array when
// TypeInfo::serialize_as_array is set (Vec3 -> [x, y, z]).
//
// Loading is tolerant, because data files outlive the code that wrote them:
// - A field missing from the data keeps whatever value the object already
//   had (normally its default), so load into a default-constructed object.
// - Data for a field that no longer exists is ignored.
// - A value of the wrong JSON type, an out-of-range integer, or an unknown
//   enum name is skipped with a warning, keeping the existing value.
// - Data written by an older schema version ("$v" lower than the type's
//   version) is passed through the type's migration hook, if one is
//   registered, before loading.
// The load functions return false only when the data can't be parsed at all,
// or its top-level shape doesn't match the type (an array where an object
// was expected, for example).

struct LoadReport {
    std::vector<std::string> warnings;
};

Json ToJson(const TypeInfo& type, const void* object);
bool FromJson(const TypeInfo& type, void* object, const Json& data, LoadReport* report = nullptr);

std::string SaveJsonText(const TypeInfo& type, const void* object);
bool LoadJsonText(const TypeInfo& type, void* object, std::string_view text, LoadReport* report = nullptr);

std::vector<u8> SaveBinary(const TypeInfo& type, const void* object);
bool LoadBinary(const TypeInfo& type, void* object, std::span<const u8> bytes, LoadReport* report = nullptr);

// Schema migration. `data` is the struct's JSON object as it was saved (with
// its "$v"); the hook edits it in place into the current version's shape
// (renaming, converting or removing entries). It receives the saved version
// and is responsible for every step from there to the current version.
using MigrationFn = void (*)(u16 from_version, Json& data);
void RegisterMigration(const TypeInfo& type, MigrationFn fn);

template <typename T>
Json ToJson(const T& object) {
    return ToJson(Reflect<T>(), &object);
}
template <typename T>
bool FromJson(T& object, const Json& data, LoadReport* report = nullptr) {
    return FromJson(Reflect<T>(), &object, data, report);
}
template <typename T>
std::string SaveJsonText(const T& object) {
    return SaveJsonText(Reflect<T>(), &object);
}
template <typename T>
bool LoadJsonText(T& object, std::string_view text, LoadReport* report = nullptr) {
    return LoadJsonText(Reflect<T>(), &object, text, report);
}
template <typename T>
std::vector<u8> SaveBinary(const T& object) {
    return SaveBinary(Reflect<T>(), &object);
}
template <typename T>
bool LoadBinary(T& object, std::span<const u8> bytes, LoadReport* report = nullptr) {
    return LoadBinary(Reflect<T>(), &object, bytes, report);
}
template <typename T>
void RegisterMigration(MigrationFn fn) {
    RegisterMigration(Reflect<T>(), fn);
}

} // namespace aether::reflect
