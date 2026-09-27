#include "aether/audio/audio_system.h"

#include "aether/ecs/component.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"

#include <algorithm>
#include <cmath>

namespace aether {

void AudioSource::Play() { commands.push_back({AudioSourceCommand::Kind::Play, 0.0f, {}}); }
void AudioSource::Stop() { commands.push_back({AudioSourceCommand::Kind::Stop, 0.0f, {}}); }
void AudioSource::FadeIn(f32 seconds) { commands.push_back({AudioSourceCommand::Kind::FadeIn, seconds, {}}); }
void AudioSource::FadeOut(f32 seconds) { commands.push_back({AudioSourceCommand::Kind::FadeOut, seconds, {}}); }
void AudioSource::SetVolume(f32 db) {
    volume_db = db;
    commands.push_back({AudioSourceCommand::Kind::SetVolume, db, {}});
}
void AudioSource::SetCue(const std::string& name) {
    cue = name;
    commands.push_back({AudioSourceCommand::Kind::SetCue, 0.0f, name});
}

void RegisterAudioComponents() {
    (void)GetComponentId<AudioSource>();
    (void)GetComponentId<AudioListener>();
    (void)GetComponentId<ReverbZone>();
}

void Audio::PlaySound2D(const std::string& cue, f32 volume_db, f32 pitch) {
    if (auto* s = audio::AudioSystem::Active()) s->PlaySound2D(cue, volume_db, pitch);
}
void Audio::PlaySoundAtLocation(const std::string& cue, const Vec3& location, f32 volume_db, f32 pitch) {
    if (auto* s = audio::AudioSystem::Active()) s->PlaySoundAtLocation(cue, location, volume_db, pitch);
}
void Audio::SpawnSoundAttached(const std::string& cue, const Entity& target, const Vec3& offset, f32 volume_db) {
    if (auto* s = audio::AudioSystem::Active()) s->SpawnSoundAttached(cue, target, offset, volume_db);
}
void Audio::SetBusVolume(const std::string& bus, f32 volume_db, f32 fade_seconds) {
    if (auto* s = audio::AudioSystem::Active()) s->SetBusVolume(bus, volume_db, fade_seconds);
}
void Audio::StopAllSounds() {
    if (auto* s = audio::AudioSystem::Active()) s->StopAll();
}

} // namespace aether

