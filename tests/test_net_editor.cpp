#include "net/net_panels.h"
#include "test_framework.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <functional>

// Phase 22 step 6: networked Play-in-Editor (listen and dedicated servers
// with clients over a simulated network), the network profiler's views,
// and the panels drawn headless.

using namespace aether;
using namespace aether::editor;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

constexpr f64 kStep = 1.0 / 60.0;

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGui::GetIO().ConfigMacOSXBehaviors = false; // tests press Ctrl on every platform
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1200, 800);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1200, 800));
        ImGui::Begin("Net", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

// Three crates and a settings entity with no Transform.
void BuildLevel(World& world) {
    for (int i = 0; i < 3; ++i) {
        ModelRenderer model;
        SetModelPath(model, "models/crate.gltf");
        world.CreateEntity(Transform{Vec3(static_cast<f32>(i) * 3.0f, 0, 5), Quaternion{}}, model);
    }
    world.CreateEntity(ModelRenderer{});
}

usize Networked(World& world) {
    usize n = 0;
    world.ForEach<net::NetIdentity>([&](net::NetIdentity&) { ++n; });
    return n;
}

} // namespace

AETHER_TEST(NetPlay_ListenServerAndClients) {
    World edited;
    BuildLevel(edited);
    NetPlaySession play;
    NetPlaySettings settings;
    settings.clients = 2;
    settings.conditions.latency = 0.03;
    CHECK(play.Start(edited, settings) && play.Running() && play.ClientCount() == 2);
    for (int i = 0; i < 60; ++i) play.Tick(kStep);
    // 3 crates, the listen player and 2 client players.
    CHECK(Networked(play.ServerWorld()) == 6 && !play.ListenPawn().IsNull());
    for (usize c = 0; c < 2; ++c) {
        CHECK(play.Client(c).connected && play.Client(c).entities == 6 && play.Client(c).rtt > 0.05);
        CHECK(!play.ClientPawn(c).IsNull() && !play.ServerPawn(c).IsNull());
        // Crates arrive with their models, as if the level were loaded.
        usize crates = 0;
        play.ClientWorld(c).ForEach<net::NetIdentity, ModelRenderer>([&](net::NetIdentity& ni, ModelRenderer& m) {
            if (std::string(ni.archetype) == "scene" && std::string(m.asset_path) == "models/crate.gltf") ++crates;
        });
        CHECK(crates == 3);
    }
    CHECK(play.ClientPawn(0) != Entity{} && play.ServerPawn(0) != play.ServerPawn(1));
    // The edited world is untouched.
    CHECK(edited.EntityCount() == 4 && Networked(edited) == 0);

    // Client 1 walks; its prediction leads and the server follows.
    World& c0 = play.ClientWorld(0);
    const f32 start_x = c0.GetComponent<Transform>(play.ClientPawn(0))->position.x;
    for (int i = 0; i < 60; ++i) {
        CHECK(play.MoveClient(0, 1, 0, false, static_cast<f32>(kStep)));
        play.Tick(kStep);
    }
    for (int i = 0; i < 30; ++i) play.MoveClient(0, 0, 0, false, static_cast<f32>(kStep)), play.Tick(kStep);
    const Vec3 predicted = c0.GetComponent<Transform>(play.ClientPawn(0))->position;
    CHECK(predicted.x > start_x + 3.0f);
    CHECK((predicted - play.ServerWorld().GetComponent<Transform>(play.ServerPawn(0))->position).Length() < 1e-3f);
    CHECK(play.Client(0).corrections == 0);
    // The listen player moves on the server; client 2 sees it.
    for (int i = 0; i < 30; ++i) play.MoveListenPlayer(0, 1, false, static_cast<f32>(kStep)), play.Tick(kStep);
    for (int i = 0; i < 40; ++i) play.Tick(kStep);
    const Vec3 listen = play.ServerWorld().GetComponent<Transform>(play.ListenPawn())->position;
    CHECK(listen.z > 1.0f);
    const u32 listen_id = play.ServerWorld().GetComponent<net::NetIdentity>(play.ListenPawn())->net_id;
    bool seen = false;
    f32 error = 1e9f;
    play.ClientWorld(1).ForEach<net::NetIdentity, Transform>([&](net::NetIdentity& ni, Transform& t) {
        if (ni.net_id == listen_id) seen = true, error = (t.position - listen).Length();
    });
    CHECK(seen && error < 1e-3f);
    CHECK(!play.MoveClient(5, 1, 0, false, 0.016f));
    play.Stop();
    CHECK(!play.Running() && play.ClientCount() == 0 && !play.MoveListenPlayer(1, 0, false, 0.016f));
}

