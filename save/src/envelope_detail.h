#pragma once

#include "aether/pak/pak.h"

#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace aether::save::envelope::detail {

// Compare the on-disk checksum against the same compact JSON used on write.
inline std::string ChecksumText(std::string_view text) {
    const auto bytes = std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size());
    char hex[16];
    std::snprintf(hex, sizeof hex, "%08x", pak::Crc32(bytes));
    return hex;
}

} // namespace aether::save::envelope::detail