namespace aether::audio {

namespace {

AudioSystem* g_active = nullptr;

Vec3 Rotate(const Quaternion& q, const Vec3& v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = u.Cross(v) * 2.0f;
    return v + t * q.w + u.Cross(t);
}

constexpr f32 kStopFade = 0.05f; // sounds whose entity goes away fade out this fast

} // namespace

AudioSystem* AudioSystem::Active() { return g_active; }
void AudioSystem::MakeActive() { g_active = this; }

AudioSystem::AudioSystem(World& world, Mixer& mixer, const SoundBank& bank, std::function<const SoundCue*(const std::string&)> cues,
                         const GuidIndex* guids)
    : world_(world), mixer_(mixer), bank_(bank), cues_(std::move(cues)), guids_(guids), player_(mixer, bank) {
    RegisterAudioComponents();
    MakeActive();
}

AudioSystem::~AudioSystem() {
    StopAll();
    if (reverb_ != nullptr) SetReverb(0.0f, reverb_room_size_, reverb_damping_); // the effect stays on the mixer's bus, silent
    if (g_active == this) g_active = nullptr;
}

const SoundCue* AudioSystem::FindCue(const std::string& name) {
    const SoundCue* cue = cues_ ? cues_(name) : nullptr;
    if (reported_.count(name) != 0) return reported_[name] ? cue : nullptr;
    // First use of this name: check it once.
    bool ok = cue != nullptr;
    if (!ok) {
        problems_.push_back("no sound cue named '" + name + "'");
    } else {
        for (const CueDiagnostic& d : ValidateCue(*cue, [&](const std::string& s) { return bank_.Contains(s); })) {
            if (d.error) {
                problems_.push_back("sound cue '" + name + "': " + d.code + ": " + d.message);
                ok = false;
            }
        }
    }
    reported_[name] = ok;
    return ok ? cue : nullptr;
}

CueHandle AudioSystem::Start(const std::string& name, const CuePlayParams& params) {
    const SoundCue* cue = FindCue(name);
    return cue == nullptr ? 0 : player_.Play(*cue, params);
}

bool AudioSystem::WorldPose(Entity e, Vec3& position, Quaternion& rotation) const {
    const Transform* t = world_.IsAlive(e) ? world_.GetComponent<Transform>(e) : nullptr;
    if (t == nullptr) return false;
    position = t->position;
    rotation = t->rotation;
    if (guids_ != nullptr) {
        // Up the parent chain (same limit as the hierarchy's).
        Entity at = GetParent(world_, *guids_, e);
        for (int depth = 0; !at.IsNull() && depth < kMaxHierarchyDepth; ++depth) {
            const Transform* p = world_.GetComponent<Transform>(at);
            if (p == nullptr) break;
            position = p->position + Rotate(p->rotation, position);
            rotation = p->rotation * rotation;
            at = GetParent(world_, *guids_, at);
        }
    }
    return true;
}

CueHandle AudioSystem::PlaySound2D(const std::string& cue, f32 volume_db, f32 pitch) {
    CuePlayParams p;
    p.volume_db = volume_db;
    p.pitch = pitch;
    p.force_2d = true;
    return Start(cue, p);
}

CueHandle AudioSystem::PlaySoundAtLocation(const std::string& cue, const Vec3& location, f32 volume_db, f32 pitch) {
    CuePlayParams p;
    p.volume_db = volume_db;
    p.pitch = pitch;
    p.position = location;
    return Start(cue, p);
}

CueHandle AudioSystem::SpawnSoundAttached(const std::string& cue, Entity target, const Vec3& offset, f32 volume_db) {
    Vec3 position;
    Quaternion rotation;
    if (!WorldPose(target, position, rotation)) {
        problems_.push_back("Spawn Sound Attached: entity " + std::to_string(target.index) + " doesn't exist or has no Transform");
        return 0;
    }
    CuePlayParams p;
    p.volume_db = volume_db;
    p.position = position + Rotate(rotation, offset);
    const CueHandle h = Start(cue, p);
    if (h != 0) attached_.push_back({h, target, offset, p.position});
    return h;
}

bool AudioSystem::SetBusVolume(const std::string& name, f32 volume_db, f32 fade_seconds) {
    const BusId bus = mixer_.FindBus(name);
    if (bus == Mixer::kInvalidBus) {
        problems_.push_back("Set Bus Volume: no bus named '" + name + "'");
        return false;
    }
    std::erase_if(fades_, [&](const BusFade& f) { return f.bus == bus; }); // the newest fade wins
    if (fade_seconds <= 0.0f) return mixer_.SetBusVolume(bus, volume_db);
    fades_.push_back({bus, mixer_.BusVolume(bus), volume_db, fade_seconds, 0.0f});
    return true;
}

void AudioSystem::StopAll() {
    player_.StopAll();
    for (auto& [index, s] : sources_) s.handle = 0;
    attached_.clear();
}

CueHandle AudioSystem::HandleOf(Entity e) const {
    const auto it = sources_.find(e.index);
    return it == sources_.end() || it->second.entity != e ? 0 : it->second.handle;
}

void AudioSystem::UpdateListener(f32 dt) {
    Entity chosen;
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<AudioListener>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            for (usize i = 0; i < archetype.ChunkEntityCount(c); ++i) {
                if (world_.GetComponent<AudioListener>(e[i])->active && (chosen.IsNull() || e[i].index < chosen.index)) chosen = e[i];
            }
        }
    });
    Vec3 position;
    Quaternion rotation;
    if (chosen.IsNull() || !WorldPose(chosen, position, rotation)) return; // keep the last listener
    Listener l;
    l.position = position;
    l.forward = Rotate(rotation, {0.0f, 0.0f, -1.0f});
    l.up = Rotate(rotation, {0.0f, 1.0f, 0.0f});
    // Velocity from movement (for doppler); none on the first frame or after switching listeners.
    const bool continuing = listener_seen_ && chosen == listener_entity_ && dt > 0.0f;
    l.velocity = continuing ? (position - listener_last_) * (1.0f / dt) : Vec3{};
    listener_entity_ = chosen;
    listener_last_ = position;
    listener_seen_ = true;
    mixer_.SetListener(l);
}

