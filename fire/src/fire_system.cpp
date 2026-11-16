#include "aether/fire/fire_system.h"

#include "aether/fire/fire.h"
#include "aether/renderer/components.h"
#include "aether/scene/components.h"
#include "aether/vfx/particle_system.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace aether::fire {

namespace {

FireSystem* g_active = nullptr;

constexpr f32 kFlameFadeSeconds = 3.0f; // a flame that is out is left this long to die away before its entity goes

u64 Key(Entity e) { return (static_cast<u64>(e.generation) << 32) | e.index; }
bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
f32 Clamp01(f32 v) { return std::clamp(v, 0.0f, 1.0f); }

bool Usable(const Flammable& f) {
    return f.enabled && std::isfinite(f.fuel) && f.fuel > 0.0f && std::isfinite(f.ignition_heat) && f.ignition_heat > 0.0f && std::isfinite(f.spread_radius) &&
           std::isfinite(f.heat_rate) && std::isfinite(f.cool_rate) && std::isfinite(f.wetness) && std::isfinite(f.douse_wetness) && Finite(f.flame_offset);
}

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

} // namespace

struct FireSystem::Impl {
    enum class Phase : u8 { Idle, Burning, Burnt };
    struct State {
        Phase phase = Phase::Idle;
        f32 heat = 0.0f;
        f32 fuel_left = -1.0f; // set from the component the first time it is seen
        Entity flame;          // the flame entity while it burns
    };
    struct Dying {
        Entity flame;
        f32 left = 0.0f;
    };
    std::unordered_map<u64, State> states;
    std::vector<Dying> dying;
};

FireSystem::FireSystem(World& world) : impl_(std::make_unique<Impl>()), world_(world) { MakeActive(); }

FireSystem::~FireSystem() {
    if (g_active == this) g_active = nullptr;
}

FireSystem* FireSystem::Active() { return g_active; }
void FireSystem::MakeActive() { g_active = this; }

namespace {

// The flame for one burning thing: a child entity with the particle system and the light, or null when the Flammable asks for neither.
Entity MakeFlame(World& world, const Flammable& f, const Vec3& at) {
    const bool smoke = f.flame_effect[0] != '\0';
    const bool light = f.light_intensity > 0.0f;
    if (!smoke && !light) return Entity{};
    const Transform transform{at + f.flame_offset, Quaternion::Identity()};
    Entity flame = world.CreateEntity(transform);
    if (smoke) {
        ParticleSystem ps;
        ps.asset = f.flame_effect;
        ps.auto_activate = true;
        world.AddComponent<ParticleSystem>(flame, ps);
    }
    if (light) {
        PointLight pl;
        pl.color = f.light_color;
        pl.intensity = f.light_intensity;
        pl.range = std::max(f.light_range, 0.1f);
        pl.cast_shadows = false;
        world.AddComponent<PointLight>(flame, pl);
    }
    return flame;
}

} // namespace

bool FireSystem::Ignite(Entity entity) {
    if (!world_.IsAlive(entity)) return false;
    const Flammable* f = world_.GetComponent<Flammable>(entity);
    const Transform* at = world_.GetComponent<Transform>(entity);
    if (f == nullptr || at == nullptr || !Usable(*f) || !Finite(at->position)) return false;
    Impl::State& s = impl_->states[Key(entity)];
    if (s.fuel_left < 0.0f) s.fuel_left = f->fuel;
    if (s.phase != Impl::Phase::Idle || f->wetness >= f->douse_wetness || s.fuel_left <= 0.0f) return false;
    s.phase = Impl::Phase::Burning;
    s.heat = f->ignition_heat;
    s.flame = MakeFlame(world_, *f, at->position);
    return true;
}

