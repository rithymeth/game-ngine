#include "aether/vfx/simulation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aether::vfx {

namespace {

constexpr f32 kWarmupStep = 1.0f / 30.0f;
// Rounding in frame times mustn't lose a particle that's due (29.99999 is 30);
// the carry goes slightly negative instead.
constexpr f32 kCountSlack = 1e-3f;

Vec3 Rotate(const Quaternion& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    const Vec3 t = u.Cross(v) * 2.0f;
    return v + t * q.w + u.Cross(t);
}

Vec3 Conj(const Quaternion& q, const Vec3& v) { return Rotate(Quaternion(-q.x, -q.y, -q.z, q.w), v); }

Vec3 SafeNormalize(const Vec3& v, const Vec3& fallback) {
    const f32 len = v.Length();
    return len > 1e-8f ? v * (1.0f / len) : fallback;
}

// `v` (sampled around +Y) turned so +Y points along `dir`.
Vec3 AlignY(const Vec3& v, const Vec3& dir) {
    const Vec3 up = SafeNormalize(dir, {0, 1, 0});
    const Vec3 helper = std::abs(up.y) < 0.99f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    const Vec3 tangent = SafeNormalize(helper.Cross(up), {1, 0, 0});
    const Vec3 bitangent = up.Cross(tangent);
    return tangent * v.x + up * v.y + bitangent * v.z;
}

template <typename T>
bool On(const T& module) {
    return module.enabled;
}

} // namespace

// --- ParticleBuffer ------------------------------------------------------------------------------------

void ParticleBuffer::Reserve(usize n) {
    position.resize(n), velocity.resize(n), age.resize(n), lifetime.resize(n), size.resize(n), base_size.resize(n);
    rotation.resize(n), spin.resize(n), color.resize(n), base_color.resize(n), seed.resize(n), id.resize(n);
    count = std::min(count, n);
}

void ParticleBuffer::Kill(usize i) {
    const usize last = count - 1;
    if (i != last) {
        position[i] = position[last], velocity[i] = velocity[last], age[i] = age[last], lifetime[i] = lifetime[last];
        size[i] = size[last], base_size[i] = base_size[last], rotation[i] = rotation[last], spin[i] = spin[last];
        color[i] = color[last], base_color[i] = base_color[last], seed[i] = seed[last], id[i] = id[last];
    }
    --count;
}

// --- EmitterPose ---------------------------------------------------------------------------------------

Vec3 EmitterPose::Right() const { return Rotate(rotation, {1, 0, 0}); }
Vec3 EmitterPose::Up() const { return Rotate(rotation, {0, 1, 0}); }
Vec3 EmitterPose::Forward() const { return Rotate(rotation, {0, 0, 1}); }
Vec3 EmitterPose::ToWorld(const Vec3& local) const { return position + Rotate(rotation, local); }
Vec3 EmitterPose::ToLocal(const Vec3& world) const { return Conj(rotation, world - position); }
Vec3 EmitterPose::DirectionToWorld(const Vec3& local) const { return Rotate(rotation, local); }
Vec3 EmitterPose::DirectionToLocal(const Vec3& world) const { return Conj(rotation, world); }

// --- EmitterInstance -----------------------------------------------------------------------------------

EmitterInstance::EmitterInstance(const Emitter& emitter, u64 seed) : emitter_(emitter), seed_(seed), rng_(seed) {
    particles_.Reserve(std::max<u32>(emitter_.settings.max_particles, 1));
}

void EmitterInstance::SetPose(const EmitterPose& pose) {
    if (!placed_) {
        Teleport(pose);
        return;
    }
    pose_ = pose;
}

void EmitterInstance::Teleport(const EmitterPose& pose) {
    pose_ = last_pose_ = pose;
    placed_ = true;
}

bool EmitterInstance::Spawning() const {
    const EmitterSettings& s = emitter_.settings;
    return s.enabled && !stopped_ && (s.looping || time_ - s.start_delay < s.duration);
}

