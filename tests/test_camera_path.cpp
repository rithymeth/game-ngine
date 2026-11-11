#include "../player/src/camera_path.h"
#include "test_framework.h"

#include <cmath>

using namespace aether;
using namespace aether::player;

// The camera path of `aether_player --camera-path`: parsing it, and where the camera is along it.

static CameraPath Parse(const char* json) {
    CameraPath path;
    std::string error;
    AETHER_CHECK(ParseCameraPath(json, path, &error));
    return path;
}

AETHER_TEST(CameraPath_ParsesAKeyframeFileAndRefusesABadOne) {
    const CameraPath path = Parse(R"({"fps": 30, "keys": [
        {"t": 0, "position": [1, 2, 3], "yaw": 90, "pitch": -10, "fov": 50},
        {"t": 2.5, "position": [4, 5, 6]}]})");
    AETHER_CHECK(path.keys.size() == 2 && path.fps == 30.0f && path.Duration() == 2.5f);
    AETHER_CHECK(path.FrameCount() == 75);
    AETHER_CHECK(path.keys[0].position.y == 2.0f && path.keys[0].yaw_degrees == 90.0f && path.keys[0].fov_degrees == 50.0f);
    AETHER_CHECK(path.keys[1].fov_degrees == 0.0f); // no fov: the camera's own
    CameraPath refused;
    std::string error;
    AETHER_CHECK(!ParseCameraPath("not json", refused, &error) && !error.empty());
    AETHER_CHECK(!ParseCameraPath(R"({"keys": []})", refused, &error));
    AETHER_CHECK(!ParseCameraPath(R"({"keys": [{"t": 0, "position": [1, 2]}]})", refused, &error));
    AETHER_CHECK(!ParseCameraPath(R"({"keys": [{"t": 1, "position": [0,0,0]}, {"t": 1, "position": [0,0,0]}]})", refused, &error)); // times must rise
    AETHER_CHECK(!ParseCameraPath(R"({"fps": 0, "keys": [{"t": 0, "position": [0,0,0]}]})", refused, &error));
}

AETHER_TEST(CameraPath_PassesThroughItsKeysAndIsHeldAtTheEnds) {
    const CameraPath path = Parse(R"({"keys": [
        {"t": 0, "position": [0, 0, 0], "yaw": 0},
        {"t": 1, "position": [10, 0, 0], "yaw": 40},
        {"t": 2, "position": [10, 5, 0], "yaw": 80}]})");
    for (const CameraKey& key : path.keys) {
        const CameraKey at = SampleCameraPath(path, key.time);
        AETHER_CHECK(std::fabs(at.position.x - key.position.x) < 1e-4f && std::fabs(at.position.y - key.position.y) < 1e-4f && std::fabs(at.yaw_degrees - key.yaw_degrees) < 1e-3f);
    }
    AETHER_CHECK(SampleCameraPath(path, -3.0f).position.x == 0.0f && SampleCameraPath(path, 9.0f).position.y == 5.0f); // held outside the path
    const CameraKey mid = SampleCameraPath(path, 0.5f);
    AETHER_CHECK(mid.position.x > 0.0f && mid.position.x < 10.0f && mid.yaw_degrees > 0.0f && mid.yaw_degrees < 40.0f);
    // Smooth: no jump between nearby times.
    f32 largest = 0.0f;
    CameraKey previous = SampleCameraPath(path, 0.0f);
    for (int i = 1; i <= 200; ++i) {
        const CameraKey now = SampleCameraPath(path, 2.0f * static_cast<f32>(i) / 200.0f);
        largest = std::max(largest, (now.position - previous.position).Length());
        previous = now;
    }
    AETHER_CHECK(largest < 0.35f);
}

AETHER_TEST(CameraPath_AHeadingTurnsTheShortWayRound) {
    const CameraPath path = Parse(R"({"keys": [
        {"t": 0, "position": [0, 0, 0], "yaw": 350},
        {"t": 1, "position": [0, 0, 0], "yaw": 10}]})");
    const CameraKey mid = SampleCameraPath(path, 0.5f);
    // Half way between 350 and 10 is 0 (360), not 180.
    AETHER_CHECK(std::fabs(std::remainder(mid.yaw_degrees, 360.0f)) < 1.0f);
}

AETHER_TEST(CameraPath_YawTurnsLeftAndPitchLooksDown) {
    CameraKey pose;
    const Quaternion identity = CameraPoseRotation(pose);
    const auto forward = [](const Quaternion& q) {
        const Vec4 v = q.ToMat4() * Vec4(0, 0, -1, 0);
        return Vec3(v.x, v.y, v.z);
    };
    AETHER_CHECK(std::fabs(forward(identity).z + 1.0f) < 1e-4f); // an identity camera faces -Z
    pose.yaw_degrees = 90.0f;
    AETHER_CHECK(forward(CameraPoseRotation(pose)).x < -0.99f);  // turning left from -Z faces -X
    pose.yaw_degrees = 0.0f;
    pose.pitch_degrees = -30.0f;
    AETHER_CHECK(forward(CameraPoseRotation(pose)).y < -0.45f);  // a negative pitch looks down
}
