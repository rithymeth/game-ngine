#include "aether/scene/components.h"
#include "aether/scene/serialization.h"
#include "net/network_panel.h"
#include "test_framework.h"

#include <imgui.h>

#include <algorithm>
#include <functional>

// Phase 22 step 7: multi-client play and the Network panel - a server world
// and client worlds filling by replication, ownership, joining and leaving,
// the adjustable link, the bandwidth profiler and the panel drawn headless.

using namespace aether;
using namespace aether::editor;
using namespace aether::net;

struct NetEditorUnit {
    i32 health = 100;
    std::string label;
};

AETHER_REFLECT(NetEditorUnit, 1,
    AETHER_FIELD(health, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(label, Field_EditAnywhere | Field_Replicated)
)

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400, 900);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1400, 900));
        ImGui::Begin("Network", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

// A scene with `count` replicated units; returns the snapshot bytes.
std::vector<u8> UnitScene(int count) {
    RegisterReplicatedComponent<NetEditorUnit>();
    World w;
    for (int i = 0; i < count; ++i) {
        Transform t;
        t.position = Vec3(static_cast<f32>(i), 0, 0);
        NetEditorUnit unit;
        unit.label = "unit " + std::to_string(i);
        w.CreateEntity(NetIdentity{}, t, unit);
    }
    w.CreateEntity(Transform{}); // not replicated
    return SaveSceneToMemory(w);
}

void Run(NetPlaySession& s, f64 seconds, NetworkPanel* panel = nullptr) {
    const f64 dt = 1.0 / 60.0;
    for (f64 t = 0.0; t < seconds; t += dt) {
        s.Step(dt);
        if (panel != nullptr) panel->Sample(dt);
    }
}

std::vector<Entity> Units(NetPlaySession& s) {
    std::vector<Entity> out;
    s.Server().ForEach<NetIdentity, NetEditorUnit>([&](NetIdentity&, NetEditorUnit&) {});
    s.Server().ForEachArchetype([&](Archetype& a) {
        if (!a.Has(GetComponentId<NetEditorUnit>())) return;
        for (usize c = 0; c < a.ChunkCount(); ++c) {
            const Entity* e = a.EntityArray(c);
            out.insert(out.end(), e, e + a.ChunkEntityCount(c));
        }
    });
    std::sort(out.begin(), out.end(), [](Entity a, Entity b) { return a.index < b.index; });
    return out;
}

usize UnitsIn(World& w) {
    usize n = 0;
    w.ForEach<NetEditorUnit>([&](NetEditorUnit&) { ++n; });
    return n;
}

} // namespace

AETHER_TEST(NetPlay_ClientsFillByReplication) {
    NetPlayConfig cfg;
    cfg.clients = 2;
    NetPlaySession s(UnitScene(4), cfg);
    AETHER_CHECK(s.ClientCount() == 2 && UnitsIn(s.Server()) == 4);
    AETHER_CHECK(UnitsIn(s.Client(0)) == 0); // nothing before the network runs
    Run(s, 2.0);
    AETHER_CHECK(UnitsIn(s.Client(0)) == 4 && UnitsIn(s.Client(1)) == 4);
    AETHER_CHECK(s.Client(0).EntityCount() == 4); // the non-replicated entity stayed on the server
    AETHER_CHECK(s.Info(0).connected && s.Info(1).connected && s.Info(0).entities == 4);
    AETHER_CHECK(s.Info(0).address != s.Info(1).address && s.Info(9).address == 0 && !s.Info(9).connected);

    // A server-side change reaches both clients.
    s.Server().GetComponent<NetEditorUnit>(Units(s)[2])->health = 7;
    Run(s, 1.0);
    for (usize c = 0; c < 2; ++c) {
        bool found = false;
        s.Client(c).ForEach<NetEditorUnit>([&](NetEditorUnit& u) { found = found || (u.health == 7 && u.label == "unit 2"); });
        AETHER_CHECK(found);
    }
}

AETHER_TEST(NetPlay_MovementIsInterpolated) {
    NetPlayConfig cfg;
    cfg.clients = 1;
    cfg.conditions = LinkPresets()[2].conditions; // good broadband
    NetPlaySession s(UnitScene(1), cfg);
    Run(s, 2.0);
    const Entity unit = Units(s)[0];
    Entity client_unit = kNullEntity;
    s.Client(0).ForEach<NetEditorUnit>([&](NetEditorUnit&) {});
    const u32 id = s.Server().GetComponent<NetIdentity>(unit)->net_id;
    client_unit = s.Replication().EntityOf(id);
    AETHER_CHECK(!client_unit.IsNull());

    // The server moves the unit at 5 m/s; the client follows smoothly, a little behind.
    f32 last = 0.0f, biggest_step = 0.0f;
    bool started = false;
    f64 t = s.Time();
    for (int i = 0; i < 300; ++i) {
        t += 1.0 / 60.0;
        s.Server().GetComponent<Transform>(unit)->position = Vec3(static_cast<f32>(5.0 * (t - 2.0)), 0, 0);
        s.Step(1.0 / 60.0);
        Entity cu = kNullEntity;
        s.Client(0).ForEach<NetIdentity>([&](NetIdentity& n) { (void)n; });
        // The client's copy has the same net id.
        s.Client(0).ForEachArchetype([&](Archetype& a) {
            if (!a.Has(GetComponentId<NetEditorUnit>())) return;
            cu = a.EntityArray(0)[0];
        });
        const f32 x = s.Client(0).GetComponent<Transform>(cu)->position.x;
        if (started) biggest_step = std::max(biggest_step, std::fabs(x - last));
        if (i > 60) started = true;
        last = x;
    }
    AETHER_CHECK(biggest_step < 0.2f && last > 5.0f);
    AETHER_CHECK(s.Interpolator(0).Stats().snapshots > 50);
}

