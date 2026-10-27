// Ghost Shift first-playable checks (docs/design/GHOST_SHIFT_ENGINE_INTEGRATION.md, GS-03/05/06/07 foundations).
// The cooked games/GHOST-SHIFT project is run headless in the player at 30, 60 and 144 Hz render rates (the
// project simulates at a fixed 30 Hz), plus an in-process loopback host with two clients for the
// host-validated vacuum request (test-local replicated components, since Luau state doesn't replicate).
#include "aether/cook/cooker.h"
#include "aether/gameplay/attribute_set.h"
#include "aether/net/rpc.h"
#include "aether/net/session.h"
#include "aether/player/game.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "test_framework.h"
#if AETHER_TEST_HAS_PHYSICS
#include "aether/physics/character.h"
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace aether;
namespace fs = std::filesystem;

#define GS_CHECK(...) AETHER_CHECK((__VA_ARGS__))

#if AETHER_TEST_HAS_SCRIPTING && AETHER_TEST_HAS_PHYSICS
namespace {

struct GhostShiftProject {
    fs::path root, pak_file;
    GhostShiftProject() {
        root = fs::temp_directory_path() / "ghost_shift_tests" / assets::ToString(assets::NewAssetGuid());
        fs::create_directories(root);
        const auto source = fs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "games/GHOST-SHIFT";
        fs::copy(source, root / "project", fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        cook::CookOptions options;
        options.project_file = root / "project/GHOST-SHIFT.aproject";
        options.output_dir = root / "cooked";
        const auto cooked = cook::Cook(options);
        GS_CHECK(cooked.ok);
        pak_file = cooked.pak_file;
    }
};

const GhostShiftProject& Project() {
    static const GhostShiftProject project;
    return project;
}

constexpr int kRates[] = {30, 60, 144};

struct GsRun {
    fs::path user;
    player::GamePackage package;
    std::unique_ptr<player::Game> game;
    Entity player, prankster, van, tool;
    std::vector<Entity> patches; // Patch1..Patch4
    std::vector<Vec3> hiding;    // Hiding1..Hiding4
    int rate;

    GsRun(const char* name, int fps, fs::path user_dir = {}) : rate(fps) {
        user = user_dir.empty() ? Project().root / (std::string(name) + "-" + std::to_string(fps) + "-" + assets::ToString(assets::NewAssetGuid())) : user_dir;
        std::string error;
        GS_CHECK(package.Mount(Project().pak_file.string(), 0, &error));
        GS_CHECK(package.LoadManifest(&error));
        Start();
    }

    Entity One(const char* tag) {
        const auto found = FindEntitiesWithTag(game->GetWorld(), tag);
        GS_CHECK(found.size() == 1);
        return found.empty() ? Entity{} : found.front();
    }

    void Start() {
        game.reset();
        game = std::make_unique<player::Game>(package);
        std::string error;
        game->SetUserPaths(player::ResolveUserPaths("GHOST-SHIFT", user));
        GS_CHECK(game->LoadStartupScene(&error));
        player = One("Player");
        prankster = One("Prankster");
        van = One("Van");
        tool = One("StolenTool");
        patches.clear();
        hiding.clear();
        for (int i = 1; i <= 4; ++i) patches.push_back(One(("Patch" + std::to_string(i)).c_str()));
        for (int i = 1; i <= 4; ++i) hiding.push_back(game->GetWorld().GetComponent<Transform>(One(("Hiding" + std::to_string(i)).c_str()))->position);
        game->BeginPlay();
        Frames(1);
    }

    void Frames(int count) {
        for (int i = 0; i < count; ++i) game->Tick(1.0f / static_cast<f32>(rate));
        GS_CHECK(game->ScriptErrors().empty());
        for (const auto& e : game->ScriptErrors()) std::printf("script error: %s\n", e.c_str());
    }
    void Seconds(f32 s) { Frames(static_cast<int>(std::lround(s * static_cast<f32>(rate)))); }

    f32 Stat(Entity e, const char* name) {
        const auto* a = game->GetWorld().GetComponent<gas::AttributeSet>(e);
        GS_CHECK(a != nullptr);
        return a ? a->Get(name, -999) : -999;
    }
    void Set(Entity e, const char* name, f32 value) { game->GetWorld().GetComponent<gas::AttributeSet>(e)->SetBase(name, value); }
    f32 P(const char* name) { return Stat(player, name); }
    f32 G(const char* name) { return Stat(prankster, name); }
    Vec3 Pos(Entity e) { return game->GetWorld().GetComponent<Transform>(e)->position; }