u32 EmitterInstance::Loop() const {
    const EmitterSettings& s = emitter_.settings;
    if (s.duration <= 0.0f) return 0;
    return static_cast<u32>(std::max(0.0f, time_ - s.start_delay) / s.duration);
}

void EmitterInstance::Clear() { particles_.count = 0; }

void EmitterInstance::Restart() {
    Clear();
    rng_.Seed(seed_);
    time_ = 0.0f;
    carries_.assign(emitter_.spawn.size(), 0.0f);
    next_id_ = 0;
    stopped_ = false;
    warmed_ = false;
    last_pose_ = pose_;
}

usize EmitterInstance::Emit(usize count) {
    const usize before = particles_.count;
    Spawn(std::vector<Birth>(count, Birth{0.0f, 1.0f}));
    return particles_.count - before;
}

Vec3 EmitterInstance::PointIn(const Vec3& p, bool world) const {
    const bool sim_world = emitter_.settings.space == SimSpace::World;
    if (world == sim_world) return p;
    return world ? pose_.ToLocal(p) : pose_.ToWorld(p);
}

Vec3 EmitterInstance::DirectionIn(const Vec3& d, bool world) const {
    const bool sim_world = emitter_.settings.space == SimSpace::World;
    if (world == sim_world) return d;
    return world ? pose_.DirectionToLocal(d) : pose_.DirectionToWorld(d);
}

void EmitterInstance::Update(f32 dt) {
    if (auto_clear_events_) events_.clear();
    if (!emitter_.settings.enabled || dt < 0.0f) return;
    if (!warmed_) {
        warmed_ = true;
        // Run the warmup at once, with the emitter still (and its events forgotten).
        const EmitterPose moving_from = last_pose_;
        last_pose_ = pose_;
        const usize kept = events_.size();
        for (f32 left = emitter_.settings.warmup; left > 1e-6f; left -= kWarmupStep) Advance(std::min(kWarmupStep, left));
        events_.resize(kept);
        last_pose_ = moving_from;
    }
    Advance(dt);
}

void EmitterInstance::Record(ParticleEventKind kind, usize i, const Vec3& normal, bool expired) {
    if ((event_mask_ & EventBit(kind)) == 0) return;
    const ParticleBuffer& p = particles_;
    const bool local = emitter_.settings.space == SimSpace::Local;
    ParticleEvent e;
    e.kind = kind;
    e.position = local ? pose_.ToWorld(p.position[i]) : p.position[i];
    e.velocity = local ? pose_.DirectionToWorld(p.velocity[i]) : p.velocity[i];
    e.normal = local ? pose_.DirectionToWorld(normal) : normal;
    e.color = p.color[i];
    e.size = p.size[i];
    e.id = p.id[i];
    e.expired = expired;
    events_.push_back(e);
}

usize EmitterInstance::EmitAt(usize count, const Vec3& world_position, const Vec3& add_velocity, const LinearColor* color, const f32* size) {
    // Born at the point as if the emitter stood there, still.
    const EmitterPose pose = pose_, last = last_pose_;
    const Vec3 velocity = emitter_velocity_;
    pose_.position = last_pose_.position = world_position;
    emitter_velocity_ = {};
    const usize first = particles_.count;
    const u8 mask = event_mask_;
    event_mask_ &= static_cast<u8>(~EventBit(ParticleEventKind::Birth)); // recorded below, once the overrides are in
    Spawn(std::vector<Birth>(count, Birth{0.0f, 1.0f}));
    event_mask_ = mask;
    const bool local = emitter_.settings.space == SimSpace::Local;
    const Vec3 add = local ? pose_.DirectionToLocal(add_velocity) : add_velocity;
    pose_ = pose, last_pose_ = last;
    emitter_velocity_ = velocity;
    ParticleBuffer& p = particles_;
    for (usize i = first; i < p.count; ++i) {
        // A local simulation put the shape's offset about the origin: move it to the point.
        if (local) p.position[i] = pose_.ToLocal(world_position) + p.position[i];
        p.velocity[i] = p.velocity[i] + add;
        if (color != nullptr) p.color[i] = p.base_color[i] = *color;
        if (size != nullptr) p.size[i] = p.base_size[i] = *size;
        Record(ParticleEventKind::Birth, i);
    }
    return p.count - first;
}

