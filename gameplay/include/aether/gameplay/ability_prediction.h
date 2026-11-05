#pragma once

// Phase 41.6: the client half of predicted abilities, without the transport.
//
// The player presses a button; the client runs the ability at once so the effect is not a
// round-trip late, and remembers the activation. When the server answers:
//   * accepted -- commit it, which applies the cost and cooldown (`AbilitySystem::Commit` already
//     exists for exactly this: activation and payment are two separate steps);
//   * refused  -- cancel it (`Cancel` releases everything activation added), so the world looks
//     as though the press never happened.
//
// This class is deliberately not part of the message layer: the reconciliation is the part that is
// hard to get right, and keeping the transport out of it means it can be tested without a socket.
//
// The abilities it drives must be authored with `commit_on_activate = false`. `TryActivate` then
// runs and tags the ability without applying its cost or cooldown, which is exactly the state a
// prediction needs: the player sees it happen, and nothing has been paid that would have to be
// refunded if the server refuses. With the default (`true`) the cost is taken at activation, so
// there is nothing left for `Confirm` to do -- `Commit` returns true immediately and the mana is
// already gone, which is why an ability that should be predicted must opt out of committing.

#include "aether/gameplay/ability_system.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aether::gas {

class AbilityPredictor {
public:
    explicit AbilityPredictor(AbilitySystem& abilities);

    // The number of an activation the server has not answered yet.
    using Sequence = u32;

    struct Prediction {
        Sequence sequence = 0;
        AbilityHandle handle = 0;
        FailReason reason = FailReason::None; // None when handle != 0
    };

    // Runs `ability` for `owner` immediately and records it as unconfirmed. A refused activation
    // (unknown, on cooldown, cannot afford) is reported and nothing is recorded: there is nothing
    // for the server to confirm.
    Prediction Predict(Entity owner, const std::string& ability);

    // The server accepted `sequence`: commit it so the cost and cooldown apply. False when the
    // sequence is unknown or has already been answered -- a duplicate reply is not an error, but
    // it must not pay twice.
    bool Confirm(Sequence sequence);
    // The server refused `sequence`: cancel it and forget it. False when it was already answered.
    bool Reject(Sequence sequence);

    // Activations still waiting for an answer.
    usize Pending() const { return pending_.size(); }

    struct Stats {
        u32 predicted = 0, local_failures = 0, confirmed = 0, rejected = 0, stale_replies = 0;
        u32 commit_failed = 0; // confirmed by the server, but the cost could no longer be paid
    };
    const Stats& GetStats() const { return stats_; }

    // Forgets every unconfirmed activation without touching the world (the owner died, or the
    // connection went away). Returns how many were dropped.
    usize DropPending();

private:
    struct Entry {
        Sequence sequence;
        AbilityHandle handle;
    };

    AbilitySystem& abilities_;
    std::vector<Entry> pending_; // in activation order
    Sequence next_ = 1;
    Stats stats_;
};

} // namespace aether::gas
