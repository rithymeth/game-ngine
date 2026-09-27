#include "aether/vfx/particle_system.h"

#include "aether/ecs/component.h"
#include "aether/math/math.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"

#include <algorithm>
#include <cmath>

namespace aether {

namespace {
vfx::ParticleWorld* g_active = nullptr;
} // namespace

void ParticleSystem::Activate() { commands.push_back({ParticleSystemCommand::Kind::Activate, {}, {}, 0.0f}); }
void ParticleSystem::Deactivate() { commands.push_back({ParticleSystemCommand::Kind::Deactivate, {}, {}, 0.0f}); }
void ParticleSystem::Restart() { commands.push_back({ParticleSystemCommand::Kind::Restart, {}, {}, 0.0f}); }
void ParticleSystem::SetFloatParameter(const std::string& name, f32 value) { commands.push_back({ParticleSystemCommand::Kind::SetFloat, name, {}, value}); }
void ParticleSystem::SetVectorParameter(const std::string& name, const Vec3& value) {
    commands.push_back({ParticleSystemCommand::Kind::SetVector, name, value, 0.0f});
}
void ParticleSystem::SetColorParameter(const std::string& name, const Vec3& rgb, f32 alpha) {
    commands.push_back({ParticleSystemCommand::Kind::SetColor, name, rgb, alpha});
}
void ParticleSystem::SetAsset(const std::string& name) {
    asset = name;
    commands.push_back({ParticleSystemCommand::Kind::SetAsset, name, {}, 0.0f});
}

Entity Particles::SpawnEmitterAtLocation(const std::string& asset, const Vec3& location, const Vec3& rotation_degrees) {
    if (g_active == nullptr) return kNullEntity;
    const Quaternion q = Quaternion::FromAxisAngle({0, 1, 0}, Radians(rotation_degrees.y)) * Quaternion::FromAxisAngle({1, 0, 0}, Radians(rotation_degrees.x)) *
                         Quaternion::FromAxisAngle({0, 0, 1}, Radians(rotation_degrees.z));
    return g_active->SpawnAtLocation(asset, location, q);
}

Entity Particles::SpawnEmitterAttached(const std::string& asset, const Entity& target, const Vec3& offset) {
    return g_active != nullptr ? g_active->SpawnAttached(asset, target, offset) : kNullEntity;
}

void RegisterParticleComponents() { (void)GetComponentId<ParticleSystem>(); }

} // namespace aether