bool EmitterInstance::SetField(const std::string& field, const ParameterValue& value, std::string* error) {
    return SetModuleField(emitter_, field, value, error);
}

void EmitterInstance::Advance(f32 dt) {
    const EmitterSettings& s = emitter_.settings;
    emitter_velocity_ = dt > 0.0f ? (pose_.position - last_pose_.position) * (1.0f / dt) : Vec3{};
    const f32 t0 = time_ - s.start_delay, t1 = t0 + dt;
    time_ += dt;
    std::vector<Birth> births;
    auto birth_at = [&](f32 local_time) {
        // Born `local_time` (emitter time) into this frame: it has lived the rest of it.
        const f32 frac = dt > 0.0f ? std::clamp((local_time - t0) / dt, 0.0f, 1.0f) : 1.0f;
        births.push_back({std::max(t1 - local_time, 0.0f), frac});
    };
    if (!stopped_ && s.duration > 0.0f) {
        const f32 end = s.looping ? std::numeric_limits<f32>::max() : s.duration;
        const f32 a0 = std::max(t0, 0.0f), a1 = std::min(t1, end);
        if (a1 > a0) {
            carries_.resize(emitter_.spawn.size(), 0.0f);
            for (usize mi = 0; mi < emitter_.spawn.size(); ++mi) {
                const SpawnModule& m = emitter_.spawn[mi];
                f32& carry = carries_[mi]; // each module's part-particle left over
                if (const auto* r = std::get_if<SpawnRate>(&m); r != nullptr && On(*r) && r->rate > 0.0f) {
                    const f32 due = carry + r->rate * (a1 - a0);
                    const u32 n = static_cast<u32>(due + kCountSlack);
                    for (u32 k = 0; k < n; ++k) birth_at(a0 + (static_cast<f32>(k) + 1.0f - carry) / r->rate);
                    carry = due - static_cast<f32>(n);
                } else if (const auto* b = std::get_if<SpawnBurst>(&m); b != nullptr && On(*b)) {
                    const u32 first_loop = s.looping ? static_cast<u32>(a0 / s.duration) : 0;
                    const u32 last_loop = s.looping ? static_cast<u32>(a1 / s.duration) : 0;
                    for (u32 loop = first_loop; loop <= last_loop; ++loop) {
                        const f32 start = static_cast<f32>(loop) * s.duration;
                        const u32 cycles = b->cycles == 0 ? std::numeric_limits<u32>::max() : b->cycles;
                        for (u32 k = 0; k < cycles; ++k) {
                            const f32 at = b->time + static_cast<f32>(k) * std::max(b->interval, 1e-3f);
                            if (at >= s.duration) break;
                            const f32 fire = start + at;
                            if (fire >= a1) break;
                            if (fire < a0) continue;
                            const u32 n = static_cast<u32>(std::lround(std::max(rng_.Range(b->count), 0.0f)));
                            for (u32 c = 0; c < n; ++c) birth_at(fire);
                        }
                    }
                } else if (const auto* d = std::get_if<SpawnPerDistance>(&m); d != nullptr && On(*d) && d->per_unit > 0.0f) {
                    const f32 moved = (pose_.position - last_pose_.position).Length();
                    const f32 want = moved * d->per_unit;
                    const f32 due = carry + want;
                    const u32 n = static_cast<u32>(due + kCountSlack);
                    for (u32 k = 0; k < n; ++k) {
                        // Evenly along the path moved this frame.
                        const f32 frac = want > 0.0f ? (static_cast<f32>(k) + 1.0f - carry) / want : 1.0f;
                        births.push_back({dt * (1.0f - std::clamp(frac, 0.0f, 1.0f)), std::clamp(frac, 0.0f, 1.0f)});
                    }
                    carry = due - static_cast<f32>(n);
                }
            }
        }
    }
    const usize first_new = particles_.count;
    Spawn(births);
    Simulate(dt, first_new);
    last_pose_ = pose_;
}

