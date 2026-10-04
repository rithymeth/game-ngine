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

SequencePlayer::SequencePlayer(const LevelSequence& sequence, World& world, const GuidIndex& guids)
    : sequence_(sequence), world_(world), guids_(guids) {
    duration_ = sequence.EffectiveDuration();
    Bind();
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
    if (track.type == TrackType::Event) {
        t.bound = true; // fires without an entity
        return true;
    }
    t.entity = guids_.Find(world_, track.binding);
    if (t.entity.IsNull()) {
        Report(index, "no entity has the GUID " + ToString(track.binding));
        return false;
    }
    if (track.type == TrackType::Visibility) {
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
    problems_.clear();
    targets_.assign(sequence_.tracks.size(), Target{});
    duration_ = sequence_.EffectiveDuration();
    for (usize i = 0; i < sequence_.tracks.size(); ++i) ResolveTrack(i);
    time_ = std::clamp(time_, 0.0f, duration_);
}

void SequencePlayer::SetTime(f32 time) {
    time_ = std::clamp(time, 0.0f, duration_);
    fresh_ = time_ <= 0.0f;
}

void SequencePlayer::Stop() {
    playing_ = false;
    time_ = 0.0f;
    fresh_ = true;
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
    if (!on_event) return;
    for (const Track& track : sequence_.tracks) {
        if (track.type != TrackType::Event || track.mute) continue;
        for (const EventKey& e : track.events) {
            if (e.time > to) break;
            if (e.time > from || (include_from && e.time >= from)) on_event(track, e);
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
    for (usize i = 0; i < sequence_.tracks.size(); ++i) {
        const Track& track = sequence_.tracks[i];
        if (track.mute) continue;
        Target& target = targets_[i];
        // A target whose entity was destroyed is looked up again (it may have been recreated).
        if (target.bound && !world_.IsAlive(target.entity)) {
            target.reported = false;
            target.bound = false;
        }
        if (!target.bound) {
            if (target.reported) continue; // already reported: try again only after Bind()
            if (!ResolveTrack(i)) continue;
        }
        if (track.type == TrackType::Transform) ApplyTransform(track, target);
        else if (track.type == TrackType::Property) ApplyProperty(track, target);
        else if (track.type == TrackType::Visibility) ApplyVisibility(track, target);
    }
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
        }
    } else if (rate_ >= 0.0f) {
        FireEvents(before, time_, include_start);
    } else if (time_ <= 0.0f) {
        finished = true;
        if (loop && duration_ > 0.0f) time_ = duration_ + std::fmod(time_, duration_);
        else {
            time_ = 0.0f;
            playing_ = false;
        }
    }
    Evaluate();
    if (finished && on_finished) on_finished();
}

} // namespace aether::seq
