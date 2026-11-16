#include "aether/water/water_system.h"

#include "aether/physics/components.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace aether::water {

namespace {

WaterSystem* g_active = nullptr;

constexpr f32 kPi = 3.14159265358979f;

u64 Key(Entity e) { return (static_cast<u64>(e.generation) << 32) | e.index; }
f32 Clamp(f32 v, f32 lo, f32 hi) { return std::clamp(v, lo, hi); }
bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
f32 Rad(f32 degrees) { return degrees * (kPi / 180.0f); }

// A vector turned by a quaternion.
Vec3 Rotate(const Quaternion& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    const Vec3 t = u.Cross(v) * 2.0f;
    return v + t * q.w + u.Cross(t);
}

bool UsableSurface(const WaterSurface& w) {
    return w.enabled && std::isfinite(w.density) && w.density > 0.0f && std::isfinite(w.wave_height) && std::isfinite(w.wave_length) && w.wave_length > 0.0f &&
           std::isfinite(w.wave_speed) && std::isfinite(w.wave_direction) && std::isfinite(w.current_speed) && std::isfinite(w.current_direction) &&
           Finite(w.half_extents);
}

// A water surface as the step sees it: a copy of the component and where it is.
struct Surface {
    Entity entity;
    WaterSurface water;
    Vec3 position{0, 0, 0};
};

} // namespace

f32 WaveHeight(const WaterSurface& w, f32 x, f32 z, f32 time) {
    if (!(w.wave_height > 0.0f) || !(w.wave_length > 0.0f)) return 0.0f;
    const f32 k = 2.0f * kPi / w.wave_length;
    const f32 a = Rad(w.wave_direction);
    const f32 along = x * std::cos(a) + z * std::sin(a);
    f32 h = w.wave_height * std::sin(k * (along - w.wave_speed * time));
    // A second, shorter and lower wave, travelling a little across the first: the surface is not a row of identical ridges.
    const f32 k2 = k * 2.3f;
    const f32 a2 = a + Rad(37.0f);
    const f32 along2 = x * std::cos(a2) + z * std::sin(a2);
    h += 0.4f * w.wave_height * std::sin(k2 * (along2 - w.wave_speed * 0.7f * time) + 1.3f);
    return h;
}

struct WaterSystem::Impl {
    struct Input {
        f32 throttle = 0.0f, steer = 0.0f;
    };
    f32 time = 0.0f;
    std::unordered_map<u64, FloaterState> states;
    std::unordered_map<u64, Input> inputs;
};

namespace {

std::vector<Entity> EntitiesWith(World& world, ComponentId id) {
    std::vector<Entity> out;
    world.ForEachArchetype([&](const Archetype& a) {
        if (!a.Mask().test(id)) return;
        for (usize c = 0; c < a.ChunkCount(); ++c) {
            const Entity* chunk = a.EntityArray(c);
            out.insert(out.end(), chunk, chunk + a.ChunkEntityCount(c));
        }
    });
    std::sort(out.begin(), out.end(), [](Entity x, Entity y) { return x.index != y.index ? x.index < y.index : x.generation < y.generation; });
    return out;
}

std::vector<Surface> Surfaces(World& world) {
    std::vector<Surface> out;
    for (const Entity e : EntitiesWith(world, GetComponentId<WaterSurface>())) {
        const WaterSurface* w = world.GetComponent<WaterSurface>(e);
        const Transform* at = world.GetComponent<Transform>(e);
        if (w == nullptr || at == nullptr || !UsableSurface(*w) || !Finite(at->position)) continue;
        out.push_back({e, *w, at->position});
    }
    return out;
}

// The surface over (x, z): a bounded one that covers it, else the first infinite one.
bool Sample(const std::vector<Surface>& surfaces, f32 x, f32 z, f32 time, SurfaceSample& out) {
    const Surface* chosen = nullptr;
    for (const Surface& s : surfaces) {
        if (s.water.infinite) continue;
        if (std::fabs(x - s.position.x) <= s.water.half_extents.x && std::fabs(z - s.position.z) <= s.water.half_extents.z) {
            chosen = &s;
            break;
        }
    }
    if (chosen == nullptr) {
        for (const Surface& s : surfaces) {
            if (s.water.infinite) {
                chosen = &s;
                break;
            }
        }
    }
    if (chosen == nullptr) return false;
    const WaterSurface& w = chosen->water;
    out.height = chosen->position.y + WaveHeight(w, x, z, time);
    const f32 c = Rad(w.current_direction);
    out.current = Vec3(std::cos(c), 0.0f, std::sin(c)) * w.current_speed;
    out.density = w.density;
    return true;
}

} // namespace

WaterSystem::WaterSystem(World& world, PhysicsScene& scene) : impl_(std::make_unique<Impl>()), world_(world), scene_(scene) { MakeActive(); }

WaterSystem::~WaterSystem() {
    if (g_active == this) g_active = nullptr;
}

WaterSystem* WaterSystem::Active() { return g_active; }
void WaterSystem::MakeActive() { g_active = this; }

