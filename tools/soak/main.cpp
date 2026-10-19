// aether_soak (Phase 47 step 5): leak gate. Each scenario runs a few warm-up cycles, notes the live heap bytes, runs
// many more cycles and fails if the heap grew by more than a small allowance or the scenario's own invariant (the
// entity count returning to where it started) broke. Heap bytes are counted exactly: every allocation carries a
// size header, so frees subtract what the matching allocation added.

#include "aether/audio/mixer.h"
#include "aether/audio/sound.h"
#include "aether/core/log.h"
#include "aether/ecs/world.h"
#include "aether/net/replication.h"
#include "aether/scene/components.h"
#include "aether/scene/test_world.h"
#include "aether/streaming/streamer.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {
std::atomic<long long> g_live{0};
constexpr std::size_t kHeader = 32; // keeps 16-byte alignment for the caller

void* Alloc(std::size_t size) {
    void* raw = std::malloc(size + kHeader);
    if (raw == nullptr) throw std::bad_alloc();
    *static_cast<std::size_t*>(raw) = size;
    g_live.fetch_add(static_cast<long long>(size), std::memory_order_relaxed);
    return static_cast<char*>(raw) + kHeader;
}
void Free(void* p) noexcept {
    if (p == nullptr) return;
    void* raw = static_cast<char*>(p) - kHeader;
    g_live.fetch_sub(static_cast<long long>(*static_cast<std::size_t*>(raw)), std::memory_order_relaxed);
    std::free(raw);
}
} // namespace

void* operator new(std::size_t size) { return Alloc(size); }
void* operator new[](std::size_t size) { return Alloc(size); }
void operator delete(void* p) noexcept { Free(p); }
void operator delete[](void* p) noexcept { Free(p); }
void operator delete(void* p, std::size_t) noexcept { Free(p); }
void operator delete[](void* p, std::size_t) noexcept { Free(p); }

using namespace aether;

namespace {

struct Scenario {
    const char* name;
    std::function<void()> setup;
    std::function<bool()> cycle; // false: an invariant broke
    std::function<void()> teardown;
    int cycles;
};

int g_failures = 0;
constexpr long long kAllowanceBytes = 64 * 1024;

void Run(Scenario& s, int scale) {
    if (s.setup) s.setup();
    constexpr int kWarmup = 4;
    bool ok = true;
    for (int i = 0; i < kWarmup && ok; ++i) ok = s.cycle();
    const long long baseline = g_live.load();
    const int cycles = s.cycles * scale;
    for (int i = 0; i < cycles && ok; ++i) ok = s.cycle();
    const long long growth = g_live.load() - baseline;
    const bool pass = ok && growth <= kAllowanceBytes;
    std::printf("%-4s %-34s %5d cycles, heap growth %8lld bytes (allowed %lld)%s\n", pass ? "ok" : "FAIL", s.name, cycles, growth,
                kAllowanceBytes, ok ? "" : ", invariant broken");
    if (!pass) ++g_failures;
    if (s.teardown) s.teardown();
}

} // namespace