void AudioSystem::UpdateSources(f32 dt) {
    const u64 now = ++generation_;
    std::vector<Entity> entities;
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<AudioSource>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            entities.insert(entities.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return a.index < b.index; });
    for (Entity e : entities) {
        AudioSource& a = *world_.GetComponent<AudioSource>(e);
        auto it = sources_.find(e.index);
        const bool fresh = it == sources_.end() || it->second.entity != e;
        if (fresh) {
            if (it != sources_.end()) player_.Stop(it->second.handle, kStopFade); // a recycled slot
            it = sources_.insert_or_assign(e.index, Source{e, a.cue, 0, {}, 0}).first;
        }
        Source& s = it->second;
        s.seen = now;
        Vec3 position;
        Quaternion rotation;
        const bool placed = WorldPose(e, position, rotation);
        if (!placed) position = s.last_position;
        auto play = [&](f32 fade_in) {
            player_.Stop(s.handle);
            CuePlayParams p;
            p.volume_db = a.volume_db;
            p.pitch = a.pitch;
            p.fade_in = fade_in;
            p.position = position;
            s.cue = a.cue;
            s.handle = Start(a.cue, p);
        };
        if (fresh && a.auto_play) play(0.0f);
        for (const AudioSourceCommand& c : a.commands) {
            switch (c.kind) {
            case AudioSourceCommand::Kind::Play: play(0.0f); break;
            case AudioSourceCommand::Kind::FadeIn: play(std::max(c.value, 0.0f)); break;
            case AudioSourceCommand::Kind::Stop:
                player_.Stop(s.handle);
                s.handle = 0;
                break;
            case AudioSourceCommand::Kind::FadeOut:
                player_.Stop(s.handle, std::max(c.value, 0.0f));
                s.handle = 0; // it finishes fading on its own, and isn't reported as finished
                break;
            case AudioSourceCommand::Kind::SetVolume: player_.SetVolume(s.handle, c.value); break;
            case AudioSourceCommand::Kind::SetCue:
                // Switching cues while playing plays the new one.
                if (s.handle != 0 && player_.IsPlaying(s.handle)) play(0.0f);
                else s.cue = a.cue;
                break;
            }
        }
        a.commands.clear();
        // Follow the entity; velocity from its movement, for doppler.
        if (s.handle != 0) {
            const Vec3 velocity = !fresh && dt > 0.0f ? (position - s.last_position) * (1.0f / dt) : Vec3{};
            player_.SetPosition(s.handle, position, velocity);
        }
        s.last_position = position;
    }
    // Entities gone, or without an AudioSource any more: stop their sounds.
    for (auto it = sources_.begin(); it != sources_.end();) {
        if (it->second.seen == now) {
            ++it;
        } else {
            player_.Stop(it->second.handle, kStopFade);
            it = sources_.erase(it);
        }
    }
    // Attached sounds follow their entity, and stop with it.
    for (usize i = 0; i < attached_.size();) {
        Attached& a = attached_[i];
        Vec3 position;
        Quaternion rotation;
        if (!player_.IsPlaying(a.handle)) {
            attached_.erase(attached_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (!WorldPose(a.target, position, rotation)) {
            player_.Stop(a.handle, kStopFade);
            attached_.erase(attached_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        position = position + Rotate(rotation, a.offset);
        player_.SetPosition(a.handle, position, dt > 0.0f ? (position - a.last_position) * (1.0f / dt) : Vec3{});
        a.last_position = position;
        ++i;
    }
}

void AudioSystem::UpdateReverb() {
    // The listener's zone: highest priority, then strongest.
    const Vec3 at = mixer_.GetListener().position;
    const ReverbZone* best = nullptr;
    f32 best_weight = 0.0f;
    bool any = false;
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<ReverbZone>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            for (usize i = 0; i < archetype.ChunkEntityCount(c); ++i) {
                any = true;
                Vec3 center;
                Quaternion rotation;
                if (!WorldPose(e[i], center, rotation)) continue;
                const ReverbZone& z = *world_.GetComponent<ReverbZone>(e[i]);
                const f32 d = (at - center).Length();
                const f32 w = d <= z.radius ? 1.0f : z.blend_distance > 0.0f ? std::clamp(1.0f - (d - z.radius) / z.blend_distance, 0.0f, 1.0f) : 0.0f;
                if (w <= 0.0f) continue;
                if (best == nullptr || z.priority > best->priority || (z.priority == best->priority && w > best_weight)) {
                    best = &z;
                    best_weight = w;
                }
            }
        }
    });
    if (!any && reverb_ == nullptr) return; // no zones: no effect at all
    if (reverb_ == nullptr) {
        BusId bus = mixer_.FindBus(reverb_bus);
        if (bus == Mixer::kInvalidBus) bus = kMasterBus;
        auto effect = std::make_unique<ReverbEffect>();
        effect->wet = 0.0f;
        effect->dry = 1.0f;
        reverb_room_size_ = effect->room_size;
        reverb_damping_ = effect->damping;
        reverb_wet_ = 0.0f;
        reverb_ = static_cast<ReverbEffect*>(mixer_.AddEffect(bus, std::move(effect)));
    }
    reverb_strength_ = best_weight;
    if (best != nullptr) {
        SetReverb(best->wet * best_weight, best->room_size, best->damping);
    } else {
        SetReverb(0.0f, reverb_room_size_, reverb_damping_);
    }
}

