#include "aether/sequencer/player.h"

#include "aether/ecs/component.h"
#include "aether/reflection/registry.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aether::seq {

namespace {

using reflect::TypeInfo;
using reflect::TypeKind;

bool IsFloatField(const TypeInfo& t) { return t.kind == TypeKind::Float && (t.size == 4 || t.size == 8); }

// Whether a field of this type can be keyed: a number, bool or enum, or a
// struct whose members are all floats.
bool Keyable(const TypeInfo& t) {
    switch (t.kind) {
    case TypeKind::Float: return IsFloatField(t);
    case TypeKind::Int:
    case TypeKind::UInt: return t.size == 1 || t.size == 2 || t.size == 4 || t.size == 8;
    case TypeKind::Bool: return true;
    case TypeKind::Enum: return t.underlying && (t.underlying->size == 1 || t.underlying->size == 2 || t.underlying->size == 4 || t.underlying->size == 8);
    case TypeKind::Struct:
        if (t.fields.empty()) return false;
        for (const reflect::FieldInfo& f : t.fields) {
            if (!f.type || !IsFloatField(*f.type)) return false;
        }
        return true;
    default: return false;
    }
}

template <typename T>
void StoreInt(void* ptr, f32 value) {
    const f64 r = std::round(static_cast<f64>(value));
    const T v = static_cast<T>(std::clamp(r, static_cast<f64>(std::numeric_limits<T>::lowest()), static_cast<f64>(std::numeric_limits<T>::max())));
    std::memcpy(ptr, &v, sizeof(T));
}

void StoreInteger(bool is_signed, u32 size, void* ptr, f32 value) {
    switch (size) {
    case 1: is_signed ? StoreInt<i8>(ptr, value) : StoreInt<u8>(ptr, value); break;
    case 2: is_signed ? StoreInt<i16>(ptr, value) : StoreInt<u16>(ptr, value); break;
    case 4: is_signed ? StoreInt<i32>(ptr, value) : StoreInt<u32>(ptr, value); break;
    case 8: is_signed ? StoreInt<i64>(ptr, value) : StoreInt<u64>(ptr, value); break;
    default: break;
    }
}

void StoreFloat(const TypeInfo& type, void* ptr, f32 value) {
    if (type.size == 4) {
        std::memcpy(ptr, &value, 4);
    } else {
        const f64 v = value;
        std::memcpy(ptr, &v, 8);
    }
}

// Writes `value` into a scalar field.
void StoreScalar(const TypeInfo& type, void* ptr, f32 value) {
    switch (type.kind) {
    case TypeKind::Float: StoreFloat(type, ptr, value); break;
    case TypeKind::Int: StoreInteger(true, type.size, ptr, value); break;
    case TypeKind::UInt: StoreInteger(false, type.size, ptr, value); break;
    case TypeKind::Bool: *static_cast<bool*>(ptr) = value >= 0.5f; break;
    case TypeKind::Enum: StoreInteger(type.underlying->kind != TypeKind::UInt, type.underlying->size, ptr, value); break;
    default: break;
    }
}

} // namespace

SequencePlayer::SequencePlayer(const LevelSequence& sequence, World& world, const GuidIndex& guids, SequenceResolver resolver, std::string path)
    : sequence_(sequence), world_(world), guids_(guids), resolver_(std::move(resolver)) {
    if (!path.empty()) path_.push_back(std::move(path));
    duration_ = sequence.EffectiveDuration();
    Bind();
}

SequencePlayer::SequencePlayer(const LevelSequence& sequence, World& world, const GuidIndex& guids, SequenceResolver resolver, std::vector<std::string> path)
    : sequence_(sequence), world_(world), guids_(guids), resolver_(std::move(resolver)), path_(std::move(path)) {
    duration_ = sequence.EffectiveDuration();
    Bind();
}

SequencePlayer::~SequencePlayer() { Deactivate(false); }

