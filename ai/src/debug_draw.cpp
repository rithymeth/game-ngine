#include "aether/ai/debug_draw.h"

#include "aether/scene/components.h"

#include <cmath>

namespace aether::ai {

namespace {
constexpr f32 kPi = 3.14159265f;

Vec3 Rotate(const Quaternion& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    const Vec3 t = u.Cross(v) * 2.0f;
    return v + t * q.w + u.Cross(t);
}

template <typename C, typename F>
void Each(World& world, F&& fn) {
    std::vector<Entity> list;
    world.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<C>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            list.insert(list.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    for (Entity e : list) fn(e, *world.GetComponent<C>(e));
}

void Arc(nav::NavDebugDraw& out, const Vec3& c, f32 radius, f32 from, f32 to, u32 color, int segments) {
    for (int i = 0; i < segments; ++i) {
        const f32 a0 = from + (to - from) * static_cast<f32>(i) / static_cast<f32>(segments);
        const f32 a1 = from + (to - from) * static_cast<f32>(i + 1) / static_cast<f32>(segments);
        out.Line(c + Vec3(std::sin(a0) * radius, 0, std::cos(a0) * radius), c + Vec3(std::sin(a1) * radius, 0, std::cos(a1) * radius), color);
    }
}
} // namespace

void DrawAIDebug(World& world, const nav::NavCrowd* crowd, const AIDebugOptions& o, nav::NavDebugDraw& out) {
    if (o.agent_paths) {
        Each<NavAgent>(world, [&](Entity e, const NavAgent& agent) {
            const Transform* t = world.GetComponent<Transform>(e);
            if (t == nullptr) return;
            Vec3 prev = t->position + Vec3(0, 0.1f, 0);
            if (crowd != nullptr) {
                for (const Vec3& c : crowd->Corners(e)) {
                    const Vec3 p = c + Vec3(0, 0.1f, 0);
                    out.Line(prev, p, ai_colors::kCorners);
                    prev = p;
                }
            }
            if (agent.IsMoving()) {
                const Vec3& g = agent.goal;
                out.Line(g + Vec3(-0.3f, 0.1f, 0), g + Vec3(0.3f, 0.1f, 0), ai_colors::kGoal);
                out.Line(g + Vec3(0, 0.1f, -0.3f), g + Vec3(0, 0.1f, 0.3f), ai_colors::kGoal);
                out.Line(g, g + Vec3(0, 1.0f, 0), ai_colors::kGoal);
            }
        });
    }
    Each<AIPerception>(world, [&](Entity e, const AIPerception& p) {
        const Transform* t = world.GetComponent<Transform>(e);
        if (t == nullptr) return;
        const Vec3 eye = t->position + Vec3(0, p.eye_height, 0);
        if (o.sight) {
            const Vec3 f = Rotate(t->rotation, Vec3(0, 0, 1));
            const f32 yaw = std::atan2(f.x, f.z);
            if (p.fov_degrees >= 360.0f) {
                Arc(out, eye, p.sight_radius, 0.0f, 2.0f * kPi, ai_colors::kSight, 32);
            } else {
                const f32 half = p.fov_degrees * 0.5f * kPi / 180.0f;
                Arc(out, eye, p.sight_radius, yaw - half, yaw + half, ai_colors::kSight, 16);
                out.Line(eye, eye + Vec3(std::sin(yaw - half), 0, std::cos(yaw - half)) * p.sight_radius, ai_colors::kSight);
                out.Line(eye, eye + Vec3(std::sin(yaw + half), 0, std::cos(yaw + half)) * p.sight_radius, ai_colors::kSight);
            }
        }
        if (o.hearing) Arc(out, t->position + Vec3(0, 0.1f, 0), p.hearing_radius, 0.0f, 2.0f * kPi, ai_colors::kHearing, 32);
        if (o.known)
            for (const PerceivedActor& k : p.known) out.Line(eye, k.location + Vec3(0, 1.0f, 0), k.visible ? ai_colors::kSeen : ai_colors::kRemembered);
    });
}

} // namespace aether::ai