    void Teleport(const Vec3& at) {
        game->GetWorld().GetComponent<Transform>(player)->position = at;
        Frames(2);
    }
    void Hold(input::Key key, f32 seconds) {
        game->Input().SetButton(key, true);
        Seconds(seconds);
        game->Input().SetButton(key, false);
        Frames(2);
    }
    void Tap(input::Key key) { Hold(key, 0.15f); Seconds(0.1f); }
    void Suppress(bool on) { Set(prankster, "Suppressed", on ? 1.0f : 0.0f); }
    Vec3 Beside(const Vec3& spot) { return Vec3{spot.x + 0.8f, (spot.y < -1.0f ? -3.0f : 0.0f) + 0.05f, spot.z}; }
    void Vacuum(int patch, f32 seconds = 1.2f) {
        Teleport(Beside(Pos(patches[static_cast<usize>(patch)])));
        Hold(input::Key::MouseLeft, seconds);
    }
    void EmptyAtVan() {
        Teleport(Vec3{0.0f, 0.05f, 5.5f});
        Tap(input::Key::E);
    }
};

} // namespace

AETHER_TEST(GhostShift_CookedStartState) {
    GsRun run("start", 60);
    GS_CHECK(run.P("Battery") == 30 && run.P("Tank") == 0 && run.P("TankMax") == 6);
    GS_CHECK(run.P("ToolIndex") == 1 && run.P("HasVacuum") == 1 && run.P("HasScrubber") == 1);
    GS_CHECK(run.P("Haunting") < 0.1f && run.P("HauntingStage") == 0);
    GS_CHECK(run.P("Currency") == 0 && run.P("ContractsCompleted") == 0 && run.P("QuestDone") == 0);
    for (Entity p : run.patches) GS_CHECK(run.Stat(p, "Residue") == 2);
    GS_CHECK(run.G("GhostState") == 0 && run.G("Warnings") == 0 && run.G("Pranks") == 0);
    GS_CHECK(run.game->GetWorld().GetComponent<CharacterMovement>(run.player) != nullptr);
    GS_CHECK(FindEntitiesWithTag(run.game->GetWorld(), "HidingSpot").size() == 4);
    GS_CHECK(FindEntitiesWithTag(run.game->GetWorld(), "AtticHatch").size() == 1);
    GS_CHECK(run.game->Stats().physics_bodies > 30);
}

AETHER_TEST(GhostShift_VacuumCostsAndNeverSpendsOnCleanPatch) {
    for (int rate : kRates) {
        GsRun run("vacuum", rate);
        run.Suppress(true);
        run.Vacuum(0, 1.2f); // three pulse windows, two pulses of residue
        GS_CHECK(run.Stat(run.patches[0], "Residue") == 0);
        GS_CHECK(run.P("Battery") == 26 && run.P("Tank") == 2 && run.P("VacuumPulses") == 2 && run.P("PatchesCleaned") == 1);
        // Holding on over the clean spot spends nothing more.
        run.Hold(input::Key::MouseLeft, 2.0f);
        GS_CHECK(run.P("Battery") == 26 && run.P("Tank") == 2 && run.P("VacuumPulses") == 2 && run.P("PatchesCleaned") == 1);
        // Nothing in reach at the van either.
        run.Teleport(Vec3{0.0f, 0.05f, 5.5f});
        run.Hold(input::Key::MouseLeft, 1.0f);
        GS_CHECK(run.P("Battery") == 26 && run.P("Tank") == 2);
        // UV scrubber on ectoplasm: wrong cleaning, haunting up, no residue removed, no battery spent.
        run.Tap(input::Key::Q);
        GS_CHECK(run.P("ToolIndex") == 2);
        const f32 before = run.P("Haunting");
        run.Vacuum(1, 0.3f);
        GS_CHECK(run.P("WrongCleans") == 1 && run.Stat(run.patches[1], "Residue") == 2 && run.P("Battery") == 26);
        GS_CHECK(run.P("Haunting") >= before + 5.0f);
        // Empty the tank at the van.
        run.EmptyAtVan();
        GS_CHECK(run.P("Tank") == 0 && run.P("TankEmptied") == 2);
    }
}

AETHER_TEST(GhostShift_ContractCompletesOncePaysOnceAndCompanySaveSurvivesRestart) {
    const fs::path user = Project().root / ("company-" + assets::ToString(assets::NewAssetGuid()));
    for (int rate : kRates) {
        fs::remove_all(user);
        {
            GsRun run("contract", rate, user);
            run.Suppress(true);
            for (int i = 0; i < 3; ++i) run.Vacuum(i);
            GS_CHECK(run.P("Tank") == 6 && run.P("PatchesCleaned") == 3);
            // A full tank stops the vacuum (and spends nothing) until it is emptied at the van.
            run.Vacuum(3, 0.9f);
            GS_CHECK(run.Stat(run.patches[3], "Residue") == 2 && run.P("Battery") == 18);
            run.EmptyAtVan();
            GS_CHECK(run.P("Extracted") == 0 && run.P("QuestDone") == 0); // not clean yet: no extraction
            run.Vacuum(3);
            GS_CHECK(run.P("PatchesCleaned") == 4 && run.P("QuestDone") == 0);
            run.EmptyAtVan();
            GS_CHECK(run.P("Extracted") == 1 && run.P("QuestDone") == 1);
            GS_CHECK(run.P("Currency") == 250 && run.P("ContractsCompleted") == 1 && run.P("SaveStatus") == 1);
            // More presses, more cleaning attempts: still paid once.
            run.EmptyAtVan();
            run.EmptyAtVan();
            for (int i = 0; i < 4; ++i) run.Vacuum(i, 0.5f);
            run.Seconds(1.0f);
            GS_CHECK(run.P("Currency") == 250 && run.P("ContractsCompleted") == 1 && run.P("PatchesCleaned") == 4);
            // Restart the game: the company save brings the balance back, without paying again.
            run.Start();
            GS_CHECK(run.P("SaveStatus") == 1 && run.P("Currency") == 250 && run.P("ContractsCompleted") == 1 && run.P("QuestDone") == 0);
            run.Seconds(2.0f);
            run.Start();
            GS_CHECK(run.P("Currency") == 250 && run.P("ContractsCompleted") == 1);
        }
        GsRun again("contract-reopen", rate, user); // a new process' worth of objects, same user folder
        GS_CHECK(again.P("Currency") == 250 && again.P("ContractsCompleted") == 1 && again.P("SaveStatus") == 1);
    }
}

AETHER_TEST(GhostShift_HauntingRisesWhileDirtyAndFallsWithCleaning) {
    for (int rate : kRates) {
        GsRun run("haunting", rate);
        run.Suppress(true);
        run.Seconds(10.0f);
        const f32 h10 = run.P("Haunting");
        GS_CHECK(std::fabs(h10 - 6.0f) < 0.25f); // 4 dirty patches x 0.15/s
        run.Seconds(30.0f);
        GS_CHECK(run.P("Haunting") > 23.0f && run.P("HauntingStage") == 1); // Stirring
        const f32 dirty = run.P("Haunting");
        run.Vacuum(0, 0.3f); // one pulse
        GS_CHECK(run.P("Haunting") < dirty);
        for (int i = 0; i < 3; ++i) run.Vacuum(i);
        run.EmptyAtVan();
        run.Vacuum(3);
        GS_CHECK(run.P("PatchesCleaned") == 4);
        const f32 clean = run.P("Haunting");
        run.Seconds(5.0f);
        GS_CHECK(run.P("Haunting") < clean - 4.0f); // calming down once the house is clean
        // Wrong cleaning can push it up through the stages; the attribute stays within 0..100.
        run.Set(run.player, "Haunting", 79.0f);
        run.Seconds(0.2f);
        run.Tap(input::Key::Q);
        run.Set(run.patches[0], "Residue", 2); // a fresh smear to get wrong
        run.game->GetWorld().GetComponent<Transform>(run.patches[0])->position = Vec3{-3.0f, 0.02f, -3.0f};
        run.Vacuum(0, 0.3f);
        GS_CHECK(run.P("HauntingStage") == 4 && run.P("Haunting") <= 100.0f);
    }
}

namespace {
struct PrankEvent {
    int kind; // 0 warning, 1 prank
    int step;
    int spot;
    bool operator==(const PrankEvent&) const = default;
};

std::vector<PrankEvent> WatchPrankster(GsRun& run, f32 seconds) {
    std::vector<PrankEvent> log;
    f32 warnings = 0, pranks = 0;
    const int frames = static_cast<int>(std::lround(seconds * static_cast<f32>(run.rate)));
    for (int f = 0; f < frames; ++f) {
        run.Frames(1);
        if (run.G("Warnings") != warnings) {
            warnings = run.G("Warnings");
            log.push_back({0, static_cast<int>(run.G("LastWarningStep")), 0});
            GS_CHECK(run.G("Warning") == 1);
        }
        if (run.G("Pranks") != pranks) {
            pranks = run.G("Pranks");
            log.push_back({1, static_cast<int>(run.G("LastPrankStep")), static_cast<int>(run.G("LastHideSpot"))});
        }
    }
    return log;
}
} // namespace

AETHER_TEST(GhostShift_PranksterIsDeterministicAndWarnsBeforeEveryPrank) {
    std::vector<PrankEvent> reference;
    for (int rate : kRates) {
        GsRun run("prankster", rate);
        const auto log = WatchPrankster(run, 45.0f);
        int pranks = 0;
        for (usize i = 0; i < log.size(); ++i) {
            if (log[i].kind != 1) continue;
            ++pranks;
            // The event right before every prank is its warning, a full 1.5 s (45 steps) earlier.
            GS_CHECK(i > 0 && log[i - 1].kind == 0 && log[i].step - log[i - 1].step == 45);
            GS_CHECK(log[i].spot >= 1 && log[i].spot <= 4);
        }
        GS_CHECK(pranks == 1); // one tool hidden at a time; later visits only warn when there's something to take
        if (reference.empty()) reference = log;
        GS_CHECK(log == reference);
        // The held tool (the vacuum) was taken and hidden at the logged spot; Interact there gets it back.
        GS_CHECK(run.P("HasVacuum") == 0 && run.P("HasScrubber") == 1 && run.Stat(run.tool, "Item") == 1);
        GS_CHECK(run.P("Haunting") > 5.0f);
        run.Suppress(true);
        const Vec3 spot = run.hiding[static_cast<usize>(run.G("LastHideSpot")) - 1];
        GS_CHECK((run.Pos(run.tool) - spot).Length() < 0.5f);
        run.Teleport(Vec3{spot.x + 1.0f, (spot.y < -1.0f ? -3.0f : 0.0f) + 0.05f, spot.z});
        run.Tap(input::Key::E);
        GS_CHECK(run.P("HasVacuum") == 1 && run.P("ToolsRecovered") == 1 && run.Stat(run.tool, "Item") == 0);
    }
    GS_CHECK(reference.size() >= 3);
}

AETHER_TEST(GhostShift_StunBlastCountersThePrank) {
    for (int rate : kRates) {
        GsRun run("stun", rate);
        int guard = 0;
        while (run.G("GhostState") != 1 && guard++ < rate * 20) run.Frames(1);
        GS_CHECK(run.G("GhostState") == 1);
        run.Seconds(1.0f); // mid-investigation
        GS_CHECK(run.G("GhostState") == 1 && run.G("Pranks") == 0);
        const Vec3 ghost = run.Pos(run.prankster);
        run.Teleport(Vec3{ghost.x, (ghost.y < -1.0f ? -3.0f : 0.0f) + 0.05f, ghost.z});
        run.Hold(input::Key::MouseLeft, 0.2f);
        GS_CHECK(run.G("GhostState") == 4 && run.G("Stuns") == 1 && run.P("Stuns") == 1);
        GS_CHECK(run.P("Battery") == 26 && run.G("Warning") == 0);
        run.Seconds(3.5f);
        GS_CHECK(run.G("GhostState") == 4 && run.G("Pranks") == 0 && run.P("HasVacuum") == 1);
        run.Seconds(1.0f);
        GS_CHECK(run.G("GhostState") == 0); // recovers to Dormant, ready to try again
    }
}
#endif

// ------------------------------------------------------------------------------------------------
// Loopback multiplayer: host-validated vacuum requests (GS-02/GS-03 foundation). The Luau gameplay
// state above is not replicated by the engine, so this models the contract with test-local
// components on the real session/replication/RPC stack.
namespace gs_net {
struct GsPatch {
    i32 residue = 2;
    i32 removed_total = 0; // every residue removal the host ever made on this patch
};
struct GsCleaner {
    i32 battery = 30;
    i32 tank = 0;
    i32 accepted = 0;
    i32 refused = 0;
    std::vector<std::pair<u32, u32>> pending; // host only: (patch net id, request id); not replicated
    u32 last_request = 0;                      // host only: duplicate/late request filter
    void RequestVacuum(u32 patch, u32 request) { pending.push_back({patch, request}); }
};
} // namespace gs_net

AETHER_REFLECT(gs_net::GsPatch, 1, AETHER_FIELD(residue, Field_Replicated), AETHER_FIELD(removed_total, Field_Replicated))
AETHER_REFLECT(gs_net::GsCleaner, 1,
    AETHER_FIELD(battery, Field_Replicated), AETHER_FIELD(tank, Field_Replicated),
    AETHER_FIELD(accepted, Field_Replicated), AETHER_FIELD(refused, Field_Replicated),
    AETHER_METHOD(RequestVacuum, Fn_Server, {"patch", "request"}))

namespace {
using namespace aether::net;
using gs_net::GsCleaner;
using gs_net::GsPatch;

constexpr u16 kGsPort = 7790;
constexpr u32 kGsBuild = 3001;
constexpr f64 kGsStep = 1.0 / 30.0;
constexpr f32 kReach = 2.5f;

struct GsNetSim {
    LoopbackNetwork net;
    World world;
    std::unique_ptr<DatagramSocket> socket;
    std::unique_ptr<NetHost> host;
    std::unique_ptr<SessionHost> session;
    std::unique_ptr<ReplicationServer> replication;
    std::unique_ptr<RpcRouter> rpc;
    std::vector<Entity> patches;
    std::vector<std::pair<std::string, Entity>> pawns;