bool FireSystem::Extinguish(Entity entity) {
    const auto it = impl_->states.find(Key(entity));
    if (it == impl_->states.end() || it->second.phase != Impl::Phase::Burning) return false;
    Impl::State& s = it->second;
    s.phase = Impl::Phase::Idle;
    s.heat = 0.0f;
    if (!s.flame.IsNull() && world_.IsAlive(s.flame)) {
        if (ParticleSystem* ps = world_.GetComponent<ParticleSystem>(s.flame)) ps->Deactivate();
        if (PointLight* pl = world_.GetComponent<PointLight>(s.flame)) pl->intensity = 0.0f;
        impl_->dying.push_back({s.flame, kFlameFadeSeconds});
    }
    s.flame = Entity{};
    return true;
}

usize FireSystem::IgniteNear(const Vec3& centre, f32 radius) {
    if (!Finite(centre) || !std::isfinite(radius) || radius < 0.0f) return 0;
    usize caught = 0;
    for (const Entity e : EntitiesWith(world_, GetComponentId<Flammable>())) {
        const Transform* at = world_.GetComponent<Transform>(e);
        if (at == nullptr || !Finite(at->position)) continue;
        if ((at->position - centre).LengthSq() <= radius * radius && Ignite(e)) ++caught;
    }
    return caught;
}

bool FireSystem::IsBurning(Entity entity) const {
    const auto it = impl_->states.find(Key(entity));
    return it != impl_->states.end() && it->second.phase == Impl::Phase::Burning;
}

bool FireSystem::IsBurnt(Entity entity) const {
    const auto it = impl_->states.find(Key(entity));
    return it != impl_->states.end() && it->second.phase == Impl::Phase::Burnt;
}

f32 FireSystem::Heat(Entity entity) const {
    const auto it = impl_->states.find(Key(entity));
    return it != impl_->states.end() ? it->second.heat : 0.0f;
}

f32 FireSystem::FuelLeft(Entity entity) const {
    const auto it = impl_->states.find(Key(entity));
    if (it != impl_->states.end() && it->second.fuel_left >= 0.0f) return it->second.fuel_left;
    const Flammable* f = world_.IsAlive(entity) ? world_.GetComponent<Flammable>(entity) : nullptr;
    return f != nullptr && Usable(*f) ? f->fuel : 0.0f; // never lit: all of it
}

usize FireSystem::BurningCount() const {
    usize n = 0;
    for (const auto& kv : impl_->states) n += kv.second.phase == Impl::Phase::Burning ? 1 : 0;
    return n;
}

Entity FireSystem::FlameOf(Entity entity) const {
    const auto it = impl_->states.find(Key(entity));
    return it != impl_->states.end() ? it->second.flame : Entity{};
}

bool FireSystem::SetWetness(Entity entity, f32 wetness) {
    if (!std::isfinite(wetness) || !world_.IsAlive(entity)) return false;
    Flammable* f = world_.GetComponent<Flammable>(entity);
    if (f == nullptr) return false;
    f->wetness = Clamp01(wetness);
    return true;
}

usize FireSystem::WetAll(f32 wetness) {
    if (!std::isfinite(wetness)) return 0;
    usize n = 0;
    for (const Entity e : EntitiesWith(world_, GetComponentId<Flammable>())) {
        if (Flammable* f = world_.GetComponent<Flammable>(e)) {
            f->wetness = Clamp01(wetness);
            ++n;
        }
    }
    return n;
}