AETHER_TEST(NetPlay_OwnershipFollowsTheClient) {
    NetPlayConfig cfg;
    cfg.clients = 2;
    NetPlaySession s(UnitScene(2), cfg);
    Run(s, 1.0);
    const Entity unit = Units(s)[0];
    AETHER_CHECK(s.SetOwner(unit, 1) && s.Server().GetComponent<NetIdentity>(unit)->owner == s.Info(1).address);
    AETHER_CHECK(!s.SetOwner(unit, 5) && !s.SetOwner(kNullEntity, 0));
    Run(s, 1.0);
    // Every client sees who owns it.
    const u32 id = s.Server().GetComponent<NetIdentity>(unit)->net_id;
    for (usize c = 0; c < 2; ++c) {
        bool seen = false;
        s.Client(c).ForEach<NetIdentity>([&](NetIdentity& n) { seen = seen || (n.net_id == id && n.owner == s.Info(1).address); });
        AETHER_CHECK(seen);
    }
}

AETHER_TEST(NetPlay_ClientsLeaveJoinAndRejoin) {
    NetPlayConfig cfg;
    cfg.clients = 1;
    NetPlaySession s(UnitScene(3), cfg);
    Run(s, 1.5);
    AETHER_CHECK(UnitsIn(s.Client(0)) == 3 && s.ServerEndpoint().ConnectedPeers().size() == 1);

    s.DisconnectClient(0);
    Run(s, 0.5);
    AETHER_CHECK(!s.Info(0).connected && UnitsIn(s.Client(0)) == 0 && s.ServerEndpoint().ConnectedPeers().empty());

    s.ReconnectClient(0);
    Run(s, 1.5);
    AETHER_CHECK(s.Info(0).connected && UnitsIn(s.Client(0)) == 3);

    // A client that joins late gets everything, including later changes.
    s.Server().GetComponent<NetEditorUnit>(Units(s)[0])->health = 55;
    const usize late = s.AddClient();
    AETHER_CHECK(late == 1 && s.ClientCount() == 2);
    Run(s, 1.5);
    AETHER_CHECK(UnitsIn(s.Client(1)) == 3);
    bool changed = false;
    s.Client(1).ForEach<NetEditorUnit>([&](NetEditorUnit& u) { changed = changed || u.health == 55; });
    AETHER_CHECK(changed);
    s.DisconnectClient(99); // out of range: ignored
    s.ReconnectClient(99);
}

AETHER_TEST(NetPlay_LinkConditionsAreAdjustableLive) {
    NetPlayConfig cfg;
    cfg.clients = 1;
    NetPlaySession s(UnitScene(5), cfg);
    NetworkPanel panel(s);
    AETHER_CHECK(!panel.ApplyPreset(99));
    AETHER_CHECK(panel.ApplyPreset(5)); // terrible
    AETHER_CHECK(s.Conditions().latency == 0.25 && s.Conditions().loss == 0.15);
    // Even then, the world gets there and stays in step.
    Run(s, 12.0);
    AETHER_CHECK(UnitsIn(s.Client(0)) == 5 && s.Info(0).connected);
    s.Server().GetComponent<NetEditorUnit>(Units(s)[1])->health = 1;
    Run(s, 8.0);
    bool changed = false;
    s.Client(0).ForEach<NetEditorUnit>([&](NetEditorUnit& u) { changed = changed || u.health == 1; });
    AETHER_CHECK(changed);
    AETHER_CHECK(s.Info(0).rtt > 0.3);                // the round trip reflects the 250 ms hops
    AETHER_CHECK(s.Info(0).packets_lost > 0);

    // Presets are ordered from kind to cruel.
    const auto& presets = LinkPresets();
    AETHER_CHECK(presets.size() == 6);
    for (usize i = 1; i < presets.size(); ++i) AETHER_CHECK(presets[i].conditions.latency >= presets[i - 1].conditions.latency);
}

