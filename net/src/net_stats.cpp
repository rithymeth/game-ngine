#include "aether/net/net_stats.h"

#include <cstdio>

namespace aether::net {

NetStatGroup::NetStatGroup(const NetHost& host, std::string name) : host_(host), name_(std::move(name)) {
    StatGroups::Get().Register(name_, "Network connections: round trip, loss and traffic", [this] { return Lines(); });
}

NetStatGroup::~NetStatGroup() { StatGroups::Get().Unregister(name_); }

std::vector<StatLine> NetStatGroup::Lines() const {
    std::vector<StatLine> out;
    const std::vector<PeerId> peers = host_.Peers();
    out.push_back({std::to_string(peers.size()) + (peers.size() == 1 ? " peer" : " peers"), kStatPlain});
    for (PeerId id : peers) {
        const std::optional<PeerInfo> info = host_.Peer(id);
        if (!info || !info->connection) continue;
        const ConnectionStats& s = info->connection->Stats();
        char buf[160];
        std::snprintf(buf, sizeof(buf), "#%u %s  %.0f ms  %.1f%% loss  up %.1f KB  down %.1f KB", id,
                      info->address.ToString().c_str(), s.rtt * 1000.0, s.packet_loss * 100.0f,
                      static_cast<double>(s.bytes_sent) / 1024.0, static_cast<double>(s.bytes_received) / 1024.0);
        const u32 color = s.rtt > 0.2 || s.packet_loss > 0.1f ? kStatBad : s.rtt > 0.1 || s.packet_loss > 0.02f ? kStatWarn : kStatGood;
        out.push_back({buf, color});
    }
    return out;
}

} // namespace aether::net
