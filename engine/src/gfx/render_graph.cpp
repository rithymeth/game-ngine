#include "aether/gfx/render_graph.h"

#include "aether/gfx/device.h"

#include <algorithm>

namespace aether::gfx {

namespace {
constexpr u32 kRtvHeapCapacity = 16;
constexpr u32 kDsvHeapCapacity = 16;
} // namespace

RenderGraph::RenderGraph(Device& device) : device_(device) {}

void RenderGraph::PassBuilder::AddUsage(ResourceHandle resource, D3D12_RESOURCE_STATES state) {
    graph_.passes_[pass_index_].usages.push_back({resource, state});
}

RenderGraph::ResourceHandle RenderGraph::ImportResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES initial_state,
                                                         const char* name) {
    auto it = resource_lookup_.find(resource);
    if (it != resource_lookup_.end()) {
        return it->second;
    }
    ResourceHandle handle = static_cast<ResourceHandle>(resources_.size());
    TrackedResource tracked;
    tracked.resource = resource;
    tracked.current_state = initial_state;
    tracked.name = name;
    resources_.push_back(std::move(tracked));
    resource_lookup_.emplace(resource, handle);
    return handle;
}

RenderGraph::ResourceHandle RenderGraph::CreateTransientTexture(const D3D12_RESOURCE_DESC& desc,
                                                                 D3D12_RESOURCE_STATES initial_state,
                                                                 const D3D12_CLEAR_VALUE* clear_value,
                                                                 const char* name) {
    auto it = transient_lookup_.find(name);
    ResourceHandle handle;
    if (it != transient_lookup_.end()) {
        handle = it->second;
        TrackedResource& tracked = resources_[handle];
        if (tracked.rtv_index != DescriptorHeap::kInvalidIndex) {
            rtv_heap_->Free(tracked.rtv_index);
            tracked.rtv_index = DescriptorHeap::kInvalidIndex;
        }
        if (tracked.dsv_index != DescriptorHeap::kInvalidIndex) {
            dsv_heap_->Free(tracked.dsv_index);
            tracked.dsv_index = DescriptorHeap::kInvalidIndex;
        }
        if (tracked.resource) {
            resource_lookup_.erase(tracked.resource);
        }
        tracked.owned_resource.Reset();
        tracked.resource = nullptr;

        // Drop its old bucket membership; AllocateTransientResources() will
        // reassign it (possibly to a different bucket, given the new desc).
        for (TransientBucket& bucket : transient_buckets_) {
            auto& members = bucket.resources_in_order;
            members.erase(std::remove(members.begin(), members.end(), handle), members.end());
        }
    } else {
        handle = static_cast<ResourceHandle>(resources_.size());
        resources_.push_back(TrackedResource{});
        resources_[handle].name = name;
        transient_lookup_.emplace(name, handle);
    }

    TrackedResource& tracked = resources_[handle];
    tracked.is_transient = true;
    tracked.needs_allocation = true;
    tracked.pending_desc = desc;
    tracked.pending_initial_state = initial_state;
    tracked.has_clear_value = (clear_value != nullptr);
    if (clear_value) {
        tracked.pending_clear = *clear_value;
    }

    transients_dirty_ = true;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderGraph::GetOrCreateRTV(ResourceHandle handle) {
    TrackedResource& tracked = resources_[handle];
    AETHER_ASSERT(tracked.resource != nullptr); // Execute() must have run (allocating it) before a pass can bind it
    if (tracked.rtv_index == DescriptorHeap::kInvalidIndex) {
        if (!rtv_heap_) {
            rtv_heap_ = std::make_unique<DescriptorHeap>(device_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapCapacity,
                                                          /*shader_visible=*/false);
        }
        tracked.rtv_index = rtv_heap_->Allocate();
        device_.Handle()->CreateRenderTargetView(tracked.resource, nullptr, rtv_heap_->CPUHandle(tracked.rtv_index));
    }
    return rtv_heap_->CPUHandle(tracked.rtv_index);
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderGraph::GetOrCreateDSV(ResourceHandle handle) {
    TrackedResource& tracked = resources_[handle];
    AETHER_ASSERT(tracked.resource != nullptr);
    if (tracked.dsv_index == DescriptorHeap::kInvalidIndex) {
        if (!dsv_heap_) {
            dsv_heap_ = std::make_unique<DescriptorHeap>(device_, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapCapacity,
                                                          /*shader_visible=*/false);
        }
        tracked.dsv_index = dsv_heap_->Allocate();
        device_.Handle()->CreateDepthStencilView(tracked.resource, nullptr, dsv_heap_->CPUHandle(tracked.dsv_index));
    }
    return dsv_heap_->CPUHandle(tracked.dsv_index);
}

bool RenderGraph::ShareHeapAllocation(ResourceHandle a, ResourceHandle b) const {
    for (const TransientBucket& bucket : transient_buckets_) {
        const auto& members = bucket.resources_in_order;
        bool has_a = std::find(members.begin(), members.end(), a) != members.end();
        bool has_b = std::find(members.begin(), members.end(), b) != members.end();
        if (has_a && has_b) {
            return true;
        }
    }
    return false;
}

void RenderGraph::AddPass(const char* name, SetupFunc setup, ExecuteFunc execute, QueueType queue) {
    usize index = passes_.size();
    passes_.push_back(Pass{name, std::move(execute), {}, queue, {}});
    PassBuilder builder(*this, index);
    setup(builder);
}

bool RenderGraph::HasComputePasses() const {
    for (const Pass& pass : passes_) {
        if (pass.queue == QueueType::Compute) {
            return true;
        }
    }
    return false;
}

std::vector<usize> RenderGraph::ComputeExecutionOrder(QueueType queue) const {
    // Filters passes_ down to one queue's, in registration order. An earlier
    // version of this function attempted an actual dependency-driven
    // topological sort (edges from whichever pass most recently touched a
    // resource to the next one that touches it), but building it surfaced a
    // real result worth recording rather than hiding: with only a resource
    // handle and a state per usage (no notion of a Write producing a new
    // "version" a Read can selectively bind to), every edge that
    // construction can produce necessarily goes from an earlier registration
    // index to a later one. Feeding that into Kahn's algorithm with
    // "smallest ready index first" as the tie-break is mathematically
    // guaranteed to reproduce exactly the original registration order for
    // any input — it can never reorder anything, and (since a cycle would
    // require a backward edge) can never detect one either. It was a
    // correctly-implemented no-op. Genuine dependency-driven reordering
    // needs resource versioning; that's real future work, not this.
    std::vector<usize> queue_passes;
    for (usize i = 0; i < passes_.size(); ++i) {
        if (passes_[i].queue == queue) {
            queue_passes.push_back(i);
        }
    }
    return queue_passes;
}

void RenderGraph::AllocateTransientResources() {
    std::vector<usize> graphics_order = ComputeExecutionOrder(QueueType::Graphics);
    std::vector<usize> compute_order = ComputeExecutionOrder(QueueType::Compute);

    struct Candidate {
        ResourceHandle handle = kInvalidHandle;
        usize first = static_cast<usize>(-1);
        usize last = 0;
        bool seen = false;
        bool cross_queue = false;
        u64 size = 0;
        u64 alignment = 0;
    };
    std::unordered_map<ResourceHandle, Candidate> candidates;

    auto scan = [&](const std::vector<usize>& order) {
        for (usize pos = 0; pos < order.size(); ++pos) {
            for (const Usage& usage : passes_[order[pos]].usages) {
                if (!resources_[usage.resource].is_transient) {
                    continue;
                }
                Candidate& c = candidates[usage.resource];
                if (!c.seen) {
                    c.handle = usage.resource;
                    c.first = pos;
                    c.last = pos;
                    c.seen = true;
                } else {
                    c.first = std::min(c.first, pos);
                    c.last = std::max(c.last, pos);
                }
            }
        }
    };
    scan(graphics_order);
    scan(compute_order);

    // A resource touched from both queues can't be given a simple same-queue
    // lifetime interval at all (their "positions" aren't comparable across
    // queues), so it's excluded from aliasing outright rather than risk an
    // incorrect overlap check.
    std::unordered_map<ResourceHandle, QueueType> first_queue_seen;
    for (const Pass& pass : passes_) {
        for (const Usage& usage : pass.usages) {
            if (!resources_[usage.resource].is_transient) {
                continue;
            }
            auto it = first_queue_seen.find(usage.resource);
            if (it == first_queue_seen.end()) {
                first_queue_seen[usage.resource] = pass.queue;
            } else if (it->second != pass.queue) {
                candidates[usage.resource].cross_queue = true;
            }
        }
    }

    for (auto& [handle, candidate] : candidates) {
        D3D12_RESOURCE_ALLOCATION_INFO info =
            device_.Handle()->GetResourceAllocationInfo(0, 1, &resources_[handle].pending_desc);
        candidate.size = info.SizeInBytes;
        candidate.alignment = info.Alignment;
    }

    // Release every existing transient allocation/view; everything gets
    // reassigned to a (possibly different) bucket below. Simpler and safer
    // than trying to preserve unaffected buckets, and this only runs when
    // transients_dirty_ (a resource was newly declared or resized) — not
    // every frame.
    transient_buckets_.clear();
    for (TrackedResource& tracked : resources_) {
        if (!tracked.is_transient) {
            continue;
        }
        if (tracked.rtv_index != DescriptorHeap::kInvalidIndex) {
            rtv_heap_->Free(tracked.rtv_index);
            tracked.rtv_index = DescriptorHeap::kInvalidIndex;
        }
        if (tracked.dsv_index != DescriptorHeap::kInvalidIndex) {
            dsv_heap_->Free(tracked.dsv_index);
            tracked.dsv_index = DescriptorHeap::kInvalidIndex;
        }
        if (tracked.resource) {
            resource_lookup_.erase(tracked.resource);
        }
        tracked.owned_resource.Reset();
        tracked.resource = nullptr;
    }

    // Greedy bucket assignment, largest-first (the classic first-fit-
    // decreasing heuristic): each resource goes into the first existing
    // bucket none of whose members' lifetimes overlap it, else a fresh one.
    struct BuildingBucket {
        u64 size = 0;
        u64 alignment = 0;
        std::vector<Candidate*> members;
    };
    std::vector<Candidate*> ordered;
    ordered.reserve(candidates.size());
    for (auto& [handle, candidate] : candidates) {
        ordered.push_back(&candidate);
    }
    std::sort(ordered.begin(), ordered.end(), [](const Candidate* a, const Candidate* b) { return a->size > b->size; });

    std::vector<BuildingBucket> building;
    for (Candidate* c : ordered) {
        if (c->cross_queue) {
            BuildingBucket solo;
            solo.size = c->size;
            solo.alignment = c->alignment;
            solo.members.push_back(c);
            building.push_back(std::move(solo));
            continue;
        }

        bool placed = false;
        for (BuildingBucket& bucket : building) {
            bool overlaps = false;
            for (Candidate* existing : bucket.members) {
                if (existing->cross_queue || (c->first <= existing->last && existing->first <= c->last)) {
                    overlaps = true;
                    break;
                }
            }
            if (!overlaps) {
                bucket.members.push_back(c);
                bucket.size = std::max(bucket.size, c->size);
                bucket.alignment = std::max(bucket.alignment, c->alignment);
                placed = true;
                break;
            }
        }
        if (!placed) {
            BuildingBucket fresh;
            fresh.size = c->size;
            fresh.alignment = c->alignment;
            fresh.members.push_back(c);
            building.push_back(std::move(fresh));
        }
    }

    // Materialize: one ID3D12Heap per bucket, one CreatePlacedResource per
    // member at offset 0 (only one member is ever "live" in the heap's
    // memory at a time within a frame, by construction of the overlap
    // check above).
    for (BuildingBucket& bucket : building) {
        u64 alignment = bucket.alignment != 0 ? bucket.alignment : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

        D3D12_HEAP_DESC heap_desc{};
        heap_desc.SizeInBytes = AlignUp(bucket.size, alignment);
        heap_desc.Alignment = alignment;
        heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;

        TransientBucket tracked_bucket;
        AETHER_D3D_CHECK(device_.Handle()->CreateHeap(&heap_desc, IID_PPV_ARGS(&tracked_bucket.heap)));
        tracked_bucket.size = heap_desc.SizeInBytes;

        std::sort(bucket.members.begin(), bucket.members.end(),
                  [](const Candidate* a, const Candidate* b) { return a->first < b->first; });

        for (Candidate* member : bucket.members) {
            TrackedResource& tracked = resources_[member->handle];
            ComPtr<ID3D12Resource> resource;
            AETHER_D3D_CHECK(device_.Handle()->CreatePlacedResource(
                tracked_bucket.heap.Get(), 0, &tracked.pending_desc, tracked.pending_initial_state,
                tracked.has_clear_value ? &tracked.pending_clear : nullptr, IID_PPV_ARGS(&resource)));

            tracked.owned_resource = resource;
            tracked.resource = resource.Get();
            tracked.current_state = tracked.pending_initial_state;
            tracked.needs_allocation = false;
            resource_lookup_[tracked.resource] = member->handle;
            tracked_bucket.resources_in_order.push_back(member->handle);
        }

        transient_buckets_.push_back(std::move(tracked_bucket));
    }
}

void RenderGraph::AssignAliasingBarriers() {
    for (Pass& pass : passes_) {
        pass.aliasing_barriers.clear();
    }
    if (transient_buckets_.empty()) {
        return;
    }

    std::vector<usize> graphics_order = ComputeExecutionOrder(QueueType::Graphics);
    std::vector<usize> compute_order = ComputeExecutionOrder(QueueType::Compute);

    auto find_first_use = [&](ResourceHandle handle) -> usize {
        for (usize global_index : graphics_order) {
            for (const Usage& usage : passes_[global_index].usages) {
                if (usage.resource == handle) {
                    return global_index;
                }
            }
        }
        for (usize global_index : compute_order) {
            for (const Usage& usage : passes_[global_index].usages) {
                if (usage.resource == handle) {
                    return global_index;
                }
            }
        }
        return static_cast<usize>(-1);
    };

    for (TransientBucket& bucket : transient_buckets_) {
        if (bucket.resources_in_order.size() < 2) {
            continue; // sole occupant of its heap this "epoch"; nothing to alias against
        }
        ID3D12Resource* previous = nullptr;
        for (ResourceHandle handle : bucket.resources_in_order) {
            usize global_index = find_first_use(handle);
            if (global_index == static_cast<usize>(-1)) {
                continue; // not actually used this frame
            }
            if (previous != nullptr) {
                passes_[global_index].aliasing_barriers.push_back({previous, resources_[handle].resource});
            }
            previous = resources_[handle].resource;
        }
    }
}

void RenderGraph::Compile() {
    if (transients_dirty_) {
        AllocateTransientResources();
        transients_dirty_ = false;
    }
    AssignAliasingBarriers();
}

void RenderGraph::ExecuteQueue(const std::vector<usize>& order, ID3D12GraphicsCommandList* cmd) {
    for (usize global_index : order) {
        Pass& pass = passes_[global_index];
        AETHER_ASSERT(cmd != nullptr);

        if (!pass.aliasing_barriers.empty()) {
            std::vector<D3D12_RESOURCE_BARRIER> alias_barriers;
            alias_barriers.reserve(pass.aliasing_barriers.size());
            for (const AliasingBarrier& ab : pass.aliasing_barriers) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
                barrier.Aliasing.pResourceBefore = ab.before;
                barrier.Aliasing.pResourceAfter = ab.after;
                alias_barriers.push_back(barrier);
            }
            cmd->ResourceBarrier(static_cast<UINT>(alias_barriers.size()), alias_barriers.data());
        }

        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (const Usage& usage : pass.usages) {
            TrackedResource& tracked = resources_[usage.resource];
            if (tracked.current_state != usage.state) {
                barriers.push_back(TransitionBarrier(tracked.resource, tracked.current_state, usage.state));
                tracked.current_state = usage.state;
            }
        }
        if (!barriers.empty()) {
            cmd->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }

        pass.execute(cmd);
    }
}

void RenderGraph::Execute(ID3D12GraphicsCommandList* graphics_cmd, ID3D12GraphicsCommandList* compute_cmd) {
    Compile();
    ExecuteQueue(ComputeExecutionOrder(QueueType::Graphics), graphics_cmd);
    ExecuteQueue(ComputeExecutionOrder(QueueType::Compute), compute_cmd);
}

} // namespace aether::gfx
