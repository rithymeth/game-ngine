#include "aether/script/ai_api.h"

#include "aether/ai/perception.h"
#include "aether/ecs/world.h"
#include "aether/nav/components.h"
#include "aether/nav/navmesh.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>

namespace aether::script {

namespace {

bool NeedEntity(NativeCall& c, usize i, const char* api, const char* what) {
    if (c.IsEntity(i)) return true;
    c.Fail(std::string(api) + ": " + what + " must be an entity");
    return false;
}
bool NeedVector(NativeCall& c, usize i, const char* api, const char* what) {
    if (c.IsVector(i)) return true;
    c.Fail(std::string(api) + ": " + what + " must be a vector");
    return false;
}
bool NeedNumber(NativeCall& c, usize i, const char* api, const char* what) {
    if (c.IsNumber(i)) return true;
    c.Fail(std::string(api) + ": " + what + " must be a number");
    return false;
}

const nav::NavMesh* ActiveMesh() {
    const nav::NavWorld* nw = nav::NavWorld::Active();
    return nw != nullptr && nw->Ready() ? &nw->Mesh() : nullptr;
}

bool CanSee(World& world, Entity viewer, Entity target, f64 max_distance) {
    const Transform* a = world.GetComponent<Transform>(viewer);
    const Transform* b = world.GetComponent<Transform>(target);
    if (a == nullptr || b == nullptr || viewer == target) return false;
    const AIPerception* p = world.GetComponent<AIPerception>(viewer);
    const AIStimuliSource* s = world.GetComponent<AIStimuliSource>(target);
    const Vec3 eye = a->position + Vec3(0, p != nullptr ? p->eye_height : 1.6f, 0);
    const Vec3 aim = b->position + Vec3(0, s != nullptr ? s->target_height : 1.0f, 0);
    if (max_distance >= 0.0 && (aim - eye).Length() > max_distance) return false;
    const ai::PerceptionWorld* pw = ai::PerceptionWorld::Active();
    return pw == nullptr || pw->HasLineOfSight(eye, aim, viewer, target);
}

} // namespace

void AiDebugStore::SetState(Entity entity, std::string state) {
    if (state.empty()) {
        states_.erase(entity.index);
        return;
    }
    State& slot = states_[entity.index];
    slot.entity = entity;
    if (slot.text != state) slot.text = std::move(state); // a script reports every frame: copy only on change
}

const std::string* AiDebugStore::FindState(Entity entity) const {
    const auto it = states_.find(entity.index);
    return it != states_.end() && it->second.entity == entity ? &it->second.text : nullptr;
}

const AiDebugStore::CachedPath* AiDebugStore::FindPath(Entity entity) const {
    const auto it = paths_.find(entity.index);
    return it != paths_.end() && it->second.entity == entity && !it->second.points.empty() ? &it->second : nullptr;
}

std::vector<AiDebugEntry> AiDebugStore::Collect(World& world) const {
    std::vector<Entity> entities;
    for (const auto& [index, s] : states_) entities.push_back(s.entity);
    for (const auto& [index, p] : paths_) if (!p.points.empty()) entities.push_back(p.entity);
    const ComponentId perception = GetComponentId<AIPerception>();
    world.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(perception)) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* e = archetype.EntityArray(c);
            entities.insert(entities.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return a.index != b.index ? a.index < b.index : a.generation < b.generation; });
    entities.erase(std::unique(entities.begin(), entities.end()), entities.end());
    std::vector<AiDebugEntry> out;
    for (Entity e : entities) {
        if (!world.IsAlive(e)) continue;
        AiDebugEntry entry;
        entry.entity = e;
        if (const std::string* state = FindState(e)) entry.state = *state;
        if (const AIPerception* p = world.GetComponent<AIPerception>(e)) {
            entry.has_perception = true;
            entry.target = p->GetTarget();
            for (const PerceivedActor& k : p->known) entry.known.push_back(k.actor);
        }
        if (const CachedPath* path = FindPath(e)) {
            entry.path = path->points;
            entry.next_corner = std::min(path->next, path->points.size());
            entry.path_complete = path->complete;
            entry.goal = path->goal;
        }
        if (entry.state.empty() && !entry.has_perception && entry.path.empty()) continue;
        out.push_back(std::move(entry));
    }
    return out;
}

namespace {
bool g_path_cache_enabled = true;
} // namespace

void AiDebugStore::SetPathCacheEnabled(bool enabled) { g_path_cache_enabled = enabled; }
bool AiDebugStore::PathCacheEnabled() { return g_path_cache_enabled; }

