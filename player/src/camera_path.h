#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"

#include <string>
#include <vector>

// A camera flying along keyframes, for recording a cinematic: `aether_player --camera-path path.json --record-dir frames/` moves the scene's camera
// along the path one frame at a time (game time advances 1 / fps a frame, so water, wind and everything that moves follows the film's clock) and
// writes every frame. The path is smooth: position, heading, tilt and field of view go through a Catmull-Rom curve, so the camera does not stop at
// each key, and a heading that crosses 360 degrees turns the short way round.
//
//   { "fps": 24, "keys": [ { "t": 0, "position": [x, y, z], "yaw": 180, "pitch": -8, "fov": 50 }, ... ] }
//
// yaw turns about +Y (0 faces -Z, as an identity camera does; positive turns left), pitch about the camera's own x (negative looks down), roll is
// optional, fov is optional (absent or 0 keeps the camera's own). Times are seconds and must rise.
namespace aether::player {

struct CameraKey {
    f32 time = 0.0f;
    Vec3 position;
    f32 yaw_degrees = 0.0f, pitch_degrees = 0.0f, roll_degrees = 0.0f;
    f32 fov_degrees = 0.0f; // 0: leave the camera's
};

struct CameraPath {
    f32 fps = 24.0f;
    std::vector<CameraKey> keys;
    f32 Duration() const { return keys.empty() ? 0.0f : keys.back().time; }
    u32 FrameCount() const; // frames to record: Duration() * fps, rounded up, at least 1
};

bool ParseCameraPath(const std::string& json, CameraPath& out, std::string* error = nullptr);

// The camera's pose at `time` (held at the first and last key outside the path).
CameraKey SampleCameraPath(const CameraPath& path, f32 time);
// The rotation of a pose: yaw about Y, then pitch about the camera's x, then roll about its view axis.
Quaternion CameraPoseRotation(const CameraKey& pose);

} // namespace aether::player
