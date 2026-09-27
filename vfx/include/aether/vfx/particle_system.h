#pragma once

#include "aether/ecs/world.h"
#include "aether/reflection/reflection.h"
#include "aether/scene/entity_guid.h"
#include "aether/vfx/render.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aether {

// A request from gameplay or Blueprints, applied by the next ParticleWorld::Update.
struct ParticleSystemCommand {
    enum class Kind : u8 { Activate, Deactivate, Restart, SetFloat, SetVector, SetColor, SetAsset };
    Kind kind = Kind::Activate;
    std::string name;
    Vec3 v;
    f32 f = 0.0f;
};

// Plays a particle system asset (.avfx) at an entity (Phase 19 step 4,
// docs/design/PHASE_SPECS.md §19.4). It follows the entity's world
// transform. The methods are Blueprint nodes; they queue commands.
struct ParticleSystem {
    std::string asset;
    bool auto_activate = true;       // start when the entity appears
    f32 time_scale = 1.0f;
    f32 cull_distance = 0.0f;        // not simulated or drawn farther from the camera (0: never)
    f32 lod_distance = 0.0f;         // beyond this, spawning is scaled by lod_spawn_scale (0: never)
    f32 lod_spawn_scale = 0.25f;
    bool destroy_when_finished = false; // the entity goes once everything has died out (one-shots)
    bool active = false;                          // set by the particle world; not saved
    std::vector<ParticleSystemCommand> commands;  // not saved

    void Activate();   // from the start, if it had finished or was never started
    void Deactivate(); // stops spawning; what's alive lives out its life
    void Restart();
    void SetFloatParameter(const std::string& name, f32 value);
    void SetVectorParameter(const std::string& name, const Vec3& value);
    void SetColorParameter(const std::string& name, const Vec3& rgb, f32 alpha);
    void SetAsset(const std::string& asset);
    bool IsActive() const { return active; }
};

// Blueprint function library: effects that aren't placed in the level.
// Each makes an entity with a ParticleSystem that removes itself when it
// has finished, and returns it (so its parameters can be set).
struct Particles {
    static Entity SpawnEmitterAtLocation(const std::string& asset, const Vec3& location, const Vec3& rotation_degrees);
    // Follows `target` (plus `offset` in its space); stops spawning, and goes when done, if the target goes.
    static Entity SpawnEmitterAttached(const std::string& asset, const Entity& target, const Vec3& offset);
};

void RegisterParticleComponents();

} // namespace aether

namespace aether::vfx {

struct ParticleSystemFinishedEvent {
    Entity entity;
    std::string asset;
};

// Runs a world's ParticleSystem components: an instance per component
// (reused from a pool per asset), moved with its entity, stepped, culled
// and scaled by distance from the camera, and drawn.
class ParticleWorld {
public:
    using AssetLookup = std::function<const ParticleSystemAsset*(const std::string& asset)>;
    // With `guids`, positions follow parents; without, each Transform is world space.
    ParticleWorld(World& world, AssetLookup assets, const GuidIndex* guids = nullptr);
    ~ParticleWorld();
    ParticleWorld(const ParticleWorld&) = delete;
    ParticleWorld& operator=(const ParticleWorld&) = delete;

    // `camera`: for culling and LOD (none: everything runs at full rate).
    void Update(f32 dt, const ParticleCamera* camera = nullptr);
    // What's shown (not culled), for the renderer. With `view_projection`,
    // systems whose particles are all off screen are left out too.
    void BuildRenderData(const ParticleCamera& camera, ParticleRenderData& out, const Mat4* view_projection = nullptr) const;

    // What the Particles library calls.
    Entity SpawnAtLocation(const std::string& asset, const Vec3& location, const Quaternion& rotation = {});
    Entity SpawnAttached(const std::string& asset, Entity target, const Vec3& offset = {});

    ParticleSystemInstance* InstanceOf(Entity entity);
    const ParticleSystemInstance* InstanceOf(Entity entity) const;
    void SetCollider(const ParticleCollider* collider);