std::shared_ptr<AiDebugStore> InstallAiApi(LuauHost& host) {
    LuauHost* h = &host;
    auto store = std::make_shared<AiDebugStore>();
    const auto def = [&](const char* table, const char* name, NativeFunction fn) { host.RegisterNative(table, name, std::move(fn)); };

    def("AI", "CanSee", [h](NativeCall& c) {
        if (!NeedEntity(c, 0, "AI.CanSee", "the viewer") || !NeedEntity(c, 1, "AI.CanSee", "the target")) return;
        World* world = h->BoundWorld();
        c.Return(world != nullptr && CanSee(*world, c.EntityArg(0), c.EntityArg(1), c.IsNumber(2) ? c.Number(2) : -1.0));
    });
    def("AI", "IsSightReady", [](NativeCall& c) {
        const ai::PerceptionWorld* pw = ai::PerceptionWorld::Active();
        c.Return(pw != nullptr && pw->HasLineOfSightTest());
    });
    def("AI", "GetKnown", [h](NativeCall& c) {
        if (!NeedEntity(c, 0, "AI.GetKnown", "the entity")) return;
        World* world = h->BoundWorld();
        const AIPerception* p = world != nullptr ? world->GetComponent<AIPerception>(c.EntityArg(0)) : nullptr;
        if (p == nullptr) return c.Return(ScriptValue{});
        std::vector<ScriptValue> known;
        known.reserve(p->known.size());
        for (const PerceivedActor& k : p->known) known.emplace_back(EntityRef{k.actor});
        c.ReturnList(known);
    });
    def("AI", "GetTarget", [h](NativeCall& c) {
        if (!NeedEntity(c, 0, "AI.GetTarget", "the entity")) return;
        World* world = h->BoundWorld();
        const AIPerception* p = world != nullptr ? world->GetComponent<AIPerception>(c.EntityArg(0)) : nullptr;
        const Entity t = p != nullptr ? p->GetTarget() : kNullEntity;
        c.Return(t.IsNull() ? ScriptValue{} : ScriptValue(EntityRef{t}));
    });

    def("AI", "SetDebugState", [store](NativeCall& c) {
        if (!NeedEntity(c, 0, "AI.SetDebugState", "the entity")) return;
        store->SetState(c.EntityArg(0), c.IsString(1) ? c.String(1) : std::string());
    });

    def("Nav", "IsReady", [](NativeCall& c) { c.Return(ActiveMesh() != nullptr); });
    def("Nav", "FindPath", [](NativeCall& c) {
        if (!NeedVector(c, 0, "Nav.FindPath", "the start") || !NeedVector(c, 1, "Nav.FindPath", "the goal")) return;
        const nav::NavMesh* mesh = ActiveMesh();
        nav::NavPath path;
        if (mesh == nullptr || !mesh->FindPath(c.Vector(0), c.Vector(1), path) || path.status == nav::PathStatus::None || path.points.empty())
            return c.Return(ScriptValue{});
        c.ReturnVectorList(path.points);
        c.Return(path.status == nav::PathStatus::Complete);
    });
    // Paths are cached per entity: a call re-paths only when the goal moved more
    // than kGoalMove, the mesh was rebaked (its revision moved), or the entity
    // isn't where the last call left it (a script or a reload teleported it).
    // The cache is the debug store's, so the AI debugger shows the same path.
    def("Nav", "MoveTowards", [h, store](NativeCall& c) {
        if (!NeedEntity(c, 0, "Nav.MoveTowards", "the entity") || !NeedVector(c, 1, "Nav.MoveTowards", "the goal") ||
            !NeedNumber(c, 2, "Nav.MoveTowards", "the speed") || !NeedNumber(c, 3, "Nav.MoveTowards", "dt"))
            return;
        constexpr f32 kGoalMove = 0.25f, kOffPath = 1e-3f;
        const auto none = [&] { c.Return(false); c.Return(ScriptValue{}); };
        World* world = h->BoundWorld();
        const Entity entity = c.EntityArg(0);
        Transform* t = world != nullptr ? world->GetComponent<Transform>(entity) : nullptr;
        nav::NavWorld* nw = nav::NavWorld::Active();
        const nav::NavMesh* mesh = ActiveMesh();
        if (t == nullptr || mesh == nullptr) return none();
        const Vec3 goal = c.Vector(1);
        const u64 revision = nw->Dynamic().Revision();
        AiDebugStore::CachedPath& entry = store->PathSlot(entity);
        const auto flat = [](const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.z - b.z); };
        if (!g_path_cache_enabled || entry.entity != entity || entry.mesh != mesh || entry.revision != revision || (goal - entry.goal).Length() > kGoalMove ||
            flat(t->position, entry.left_at) > kOffPath || entry.points.empty()) {
            entry = AiDebugStore::CachedPath{entity, mesh, revision, goal, t->position, 0, false, {}};
            nav::NavPath found;
            if (!mesh->FindPath(t->position, goal, found) || found.status == nav::PathStatus::None || found.points.empty()) return none();
            entry.complete = found.status == nav::PathStatus::Complete;
            entry.points = std::move(found.points);
        }
        const std::vector<Vec3>& points = entry.points;
        f32 step = static_cast<f32>(c.Number(2) * c.Number(3));
        if (!(step > 0.0f)) step = 0.0f;
        // Walk the corners; the first is the mesh point under the entity. Dropping
        // onto the mesh is free, so an entity whose origin is above its feet (a
        // box centred off the floor) still moves the full step along the ground.
        usize next = std::min(entry.next, points.size() - 1);
        Vec3 at(t->position.x, points[next].y, t->position.z);
        while (next < points.size()) {
            const Vec3 to = points[next];
            const f32 d = (to - at).Length();
            if (d > step) {
                at = at + (to - at) * (step / d);
                step = 0.0f;
                break;
            }
            at = to;
            step -= d;
            ++next;
        }
        t->position = at;
        entry.next = next;
        entry.left_at = at;
        f32 remaining = next < points.size() ? (points[next] - at).Length() : 0.0f;
        for (usize i = next + 1; i < points.size(); ++i) remaining += (points[i] - points[i - 1]).Length();
        c.Return(remaining <= 1e-3f && entry.complete);
        c.Return(static_cast<f64>(remaining));
    });
    return store;
}

} // namespace aether::script
