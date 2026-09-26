#pragma once

#include "aether/reflection/type_info.h"

#include <vector>

namespace aether::reflect {

// Lightweight entry points into the binary archive (serialize.h) for code
// that shouldn't pull in the JSON library's headers — chiefly the ECS, whose
// component registry uses these as the default (de)serializer for reflected
// components. Same format as SaveBinary/LoadBinary; warnings are logged.
void AppendBinary(const TypeInfo& type, const void* object, std::vector<u8>& out);
bool ReadBinary(const TypeInfo& type, void* object, const u8* data, usize size);

} // namespace aether::reflect