void SequencePlayer::Deactivate(bool send_cue_stops) {
    for (usize i = 0; i < targets_.size(); ++i) {
        Target& t = targets_[i];
        if (send_cue_stops && on_audio && t.cleanup_track) {
            for (const ActiveAudio& active : t.active_audio) {
                AudioKey stop;
                stop.cue = active.cue;
                stop.action = AudioAction::Stop;
                on_audio(*t.cleanup_track, active.entity, stop);
            }
        }
        t.active_audio.clear();
        if (send_cue_stops && on_animation && t.cleanup_track) {
            for (const ActiveAnimation& active : t.active_animation) {
                AnimKey stop;
                stop.montage = active.montage;
                stop.action = AnimAction::Stop;
                stop.rate = active.rate;
                on_animation(*t.cleanup_track, active.entity, stop);
            }
        }
        t.active_animation.clear();
        ReleaseCut(t);
        t.cut = -2;
        for (usize i = 0; i < t.children.size(); ++i) {
            if (t.children[i]) t.children[i]->Deactivate(send_cue_stops);
            if (i < t.child_active.size()) t.child_active[i] = 0;
        }
    }
    // Stop attached cues while their spawned subjects still exist.
    DespawnAll();
    if (!(fade_ == FadeState{})) {
        fade_ = FadeState{};
        if (on_fade) on_fade(fade_);
    }
}

void SequencePlayer::Report(usize index, const std::string& message) {
    Target& t = targets_[index];
    if (t.reported) return;
    t.reported = true;
    problems_.push_back("Track '" + sequence_.tracks[index].id + "': " + message);
}

