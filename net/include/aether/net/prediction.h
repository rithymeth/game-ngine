#pragma once

#include "aether/math/vec.h"
#include "aether/net/endpoint.h"

#include <deque>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

namespace aether::net {

// Client-side prediction and server reconciliation for character movement
// (Phase 22 step 4). The client applies its own input at once (no waiting for
// the server), sends numbered inputs unreliably (each message repeats the
// latest few, so a lost packet doesn't lose an input), and the server runs
// every input through the same movement step and reports the state after the
// last one it processed. When the server's state differs from what the client
// predicted at that input, the client takes the server's state and replays
// its still-unacknowledged inputs on top; the visible position eases to the
// corrected one instead of jumping.
//
// The movement step is pluggable (MoveStepFn) but must be deterministic: the
// same state and input give the same result on both sides. StepMovement is a
// kinematic default (walking, acceleration, gravity, jumping over a ground
// function). A move that needs Jolt's CharacterVirtual can't be rewound, so
// use a step of this shape for networked players.
//
// The server advances a character only when an input arrives, one step per
// input, so the two sides step the same inputs in the same order.

inline constexpr u8 kInputMessage = 0xA2;
inline constexpr u8 kStateMessage = 0xA3;

struct MoveInput {
    f32 move_x = 0.0f; // world-space direction, length <= 1; quantized to 1/127 on the wire
    f32 move_z = 0.0f;
    bool jump = false;
    bool run = false;
};

struct MoveState {
    Vec3 position{0, 0, 0}; // the character's feet
    Vec3 velocity{0, 0, 0};
    bool grounded = true;
};

struct MoveSettings {
    f32 dt = 1.0f / 60.0f; // one input's duration
    f32 walk_speed = 4.0f;
    f32 run_speed = 7.0f;
    f32 acceleration = 20.0f;
    f32 braking = 25.0f;
    f32 air_acceleration = 5.0f;
    f32 air_control = 0.3f;
    f32 jump_velocity = 5.0f;
    f32 gravity = -9.81f;
};

using GroundFn = std::function<f32(f32 x, f32 z)>; // ground height; null = flat at y = 0
using MoveStepFn = std::function<void(MoveState&, const MoveInput&)>;

// One deterministic movement step.
void StepMovement(const MoveSettings& settings, const GroundFn& ground, MoveState& state, const MoveInput& input);
MoveStepFn MakeMoveStep(const MoveSettings& settings, GroundFn ground = {});

// Clamps the move vector to length 1 and quantizes it to what survives the
// wire; both sides must step the sanitized input.
MoveInput Sanitize(const MoveInput& input);

// --- client -----------------------------------------------------------------

struct PredictionConfig {
    usize redundancy = 4;          // inputs per message: the newest plus this many earlier unacknowledged ones
    usize max_history = 256;       // unacknowledged inputs kept
    f32 position_tolerance = 0.01f; // a smaller error than this is not corrected
    f32 snap_distance = 2.0f;      // a bigger correction than this jumps instead of easing
    f32 correction_rate = 10.0f;   // 1/s: how fast the visible error decays
};

struct PredictionStats {
    u64 inputs = 0;
    u64 reconciliations = 0; // server disagreed: took its state and replayed
    u64 snaps = 0;           // ... by more than snap_distance
    u64 replayed_inputs = 0;
    f32 last_error = 0.0f;   // the disagreement behind the latest reconciliation, metres
};

class PredictionClient {
public:
    PredictionClient(MoveStepFn step, const PredictionConfig& config = {});

    void Reset(const MoveState& state);

    // Applies this tick's input at once and returns the message to send to the
    // server (unreliable: it repeats the latest unacknowledged inputs).
    std::vector<u8> Predict(const MoveInput& input);

    // A kStateMessage from the server. False if it doesn't parse.
    bool OnStateMessage(std::span<const u8> message);
    void Reconcile(u32 acknowledged_sequence, const MoveState& server_state);

    // Eases the visible error out; call every frame.
    void Update(f32 dt);

    const MoveState& State() const { return state_; }   // predicted, uncorrected
    Vec3 VisualPosition() const { return state_.position + offset_; }
    u32 LastSequence() const { return next_sequence_ - 1; }
    u32 Acknowledged() const { return acknowledged_; }
    usize Unacknowledged() const { return history_.size(); }
    const PredictionStats& Stats() const { return stats_; }

private:
    struct Entry {
        u32 sequence = 0;
        MoveInput input;
        MoveState after;
    };
    MoveStepFn step_;
    PredictionConfig config_;
    MoveState state_;
    Vec3 offset_{0, 0, 0}; // visible position minus predicted position
    u32 next_sequence_ = 1;
    u32 acknowledged_ = 0;
    std::deque<Entry> history_;
    PredictionStats stats_;
};

// --- server -----------------------------------------------------------------

struct PredictionServerConfig {
    usize max_queued_inputs = 32;
    usize max_burst = 4; // inputs processed in one Tick: a stalled link's backlog can catch up, but no faster on average
};

struct PredictionServerStats {
    u64 processed = 0;
    u64 dropped = 0;   // stale, duplicate or over the queue limit
    u64 clamped = 0;   // inputs whose move vector was too long
    u64 malformed = 0;
};

class PredictionServer {
public:
    PredictionServer(MoveStepFn step, const PredictionServerConfig& config = {});

    void AddPeer(NetAddress peer, const MoveState& initial);
    void RemovePeer(NetAddress peer);

    // A kInputMessage from `peer`. False if it doesn't parse.
    bool OnInputMessage(NetAddress peer, std::span<const u8> message);

    // Runs the queued inputs (one per call, plus a bounded catch-up).
    void Tick();

    const MoveState* StateOf(NetAddress peer) const;
    u32 LastProcessed(NetAddress peer) const;
    usize Queued(NetAddress peer) const;
    // The kStateMessage for `peer`: the sequence last processed and the state after it.
    std::vector<u8> BuildStateMessage(NetAddress peer) const;
    // Moves a character outright (a teleport, a respawn); the client reconciles to it.
    void SetState(NetAddress peer, const MoveState& state);

    const PredictionServerStats& Stats() const { return stats_; }

private:
    struct Peer {
        MoveState state;
        u32 last_processed = 0;
        std::deque<std::pair<u32, MoveInput>> queue; // ascending sequence
        f32 credit = 1.0f;
    };
    MoveStepFn step_;
    PredictionServerConfig config_;
    std::unordered_map<NetAddress, Peer> peers_;
    PredictionServerStats stats_;
};

} // namespace aether::net
