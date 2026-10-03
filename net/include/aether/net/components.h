#pragma once

#include "aether/ecs/component.h"
#include "aether/net/transport.h"
#include "aether/reflection/reflection.h"

namespace aether {

// Marks an entity as replicated (Phase 22): the server gives it a network id
// and sends it, with every component that has Field_Replicated fields, to
// the clients it is relevant to. `owner` is the NetAddress of the client that
// controls it (0 = the server). The client's copy carries the same values.
struct NetIdentity {
    u32 net_id = 0; // 0 = not assigned yet; the ReplicationServer assigns one
    u32 owner = 0;
};

} // namespace aether

AETHER_REFLECT(aether::NetIdentity, 1,
    AETHER_FIELD(net_id, Field_ReadOnly | Field_Replicated, {.tooltip = "Assigned by the server"}),
    AETHER_FIELD(owner, Field_EditAnywhere | Field_Replicated, {.tooltip = "Address of the owning client; 0 is the server"})
)