    struct Stats {
        usize systems = 0;   // with an instance
        usize simulated = 0; // stepped last Update
        usize culled = 0;    // too far away
        usize lod = 0;       // spawning scaled down
        usize particles = 0;
        usize pooled = 0;    // instances waiting for reuse
        u64 reused = 0;      // instances taken from the pool so far
    };
    const Stats& GetStats() const { return stats_; }

    // Systems that finished (everything spawned has died, and it won't spawn more) in the last Update.
    const std::vector<ParticleSystemFinishedEvent>& Events() const { return events_; }
    const std::vector<std::string>& Problems() const { return problems_; }

    static ParticleWorld* Active();
    void MakeActive();
    // The Blueprint event finished systems are dispatched as (argument: the asset).
    static constexpr const char* kFinishedEvent = "Event.OnParticleSystemFinished";
    static constexpr usize kPoolPerAsset = 16;

private:
    struct Live {
        Entity entity;
        std::string asset;
        std::unique_ptr<ParticleSystemInstance> instance;
        bool culled = false;
        bool started = false; // has been active (so emptying out after a Deactivate ends it)
        u64 seen = 0;
    };
    struct Attachment {
        Entity self, target;
        Vec3 offset;
        bool detached = false;
    };
    bool WorldPose(Entity e, EmitterPose& out) const;
    std::unique_ptr<ParticleSystemInstance> Acquire(const std::string& asset, const ParticleSystemAsset& data, Entity e);
    void Release(Live& live);
    void Apply(Live& live, ParticleSystem& ps, bool& restart);
    void Report(const std::string& key, const std::string& message);

    World& world_;
    AssetLookup assets_;
    const GuidIndex* guids_;
    const ParticleCollider* collider_ = nullptr;
    std::map<u32, Live> live_; // by entity index
    std::map<u32, Attachment> attachments_;
    std::map<std::string, std::vector<std::unique_ptr<ParticleSystemInstance>>> pool_;
    std::vector<ParticleSystemFinishedEvent> events_;
    std::vector<std::string> problems_;
    std::map<std::string, bool> reported_;
    Stats stats_;
    u64 generation_ = 0;
};

} // namespace aether::vfx

AETHER_REFLECT(aether::ParticleSystem, 1,
    AETHER_FIELD(asset, Field_EditAnywhere, {.tooltip = "The particle system to play (.avfx)"}),
    AETHER_FIELD(auto_activate, Field_EditAnywhere, {.tooltip = "Start when the entity appears"}),
    AETHER_FIELD(time_scale, Field_EditAnywhere, {.range_min = 0.0, .range_max = 10.0}),
    AETHER_FIELD(cull_distance, Field_EditAnywhere, {.tooltip = "Not run or drawn farther from the camera (0: never)", .range_min = 0.0, .range_max = 100000.0, .units = "m"}),
    AETHER_FIELD(lod_distance, Field_EditAnywhere, {.tooltip = "Spawn less beyond this (0: never)", .range_min = 0.0, .range_max = 100000.0, .units = "m"}),
    AETHER_FIELD(lod_spawn_scale, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(destroy_when_finished, Field_EditAnywhere, {.tooltip = "Remove the entity once the effect is over"}),
    AETHER_METHOD(Activate, Fn_BlueprintCallable),
    AETHER_METHOD(Deactivate, Fn_BlueprintCallable),
    AETHER_METHOD(Restart, Fn_BlueprintCallable),
    AETHER_METHOD(SetFloatParameter, Fn_BlueprintCallable, {"name", "value"}),
    AETHER_METHOD(SetVectorParameter, Fn_BlueprintCallable, {"name", "value"}),
    AETHER_METHOD(SetColorParameter, Fn_BlueprintCallable, {"name", "rgb", "alpha"}),
    AETHER_METHOD(SetAsset, Fn_BlueprintCallable, {"asset"}),
    AETHER_METHOD(IsActive, Fn_BlueprintCallable | Fn_Pure)
)

AETHER_REFLECT(aether::Particles, 1,
    AETHER_METHOD(SpawnEmitterAtLocation, Fn_BlueprintCallable, {"asset", "location", "rotation_degrees"}),
    AETHER_METHOD(SpawnEmitterAttached, Fn_BlueprintCallable, {"asset", "target", "offset"})
)
