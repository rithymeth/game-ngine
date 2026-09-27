#pragma once

#include "aether/math/mat4.h"

#include <functional>
#include <vector>

namespace aether::vfx {

// What SceneCollision modules hit (Phase 19 step 3, §19.3): a segment (a
// particle's move this frame, world space) against the scene. The first
// hit's point and surface normal (facing back towards `from`) come back.
class ParticleCollider {
public:
    virtual ~ParticleCollider() = default;
    virtual bool Raycast(const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal) const = 0;
};

// Any raycast (the physics world's, a test's).
class FunctionCollider final : public ParticleCollider {
public:
    using Fn = std::function<bool(const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal)>;
    explicit FunctionCollider(Fn fn) : fn_(std::move(fn)) {}
    bool Raycast(const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal) const override { return fn_ && fn_(from, to, hit, normal); }

private:
    Fn fn_;
};

// Against what the camera saw: a depth buffer (clip-space depth 0..1, rows
// top to bottom) and the view-projection it was drawn with, as the GPU
// path does. Only what's on screen collides; a particle counts as hitting
// when it goes behind the surface by less than `thickness` (world units),
// so things passing well behind an object don't. The normal comes from
// neighbouring depths.
class DepthBufferCollider final : public ParticleCollider {
public:
    DepthBufferCollider(const Mat4& view_projection, i32 width, i32 height, std::vector<f32> depth, f32 thickness = 0.5f, u32 steps = 8);
    bool Raycast(const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal) const override;
    // The world point the depth buffer holds at a pixel (false off the edges).
    bool SurfaceAt(i32 x, i32 y, Vec3& out) const;

private:
    Mat4 view_projection_, inverse_;
    i32 width_, height_;
    std::vector<f32> depth_;
    f32 thickness_;
    u32 steps_;
};

// A general 4x4 inverse (identity if singular).
Mat4 Inverse(const Mat4& m);

} // namespace aether::vfx