void EmitterInstance::Spawn(const std::vector<Birth>& births) {
    for (const Birth& b : births) {
        if (particles_.count >= particles_.Capacity() || particles_.count >= emitter_.settings.max_particles) return;
        const usize i = particles_.count++;
        Initialize(i, b);
    }
}

void EmitterInstance::Initialize(usize i, const Birth& b) {
    ParticleBuffer& p = particles_;
    EmitterPose at = pose_;
    at.position = last_pose_.position + (pose_.position - last_pose_.position) * b.frac;
    f32 lifetime = 1.0f, size = 0.1f, angle = 0.0f, spin = 0.0f;
    LinearColor color;
    Vec3 local, velocity, inherited;
    for (const InitModule& m : emitter_.init) {
        std::visit(
            [&](const auto& mod) {
                using T = std::decay_t<decltype(mod)>;
                if (!mod.enabled) return;
                if constexpr (std::is_same_v<T, InitLifetime>) {
                    lifetime = std::max(rng_.Range(mod.seconds), 1e-3f);
                } else if constexpr (std::is_same_v<T, InitShape>) {
                    const f32 t = std::clamp(mod.thickness, 0.0f, 1.0f);
                    switch (mod.shape) {
                    case ShapeKind::Point: local = {}; break;
                    case ShapeKind::Sphere:
                    case ShapeKind::Hemisphere: {
                        Vec3 d = rng_.OnUnitSphere();
                        if (mod.shape == ShapeKind::Hemisphere) d.y = std::abs(d.y);
                        local = d * (mod.radius * (1.0f - t + t * std::cbrt(rng_.Next01())));
                        break;
                    }
                    case ShapeKind::Box: {
                        const Vec3 h = mod.half_extents;
                        local = {rng_.Range(-h.x, h.x), rng_.Range(-h.y, h.y), rng_.Range(-h.z, h.z)};
                        if (rng_.Next01() >= t) {
                            // Onto a face, chosen by area.
                            const f32 ax = h.y * h.z, ay = h.x * h.z, az = h.x * h.y, pick = rng_.Next01() * (ax + ay + az);
                            const f32 side = rng_.Next01() < 0.5f ? -1.0f : 1.0f;
                            if (pick < ax) local.x = side * h.x;
                            else if (pick < ax + ay) local.y = side * h.y;
                            else local.z = side * h.z;
                        }
                        break;
                    }
                    case ShapeKind::Cone:
                    case ShapeKind::Circle: {
                        const f32 r = mod.radius * std::sqrt(1.0f - t + t * rng_.Next01()), a = rng_.Range(0.0f, 6.28318530718f);
                        local = {r * std::cos(a), 0.0f, r * std::sin(a)};
                        break;
                    }
                    case ShapeKind::Edge: local = {rng_.Range(-mod.radius, mod.radius), 0.0f, 0.0f}; break;
                    }
                } else if constexpr (std::is_same_v<T, InitVelocity>) {
                    const f32 speed = rng_.Range(mod.speed);
                    Vec3 dir;
                    switch (mod.mode) {
                    case VelocityMode::Cone: dir = AlignY(rng_.InCone(mod.cone_angle), mod.direction); break;
                    case VelocityMode::Radial: dir = local.Length() > 1e-6f ? local.Normalized() : rng_.OnUnitSphere(); break;
                    case VelocityMode::Direction: dir = SafeNormalize(mod.direction, {0, 1, 0}); break;
                    }
                    velocity = dir * speed;
                } else if constexpr (std::is_same_v<T, InitSize>) {
                    size = std::max(rng_.Range(mod.size), 0.0f);
                } else if constexpr (std::is_same_v<T, InitColor>) {
                    color = mod.color.Evaluate(rng_.Next01());
                } else if constexpr (std::is_same_v<T, InitRotation>) {
                    angle = rng_.Range(mod.angle);
                    spin = rng_.Range(mod.spin);
                } else if constexpr (std::is_same_v<T, InheritVelocity>) {
                    inherited = inherited + emitter_velocity_ * mod.amount;
                }
            },
            m);
    }
    if (emitter_.settings.space == SimSpace::World) {
        p.position[i] = at.ToWorld(local);
        p.velocity[i] = at.DirectionToWorld(velocity) + inherited;
    } else {
        p.position[i] = local;
        p.velocity[i] = velocity + at.DirectionToLocal(inherited);
    }
    p.age[i] = b.age;
    p.lifetime[i] = lifetime;
    p.size[i] = p.base_size[i] = size;
    p.color[i] = p.base_color[i] = color;
    p.rotation[i] = angle;
    p.spin[i] = spin;
    p.seed[i] = rng_.NextU32();
    p.id[i] = next_id_++;
    Record(ParticleEventKind::Birth, i);
}