int main() {
    Logger::Instance().SetMinLevel(LogLevel::Error);
    int scale = 1;
    if (const char* env = std::getenv("AETHER_SOAK_SCALE")) scale = std::max(1, std::atoi(env));

    // 1. Entity churn: create and destroy, the world must end each cycle where it began.
    World churn_world;
    usize churn_base = 0;
    Scenario churn{"ecs: create and destroy 2000 entities",
                   [&] { churn_base = churn_world.EntityCount(); },
                   [&] {
                       std::vector<Entity> made;
                       for (int i = 0; i < 2000; ++i) made.push_back(churn_world.CreateEntity(Transform{}));
                       for (Entity e : made) churn_world.DestroyEntity(e);
                       return churn_world.EntityCount() == churn_base;
                   },
                   nullptr, 50};

    // 2. Streaming: a camera shuttles across a generated world, cells load and unload.
    World source, game;
    streaming::PartitionResult partition;
    std::unique_ptr<streaming::WorldStreamer> streamer;
    Entity camera = kNullEntity;
    scene::TestWorldStats stats;
    bool forward = true;
    Scenario stream{"streaming: shuttle across a world",
                    [&] {
                        scene::TestWorldParams params;
                        params.blocks_x = 24;
                        params.blocks_z = 24;
                        if (!scene::GenerateTestWorld(source, params, &stats)) std::abort();
                        std::vector<Entity> all;
                        source.ForEachArchetype([&](Archetype& a) {
                            for (usize c = 0; c < a.ChunkCount(); ++c) all.insert(all.end(), a.EntityArray(c), a.EntityArray(c) + a.ChunkEntityCount(c));
                        });
                        for (Entity e : all) source.AddComponent(e, IdComponent{NewEntityGuid()});
                        partition = streaming::PartitionWorld(source, streaming::PartitionSettings{});
                        camera = game.CreateEntity(Transform{}, StreamingSource{90.0f, true});
                        streamer = std::make_unique<streaming::WorldStreamer>(game, partition.index, [&](const streaming::CellCoord& c, std::vector<u8>& bytes) {
                            const auto it = partition.cells.find(c);
                            if (it == partition.cells.end()) return false;
                            bytes = it->second;
                            return true;
                        });
                        streamer->max_loads_per_update = 8;
                    },
                    [&] {
                        // One cycle is a full crossing; the cells in memory depend only on where the camera is.
                        for (int f = 0; f < 60; ++f) {
                            const f32 t = static_cast<f32>(f) / 59.0f;
                            const f32 k = forward ? t : 1.0f - t;
                            game.GetComponent<Transform>(camera)->position = Vec3(k * stats.extent_x, 5.0f, k * stats.extent_z);
                            streamer->Update();
                        }
                        forward = !forward;
                        return true;
                    },
                    nullptr, 20};

    // 3. Replication: entities spawn and despawn on the server, one client follows.
    net::LoopbackNetwork network(1);
    World server_world, client_world;
    std::unique_ptr<net::DatagramSocket> server_socket, client_socket;
    std::unique_ptr<net::NetHost> server_host, client_host;
    std::unique_ptr<net::ReplicationServer> rserver;
    std::unique_ptr<net::ReplicationClient> rclient;
    std::vector<net::PeerId> peers;
    std::vector<Entity> live;
    auto step = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            network.Advance(1.0 / 60.0);
            server_host->Update(network.Now());
            for (const net::NetEvent& e : server_host->TakeEvents()) {
                if (e.type == net::NetEventType::Connected) peers.push_back(e.peer);
                rserver->HandleEvent(e);
            }
            rserver->Update(network.Now());
            client_host->Update(network.Now());
            for (const net::NetEvent& e : client_host->TakeEvents()) rclient->HandleEvent(e);
        }
    };
    Scenario repl{"replication: spawn and despawn churn",
                  [&] {
                      server_socket = network.Open(7777);
                      server_host = std::make_unique<net::NetHost>(*server_socket);
                      server_host->Listen();
                      net::ReplicationConfig config;
                      config.max_profiled_entities = 64; // a small table, so the soak exercises the eviction
                      rserver = std::make_unique<net::ReplicationServer>(server_world, *server_host, config);
                      client_socket = network.Open();
                      client_host = std::make_unique<net::NetHost>(*client_socket);
                      const net::PeerId id = client_host->Connect(net::Address::Loopback(7777), network.Now());
                      rclient = std::make_unique<net::ReplicationClient>(client_world, *client_host, id);
                      step(30);
                  },
                  [&] {
                      for (int i = 0; i < 40; ++i) {
                          net::NetIdentity ni;
                          net::SetNetArchetype(ni, "unit");
                          live.push_back(server_world.CreateEntity(ni, Transform{Vec3(static_cast<f32>(i), 0, 0), Quaternion{}}));
                      }
                      step(30);
                      for (Entity e : live) server_world.DestroyEntity(e);
                      live.clear();
                      step(30);
                      return rclient->Entities().empty();
                  },
                  nullptr, 30};

    // 4. Audio: mix and retire a burst of looping voices repeatedly. This is
    // device-independent, so it also exercises the audio heap gate in headless CI.
    audio::Mixer mixer;
    mixer.AddDefaultBuses();
    const audio::SoundWave tone = audio::GenerateTone(440.0f, 0.25f);
    std::vector<f32> mixed(256);
    Scenario audio_churn{"audio: voice allocation and retirement",
                         nullptr,
                         [&] {
                             std::vector<audio::VoiceId> voices;
                             voices.reserve(64);
                             audio::PlayParams params;
                             params.loop = true;
                             for (int i = 0; i < 64; ++i) voices.push_back(mixer.Play(&tone, params));
                             mixer.Render(mixed.data(), 128);
                             for (audio::VoiceId voice : voices) mixer.Stop(voice);
                             mixer.Render(mixed.data(), 128);
                             return mixer.VoiceCount() == 0;
                         },
                         nullptr, 50};

    Scenario* scenarios[] = {&churn, &stream, &repl, &audio_churn};
    for (Scenario* s : scenarios) Run(*s, scale);
    // Tear the long-lived objects down inside main so nothing outlives the counter.
    std::printf("%s\n", g_failures == 0 ? "soak passed" : "soak FAILED");
    return g_failures == 0 ? 0 : 1;
}
