#include "aether/ai/perception.h"

#include "aether/ai/bt_world.h"
#include "aether/nav/navmesh.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <cmath>

namespace aether {

namespace {
ai::PerceptionWorld* g_active = nullptr;

Vec3 Rotate(const Quaternion& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    const Vec3 t = u.Cross(v) * 2.0f;
    return v + t * q.w + u.Cross(t);
}
} // namespace

const PerceivedActor* AIPerception::Find(Entity actor) const {
    for (const PerceivedActor& k : known)
        if (k.actor == actor) return &k;
    return nullptr;
}
bool AIPerception::CanSee(const Entity& actor) const {
    const PerceivedActor* k = Find(actor);
    return k != nullptr && k->visible;
}
bool AIPerception::IsAwareOf(const Entity& actor) const { return Find(actor) != nullptr; }
Vec3 AIPerception::GetLastKnownLocation(const Entity& actor) const {
    const PerceivedActor* k = Find(actor);
    return k != nullptr ? k->location : Vec3();
}
Entity AIPerception::GetTarget() const {
    const PerceivedActor* best = nullptr;
    for (const PerceivedActor& k : known) {
        if (best == nullptr) {
            best = &k;
        } else if (k.visible != best->visible) {
            if (k.visible) best = &k;
        } else if (k.visible ? k.distance < best->distance : k.age < best->age) {
            best = &k;
        }
    }
    return best != nullptr ? best->actor : kNullEntity;
}

void Perception::ReportNoise(const Vec3& location, f32 loudness, const Entity& instigator) {
    if (g_active != nullptr) g_active->ReportNoise(location, loudness, instigator);
}
void Perception::ReportDamage(const Entity& victim, const Entity& instigator, f32 amount) {
    if (g_active != nullptr) g_active->ReportDamage(victim, instigator, amount);
}

void RegisterPerceptionComponents() {
    RegisterBehaviorTreeComponents();
    (void)GetComponentId<AIPerception>();
    (void)GetComponentId<AIStimuliSource>();
}

} // namespace aether