bool SequencePlayer::ResolveTrack(usize index) {
    const Track& track = sequence_.tracks[index];
    Target& t = targets_[index];
    t.bound = false;
    if (track.type == TrackType::Audio) {
        if (!t.cleanup_track) t.cleanup_track = std::make_unique<Track>(track);
        t.active_audio.reserve(track.audio.size());
    }
    if (track.type == TrackType::Animation) {
        if (!t.cleanup_track) t.cleanup_track = std::make_unique<Track>(track);
        t.active_animation.reserve(track.anims.size());
    }
    if (track.type == TrackType::Spawn) {
        t.spawned.assign(track.spawns.size(), kNullEntity);
        t.spawn_done.assign(track.spawns.size(), 0);
    }
    if (track.type == TrackType::Subsequence) {
        t.children.clear();
        t.children.resize(track.subs.size());
        t.child_active.assign(track.subs.size(), 0);
        for (usize k = 0; k < track.subs.size(); ++k) {
            const std::string where = "Track '" + track.id + "': subsequence '" + track.subs[k].sequence + "' ";
            if (track.subs[k].sequence.empty()) continue; // SQ021
            if (!resolver_) {
                problems_.push_back(where + "can't be played: this player has no way to find sequences");
                continue;
            }
            if (std::find(path_.begin(), path_.end(), track.subs[k].sequence) != path_.end()) {
                problems_.push_back(where + "plays itself (a sequence can't contain itself, directly or through others)");
                continue;
            }
            if (static_cast<int>(path_.size()) >= kMaxSubsequenceDepth) {
                problems_.push_back(where + "is nested too deep (the limit is " + std::to_string(kMaxSubsequenceDepth) + " levels)");
                continue;
            }
            const LevelSequence* child = resolver_(track.subs[k].sequence);
            if (!child) {
                problems_.push_back(where + "wasn't found");
                continue;
            }
            std::vector<std::string> path = path_;
            path.push_back(track.subs[k].sequence);
            auto player = std::unique_ptr<SequencePlayer>(new SequencePlayer(*child, world_, guids_, resolver_, std::move(path)));
            // The child reports through this player's hooks, as they are when it fires.
            SequencePlayer* me = this;
            player->on_event = [me](const Track& tr, const EventKey& e) { if (me->on_event) me->on_event(tr, e); };
            player->on_audio = [me](const Track& tr, Entity en, const AudioKey& a) { if (me->on_audio) me->on_audio(tr, en, a); };
            player->on_animation = [me](const Track& tr, Entity en, const AnimKey& a) { if (me->on_animation) me->on_animation(tr, en, a); };
            player->on_spawn = [me](const Track& tr, Entity parent, const SpawnKey& key) { return me->on_spawn ? me->on_spawn(tr, parent, key) : kNullEntity; };
            player->on_despawn = [me](Entity e) { if (me->on_despawn) me->on_despawn(e); };
            player->on_camera_cut = [me](Entity e) { if (me->on_camera_cut) me->on_camera_cut(e); };
            player->set_active = [me](Entity e, bool on) {
                if (me->set_active) me->set_active(e, on);
                else if (Active* a = me->world_.GetComponent<Active>(e)) a->active = on;
                else if (!on) me->world_.AddComponent(e, Active{false});
            };
            for (const std::string& p : player->Problems()) problems_.push_back(where + "has a problem: " + p);
            t.children[k] = std::move(player);
        }
    }
    t.entity = track.binding.IsNull() ? kNullEntity : guids_.Find(world_, track.binding);
    const bool entity_optional = track.type == TrackType::Event || track.type == TrackType::Spawn || track.type == TrackType::CameraCut ||
                                 track.type == TrackType::Audio || track.type == TrackType::Fade || track.type == TrackType::Subsequence;
    if (entity_optional) {
        // These work without their entity (a bound one that is missing is
        // simply treated as none).
        t.bound = true;
        return true;
    }
    if (t.entity.IsNull()) {
        Report(index, "no entity has the GUID " + ToString(track.binding));
        return false;
    }
    if (track.type == TrackType::Visibility || track.type == TrackType::Animation) {
        t.shown = -1;
        t.bound = true;
        return true;
    }
    if (track.type == TrackType::Transform) {
        if (!world_.GetComponent<Transform>(t.entity)) {
            Report(index, "its entity has no Transform");
            return false;
        }
        t.bound = true;
        return true;
    }
    const TypeInfo* component_type = reflect::TypeRegistry::Find(track.component);
    const ComponentId id = FindComponentIdByName(track.component);
    if (!component_type || id == kInvalidComponentId) {
        Report(index, "no component named '" + track.component + "'");
        return false;
    }
    const reflect::FieldInfo* field = component_type->FindField(track.field);
    if (!field || !field->type) {
        Report(index, "'" + track.component + "' has no field '" + track.field + "'");
        return false;
    }
    if (!Keyable(*field->type)) {
        Report(index, "the field '" + track.component + "." + track.field + "' can't be keyed (a number, bool, enum or a struct of floats can)");
        return false;
    }
    if (!world_.HasComponentRaw(t.entity, id)) {
        Report(index, "its entity has no " + track.component);
        return false;
    }
    t.component = id;
    t.field_type = field->type;
    t.field_offset = field->offset;
    t.bound = true;
    return true;
}

void SequencePlayer::Bind() {
    Deactivate();
    problems_.clear();
    targets_.clear();
    targets_.resize(sequence_.tracks.size());
    duration_ = sequence_.EffectiveDuration();
    for (usize i = 0; i < sequence_.tracks.size(); ++i) ResolveTrack(i);
    time_ = std::clamp(time_, 0.0f, duration_);
}

void SequencePlayer::SetTime(f32 time) {
    time_ = std::clamp(time, 0.0f, duration_);
    fresh_ = time_ <= 0.0f;
    completed_ = false;
}

void SequencePlayer::Stop() {
    playing_ = false;
    time_ = 0.0f;
    fresh_ = true;
    completed_ = false;
    Deactivate();
}

void SequencePlayer::Skip() {
    if (completed_) return;
    playing_ = false;
    time_ = duration_;
    fresh_ = false;
    final_pose_ = true;
    Evaluate();
    final_pose_ = false;
    Deactivate();
    completed_ = true;
    if (on_finished) on_finished();
}

