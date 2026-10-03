#include "aether/debug/debug_draw.h"

#include "aether/core/cvar.h"

#include <algorithm>
#include <cmath>

namespace aether {

namespace {
AutoCVar<bool> g_debug_draw("debug.draw", true, "Draw debug lines, shapes and text");
const int g_debug_draw_hook =
    g_debug_draw.Raw().OnChange([](const CVar& v) { DebugDrawList::Get().enabled = v.GetBool(); });
constexpr f32 kScreenLine = 16.0f;
} // namespace

u32 DebugColor(f32 r, f32 g, f32 b, f32 a) {
    const auto c = [](f32 v) { return static_cast<u32>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

u32 DebugColor(const Vec3& rgb) { return DebugColor(rgb.x, rgb.y, rgb.z, 1.0f); }

Vec3 RotateByQuaternion(const Quaternion& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    const Vec3 t = u.Cross(v) * 2.0f;
    return v + t * q.w + u.Cross(t);
}

DebugDrawList& DebugDrawList::Get() {
    static DebugDrawList list;
    return list;
}

void DebugDrawList::Push(DebugLine line) {
    if (lines_.size() >= max_lines) {
        ++dropped_;
        return;
    }
    lines_.push_back(line);
}

void DebugDrawList::Line(const Vec3& a, const Vec3& b, u32 color, f32 duration, bool depth_test) {
    if (!enabled) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Push({a, b, color, depth_test, std::max(duration, 0.0f)});
}

void DebugDrawList::Arrow(const Vec3& from, const Vec3& to, u32 color, f32 head, f32 duration) {
    if (!enabled) return;
    const Vec3 d = to - from;
    const f32 len = d.Length();
    Line(from, to, color, duration);
    if (len <= 1e-6f) return;
    const Vec3 dir = d * (1.0f / len);
    const Vec3 any = std::fabs(dir.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    const Vec3 side = dir.Cross(any).Normalized(), up = side.Cross(dir);
    const f32 h = std::min(head, len * 0.5f);
    const Vec3 base = to - dir * h;
    for (const Vec3& o : {side, side * -1.0f, up, up * -1.0f}) Line(to, base + o * (h * 0.5f), color, duration);
}

void DebugDrawList::Box(const Vec3& center, const Vec3& e, u32 color, f32 duration, const Quaternion& rotation) {
    if (!enabled) return;
    Vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        const Vec3 local((i & 1) ? e.x : -e.x, (i & 2) ? e.y : -e.y, (i & 4) ? e.z : -e.z);
        c[i] = center + RotateByQuaternion(rotation, local);
    }
    static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& edge : kEdges) Line(c[edge[0]], c[edge[1]], color, duration);
}

void DebugDrawList::Circle(const Vec3& center, const Vec3& normal, f32 radius, u32 color, f32 duration, u32 segments) {
    if (!enabled || segments < 3) return;
    const Vec3 n = normal.Normalized();
    const Vec3 any = std::fabs(n.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    const Vec3 u = n.Cross(any).Normalized(), v = n.Cross(u);
    Vec3 prev = center + u * radius;
    for (u32 i = 1; i <= segments; ++i) {
        const f32 a = 6.28318530718f * static_cast<f32>(i) / static_cast<f32>(segments);
        const Vec3 p = center + (u * std::cos(a) + v * std::sin(a)) * radius;
        Line(prev, p, color, duration);
        prev = p;
    }
}

void DebugDrawList::Sphere(const Vec3& center, f32 radius, u32 color, f32 duration, u32 segments) {
    Circle(center, Vec3(1, 0, 0), radius, color, duration, segments);
    Circle(center, Vec3(0, 1, 0), radius, color, duration, segments);
    Circle(center, Vec3(0, 0, 1), radius, color, duration, segments);
}

void DebugDrawList::Point(const Vec3& p, f32 size, u32 color, f32 duration) {
    const f32 h = size * 0.5f;
    Line(p - Vec3(h, 0, 0), p + Vec3(h, 0, 0), color, duration);
    Line(p - Vec3(0, h, 0), p + Vec3(0, h, 0), color, duration);
    Line(p - Vec3(0, 0, h), p + Vec3(0, 0, h), color, duration);
}

void DebugDrawList::Axes(const Vec3& origin, const Quaternion& rotation, f32 size, f32 duration) {
    Line(origin, origin + RotateByQuaternion(rotation, Vec3(size, 0, 0)), DebugColor(1, 0.2f, 0.2f), duration);
    Line(origin, origin + RotateByQuaternion(rotation, Vec3(0, size, 0)), DebugColor(0.2f, 1, 0.2f), duration);
    Line(origin, origin + RotateByQuaternion(rotation, Vec3(0, 0, size)), DebugColor(0.3f, 0.5f, 1), duration);
}

void DebugDrawList::Text(const Vec3& position, std::string text, u32 color, f32 duration) {
    if (!enabled) return;
    std::lock_guard<std::mutex> lock(mutex_);
    texts_.push_back({position, std::move(text), color, false, std::max(duration, 0.0f)});
}

void DebugDrawList::ScreenText(std::string text, u32 color, f32 duration) {
    if (!enabled) return;
    std::lock_guard<std::mutex> lock(mutex_);
    texts_.push_back({Vec3(8.0f, 8.0f + screen_cursor_, 0), std::move(text), color, true, std::max(duration, 0.0f)});
    screen_cursor_ += kScreenLine;
}

void DebugDrawList::Tick(f32 dt) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto expired = [dt](auto& item) {
        if (item.remaining <= 0.0f) return true;
        item.remaining -= dt;
        return false; // drawn at least once more after it was asked for
    };
    std::erase_if(lines_, expired);
    std::erase_if(texts_, expired);
    // Screen text that's left moves up to fill the gaps.
    screen_cursor_ = 0.0f;
    for (DebugText& t : texts_)
        if (t.screen) t.position.y = 8.0f + screen_cursor_, screen_cursor_ += kScreenLine;
}

void DebugDrawList::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
    texts_.clear();
    screen_cursor_ = 0.0f;
}

std::vector<DebugLine> DebugDrawList::Lines() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_;
}

std::vector<DebugText> DebugDrawList::Texts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return texts_;
}

usize DebugDrawList::LineCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_.size();
}

usize DebugDrawList::TextCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return texts_.size();
}

void DebugDraw::Line(Vec3 from, Vec3 to, Vec3 color, f32 duration) { DebugDrawList::Get().Line(from, to, DebugColor(color), duration); }
void DebugDraw::Arrow(Vec3 from, Vec3 to, Vec3 color, f32 duration) {
    DebugDrawList::Get().Arrow(from, to, DebugColor(color), 0.25f, duration);
}
void DebugDraw::Box(Vec3 center, Vec3 half_extents, Vec3 color, f32 duration) {
    DebugDrawList::Get().Box(center, half_extents, DebugColor(color), duration);
}
void DebugDraw::Sphere(Vec3 center, f32 radius, Vec3 color, f32 duration) {
    DebugDrawList::Get().Sphere(center, radius, DebugColor(color), duration);
}
void DebugDraw::Point(Vec3 position, f32 size, Vec3 color, f32 duration) {
    DebugDrawList::Get().Point(position, size, DebugColor(color), duration);
}
void DebugDraw::Text(Vec3 position, std::string text, Vec3 color, f32 duration) {
    DebugDrawList::Get().Text(position, std::move(text), DebugColor(color), duration);
}
void DebugDraw::ScreenText(std::string text, Vec3 color, f32 duration) {
    DebugDrawList::Get().ScreenText(std::move(text), DebugColor(color), duration);
}

} // namespace aether
