#include "camera_path.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace aether::player {

namespace {

f32 CatmullRom(f32 p0, f32 p1, f32 p2, f32 p3, f32 t) {
    const f32 t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

// The angle `to` as the multiple of 360 degrees nearest `from`, so a turn goes the short way round.
f32 Unwrap(f32 from, f32 to) { return to + 360.0f * std::round((from - to) / 360.0f); }

} // namespace

u32 CameraPath::FrameCount() const {
    return std::max(1u, static_cast<u32>(std::ceil(Duration() * std::max(fps, 1.0f))));
}

bool ParseCameraPath(const std::string& text, CameraPath& out, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return fail("the camera path is not a JSON object");
    CameraPath path;
    if (json.contains("fps")) {
        if (!json["fps"].is_number()) return fail("fps must be a number");
        path.fps = json["fps"].get<f32>();
        if (!(path.fps >= 1.0f && path.fps <= 240.0f)) return fail("fps must be from 1 to 240");
    }
    if (!json.contains("keys") || !json["keys"].is_array() || json["keys"].empty()) return fail("keys must be a non-empty array");
    for (const nlohmann::json& k : json["keys"]) {
        if (!k.is_object() || !k.contains("t") || !k["t"].is_number() || !k.contains("position") || !k["position"].is_array() || k["position"].size() != 3)
            return fail("each key needs a number t and a position of three numbers");
        CameraKey key;
        key.time = k["t"].get<f32>();
        for (usize i = 0; i < 3; ++i) {
            if (!k["position"][i].is_number()) return fail("a position holds numbers");
        }
        key.position = Vec3(k["position"][0].get<f32>(), k["position"][1].get<f32>(), k["position"][2].get<f32>());
        const auto number = [&](const char* name, f32 fallback) { return k.contains(name) && k[name].is_number() ? k[name].get<f32>() : fallback; };
        key.yaw_degrees = number("yaw", 0.0f);
        key.pitch_degrees = number("pitch", 0.0f);
        key.roll_degrees = number("roll", 0.0f);
        key.fov_degrees = number("fov", 0.0f);
        if (!std::isfinite(key.time) || !std::isfinite(key.position.x) || !std::isfinite(key.position.y) || !std::isfinite(key.position.z) ||
            !std::isfinite(key.yaw_degrees) || !std::isfinite(key.pitch_degrees) || !std::isfinite(key.roll_degrees) || !std::isfinite(key.fov_degrees))
            return fail("a key holds a number that is not finite");
        if (!path.keys.empty() && !(key.time > path.keys.back().time)) return fail("key times must rise");
        path.keys.push_back(key);
    }
    out = std::move(path);
    return true;
}

CameraKey SampleCameraPath(const CameraPath& path, f32 time) {
    CameraKey out;
    if (path.keys.empty()) return out;
    if (path.keys.size() == 1 || time <= path.keys.front().time) return path.keys.front();
    if (time >= path.keys.back().time) return path.keys.back();
    usize segment = 0;
    while (segment + 2 < path.keys.size() && time >= path.keys[segment + 1].time) ++segment;
    const CameraKey& a = path.keys[segment];
    const CameraKey& b = path.keys[segment + 1];
    // The keys either side, held at the ends, for the curve's tangents.
    const CameraKey& before = segment > 0 ? path.keys[segment - 1] : a;
    const CameraKey& after = segment + 2 < path.keys.size() ? path.keys[segment + 2] : b;
    const f32 t = (time - a.time) / (b.time - a.time);
    const auto angle = [&](f32 CameraKey::*field) {
        const f32 a0 = a.*field;
        const f32 p0 = Unwrap(a0, before.*field), p2 = Unwrap(a0, b.*field), p3 = Unwrap(p2, after.*field);
        return CatmullRom(p0, a0, p2, p3, t);
    };
    out.time = time;
    out.position = Vec3(CatmullRom(before.position.x, a.position.x, b.position.x, after.position.x, t),
                        CatmullRom(before.position.y, a.position.y, b.position.y, after.position.y, t),
                        CatmullRom(before.position.z, a.position.z, b.position.z, after.position.z, t));
    out.yaw_degrees = angle(&CameraKey::yaw_degrees);
    out.pitch_degrees = angle(&CameraKey::pitch_degrees);
    out.roll_degrees = angle(&CameraKey::roll_degrees);
    out.fov_degrees = CatmullRom(before.fov_degrees, a.fov_degrees, b.fov_degrees, after.fov_degrees, t);
    return out;
}

Quaternion CameraPoseRotation(const CameraKey& pose) {
    constexpr f32 kRadians = 3.14159265358979f / 180.0f;
    const Quaternion yaw = Quaternion::FromAxisAngle(Vec3(0, 1, 0), pose.yaw_degrees * kRadians);
    const Quaternion pitch = Quaternion::FromAxisAngle(Vec3(1, 0, 0), pose.pitch_degrees * kRadians);
    const Quaternion roll = Quaternion::FromAxisAngle(Vec3(0, 0, 1), pose.roll_degrees * kRadians);
    return yaw * pitch * roll;
}

} // namespace aether::player
