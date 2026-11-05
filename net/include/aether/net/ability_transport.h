#pragma once

// Phase 41.6: the wire half of ability prediction.
//
// The gameplay side owns the semantics (gameplay/ability_prediction.h); this file only moves
// requests and replies. It cannot name AbilitySystem -- net and gameplay are sibling targets and the
// build forbids that include -- so the three operations arrive as callbacks, the way
// MovementServer::SetStep takes the movement rule instead of linking the code that owns it.
//
// The flow, mirroring predicted movement:
//   client: activate locally at once (predicted, uncommitted) and send {sequence, entity, ability}
//   server: activate its own copy, commit the cost if it worked, reply {sequence, accepted}
//   client: accepted -> commit the prediction; refused -> cancel it, as if the press never happened
//
// The server needs the world because it has to read NetIdentity: only the owner of an entity may ask
// for that entity's abilities, and ReplicationServer maps ids to entities but does not expose
// components.

#include "aether/ecs/world.h"
#include "aether/net/replication.h"

#include <functional>
#include <string>
#include <vector>

namespace aether::net {

// The three operations the wire layer needs, supplied by the gameplay side. activate returns a
// handle, or 0 when the ability could not run; the other two take a handle from it.
struct AbilityOps {
    std::function<u32(u32 net_id, const std::string& ability)> activate;
    std::function<bool(u32 handle)> commit;
    std::function<void(u32 handle)> cancel;
};

struct AbilityChannelConfig {
    u8 request_channel = 2; // reliable: a lost press must not silently vanish
    u8 reply_channel = 2;
    usize max_ability_name = 96; // longer names are malformed rather than truncated
};

class AbilityServer {
public:
    AbilityServer(World& world, NetHost& host, ReplicationServer& replication, AbilityOps ops,
                  AbilityChannelConfig config = {});

    bool HandleEvent(const NetEvent& event);

    struct Stats {
        u64 accepted = 0, refused = 0, malformed = 0, unknown_entity = 0, not_owner = 0;
    };
    const Stats& GetStats() const { return stats_; }

private:
    World& world_;
    NetHost& host_;
    ReplicationServer& replication_;
    AbilityOps ops_;
    AbilityChannelConfig config_;
    Stats stats_;
};

class AbilityClient {
public:
    AbilityClient(World& world, NetHost& host, ReplicationClient& replication, PeerId server, AbilityOps ops,
                  AbilityChannelConfig config = {});

    // Runs ability for entity (one this client owns) straight away and asks the server to agree.
    // False when the prediction could not run locally -- there is then nothing to send.
    bool Request(Entity entity, const std::string& ability);
    bool HandleEvent(const NetEvent& event);

    usize Pending() const { return pending_.size(); }

    struct Stats {
        u32 predicted = 0, local_failures = 0, accepted = 0, refused = 0, stale_replies = 0, malformed = 0;
    };
    const Stats& GetStats() const { return stats_; }

private:
    struct Entry {
        u32 sequence = 0;
        u32 handle = 0; // the local (predicted) handle, not the server's
    };

    World& world_;
    NetHost& host_;
    ReplicationClient& replication_;
    PeerId server_;
    AbilityOps ops_;
    AbilityChannelConfig config_;
    std::vector<Entry> pending_;
    u32 next_ = 1;
    Stats stats_;
};

} // namespace aether::net