#pragma once

#include "aether/reflection/type_info.h"

#include <nlohmann/json_fwd.hpp>

namespace aether::reflect {

// Custom JSON representation for a type, used by the reflection archives
// (serialize.h) instead of the default field-by-field encoding. For small
// value types with a conventional text form, e.g. a GUID written as
// "5c0a9e2e-7b1d-4c3a-9f10-2a6b8e4d1f77" rather than {"hi": ..., "lo": ...}.
// A FromJsonConverter returns false when the data has the wrong shape; the
// loader then warns and keeps the existing value. Only the JSON form is
// affected; the binary archive stores the same document as MessagePack, so
// it uses the converter too.
using ToJsonConverter = nlohmann::json (*)(const void* object);
using FromJsonConverter = bool (*)(const nlohmann::json& data, void* object);

void RegisterJsonConverter(const TypeInfo& type, ToJsonConverter to_json, FromJsonConverter from_json);

} // namespace aether::reflect