void EmitterInstance::Simulate(f32 dt, usize first_new) {
    ParticleBuffer& p = particles_;
    const usize n = p.count;
    step_.resize(n);
    for (usize i = 0; i < n; ++i) {
        // Newborns already carry the part of the frame they lived.
        if (i < first_new) p.age[i] += dt;
        step_[i] = i < first_new ? dt : p.age[i];
    }
    std::vector<u8> dead(n, 0);
    std::vector<f32> speed(n, 1.0f);

    // Forces, in the stack's order.
    for (const UpdateModule& m : emitter_.update) {
        std::visit(
            [&](const auto& mod) {
                using T = std::decay_t<decltype(mod)>;
                if (!mod.enabled) return;
                if constexpr (std::is_same_v<T, Gravity>) {
                    const Vec3 g = DirectionIn(mod.acceleration, true);
                    for (usize i = 0; i < n; ++i) p.velocity[i] = p.velocity[i] + g * step_[i];
                } else if constexpr (std::is_same_v<T, Drag>) {
                    for (usize i = 0; i < n; ++i) p.velocity[i] = p.velocity[i] * std::exp(-mod.coefficient * step_[i]);
                } else if constexpr (std::is_same_v<T, CurlNoiseForce>) {
                    const Vec3 drift(0.0f, time_ * mod.scroll * mod.frequency, 0.0f);
                    for (usize i = 0; i < n; ++i) {
                        const Vec3 c = CurlNoise(p.position[i] * mod.frequency + drift);
                        p.velocity[i] = p.velocity[i] + c * (mod.strength * step_[i]);
                    }
                } else if constexpr (std::is_same_v<T, Vortex>) {
                    const Vec3 c = PointIn(mod.center, mod.world), a = SafeNormalize(DirectionIn(mod.axis, mod.world), {0, 1, 0});
                    for (usize i = 0; i < n; ++i) {
                        const Vec3 r = p.position[i] - c;
                        const Vec3 radial = r - a * r.Dot(a);
                        const f32 dist = radial.Length();
                        if (dist < 1e-5f) continue;
                        const Vec3 tangent = a.Cross(radial) * (1.0f / dist);
                        p.velocity[i] = p.velocity[i] + (tangent * mod.strength - radial * (mod.pull / dist)) * step_[i];
                    }
                } else if constexpr (std::is_same_v<T, PointAttractor>) {
                    const Vec3 c = PointIn(mod.position, mod.world);
                    for (usize i = 0; i < n; ++i) {
                        const Vec3 d = c - p.position[i];
                        const f32 dist = d.Length();
                        if (mod.kill_radius > 0.0f && dist < mod.kill_radius) dead[i] = 1;
                        if (dist < 1e-5f) continue;
                        const f32 falloff = mod.radius > 0.0f ? std::max(0.0f, 1.0f - dist / mod.radius) : 1.0f;
                        p.velocity[i] = p.velocity[i] + d * (mod.strength * falloff * step_[i] / dist);
                    }
                } else if constexpr (std::is_same_v<T, SpeedOverLife>) {
                    for (usize i = 0; i < n; ++i) speed[i] *= mod.curve.Evaluate(std::min(p.NormalizedAge(i), 1.0f));
                }
            },
            m);
    }
    // Move (keeping where they were, for scene collisions).
    const bool scene = collider_ != nullptr && std::any_of(emitter_.update.begin(), emitter_.update.end(), [](const UpdateModule& m) {
        const auto* c = std::get_if<SceneCollision>(&m);
        return c != nullptr && c->enabled;
    });
    if (scene) previous_.assign(p.position.begin(), p.position.begin() + static_cast<std::ptrdiff_t>(n));
    for (usize i = 0; i < n; ++i) p.position[i] = p.position[i] + p.velocity[i] * (speed[i] * step_[i]);

    // Collisions, kill volumes and looks, in the stack's order.
    for (const UpdateModule& m : emitter_.update) {
        std::visit(
            [&](const auto& mod) {
                using T = std::decay_t<decltype(mod)>;
                if (!mod.enabled) return;
                if constexpr (std::is_same_v<T, CollisionPlane>) {
                    const Vec3 normal_m = SafeNormalize(mod.normal, {0, 1, 0});
                    const Vec3 nrm = SafeNormalize(DirectionIn(normal_m, mod.world), {0, 1, 0});
                    const f32 d = PointIn(normal_m * mod.offset, mod.world).Dot(nrm);
                    for (usize i = 0; i < n; ++i) {
                        const f32 radius = p.size[i] * mod.radius_scale;
                        const f32 depth = p.position[i].Dot(nrm) - d - radius;
                        if (depth >= 0.0f) continue;
                        p.position[i] = p.position[i] - nrm * depth; // back onto the plane
                        const f32 into = p.velocity[i].Dot(nrm);
                        if (into < 0.0f) {
                            const Vec3 along = p.velocity[i] - nrm * into;
                            p.velocity[i] = along * (1.0f - std::clamp(mod.friction, 0.0f, 1.0f)) - nrm * (into * mod.bounce);
                            p.age[i] += p.lifetime[i] * mod.lifetime_loss;
                            Record(ParticleEventKind::Collision, i, nrm);
                        }
                    }
                } else if constexpr (std::is_same_v<T, SceneCollision>) {
                    if (!scene) return;
                    const bool local = emitter_.settings.space == SimSpace::Local;
                    for (usize i = 0; i < n; ++i) {
                        const Vec3 from = local ? pose_.ToWorld(previous_[i]) : previous_[i];
                        const Vec3 to = local ? pose_.ToWorld(p.position[i]) : p.position[i];
                        const Vec3 move = to - from;
                        const f32 length = move.Length();
                        if (length < 1e-7f) continue;
                        // Reach one radius past the move, so a particle resting on the surface stays on it.
                        const f32 radius = p.size[i] * mod.radius_scale;
                        Vec3 hit, normal;
                        if (!collider_->Raycast(from, to + move * (radius / length), hit, normal)) continue;
                        const Vec3 nrm = local ? pose_.DirectionToLocal(normal) : normal;
                        const Vec3 at = hit + normal * radius;
                        p.position[i] = local ? pose_.ToLocal(at) : at;
                        const f32 into = p.velocity[i].Dot(nrm);
                        if (into < 0.0f) {
                            const Vec3 along = p.velocity[i] - nrm * into;
                            p.velocity[i] = along * (1.0f - std::clamp(mod.friction, 0.0f, 1.0f)) - nrm * (into * mod.bounce);
                        }
                        p.age[i] += p.lifetime[i] * mod.lifetime_loss;
                        Record(ParticleEventKind::Collision, i, nrm);
                    }
                } else if constexpr (std::is_same_v<T, KillVolume>) {
                    const Vec3 c = PointIn(mod.center, mod.world);
                    for (usize i = 0; i < n; ++i) {
                        bool inside = false;
                        if (mod.shape == VolumeShape::Sphere) {
                            inside = (p.position[i] - c).LengthSq() <= mod.radius * mod.radius;
                        } else {
                            // The box turns with the emitter unless it's in the world.
                            Vec3 local = p.position[i] - c;
                            if (!mod.world && emitter_.settings.space == SimSpace::World) local = pose_.DirectionToLocal(local);
                            if (mod.world && emitter_.settings.space == SimSpace::Local) local = pose_.DirectionToWorld(local);
                            inside = std::abs(local.x) <= mod.half_extents.x && std::abs(local.y) <= mod.half_extents.y && std::abs(local.z) <= mod.half_extents.z;
                        }
                        if (inside == mod.kill_inside) dead[i] = 1;
                    }
                }
            },
            m);
    }
    for (usize i = 0; i < n; ++i) {
        p.size[i] = p.base_size[i];
        p.color[i] = p.base_color[i];
        p.rotation[i] += p.spin[i] * step_[i];
    }
    for (const UpdateModule& m : emitter_.update) {
        if (const auto* s = std::get_if<SizeOverLife>(&m); s != nullptr && s->enabled) {
            for (usize i = 0; i < n; ++i) p.size[i] *= s->curve.Evaluate(std::min(p.NormalizedAge(i), 1.0f));
        } else if (const auto* c = std::get_if<ColorOverLife>(&m); c != nullptr && c->enabled) {
            for (usize i = 0; i < n; ++i) p.color[i] = p.color[i] * c->gradient.Evaluate(std::min(p.NormalizedAge(i), 1.0f));
        }
    }
    // Deaths, from the back so each swap brings in one already handled.
    for (usize i = n; i-- > 0;) {
        if (dead[i] || p.age[i] >= p.lifetime[i]) {
            Record(ParticleEventKind::Death, i, {}, !dead[i]);
            p.Kill(i);
        }
    }
}

