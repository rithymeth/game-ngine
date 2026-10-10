#pragma once

#include "aether/core/base.h"

namespace aether::assets {

// Alpha handling shared by glTF import, cooked material data, and renderers.
enum class MaterialAlphaMode : u8 {
    Opaque,
    Mask,
    Blend,
};

} // namespace aether::assets
