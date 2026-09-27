#include "aether/job/job_system.h"
#include "aether/physics/debug_draw.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>

using namespace aether;

// Phase 13 step 6: physics debug lines and collider gizmo handles.

namespace {


struct Sim {
    JobSystem jobs{2};
    World world;
    PhysicsWorld physics{jobs};
    PhysicsScene scene{world, physics};
};

f32 Dist(const Vec3& a, const Vec3& b) {
    const Vec3 d = a - b;
    return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

template <typename Pred>
bool AllEnds(const std::vector<DebugLine>& lines, Pred pred) {
    return std::all_of(lines.begin(), lines.end(), [&](const DebugLine& l) { return pred(l.a) && pred(l.b); });
}

usize CountColor(const std::vector<DebugLine>& lines, u32 color) {
    return static_cast<usize>(std::count_if(lines.begin(), lines.end(), [&](const DebugLine& l) { return l.color == color; }));
}

} // namespace

AETHER_TEST(PhysicsDebug_WireframesMatchTheColliders) {
    World world;
    // A box turned 90° about Y: its x and z extents swap in world space.
    BoxCollider box;
    box.half_extents = Vec3(1, 0.5f, 2);
    Transform turned_y;
    turned_y.position = Vec3(5, 1, 0);
    turned_y.rotation = Quaternion::FromAxisAngle(Vec3(0, 1, 0), kPi / 2);
    const Entity b = world.CreateEntity(turned_y, box);
    std::vector<DebugLine> lines;
    DrawColliderWireframe(world, nullptr, b, 0xFFFFFFFF, lines);
    AETHER_CHECK(lines.size() == 12);
    AETHER_CHECK(AllEnds(lines, [](const Vec3& p) {
        return std::fabs(std::fabs(p.x - 5) - 2) < 1e-4f && std::fabs(std::fabs(p.y - 1) - 0.5f) < 1e-4f && std::fabs(std::fabs(p.z) - 1) < 1e-4f;
    }));

    // A sphere with an offset: three circles, every point on the surface.
    lines.clear();
    SphereCollider sphere;
    sphere.radius = 0.75f;
    sphere.center = Vec3(0, 1, 0);
    const Entity s = world.CreateEntity(Transform{Vec3(0, 2, 0), Quaternion::Identity()}, sphere);
    DrawColliderWireframe(world, nullptr, s, 0xFFFFFFFF, lines);
    AETHER_CHECK(lines.size() == 72);
    AETHER_CHECK(AllEnds(lines, [](const Vec3& p) { return std::fabs(Dist(p, Vec3(0, 3, 0)) - 0.75f) < 1e-4f; }));

    // A capsule spans its whole height around its center.
    lines.clear();
    CapsuleCollider capsule; // 0.35 x 1.8
    const Entity c = world.CreateEntity(Transform{Vec3(0, 1, 0), Quaternion::Identity()}, capsule);
    DrawColliderWireframe(world, nullptr, c, 0xFFFFFFFF, lines);
    f32 lo = 1e9f, hi = -1e9f, wide = 0.0f;
    for (const DebugLine& l : lines) {
        for (const Vec3& p : {l.a, l.b}) {
            lo = std::min(lo, p.y);
            hi = std::max(hi, p.y);
            wide = std::max(wide, std::sqrt(p.x * p.x + p.z * p.z));
        }
    }
    AETHER_CHECK(std::fabs(lo - 0.1f) < 1e-3f && std::fabs(hi - 1.9f) < 1e-3f && std::fabs(wide - 0.35f) < 1e-3f);

    // A convex collider without a physics body: a cross per point.
    lines.clear();
    ConvexCollider hull;
    hull.points = {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
    const Entity h = world.CreateEntity(Transform{}, hull);
    DrawColliderWireframe(world, nullptr, h, 0xFFFFFFFF, lines);
    AETHER_CHECK(lines.size() == 12);
}

AETHER_TEST(PhysicsDebug_ColorsTogglesCharactersAndContacts) {
    Sim s;
    BoxCollider floor_box;
    floor_box.half_extents = Vec3(10, 0.5f, 10);
    s.world.CreateEntity(Transform{}, floor_box); // static
    SphereCollider ball;
    const Entity dynamic = s.world.CreateEntity(Transform{Vec3(0, 3, 0), Quaternion::Identity()}, RigidBody{}, ball);
    RigidBody kinematic;
    kinematic.motion = BodyMotion::Kinematic;
    s.world.CreateEntity(Transform{Vec3(4, 3, 0), Quaternion::Identity()}, kinematic, ball);
    SphereCollider zone = ball;
    zone.is_trigger = true;
    s.world.CreateEntity(Transform{Vec3(-4, 3, 0), Quaternion::Identity()}, zone);
    // A convex rock with a body: drawn from its hull's triangles.
    ConvexCollider rock;
    for (float x : {-0.5f, 0.5f})
        for (float y : {-0.5f, 0.5f})
            for (float z : {-0.5f, 0.5f}) rock.points.push_back(Vec3(x, y, z));
    const Entity r = s.world.CreateEntity(Transform{Vec3(0, 1, 6), Quaternion::Identity()}, rock);
    s.world.CreateEntity(Transform{Vec3(8, 0.5f, 8), Quaternion::Identity()}, CharacterMovement{});
    s.scene.Sync();

    PhysicsDebugOptions options;
    std::vector<DebugLine> lines;
    DrawPhysicsDebug(s.world, &s.scene, options, lines);
    AETHER_CHECK(lines.empty()); // off by default

    options.enabled = true;
    DrawPhysicsDebug(s.world, &s.scene, options, lines);
    AETHER_CHECK(CountColor(lines, debug_colors::kStatic) == 12 + 36); // the floor, and the rock's 12 triangles
    AETHER_CHECK(CountColor(lines, debug_colors::kDynamic) == 72 && CountColor(lines, debug_colors::kKinematic) == 72);
    AETHER_CHECK(CountColor(lines, debug_colors::kTrigger) == 72 && CountColor(lines, debug_colors::kCharacter) > 0);
    // The rock's triangle edges sit on its cube.
    std::vector<DebugLine> rock_lines;
    DrawColliderWireframe(s.world, &s.scene, r, 1, rock_lines);
    AETHER_CHECK(rock_lines.size() == 36 && AllEnds(rock_lines, [](const Vec3& p) {
        return std::fabs(std::fabs(p.x) - 0.5f) < 1e-3f && std::fabs(std::fabs(p.y - 1) - 0.5f) < 1e-3f &&
               std::fabs(std::fabs(p.z - 6) - 0.5f) < 1e-3f;
    }));
    // The character is drawn from its feet up.
    f32 lowest = 1e9f;
    for (const DebugLine& l : lines) {
        if (l.color == debug_colors::kCharacter) lowest = std::min({lowest, l.a.y, l.b.y});
    }
    AETHER_CHECK(std::fabs(lowest - 0.5f) < 1e-3f);

    // Toggles.
    lines.clear();
    options.colliders = false;
    options.characters = false;
    DrawPhysicsDebug(s.world, &s.scene, options, lines);
    AETHER_CHECK(lines.size() == 72 && CountColor(lines, debug_colors::kTrigger) == 72);
    lines.clear();
    options.triggers = false;
    DrawPhysicsDebug(s.world, &s.scene, options, lines);
    AETHER_CHECK(lines.empty());

    // Contacts: the ball's landing, with its normal.
    options = PhysicsDebugOptions{};
    options.enabled = true;
    options.colliders = options.triggers = options.characters = false;
    options.contacts = true;
    bool saw_contact = false;
    for (int i = 0; i < 60 && !saw_contact; ++i) {
        s.scene.Step(1.0f / 60.0f);
        lines.clear();
        DrawPhysicsDebug(s.world, &s.scene, options, lines);
        saw_contact = CountColor(lines, debug_colors::kContact) == 3 * s.physics.Contacts().size() && !lines.empty();
    }
    AETHER_CHECK(saw_contact);

    // Resting long enough to sleep: drawn dimmer.
    for (int i = 0; i < 300; ++i) s.scene.Step(1.0f / 60.0f);
    options = PhysicsDebugOptions{};
    options.enabled = true;
    lines.clear();
    DrawPhysicsDebug(s.world, &s.scene, options, lines);
    AETHER_CHECK(!s.physics.BodyInterface().IsActive(s.scene.BodyOf(dynamic)));
    AETHER_CHECK(CountColor(lines, debug_colors::kSleeping) == 72 && CountColor(lines, debug_colors::kDynamic) == 0);
}

AETHER_TEST(PhysicsDebug_GizmoHandlesResizeColliders) {
    World world;
    BoxCollider box;
    box.half_extents = Vec3(1, 0.5f, 2);
    const Entity b = world.CreateEntity(Transform{Vec3(10, 0, 0), Quaternion::Identity()}, box);
    std::vector<ColliderHandle> handles = ColliderHandles(world, b);
    AETHER_CHECK(handles.size() == 6);
    auto find = [&](ColliderHandleKind kind, u8 axis, i8 sign) {
        return *std::find_if(handles.begin(), handles.end(), [&](const ColliderHandle& h) {
            return h.kind == kind && h.axis == axis && h.sign == sign;
        });
    };
    const ColliderHandle px = find(ColliderHandleKind::BoxFace, 0, 1);
    AETHER_CHECK(std::fabs(px.position.x - 11) < 1e-5f && std::fabs(px.direction.x - 1) < 1e-5f);

    // Dragging the +x face out by 1: the box grows to 3 wide, the -x face stays at x = 9.
    AETHER_CHECK(DragColliderHandle(world, b, px, 1.0f));
    const BoxCollider& grown = *world.GetComponent<BoxCollider>(b);
    AETHER_CHECK(grown.half_extents.x == 1.5f && grown.center.x == 0.5f);
    handles = ColliderHandles(world, b);
    AETHER_CHECK(std::fabs(find(ColliderHandleKind::BoxFace, 0, -1).position.x - 9) < 1e-5f);
    AETHER_CHECK(std::fabs(find(ColliderHandleKind::BoxFace, 0, 1).position.x - 12) < 1e-5f);
    // Shrinking past zero clamps to 1 cm.
    DragColliderHandle(world, b, find(ColliderHandleKind::BoxFace, 1, -1), -5.0f);
    AETHER_CHECK(world.GetComponent<BoxCollider>(b)->half_extents.y == 0.01f);

    // On a rotated entity the handles turn with it.
    Transform turned_z;
    turned_z.rotation = Quaternion::FromAxisAngle(Vec3(0, 0, 1), kPi / 2);
    const Entity turned = world.CreateEntity(turned_z, box);
    handles = ColliderHandles(world, turned);
    const ColliderHandle tx = find(ColliderHandleKind::BoxFace, 0, 1);
    AETHER_CHECK(std::fabs(tx.position.y - 1) < 1e-5f && std::fabs(tx.direction.y - 1) < 1e-5f); // local +x is world +y

    // Spheres: six radius handles.
    SphereCollider sphere;
    const Entity s = world.CreateEntity(Transform{}, sphere);
    handles = ColliderHandles(world, s);
    AETHER_CHECK(handles.size() == 6);
    DragColliderHandle(world, s, handles[0], 0.25f);
    AETHER_CHECK(world.GetComponent<SphereCollider>(s)->radius == 0.75f);

    // Capsules: four radius handles and two height handles; the top one keeps the bottom in place.
    CapsuleCollider capsule; // 0.35 x 1.8
    const Entity c = world.CreateEntity(Transform{}, capsule);
    handles = ColliderHandles(world, c);
    AETHER_CHECK(handles.size() == 6);
    DragColliderHandle(world, c, find(ColliderHandleKind::CapsuleHeight, 1, 1), 0.4f);
    const CapsuleCollider& tall = *world.GetComponent<CapsuleCollider>(c);
    AETHER_CHECK(std::fabs(tall.height - 2.2f) < 1e-5f && std::fabs(tall.center.y - 0.2f) < 1e-5f);
    AETHER_CHECK(std::fabs((tall.center.y - 0.5f * tall.height) - (-0.9f)) < 1e-5f); // bottom unchanged
    DragColliderHandle(world, c, find(ColliderHandleKind::CapsuleRadius, 0, 1), 5.0f);
    AETHER_CHECK(world.GetComponent<CapsuleCollider>(c)->radius == 1.1f); // at most half the height
    AETHER_CHECK(!DragColliderHandle(world, s, find(ColliderHandleKind::CapsuleHeight, 1, 1), 1.0f)); // not a capsule
}
