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
            row.awareness = e.target_awareness;
            row.alerted = e.target_alerted;
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
            if (const Transform* target = world.GetComponent<Transform>(e.target)) {
                // Perception 2.0: orange while only suspicious, red once alerted
                // (and red as before when awareness hasn't been established).
                const u32 color = e.target_alerted ? ai_debug_colors::kTarget
                                                   : (e.target_awareness > 0.0f ? ai_debug_colors::kTargetSuspicious
                                                                               : ai_debug_colors::kTarget);
                out.Line(self->position + Vec3(0, 1, 0), target->position + Vec3(0, 1, 0), color);
            }
        }
    }
}

std::vector<UtilityDebugRow> BuildUtilityDebug(const ai::UtilityBrain& brain, const ai::Blackboard& blackboard) {
    std::vector<UtilityDebugRow> rows;
    rows.reserve(brain.Actions().size());
    for (const ai::UtilityAction& action : brain.Actions()) {
        UtilityDebugRow row;
        row.action = action.name;
        row.score = action.Score(blackboard);
        row.considerations = action.considerations.size();
        rows.push_back(std::move(row));
    }
    const ai::UtilityBrain::Choice choice = brain.Evaluate(blackboard);
    if (choice.valid && choice.index < rows.size()) rows[choice.index].chosen = true;
    return rows;
}

StateTreeDebug BuildStateTreeDebug(const ai::StateTree& tree, const ai::StateTree::TickResult& last_tick) {
    StateTreeDebug out;
    out.active_path = tree.ActivePath();
    out.leaf = tree.CurrentLeaf();
    if (out.leaf.empty()) out.leaf = "-";
    out.last_transitions = last_tick.transitions;
    out.loop_limited = last_tick.loop_limited;
    return out;
}

std::vector<GoapDebugRow> BuildGoapDebug(const ai::GoapPlan& plan, std::span<const ai::GoapAction> actions) {
    std::vector<GoapDebugRow> rows;
    rows.reserve(plan.steps.size());
    for (usize i = 0; i < plan.steps.size(); ++i) {
        GoapDebugRow row;
        row.step = static_cast<u32>(i);
        row.action = plan.steps[i];
        if (plan.steps[i] < actions.size()) row.name = actions[plan.steps[i]].name;
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace aether::editor
