#pragma once

#include "net/net_play.h"

#include <string>
#include <vector>

namespace aether::editor {

// The profiler's views of a NetProfile (Phase 22 step 6): entities by the
// bytes they cost, and fields by the bytes they cost over all entities.
struct NetEntityRow {
    u32 net_id = 0;
    std::string archetype;
    u64 bytes = 0;
    f64 bytes_per_second = 0.0;
    u32 updates = 0, spawns = 0;
    std::vector<std::pair<std::string, u64>> components; // most expensive first
};
struct NetFieldRow {
    std::string component, field;
    u64 bytes = 0;
    u32 sends = 0;
    f64 bytes_per_second = 0.0;
};
std::vector<NetEntityRow> BuildEntityRows(const net::NetProfile& profile, f64 now);
std::vector<NetFieldRow> BuildFieldRows(const net::NetProfile& profile, f64 now);

// Starts and stops networked play, and shows how it's going: the mode and
// client count, the simulated network (changeable while playing), and per
// client the connection, round trip, loss, traffic, entity count and
// prediction corrections. Portable; draws into the current ImGui window.
class NetPlayPanel {
public:
    NetPlayPanel(NetPlaySession& session, const World& edited) : session_(session), edited_(edited) {}
    void Draw();
    bool Start(); // with `settings`
    void Stop() { session_.Stop(); }
    NetPlaySettings settings;
    const std::string& LastError() const { return error_; }

private:
    NetPlaySession& session_;
    const World& edited_;
    std::string error_;
};

// Where the server's bandwidth goes: totals and rate since Reset, entities
// by cost (with their components and fields), and fields by cost.
class NetProfilerPanel {
public:
    explicit NetProfilerPanel(NetPlaySession& session) : session_(session) {}
    void Draw();
    enum class View { Entities, Fields };
    View view = View::Entities;

private:
    NetPlaySession& session_;
};

} // namespace aether::editor
