#pragma once

#include "aether/math/math.h"
#include "aether/physics/constraints.h"
#include "aether/scene/entity_guid.h"

#include <optional>
#include <span>
#include <vector>

namespace aether::physics_net {

// The wire format of physics over the network (Priority 2). One byte says which message it is; all numbers are little-endian. Snapshots and
// grab targets are state ("the latest wins") and travel unreliably; grab requests, results and releases are events and travel reliably.
//
//   Snapshot chunk   0x50 u32 sequence, u16 index, u16 count, then a ConstraintSnapshot (physics/constraints.h) of a share of the records
//   Grab request     0x51 u16 request id, guid object, f32[3] point   (the world point on the object the client wants to hold)
//   Grab target      0x52 u32 sequence, guid object, f32[3] target
//   Grab release     0x53 guid object
//   Grab result      0x54 u16 request id, u8 status, guid object       (host to the requester)
//
// A guid is u64 hi, u64 lo. Decoding rejects anything malformed: wrong type byte, wrong length, non-finite numbers, an unknown status.

constexpr u8 kReliableChannel = 0;   // ReliableOrdered in the default ConnectionConfig
constexpr u8 kUnreliableChannel = 1; // Unreliable: snapshot chunks (at most one packet's worth each)
constexpr u8 kSequencedChannel = 2;  // UnreliableSequenced: grab targets (never older than one already delivered)

enum class MessageType : u8 { SnapshotChunk = 0x50, GrabRequest = 0x51, GrabTarget = 0x52, GrabRelease = 0x53, GrabResult = 0x54 };

enum class GrabStatus : u8 {
    Granted,
    UnknownPlayer,   // the peer has no pawn here
    UnknownObject,   // no such entity, or it has no dynamic body
    NotGrabbable,    // the game's filter said no
    TooFar,          // the grab point is out of the holder's reach, or not on the object
    TooManyHolders,  // the object already has its maximum holders
    AlreadyHolding,  // the peer is holding as much as it may
    RateLimited,     // too many requests
    Released,        // sent later by the host: the grab ended (timeout, break, the object went away)
};
const char* GrabStatusName(GrabStatus status);

struct SnapshotChunk {
    u32 sequence = 0; // one per snapshot; every chunk of it carries the same
    u16 index = 0, count = 1;
    ConstraintSnapshot snapshot; // a share of the records
};
struct GrabRequest {
    u16 request_id = 0;
    EntityGuid object;
    Vec3 point{0, 0, 0};
};
struct GrabTarget {
    u32 sequence = 0;
    EntityGuid object;
    Vec3 target{0, 0, 0};
};
struct GrabRelease {
    EntityGuid object;
};
struct GrabResult {
    u16 request_id = 0;
    GrabStatus status = GrabStatus::Granted;
    EntityGuid object;
};

// The message type of a datagram payload, or nullopt for one that isn't ours.
std::optional<MessageType> PeekMessageType(std::span<const u8> data);

std::vector<u8> Encode(const GrabRequest& message);
std::vector<u8> Encode(const GrabTarget& message);
std::vector<u8> Encode(const GrabRelease& message);
std::vector<u8> Encode(const GrabResult& message);
std::vector<u8> Encode(const SnapshotChunk& message);
[[nodiscard]] bool Decode(std::span<const u8> data, GrabRequest& out);
[[nodiscard]] bool Decode(std::span<const u8> data, GrabTarget& out);
[[nodiscard]] bool Decode(std::span<const u8> data, GrabRelease& out);
[[nodiscard]] bool Decode(std::span<const u8> data, GrabResult& out);
[[nodiscard]] bool Decode(std::span<const u8> data, SnapshotChunk& out);

// A snapshot as chunks that each encode to at most `max_bytes` (an unreliable message holds 1024 at most), joints first, then bodies, in the
// snapshot's order. Records are independent, so a chunk applies on its own and a lost chunk only delays those records. An empty snapshot is
// one empty chunk, so the sequence still advances.
std::vector<std::vector<u8>> EncodeSnapshotChunks(u32 sequence, const ConstraintSnapshot& snapshot, usize max_bytes = 1000);

} // namespace aether::physics_net