namespace aether::ai {

PerceptionWorld::PerceptionWorld(World& world) : world_(world) {
    RegisterPerceptionComponents();
    MakeActive();
}

PerceptionWorld::~PerceptionWorld() {
    if (g_active == this) g_active = nullptr;
}

PerceptionWorld* PerceptionWorld::Active() { return g_active; }
void PerceptionWorld::MakeActive() { g_active = this; }

const char* PerceptionWorld::SenseName(PerceptionSense sense) {
    switch (sense) {
    case PerceptionSense::Sight: return "Sight";
    case PerceptionSense::Hearing: return "Hearing";
    case PerceptionSense::Damage: return "Damage";
    }
    return "";
}

void PerceptionWorld::UseNavMeshLineOfSight(const nav::NavMesh& mesh) {
    los_ = [this, &mesh](const Vec3&, const Vec3&, Entity looker, Entity target) {
        const Transform* a = world_.GetComponent<Transform>(looker);
        const Transform* b = world_.GetComponent<Transform>(target);
        if (a == nullptr || b == nullptr) return false;
        Vec3 hit;
        return !mesh.Raycast(a->position, b->position, hit);
    };
}

void PerceptionWorld::ReportNoise(const Vec3& location, f32 loudness, Entity instigator) { noises_.push_back({location, loudness, instigator}); }
void PerceptionWorld::ReportDamage(Entity victim, Entity instigator, f32 amount) { damages_.push_back({victim, instigator, amount}); }

void PerceptionWorld::Sense(Entity perceiver, AIPerception& p, Entity actor, const Vec3& at, PerceptionSense sense, f32 strength) {
    auto it = std::find_if(p.known.begin(), p.known.end(), [&](const PerceivedActor& k) { return k.actor == actor; });
    if (it == p.known.end()) {
        p.known.push_back({actor, at, 0.0f, false, sense, strength, 0.0f});
        it = p.known.end() - 1;
    }
    it->location = at;
    it->age = 0.0f;
    it->last_sense = sense;
    it->strength = strength;
    if (sense == PerceptionSense::Sight) it->visible = true;
    else events_.push_back({perceiver, actor, sense, true, false, at}); // sight announces only changes (in Update)
}

void PerceptionWorld::SyncBlackboard(Entity perceiver, const AIPerception& p) {
    if (p.target_key.empty() && p.location_key.empty()) return;
    BehaviorTreeComponent* bt = world_.GetComponent<BehaviorTreeComponent>(perceiver);
    if (bt == nullptr) return;
    const Entity target = p.GetTarget();
    if (!p.target_key.empty()) target.IsNull() ? bt->ClearValue(p.target_key) : bt->SetEntity(p.target_key, target);
    if (!p.location_key.empty()) target.IsNull() ? bt->ClearValue(p.location_key) : bt->SetVector(p.location_key, p.GetLastKnownLocation(target));
}

void PerceptionWorld::Update(f32 dt) {
    events_.clear();
    const auto with = [&](ComponentId id) {
        std::vector<Entity> out;
        world_.ForEachArchetype([&](Archetype& archetype) {
            if (!archetype.Mask().test(id)) return;
            for (usize c = 0; c < archetype.ChunkCount(); ++c) {
                Entity* e = archetype.EntityArray(c);
                out.insert(out.end(), e, e + archetype.ChunkEntityCount(c));
            }
        });
        return out;
    };
    const std::vector<Entity> perceivers = with(GetComponentId<AIPerception>());
    const std::vector<Entity> sources = with(GetComponentId<AIStimuliSource>());
    const auto team_of = [&](Entity e) {
        if (const AIStimuliSource* s = world_.GetComponent<AIStimuliSource>(e)) return s->team;
        if (const AIPerception* q = world_.GetComponent<AIPerception>(e)) return q->team;
        return 0;
    };

    for (Entity self : perceivers) {
        AIPerception& p = *world_.GetComponent<AIPerception>(self);
        const Transform* me = world_.GetComponent<Transform>(self);
        // Age what it knows; drop what went.
        std::vector<Entity> was_visible;
        for (auto it = p.known.begin(); it != p.known.end();) {
            if (!world_.IsAlive(it->actor)) {
                events_.push_back({self, it->actor, it->last_sense, false, true, it->location});
                it = p.known.erase(it);
                continue;
            }
            it->age += dt;
            if (it->visible) was_visible.push_back(it->actor);
            it->visible = false;
            ++it;
        }
        if (me != nullptr) {
            const Vec3 eye = me->position + Vec3(0, p.eye_height, 0);
            Vec3 fwd = Rotate(me->rotation, Vec3(0, 0, 1));
            fwd.y = 0.0f;
            const f32 fwd_len = fwd.Length();
            const f32 half_cos = std::cos(std::min(p.fov_degrees, 360.0f) * 0.5f * 3.14159265f / 180.0f);
            for (Entity s : sources) {
                if (s == self) continue;
                const AIStimuliSource& src = *world_.GetComponent<AIStimuliSource>(s);
                const Transform* t = world_.GetComponent<Transform>(s);
                if (!src.visible || t == nullptr || !Hostile(p, src.team)) continue;
                const Vec3 aim = t->position + Vec3(0, src.target_height, 0);
                const Vec3 d = aim - eye;
                const bool tracked = std::find(was_visible.begin(), was_visible.end(), s) != was_visible.end();
                const f32 radius = tracked ? std::max(p.lose_sight_radius, p.sight_radius) : p.sight_radius;
                if (d.Length() > radius) continue;
                const Vec3 flat(d.x, 0, d.z);
                const f32 flat_len = flat.Length();
                if (p.fov_degrees < 360.0f && flat_len > 1e-4f && fwd_len > 1e-4f && flat.Dot(fwd) / (flat_len * fwd_len) < half_cos) continue;
                if (los_ && !los_(eye, aim, self, s)) continue;
                Sense(self, p, s, t->position, PerceptionSense::Sight, 1.0f);
                if (!tracked) events_.push_back({self, s, PerceptionSense::Sight, true, false, t->position});
            }
            for (const Noise& n : noises_) {
                if (n.instigator == self || n.instigator.IsNull() || !world_.IsAlive(n.instigator) || !Hostile(p, team_of(n.instigator))) continue;
                if ((n.location - me->position).Length() <= p.hearing_radius * n.loudness) Sense(self, p, n.instigator, n.location, PerceptionSense::Hearing, n.loudness);
            }
        }
        for (const Damage& d : damages_) {
            if (d.victim != self || d.instigator.IsNull() || d.instigator == self || !world_.IsAlive(d.instigator)) continue;
            const Transform* t = world_.GetComponent<Transform>(d.instigator);
            Sense(self, p, d.instigator, t != nullptr ? t->position : (me != nullptr ? me->position : Vec3()), PerceptionSense::Damage, d.amount);
        }
        // Lost sight, forgotten, and distances.
        for (auto it = p.known.begin(); it != p.known.end();) {
            if (!it->visible && std::find(was_visible.begin(), was_visible.end(), it->actor) != was_visible.end())
                events_.push_back({self, it->actor, PerceptionSense::Sight, false, false, it->location});
            if (!it->visible && p.forget_after > 0.0f && it->age > p.forget_after) {
                events_.push_back({self, it->actor, it->last_sense, false, true, it->location});
                it = p.known.erase(it);
                continue;
            }
            it->distance = me != nullptr ? (it->location - me->position).Length() : 0.0f;
            ++it;
        }
        SyncBlackboard(self, p);
    }
    noises_.clear();
    damages_.clear();
}

} // namespace aether::ai
