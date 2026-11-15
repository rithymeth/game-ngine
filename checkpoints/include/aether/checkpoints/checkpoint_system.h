#pragma once

#include "aether/checkpoints/checkpoint.h"
#include "aether/ecs/entity.h"
#include "aether/ecs/world.h"
#include "aether/save/save_bag.h"

#include <functional>
#include <string>
#include <vector>

namespace aether::chk {

enum class Result : u8 { Ok, DeadEntity, NoCheckpoint, NotInScene, NoTransform };
// "ok", "dead_entity", "no_checkpoint" (none has been reached), "not_in_scene" (the last checkpoint is in another scene), "no_transform"
const char* ResultName(Result result);

// The last checkpoint was changed: `checkpoint` is the entity that marks it, `activator` the one that reached it (null when a script made it).
struct CheckpointEvent {
    Entity checkpoint;
    Entity activator;
    std::string name;
    bool first_time = false; // it had never been reached before
};

// Notices entities reaching Checkpoint components, remembers the last one and puts an entity back at it. The state is in the game's save bag, as
//   "checkpoint.last"  (string)  the name of the last checkpoint reached
//   "checkpoint.scene" (string)  the scene it is in, when the host says (so a script can load it)
//   "checkpoint.<name>" (int 1)  each one that has been reached
// so it is written and read with the save slot, and a scene change does not touch it. Deterministic and event-driven: a checkpoint is reached on the
// Update in which an activator is within its radius while it is not already the last one.
class CheckpointSystem {
public:
    CheckpointSystem(World& world, save::SaveBag& bag, std::function<std::string()> current_scene = {});
    ~CheckpointSystem();
    CheckpointSystem(const CheckpointSystem&) = delete;
    CheckpointSystem& operator=(const CheckpointSystem&) = delete;

    static CheckpointSystem* Active();
    void MakeActive();

    void Update();

    // The name of the last checkpoint reached ("" when none), the scene it is in ("" when the host did not say), and whether `name` was ever reached.
    std::string Last() const;
    std::string LastScene() const;
    bool IsReached(const std::string& name) const;
    // The entity that marks `name` in this scene (null when there is none; the first by entity order when two share a name).
    Entity Find(const std::string& name) const;
    // Makes `name` the last checkpoint at once, as if the player had reached it. False for a name no checkpoint of this scene has.
    bool Activate(const std::string& name);
    // Puts `who` at the last checkpoint: its position becomes the checkpoint's plus the checkpoint's respawn_offset, and its rotation the checkpoint's.
    Result Respawn(Entity who);

    const std::vector<CheckpointEvent>& Events() const { return events_; }
    void ClearEvents() { events_.clear(); }

private:
    std::vector<Entity> Checkpoints() const;
    void Reach(Entity checkpoint, const Checkpoint& c, Entity activator);

    World& world_;
    save::SaveBag& bag_;
    std::function<std::string()> scene_;
    std::vector<CheckpointEvent> events_;
};

} // namespace aether::chk
