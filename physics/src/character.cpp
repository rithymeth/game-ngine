#include "aether/physics/character.h"

#include "aether/scene/gameplay.h"

#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>

#include <algorithm>
#include <cmath>

namespace aether {

namespace {

JPH::Vec3 ToJolt(const Vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
Vec3 ToAether(JPH::Vec3Arg v) { return Vec3(v.GetX(), v.GetY(), v.GetZ()); }

f32 Length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// Moves `from` toward `to` by at most `step`.
Vec3 MoveTowards(const Vec3& from, const Vec3& to, f32 step) {
    const Vec3 d = to - from;
    const f32 len = Length(d);
    if (len <= step || len < 1e-6f) return to;
    return from + d * (step / len);
}

// What a character is built from: a change rebuilds it.
struct Shape {
    f32 radius, height, mass, max_slope;
    u8 layer;
    bool operator==(const Shape& o) const {
        return radius == o.radius && height == o.height && mass == o.mass && max_slope == o.max_slope && layer == o.layer;
    }
};

} // namespace

const char* CharacterEventName(CharacterEventType type) {
    switch (type) {
    case CharacterEventType::Landed: return "Event.OnLanded";
    case CharacterEventType::Jumped: return "Event.OnJumped";
    case CharacterEventType::MovementModeChanged: return "Event.OnMovementModeChanged";
    }
    return "";
}

struct CharacterSystem::Tracked {
    Entity entity;
    JPH::Ref<JPH::CharacterVirtual> character;
    Shape shape{};
    Vec3 position; // the Transform position last written or read
};

CharacterSystem::CharacterSystem(World& world, PhysicsWorld& physics, PhysicsScene* scene)
    : world_(world), physics_(physics), scene_(scene) {}

CharacterSystem::~CharacterSystem() {
    for (auto& [key, t] : characters_) {
        (void)key;
        if (scene_ != nullptr) scene_->ReleaseBody(t->character->GetInnerBodyID());
    }
    characters_.clear(); // the characters remove their inner bodies
}

JPH::BodyID CharacterSystem::InnerBodyOf(Entity entity) const {
    auto it = characters_.find(PhysicsScene::EntityKey(entity));
    return it == characters_.end() ? JPH::BodyID() : it->second->character->GetInnerBodyID();
}

void CharacterSystem::Report(const CharacterEvent& e) {
    events_.push_back(e);
    if (handler_) handler_(e);
}

void CharacterSystem::Step(f32 dt) {
    events_.clear();
    std::vector<Entity> entities;
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<CharacterMovement>()) || !archetype.Mask().test(GetComponentId<Transform>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            entities.insert(entities.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return a.index < b.index; });

    // Characters whose entity (or component) is gone.
    for (auto it = characters_.begin(); it != characters_.end();) {
        const Entity e = it->second->entity;
        if (world_.IsAlive(e) && world_.HasComponent<CharacterMovement>(e) && world_.HasComponent<Transform>(e)) {
            ++it;
            continue;
        }
        if (scene_ != nullptr) scene_->ReleaseBody(it->second->character->GetInnerBodyID());
        it = characters_.erase(it);
    }

