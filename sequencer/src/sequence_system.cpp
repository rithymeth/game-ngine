#include "aether/sequencer/sequence_system.h"

#include "aether/ecs/component.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"

#include <algorithm>

namespace aether {

void SequenceComponent::Play() { commands.push_back({SequenceCommand::Kind::Play, 0.0f}); }
void SequenceComponent::Pause() { commands.push_back({SequenceCommand::Kind::Pause, 0.0f}); }
void SequenceComponent::Stop() { commands.push_back({SequenceCommand::Kind::Stop, 0.0f}); }
void SequenceComponent::SetTime(f32 seconds) { commands.push_back({SequenceCommand::Kind::SetTime, seconds}); }
void SequenceComponent::SetRate(f32 r) {
    rate = r;
    commands.push_back({SequenceCommand::Kind::SetRate, r});
}
void SequenceComponent::SetLoop(bool l) {
    loop = l;
    commands.push_back({SequenceCommand::Kind::SetLoop, l ? 1.0f : 0.0f});
}

void RegisterSequenceComponents() { (void)GetComponentId<SequenceComponent>(); }

void Sequencer::PlaySequence(const std::string& sequence, bool loop) {
    if (auto* s = seq::SequenceSystem::Active()) s->PlaySequence(sequence, loop);
}
void Sequencer::StopAll() {
    if (auto* s = seq::SequenceSystem::Active()) s->StopAll();
}

} // namespace aether

namespace aether::seq {

namespace {
SequenceSystem* g_active = nullptr;
}

SequenceSystem* SequenceSystem::Active() { return g_active; }
void SequenceSystem::MakeActive() { g_active = this; }

SequenceSystem::SequenceSystem(World& world, GuidIndex& guids, SequenceLookup find, Lifecycle* lifecycle)
    : world_(world), guids_(guids), find_(std::move(find)), lifecycle_(lifecycle) {
    RegisterSequenceComponents();
    MakeActive();
}

SequenceSystem::~SequenceSystem() {
    if (g_active == this) g_active = nullptr;
}

void SequenceSystem::Report(const std::string& message) {
    if (reported_.emplace(message, true).second) problems_.push_back(message);
}

SequencePlayer* SequenceSystem::PlayerOf(Entity entity) {
    const auto it = slots_.find(entity.index);
    return it != slots_.end() && it->second.entity == entity ? it->second.player.get() : nullptr;
}

Entity SequenceSystem::PlaySequence(const std::string& sequence, bool loop) {
    if (!find_ || !find_(sequence)) {
        Report("Sequence '" + sequence + "' not found");
        return kNullEntity;
    }
    SequenceComponent c;
    c.sequence = sequence;
    c.auto_play = true;
    c.loop = loop;
    c.destroy_when_finished = true;
    return world_.CreateEntity(c);
}

void SequenceSystem::StopAll() {
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<SequenceComponent>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            for (usize i = 0; i < archetype.ChunkEntityCount(c); ++i) {
                if (SequenceComponent* s = world_.GetComponent<SequenceComponent>(e[i])) s->Stop();
            }
        }
    });
}

void SequenceSystem::Update(f32 dt) {
    events_.clear();
    const u64 now = ++generation_;
    std::vector<Entity> entities;
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<SequenceComponent>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            entities.insert(entities.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return a.index < b.index; });

    std::vector<Entity> to_destroy;
    for (Entity e : entities) {
        SequenceComponent& comp = *world_.GetComponent<SequenceComponent>(e);
        auto it = slots_.find(e.index);
        const bool fresh = it == slots_.end() || it->second.entity != e;
        if (fresh) it = slots_.insert_or_assign(e.index, Slot{}).first;
        Slot& slot = it->second;
        slot.seen = now;
        // (Re)make the player when the component is new, or points at another sequence.
        if (fresh || slot.path != comp.sequence) {
            slot.entity = e;
            slot.path = comp.sequence;
            slot.player.reset();
            slot.sequence = comp.sequence.empty() || !find_ ? nullptr : find_(comp.sequence);
            if (slot.sequence) {
                slot.player = std::make_unique<SequencePlayer>(*slot.sequence, world_, guids_);
                SequencePlayer* player = slot.player.get();
                Slot* slot_ptr = &slot; // std::map nodes don't move
                player->loop = comp.loop;
                player->SetRate(comp.rate);
                player->set_active = [this](Entity target, bool active) {
                    if (lifecycle_) lifecycle_->SetActive(target, active);
                    else if (aether::Active* a = world_.GetComponent<aether::Active>(target)) a->active = active;
                    else world_.AddComponent(target, aether::Active{active});
                };
                player->on_event = [this, e](const Track& track, const EventKey& key) {
                    SequenceEvent ev;
                    ev.kind = SequenceEvent::Kind::Marker;
                    ev.entity = e;
                    ev.name = key.name;
                    ev.payload = key.payload;
                    ev.subject = track.binding.IsNull() ? kNullEntity : guids_.Find(world_, track.binding);
                    events_.push_back(std::move(ev));
                };
                player->on_finished = [slot_ptr] { slot_ptr->finished = true; };
                for (const std::string& p : player->Problems()) Report("Sequence '" + comp.sequence + "': " + p);
                if (comp.auto_play) {
                    player->Play();
                    player->Evaluate(); // the state at time 0 is in place at once
                }
            } else if (!comp.sequence.empty()) {
                Report("Sequence '" + comp.sequence + "' not found");
            }
        }
        SequencePlayer* player = slot.player.get();
        if (!player) {
            comp.commands.clear();
            continue;
        }
        for (const SequenceCommand& c : comp.commands) {
            switch (c.kind) {
            case SequenceCommand::Kind::Play:
                player->Play();
                break;
            case SequenceCommand::Kind::Pause: player->Pause(); break;
            case SequenceCommand::Kind::Stop:
                player->Stop();
                player->Evaluate();
                break;
            case SequenceCommand::Kind::SetTime:
                player->SetTime(c.value);
                player->Evaluate();
                break;
            case SequenceCommand::Kind::SetRate: player->SetRate(c.value); break;
            case SequenceCommand::Kind::SetLoop: player->loop = c.value >= 0.5f; break;
            }
        }
        comp.commands.clear();
        player->loop = comp.loop;
        player->SetRate(comp.rate);
        slot.finished = false;
        player->Update(dt);
        for (const std::string& p : player->Problems()) Report("Sequence '" + comp.sequence + "': " + p);
        comp.time = player->Time();
        comp.playing = player->Playing();
        if (slot.finished) {
            SequenceEvent ev;
            ev.kind = SequenceEvent::Kind::Finished;
            ev.entity = e;
            ev.name = comp.sequence;
            events_.push_back(std::move(ev));
            if (comp.destroy_when_finished && !comp.playing) to_destroy.push_back(e);
        }
    }
    // Players whose entity is gone.
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (it->second.seen != now) it = slots_.erase(it);
        else ++it;
    }
    for (Entity e : to_destroy) {
        if (lifecycle_) lifecycle_->Destroy(e);
        else world_.DestroyEntity(e);
    }
}

} // namespace aether::seq