    struct Client {
        World world;
        std::unique_ptr<DatagramSocket> socket;
        std::unique_ptr<NetHost> host;
        std::unique_ptr<SessionClient> session;
        std::unique_ptr<ReplicationClient> replication;
        std::unique_ptr<RpcRouter> rpc;
        u32 next_request = 1;
    };
    std::vector<std::unique_ptr<Client>> clients;

    explicit GsNetSim(u64 seed) : net(seed) {
        (void)GetComponentId<GsPatch>();
        (void)GetComponentId<GsCleaner>();
        socket = net.Open(kGsPort);
        host = std::make_unique<NetHost>(*socket);
        host->Listen();
        SessionInfo info;
        info.name = "ghost shift sync lab";
        info.port = kGsPort;
        info.build = kGsBuild;
        info.max_players = 4;
        session = std::make_unique<SessionHost>(*host, info, "willow");
        replication = std::make_unique<ReplicationServer>(world, *host);
        rpc = std::make_unique<RpcRouter>(world, *host, *replication);
        const Vec3 at[] = {{-3, 0, -3}, {3, 0, -5}, {-3, 0, -12}, {0, -3, -13}};
        for (const Vec3& p : at) {
            NetIdentity ni;
            SetNetArchetype(ni, "patch");
            patches.push_back(world.CreateEntity(ni, Transform{p, Quaternion{}}, GsPatch{}));
        }
    }

