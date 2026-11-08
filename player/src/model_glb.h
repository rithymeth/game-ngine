#pragma once

#include "aether/core/base.h"

#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace aether::player {

// A .glb holding `json` and `bin` (the BIN chunk is buffer 0). The player hands models to the shared glTF parser this way, not as a
// data URI: base64 in the JSON is a third larger than the data and counts against the parser's 32 MiB JSON cap, which a 24 MiB buffer
// already exceeds (a character with 4K textures is 39 MiB). `json` must describe buffer 0 without a uri.
inline std::vector<u8> MakeGlb(const std::string& json, std::span<const u8> bin) {
    const auto padded = [](usize size) { return (size + 3) & ~usize{3}; };
    const usize json_size = padded(json.size()), bin_size = padded(bin.size());
    std::vector<u8> glb(12 + 8 + json_size + 8 + bin_size, 0);
    const auto put = [&](usize at, u32 value) { std::memcpy(glb.data() + at, &value, sizeof(value)); };
    put(0, 0x46546C67u); // "glTF"
    put(4, 2u);
    put(8, static_cast<u32>(glb.size()));
    put(12, static_cast<u32>(json_size));
    put(16, 0x4E4F534Au); // "JSON"
    std::memcpy(glb.data() + 20, json.data(), json.size());
    std::memset(glb.data() + 20 + json.size(), ' ', json_size - json.size());
    put(20 + json_size, static_cast<u32>(bin_size));
    put(24 + json_size, 0x004E4942u); // "BIN\0"
    if (!bin.empty()) std::memcpy(glb.data() + 28 + json_size, bin.data(), bin.size());
    return glb;
}

} // namespace aether::player
