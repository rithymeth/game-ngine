#include "aether/renderer/frame_graph.h"
#include "test_framework.h"

#include <algorithm>
#include <random>

using namespace aether;

// Phase 14 step 3: frame graph planning (order, culling, barriers, queues,
// lifetimes, transient memory aliasing), independent of the GPU API.

namespace {

bool HasBarrier(const std::vector<Barrier>& barriers, ResourceId r, Access before, Access after) {
    return std::any_of(barriers.begin(), barriers.end(),
                       [&](const Barrier& b) { return b.resource == r && b.before == before && b.after == after; });
}

bool Overlaps(const Placement& a, const Placement& b) { return a.offset < b.offset + b.size && b.offset < a.offset + a.size; }

} // namespace

AETHER_TEST(FrameGraph_PlansAForwardPlusFrame) {
    FrameGraph g;
    const ResourceId backbuffer = g.Import("backbuffer", Access::Present, Access::Present);
    const ResourceId shadow = g.CreateTexture("shadow map", {2048, 2048, 1, 1, Format::D32});
    const ResourceId depth = g.CreateTexture("depth", {1920, 1080, 1, 1, Format::D32});
    const ResourceId clusters = g.CreateBuffer("light clusters", 1 << 20);
    const ResourceId hdr = g.CreateTexture("hdr", {1920, 1080, 1, 1, Format::RGBA16F});
    const ResourceId bloom = g.CreateTexture("bloom", {960, 540, 5, 1, Format::RGBA16F});
    const ResourceId debug = g.CreateTexture("debug view", {512, 512, 1, 1, Format::RGBA8});

    const PassId shadows = g.AddPass("Shadows", Queue::Graphics, [&](FrameGraph::PassBuilder& p) { p.Write(shadow, Access::DepthWrite); });
    const PassId prepass = g.AddPass("Depth prepass", Queue::Graphics, [&](FrameGraph::PassBuilder& p) { p.Write(depth, Access::DepthWrite); });
    const PassId cull = g.AddPass("Light culling", Queue::Compute, [&](FrameGraph::PassBuilder& p) {
        p.Read(depth, Access::ShaderRead);
        p.Write(clusters, Access::ShaderWrite);
    });
    const PassId forward = g.AddPass("Forward", Queue::Graphics, [&](FrameGraph::PassBuilder& p) {
        p.Read(shadow, Access::ShaderRead);
        p.Read(depth, Access::DepthRead);
        p.Read(clusters, Access::ShaderRead);
        p.Write(hdr, Access::ColorTarget);
    });
    const PassId debug_pass = g.AddPass("Debug view", Queue::Graphics, [&](FrameGraph::PassBuilder& p) {
        p.Read(depth, Access::ShaderRead);
        p.Write(debug, Access::ColorTarget); // nobody reads it: culled
    });
    const PassId bloom_pass = g.AddPass("Bloom", Queue::Compute, [&](FrameGraph::PassBuilder& p) {
        p.Read(hdr, Access::ShaderRead);
        p.Write(bloom, Access::ShaderWrite);
    });
    const PassId tonemap = g.AddPass("Tone map", Queue::Graphics, [&](FrameGraph::PassBuilder& p) {
        p.Read(hdr, Access::ShaderRead);
        p.Read(bloom, Access::ShaderRead);
        p.Write(backbuffer, Access::ColorTarget);
    });
    std::string error;
    AETHER_CHECK(g.Compile(&error));
    AETHER_CHECK(g.Validate(&error));

    // Order and culling.
    AETHER_CHECK(g.Order() == (std::vector<PassId>{shadows, prepass, cull, forward, bloom_pass, tonemap}));
    AETHER_CHECK(g.IsCulled(debug_pass) && !g.IsCulled(forward));

    // Barriers.
    AETHER_CHECK(HasBarrier(g.BarriersBefore(shadows), shadow, Access::Undefined, Access::DepthWrite));
    AETHER_CHECK(HasBarrier(g.BarriersBefore(cull), depth, Access::DepthWrite, Access::ShaderRead));
    AETHER_CHECK(HasBarrier(g.BarriersBefore(forward), shadow, Access::DepthWrite, Access::ShaderRead));
    AETHER_CHECK(HasBarrier(g.BarriersBefore(forward), depth, Access::ShaderRead, Access::DepthRead));
    AETHER_CHECK(HasBarrier(g.BarriersBefore(forward), clusters, Access::ShaderWrite, Access::ShaderRead));
    AETHER_CHECK(HasBarrier(g.BarriersBefore(tonemap), backbuffer, Access::Present, Access::ColorTarget));
    AETHER_CHECK(HasBarrier(g.BarriersBefore(tonemap), hdr, Access::ShaderRead, Access::ShaderRead) == false); // already there
    AETHER_CHECK(g.FinalBarriers().size() == 1 && HasBarrier(g.FinalBarriers(), backbuffer, Access::ColorTarget, Access::Present));

    // Queues: each compute pass waits for its graphics inputs, and back.
    AETHER_CHECK(g.Waits(cull) == std::vector<PassId>{prepass});
    AETHER_CHECK(g.Waits(forward) == std::vector<PassId>{cull});
    AETHER_CHECK(g.Waits(bloom_pass) == std::vector<PassId>{forward});
    AETHER_CHECK(g.Waits(tonemap) == std::vector<PassId>{bloom_pass});
    AETHER_CHECK(g.Waits(prepass).empty());

    // Lifetimes (positions in the order) and memory.
    u32 first, last;
    AETHER_CHECK(g.Lifetime(shadow, first, last) && first == 0 && last == 3);
    AETHER_CHECK(g.Lifetime(bloom, first, last) && first == 4 && last == 5);
    AETHER_CHECK(!g.Lifetime(debug, first, last));  // its pass was culled
    AETHER_CHECK(!g.Lifetime(backbuffer, first, last)); // imported
    AETHER_CHECK(g.PlacementOf(shadow).size == 16u * 1024 * 1024);
    // Bloom starts after the shadow map's last use, so it reuses that memory.
    AETHER_CHECK(Overlaps(g.PlacementOf(bloom), g.PlacementOf(shadow)));
    AETHER_CHECK(std::any_of(g.BarriersBefore(bloom_pass).begin(), g.BarriersBefore(bloom_pass).end(),
                             [&](const Barrier& b) { return b.resource == bloom && b.aliasing; }));
    AETHER_CHECK(g.HeapSize() < g.UnaliasedSize());
    for (const ResourceId a : {shadow, depth, clusters, hdr, bloom}) {
        for (const ResourceId b : {shadow, depth, clusters, hdr, bloom}) {
            if (a >= b) continue;
            u32 fa, la, fb, lb;
            g.Lifetime(a, fa, la);
            g.Lifetime(b, fb, lb);
            if (fa <= lb && fb <= la) AETHER_CHECK(!Overlaps(g.PlacementOf(a), g.PlacementOf(b)));
        }
    }
    AETHER_CHECK(g.PlacementOf(hdr).offset % FrameGraph::kPlacementAlignment == 0);
}