AETHER_TEST(NetPlay_DedicatedServerUnderBadNetwork) {
    World edited;
    BuildLevel(edited);
    NetPlaySession play;
    NetPlaySettings settings;
    settings.mode = NetPlayMode::DedicatedServer;
    settings.clients = 3;
    settings.replicate_scene = false;
    CHECK(play.Start(edited, settings));
    for (int i = 0; i < 60; ++i) play.Tick(kStep);
    CHECK(play.ListenPawn().IsNull() && Networked(play.ServerWorld()) == 3); // just the players
    // The network turns bad while playing.
    net::LinkConditions bad;
    bad.latency = 0.08, bad.jitter = 0.03, bad.loss = 0.15f;
    play.SetConditions(bad);
    CHECK(play.Settings().conditions.loss == 0.15f);
    for (int i = 0; i < 180; ++i) {
        for (usize c = 0; c < 3; ++c) play.MoveClient(c, std::cos(static_cast<f32>(i + c) * 0.05f), 0.5f, i == 40, static_cast<f32>(kStep));
        play.Tick(kStep);
    }
    for (int i = 0; i < 120; ++i) {
        for (usize c = 0; c < 3; ++c) play.MoveClient(c, 0, 0, false, static_cast<f32>(kStep));
        play.Tick(kStep);
    }
    for (usize c = 0; c < 3; ++c) {
        const Vec3 mine = play.ClientWorld(c).GetComponent<Transform>(play.ClientPawn(c))->position;
        const Vec3 server = play.ServerWorld().GetComponent<Transform>(play.ServerPawn(c))->position;
        CHECK((mine - server).Length() < 1e-3f);
        CHECK(play.Client(c).loss > 0.0f && play.Client(c).entities == 3);
    }
    // Restarting starts clean.
    CHECK(play.Start(edited, settings) && Networked(play.ServerWorld()) == 0 && play.Client(0).entities == 0);
}

AETHER_TEST(NetPlay_ProfilerFindsTheExpensiveThings) {
    World edited;
    BuildLevel(edited);
    NetPlaySession play;
    NetPlaySettings settings;
    settings.clients = 1;
    CHECK(play.Start(edited, settings));
    for (int i = 0; i < 30; ++i) play.Tick(kStep);
    play.ResetProfile();
    CHECK(play.Profile().snapshot_bytes == 0 && play.Profile().since == play.Now());
    for (int i = 0; i < 120; ++i) {
        play.MoveListenPlayer(1, 0, false, static_cast<f32>(kStep));
        play.Tick(kStep);
    }
    const net::NetProfile& p = play.Profile();
    CHECK(p.snapshots > 30 && p.snapshot_bytes > 0);
    const std::vector<NetEntityRow> entities = BuildEntityRows(p, play.Now());
    CHECK(!entities.empty());
    // The moving listen player is the most expensive, and Transform its biggest component.
    const u32 listen_id = play.ServerWorld().GetComponent<net::NetIdentity>(play.ListenPawn())->net_id;
    CHECK(entities[0].net_id == listen_id && entities[0].archetype == "player" && entities[0].updates > 30);
    CHECK(!entities[0].components.empty() && entities[0].components[0].first == "Transform");
    CHECK(entities[0].bytes_per_second > 0.0);
    for (usize i = 1; i < entities.size(); ++i) CHECK(entities[i - 1].bytes >= entities[i].bytes);
    const std::vector<NetFieldRow> fields = BuildFieldRows(p, play.Now());
    CHECK(!fields.empty() && fields[0].component == "Transform" && fields[0].field == "position");
    CHECK(std::any_of(fields.begin(), fields.end(), [](const NetFieldRow& f) { return f.component == "NetMovement" && f.field == "velocity"; }));
    // Still crates aren't re-sent.
    CHECK(std::none_of(entities.begin(), entities.end(), [](const NetEntityRow& r) { return r.archetype == "scene"; }));
}

AETHER_TEST(NetPlay_PanelsDrawHeadless) {
    HeadlessImGui imgui;
    World edited;
    BuildLevel(edited);
    NetPlaySession play;
    NetPlayPanel panel(play, edited);
    NetProfilerPanel profiler(play);
    panel.settings.clients = 2;
    for (int i = 0; i < 2; ++i) imgui.Frame([&] { panel.Draw(); profiler.Draw(); });
    CHECK(!play.Running());
    CHECK(panel.Start() && play.Running() && panel.LastError().empty());
    for (int i = 0; i < 90; ++i) {
        play.MoveClient(0, 1, 0, false, static_cast<f32>(kStep));
        play.Tick(kStep);
        imgui.Frame([&] { panel.Draw(); profiler.Draw(); });
    }
    CHECK(play.Client(1).connected && profiler.view == NetProfilerPanel::View::Entities);
    panel.Stop();
    imgui.Frame([&] { panel.Draw(); profiler.Draw(); });
    CHECK(!play.Running());
}
