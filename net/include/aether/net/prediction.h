#pragma once

#include "aether/net/replication.h"
#include "aether/scene/components.h"

#include <deque>
#include <functional>

namespace aether::net {

// A networked character's movement state and tuning (Phase 22 step 4).
// Velocity and grounded replicate (with the Transform), so other clients
// see them too; the owner predicts them.
struct NetMovement {
    Vec3 velocity;
    bool grounded = true;
    f32 speed = 6.0f;        // m/s at full input
    f32 acceleration = 40.0f; // m/s^2 towards the input's velocity
    f32 jump_speed = 6.0f;
    f32 gravity = -20.0f;
    f32 ground_height = 0.0f;
};

// One frame of the owner's input. dt is part of it, so the server replays
// exactly the frames the client simulated.
struct MovementInput {
    u32 sequence = 0;
    f32 dt = 0.0f;
    f32 move_x = 0.0f, move_z = 0.0f; // world-space direction; length at most 1
    bool jump = false;
};

// The movement rule both sides run. Deterministic: the same state and input
// give the same result on every machine with the same build.
void StepMovement(Transform& transform, NetMovement& movement, const MovementInput& input);
// What the server accepts: dt within (0, max_dt], the direction at most
// unit length, and no NaNs.
MovementInput SanitizeInput(MovementInput input, f32 max_dt = 0.1f);

using MovementStep = std::function<void(World&, Entity, Transform&, NetMovement&, const MovementInput&)>;

struct PredictionConfig {
    u8 input_channel = 1;    // Unreliable: each packet repeats the unacknowledged inputs
    u8 ack_channel = 1;
    usize redundancy = 8;    // inputs per packet
    usize max_pending = 256; // inputs kept for replay
    f32 max_dt = 0.1f;
    f32 snap_distance = 0.01f; // a correction smaller than this is not counted
    usize max_inputs_per_update = 32; // per entity per server update (no speed hacks by flooding)
};

// The server side of predicted movement: runs each owned character's inputs
// in order as they arrive (from its owner only, sanitized, each sequence
// once), and tells the owner after each batch which input it reached and
// the state it got.
class MovementServer {
public:
    MovementServer(World& world, NetHost& host, ReplicationServer& replication, PredictionConfig config = {});
    void SetStep(MovementStep step) { step_ = std::move(step); }

    bool HandleEvent(const NetEvent& event);
    void Update(f64 now);

    u32 LastProcessed(u32 net_id) const;
    struct Stats {
        u64 inputs_run = 0, duplicates = 0, rejected = 0, malformed = 0, acks_sent = 0;
    };
    const Stats& GetStats() const { return stats_; }

private:
    struct Character {
        u32 last = 0;
        std::deque<MovementInput> queue;
        bool dirty = false;
    };
    World& world_;
    NetHost& host_;
    ReplicationServer& replication_;
    PredictionConfig config_;
    MovementStep step_;
    std::unordered_map<u32, Character> characters_;
    Stats stats_;
};

// The owner's side: Predict runs the movement at once and sends the input;
// each acknowledgement resets the character to the server's state and
// replays the inputs the server hasn't reached yet (reconciliation). With
// a deterministic step and no interference, the replay lands where the
// prediction was, so nothing visibly moves.
class MovementClient {
public:
    MovementClient(World& world, NetHost& host, ReplicationClient& replication, PeerId server,
                   PredictionConfig config = {});
    void SetStep(MovementStep step) { step_ = std::move(step); }

    // Moves `entity` (one this client owns) for one frame and sends the input.
    bool Predict(Entity entity, f32 move_x, f32 move_z, bool jump, f32 dt);
    bool HandleEvent(const NetEvent& event);

    usize Pending(u32 net_id) const;
    struct Stats {
        u32 predicted = 0, acks = 0, corrections = 0;
        f32 last_error = 0.0f, max_error = 0.0f;
    };
    const Stats& GetStats() const { return stats_; }

private:
    struct Character {
        u32 next = 1;
        u32 acked = 0;
        std::deque<MovementInput> pending;
    };
    World& world_;
    NetHost& host_;
    ReplicationClient& replication_;
    PeerId server_;
    PredictionConfig config_;
    MovementStep step_;
    std::unordered_map<u32, Character> characters_;
    Stats stats_;
};

} // namespace aether::net

AETHER_REFLECT(aether::net::NetMovement, 1,
    AETHER_FIELD(velocity, Field_Replicated),
    AETHER_FIELD(grounded, Field_Replicated),
    AETHER_FIELD(speed, Field_EditAnywhere),
    AETHER_FIELD(acceleration, Field_EditAnywhere),
    AETHER_FIELD(jump_speed, Field_EditAnywhere),
    AETHER_FIELD(gravity, Field_EditAnywhere),
    AETHER_FIELD(ground_height, Field_EditAnywhere)
)