Bounds EmitterInstance::ComputeBounds() const {
    Bounds b;
    const ParticleBuffer& p = particles_;
    for (usize i = 0; i < p.count; ++i) {
        const f32 r = p.size[i] * 0.5f;
        const Vec3 lo = p.position[i] - Vec3(r, r, r), hi = p.position[i] + Vec3(r, r, r);
        if (!b.valid) {
            b = {lo, hi, true};
            continue;
        }
        b.min = Vec3(std::min(b.min.x, lo.x), std::min(b.min.y, lo.y), std::min(b.min.z, lo.z));
        b.max = Vec3(std::max(b.max.x, hi.x), std::max(b.max.y, hi.y), std::max(b.max.z, hi.z));
    }
    return b;
}

// --- ParticleSystemInstance ----------------------------------------------------------------------------

ParticleSystemInstance::ParticleSystemInstance(const ParticleSystemAsset& asset, u64 seed)
    : parameters_(asset.parameters), assets_(asset.emitters), rng_(seed ^ 0xA5A5A5A5DEADBEEFULL) {
    const usize n = asset.emitters.size();
    subs_.resize(n);
    needed_masks_.assign(n, 0);
    for (usize i = 0; i < n; ++i) {
        emitters_.push_back(std::make_unique<EmitterInstance>(asset.emitters[i], seed * 0x9E3779B97F4A7C15ULL + i + 1));
        emitters_.back()->SetAutoClearEvents(false);
        for (const SubEmitter& sub : asset.emitters[i].sub_emitters) {
            const i64 target = asset.FindEmitter(sub.emitter);
            if (target < 0 || static_cast<usize>(target) == i) continue; // FX011
            subs_[i].push_back({sub, static_cast<usize>(target)});
            needed_masks_[i] |= EventBit(sub.event);
        }
        emitters_.back()->RecordEvents(needed_masks_[i]);
    }
    // The defaults, into every bound field.
    for (const ParticleParameter& p : parameters_) (void)SetParameter(p.name, p.value);
}