void SequencePlayer::ApplySubsequences(const Track& track, Target& target) {
    for (usize i = 0; i < track.subs.size() && i < target.children.size(); ++i) {
        const SubKey& k = track.subs[i];
        SequencePlayer* child = target.children[i].get();
        if (!child) continue;
        const f32 end = k.time + k.duration;
        const bool inside = time_ >= k.time && (time_ < end || (time_ >= duration_ && time_ <= end));
        if (inside) {
            child->SetTime((time_ - k.time) * k.scale + k.offset);
            child->final_pose_ = final_pose_;
            child->Evaluate();
            child->final_pose_ = false;
            target.child_active[i] = 1;
            if (child->Fade().amount > pending_fade_.amount) pending_fade_ = child->Fade();
        } else if (target.child_active[i]) {
            // Leaving the range: the child's last pose (or its first, going back) is
            // applied, then what it spawned goes, and its cuts and fade.
            const bool after = time_ >= end;
            child->SetTime(after ? k.duration * k.scale + k.offset : k.offset);
            child->final_pose_ = true;
            child->Evaluate();
            child->final_pose_ = false;
            child->Deactivate();
            target.child_active[i] = 0;
        }
    }
}

void SequencePlayer::DespawnAll() {
    for (Target& t : targets_) {
        for (Entity& e : t.spawned) {
            if (!e.IsNull() && on_despawn) on_despawn(e);
            e = kNullEntity;
        }
        std::fill(t.spawn_done.begin(), t.spawn_done.end(), 0);
    }
}

void SequencePlayer::ApplySpawns(const Track& track, Target& target) {
    if (final_pose_) return;
    if (target.spawned.size() != track.spawns.size()) {
        target.spawned.assign(track.spawns.size(), kNullEntity);
        target.spawn_done.assign(track.spawns.size(), 0);
    }
    const Entity parent = target.entity.IsNull() || !world_.IsAlive(target.entity) ? kNullEntity : target.entity;
    for (usize i = 0; i < track.spawns.size(); ++i) {
        const SpawnKey& k = track.spawns[i];
        const bool inside = time_ >= k.time && (k.duration <= 0.0f || time_ < k.time + k.duration);
        Entity& live = target.spawned[i];
        if (!live.IsNull() && !world_.IsAlive(live)) live = kNullEntity; // destroyed by someone else: not respawned until it leaves and re-enters
        if (inside && live.IsNull() && !target.spawn_done[i]) {
            if (on_spawn) live = on_spawn(track, parent, k);
            target.spawn_done[i] = 1;
        } else if (!inside) {
            if (!live.IsNull() && on_despawn) on_despawn(live);
            live = kNullEntity;
            target.spawn_done[i] = 0;
        }
    }
}

void SequencePlayer::ReleaseCut(Target& target) {
    if (!target.cut_camera.IsNull() && world_.IsAlive(target.cut_camera)) {
        if (Camera* c = world_.GetComponent<Camera>(target.cut_camera)) c->priority = target.cut_saved;
    }
    target.cut_camera = kNullEntity;
}

void SequencePlayer::ApplyCuts(const Track& track, Target& target) {
    if (final_pose_) return;
    int index = -1;
    for (usize i = 0; i < track.cuts.size() && track.cuts[i].time <= time_; ++i) index = static_cast<int>(i);
    if (index == target.cut) return;
    target.cut = index;
    ReleaseCut(target);
    Entity camera = kNullEntity;
    if (index >= 0) {
        camera = guids_.Find(world_, track.cuts[static_cast<usize>(index)].camera);
        if (Camera* c = camera.IsNull() ? nullptr : world_.GetComponent<Camera>(camera)) {
            target.cut_camera = camera;
            target.cut_saved = c->priority;
            c->priority = kCutPriority;
        }
    }
    if (on_camera_cut) on_camera_cut(camera);
}

