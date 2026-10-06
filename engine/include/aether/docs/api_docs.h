#pragma once

#include "aether/core/base.h"
#include "aether/reflection/reflection.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

// The API reference, generated from reflection (Phase 26 step 5,
// docs/design/PHASE_SPECS.md §26.7). Every reflected type the program has
// registered is documented from what the code already says: its fields with
// their types, editing flags, ranges, units and tooltips, its functions with
// their signatures, and an enum's values. Because it is generated, it can't
// drift from the engine, and a plugin's types appear in it too.
//
// `aether_docgen` registers the engine's components and writes the whole
// reference; a game or plugin can call these with its own types registered.

namespace aether::docs {

struct ApiDocOptions {
    std::string title = "Aether API reference";
    // Leave out the types that are only data (numbers, strings, arrays): they are
    // named where they're used. On by default; the math types are structs and stay.
    bool skip_scalars = true;
};

// How a type is written: "f32", "Vec3", "AssetRef<Model>", "Array<Light>".
std::string DisplayTypeName(const reflect::TypeInfo& type);

// What a type is, in the reference: "Component" (registered with the ECS
// by name), "Enum" or "Struct".
std::string TypeCategory(const reflect::TypeInfo& type);

// Every registered type, sorted by name, as Markdown: a contents list, then
// Components, Structs and Enums, each type with a table of its fields.
std::string GenerateApiMarkdown(const ApiDocOptions& options = {});

// The same content as data: {"title", "types": [{name, category, kind, size,
// version, fields: [...], functions: [...], values: [...]}]}.
nlohmann::json GenerateApiJson(const ApiDocOptions& options = {});

// Writes API.md and api.json into `directory` (made if missing).
bool WriteApiDocs(const std::filesystem::path& directory, const ApiDocOptions& options = {},
                  std::string* error = nullptr);

} // namespace aether::docs
