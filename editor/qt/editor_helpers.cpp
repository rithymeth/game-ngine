#include "editor_helpers.h"

#include <algorithm>
#include <cmath>

namespace aether::editor::qt {

Vec3 RotationToEulerDegrees(Quaternion rotation) {
    rotation = rotation.Normalized();
    const f32 x = rotation.x;
    const f32 y = rotation.y;
    const f32 z = rotation.z;
    const f32 w = rotation.w;
    const f32 sin_pitch = std::clamp(2.0f * (w * x - y * z), -1.0f, 1.0f);
    const f32 pitch = std::asin(sin_pitch);
    f32 yaw;
    f32 roll;
    if (std::fabs(sin_pitch) > 0.9999f) {
        yaw = std::atan2(-2.0f * (x * z - w * y), 1.0f - 2.0f * (y * y + z * z));
        roll = 0.0f;
    } else {
        yaw = std::atan2(2.0f * (w * y + z * x), 1.0f - 2.0f * (x * x + y * y));
        roll = std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (x * x + z * z));
    }
    return {Degrees(pitch), Degrees(yaw), Degrees(roll)};
}

Quaternion EulerDegreesToRotation(const Vec3& degrees) {
    const Quaternion pitch = Quaternion::FromAxisAngle({1.0f, 0.0f, 0.0f}, Radians(degrees.x));
    const Quaternion yaw = Quaternion::FromAxisAngle({0.0f, 1.0f, 0.0f}, Radians(degrees.y));
    const Quaternion roll = Quaternion::FromAxisAngle({0.0f, 0.0f, 1.0f}, Radians(degrees.z));
    return (yaw * pitch * roll).Normalized();
}

std::string StablePathKey(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) absolute = path;
    const std::string key = absolute.lexically_normal().generic_string();
    u64 hash = 14695981039346656037ull;
    for (const unsigned char byte : key) hash = (hash ^ byte) * 1099511628211ull;
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded(16, '0');
    for (int i = 15; i >= 0; --i) {
        encoded[static_cast<std::size_t>(i)] = digits[hash & 0xfu];
        hash >>= 4u;
    }
    return encoded;
}

} // namespace aether::editor::qt
