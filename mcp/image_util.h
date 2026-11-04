#pragma once

// Small image helpers for tools that return a picture: PNG encoding (stb_image_write, implemented in screenshot.cpp) and
// the base64 an MCP image content item carries.

#include "aether/core/base.h"

#include <stb_image_write.h>

#include <string>
#include <vector>

namespace aether::mcp {

inline std::vector<u8> EncodePngRgba(const std::vector<u8>& rgba, u32 width, u32 height) {
    std::vector<u8> png;
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto* out = static_cast<std::vector<u8>*>(context);
            out->insert(out->end(), static_cast<u8*>(data), static_cast<u8*>(data) + size);
        },
        &png, static_cast<int>(width), static_cast<int>(height), 4, rgba.data(), static_cast<int>(width) * 4);
    return png;
}

inline std::string Base64Encode(const std::vector<u8>& bytes) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (usize i = 0; i < bytes.size(); i += 3) {
        const u32 n = (static_cast<u32>(bytes[i]) << 16) | (i + 1 < bytes.size() ? static_cast<u32>(bytes[i + 1]) << 8 : 0) | (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? kAlphabet[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? kAlphabet[n & 63] : '=');
    }
    return out;
}

} // namespace aether::mcp