    Client& AddClient(const std::string& name) {
        auto c = std::make_unique<Client>();
        c->socket = net.Open(0);
        c->host = std::make_unique<NetHost>(*c->socket);
        c->session = std::make_unique<SessionClient>(*c->host);
        c->session->Join(Address::Loopback(kGsPort), name, "willow", kGsBuild, net.Now());
        const PeerId server = c->session->Server();
        c->replication = std::make_unique<ReplicationClient>(c->world, *c->host, server);
        c->replication->OnSpawn("patch", [](World& w, Entity e, const NetIdentity&) { w.AddComponent(e, GsPatch{}); });
        c->replication->OnSpawn("cleaner", [](World& w, Entity e, const NetIdentity&) { w.AddComponent(e, GsCleaner{}); });
        c->rpc = std::make_unique<RpcRouter>(c->world, *c->host, *c->replication, server);
        clients.push_back(std::move(c));
        return *clients.back();
    }

    void OnSessionEvent(const SessionEvent& e) {
        if (e.type != SessionEventType::PlayerJoined) return;
        NetIdentity ni;
        SetNetArchetype(ni, "cleaner");
        ni.owner = e.peer;
        const Entity pawn = world.CreateEntity(ni, Transform{Vec3{0, 0, 4}, Quaternion{}}, GsCleaner{});
        pawns.push_back({e.name, pawn});
    }