bool ParticleSystemInstance::SetParameter(const std::string& name, const ParameterValue& value, std::string* error) {
    auto it = std::find_if(parameters_.begin(), parameters_.end(), [&](const ParticleParameter& p) { return p.name == name; });
    if (it == parameters_.end()) {
        if (error != nullptr) *error = "no parameter '" + name + "'";
        return false;
    }
    if (it->value.type != value.type) {
        if (error != nullptr) *error = "parameter '" + name + "' is a different type";
        return false;
    }
    it->value = value;
    for (usize i = 0; i < emitters_.size(); ++i) {
        for (const ParameterBinding& b : assets_[i].bindings) {
            if (b.parameter == name) (void)emitters_[i]->SetField(b.field, value); // bad bindings are FX013
        }
    }
    return true;
}

const ParameterValue* ParticleSystemInstance::GetParameter(const std::string& name) const {
    for (const ParticleParameter& p : parameters_) {
        if (p.name == name) return &p.value;
    }
    return nullptr;
}

void ParticleSystemInstance::SetCollider(const ParticleCollider* collider) {
    for (auto& e : emitters_) e->SetCollider(collider);
}

void ParticleSystemInstance::RecordEvents(u8 mask) {
    for (usize i = 0; i < emitters_.size(); ++i) emitters_[i]->RecordEvents(static_cast<u8>(mask | needed_masks_[i]));
}

