#pragma once

#include "aether/debug/stats.h"
#include "aether/net/host.h"

#include <string>

namespace aether::net {

// Shows a host's connections as a stat overlay group (`stat net`, Phase 23
// step 3) while it exists: the peer count, and per peer its address, round
// trip, loss and traffic each way.
class NetStatGroup {
public:
    explicit NetStatGroup(const NetHost& host, std::string name = "net");
    ~NetStatGroup();
    NetStatGroup(const NetStatGroup&) = delete;
    NetStatGroup& operator=(const NetStatGroup&) = delete;
    std::vector<StatLine> Lines() const;

private:
    const NetHost& host_;
    std::string name_;
};

} // namespace aether::net