AETHER_TEST(FrameGraph_UavBarriersSideEffectsAndErrors) {
    {
        // Two compute passes writing the same buffer need a UAV barrier between them.
        FrameGraph g;
        const ResourceId particles = g.CreateBuffer("particles", 4096);
        const ResourceId readback = g.Import("readback", Access::CopyDest, Access::CopyDest);
        const PassId emit = g.AddPass("Emit", Queue::Compute, [&](FrameGraph::PassBuilder& p) { p.Write(particles, Access::ShaderWrite); });
        const PassId simulate = g.AddPass("Simulate", Queue::Compute, [&](FrameGraph::PassBuilder& p) {
            p.Read(particles, Access::ShaderWrite); // read-modify-write
            p.Write(particles, Access::ShaderWrite);
        });
        const PassId capture = g.AddPass("Capture", Queue::Graphics, [&](FrameGraph::PassBuilder& p) {
            p.Read(particles, Access::CopySource);
            p.Read(readback, Access::CopyDest); // reading an import isn't an output...
            p.SideEffect();                      // ...but a side effect keeps it
        });
        AETHER_CHECK(g.Compile());
        AETHER_CHECK(g.Validate());
        AETHER_CHECK(g.Order() == (std::vector<PassId>{emit, simulate, capture}));
        AETHER_CHECK(HasBarrier(g.BarriersBefore(simulate), particles, Access::ShaderWrite, Access::ShaderWrite));
        AETHER_CHECK(HasBarrier(g.BarriersBefore(capture), particles, Access::ShaderWrite, Access::CopySource));
        AETHER_CHECK(g.Waits(capture) == std::vector<PassId>{simulate});
    }
    {
        // Nothing needed: everything is culled.
        FrameGraph g;
        const ResourceId t = g.CreateTexture("t", {64, 64});
        g.AddPass("Orphan", Queue::Graphics, [&](FrameGraph::PassBuilder& p) { p.Write(t, Access::ColorTarget); });
        AETHER_CHECK(g.Compile() && g.Order().empty() && g.HeapSize() == 0);
        AETHER_CHECK(g.Validate());
    }
    std::string error;
    {
        FrameGraph g;
        const ResourceId t = g.CreateTexture("gbuffer", {64, 64});
        const ResourceId out = g.Import("out", Access::Present, Access::Present);
        g.AddPass("Lighting", Queue::Graphics, [&](FrameGraph::PassBuilder& p) {
            p.Read(t, Access::ShaderRead);
            p.Write(out, Access::ColorTarget);
        });
        AETHER_CHECK(!g.Compile(&error) && error.find("before anything writes it") != std::string::npos);
        AETHER_CHECK(!g.Validate(&error) && error.find("has not been compiled") != std::string::npos);
    }
    {
        FrameGraph g;
        const ResourceId t = g.CreateTexture("t", {64, 64});
        g.AddPass("Both", Queue::Graphics, [&](FrameGraph::PassBuilder& p) {
            p.Write(t, Access::ColorTarget);
            p.Read(t, Access::ShaderRead);
        });
        AETHER_CHECK(!g.Compile(&error) && error.find("both") != std::string::npos);
    }
}