void AudioSystem::SetReverb(f32 wet, f32 room_size, f32 damping) {
    if (wet == reverb_wet_ && room_size == reverb_room_size_ && damping == reverb_damping_) return;
    reverb_wet_ = wet, reverb_room_size_ = room_size, reverb_damping_ = damping;
    // On the audio thread when the mixer is threaded.
    mixer_.Post([effect = reverb_, wet, room_size, damping](Mixer&) {
        effect->wet = wet;
        effect->room_size = room_size;
        effect->damping = damping;
    });
}

void AudioSystem::Update(f32 dt) {
    events_.clear();
    UpdateListener(dt);
    // Cues that ended by themselves since the last Update.
    for (auto& [index, s] : sources_) {
        if (s.handle != 0 && !player_.IsPlaying(s.handle)) {
            events_.push_back({s.entity, s.cue});
            s.handle = 0;
        }
    }
    UpdateSources(dt);
    for (usize i = 0; i < fades_.size();) {
        BusFade& f = fades_[i];
        f.elapsed += dt;
        const f32 t = std::clamp(f.elapsed / f.duration, 0.0f, 1.0f);
        mixer_.SetBusVolume(f.bus, f.from + (f.to - f.from) * t);
        if (t >= 1.0f) {
            fades_.erase(fades_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    UpdateReverb();
    mixer_.UpdateOcclusion();
    player_.Update();
    // Keep the components' playing flags current.
    for (auto& [index, s] : sources_) {
        if (AudioSource* a = world_.GetComponent<AudioSource>(s.entity)) a->playing = s.handle != 0 && player_.IsPlaying(s.handle);
    }
}

} // namespace aether::audio
