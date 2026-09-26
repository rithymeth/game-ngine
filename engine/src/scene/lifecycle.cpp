#include "aether/scene/lifecycle.h"

#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"

#include <algorithm>

namespace aether {

void Lifecycle::Register(ComponentId component, LifecycleCallbacks callbacks) {
    registrations_.push_back({component, std::move(callbacks)});
}

u32 Lifecycle::Depth(Entity entity) const {
    u32 depth = 0;
    for (Entity parent = GetParent(world_, guids_, entity); !parent.IsNull() && depth < kMaxHierarchyDepth;
         parent = GetParent(world_, guids_, parent)) {
        ++depth;
    }
    return depth;
}

std::vector<Lifecycle::Item> Lifecycle::Ordered(bool parents_first) const {
    std::vector<Item> items;
    items.reserve(tracked_.size());
    for (const auto& [key, state] : tracked_) {
        if (world_.IsAlive(state.entity)) {
            items.push_back({key, state.entity, Depth(state.entity)});
        }
    }
    std::sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
        if (a.depth != b.depth) {
            return parents_first ? a.depth < b.depth : a.depth > b.depth;
        }
        if (a.entity.index != b.entity.index) {
            return parents_first ? a.entity.index < b.entity.index : a.entity.index > b.entity.index;
        }
        return a.key.registration < b.key.registration;
    });
    return items;
}

void Lifecycle::Remove(const Key& key, const State& state) {
    const LifecycleCallbacks& callbacks = registrations_[key.registration].callbacks;
    if (state.enabled && callbacks.on_disable) {
        callbacks.on_disable(state.entity);
    }
    if (callbacks.on_destroy) {
        callbacks.on_destroy(state.entity);
    }
}

void Lifecycle::Sync() {
    // 1. What should be tracked now.
    std::vector<Item> fresh;
    std::unordered_map<Key, Entity, KeyHash> present;
    for (usize r = 0; r < registrations_.size(); ++r) {
        const ComponentId component = registrations_[r].component;
        std::vector<Entity> entities;
        world_.ForEachArchetype([&](const Archetype& archetype) {
            if (!archetype.Mask().test(component)) {
                return;
            }
            for (usize c = 0; c < archetype.ChunkCount(); ++c) {
                const Entity* list = archetype.EntityArray(c);
                entities.insert(entities.end(), list, list + archetype.ChunkEntityCount(c));
            }
        });
        for (Entity entity : entities) {
            const Key key{EnsureGuid(world_, entity, &guids_), r}; // may move storage, not the handle
            present[key] = entity;
            if (tracked_.count(key) == 0) {
                fresh.push_back({key, entity, 0});
            }
        }
    }

    // 2. Components removed (or entities destroyed behind our back).
    std::vector<Item> gone;
    for (const auto& [key, state] : tracked_) {
        if (present.count(key) == 0) {
            gone.push_back({key, state.entity, world_.IsAlive(state.entity) ? Depth(state.entity) : 0});
        }
    }
    std::sort(gone.begin(), gone.end(), [](const Item& a, const Item& b) { return a.depth > b.depth; });
    for (const Item& item : gone) {
        const State state = tracked_[item.key];
        tracked_.erase(item.key);
        if (world_.IsAlive(state.entity)) {
            Remove(item.key, state); // the component was removed: it's destroyed
        }
    }

    // 3. New ones: create, enable, start, parents first.
    for (Item& item : fresh) {
        item.depth = Depth(item.entity);
    }
    std::sort(fresh.begin(), fresh.end(), [](const Item& a, const Item& b) {
        if (a.depth != b.depth) {
            return a.depth < b.depth;
        }
        return a.entity.index != b.entity.index ? a.entity.index < b.entity.index
                                                : a.key.registration < b.key.registration;
    });
    for (const Item& item : fresh) {
        tracked_[item.key] = {item.entity, false};
        if (const auto& on_create = registrations_[item.key.registration].callbacks.on_create) {
            on_create(item.entity);
        }
    }

    // 4. Enabled state (new entries included), then OnStart for the new.
    std::vector<Item> ordered = Ordered(/*parents_first=*/true);
    std::vector<Item> disabling;
    for (const Item& item : ordered) {
        State& state = tracked_[item.key];
        const bool active = IsActiveInHierarchy(world_, guids_, item.entity);
        if (active && !state.enabled) {
            state.enabled = true;
            if (const auto& on_enable = registrations_[item.key.registration].callbacks.on_enable) {
                on_enable(item.entity);
            }
        } else if (!active && state.enabled) {
            disabling.push_back(item);
        }
    }
    for (auto it = disabling.rbegin(); it != disabling.rend(); ++it) { // children first
        tracked_[it->key].enabled = false;
        if (const auto& on_disable = registrations_[it->key.registration].callbacks.on_disable) {
            on_disable(it->entity);
        }
    }
    for (const Item& item : fresh) {
        if (world_.IsAlive(item.entity) && tracked_.count(item.key) != 0) {
            if (const auto& on_start = registrations_[item.key.registration].callbacks.on_start) {
                on_start(item.entity);
            }
        }
    }
}

