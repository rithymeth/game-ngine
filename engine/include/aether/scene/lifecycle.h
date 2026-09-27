#pragma once

#include "aether/ecs/world.h"
#include "aether/scene/entity_guid.h"

#include <functional>
#include <unordered_map>
#include <vector>

namespace aether {

// Callbacks for one kind of component (a script, a gameplay component), run
// by Lifecycle for every entity that has it. Any may be empty.
struct LifecycleCallbacks {
    std::function<void(Entity)> on_create;  // the entity (with this component) entered play
    std::function<void(Entity)> on_enable;  // it became active in the hierarchy (also right after create, if active)
    std::function<void(Entity)> on_start;   // once, after every entity that entered play with it was created and enabled
    std::function<void(Entity, f32)> on_update;       // every frame, while enabled
    std::function<void(Entity, f32)> on_fixed_update; // every fixed step, while enabled
    std::function<void(Entity, f32)> on_late_update;  // after every entity's update, while enabled
    std::function<void(Entity)> on_disable; // it stopped being active (also just before destroy, if enabled)
    // It's about to be destroyed, or play ended; or the component was
    // removed (then this runs after it's gone, so don't read it).
    std::function<void(Entity)> on_destroy;
};

// Drives the lifecycle events of docs/ROADMAP.md Phase 9 (OnCreate, OnStart,
// OnUpdate, OnFixedUpdate, OnLateUpdate, OnDestroy, OnEnable, OnDisable)
// during play.
//
// - Order: create / enable / start run parents first (then by creation);
//   disable / destroy run children first. Updates run per registration,
//   parents first within one.
// - Entities spawned during play, and components added to existing
//   entities, are picked up at the end of the pass they appeared in (or the
//   start of the next one): create, enable, then start.
// - Destroy(), and SetActive() during a callback, take effect at the end of
//   the current pass, so callbacks never see half-destroyed state.
//   Entities destroyed directly with World::DestroyEntity get no OnDestroy.
// - Entities are tracked by EntityGuid (they get one if they had none).
class Lifecycle {
public:
    Lifecycle(World& world, GuidIndex& guids) : world_(world), guids_(guids) {}

    // Callbacks for entities with `component`, run in registration order.
    void Register(ComponentId component, LifecycleCallbacks callbacks);
    template <typename T>
    void Register(LifecycleCallbacks callbacks) {
        Register(GetComponentId<T>(), std::move(callbacks));
    }

    // Starts play: OnCreate, OnEnable, OnStart for everything in the world.
    void BeginPlay();
    // Ends play: OnDisable and OnDestroy for everything that's in play.
    void EndPlay();
    bool IsPlaying() const { return playing_; }
    GuidIndex& Guids() { return guids_; }

    void Update(f32 dt);
    void FixedUpdate(f32 dt);
    void LateUpdate(f32 dt);

    // Destroys the entity and everything under it (children first), with
    // OnDisable / OnDestroy. Deferred to the end of an update pass when
    // called from inside one.
    void Destroy(Entity entity);
    // Sets the entity's Active component, firing OnEnable / OnDisable on it
    // and its descendants as their active-in-hierarchy state changes.
    // Deferred like Destroy when called during an update pass.
    void SetActive(Entity entity, bool active);

    // How many (entity, registration) pairs are in play.
    usize TrackedCount() const { return tracked_.size(); }

private:
    struct Key {
        EntityGuid guid;
        usize registration;
        bool operator==(const Key& o) const { return guid == o.guid && registration == o.registration; }
    };
    struct KeyHash {
        usize operator()(const Key& k) const noexcept {
            return std::hash<EntityGuid>()(k.guid) ^ (k.registration * 0x9e3779b97f4a7c15ull);
        }
    };
    struct State {
        Entity entity;
        bool enabled = false;
    };
    struct Registration {
        ComponentId component;
        LifecycleCallbacks callbacks;
    };
    struct Item {
        Key key;
        Entity entity;
        u32 depth;
    };

    // Brings tracking in line with the world: new entries created / enabled /
    // started, removed components destroyed, activity changes enabled or
    // disabled.
    void Sync();
    void RunUpdates(std::function<void(Entity, f32)> LifecycleCallbacks::*member, f32 dt);
    void FinishPass();
    void DestroyNow(Entity entity);
    void Remove(const Key& key, const State& state);
    u32 Depth(Entity entity) const;
    std::vector<Item> Ordered(bool parents_first) const;

    World& world_;
    GuidIndex& guids_;
    std::vector<Registration> registrations_;
    std::unordered_map<Key, State, KeyHash> tracked_;
    bool playing_ = false;
    bool in_pass_ = false;
    std::vector<EntityGuid> pending_destroy_;
    std::vector<std::pair<EntityGuid, bool>> pending_active_;
};

} // namespace aether
