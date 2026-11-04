#pragma once

#include "aether/physics/constraints.h"
#include "aether/physics_net/protocol.h"

namespace aether::physics_net {

// What the host allows a client to do with grabs. A client never creates a grab itself: it asks, and the host checks these rules against its own
// world (the client's claims about where it stands or what it can reach are never trusted).
struct GrabPolicy {
    f32 reach = 3.0f;                 // m, from the holder's position to the grab point
    f32 max_object_offset = 3.0f;     // m, from the object's position to the grab point (the point must be on the object)
    f32 max_target_distance = 4.0f;   // m, from the holder: a target further than this is pulled in to it
    u32 max_holders_per_object = 2;
    u32 max_grabs_per_peer = 1;       // a player holds one thing at a time
    f64 requests_per_second = 4.0;    // grab requests per peer (a token bucket)
    f64 request_burst = 6.0;
    f64 target_timeout = 1.0;         // s: a grab whose target has not been updated for this long is released (a stalled or vanished client)
    f64 first_target_grace = 3.0;     // s: after a grab is granted, the wait for the client's first target. The client can't send one until the
                                      // (reliable) answer reaches it, which on a lossy link can take a while; a quiet link after that is target_timeout.
    GrabSettings settings;            // the spring every grab gets
};

// The facts the host gathers about one request, so the rules are one pure function.
struct GrabFacts {
    bool player_known = false;
    bool object_known = false;   // the entity exists and has a dynamic body
    bool grabbable = true;       // the game's filter
    bool rate_ok = true;         // the peer's token bucket had a token
    bool already_holding_object = false;
    u32 peer_grabs = 0;          // grabs this peer holds now
    u32 object_holders = 0;      // grabs on the object now (all peers)
    Vec3 holder_position{0, 0, 0};
    Vec3 object_position{0, 0, 0};
    Vec3 point{0, 0, 0};
};

// The first rule a request breaks, or Granted. Checked in this order: player, rate, object, filter, already holding, reach, point on the
// object, holders.
GrabStatus ValidateGrab(const GrabPolicy& policy, const GrabFacts& facts);

// A target pulled in to within max_target_distance of the holder (a straight line from the holder); a target already inside is unchanged.
Vec3 ClampGrabTarget(const GrabPolicy& policy, const Vec3& holder, const Vec3& target);

// A token bucket: `burst` tokens at most, refilled at `per_second`. Time is passed in (seconds) so it is repeatable.
struct TokenBucket {
    f64 tokens = -1.0; // negative: not started (the first Take starts it full)
    f64 last = 0.0;
    bool Take(f64 now, f64 per_second, f64 burst);
};

} // namespace aether::physics_net