usize FireSystem::Advance(f32 dt) {
    if (!std::isfinite(dt) || dt <= 0.0f) return BurningCount();

    struct Info {
        Entity entity;
        Flammable f;
        Vec3 position;
    };
    std::vector<Info> infos;
    for (const Entity e : EntitiesWith(world_, GetComponentId<Flammable>())) {
        const Flammable* f = world_.GetComponent<Flammable>(e);
        const Transform* at = world_.GetComponent<Transform>(e);
        if (f == nullptr || at == nullptr || !Finite(at->position)) continue;
        infos.push_back({e, *f, at->position});
    }

    // 1. Forget what is gone or switched off; a flame it had is left to die away.
    const auto release_flame = [&](Impl::State& s) {
        if (!s.flame.IsNull() && world_.IsAlive(s.flame)) {
            if (ParticleSystem* ps = world_.GetComponent<ParticleSystem>(s.flame)) ps->Deactivate();
            if (PointLight* pl = world_.GetComponent<PointLight>(s.flame)) pl->intensity = 0.0f;
            impl_->dying.push_back({s.flame, kFlameFadeSeconds});
        }
        s.flame = Entity{};
    };
    {
        std::unordered_map<u64, bool> present;
        for (const Info& i : infos)
            if (Usable(i.f)) present[Key(i.entity)] = true;
        for (auto it = impl_->states.begin(); it != impl_->states.end();) {
            if (present.count(it->first) == 0) {
                release_flame(it->second);
                it = impl_->states.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 2. What burns: the fuel goes, and the wet go out.
    std::vector<usize> burning; // indices into infos, in entity order, of what was alight at the start of this step
    std::vector<Entity> consumed;
    for (usize k = 0; k < infos.size(); ++k) {
        const Info& i = infos[k];
        if (!Usable(i.f)) continue;
        Impl::State& s = impl_->states[Key(i.entity)];
        if (s.fuel_left < 0.0f) s.fuel_left = i.f.fuel;
        if (s.phase != Impl::Phase::Burning) continue;
        if (i.f.wetness >= i.f.douse_wetness) {
            Extinguish(i.entity);
            continue;
        }
        s.fuel_left -= dt;
        if (s.fuel_left <= 0.0f) {
            s.fuel_left = 0.0f;
            s.phase = Impl::Phase::Burnt;
            s.heat = 0.0f;
            release_flame(s);
            if (i.f.consume) consumed.push_back(i.entity);
            continue;
        }
        burning.push_back(k);
    }

    // 3. Heat: what each burning one gives out is worked out first, so the order of the things does not matter.
    std::vector<f32> gain(infos.size(), 0.0f);
    for (const usize b : burning) {
        const Info& from = infos[b];
        const f32 radius = from.f.spread_radius;
        if (!(radius > 0.0f) || !(from.f.heat_rate > 0.0f)) continue;
        for (usize k = 0; k < infos.size(); ++k) {
            if (k == b || !Usable(infos[k].f)) continue;
            const Impl::State& to = impl_->states[Key(infos[k].entity)];
            if (to.phase != Impl::Phase::Idle) continue;
            const f32 d = (infos[k].position - from.position).Length();
            if (d >= radius || infos[k].f.wetness >= infos[k].f.douse_wetness) continue; // too wet to heat at all
            gain[k] += from.f.heat_rate * (1.0f - d / radius) * dt * (1.0f - Clamp01(infos[k].f.wetness));
        }
    }
    for (usize k = 0; k < infos.size(); ++k) {
        const Info& i = infos[k];
        if (!Usable(i.f)) continue;
        Impl::State& s = impl_->states[Key(i.entity)];
        if (s.phase != Impl::Phase::Idle) continue;
        if (gain[k] > 0.0f) s.heat += gain[k];
        else s.heat = std::max(0.0f, s.heat - i.f.cool_rate * dt);
        if (s.heat >= i.f.ignition_heat) Ignite(i.entity);
    }

    // 4. The flames follow what burns.
    for (const Info& i : infos) {
        const auto it = impl_->states.find(Key(i.entity));
        if (it == impl_->states.end() || it->second.phase != Impl::Phase::Burning) continue;
        const Entity flame = it->second.flame;
        if (flame.IsNull() || !world_.IsAlive(flame)) continue;
        if (Transform* t = world_.GetComponent<Transform>(flame)) t->position = i.position + i.f.flame_offset;
    }

    // 5. Flames that are out are let go once they have died away; what has burnt out and was to be consumed goes.
    for (auto it = impl_->dying.begin(); it != impl_->dying.end();) {
        it->left -= dt;
        if (it->left <= 0.0f || !world_.IsAlive(it->flame)) {
            if (world_.IsAlive(it->flame)) world_.DestroyEntity(it->flame);
            it = impl_->dying.erase(it);
        } else {
            ++it;
        }
    }
    for (const Entity e : consumed) {
        impl_->states.erase(Key(e));
        if (world_.IsAlive(e)) world_.DestroyEntity(e);
    }
    return BurningCount();
}

} // namespace aether::fire
