#pragma once

#include "aether/scene/entity_guid.h"
#include "core/command_stack.h"

#include <vector>

namespace aether::editor {

// Play-in-Editor (Phase 7 step 5).
//
// Play snapshots the edited world into memory (the binary scene format) and
// freezes the command stack; the game then runs in that same World. Stop
// throws the played world away and rebuilds the edited one from the
// snapshot, so everything that happened during play — physics, spawns,
// deletions, edits — is undone exactly, and the undo history is as it was.
//
// (The spec describes duplicating into a separate play world. Restoring in
// place gives the same result for the user without every system that holds a
// World& needing to switch worlds.)
//
// Hooks: on Stop, EditorHooks::on_entity_destroying fires for every entity
// of the played world before it's discarded, and on_entity_created for every
// restored entity, so state outside the ECS (Jolt bodies) follows along.
// Entities keep their GUIDs, so the editor can carry its selection across by
// GUID.
class PlaySession {
public:
    enum class State { Editing, Playing, Paused };

    State GetState() const { return state_; }
    // A fresh ID on every new session, including across documents. Zero while editing; pause/resume keeps the same ID.
    u64 Id() const { return state_ == State::Editing ? 0 : id_; }
    bool IsEditing() const { return state_ == State::Editing; }

    // Editing -> Playing (takes the snapshot), or Paused -> Playing. `simulate` only matters on the
    // editing-to-playing edge: Simulate runs the game with the editor's camera left alone (see
    // IsSimulating), Play lets the view's camera drive the game's.
    void Play(CommandContext& ctx, CommandStack& stack, bool simulate = false);
    // Playing -> Paused. No-op otherwise.
    void Pause();
    // While paused, lets exactly one simulation step run (see ShouldSimulate).
    void Step();
    // Playing/Paused -> Editing, restoring the snapshot. No-op when editing.
    void Stop(CommandContext& ctx, CommandStack& stack);
    // Whether the session was started by Simulate: the game runs, but the editor's view is not its camera --
    // the host leaves its own camera under the person's control and does not eject it on Stop.
    bool IsSimulating() const { return state_ != State::Editing && simulate_; }

    // Call once per frame: whether the game/physics should advance this frame.
    bool ShouldSimulate();

    usize SnapshotBytes() const { return snapshot_.size(); }

private:
    State state_ = State::Editing;
    u64 id_ = 0;
    bool step_pending_ = false;
    bool simulate_ = false; // this session was started by Simulate
    std::vector<u8> snapshot_;
};

} // namespace aether::editor
