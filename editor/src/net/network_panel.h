#pragma once

#include "net/net_play.h"

#include <string>
#include <vector>

namespace aether::editor {

// The Network panel (Phase 22 step 7): the simulated link (latency, jitter,
// loss, duplication, with presets), the connected clients (round trip, lost
// packets, traffic, entities) with disconnect/reconnect/add, the server's
// bandwidth over time, and a profiler that says where replication bandwidth
// goes - per replicated field and per entity. Draw() fills the current ImGui
// window; Sample() is called every frame to build the bandwidth history.
// Everything runs headless in tests.

struct LinkPreset {
    const char* name;
    net::LinkConditions conditions;
};
// None, LAN, Good broadband, Mobile, Poor, Terrible.
const std::vector<LinkPreset>& LinkPresets();

struct FieldCostRow {
    std::string component;
    std::string field;
    u64 bytes = 0;
    u64 sends = 0;
};
struct EntityCostRow {
    u32 net_id = 0;
    u64 bytes = 0;
};

class NetworkPanel {
public:
    explicit NetworkPanel(NetPlaySession& session) : session_(session) {}

    void Draw();
    // Call once per frame with the frame's time step; once a second it records the server's send rate.
    void Sample(f64 dt);

    // The `count` most expensive replicated fields / entities so far, costliest first.
    std::vector<FieldCostRow> TopFields(usize count) const;
    std::vector<EntityCostRow> TopEntities(usize count) const;

    // Sets the link to a preset (by index into LinkPresets) and returns false for a bad index.
    bool ApplyPreset(usize index);

    const std::vector<f32>& BandwidthHistory() const { return history_; } // bytes per second, oldest first
    f64 CurrentBandwidth() const { return history_.empty() ? 0.0 : history_.back(); }
    usize max_history = 60;

private:
    NetPlaySession& session_;
    std::vector<f32> history_;
    u64 last_total_ = 0;
    f64 window_ = 0.0;
    int preset_ = 0;
};

} // namespace aether::editor