f32 WaterSystem::Time() const { return impl_->time; }
void WaterSystem::SetTime(f32 time) {
    if (std::isfinite(time)) impl_->time = time;
}

bool WaterSystem::SurfaceAt(f32 x, f32 z, SurfaceSample& out) const {
    if (!std::isfinite(x) || !std::isfinite(z)) return false;
    return Sample(Surfaces(world_), x, z, impl_->time, out);
}

f32 WaterSystem::HeightAt(f32 x, f32 z) const {
    SurfaceSample s;
    return SurfaceAt(x, z, s) ? s.height : std::numeric_limits<f32>::lowest();
}

FloaterState WaterSystem::StateOf(Entity floater) const {
    const auto it = impl_->states.find(Key(floater));
    return it != impl_->states.end() ? it->second : FloaterState{};
}

bool WaterSystem::Drive(Entity boat, f32 throttle, f32 steer) {
    if (!std::isfinite(throttle) || !std::isfinite(steer) || !world_.IsAlive(boat)) return false;
    const Boat* b = world_.GetComponent<Boat>(boat);
    if (b == nullptr || !b->enabled) return false;
    Impl::Input& input = impl_->inputs[Key(boat)];
    input.throttle = Clamp(throttle, -1.0f, 1.0f);
    input.steer = Clamp(steer, -1.0f, 1.0f);
    return true;
}

