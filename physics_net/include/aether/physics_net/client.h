#pragma once

#include "aether/net/host.h"
#include "aether/physics/physics_scene.h"
#include "aether/physics_net/protocol.h"

#include <map>

namespace aether::physics_net {

struct PhysicsNetClientStats {
    u32 chunks_received = 0, chunks_applied = 0, chunks_stale = 0;
    u32 grabs_granted = 0, grabs_denied = 0, grabs_released_by_host = 0;
    u32 malformed = 0;
};

enum class ClientGrabState : u8 { None, Pending, Held, Denied };

// The client's side of physics over the network (Priority 2): it applies the host's constraint snapshots to its own copy of the scene (the
// host is authoritative; a snapshot teleports the bodies it names and sets the joints' live state, see PhysicsScene::ApplyConstraintState), and
// it asks the host for grabs. It never creates a grab: Request sends the ask, the answer arrives as a GrabResult, and the held body then moves
// because the host's snapshots say so. A chunk of an older snapshot than one already applied is dropped, so a late or duplicated datagram
// can't move the scene backwards.
class PhysicsNetClient {
public:
    PhysicsNetClient(PhysicsScene& scene, net::NetHost& host, net::PeerId server) : scene_(scene), host_(host), server_(server) {}

    // Asks to hold `object` at `point` (world space). Returns the request id (it comes back in the result).
    u16 RequestGrab(const EntityGuid& object, const Vec3& point);
    // Where the held object should be pulled to (latest wins; call every frame while holding). False when nothing is held there.
    bool SetTarget(const EntityGuid& object, const Vec3& target);
    // Lets go. False when nothing was held or asked for there.
    bool Release(const EntityGuid& object);

    // Handles a message of ours (true).
    bool HandleEvent(const net::NetEvent& event);

    ClientGrabState StateOf(const EntityGuid& object) const;
    // The status of the last answer about this object (Granted until one says otherwise).
    GrabStatus LastStatus(const EntityGuid& object) const;
    const PhysicsNetClientStats& Stats() const { return stats_; }

private:
    struct Entry {
        ClientGrabState state = ClientGrabState::None;
        GrabStatus status = GrabStatus::Granted;
        u16 request_id = 0;
    };
    PhysicsScene& scene_;
    net::NetHost& host_;
    net::PeerId server_;
    std::map<EntityGuid, Entry> grabs_;
    PhysicsNetClientStats stats_;
    u16 next_request_ = 1;
    u32 target_sequence_ = 0;
    u32 newest_snapshot_ = 0;
    bool have_snapshot_ = false;
};

} // namespace aether::physics_net
