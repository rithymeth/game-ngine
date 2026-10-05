#pragma once

#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

#include <mutex>
#include <string>
#include <vector>

namespace aether {

// Debug drawing (Phase 23 step 3, docs/design/PHASE_SPECS.md §23.3): lines
// and shapes in the world and text in the world or on screen, from any
// thread, for one frame (duration 0) or for some seconds. Shapes become
// lines here, so whatever draws them (the renderer's debug pass, or the
// editor's ImGui overlay) only draws lines and text. C++ calls DebugDraw::*
// (or the list directly), Blueprints get the same functions as nodes
// ("Debug Draw" category) and Luau the global `Draw` table.

// RGBA as 0xAABBGGRR (ImGui's order); Rgba makes one from 0..1 floats.
u32 DebugColor(f32 r, f32 g, f32 b, f32 a = 1.0f);
u32 DebugColor(const Vec3& rgb);

struct DebugLine {
    Vec3 a, b;
    u32 color = 0xFFFFFFFFu;
    bool depth_test = true;
    f32 remaining = 0.0f; // seconds left (0: this frame only)
};

struct DebugText {
    Vec3 position;        // world position, or pixels when `screen`
    std::string text;
    u32 color = 0xFFFFFFFFu;
    bool screen = false;
    f32 remaining = 0.0f;
};

class DebugDrawList {
public:
    static DebugDrawList& Get();

    void Line(const Vec3& a, const Vec3& b, u32 color, f32 duration = 0.0f, bool depth_test = true);
    void Arrow(const Vec3& from, const Vec3& to, u32 color, f32 head = 0.25f, f32 duration = 0.0f);
    // An oriented box: 12 lines.
    void Box(const Vec3& center, const Vec3& half_extents, u32 color, f32 duration = 0.0f,
             const Quaternion& rotation = Quaternion{});
    // Three great circles of `segments` lines each.
    void Sphere(const Vec3& center, f32 radius, u32 color, f32 duration = 0.0f, u32 segments = 16);
    void Circle(const Vec3& center, const Vec3& normal, f32 radius, u32 color, f32 duration = 0.0f, u32 segments = 24);
    // Three short axis-aligned lines.
    void Point(const Vec3& p, f32 size, u32 color, f32 duration = 0.0f);
    // A coordinate frame: x red, y green, z blue.
    void Axes(const Vec3& origin, const Quaternion& rotation, f32 size, f32 duration = 0.0f);
    void Text(const Vec3& position, std::string text, u32 color, f32 duration = 0.0f);
    // Stacked top-left on screen, under the stat overlays' column.
    void ScreenText(std::string text, u32 color, f32 duration = 0.0f);

    // Ages everything by `dt` and drops what has expired; one-frame
    // items go on the Tick after they were drawn. Call once per frame
    // after drawing what has been collected.
    void Tick(f32 dt);
    void Clear();

    // What to draw now (copies, so drawing can't race with new requests).
    std::vector<DebugLine> Lines() const;
    std::vector<DebugText> Texts() const;
    usize LineCount() const;
    usize TextCount() const;

    bool enabled = true;           // the `debug.draw` CVar
    usize max_lines = 200'000;     // beyond this, new lines are dropped (and counted)
    u64 Dropped() const { return dropped_; }

private:
    void Push(DebugLine line);
    mutable std::mutex mutex_;
    std::vector<DebugLine> lines_;
    std::vector<DebugText> texts_;
    f32 screen_cursor_ = 0.0f;
    u64 dropped_ = 0;
};

// The same, as reflected static functions, so Blueprints show them as nodes
// and any language can call them: colors are 0..1 RGB.
struct DebugDraw {
    static void Line(Vec3 from, Vec3 to, Vec3 color, f32 duration);
    static void Arrow(Vec3 from, Vec3 to, Vec3 color, f32 duration);
    static void Box(Vec3 center, Vec3 half_extents, Vec3 color, f32 duration);
    static void Sphere(Vec3 center, f32 radius, Vec3 color, f32 duration);
    static void Point(Vec3 position, f32 size, Vec3 color, f32 duration);
    static void Text(Vec3 position, std::string text, Vec3 color, f32 duration);
    static void ScreenText(std::string text, Vec3 color, f32 duration);
};

Vec3 RotateByQuaternion(const Quaternion& q, const Vec3& v);

} // namespace aether

AETHER_REFLECT(aether::DebugDraw, 1,
    AETHER_METHOD(Line, Fn_BlueprintCallable, {"from", "to", "color", "duration"}),
    AETHER_METHOD(Arrow, Fn_BlueprintCallable, {"from", "to", "color", "duration"}),
    AETHER_METHOD(Box, Fn_BlueprintCallable, {"center", "half_extents", "color", "duration"}),
    AETHER_METHOD(Sphere, Fn_BlueprintCallable, {"center", "radius", "color", "duration"}),
    AETHER_METHOD(Point, Fn_BlueprintCallable, {"position", "size", "color", "duration"}),
    AETHER_METHOD(Text, Fn_BlueprintCallable, {"position", "text", "color", "duration"}),
    AETHER_METHOD(ScreenText, Fn_BlueprintCallable, {"text", "color", "duration"})
)