void ParticleSystemInstance::SetPose(const EmitterPose& pose) {
    for (auto& e : emitters_) e->SetPose(pose);
}
void ParticleSystemInstance::Teleport(const EmitterPose& pose) {
    for (auto& e : emitters_) e->Teleport(pose);
}
void ParticleSystemInstance::Update(f32 dt) {
    for (auto& e : emitters_) e->ClearEvents();
    for (auto& e : emitters_) e->Update(dt);
    // Feed sub-emitters from this frame's events. Their births can be events
    // too (chains), so go round until nothing new, each sub-emitter handling
    // at most its max_per_frame.
    const usize n = emitters_.size();
    std::vector<usize> cursor(n, 0);
    std::vector<std::vector<u32>> handled(n);
    for (usize i = 0; i < n; ++i) handled[i].assign(subs_[i].size(), 0);
    for (int pass = 0; pass < 16; ++pass) {
        bool any = false;
        for (usize i = 0; i < n; ++i) {
            const usize end = emitters_[i]->Events().size();
            for (usize k = cursor[i]; k < end; ++k) {
                const ParticleEvent ev = emitters_[i]->Events()[k]; // a copy: the target may be this one's own list
                for (usize si = 0; si < subs_[i].size(); ++si) {
                    const SubEmitter& sub = subs_[i][si].sub;
                    if (sub.event != ev.kind || handled[i][si] >= sub.max_per_frame) continue;
                    ++handled[i][si];
                    if (rng_.Next01() >= sub.probability) continue;
                    const usize count = static_cast<usize>(std::lround(std::max(rng_.Range(sub.count), 0.0f)));
                    sub_spawns_ += emitters_[subs_[i][si].target]->EmitAt(count, ev.position, ev.velocity * sub.inherit_velocity,
                                                                        sub.inherit_color ? &ev.color : nullptr, sub.inherit_size ? &ev.size : nullptr);
                }
            }
            any |= end > cursor[i];
            cursor[i] = end;
        }
        if (!any) break;
    }
}
void ParticleSystemInstance::Stop() {
    for (auto& e : emitters_) e->Stop();
}
void ParticleSystemInstance::Restart() {
    for (auto& e : emitters_) e->Restart();
}
bool ParticleSystemInstance::Finished() const {
    // Emitters only fed by sub-emitters (no spawn modules of their own) are done when empty.
    return std::all_of(emitters_.begin(), emitters_.end(), [](const auto& e) {
        const bool spawns_itself = std::any_of(e->Asset().spawn.begin(), e->Asset().spawn.end(), [](const SpawnModule& m) {
            return std::visit([](const auto& x) { return x.enabled; }, m);
        });
        return !e->Asset().settings.enabled || e->Finished() || (!spawns_itself && e->Count() == 0);
    });
}
usize ParticleSystemInstance::Count() const {
    usize n = 0;
    for (const auto& e : emitters_) n += e->Count();
    return n;
}

} // namespace aether::vfx