    Entity Pawn(const std::string& name) {
        for (auto& [n, e] : pawns)
            if (n == name) return e;
        return Entity{};
    }

    // The host's validation of every queued request: reach, battery, tank space, residue left, request order.
    void Validate() {
        for (auto& [name, pawn] : pawns) {
            GsCleaner* cleaner = world.GetComponent<GsCleaner>(pawn);
            const Vec3 at = world.GetComponent<Transform>(pawn)->position;
            auto pending = std::move(cleaner->pending);
            cleaner->pending.clear();
            for (auto [patch_id, request] : pending) {
                const Entity patch = replication->FindEntity(patch_id);
                GsPatch* p = patch.IsNull() ? nullptr : world.GetComponent<GsPatch>(patch);
                const bool fresh = request > cleaner->last_request;
                const bool ok = fresh && p != nullptr && p->residue > 0 && cleaner->battery >= 2 && cleaner->tank < 6 &&
                                (world.GetComponent<Transform>(patch)->position - at).Length() <= kReach;
                if (fresh) cleaner->last_request = request;
                if (!ok) {
                    ++cleaner->refused;
                    continue;
                }
                cleaner->battery -= 2;
                cleaner->tank += 1;
                ++cleaner->accepted;
                --p->residue;
                ++p->removed_total;
            }
        }
    }