namespace aether::vfx {

namespace {
Vec3 Rotate(const Quaternion& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    const Vec3 t = u.Cross(v) * 2.0f;
    return v + t * q.w + u.Cross(t);
}

// Whether a world box is wholly outside one of the view's clip planes.
bool OffScreen(const Mat4& vp, const Vec3& lo, const Vec3& hi) {
    int outside[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 8; ++i) {
        const Vec3 c(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z);
        const Vec4 p = vp * Vec4(c.x, c.y, c.z, 1.0f);
        outside[0] += p.x < -p.w, outside[1] += p.x > p.w;
        outside[2] += p.y < -p.w, outside[3] += p.y > p.w;
        outside[4] += p.z < 0.0f, outside[5] += p.z > p.w;
    }
    return std::any_of(std::begin(outside), std::end(outside), [](int n) { return n == 8; });
}
} // namespace

ParticleWorld* ParticleWorld::Active() { return g_active; }
void ParticleWorld::MakeActive() { g_active = this; }

ParticleWorld::ParticleWorld(World& world, AssetLookup assets, const GuidIndex* guids) : world_(world), assets_(std::move(assets)), guids_(guids) {
    RegisterParticleComponents();
    MakeActive();
}

ParticleWorld::~ParticleWorld() {
    if (g_active == this) g_active = nullptr;
}

void ParticleWorld::Report(const std::string& key, const std::string& message) {
    if (reported_[key]) return;
    reported_[key] = true;
    problems_.push_back(message);
}

bool ParticleWorld::WorldPose(Entity e, EmitterPose& out) const {
    const Transform* t = world_.IsAlive(e) ? world_.GetComponent<Transform>(e) : nullptr;
    if (t == nullptr) return false;
    out.position = t->position;
    out.rotation = t->rotation;
    if (guids_ != nullptr) {
        Entity at = GetParent(world_, *guids_, e);
        for (int depth = 0; !at.IsNull() && depth < kMaxHierarchyDepth; ++depth) {
            const Transform* p = world_.GetComponent<Transform>(at);
            if (p == nullptr) break;
            out.position = p->position + Rotate(p->rotation, out.position);
            out.rotation = p->rotation * out.rotation;
            at = GetParent(world_, *guids_, at);
        }
    }
    return true;
}

std::unique_ptr<ParticleSystemInstance> ParticleWorld::Acquire(const std::string& asset, const ParticleSystemAsset& data, Entity e) {
    auto& pool = pool_[asset];
    std::unique_ptr<ParticleSystemInstance> inst;
    if (!pool.empty()) {
        inst = std::move(pool.back());
        pool.pop_back();
        inst->Restart();
        ++stats_.reused;
    } else {
        inst = std::make_unique<ParticleSystemInstance>(data, static_cast<u64>(e.index) + 1);
    }
    inst->SetCollider(collider_);
    inst->SetSpawnScale(1.0f);
    return inst;
}

void ParticleWorld::Release(Live& live) {
    if (!live.instance) return;
    auto& pool = pool_[live.asset];
    if (pool.size() < kPoolPerAsset) {
        live.instance->Stop();
        for (usize i = 0; i < live.instance->EmitterCount(); ++i) live.instance->Emitter(i).Clear();
        pool.push_back(std::move(live.instance));
    }
    live.instance.reset();
}

void ParticleWorld::SetCollider(const ParticleCollider* collider) {
    collider_ = collider;
    for (auto& [index, live] : live_) {
        if (live.instance) live.instance->SetCollider(collider);
    }
}

ParticleSystemInstance* ParticleWorld::InstanceOf(Entity e) {
    const auto it = live_.find(e.index);
    return it == live_.end() || it->second.entity != e ? nullptr : it->second.instance.get();
}
const ParticleSystemInstance* ParticleWorld::InstanceOf(Entity e) const { return const_cast<ParticleWorld*>(this)->InstanceOf(e); }

Entity ParticleWorld::SpawnAtLocation(const std::string& asset, const Vec3& location, const Quaternion& rotation) {
    ParticleSystem ps;
    ps.asset = asset;
    ps.destroy_when_finished = true;
    return world_.CreateEntity(Transform{location, rotation}, ps);
}

Entity ParticleWorld::SpawnAttached(const std::string& asset, Entity target, const Vec3& offset) {
    EmitterPose pose;
    if (!WorldPose(target, pose)) {
        Report("attach:" + std::to_string(target.index), "Spawn Emitter Attached: entity " + std::to_string(target.index) + " doesn't exist or has no Transform");
        return kNullEntity;
    }
    const Entity e = SpawnAtLocation(asset, pose.position + Rotate(pose.rotation, offset), pose.rotation);
    attachments_[e.index] = {e, target, offset, false};
    return e;
}

void ParticleWorld::Apply(Live& live, ParticleSystem& ps, bool& restart) {
    for (const ParticleSystemCommand& c : ps.commands) {
        std::string error;
        bool ok = true;
        switch (c.kind) {
        case ParticleSystemCommand::Kind::Activate:
            if (!ps.active || live.instance->Finished()) restart = true;
            break;
        case ParticleSystemCommand::Kind::Deactivate:
            live.instance->Stop();
            ps.active = false;
            restart = false;
            break;
        case ParticleSystemCommand::Kind::Restart: restart = true; break;
        case ParticleSystemCommand::Kind::SetFloat: ok = live.instance->SetParameter(c.name, ParameterValue::Float(c.f), &error); break;
        case ParticleSystemCommand::Kind::SetVector: ok = live.instance->SetParameter(c.name, ParameterValue::Vector(c.v), &error); break;
        case ParticleSystemCommand::Kind::SetColor:
            ok = live.instance->SetParameter(c.name, ParameterValue::Color({c.v.x, c.v.y, c.v.z, c.f}), &error);
            break;
        case ParticleSystemCommand::Kind::SetAsset: break; // handled when the instance is made
        }
        if (!ok) Report("param:" + live.asset + ":" + c.name, "particle system '" + live.asset + "': " + error);
    }
    ps.commands.clear();
}

void ParticleWorld::Update(f32 dt, const ParticleCamera* camera) {
    const u64 now = ++generation_;
    events_.clear();
    stats_ = {0, 0, 0, 0, 0, 0, stats_.reused};
    // Attached effects follow their targets; when a target goes, they stop spawning and finish.
    for (auto it = attachments_.begin(); it != attachments_.end();) {
        Attachment& att = it->second;
        const Entity e = att.self;
        Transform* t = world_.IsAlive(e) ? world_.GetComponent<Transform>(e) : nullptr;
        if (t == nullptr) {
            it = attachments_.erase(it);
            continue;
        }
        EmitterPose target;
        if (!att.detached && WorldPose(att.target, target)) {
            t->position = target.position + Rotate(target.rotation, att.offset);
            t->rotation = target.rotation;
        } else if (!att.detached) {
            att.detached = true;
            if (ParticleSystem* ps = world_.GetComponent<ParticleSystem>(e)) ps->Deactivate();
        }
        ++it;
    }

    std::vector<Entity> entities;
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<ParticleSystem>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            entities.insert(entities.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    std::vector<Entity> doomed;
    for (Entity e : entities) {
        ParticleSystem& ps = *world_.GetComponent<ParticleSystem>(e);
        Live& live = live_[e.index];
        if (live.instance && (live.entity != e || live.asset != ps.asset)) {
            Release(live); // a recycled entity, or a new asset
            live.instance.reset();
        }
        bool restart = false;
        if (!live.instance) {
            const ParticleSystemAsset* data = assets_ ? assets_(ps.asset) : nullptr;
            live.entity = e;
            live.asset = ps.asset;
            if (data == nullptr) {
                Report("asset:" + ps.asset, "particle system '" + ps.asset + "' couldn't be found");
                live.seen = now;
                ps.commands.clear();
                continue;
            }
            live.instance = Acquire(ps.asset, *data, e);
            EmitterPose pose;
            if (WorldPose(e, pose)) live.instance->Teleport(pose);
            ps.active = false;
            restart = ps.auto_activate;
            // Everything spawned so far was for the old instance; it starts clean.
            for (usize i = 0; i < live.instance->EmitterCount(); ++i) live.instance->Emitter(i).Stop();
        }
        live.seen = now;
        Apply(live, ps, restart);
        if (restart) {
            live.instance->Restart();
            ps.active = true;
            live.started = true;
        }
        ++stats_.systems;
        EmitterPose pose;
        if (!WorldPose(e, pose)) {
            Report("transform:" + std::to_string(e.index), "particle system '" + ps.asset + "': its entity has no Transform");
            continue;
        }
        // Distance culling and LOD.
        live.culled = false;
        f32 scale = 1.0f;
        if (camera != nullptr) {
            const f32 d = (pose.position - camera->position).Length();
            live.culled = ps.cull_distance > 0.0f && d > ps.cull_distance;
            if (ps.lod_distance > 0.0f && d > ps.lod_distance) scale = ps.lod_spawn_scale;
        }
        if (live.culled) {
            ++stats_.culled;
            continue;
        }
        if (scale < 1.0f) ++stats_.lod;
        live.instance->SetSpawnScale(scale);
        live.instance->SetPose(pose);
        const bool running = ps.active || live.instance->Count() > 0;
        if (running) {
            live.instance->Update(dt * std::max(ps.time_scale, 0.0f));
            ++stats_.simulated;
        }
        stats_.particles += live.instance->Count();
        // Done: it won't spawn more and everything has died (or it was deactivated and has emptied out).
        const bool finished = live.started && (ps.active ? live.instance->Finished() : live.instance->Count() == 0);
        if (finished) {
            ps.active = false;
            live.started = false;
            events_.push_back({e, ps.asset});
            if (ps.destroy_when_finished) doomed.push_back(e);
        }
    }
    for (Entity e : doomed) {
        auto it = live_.find(e.index);
        if (it != live_.end()) Release(it->second);
        live_.erase(e.index);
        attachments_.erase(e.index);
        world_.DestroyEntity(e);
    }
    // Components that went away: their instances back to the pool.
    for (auto it = live_.begin(); it != live_.end();) {
        if (it->second.seen == now) {
            ++it;
            continue;
        }
        Release(it->second);
        attachments_.erase(it->first);
        it = live_.erase(it);
    }
    for (const auto& [asset, pool] : pool_) stats_.pooled += pool.size();
}

void ParticleWorld::BuildRenderData(const ParticleCamera& camera, ParticleRenderData& out, const Mat4* view_projection) const {
    for (const auto& [index, live] : live_) {
        if (!live.instance || live.culled) continue;
        for (usize i = 0; i < live.instance->EmitterCount(); ++i) {
            const EmitterInstance& e = live.instance->Emitter(i);
            if (e.Count() == 0) continue;
            if (view_projection != nullptr) {
                Bounds b = e.ComputeBounds();
                if (e.Asset().settings.space == SimSpace::Local) {
                    // Turn the local box into a world box around it.
                    const f32 r = (b.max - b.min).Length() * 0.5f;
                    const Vec3 c = e.Pose().ToWorld((b.min + b.max) * 0.5f);
                    b = {c - Vec3(r, r, r), c + Vec3(r, r, r), true};
                }
                if (b.valid && OffScreen(*view_projection, b.min, b.max)) continue;
            }
            vfx::BuildRenderData(e, camera, out);
        }
    }
}

} // namespace aether::vfx
