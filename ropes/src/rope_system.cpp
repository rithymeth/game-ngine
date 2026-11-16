#include "aether/ropes/rope_system.h"

#include "aether/physics/components.h"
#include "aether/physics/constraints.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <unordered_map>

namespace aether::ropes {

namespace {

RopeSystem* g_active = nullptr;

u64 Key(Entity e) { return (static_cast<u64>(e.generation) << 32) | e.index; }
bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// The turn that takes +Y to `dir` (a unit vector) by the short way; straight down turns half a circle about X.
Quaternion FromYTo(const Vec3& dir) {
    const f32 d = dir.y;
    if (d < -0.99999f) return Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    const Vec3 axis = Vec3(0, 1, 0).Cross(dir);
    f32 x = axis.x, y = axis.y, z = axis.z, w = 1.0f + d;
    const f32 n = std::sqrt(x * x + y * y + z * z + w * w);
    return Quaternion(x / n, y / n, z / n, w / n);
}

// What a built rope depends on: when any of it differs the rope is built again.
std::vector<f32> Signature(const Rope& r) {
    std::vector<f32> s = {r.length, static_cast<f32>(r.segments), r.direction.x, r.direction.y, r.direction.z, r.radius, r.mass, r.damping, r.pin_start ? 1.0f : 0.0f,
                          r.end_anchor.x, r.end_anchor.y, r.end_anchor.z};
    for (const u64 v : {r.start_entity.hi, r.start_entity.lo, r.end_entity.hi, r.end_entity.lo}) {
        s.push_back(static_cast<f32>(v & 0xFFFFFFu));
        s.push_back(static_cast<f32>((v >> 24) & 0xFFFFFFu));
        s.push_back(static_cast<f32>((v >> 48) & 0xFFFFu));
    }
    for (usize i = 0; i < sizeof(r.segment_model); ++i) s.push_back(static_cast<f32>(static_cast<unsigned char>(r.segment_model[i])));
    return s;
}

bool Usable(const Rope& r) {
    return std::isfinite(r.length) && r.length > 0.0f && std::isfinite(r.radius) && r.radius > 0.0f && std::isfinite(r.mass) && r.mass > 0.0f &&
           std::isfinite(r.damping) && Finite(r.direction) && r.direction.LengthSq() > 1e-12f && Finite(r.end_anchor);
}

} // namespace

struct RopeSystem::Impl {
    struct Record {
        Entity rope;
        std::vector<f32> signature;
        std::vector<Entity> segments;
        std::vector<EntityGuid> guids;
        f32 segment_length = 0.0f;
        Entity end_entity;           // the entity that was given a joint (null if none was)
        std::set<u32> cut;           // indices of segments whose joint to the one before was removed
    };
    std::unordered_map<u64, Record> records;
};

RopeSystem::RopeSystem(World& world, PhysicsScene& scene) : impl_(std::make_unique<Impl>()), world_(world), scene_(scene) { MakeActive(); }

RopeSystem::~RopeSystem() {
    if (g_active == this) g_active = nullptr;
}

RopeSystem* RopeSystem::Active() { return g_active; }
void RopeSystem::MakeActive() { g_active = this; }

usize RopeSystem::RopeCount() const { return impl_->records.size(); }

usize RopeSystem::Apply() {
    // A body that has come to rest is put to sleep, and taking a joint away does not wake it: it would stay where it is in mid-air. So whatever loses a joint is woken.
    const auto wake = [&](Entity e) {
        if (!world_.IsAlive(e)) return;
        const JPH::BodyID id = scene_.BodyOf(e);
        if (!id.IsInvalid()) scene_.Physics().BodyInterface().ActivateBody(id);
    };
    const auto tear_down = [&](Impl::Record& r) {
        for (const Entity e : r.segments) {
            if (world_.IsAlive(e)) world_.DestroyEntity(e);
        }
        if (!r.end_entity.IsNull() && world_.IsAlive(r.end_entity) && world_.HasComponent<PhysicsJoint>(r.end_entity)) {
            world_.RemoveComponent<PhysicsJoint>(r.end_entity);
            wake(r.end_entity);
        }
    };

    // 1. Take apart what no longer holds.
    for (auto it = impl_->records.begin(); it != impl_->records.end();) {
        Impl::Record& r = it->second;
        bool keep = world_.IsAlive(r.rope);
        if (keep) {
            const Rope* rope = world_.GetComponent<Rope>(r.rope);
            keep = rope != nullptr && rope->enabled && Usable(*rope) && Signature(*rope) == r.signature;
        }
        if (!keep) {
            tear_down(r);
            it = impl_->records.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Build the ropes that have none, in entity order.
    std::vector<Entity> candidates;
    const ComponentId rope_id = GetComponentId<Rope>();
    world_.ForEachArchetype([&](const Archetype& a) {
        if (!a.Mask().test(rope_id)) return;
        for (usize c = 0; c < a.ChunkCount(); ++c) {
            const Entity* chunk = a.EntityArray(c);
            candidates.insert(candidates.end(), chunk, chunk + a.ChunkEntityCount(c));
        }
    });
    std::sort(candidates.begin(), candidates.end(), [](Entity x, Entity y) { return x.index != y.index ? x.index < y.index : x.generation < y.generation; });
    // Entities by guid, for finding the end entity (the physics scene keeps its own map for the joints themselves).
    const auto find_by_guid = [&](const EntityGuid& guid) {
        Entity found = kNullEntity;
        const ComponentId id_component = GetComponentId<IdComponent>();
        world_.ForEachArchetype([&](const Archetype& a) {
            if (!a.Mask().test(id_component)) return;
            for (usize c = 0; c < a.ChunkCount(); ++c) {
                const Entity* chunk = a.EntityArray(c);
                for (u32 i = 0; i < a.ChunkEntityCount(c); ++i) {
                    const IdComponent* id = world_.GetComponent<IdComponent>(chunk[i]);
                    if (id != nullptr && id->guid == guid) found = chunk[i];
                }
            }
        });
        return found;
    };
    for (const Entity e : candidates) {
        if (impl_->records.count(Key(e)) != 0 || !world_.IsAlive(e)) continue;
        const Rope* rp = world_.GetComponent<Rope>(e);
        const Transform* tp = world_.GetComponent<Transform>(e);
        if (rp == nullptr || tp == nullptr || !rp->enabled || !Usable(*rp) || !Finite(tp->position)) continue;
        const Rope rope = *rp; // copies: building adds components, which can move the entity's data
        const Vec3 start = tp->position;
        const IdComponent* own_id = world_.GetComponent<IdComponent>(e);
        const EntityGuid rope_guid = own_id != nullptr ? own_id->guid : EntityGuid{};

        const u32 n = std::clamp<u32>(rope.segments, Rope::kMinSegments, Rope::kMaxSegments);
        const f32 s = rope.length / static_cast<f32>(n);
        const Vec3 dir = rope.direction.Normalized();
        const Quaternion turn = FromYTo(dir);
        const f32 radius = std::min(rope.radius, 0.45f * s);
        const bool visible = rope.segment_model[0] != '\0';

        Impl::Record record;
        record.rope = e;
        record.signature = Signature(rope);
        record.segment_length = s;
        for (u32 i = 0; i < n; ++i) {
            const Vec3 centre = start + dir * (s * (static_cast<f32>(i) + 0.5f));
            RigidBody rb;
            rb.motion = BodyMotion::Dynamic;
            rb.mass = rope.mass / static_cast<f32>(n);
            rb.linear_damping = rope.damping;
            rb.angular_damping = rope.damping;
            SphereCollider sphere;
            sphere.radius = radius;
            sphere.friction = 0.6f;
            const EntityGuid guid = NewEntityGuid();
            const Entity seg = world_.CreateEntity(Transform{centre, turn, visible ? Vec3(2.0f * radius, s, 2.0f * radius) : Vec3(1, 1, 1)}, rb, sphere, IdComponent{guid},
                                                   RopeSegment{rope_guid, i});
            if (visible) {
                ModelRenderer model;
                SetModelPath(model, rope.segment_model);
                world_.AddComponent<ModelRenderer>(seg, model);
            }
            // Joined to the segment before at the shared end (this segment's -Y end, in its own space); the first is pinned to the world or to start_entity.
            if (i > 0 || rope.pin_start) {
                PhysicsJoint joint;
                joint.type = JointType::Point;
                joint.anchor = Vec3(0.0f, -0.5f * s, 0.0f);
                joint.connected = i > 0 ? record.guids.back() : rope.start_entity;
                world_.AddComponent<PhysicsJoint>(seg, joint);
            }
            record.segments.push_back(seg);
            record.guids.push_back(guid);
        }
        // A body hung on the end: it gets the joint, tied on at `end_anchor`.
        if (!rope.end_entity.IsNull()) {
            const Entity end = find_by_guid(rope.end_entity);
            if (!end.IsNull() && world_.IsAlive(end) && world_.HasComponent<RigidBody>(end) && !world_.HasComponent<PhysicsJoint>(end)) {
                PhysicsJoint joint;
                joint.type = JointType::Point;
                joint.anchor = rope.end_anchor;
                joint.connected = record.guids.back();
                world_.AddComponent<PhysicsJoint>(end, joint);
                record.end_entity = end;
            }
        }
        impl_->records.emplace(Key(e), std::move(record));
    }
    return impl_->records.size();
}

std::vector<Entity> RopeSystem::Segments(Entity rope) const {
    const auto it = impl_->records.find(Key(rope));
    if (it == impl_->records.end() || !world_.IsAlive(rope)) return {};
    return it->second.segments;
}

bool RopeSystem::SegmentPosition(Entity rope, u32 index, Vec3& out) const {
    const auto it = impl_->records.find(Key(rope));
    if (it == impl_->records.end() || !world_.IsAlive(rope) || index >= it->second.segments.size() || !world_.IsAlive(it->second.segments[index])) return false;
    const Transform* t = world_.GetComponent<Transform>(it->second.segments[index]);
    if (t == nullptr) return false;
    out = t->position;
    return true;
}

f32 RopeSystem::CurrentLength(Entity rope) const {
    const auto it = impl_->records.find(Key(rope));
    if (it == impl_->records.end() || !world_.IsAlive(rope)) return 0.0f;
    const Impl::Record& r = it->second;
    f32 total = r.segment_length;
    for (usize i = 1; i < r.segments.size(); ++i) {
        const Transform* a = world_.GetComponent<Transform>(r.segments[i - 1]);
        const Transform* b = world_.GetComponent<Transform>(r.segments[i]);
        if (a != nullptr && b != nullptr) total += (b->position - a->position).Length();
    }
    return total;
}

bool RopeSystem::Cut(Entity rope, f32 fraction) {
    const auto it = impl_->records.find(Key(rope));
    if (it == impl_->records.end() || !world_.IsAlive(rope) || !std::isfinite(fraction)) return false;
    Impl::Record& r = it->second;
    const u32 n = static_cast<u32>(r.segments.size());
    if (n < 2) return false;
    const f32 f = std::clamp(fraction, 0.0f, 1.0f);
    const u32 k = std::clamp<u32>(1u + static_cast<u32>(std::lround(f * static_cast<f32>(n - 2))), 1u, n - 1u); // the joint between segments k - 1 and k
    if (r.cut.count(k) != 0 || !world_.IsAlive(r.segments[k]) || !world_.HasComponent<PhysicsJoint>(r.segments[k])) return false;
    world_.RemoveComponent<PhysicsJoint>(r.segments[k]);
    r.cut.insert(k);
    // Both halves wake: a rope that has hung at rest is asleep, and the part below the cut would otherwise stay in the air.
    for (const Entity e : r.segments) {
        if (!world_.IsAlive(e)) continue;
        const JPH::BodyID id = scene_.BodyOf(e);
        if (!id.IsInvalid()) scene_.Physics().BodyInterface().ActivateBody(id);
    }
    if (!r.end_entity.IsNull() && world_.IsAlive(r.end_entity)) {
        const JPH::BodyID id = scene_.BodyOf(r.end_entity);
        if (!id.IsInvalid()) scene_.Physics().BodyInterface().ActivateBody(id);
    }
    return true;
}

} // namespace aether::ropes