AETHER_TEST(NetPlay_ProfilerAttributesBandwidth) {
    NetPlayConfig cfg;
    cfg.clients = 2;
    NetPlaySession s(UnitScene(6), cfg);
    NetworkPanel panel(s);
    Run(s, 2.0);
    s.Replication().ResetProfile(); // measure just what follows
    AETHER_CHECK(panel.TopFields(5).empty() && panel.TopEntities(5).empty());

    const auto units = Units(s);
    for (int round = 0; round < 10; ++round) {
        for (usize i = 0; i < units.size(); ++i) s.Server().GetComponent<NetEditorUnit>(units[i])->health = round * 10 + static_cast<i32>(i);
        s.Server().GetComponent<NetEditorUnit>(units[0])->label = "renamed " + std::to_string(round) + " with a long suffix";
        Run(s, 0.2);
    }

    const auto fields = panel.TopFields(10);
    AETHER_CHECK(fields.size() == 2);
    AETHER_CHECK(fields[0].component == "NetEditorUnit" && fields[1].component == "NetEditorUnit");
    // The label (long, but changing on one unit) and the health (six units) both show; counts add up over two clients.
    const auto health = std::find_if(fields.begin(), fields.end(), [](const FieldCostRow& r) { return r.field == "health"; });
    const auto label = std::find_if(fields.begin(), fields.end(), [](const FieldCostRow& r) { return r.field == "label"; });
    AETHER_CHECK(health != fields.end() && label != fields.end());
    AETHER_CHECK(health->sends == 6 * 10 * 2); // health changed on six units in each of ten rounds, sent to two peers
    AETHER_CHECK(label->sends == 10 * 2);
    AETHER_CHECK(fields[0].bytes >= fields[1].bytes);

    // Field bytes are a part of the total; framing makes up the rest.
    u64 field_total = 0;
    for (const auto& f : fields) field_total += f.bytes;
    const ReplicationProfile& profile = s.Replication().Profile();
    AETHER_CHECK(field_total > 0 && field_total < profile.total_bytes);

    // Unit 0 changes the most (its label too): it costs the most.
    const auto entities = panel.TopEntities(3);
    AETHER_CHECK(entities.size() == 3 && entities[0].bytes >= entities[1].bytes && entities[1].bytes >= entities[2].bytes);
    AETHER_CHECK(entities[0].net_id == s.Server().GetComponent<NetIdentity>(units[0])->net_id);
    AETHER_CHECK(panel.TopFields(1).size() == 1 && panel.TopEntities(2).size() == 2);

    s.Replication().ResetProfile();
    AETHER_CHECK(panel.TopFields(5).empty() && s.Replication().Profile().total_bytes == 0);
}

AETHER_TEST(NetPlay_BandwidthHistoryIsSampledPerSecond) {
    NetPlayConfig cfg;
    cfg.clients = 1;
    NetPlaySession s(UnitScene(8), cfg);
    NetworkPanel panel(s);
    panel.max_history = 5;
    AETHER_CHECK(panel.BandwidthHistory().empty() && panel.CurrentBandwidth() == 0.0);
    Run(s, 3.05, &panel);
    AETHER_CHECK(panel.BandwidthHistory().size() == 3);
    AETHER_CHECK(*std::max_element(panel.BandwidthHistory().begin(), panel.BandwidthHistory().end()) > 0.0f);
    // Idle afterwards: the rate falls to keepalive traffic only, and the history stays bounded.
    const f32 busy = panel.BandwidthHistory()[0];
    Run(s, 8.0, &panel);
    AETHER_CHECK(panel.BandwidthHistory().size() == 5);
    AETHER_CHECK(panel.CurrentBandwidth() < busy);
}

AETHER_TEST(NetPlay_OtherMessagesReachTheCallbacks) {
    NetPlayConfig cfg;
    cfg.clients = 2;
    NetPlaySession s(UnitScene(1), cfg);
    Run(s, 1.0);
    std::vector<usize> client_hits;
    std::vector<NetAddress> server_hits;
    s.on_client_message = [&](usize c, const NetEvent& e) {
        if (e.type == NetEventType::Message) client_hits.push_back(c);
    };
    s.on_server_message = [&](NetAddress from, const NetEvent& e) {
        if (e.type == NetEventType::Message) server_hits.push_back(from);
    };
    const std::vector<u8> game_message = {0x01, 2, 3};
    s.ServerEndpoint().Send(s.Info(1).address, Channel::ReliableOrdered, game_message);
    s.ClientEndpoint(0).Send(s.ClientEndpoint(0).ConnectedPeers()[0], Channel::ReliableOrdered, game_message);
    Run(s, 0.5);
    AETHER_CHECK(client_hits == std::vector<usize>{1});
    AETHER_CHECK(server_hits.size() == 1 && server_hits[0] == s.Info(0).address);
}

AETHER_TEST(NetworkPanel_DrawsHeadless) {
    NetPlayConfig cfg;
    cfg.clients = 2;
    NetPlaySession s(UnitScene(5), cfg);
    NetworkPanel panel(s);
    HeadlessImGui ui;
    ui.Frame([&] { panel.Draw(); }); // before anything has run
    Run(s, 3.0, &panel);
    s.Server().GetComponent<NetEditorUnit>(Units(s)[0])->health = 1;
    Run(s, 1.0, &panel);
    ui.Frame([&] { panel.Draw(); });
    s.DisconnectClient(1);
    Run(s, 0.5, &panel);
    ui.Frame([&] { panel.Draw(); });
    panel.ApplyPreset(3);
    ui.Frame([&] { panel.Draw(); });
    AETHER_CHECK(s.Conditions().latency == 0.06);
}