void Lifecycle::BeginPlay() {
    playing_ = true;
    in_pass_ = true;
    Sync();
    FinishPass();
}

void Lifecycle::EndPlay() {
    in_pass_ = true;
    for (const Item& item : Ordered(/*parents_first=*/false)) {
        auto it = tracked_.find(item.key);
        if (it != tracked_.end()) {
            const State state = it->second;
            tracked_.erase(it);
            Remove(item.key, state);
        }
    }
    tracked_.clear();
    in_pass_ = false;
    pending_destroy_.clear();
    pending_active_.clear();
    playing_ = false;
}

void Lifecycle::RunUpdates(std::function<void(Entity, f32)> LifecycleCallbacks::*member, f32 dt) {
    in_pass_ = true;
    Sync();
    const std::vector<Item> ordered = Ordered(/*parents_first=*/true);
    for (usize r = 0; r < registrations_.size(); ++r) {
        const auto& callback = registrations_[r].callbacks.*member;
        if (!callback) {
            continue;
        }
        for (const Item& item : ordered) {
            if (item.key.registration != r) {
                continue;
            }
            auto it = tracked_.find(item.key);
            if (it != tracked_.end() && it->second.enabled && world_.IsAlive(item.entity)) {
                callback(item.entity, dt);
            }
        }
    }
    FinishPass();
}

void Lifecycle::Update(f32 dt) { RunUpdates(&LifecycleCallbacks::on_update, dt); }
void Lifecycle::FixedUpdate(f32 dt) { RunUpdates(&LifecycleCallbacks::on_fixed_update, dt); }
void Lifecycle::LateUpdate(f32 dt) { RunUpdates(&LifecycleCallbacks::on_late_update, dt); }

void Lifecycle::FinishPass() {
    // Callbacks may queue more while these run; keep going until quiet.
    for (int round = 0; round < 16 && (!pending_active_.empty() || !pending_destroy_.empty()); ++round) {
        std::vector<std::pair<EntityGuid, bool>> active = std::move(pending_active_);
        pending_active_.clear();
        for (const auto& [guid, value] : active) {
            const Entity entity = guids_.Find(world_, guid);
            if (!entity.IsNull()) {
                world_.AddComponent<Active>(entity, Active{value});
            }
        }
        std::vector<EntityGuid> destroy = std::move(pending_destroy_);
        pending_destroy_.clear();
        for (const EntityGuid& guid : destroy) {
            const Entity entity = guids_.Find(world_, guid);
            if (!entity.IsNull()) {
                DestroyNow(entity);
            }
        }
        Sync(); // enable/disable changes, components gone with destroyed entities
    }
    in_pass_ = false;
}

void Lifecycle::DestroyNow(Entity root) {
    // The entity and its descendants, children first.
    std::vector<std::pair<u32, Entity>> doomed;
    std::vector<std::pair<u32, Entity>> stack{{0, root}};
    while (!stack.empty()) {
        const auto [depth, entity] = stack.back();
        stack.pop_back();
        doomed.push_back({depth, entity});
        for (Entity child : ChildrenOf(world_, guids_, entity)) {
            stack.push_back({depth + 1, child});
        }
    }
    std::stable_sort(doomed.begin(), doomed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [depth, entity] : doomed) {
        (void)depth;
        if (!world_.IsAlive(entity)) {
            continue;
        }
        const IdComponent* id = world_.GetComponent<IdComponent>(entity);
        if (id != nullptr) {
            const EntityGuid guid = id->guid;
            for (usize r = 0; r < registrations_.size(); ++r) {
                auto it = tracked_.find({guid, r});
                if (it != tracked_.end()) {
                    const State state = it->second;
                    tracked_.erase(it);
                    Remove({guid, r}, state);
                }
            }
            guids_.Remove(guid);
        }
        if (world_.IsAlive(entity)) {
            world_.DestroyEntity(entity);
        }
    }
}

void Lifecycle::Destroy(Entity entity) {
    if (!world_.IsAlive(entity)) {
        return;
    }
    if (in_pass_) {
        pending_destroy_.push_back(EnsureGuid(world_, entity, &guids_));
        return;
    }
    DestroyNow(entity);
    if (playing_) {
        Sync();
    }
}

void Lifecycle::SetActive(Entity entity, bool active) {
    if (!world_.IsAlive(entity)) {
        return;
    }
    if (in_pass_) {
        pending_active_.push_back({EnsureGuid(world_, entity, &guids_), active});
        return;
    }
    world_.AddComponent<Active>(entity, Active{active});
    if (playing_) {
        in_pass_ = true;
        Sync();
        FinishPass();
    }
}

} // namespace aether
