// aether_alloc_audit (Phase 38 step 7): runs hot paths under an allocation counter and fails if one that should
// be allocation-free in steady state allocates. It replaces global operator new for this program only, so the
// counts are real; run it as a ctest (alloc_audit), or by hand to see every figure.
//
// A path goes in `kMustNotAllocate` once it has been shown clean; a path that allocates by design (building
// something) goes in `kAllocatesByDesign` so the figure stays visible and the counter itself stays checked.

#include "aether/ai/behavior_tree.h"
#include "aether/ai/blackboard.h"
#include "aether/ai/bt_runtime.h"
#include "aether/ai/state_tree.h"
#include "aether/ai/utility.h"
#include "aether/core/alloc_track.h"
#include "aether/ecs/world.h"
#include "aether/nav/navmesh.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <cstdio>
#ifdef _WIN32
#include <malloc.h>
#endif
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>

void* operator new(std::size_t size) {
    aether::alloc_track::OnAllocation(size);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    aether::alloc_track::OnAllocation(size);
    const std::size_t align = static_cast<std::size_t>(alignment) < sizeof(void*) ? sizeof(void*) : static_cast<std::size_t>(alignment);
#ifdef _WIN32
    void* p = _aligned_malloc(size ? size : 1, align);
    if (p == nullptr) throw std::bad_alloc();
#else
    void* p = nullptr;
    if (posix_memalign(&p, align, size ? size : 1) != 0) throw std::bad_alloc();
#endif
    return p;
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return operator new(size, alignment); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#ifdef _WIN32
// Memory from _aligned_malloc must go back through _aligned_free.
void operator delete(void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
#else
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
#endif

using namespace aether;

namespace {

struct Velocity {
    Vec3 value;
};

struct Case {
    const char* name;
    bool must_not_allocate;
    std::function<void()> setup;  // once, outside the count, so caches and capacity are warm
    std::function<void()> frame;  // the path, run several times under the count
};

int g_failures = 0;

void Run(const Case& c) {
    if (c.setup) c.setup();
    c.frame();  // a first frame outside the count warms anything lazy
    u64 worst_count = 0, worst_bytes = 0;
    for (int i = 0; i < 8; ++i) {
        AllocScope scope;
        c.frame();
        worst_count = std::max<u64>(worst_count, scope.Count());
        worst_bytes = std::max<u64>(worst_bytes, scope.Bytes());
    }
    const bool ok = c.must_not_allocate ? worst_count == 0 : worst_count > 0;
    std::printf("%-4s %-46s %6llu allocations, %8llu bytes per frame (%s)\n", ok ? "ok" : "FAIL", c.name,
                static_cast<unsigned long long>(worst_count), static_cast<unsigned long long>(worst_bytes),
                c.must_not_allocate ? "must be 0" : "allocates by design");
    if (!ok) ++g_failures;
}

} // namespace

int main() {
    World world;
    ai::Blackboard blackboard;
    ai::UtilityBrain brain;
    ai::StateTree tree;
    std::vector<Entity> made;
    ai::BehaviorTreeAsset bt_asset;
    std::unique_ptr<ai::BehaviorTreeInstance> bt;
    ai::Blackboard bt_board;
    ai::BtHooks bt_hooks;
    nav::NavMesh navmesh;
    nav::NavPath nav_path;

    const std::vector<Case> cases = {
        {"ecs: iterate Transform+Velocity over 10k entities", true,
         [&] {
             for (int i = 0; i < 10000; ++i) world.CreateEntity(Transform{}, Velocity{Vec3(1, 0, 0)});
         },
         [&] { world.ForEach<Transform, Velocity>([](Transform& t, Velocity& v) { t.position = t.position + v.value * 0.016f; }); }},

        {"blackboard: set and read existing numeric keys", true,
         [&] {
             (void)blackboard.Set("danger", 0.5f);
             (void)blackboard.Set("hurt", 3);
         },
         [&] {
             for (int i = 0; i < 100; ++i) {
                 (void)blackboard.Set("danger", 0.1f * static_cast<f32>(i % 10));
                 f64 v = 0;
                 (void)blackboard.GetNumber("hurt", v);
             }
         }},

        {"utility AI: choose among three actions", true,
         [&] {
             ai::Consideration danger;
             danger.input = "danger";
             ai::Consideration hurt;
             hurt.input = "hurt";
             hurt.max = 10.0f;
             brain.Add({"flee", 1.0f, {danger}});
             brain.Add({"heal", 1.0f, {hurt}});
             brain.Add({"idle", 0.2f, {}});
         },
         [&] {
             for (int i = 0; i < 100; ++i) (void)brain.Choose(blackboard);
         }},

        {"state tree: tick with no transition ready", true,
         [&] {
             tree.Add({"Root", "", {}, {}, {}});
             tree.Add({"Patrol", "Root", {{{{"seen", ai::CompareOp::Equal, true}}, "Chase"}}, {}, {}});
             tree.Add({"Chase", "Root", {}, {}, {}});
             (void)blackboard.Set("seen", false);
             (void)tree.Start(blackboard);
         },
         [&] {
             for (int i = 0; i < 100; ++i) (void)tree.Tick(blackboard);
         }},

        {"behavior tree: tick a running wait", true,
         [&] {
             bt_asset.root.type = ai::BtNodeType::Sequence;
             ai::BtNode wait;
             wait.type = ai::BtNodeType::Wait;
             wait.seconds = 1e6f;
             bt_asset.root.children.push_back(wait);
             bt = std::make_unique<ai::BehaviorTreeInstance>(bt_asset);
         },
         [&] {
             ai::BtContext ctx{nullptr, kNullEntity, &bt_board, &bt_hooks};
             for (int i = 0; i < 100; ++i) (void)bt->Tick(ctx, 0.016f);
         }},

        {"nav: nearest point", true,
         [&] {
             nav::NavGeometry g;
             g.AddPlane(Vec3(0, 0, 0), 30, 30);
             nav::NavMeshData data;
             if (!nav::BuildNavMesh(g, nav::NavMeshSettings{}, data) || !navmesh.Load(data)) std::abort();
         },
         [&] {
             Vec3 p;
             for (int i = 0; i < 20; ++i) (void)navmesh.NearestPoint(Vec3(1, 0, 1), p);
         }},
        {"nav: raycast", true, nullptr,
         [&] {
             Vec3 hit;
             for (int i = 0; i < 20; ++i) (void)navmesh.Raycast(Vec3(-20, 0, -20), Vec3(20, 0, 20), hit);
         }},
        {"nav: find path into a reused NavPath", true, nullptr,
         [&] {
             for (int i = 0; i < 20; ++i) (void)navmesh.FindPath(Vec3(-20, 0, -20), Vec3(20, 0, 20), nav_path);
         }},

        // The counter itself: these allocate, so a figure of 0 would mean the hook is not in place.
        {"counter check: create 100 entities", false, nullptr,
         [&] {
             for (int i = 0; i < 100; ++i) made.push_back(world.CreateEntity(Transform{}));
         }},
        {"counter check: build a string", false, nullptr,
         [] {
             std::string s(200, 'x');
             s += "more text that does not fit in the small-string buffer at all";
             volatile char c = s[10];
             (void)c;
         }},
    };
    for (const Case& c : cases) Run(c);
    std::printf("%s\n", g_failures == 0 ? "alloc audit passed" : "alloc audit FAILED");
    return g_failures == 0 ? 0 : 1;
}