usize WaterSystem::Apply(f32 dt) {
    if (std::isfinite(dt) && dt > 0.0f) impl_->time += dt;
    const std::vector<Surface> surfaces = Surfaces(world_);
    impl_->states.clear();
    // Inputs of boats that are gone go with them.
    for (auto it = impl_->inputs.begin(); it != impl_->inputs.end();) {
        const Entity e{static_cast<u32>(it->first & 0xFFFFFFFFu), static_cast<u32>(it->first >> 32)};
        it = world_.IsAlive(e) && world_.HasComponent<Boat>(e) ? std::next(it) : impl_->inputs.erase(it);
    }
    if (surfaces.empty()) return 0;

    usize wet = 0;
    JPH::BodyInterface& bodies = scene_.Physics().BodyInterface();
    const f32 gravity = scene_.Physics().GetGravity().Length();
    for (const Entity e : EntitiesWith(world_, GetComponentId<Floater>())) {
        if (!world_.IsAlive(e)) continue;
        const Floater* floater = world_.GetComponent<Floater>(e);
        if (floater == nullptr || !floater->enabled) continue;
        const Floater f = *floater;
        if (!Finite(f.half_extents) || !Finite(f.offset) || f.half_extents.x <= 0.0f || f.half_extents.y <= 0.0f || f.half_extents.z <= 0.0f) continue;
        const JPH::BodyID id = scene_.BodyOf(e);
        if (id.IsInvalid() || bodies.GetMotionType(id) != JPH::EMotionType::Dynamic) continue;

        const RigidBody* rb = world_.GetComponent<RigidBody>(e);
        const f32 mass = rb != nullptr && rb->mass > 0.0f ? rb->mass : 1.0f;
        const JPH::RVec3 origin_j = bodies.GetPosition(id);
        const JPH::RVec3 com_j = bodies.GetCenterOfMassPosition(id);
        const JPH::Quat rot_j = bodies.GetRotation(id);
        const JPH::Vec3 lv_j = bodies.GetLinearVelocity(id);
        const JPH::Vec3 av_j = bodies.GetAngularVelocity(id);
        const Vec3 origin(static_cast<f32>(origin_j.GetX()), static_cast<f32>(origin_j.GetY()), static_cast<f32>(origin_j.GetZ()));
        const Vec3 com(static_cast<f32>(com_j.GetX()), static_cast<f32>(com_j.GetY()), static_cast<f32>(com_j.GetZ()));
        const Quaternion q(rot_j.GetX(), rot_j.GetY(), rot_j.GetZ(), rot_j.GetW());
        const Vec3 lv(lv_j.GetX(), lv_j.GetY(), lv_j.GetZ());
        const Vec3 av(av_j.GetX(), av_j.GetY(), av_j.GetZ());
        if (!Finite(origin) || !Finite(lv) || !Finite(av)) continue;

        const u32 nx = std::clamp<u32>(f.columns_x, Floater::kMinColumns, Floater::kMaxColumns);
        const u32 nz = std::clamp<u32>(f.columns_z, Floater::kMinColumns, Floater::kMaxColumns);
        const f32 cell_x = 2.0f * f.half_extents.x / static_cast<f32>(nx), cell_z = 2.0f * f.half_extents.z / static_cast<f32>(nz);
        const f32 column_volume = cell_x * cell_z * 2.0f * f.half_extents.y;
        const Vec3 up = Rotate(q, Vec3(0, 1, 0));
        const f32 columns = static_cast<f32>(nx * nz);

        Vec3 force_total(0, 0, 0), torque_total(0, 0, 0);
        f32 submerged = 0.0f;
        const auto push = [&](const Vec3& point, const Vec3& force) {
            force_total = force_total + force;
            torque_total = torque_total + (point - com).Cross(force);
        };
        SurfaceSample middle;
        const Vec3 hull_middle = origin + Rotate(q, f.offset);
        const bool has_middle = Sample(surfaces, hull_middle.x, hull_middle.z, impl_->time, middle);

        for (u32 i = 0; i < nx; ++i) {
            for (u32 j = 0; j < nz; ++j) {
                const Vec3 local = f.offset + Vec3(-f.half_extents.x + (static_cast<f32>(i) + 0.5f) * cell_x, 0.0f, -f.half_extents.z + (static_cast<f32>(j) + 0.5f) * cell_z);
                const Vec3 centre = origin + Rotate(q, local);
                SurfaceSample water;
                if (!Sample(surfaces, centre.x, centre.z, impl_->time, water)) continue;
                const f32 a = centre.y - up.y * f.half_extents.y, b = centre.y + up.y * f.half_extents.y;
                const f32 low = std::min(a, b), high = std::max(a, b);
                f32 fraction;
                if (high - low > 1e-4f) fraction = Clamp((water.height - low) / (high - low), 0.0f, 1.0f);
                else fraction = water.height > centre.y ? 1.0f : 0.0f;
                if (fraction <= 0.0f) continue;
                submerged += fraction;
                // The lift is the weight of the water the column pushes aside, at the middle of the part that is under.
                const Vec3 point(centre.x, low + 0.5f * fraction * (high - low), centre.z);
                const f32 lift = water.density * gravity * column_volume * fraction * f.buoyancy_scale;
                push(point, Vec3(0.0f, lift, 0.0f));
                // The water's drag on this column's share of the body, against its motion through the water.
                const Vec3 point_velocity = lv + av.Cross(point - com);
                const Vec3 relative = point_velocity - water.current;
                // Drag follows the wetted surface, which a little depth already has most of: it is not in proportion to the volume under.
                // Bobbing is damped hard (it sets waves going); gliding through the water is a quarter as hard.
                const f32 wet = f.linear_drag * mass / columns * std::min(1.0f, fraction * 4.0f);
                push(point, Vec3(-relative.x * wet * 0.25f, -relative.y * wet, -relative.z * wet * 0.25f));
            }
        }
        FloaterState state;
        state.submersion = submerged / columns;
        state.in_water = submerged > 0.0f;
        state.surface_height = has_middle ? middle.height : 0.0f;
        impl_->states[Key(e)] = state;
        if (!state.in_water) continue;
        ++wet;

        // Spinning in the water costs it a share of its angular momentum, by a rough measure of the hull's inertia.
        const f32 inertia = mass * (f.half_extents.x * f.half_extents.x + f.half_extents.y * f.half_extents.y + f.half_extents.z * f.half_extents.z) / 3.0f;
        torque_total = torque_total - av * (f.angular_drag * inertia * state.submersion);

        // A boat: the propeller pushes along the bow axis while it is under, the rudder turns it by how fast it goes, and the keel resists sliding sideways.
        const Boat* boat = world_.GetComponent<Boat>(e);
        const auto input = impl_->inputs.find(Key(e));
        if (boat != nullptr && boat->enabled && Finite(boat->propeller_offset) && std::isfinite(boat->max_speed) && boat->max_speed > 0.0f) {
            const Boat b = *boat;
            const Vec3 forward = Rotate(q, Vec3(0, 0, 1));
            const Vec3 right = Rotate(q, Vec3(1, 0, 0));
            const f32 speed = lv.Dot(forward);
            const f32 throttle = input != impl_->inputs.end() ? input->second.throttle : 0.0f;
            const f32 steer = input != impl_->inputs.end() ? input->second.steer : 0.0f;
            const Vec3 propeller = origin + Rotate(q, b.propeller_offset);
            SurfaceSample at_propeller;
            f32 immersion = 0.0f;
            if (Sample(surfaces, propeller.x, propeller.z, impl_->time, at_propeller)) immersion = Clamp((at_propeller.height - propeller.y) / 0.4f, 0.0f, 1.0f);
            if (throttle != 0.0f && immersion > 0.0f) {
                const f32 room = throttle > 0.0f ? 1.0f - Clamp(speed / b.max_speed, 0.0f, 1.0f) : 1.0f - Clamp(-speed / b.max_speed, 0.0f, 1.0f);
                const f32 ratio = throttle > 0.0f ? 1.0f : b.reverse_ratio;
                push(propeller, forward * (b.thrust * throttle * ratio * room * immersion));
            }
            const f32 authority = Clamp(speed / std::max(b.turn_speed, 0.1f), -1.0f, 1.0f);
            // Steer right is a turn clockwise seen from above: negative about +Y.
            torque_total = torque_total + Vec3(0.0f, -steer * b.turn_torque * authority * state.submersion, 0.0f);
            const f32 slide = lv.Dot(right);
            push(com, right * (-slide * mass * b.keel_drag * state.submersion));
        }

        if (!Finite(force_total) || !Finite(torque_total)) continue;
        bodies.AddForceAndTorque(id, JPH::Vec3(force_total.x, force_total.y, force_total.z), JPH::Vec3(torque_total.x, torque_total.y, torque_total.z));
    }
    return wet;
}

} // namespace aether::water
