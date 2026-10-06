#pragma once

#include "aether/core/base.h"
#include "aether/math/vec.h"

#include <mutex>
#include <string>
#include <vector>

// World-space debug drawing (Phase 23 step 5). A small thread-safe
// accumulator of primitives (lines, boxes, spheres, points/crosses, text)
// that live a bounded time in the world; the viewport (and a game's debug
// overlay) reads them once per frame and renders them. This is separate from
// the per-collider physics DebugLine output (aether/physics/debug_draw.h):
// this module is the general-purpose API engine + scripts + Blueprints emit
// into, with a duration option per primitive.

namespace aether::dev {

struct DebugPrimitive {
    enum class Type : u8 { Line, Box, Sphere, Point, Text };
    Type type = Type::Line;
    Vec3 a{0, 0, 0};        // line start, box min, sphere/point center (with radius), text origin
    Vec3 b{0, 0, 0};        // line end, box max
    f32 radius = 0.0f;      // box half-extent / sphere radius / point cross half-size / text height
    u32 color = 0xFFFFFFFFu;
    std::string text;
    f32 duration = 0.0f;    // seconds; 0 = this frame only
    f32 time_added = 0.0f;  // engine time at Add, managed by Advance()
    u32 frame_added = 0;    // frame counter at Add, used to expire duration-0 prims
};

// Thread-safe accumulator of world-space debug primitives.
class DebugDraw {
public:
    void AddLine(const Vec3& a, const Vec3& b, u32 color = 0xFFFFFFFFu, f32 duration = 0.0f);
    void AddBox(const Vec3& center, const Vec3& half_extents, u32 color = 0xFFFFFFFFu, f32 duration = 0.0f);
    void AddSphere(const Vec3& center, f32 radius, u32 color = 0xFFFFFFFFu, f32 duration = 0.0f);
    void AddPoint(const Vec3& center, f32 half_cross, u32 color = 0xFFFFFFFFu, f32 duration = 0.0f);
    void AddText(const Vec3& origin, const std::string& text, u32 color = 0xFFFFFFFFu, f32 duration = 0.0f);

    // Advance the internal clock by `dt` seconds and expire duration-timed
    // primitives. Call once per frame.
    void Advance(f32 dt);

    // A copy of the live primitives, oldest-first, safe for the caller to
    // iterate without holding the lock (they are copied under it).
    std::vector<DebugPrimitive> Snapshot() const;
    void Clear();

private:
    void Push(DebugPrimitive prim);
    mutable std::mutex mutex_;
    std::vector<DebugPrimitive> prims_;
    f32 time_ = 0.0f;
    u32 frame_ = 0;
};

// A process-wide debug draw instance; the editor and scripts share it.
DebugDraw& DebugDrawInstance();

} // namespace aether::dev
