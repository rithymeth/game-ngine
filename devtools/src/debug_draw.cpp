#include "aether/dev/debug_draw.h"

#include <algorithm>

namespace aether::dev {

void DebugDraw::Push(DebugPrimitive prim) {
    std::lock_guard lock(mutex_);
    prim.time_added = time_;
    prim.frame_added = frame_;
    prims_.push_back(std::move(prim));
}

void DebugDraw::AddLine(const Vec3& a, const Vec3& b, u32 color, f32 duration) {
    DebugPrimitive p;
    p.type = DebugPrimitive::Type::Line;
    p.a = a;
    p.b = b;
    p.color = color;
    p.duration = duration;
    Push(std::move(p));
}

void DebugDraw::AddBox(const Vec3& center, const Vec3& half_extents, u32 color, f32 duration) {
    DebugPrimitive p;
    p.type = DebugPrimitive::Type::Box;
    p.a = center - half_extents;
    p.b = center + half_extents;
    p.color = color;
    p.duration = duration;
    Push(std::move(p));
}

void DebugDraw::AddSphere(const Vec3& center, f32 radius, u32 color, f32 duration) {
    DebugPrimitive p;
    p.type = DebugPrimitive::Type::Sphere;
    p.a = center;
    p.radius = radius;
    p.color = color;
    p.duration = duration;
    Push(std::move(p));
}

void DebugDraw::AddPoint(const Vec3& center, f32 half_cross, u32 color, f32 duration) {
    DebugPrimitive p;
    p.type = DebugPrimitive::Type::Point;
    p.a = center;
    p.radius = half_cross;
    p.color = color;
    p.duration = duration;
    Push(std::move(p));
}

void DebugDraw::AddText(const Vec3& origin, const std::string& text, u32 color, f32 duration) {
    DebugPrimitive p;
    p.type = DebugPrimitive::Type::Text;
    p.a = origin;
    p.text = text;
    p.color = color;
    p.duration = duration;
    Push(std::move(p));
}

void DebugDraw::Advance(f32 dt) {
    std::lock_guard lock(mutex_);
    time_ += dt;
    ++frame_;
    prims_.erase(std::remove_if(prims_.begin(), prims_.end(),
                                [&](const DebugPrimitive& p) {
                                    // Duration 0 = this frame only: survives one
                                    // Advance (the frame's render), removed on the next.
                                    if (p.duration <= 0.0f) return (frame_ - p.frame_added) >= 2;
                                    return (time_ - p.time_added) > p.duration;
                                }),
                 prims_.end());
}

std::vector<DebugPrimitive> DebugDraw::Snapshot() const {
    std::lock_guard lock(mutex_);
    return prims_;
}

void DebugDraw::Clear() {
    std::lock_guard lock(mutex_);
    prims_.clear();
}

DebugDraw& DebugDrawInstance() {
    static DebugDraw instance;
    return instance;
}

} // namespace aether::dev
