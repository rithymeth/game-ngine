#include "ai/ai_debugger.h"

#include "aether/ecs/world.h"
#include "aether/scene/components.h"

namespace aether::editor {

std::string AiDebugEntityLabel(const World& world, Entity entity) {
    if (entity.IsNull()) return "-";
    if (!world.IsAlive(entity)) return "#" + std::to_string(entity.index) + " (gone)";
    if (const EntityName* name = world.GetComponent<EntityName>(entity); name && !name->value.empty()) return name->value;
    return "#" + std::to_string(entity.index);
}

std::vector<AiDebugRow> BuildAiDebugRows(const World& world, const std::vector<script::AiDebugEntry>& entries) {
    std::vector<AiDebugRow> rows;
    rows.reserve(entries.size());
    for (const script::AiDebugEntry& e : entries) {
        AiDebugRow row;
        row.entity = e.entity;
        row.name = AiDebugEntityLabel(world, e.entity);
        row.state = e.state.empty() ? "-" : e.state;
        if (e.has_perception) {
            row.target = AiDebugEntityLabel(world, e.target);
            row.known = e.known.size();
        }
        if (e.path.empty()) row.path = "none";
        else if (e.next_corner >= e.path.size()) row.path = "arrived";
        else row.path = "corner " + std::to_string(e.next_corner) + "/" + std::to_string(e.path.size() - 1);
        if (!e.path.empty() && !e.path_complete) row.path += " (partial)";
        rows.push_back(std::move(row));
    }
    return rows;
}

namespace {
void Cross(nav::NavDebugDraw& out, Vec3 p, f32 s, u32 color) {
    out.Line(p - Vec3(s, 0, 0), p + Vec3(s, 0, 0), color);
    out.Line(p - Vec3(0, 0, s), p + Vec3(0, 0, s), color);
    out.Line(p, p + Vec3(0, 2 * s, 0), color);
}
} // namespace

void BuildAiDebugOverlay(const World& world, const std::vector<script::AiDebugEntry>& entries,
                         const AiDebugOverlayOptions& options, nav::NavDebugDraw& out) {
    const Vec3 lift(0, options.lift, 0);
    for (const script::AiDebugEntry& e : entries) {
        if (!options.all_agents && !(e.entity == options.selected)) continue;
        const Transform* self = world.IsAlive(e.entity) ? world.GetComponent<Transform>(e.entity) : nullptr;
        if (!e.path.empty()) {
            for (usize i = 1; i < e.path.size(); ++i)
                out.Line(e.path[i - 1] + lift, e.path[i] + lift,
                         i <= e.next_corner ? ai_debug_colors::kPathWalked : ai_debug_colors::kPathAhead);
            if (e.next_corner < e.path.size()) {
                const Vec3 next = e.path[e.next_corner] + lift;
                if (self) out.Line(self->position + lift, next, ai_debug_colors::kNextCorner);
                Cross(out, next, options.marker * 0.6f, ai_debug_colors::kNextCorner);
            }
            Cross(out, e.goal + lift, options.marker,
                  e.path_complete ? ai_debug_colors::kGoal : ai_debug_colors::kGoalPartial);
        }
        if (e.has_perception && !e.target.IsNull() && self && world.IsAlive(e.target)) {
            if (const Transform* target = world.GetComponent<Transform>(e.target))
                out.Line(self->position + Vec3(0, 1, 0), target->position + Vec3(0, 1, 0), ai_debug_colors::kTarget);
        }
    }
}

} // namespace aether::editor