    JPH::PhysicsSystem& system = physics_.System();
    const JPH::Vec3 gravity = system.GetGravity();
    for (Entity e : entities) {
        CharacterMovement& cm = *world_.GetComponent<CharacterMovement>(e);
        Transform& transform = *world_.GetComponent<Transform>(e);
        const u8 layer_index = LayerOf(world_, e);
        const Shape shape{std::max(cm.radius, 0.01f), std::max(cm.height, 2.0f * std::max(cm.radius, 0.01f)), std::max(cm.mass, 1.0f),
                          cm.max_slope_degrees, layer_index};
        const JPH::ObjectLayer layer = physics_.ObjectLayerFor(true, layer_index);

        // Create, or rebuild when the shape changed.
        std::unique_ptr<Tracked>& slot = characters_[PhysicsScene::EntityKey(e)];
        if (!slot || !(slot->shape == shape)) {
            if (slot && scene_ != nullptr) scene_->ReleaseBody(slot->character->GetInnerBodyID());
            const f32 half_cylinder = 0.5f * shape.height - shape.radius;
            JPH::RefConst<JPH::Shape> core = half_cylinder > 0.0f ? JPH::RefConst<JPH::Shape>(new JPH::CapsuleShape(half_cylinder, shape.radius))
                                                                   : JPH::RefConst<JPH::Shape>(new JPH::SphereShape(shape.radius));
            // Feet at the origin.
            JPH::RefConst<JPH::Shape> feet =
                JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0, 0.5f * shape.height, 0), JPH::Quat::sIdentity(), core).Create().Get();
            JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
            settings->mShape = feet;
            settings->mInnerBodyShape = feet; // a kinematic body that triggers and queries can see
            settings->mInnerBodyLayer = layer;
            settings->mMass = shape.mass;
            settings->mMaxSlopeAngle = JPH::DegreesToRadians(std::clamp(shape.max_slope, 0.0f, 89.0f));
            settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -shape.radius); // the lower cap supports
            const Vec3 at = transform.position;
            auto tracked = std::make_unique<Tracked>();
            tracked->entity = e;
            tracked->shape = shape;
            tracked->character = new JPH::CharacterVirtual(settings, JPH::RVec3(at.x, at.y, at.z), JPH::Quat::sIdentity(),
                                                           PhysicsScene::EntityKey(e), &system);
            tracked->position = at;
            if (scene_ != nullptr) scene_->AdoptBody(tracked->character->GetInnerBodyID(), e);
            slot = std::move(tracked);
        }
        Tracked& t = *slot;
        JPH::CharacterVirtual& ch = *t.character;

        // Gameplay moved it: teleport.
        if (transform.position.x != t.position.x || transform.position.y != t.position.y || transform.position.z != t.position.z) {
            ch.SetPosition(JPH::RVec3(transform.position.x, transform.position.y, transform.position.z));
        }

        ch.UpdateGroundVelocity();
        const bool was_grounded = !cm.flying && ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        const MovementMode old_mode = cm.mode;

        // Timers: coyote time runs from leaving the ground, the jump buffer from the press.
        cm.coyote_left = was_grounded ? cm.coyote_time : std::max(0.0f, cm.coyote_left - dt);
        if (cm.jump_requested) {
            cm.jump_buffer_left = cm.jump_buffer + dt; // this step counts as the press
            cm.jump_requested = false;
        }
        cm.jump_buffer_left = std::max(0.0f, cm.jump_buffer_left - dt);
        const bool jump_pending = cm.jump_buffer_left > 0.0f;

        // Input: clamped, flat unless flying, consumed.
        Vec3 input = cm.input;
        cm.input = Vec3(0, 0, 0);
        if (!cm.flying) input.y = 0.0f;
        const f32 input_len = Length(input);
        if (input_len > 1.0f) input = input * (1.0f / input_len);
        const Vec3 desired = input * (cm.run ? cm.run_speed : cm.walk_speed);

        Vec3 move = cm.move_velocity;
        if (!cm.flying) move.y = 0.0f;
        if (cm.flying || was_grounded) {
            move = MoveTowards(move, desired, (input_len > 0.0f ? cm.acceleration : cm.braking) * dt);
        } else if (input_len > 0.0f) {
            // In the air only part of the input counts, and changes come slowly.
            const Vec3 target = move + (desired - move) * std::clamp(cm.air_control, 0.0f, 1.0f);
            move = MoveTowards(move, target, cm.air_acceleration * dt);
        }

        bool jumped = false;
        JPH::Vec3 velocity;
        if (cm.flying) {
            velocity = ToJolt(move);
        } else {
            const JPH::Vec3 ground = ch.GetGroundVelocity();
            const JPH::Vec3 current = ch.GetLinearVelocity();
            // On the ground and not moving away from it: ride along with it.
            velocity = was_grounded && current.GetY() - ground.GetY() < 0.1f ? ground : JPH::Vec3(0, current.GetY(), 0);
            if (jump_pending && (was_grounded || cm.coyote_left > 0.0f)) {
                velocity.SetY((was_grounded ? ground.GetY() : 0.0f) + cm.jump_velocity);
                jumped = true;
                cm.jump_buffer_left = 0.0f;
                cm.coyote_left = 0.0f;
            }
            // Standing on walkable ground, gravity would only push into the
            // slope and creep the character down it; stick-to-floor keeps it
            // on the ground instead.
            if (!was_grounded || jumped) velocity += gravity * cm.gravity_scale * dt;
            velocity += JPH::Vec3(move.x, 0.0f, move.z);
        }
        const f32 falling_speed = -velocity.GetY();
        ch.SetLinearVelocity(velocity);

        JPH::CharacterVirtual::ExtendedUpdateSettings update;
        update.mWalkStairsStepUp = JPH::Vec3(0, cm.flying ? 0.0f : std::max(cm.step_height, 0.0f), 0);
        // Stick to the floor (down slopes and steps) only while walking.
        update.mStickToFloorStepDown = JPH::Vec3(0, was_grounded && !jumped ? -std::max(cm.step_height, 0.05f) : 0.0f, 0);
        ch.ExtendedUpdate(dt, cm.flying ? JPH::Vec3::sZero() : gravity * cm.gravity_scale, update,
                          system.GetDefaultBroadPhaseLayerFilter(layer), system.GetDefaultLayerFilter(layer), JPH::BodyFilter(),
                          JPH::ShapeFilter(), physics_.TempAllocator());

        const JPH::RVec3 p = ch.GetPosition();
        transform.position = Vec3(static_cast<f32>(p.GetX()), static_cast<f32>(p.GetY()), static_cast<f32>(p.GetZ()));
        t.position = transform.position;
        cm.velocity = ToAether(ch.GetLinearVelocity());
        cm.move_velocity = move;
        const bool grounded = !cm.flying && ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        cm.grounded = grounded;
        cm.ground_normal = ToAether(ch.GetGroundNormal());
        cm.mode = cm.flying ? MovementMode::Flying : grounded ? MovementMode::Walking : MovementMode::Falling;

        if (jumped) Report({e, CharacterEventType::Jumped, old_mode, cm.mode, 0.0f});
        if (grounded && !was_grounded && !cm.flying) Report({e, CharacterEventType::Landed, old_mode, cm.mode, std::max(0.0f, falling_speed)});
        if (cm.mode != old_mode) Report({e, CharacterEventType::MovementModeChanged, old_mode, cm.mode, 0.0f});
    }
}

} // namespace aether
