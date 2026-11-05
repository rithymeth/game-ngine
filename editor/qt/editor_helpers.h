#pragma once

#include "aether/math/math.h"

#include <filesystem>
#include <string>

namespace aether::editor::qt {

// The Inspector edits rotation as pitch (X), yaw (Y), roll (Z) degrees using the
// engine's Y-X-Z composition order. The scene keeps a normalized quaternion, so
// editing does not change the serialized Transform format.
Vec3 RotationToEulerDegrees(Quaternion rotation);
Quaternion EulerDegreesToRotation(const Vec3& degrees);

// A stable 16-hex-digit key for a path (FNV-1a of its normalized absolute form),
// for autosave names and the crash report's project context.
std::string StablePathKey(const std::filesystem::path& path);

} // namespace aether::editor::qt
