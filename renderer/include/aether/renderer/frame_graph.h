#pragma once

#include "aether/core/base.h"

#include <functional>
#include <string>
#include <vector>

namespace aether {

// The frame graph's planning (Phase 14 step 3), independent of D3D12 and
// Vulkan. Passes declare what they read and write; Compile() works out
// everything the backends then just follow:
// - which passes run (a pass nobody needs is culled) and in what order;
// - the barriers before each pass (and the final ones for imported
//   resources), including UAV barriers between writes;
// - which passes on one queue wait for the other queue;
// - each transient resource's lifetime, and where it lives in one shared
//   heap, so resources that are never alive at the same time share memory.
//
// Passes run in the order they were added: a pass can only depend on
// earlier ones, so that order is always valid, and it's what the author sees.

enum class Access : u8 {
    Undefined,    // no contents yet (a transient's first use)
    ColorTarget,
    DepthWrite,
    DepthRead,
    ShaderRead,   // sampled / read in any shader stage
    ShaderWrite,  // storage image / UAV
    CopySource,
    CopyDest,
    IndirectArgs,
    Present,
};
const char* AccessName(Access access);
bool IsWrite(Access access);

enum class Queue : u8 { Graphics, Compute };

enum class Format : u8 { RGBA8, RGBA16F, RGBA32F, R11G11B10F, RG16F, R32F, D32, D24S8 };
u32 BytesPerPixel(Format format);

struct TextureDesc {
    u32 width = 1, height = 1;
    u32 mips = 1, layers = 1;
    Format format = Format::RGBA8;
};

using ResourceId = u32;
using PassId = u32;

struct Barrier {
    ResourceId resource = 0;
    Access before = Access::Undefined, after = Access::Undefined;
    bool aliasing = false; // the first use of memory another resource used earlier this frame
};

struct Placement {
    u64 offset = 0, size = 0; // in the transient heap
};

class FrameGraph {
public:
    class PassBuilder {
    public:
        void Read(ResourceId resource, Access access);
        void Write(ResourceId resource, Access access);
        void SideEffect() { side_effect_ = true; } // never culled (e.g. readback, debug capture)

    private:
        friend class FrameGraph;
        struct Use {
            ResourceId resource;
            Access access;
            bool write;
        };
        std::vector<Use> uses_;
        bool side_effect_ = false;
    };

    // Transient resources live only during the frame; the graph places them.
    ResourceId CreateTexture(const std::string& name, const TextureDesc& desc);
    ResourceId CreateBuffer(const std::string& name, u64 size);
    // An outside resource (the swap chain image, a history buffer) in
    // `initial` access, left in `final` at the end of the frame. Writing an
    // imported resource makes the writer needed (it's a frame output).
    ResourceId Import(const std::string& name, Access initial, Access final_access);

    PassId AddPass(const std::string& name, Queue queue, const std::function<void(PassBuilder&)>& setup);

    // False with a message for a malformed graph: a transient read before
    // anything writes it, or one pass using a resource two different ways.
    bool Compile(std::string* error = nullptr);
    // Checks Compile()'s plan (pass culling, hazards, transient lifetimes and
    // placements). Useful for debug builds, tests and graph tooling.
    bool Validate(std::string* error = nullptr) const;

    // --- Results (valid after Compile) ------------------------------------------
    const std::vector<PassId>& Order() const { return order_; } // the passes that run
    bool IsCulled(PassId pass) const { return !passes_[pass].needed; }
    const std::vector<Barrier>& BarriersBefore(PassId pass) const { return passes_[pass].barriers; }
    const std::vector<Barrier>& FinalBarriers() const { return final_barriers_; }
    // Passes on the other queue this pass must wait for.
    const std::vector<PassId>& Waits(PassId pass) const { return passes_[pass].waits; }
    // Positions in Order() of a transient resource's first and last use; false if unused.
    bool Lifetime(ResourceId resource, u32& first, u32& last) const;
    Placement PlacementOf(ResourceId resource) const { return resources_[resource].placement; }
    u64 HeapSize() const { return heap_size_; }
    u64 UnaliasedSize() const; // what the used transients would take without sharing
    const std::string& Name(ResourceId resource) const { return resources_[resource].name; }
    const std::string& PassName(PassId pass) const { return passes_[pass].name; }

    static constexpr u64 kPlacementAlignment = 64 * 1024; // D3D12's default placement alignment

private:
    struct Resource {
        std::string name;
        bool imported = false;
        TextureDesc texture;
        bool is_buffer = false;
        u64 size = 0;
        Access initial = Access::Undefined, final_access = Access::Undefined;
        // Compile results
        bool used = false;
        u32 first = 0, last = 0;
        Placement placement;
    };
    struct Pass {
        std::string name;
        Queue queue = Queue::Graphics;
        std::vector<PassBuilder::Use> uses;
        bool side_effect = false;
        bool needed = false;
        std::vector<PassId> depends; // earlier passes this one needs
        std::vector<Barrier> barriers;
        std::vector<PassId> waits;
    };
    void PlaceTransients();

    std::vector<Resource> resources_;
    std::vector<Pass> passes_;
    std::vector<PassId> order_;
    std::vector<Barrier> final_barriers_;
    u64 heap_size_ = 0;
    bool compiled_ = false;
};

} // namespace aether