void SequencePlayer::ApplyVisibility(const Track& track, Target& target) {
    if (track.channels.empty() || track.channels[0].Empty()) return;
    const int shown = track.channels[0].Evaluate(time_) >= 0.5f ? 1 : 0;
    if (shown == target.shown) return;
    target.shown = shown;
    if (set_active) {
        set_active(target.entity, shown == 1);
    } else if (Active* a = world_.GetComponent<Active>(target.entity)) {
        a->active = shown == 1;
    } else if (shown == 0) {
        world_.AddComponent(target.entity, Active{false});
    }
}

void SequencePlayer::FireEvents(f32 from, f32 to, bool include_from) {
    const auto crossed = [&](f32 time) { return time > from || (include_from && time >= from); };
    for (usize i = 0; i < sequence_.tracks.size(); ++i) {
        const Track& track = sequence_.tracks[i];
        if (track.mute) continue;
        if (track.type == TrackType::Event && on_event) {
            for (const EventKey& e : track.events) {
                if (e.time > to) break;
                if (crossed(e.time)) on_event(track, e);
            }
        } else if (track.type == TrackType::Audio && on_audio) {
            const Entity entity = track.binding.IsNull() ? kNullEntity : guids_.Find(world_, track.binding);
            for (const AudioKey& k : track.audio) {
                if (k.time > to) break;
                if (!crossed(k.time)) continue;
                auto& active = targets_[i].active_audio;
                const auto existing = std::find_if(active.begin(), active.end(), [&](const ActiveAudio& cue) { return cue.cue == k.cue; });
                if (k.action == AudioAction::Play || k.action == AudioAction::FadeIn) {
                    if (existing == active.end()) active.push_back({k.cue, entity});
                    else existing->entity = entity;
                } else if (k.action == AudioAction::Stop && existing != active.end()) {
                    active.erase(existing);
                }
                on_audio(track, entity, k);
            }
        } else if (track.type == TrackType::Subsequence) {
            for (usize k = 0; k < track.subs.size() && k < targets_[i].children.size(); ++k) {
                const SubKey& key = track.subs[k];
                SequencePlayer* child = targets_[i].children[k].get();
                if (!child) continue;
                const f32 lo = std::max(from, key.time), hi = std::min(to, key.time + key.duration);
                if (hi < lo) continue;
                // Entering the range fires the child's keys at its start; otherwise the same rule as here.
                const bool inc = include_from || from < key.time;
                if (hi == lo && !inc) continue;
                child->FireEvents((lo - key.time) * key.scale + key.offset, (hi - key.time) * key.scale + key.offset, inc);
            }
        } else if (track.type == TrackType::Animation && on_animation) {
            const Entity entity = targets_[i].bound ? targets_[i].entity : kNullEntity;
            for (const AnimKey& k : track.anims) {
                if (k.time > to) break;
                if (!crossed(k.time)) continue;
                auto& active = targets_[i].active_animation;
                const auto existing = std::find_if(active.begin(), active.end(), [&](const ActiveAnimation& montage) { return montage.montage == k.montage; });
                if (k.action == AnimAction::Play) {
                    if (existing == active.end()) active.push_back({k.montage, entity, k.rate});
                    else { existing->entity = entity; existing->rate = k.rate; }
                } else if (existing != active.end()) {
                    active.erase(existing);
                }
                on_animation(track, entity, k);
            }
        }
    }
}

void SequencePlayer::ApplyTransform(const Track& track, Target& target) {
    Transform* t = world_.GetComponent<Transform>(target.entity);
    if (!t) return;
    if (track.channels.size() >= 3) {
        if (!track.channels[0].Empty()) t->position.x = track.channels[0].Evaluate(time_);
        if (!track.channels[1].Empty()) t->position.y = track.channels[1].Evaluate(time_);
        if (!track.channels[2].Empty()) t->position.z = track.channels[2].Evaluate(time_);
    }
    if (!track.rotation.empty()) t->rotation = EvaluateRotation(track.rotation, time_);
}

