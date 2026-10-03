#include "aether/physics/debug_draw.h"

#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <algorithm>
#include <cmath>

namespace aether {

namespace {

constexpr f32 kTwoPi = 6.28318530718f;
constexpr int kSegments = 24;

Vec3 Rotate(const Quaternion& q, const Vec3& v) {
    // v' = v + 2w (u × v) + 2 u × (u × v), u = q.xyz
    const Vec3 u(q.x, q.y, q.z);
    auto cross = [](const Vec3& a, const Vec3& b) { return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); };
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

Vec3 Axis(u8 axis) { return axis == 0 ? Vec3(1, 0, 0) : axis == 1 ? Vec3(0, 1, 0) : Vec3(0, 0, 1); }

struct Pose {
    Vec3 position;
    Quaternion rotation;
    Vec3 World(const Vec3& local) const { return position + Rotate(rotation, local); }
};

Pose PoseOf(const World& world, Entity e) {
    const Transform* t = world.GetComponent<Transform>(e);
    return t != nullptr ? Pose{t->position, t->rotation} : Pose{Vec3(0, 0, 0), Quaternion::Identity()};
}

void Line(std::vector<DebugLine>& out, const Pose& pose, const Vec3& a, const Vec3& b, u32 color) {
    out.push_back({pose.World(a), pose.World(b), color});
}

void Box(std::vector<DebugLine>& out, const Pose& pose, const Vec3& center, const Vec3& h, u32 color) {
    Vec3 c[8];
    for (int i = 0; i < 8; ++i) c[i] = center + Vec3((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z);
    for (int i = 0; i < 8; ++i) {
        for (int bit : {1, 2, 4}) {
            if ((i & bit) == 0) Line(out, pose, c[i], c[i | bit], color);
        }
    }
}

// A circle of `radius` around `center` in the plane of axes a and b; `arc`
// is how much of it (1 = whole, 0.5 = half), starting along a.
void Circle(std::vector<DebugLine>& out, const Pose& pose, const Vec3& center, const Vec3& a, const Vec3& b, f32 radius, u32 color,
            f32 arc = 1.0f) {
    const int n = std::max(2, static_cast<int>(kSegments * arc));
    auto point = [&](int i) {
        const f32 angle = kTwoPi * arc * static_cast<f32>(i) / static_cast<f32>(n);
        return center + a * (std::cos(angle) * radius) + b * (std::sin(angle) * radius);
    };
    for (int i = 0; i < n; ++i) Line(out, pose, point(i), point(i + 1), color);
}

void Sphere(std::vector<DebugLine>& out, const Pose& pose, const Vec3& center, f32 r, u32 color) {
    const Vec3 x(1, 0, 0), y(0, 1, 0), z(0, 0, 1);
    Circle(out, pose, center, x, y, r, color);
    Circle(out, pose, center, x, z, r, color);
    Circle(out, pose, center, y, z, r, color);
}

// An upright capsule: `bottom` is the lowest point, `height` the whole length.
void Capsule(std::vector<DebugLine>& out, const Pose& pose, const Vec3& bottom, f32 radius, f32 height, u32 color) {
    const f32 half = std::max(0.0f, 0.5f * height - radius);
    const Vec3 mid = bottom + Vec3(0, 0.5f * height, 0);
    const Vec3 top = mid + Vec3(0, half, 0), low = mid - Vec3(0, half, 0);
    const Vec3 x(1, 0, 0), y(0, 1, 0), z(0, 0, 1);
    Circle(out, pose, top, x, z, radius, color);
    Circle(out, pose, low, x, z, radius, color);
    for (const Vec3& side : {x, x * -1.0f, z, z * -1.0f}) Line(out, pose, low + side * radius, top + side * radius, color);
    Circle(out, pose, top, x, y, radius, color, 0.5f);          // upper cap, in XY and ZY
    Circle(out, pose, top, z, y, radius, color, 0.5f);
    Circle(out, pose, low, x, y * -1.0f, radius, color, 0.5f); // lower cap
    Circle(out, pose, low, z, y * -1.0f, radius, color, 0.5f);
}

// Edges of a body's triangles (convex hulls, meshes), in world space.
bool BodyTriangles(const PhysicsScene& scene, Entity e, u32 color, std::vector<DebugLine>& out) {
    const JPH::BodyID id = scene.BodyOf(e);
    if (id.IsInvalid()) return false;
    JPH::BodyLockRead lock(scene.Physics().System().GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return false;
    const JPH::Body& body = lock.GetBody();
    const JPH::Shape& shape = *body.GetShape();
    const JPH::RMat44 com = body.GetCenterOfMassTransform();
    JPH::Shape::GetTrianglesContext context;
    shape.GetTrianglesStart(context, JPH::AABox::sBiggest(), com.GetTranslation(), com.GetQuaternion(), JPH::Vec3::sReplicate(1.0f));
    JPH::Float3 vertices[JPH::Shape::cGetTrianglesMinTrianglesRequested * 3];
    for (;;) {
        const int count = shape.GetTrianglesNext(context, JPH::Shape::cGetTrianglesMinTrianglesRequested, vertices);
        if (count == 0) break;
        for (int t = 0; t < count; ++t) {
            for (int k = 0; k < 3; ++k) {
                const JPH::Float3& a = vertices[t * 3 + k];
                const JPH::Float3& b = vertices[t * 3 + (k + 1) % 3];
                out.push_back({Vec3(a.x, a.y, a.z), Vec3(b.x, b.y, b.z), color});
            }
        }
    }
    return true;
}

void Crosses(std::vector<DebugLine>& out, const Pose& pose, const Vec3& center, const std::vector<Vec3>& points, u32 color) {
    const f32 s = 0.05f;
    for (const Vec3& p : points) {
        const Vec3 q = center + p;
        Line(out, pose, q - Vec3(s, 0, 0), q + Vec3(s, 0, 0), color);
        Line(out, pose, q - Vec3(0, s, 0), q + Vec3(0, s, 0), color);
        Line(out, pose, q - Vec3(0, 0, s), q + Vec3(0, 0, s), color);
    }
}

std::vector<Entity> With(const World& world, ComponentMask any) {
    std::vector<Entity> out;
    world.ForEachArchetype([&](Archetype& archetype) {
        if ((archetype.Mask() & any).none()) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            out.insert(out.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    std::sort(out.begin(), out.end(), [](Entity a, Entity b) { return a.index < b.index; });
    return out;
}

bool IsTrigger(const World& world, Entity e) {
    auto trig = [](const auto* c) { return c != nullptr && c->is_trigger; };
    return trig(world.GetComponent<BoxCollider>(e)) || trig(world.GetComponent<SphereCollider>(e)) ||
           trig(world.GetComponent<CapsuleCollider>(e)) || trig(world.GetComponent<ConvexCollider>(e)) ||
           trig(world.GetComponent<MeshCollider>(e)) || trig(world.GetComponent<HeightfieldCollider>(e));
}

} // namespace

void DrawColliderWireframe(const World& world, const PhysicsScene* scene, Entity e, u32 color, std::vector<DebugLine>& out) {
    const Pose pose = PoseOf(world, e);
    if (const BoxCollider* c = world.GetComponent<BoxCollider>(e)) Box(out, pose, c->center, c->half_extents, color);
    if (const SphereCollider* c = world.GetComponent<SphereCollider>(e)) Sphere(out, pose, c->center, c->radius, color);
    if (const CapsuleCollider* c = world.GetComponent<CapsuleCollider>(e)) {
        Capsule(out, pose, c->center - Vec3(0, 0.5f * c->height, 0), c->radius, c->height, color);
    }
    if (const HeightfieldCollider* c = world.GetComponent<HeightfieldCollider>(e)) {
        // The footprint's bounding box: drawing every triangle of a terrain would flood the lines.
        if (c->width > 0 && c->depth > 0 && c->heights.size() == static_cast<usize>(c->width) * c->depth) {
            const auto [lo, hi] = std::minmax_element(c->heights.begin(), c->heights.end());
            const f32 y0 = std::min(*lo * c->vertical_scale, *hi * c->vertical_scale);
            const f32 y1 = std::max(*lo * c->vertical_scale, *hi * c->vertical_scale);
            const Vec3 half(0.5f * static_cast<f32>(c->width - 1) * c->cell_size, 0.5f * (y1 - y0),
                            0.5f * static_cast<f32>(c->depth - 1) * c->cell_size);
            Box(out, pose, c->center + Vec3(half.x, 0.5f * (y0 + y1), half.z), half, color);
        }
    }
    const ConvexCollider* convex = world.GetComponent<ConvexCollider>(e);
    const MeshCollider* mesh = world.GetComponent<MeshCollider>(e);
    if (convex != nullptr || mesh != nullptr) {
        // The body's own triangles show exactly what collides (hulls included).
        // A compound's primitives were drawn above too; that overlap is harmless.
        if (scene == nullptr || !BodyTriangles(*scene, e, color, out)) {
            if (convex != nullptr) Crosses(out, pose, convex->center, convex->points, color);
            if (mesh != nullptr) Crosses(out, pose, mesh->center, mesh->vertices, color);
        }
    }
}

void DrawPhysicsDebug(const World& world, const PhysicsScene* scene, const PhysicsDebugOptions& options, std::vector<DebugLine>& out) {
    if (!options.enabled) return;
    ComponentMask colliders;
    colliders.set(GetComponentId<BoxCollider>());
    colliders.set(GetComponentId<SphereCollider>());
    colliders.set(GetComponentId<CapsuleCollider>());
    colliders.set(GetComponentId<ConvexCollider>());
    colliders.set(GetComponentId<MeshCollider>());
    colliders.set(GetComponentId<HeightfieldCollider>());
    for (Entity e : With(world, colliders)) {
        const bool trigger = IsTrigger(world, e);
        if (trigger ? !options.triggers : !options.colliders) continue;
        u32 color = debug_colors::kStatic;
        if (trigger) {
            color = debug_colors::kTrigger;
        } else if (const RigidBody* rb = world.GetComponent<RigidBody>(e)) {
            if (rb->motion == BodyMotion::Kinematic) color = debug_colors::kKinematic;
            if (rb->motion == BodyMotion::Dynamic) {
                color = debug_colors::kDynamic;
                const JPH::BodyID id = scene != nullptr ? scene->BodyOf(e) : JPH::BodyID();
                if (options.dim_sleeping && !id.IsInvalid() && !scene->Physics().BodyInterface().IsActive(id)) {
                    color = debug_colors::kSleeping;
                }
            }
        }
        DrawColliderWireframe(world, scene, e, color, out);
    }
    if (options.characters) {
        ComponentMask characters;
        characters.set(GetComponentId<CharacterMovement>());
        for (Entity e : With(world, characters)) {
            const CharacterMovement& cm = *world.GetComponent<CharacterMovement>(e);
            const Pose feet{PoseOf(world, e).position, Quaternion::Identity()}; // characters stay upright
            Capsule(out, feet, Vec3(0, 0, 0), cm.radius, cm.height, debug_colors::kCharacter);
        }
    }
    if (options.contacts && scene != nullptr) {
        const Pose world_space{Vec3(0, 0, 0), Quaternion::Identity()};
        for (const ContactEvent& c : scene->Physics().Contacts()) {
            if (c.type == ContactType::End) continue;
            const f32 s = 0.05f;
            Line(out, world_space, c.point - Vec3(s, 0, 0), c.point + Vec3(s, 0, 0), debug_colors::kContact);
            Line(out, world_space, c.point - Vec3(0, 0, s), c.point + Vec3(0, 0, s), debug_colors::kContact);
            Line(out, world_space, c.point, c.point + c.normal * 0.3f, debug_colors::kContact);
        }
    }
}

std::vector<ColliderHandle> ColliderHandles(const World& world, Entity e) {
    std::vector<ColliderHandle> out;
    const Pose pose = PoseOf(world, e);
    auto add = [&](ColliderHandleKind kind, u8 axis, i8 sign, const Vec3& local_point) {
        ColliderHandle h;
        h.kind = kind;
        h.axis = axis;
        h.sign = sign;
        h.position = pose.World(local_point);
        h.direction = Rotate(pose.rotation, Axis(axis) * static_cast<f32>(sign));
        out.push_back(h);
    };
    if (const BoxCollider* c = world.GetComponent<BoxCollider>(e)) {
        const f32 half[3] = {c->half_extents.x, c->half_extents.y, c->half_extents.z};
        for (u8 axis = 0; axis < 3; ++axis) {
            for (i8 sign : {1, -1}) add(ColliderHandleKind::BoxFace, axis, sign, c->center + Axis(axis) * (half[axis] * sign));
        }
    }
    if (const SphereCollider* c = world.GetComponent<SphereCollider>(e)) {
        for (u8 axis = 0; axis < 3; ++axis) {
            for (i8 sign : {1, -1}) add(ColliderHandleKind::SphereRadius, axis, sign, c->center + Axis(axis) * (c->radius * sign));
        }
    }
    if (const CapsuleCollider* c = world.GetComponent<CapsuleCollider>(e)) {
        for (u8 axis : {u8{0}, u8{2}}) {
            for (i8 sign : {1, -1}) add(ColliderHandleKind::CapsuleRadius, axis, sign, c->center + Axis(axis) * (c->radius * sign));
        }
        for (i8 sign : {1, -1}) add(ColliderHandleKind::CapsuleHeight, 1, sign, c->center + Vec3(0, 0.5f * c->height * sign, 0));
    }
    return out;
}

bool DragColliderHandle(World& world, Entity e, const ColliderHandle& h, f32 distance) {
    constexpr f32 kMin = 0.01f;
    switch (h.kind) {
    case ColliderHandleKind::BoxFace: {
        BoxCollider* c = world.GetComponent<BoxCollider>(e);
        if (c == nullptr) return false;
        f32* half = h.axis == 0 ? &c->half_extents.x : h.axis == 1 ? &c->half_extents.y : &c->half_extents.z;
        f32* center = h.axis == 0 ? &c->center.x : h.axis == 1 ? &c->center.y : &c->center.z;
        const f32 new_half = std::max(kMin, *half + 0.5f * distance);
        // The opposite face stays put: the center moves by the change in half size.
        *center += (new_half - *half) * static_cast<f32>(h.sign);
        *half = new_half;
        return true;
    }
    case ColliderHandleKind::SphereRadius: {
        SphereCollider* c = world.GetComponent<SphereCollider>(e);
        if (c == nullptr) return false;
        c->radius = std::max(kMin, c->radius + distance);
        return true;
    }
    case ColliderHandleKind::CapsuleRadius: {
        CapsuleCollider* c = world.GetComponent<CapsuleCollider>(e);
        if (c == nullptr) return false;
        c->radius = std::clamp(c->radius + distance, kMin, 0.5f * c->height);
        return true;
    }
    case ColliderHandleKind::CapsuleHeight: {
        CapsuleCollider* c = world.GetComponent<CapsuleCollider>(e);
        if (c == nullptr) return false;
        const f32 new_height = std::max(2.0f * c->radius, c->height + distance);
        c->center.y += 0.5f * (new_height - c->height) * static_cast<f32>(h.sign); // the other end stays put
        c->height = new_height;
        return true;
    }
    }
    return false;
}

} // namespace aether