    void Step(int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            net.Advance(kGsStep);
            const f64 now = net.Now();
            host->Update(now);
            for (const NetEvent& e : host->TakeEvents())
                if (!session->HandleEvent(e, now) && !replication->HandleEvent(e)) rpc->HandleEvent(e);
            session->Update(now);
            for (const SessionEvent& e : session->TakeEvents()) OnSessionEvent(e);
            rpc->Tick(now);
            Validate();
            replication->Update(now);
            for (auto& c : clients) {
                c->host->Update(now);
                for (const NetEvent& e : c->host->TakeEvents())
                    if (!c->session->HandleEvent(e) && !c->replication->HandleEvent(e)) c->rpc->HandleEvent(e);
                c->rpc->Tick(now);
            }
        }
    }

    bool AllJoined() const {
        return std::all_of(clients.begin(), clients.end(), [](const auto& c) { return c->session->GetState() == SessionClient::State::Joined; }) &&
               pawns.size() == clients.size();
    }

    u32 NetId(Entity e) { return world.GetComponent<NetIdentity>(e)->net_id; }

    // A client asks to vacuum through its replicated copy of a pawn (its own, or - as a cheat - someone else's).
    RemoteCallResult Request(Client& c, Entity host_pawn, Entity host_patch, u32 request) {
        const Entity local = c.replication->FindEntity(NetId(host_pawn));
        if (local.IsNull() || c.world.GetComponent<GsCleaner>(local) == nullptr) return RemoteCallResult::Refused;
        const reflect::TypeInfo& type = reflect::Reflect<GsCleaner>();
        std::vector<reflect::Any> args = {u32{NetId(host_patch)}, u32{request}};
        RemoteCallResult result = RemoteCallResult::Refused;
        CallFunction(c.world, local, type, *type.FindFunction("RequestVacuum"), c.world.GetComponent<GsCleaner>(local), args, nullptr, &result);
        return result;
    }
    RemoteCallResult Request(Client& c, const std::string& as, int patch) { return Request(c, Pawn(as), patches[static_cast<usize>(patch)], c.next_request++); }

    void MovePawn(const std::string& name, const Vec3& at) { world.GetComponent<Transform>(Pawn(name))->position = at; }