void SequencePlayer::ApplyProperty(const Track& track, Target& target) {
    u8* base = static_cast<u8*>(world_.GetComponentRaw(target.entity, target.component));
    if (!base) return;
    u8* field = base + target.field_offset;
    const TypeInfo& type = *target.field_type;
    if (type.kind == TypeKind::Struct) {
        for (usize i = 0; i < type.fields.size() && i < track.channels.size(); ++i) {
            if (track.channels[i].Empty()) continue; // a member with no keys keeps its value
            StoreFloat(*type.fields[i].type, field + type.fields[i].offset, track.channels[i].Evaluate(time_));
        }
    } else if (!track.channels.empty() && !track.channels[0].Empty()) {
        StoreScalar(type, field, track.channels[0].Evaluate(time_));
    }
}

void SequencePlayer::Evaluate() {
    pending_fade_ = FadeState{};
    for (usize i = 0; i < sequence_.tracks.size(); ++i) {
        const Track& track = sequence_.tracks[i];
        if (track.mute) continue;
        Target& target = targets_[i];
        // A target whose entity was destroyed is looked up again (it may have been recreated).
        if (target.bound && !target.entity.IsNull() && !world_.IsAlive(target.entity)) {
            target.reported = false;
            target.bound = false;
        }
        if (!target.bound) {
            if (target.reported) continue; // already reported: try again only after Bind()
            if (!ResolveTrack(i)) continue;
        }
        if (track.type == TrackType::Spawn) ApplySpawns(track, target);
        else if (track.type == TrackType::CameraCut) ApplyCuts(track, target);
        else if (track.type == TrackType::Transform) ApplyTransform(track, target);
        else if (track.type == TrackType::Property) ApplyProperty(track, target);
        else if (track.type == TrackType::Visibility) ApplyVisibility(track, target);
        else if (track.type == TrackType::Subsequence) ApplySubsequences(track, target);
        else if (track.type == TrackType::Fade && !track.channels.empty() && !track.channels[0].Empty()) {
            const f32 amount = std::clamp(track.channels[0].Evaluate(time_), 0.0f, 1.0f);
            if (amount > pending_fade_.amount) {
                pending_fade_.amount = amount;
                pending_fade_.color = track.fade_color;
            }
        }
    }
    if (!(pending_fade_ == fade_)) {
        fade_ = pending_fade_;
        if (on_fade) on_fade(fade_);
    }
}

void SequencePlayer::AdvanceTo(f32 time) {
    const f32 target = std::clamp(time, 0.0f, duration_);
    if (target > time_ || (fresh_ && target >= time_)) FireEvents(time_, target, fresh_);
    fresh_ = false;
    time_ = target;
    Evaluate();
}

void SequencePlayer::Update(f32 dt) {
    if (!playing_) return;
    const f32 before = time_;
    const bool include_start = fresh_;
    fresh_ = false;
    time_ += dt * rate_;
    bool finished = false;
    if (rate_ >= 0.0f && time_ >= duration_) {
        finished = true;
        if (loop && duration_ > 0.0f) {
            FireEvents(before, duration_, include_start);
            time_ = std::fmod(time_, duration_);
            FireEvents(0.0f, time_, true);
        } else {
            FireEvents(before, duration_, include_start);
            time_ = duration_;
            playing_ = false;
            completed_ = true;
        }
    } else if (rate_ >= 0.0f) {
        FireEvents(before, time_, include_start);
    } else if (time_ <= 0.0f) {
        finished = true;
        if (loop && duration_ > 0.0f) time_ = duration_ + std::fmod(time_, duration_);
        else {
            time_ = 0.0f;
            playing_ = false;
            completed_ = true;
        }
    }
    Evaluate();
    if (finished && on_finished) on_finished();
}

} // namespace aether::seq
