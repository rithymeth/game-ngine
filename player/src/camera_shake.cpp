#include "aether/player/camera_shake.h"

#include "aether/scene/components.h"

#include <algorithm>
#include <cmath>

namespace aether::player {

namespace {

// A smooth wobble in about [-1, 1]: two sines at unrelated rates, a different phase for each channel.
f32 Wobble(f64 t, f32 seed) {
    return 0.6f * std::sin(static_cast<f32>(t) + seed) + 0.4f * std::sin(static_cast<f32>(t) * 1.7f + seed * 2.3f);
}

} // namespace

void CameraShake::AddTrauma(f32 amount) {
    if (!std::isfinite(amount) || amount <= 0.0f) return;
    trauma_ = std::min(1.0f, trauma_ + amount);
}

CameraShake::Pose CameraShake::Evaluate() const {
    const f32 shake = trauma_ * trauma_;
    const f64 t = time_ * settings_.frequency;
    Pose pose;
    pose.offset = Vec3(Wobble(t, 0.0f), Wobble(t, 1.3f), Wobble(t, 2.6f)) * (settings_.max_offset * shake);
    pose.angles_degrees = Vec3(Wobble(t, 3.9f), Wobble(t, 5.2f), Wobble(t, 6.5f)) * (settings_.max_angle_degrees * shake);
    return pose;
}

void CameraShake::Apply(World& world, Entity camera, f32 dt) {
    Restore(world);
    if (trauma_ <= 0.0f || !world.IsAlive(camera)) return;
    Transform* transform = world.GetComponent<Transform>(camera);
    if (!transform) return;
    time_ += dt;
    const Pose pose = Evaluate();
    constexpr f32 kRadians = 3.14159265358979f / 180.0f;
    original_ = *transform;
    applied_ = camera;
    const Mat4 axes = transform->rotation.ToMat4();
    const Vec3 world_offset(axes.cols[0].x * pose.offset.x + axes.cols[1].x * pose.offset.y + axes.cols[2].x * pose.offset.z,
                            axes.cols[0].y * pose.offset.x + axes.cols[1].y * pose.offset.y + axes.cols[2].y * pose.offset.z,
                            axes.cols[0].z * pose.offset.x + axes.cols[1].z * pose.offset.y + axes.cols[2].z * pose.offset.z);
    transform->position = transform->position + world_offset;
    const Quaternion tilt = Quaternion::FromAxisAngle(Vec3(0, 1, 0), pose.angles_degrees.y * kRadians) *
                            Quaternion::FromAxisAngle(Vec3(1, 0, 0), pose.angles_degrees.x * kRadians) *
                            Quaternion::FromAxisAngle(Vec3(0, 0, 1), pose.angles_degrees.z * kRadians);
    transform->rotation = (transform->rotation * tilt).Normalized();
    trauma_ = std::max(0.0f, trauma_ - settings_.decay * dt);
}

void CameraShake::Restore(World& world) {
    if (applied_ == kNullEntity) return;
    if (world.IsAlive(applied_))
        if (Transform* transform = world.GetComponent<Transform>(applied_)) *transform = original_;
    applied_ = kNullEntity;
}

void CameraShake::Reset() {
    applied_ = kNullEntity;
    trauma_ = 0.0f;
}

} // namespace aether::player