    bool Converged() {
        for (auto& c : clients) {
            for (Entity patch : patches) {
                const Entity local = c->replication->FindEntity(NetId(patch));
                if (local.IsNull()) return false;
                const GsPatch* mine = c->world.GetComponent<GsPatch>(local);
                const GsPatch* truth = world.GetComponent<GsPatch>(patch);
                if (mine == nullptr || mine->residue != truth->residue || mine->removed_total != truth->removed_total) return false;
            }
            for (auto& [name, pawn] : pawns) {
                const Entity local = c->replication->FindEntity(NetId(pawn));
                if (local.IsNull()) return false;
                const GsCleaner* mine = c->world.GetComponent<GsCleaner>(local);
                const GsCleaner* truth = world.GetComponent<GsCleaner>(pawn);
                if (mine == nullptr || mine->battery != truth->battery || mine->tank != truth->tank || mine->accepted != truth->accepted) return false;
            }
        }
        return true;
    }
};

void RunVacuumSyncLab(u64 seed, LinkConditions conditions) {
    GsNetSim sim(seed);
    sim.net.conditions = conditions;
    sim.AddClient("ana");
    sim.AddClient("ben");
    for (int f = 0; f < 600 && !sim.AllJoined(); ++f) sim.Step();
    GS_CHECK(sim.AllJoined());
    sim.Step(60);
    auto& ana = *sim.clients[0];
    auto& ben = *sim.clients[1];

    // Out of reach: refused, nothing spent.
    GS_CHECK(sim.Request(ana, "ana", 0) == RemoteCallResult::Sent);
    sim.Step(30);
    GS_CHECK(sim.world.GetComponent<GsPatch>(sim.patches[0])->residue == 2);
    GS_CHECK(sim.world.GetComponent<GsCleaner>(sim.Pawn("ana"))->battery == 30);

    // Both stand at patch 0; both request three times (four total would be needed for two residue).
    sim.MovePawn("ana", Vec3{-2.2f, 0, -3});
    sim.MovePawn("ben", Vec3{-3.8f, 0, -3});
    sim.Step(10);
    for (int i = 0; i < 3; ++i) {
        GS_CHECK(sim.Request(ana, "ana", 0) == RemoteCallResult::Sent);
        GS_CHECK(sim.Request(ben, "ben", 0) == RemoteCallResult::Sent);
        sim.Step(1);
    }
    // A cheat: ben calls through ana's pawn. Refused locally; forced locally-owned, the host refuses it.
    GS_CHECK(sim.Request(ben, "ana", 0) == RemoteCallResult::Refused);
    const Entity ana_on_ben = ben.replication->FindEntity(sim.NetId(sim.Pawn("ana")));
    ben.world.GetComponent<NetIdentity>(ana_on_ben)->locally_owned = true;
    GS_CHECK(sim.Request(ben, "ana", 1) == RemoteCallResult::Sent);
    // A replayed (duplicate) request id from ana is ignored.
    GS_CHECK(sim.Request(ana, sim.Pawn("ana"), sim.patches[0], 1) == RemoteCallResult::Sent);

    bool converged = false;
    for (int f = 0; f < 300 && !converged; ++f) {
        sim.Step();
        converged = sim.Converged();
    }
    sim.Step(90);
    GS_CHECK(sim.Converged());
    const GsPatch* p0 = sim.world.GetComponent<GsPatch>(sim.patches[0]);
    GS_CHECK(p0->residue == 0 && p0->removed_total == 2); // no double removal, whatever arrived first
    GS_CHECK(sim.world.GetComponent<GsPatch>(sim.patches[1])->residue == 2);
    const GsCleaner* a = sim.world.GetComponent<GsCleaner>(sim.Pawn("ana"));
    const GsCleaner* b = sim.world.GetComponent<GsCleaner>(sim.Pawn("ben"));
    // Only accepted requests were paid for, and exactly two were accepted in total.
    GS_CHECK(a->accepted + b->accepted == 2);
    GS_CHECK(a->battery == 30 - 2 * a->accepted && b->battery == 30 - 2 * b->accepted);
    GS_CHECK(a->tank == a->accepted && b->tank == b->accepted);
    GS_CHECK(sim.rpc->Stats().rejected >= 1); // the forged-ownership call
}
} // namespace

AETHER_TEST(GhostShift_LoopbackHostValidatesVacuumAndPeersConverge) {
    RunVacuumSyncLab(4101, LinkConditions{});
}

AETHER_TEST(GhostShift_LoopbackVacuumConvergesUnderLatencyAndLoss) {
    RunVacuumSyncLab(4102, LinkConditions{0.10, 0.04, 0.10f, 0.02f}); // ~100 ms + jitter, 10% loss, 2% dupes
}
