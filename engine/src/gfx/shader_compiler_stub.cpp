#include "aether/gfx/shader_compiler.h"

namespace aether::gfx {

bool CanCompileRayQueriesToSPIRV() {
    return false; // Neither DXC nor glslang is part of this build.
}

} // namespace aether::gfx