AETHER_TEST(FrameGraph_RandomGraphsKeepTheirInvariants) {
    std::mt19937 rng(11);
    for (int round = 0; round < 200; ++round) {
        FrameGraph g;
        const int resource_count = 3 + static_cast<int>(rng() % 8);
        std::vector<ResourceId> resources;
        std::uniform_int_distribution<u32> dim(16, 1024);
        for (int i = 0; i < resource_count; ++i) resources.push_back(g.CreateTexture("t" + std::to_string(i), {dim(rng), dim(rng)}));
        const ResourceId out = g.Import("out", Access::Present, Access::Present);
        std::vector<bool> written(resources.size(), false);
        const int pass_count = 4 + static_cast<int>(rng() % 10);
        std::vector<std::vector<ResourceId>> reads(pass_count);
        for (int p = 0; p < pass_count; ++p) {
            g.AddPass("p" + std::to_string(p), rng() % 3 == 0 ? Queue::Compute : Queue::Graphics, [&](FrameGraph::PassBuilder& b) {
                std::vector<bool> used(resources.size(), false);
                for (int k = 0; k < 2; ++k) {
                    const usize r = rng() % resources.size();
                    if (written[r] && !used[r]) {
                        b.Read(resources[r], Access::ShaderRead);
                        reads[p].push_back(resources[r]);
                        used[r] = true;
                    }
                }
                const usize w = rng() % resources.size();
                if (!used[w]) {
                    b.Write(resources[w], Access::ColorTarget);
                    written[w] = true;
                }
                if (p == pass_count - 1 || rng() % 5 == 0) b.Write(out, Access::ColorTarget);
            });
        }
        std::string error;
        AETHER_CHECK(g.Compile(&error));
        AETHER_CHECK(g.Validate(&error));
        // Resources alive at the same time never share memory.
        for (usize a = 0; a < resources.size(); ++a) {
            for (usize b = a + 1; b < resources.size(); ++b) {
                u32 fa, la, fb, lb;
                if (!g.Lifetime(resources[a], fa, la) || !g.Lifetime(resources[b], fb, lb)) continue;
                if (fa <= lb && fb <= la) AETHER_CHECK(!Overlaps(g.PlacementOf(resources[a]), g.PlacementOf(resources[b])));
            }
        }
        AETHER_CHECK(g.HeapSize() <= g.UnaliasedSize());
        // The last pass writes the output, so it always runs; order is increasing.
        AETHER_CHECK(!g.Order().empty() && g.Order().back() == static_cast<PassId>(pass_count - 1));
        AETHER_CHECK(std::is_sorted(g.Order().begin(), g.Order().end()));
        // A running pass's inputs are always produced by running passes: every
        // resource it reads has a lifetime starting no later than the pass.
        for (u32 pos = 0; pos < g.Order().size(); ++pos) {
            for (ResourceId r : reads[g.Order()[pos]]) {
                u32 first, last;
                AETHER_CHECK(g.Lifetime(r, first, last) && first < pos);
            }
        }
    }
}
