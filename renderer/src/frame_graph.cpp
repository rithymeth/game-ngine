#include "aether/renderer/frame_graph.h"

#include <algorithm>

namespace aether {

const char* AccessName(Access access) {
    switch (access) {
    case Access::Undefined: return "Undefined";
    case Access::ColorTarget: return "ColorTarget";
    case Access::DepthWrite: return "DepthWrite";
    case Access::DepthRead: return "DepthRead";
    case Access::ShaderRead: return "ShaderRead";
    case Access::ShaderWrite: return "ShaderWrite";
    case Access::CopySource: return "CopySource";
    case Access::CopyDest: return "CopyDest";
    case Access::IndirectArgs: return "IndirectArgs";
    case Access::Present: return "Present";
    }
    return "?";
}

bool IsWrite(Access access) {
    return access == Access::ColorTarget || access == Access::DepthWrite || access == Access::ShaderWrite || access == Access::CopyDest;
}

u32 BytesPerPixel(Format format) {
    switch (format) {
    case Format::RGBA8: return 4;
    case Format::RGBA16F: return 8;
    case Format::RGBA32F: return 16;
    case Format::R11G11B10F: return 4;
    case Format::RG16F: return 4;
    case Format::R32F: return 4;
    case Format::D32: return 4;
    case Format::D24S8: return 4;
    }
    return 4;
}

void FrameGraph::PassBuilder::Read(ResourceId resource, Access access) { uses_.push_back({resource, access, false}); }
void FrameGraph::PassBuilder::Write(ResourceId resource, Access access) { uses_.push_back({resource, access, true}); }

ResourceId FrameGraph::CreateTexture(const std::string& name, const TextureDesc& desc) {
    Resource r;
    r.name = name;
    r.texture = desc;
    u64 bytes = 0;
    for (u32 m = 0; m < std::max(desc.mips, 1u); ++m) {
        bytes += static_cast<u64>(std::max(1u, desc.width >> m)) * std::max(1u, desc.height >> m) * BytesPerPixel(desc.format);
    }
    r.size = bytes * std::max(desc.layers, 1u);
    resources_.push_back(r);
    return static_cast<ResourceId>(resources_.size() - 1);
}

ResourceId FrameGraph::CreateBuffer(const std::string& name, u64 size) {
    Resource r;
    r.name = name;
    r.is_buffer = true;
    r.size = size;
    resources_.push_back(r);
    return static_cast<ResourceId>(resources_.size() - 1);
}

ResourceId FrameGraph::Import(const std::string& name, Access initial, Access final_access) {
    Resource r;
    r.name = name;
    r.imported = true;
    r.initial = initial;
    r.final_access = final_access;
    resources_.push_back(r);
    return static_cast<ResourceId>(resources_.size() - 1);
}

PassId FrameGraph::AddPass(const std::string& name, Queue queue, const std::function<void(PassBuilder&)>& setup) {
    PassBuilder builder;
    if (setup) setup(builder);
    Pass p;
    p.name = name;
    p.queue = queue;
    p.uses = std::move(builder.uses_);
    p.side_effect = builder.side_effect_;
    passes_.push_back(std::move(p));
    return static_cast<PassId>(passes_.size() - 1);
}

bool FrameGraph::Lifetime(ResourceId resource, u32& first, u32& last) const {
    const Resource& r = resources_[resource];
    if (r.imported || !r.used) return false;
    first = r.first;
    last = r.last;
    return true;
}

u64 FrameGraph::UnaliasedSize() const {
    u64 total = 0;
    for (const Resource& r : resources_) {
        if (!r.imported && r.used) total += (r.size + kPlacementAlignment - 1) / kPlacementAlignment * kPlacementAlignment;
    }
    return total;
}

bool FrameGraph::Compile(std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    order_.clear();
    final_barriers_.clear();
    heap_size_ = 0;
    for (Resource& r : resources_) {
        r.used = false;
        r.placement = {};
    }

    // 1. Check each pass's uses, and merge repeats (a read and a write of the
    //    same access count as one write).
    for (Pass& p : passes_) {
        p.needed = false;
        p.depends.clear();
        p.barriers.clear();
        p.waits.clear();
        std::vector<PassBuilder::Use> merged;
        for (const PassBuilder::Use& u : p.uses) {
            if (u.resource >= resources_.size()) return fail("pass '" + p.name + "' uses an unknown resource");
            auto same = std::find_if(merged.begin(), merged.end(), [&](const PassBuilder::Use& m) { return m.resource == u.resource; });
            if (same == merged.end()) {
                merged.push_back(u);
            } else if (same->access != u.access) {
                return fail("pass '" + p.name + "' uses '" + resources_[u.resource].name + "' as both " + AccessName(same->access) +
                            " and " + AccessName(u.access));
            } else {
                same->write = same->write || u.write;
            }
        }
        p.uses = std::move(merged);
    }

    // 2. Dependencies. Data dependencies (read after write, write after
    //    write) decide culling; write-after-read ones only order passes.
    const usize n = passes_.size();
    std::vector<std::vector<PassId>> data_deps(n);
    std::vector<i64> last_writer(resources_.size(), -1);
    std::vector<std::vector<PassId>> readers(resources_.size());
    for (PassId p = 0; p < n; ++p) {
        Pass& pass = passes_[p];
        for (const PassBuilder::Use& u : pass.uses) {
            const i64 writer = last_writer[u.resource];
            if (!u.write && writer < 0 && !resources_[u.resource].imported) {
                return fail("pass '" + pass.name + "' reads '" + resources_[u.resource].name + "' before anything writes it");
            }
            if (writer >= 0) {
                data_deps[p].push_back(static_cast<PassId>(writer));
                pass.depends.push_back(static_cast<PassId>(writer));
            }
            if (u.write) {
                for (PassId r : readers[u.resource]) {
                    if (r != p) pass.depends.push_back(r);
                }
            }
        }
        for (const PassBuilder::Use& u : pass.uses) {
            if (u.write) {
                last_writer[u.resource] = p;
                readers[u.resource].clear();
            } else {
                readers[u.resource].push_back(p);
            }
        }
    }

    // 3. Culling: start from passes with side effects or frame outputs
    //    (writes to imported resources) and keep what they need.
    std::vector<PassId> stack;
    for (PassId p = 0; p < n; ++p) {
        const Pass& pass = passes_[p];
        const bool output = std::any_of(pass.uses.begin(), pass.uses.end(),
                                        [&](const PassBuilder::Use& u) { return u.write && resources_[u.resource].imported; });
        if (pass.side_effect || output) stack.push_back(p);
    }
    while (!stack.empty()) {
        const PassId p = stack.back();
        stack.pop_back();
        if (passes_[p].needed) continue;
        passes_[p].needed = true;
        for (PassId d : data_deps[p]) stack.push_back(d);
    }
    for (PassId p = 0; p < n; ++p) {
        if (passes_[p].needed) order_.push_back(p);
    }

    // 4. Lifetimes of the transients the running passes use.
    for (u32 pos = 0; pos < order_.size(); ++pos) {
        for (const PassBuilder::Use& u : passes_[order_[pos]].uses) {
            Resource& r = resources_[u.resource];
            if (r.imported) continue;
            if (!r.used) {
                r.used = true;
                r.first = pos;
            }
            r.last = pos;
        }
    }
    PlaceTransients();

    // 5. Barriers, in order: each use moves its resource to the needed
    //    access. Two writes in a row as ShaderWrite need a UAV barrier.
    std::vector<Access> current(resources_.size());
    std::vector<bool> written(resources_.size(), false);
    for (ResourceId i = 0; i < resources_.size(); ++i) current[i] = resources_[i].imported ? resources_[i].initial : Access::Undefined;
    for (u32 pos = 0; pos < order_.size(); ++pos) {
        Pass& pass = passes_[order_[pos]];
        for (const PassBuilder::Use& u : pass.uses) {
            const Resource& r = resources_[u.resource];
            Barrier b{u.resource, current[u.resource], u.access, false};
            if (!r.imported && r.first == pos) {
                // First use: overlapping memory another resource used earlier needs an aliasing barrier.
                for (const Resource& other : resources_) {
                    if (&other == &r || other.imported || !other.used || other.last >= r.first) continue;
                    const bool overlap = other.placement.offset < r.placement.offset + r.placement.size &&
                                         r.placement.offset < other.placement.offset + other.placement.size;
                    if (overlap) b.aliasing = true;
                }
            }
            const bool uav = u.access == Access::ShaderWrite && current[u.resource] == Access::ShaderWrite && written[u.resource];
            if (b.before != b.after || uav || b.aliasing) pass.barriers.push_back(b);
            current[u.resource] = u.access;
            if (u.write) written[u.resource] = true;
        }
    }
    for (ResourceId i = 0; i < resources_.size(); ++i) {
        const Resource& r = resources_[i];
        if (r.imported && current[i] != r.final_access) final_barriers_.push_back({i, current[i], r.final_access, false});
    }

    // 6. Cross-queue waits: a pass waits for the other queue's passes it depends on.
    for (PassId p : order_) {
        Pass& pass = passes_[p];
        for (PassId d : pass.depends) {
            if (passes_[d].needed && passes_[d].queue != pass.queue) pass.waits.push_back(d);
        }
        std::sort(pass.waits.begin(), pass.waits.end());
        pass.waits.erase(std::unique(pass.waits.begin(), pass.waits.end()), pass.waits.end());
    }
    return true;
}

void FrameGraph::PlaceTransients() {
    // Largest first, each at the lowest offset that doesn't overlap (in
    // memory) a resource alive at the same time.
    std::vector<ResourceId> todo;
    for (ResourceId i = 0; i < resources_.size(); ++i) {
        if (!resources_[i].imported && resources_[i].used) todo.push_back(i);
    }
    std::stable_sort(todo.begin(), todo.end(), [&](ResourceId a, ResourceId b) { return resources_[a].size > resources_[b].size; });
    std::vector<ResourceId> placed;
    for (ResourceId id : todo) {
        Resource& r = resources_[id];
        const u64 size = (r.size + kPlacementAlignment - 1) / kPlacementAlignment * kPlacementAlignment;
        std::vector<const Resource*> alive;
        for (ResourceId other : placed) {
            const Resource& o = resources_[other];
            if (o.first <= r.last && r.first <= o.last) alive.push_back(&o);
        }
        std::vector<u64> candidates{0};
        for (const Resource* o : alive) candidates.push_back(o->placement.offset + o->placement.size);
        std::sort(candidates.begin(), candidates.end());
        u64 offset = 0;
        for (u64 c : candidates) {
            const bool fits = std::none_of(alive.begin(), alive.end(), [&](const Resource* o) {
                return o->placement.offset < c + size && c < o->placement.offset + o->placement.size;
            });
            if (fits) {
                offset = c;
                break;
            }
        }
        r.placement = {offset, size};
        heap_size_ = std::max(heap_size_, offset + size);
        placed.push_back(id);
    }
}

} // namespace aether
