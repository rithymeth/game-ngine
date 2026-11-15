#include "aether/destruction/destruction_system.h"

#include "aether/physics/components.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <cmath>

namespace aether::destruction {

namespace {

DestructionSystem* g_active = nullptr;

bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

Vec3 Rotate(const Quaternion& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    return v + u.Cross(u.Cross(v) + v * q.w) * 2.0f;
}

usize Axis(u8 n) { return std::clamp<usize>(n, 1, Destructible::kMaxPieces); }

bool SameEntity(Entity a, Entity b) { return a.index == b.index && a.generation == b.generation; }

std::vector<Entity> EntitiesWithDebris(const World& world) {
    std::vector<Entity> out;
    const ComponentId id = GetComponentId<Debris>();
    world.ForEachArchetype([&](const Archetype& a) {
        if (!a.Mask().test(id)) return;
        for (usize c = 0; c < a.ChunkCount(); ++c) {
            const Entity* chunk = a.EntityArray(c);
            out.insert(out.end(), chunk, chunk + a.ChunkEntityCount(c));
        }
    });
    return out;
}

} // namespace

usize PieceCount(const Destructible& d) { return Axis(d.pieces_x) * Axis(d.pieces_y) * Axis(d.pieces_z); }

DestructionSystem::DestructionSystem(World& world, PhysicsScene& scene) : world_(world), scene_(scene) { MakeActive(); }

DestructionSystem::~DestructionSystem() {
    if (g_active == this) g_active = nullptr;
}

DestructionSystem* DestructionSystem::Active() { return g_active; }
void DestructionSystem::MakeActive() { g_active = this; }

bool DestructionSystem::MarkBreak(Entity entity, const Vec3& from) {
    for (const Mark& m : pending_) {
        if (SameEntity(m.entity, entity)) return true; // already going to break
    }
    pending_.push_back({entity, from});
    return true;
}

void DestructionSystem::OnEvent(const PhysicsEvent& event) {
    if (event.type != PhysicsEventType::CollisionBegin || !world_.IsAlive(event.self)) return;
    const Destructible* d = world_.GetComponent<Destructible>(event.self);
    if (d == nullptr || !d->enabled || !(d->break_speed > 0.0f) || !(event.approach_speed >= d->break_speed)) return;
    MarkBreak(event.self, Finite(event.point) ? event.point : Vec3(0, 0, 0));
}

bool DestructionSystem::Damage(Entity entity, f32 amount, const Vec3* point) {
    if (!world_.IsAlive(entity) || !(amount > 0.0f) || !std::isfinite(amount)) return false;
    Destructible* d = world_.GetComponent<Destructible>(entity);
    if (d == nullptr || !d->enabled || !(d->health > 0.0f)) return false;
    d->health -= amount;
    if (d->health > 0.0f) return false;
    const Transform* t = world_.GetComponent<Transform>(entity);
    const Vec3 from = point != nullptr && Finite(*point) ? *point : (t != nullptr ? t->position : Vec3(0, 0, 0));
    return MarkBreak(entity, from);
}

bool DestructionSystem::Shatter(Entity entity, const Vec3* point) {
    if (!world_.IsAlive(entity)) return false;
    const Destructible* d = world_.GetComponent<Destructible>(entity);
    if (d == nullptr || !d->enabled) return false;
    const Transform* t = world_.GetComponent<Transform>(entity);
    const Vec3 from = point != nullptr && Finite(*point) ? *point : (t != nullptr ? t->position : Vec3(0, 0, 0));
    return MarkBreak(entity, from);
}

std::vector<Entity> DestructionSystem::Break(Entity entity, const Vec3& from) {
    std::vector<Entity> pieces;
    if (!world_.IsAlive(entity)) return pieces;
    const Destructible* dp = world_.GetComponent<Destructible>(entity);
    const Transform* tp = world_.GetComponent<Transform>(entity);
    const BoxCollider* bp = world_.GetComponent<BoxCollider>(entity);
    if (dp == nullptr || tp == nullptr || bp == nullptr) return pieces;
    const Destructible d = *dp; // copies: the entity goes away below
    const Transform t = *tp;
    const BoxCollider box = *bp;
    const bool has_model = world_.GetComponent<ModelRenderer>(entity) != nullptr;
    const ModelRenderer model = has_model ? *world_.GetComponent<ModelRenderer>(entity) : ModelRenderer{};

    // The whole thing's own motion, which the pieces keep a share of.
    Vec3 body_velocity(0, 0, 0);
    JPH::BodyInterface& bodies = scene_.Physics().BodyInterface();
    const JPH::BodyID source = scene_.BodyOf(entity);
    if (!source.IsInvalid() && bodies.GetMotionType(source) == JPH::EMotionType::Dynamic) {
        const JPH::Vec3 v = bodies.GetLinearVelocity(source);
        body_velocity = Vec3(v.GetX(), v.GetY(), v.GetZ());
    }

    const usize nx = Axis(d.pieces_x), ny = Axis(d.pieces_y), nz = Axis(d.pieces_z);
    const usize count = nx * ny * nz;
    const f32 piece_mass = std::max(d.mass, 0.01f) / static_cast<f32>(count);
    const Vec3 piece_scale(t.scale.x / static_cast<f32>(nx), t.scale.y / static_cast<f32>(ny), t.scale.z / static_cast<f32>(nz));
    const f32 lifetime = d.piece_lifetime > 0.0f ? d.piece_lifetime : -1.0f;
    const f32 keep = std::clamp(d.inherit_velocity, 0.0f, 1.0f);
    const f32 burst = std::isfinite(d.burst_speed) ? std::max(d.burst_speed, 0.0f) : 0.0f;

    world_.DestroyEntity(entity);
    pieces.reserve(count);
    std::vector<Vec3> velocities;
    velocities.reserve(count);
    for (usize ix = 0; ix < nx; ++ix) {
        for (usize iy = 0; iy < ny; ++iy) {
            for (usize iz = 0; iz < nz; ++iz) {
                const Vec3 local(box.center.x + box.half_extents.x * ((2.0f * static_cast<f32>(ix) + 1.0f) / static_cast<f32>(nx) - 1.0f),
                                 box.center.y + box.half_extents.y * ((2.0f * static_cast<f32>(iy) + 1.0f) / static_cast<f32>(ny) - 1.0f),
                                 box.center.z + box.half_extents.z * ((2.0f * static_cast<f32>(iz) + 1.0f) / static_cast<f32>(nz) - 1.0f));
                const Vec3 position = t.position + Rotate(t.rotation, Vec3(local.x * t.scale.x, local.y * t.scale.y, local.z * t.scale.z));
                RigidBody rb;
                rb.motion = BodyMotion::Dynamic;
                rb.mass = piece_mass;
                BoxCollider piece_box;
                piece_box.half_extents = box.half_extents; // the piece's Transform scale shrinks it
                piece_box.friction = box.friction;
                piece_box.restitution = box.restitution;
                const Entity piece = world_.CreateEntity(Transform{position, t.rotation, piece_scale}, rb, piece_box, Debris{lifetime, next_serial_++});
                if (has_model) world_.AddComponent<ModelRenderer>(piece, model);
                pieces.push_back(piece);
                Vec3 away = position - from;
                const f32 distance = away.Length();
                away = distance > 1e-4f ? away * (1.0f / distance) : Vec3(0, 1, 0);
                velocities.push_back(body_velocity * keep + away * burst);
            }
        }
    }
    // The bodies are made on a Sync; the pieces' first velocity goes on them then.
    scene_.Sync();
    for (usize i = 0; i < pieces.size(); ++i) {
        const JPH::BodyID id = scene_.BodyOf(pieces[i]);
        if (id.IsInvalid()) continue;
        bodies.SetLinearVelocity(id, JPH::Vec3(velocities[i].x, velocities[i].y, velocities[i].z));
    }
    return pieces;
}

usize DestructionSystem::DebrisCount() const { return EntitiesWithDebris(world_).size(); }

usize DestructionSystem::Update(f32 dt) {
    usize made = 0;
    std::vector<Mark> marked;
    marked.swap(pending_);
    for (const Mark& m : marked) {
        if (!world_.IsAlive(m.entity)) continue;
        const Destructible* d = world_.GetComponent<Destructible>(m.entity);
        if (d == nullptr || !d->enabled) continue;
        made += Break(m.entity, m.from).size();
    }
    // Age the debris; whatever has run out goes, and so do the oldest pieces beyond the cap.
    std::vector<Entity> debris = EntitiesWithDebris(world_);
    std::vector<std::pair<u64, Entity>> alive;
    const f32 step = std::isfinite(dt) && dt > 0.0f ? dt : 0.0f;
    for (const Entity e : debris) {
        Debris* piece = world_.GetComponent<Debris>(e);
        if (piece == nullptr) continue;
        if (piece->remaining >= 0.0f) {
            piece->remaining -= step;
            if (piece->remaining <= 0.0f) {
                world_.DestroyEntity(e);
                continue;
            }
        }
        alive.push_back({piece->serial, e});
    }
    if (alive.size() > max_debris_) {
        std::sort(alive.begin(), alive.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        const usize extra = alive.size() - max_debris_;
        for (usize i = 0; i < extra; ++i) world_.DestroyEntity(alive[i].second);
    }
    return made;
}

} // namespace aether::destruction
