#pragma once

// Phase 38 step 8: draw submission -- collect what a frame wants to draw, cull it, order it, hand it
// on.
//
// Two things here are worth more than they look. Culling skips what the frustum cannot see, which is
// the point of the frustum in math/frustum.h. Ordering by material is the other half: switching
// pipeline state costs more than the draw, so the items are sorted by material and only then by mesh,
// stably, so equal keys keep the order they were added in and a caller can rely on it.
//
// The submitter is injected, so this is testable without a GPU: a fake records what it was handed, in
// order. An item carries an opaque `user` value so the caller can find its own mesh and transform
// without this file having to know about either.

#include "aether/math/frustum.h"
#include "aether/math/vec.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace aether::gfx {

struct DrawItem {
    std::string mesh;     // identity of the geometry
    std::string material; // identity of the pipeline state; items are ordered by this first
    Vec3 position;        // world-space, used for culling
    f32 bounds_radius = 0.0f;
    u64 user = 0; // the caller's own handle (a mesh id, an entity, ...)
};

class DrawSubmission {
public:
    using Submitter = std::function<void(const DrawItem&)>;

    // Starts a frame: forgets the previous one's items and counts.
    void Begin() {
        items_.clear();
        submitted_ = 0;
        culled_ = 0;
    }

    void Add(DrawItem item) { items_.push_back(std::move(item)); }
    usize Queued() const { return items_.size(); }

    // Culls against `frustum`, orders by material then mesh (stable, so equal keys keep insertion
    // order), and calls `submit` for each visible item. Returns how many were submitted.
    usize Submit(const Frustum& frustum, const Submitter& submit) {
        std::vector<const DrawItem*> visible;
        visible.reserve(items_.size());
        for (const DrawItem& item : items_) {
            if (frustum.ContainsSphere(item.position, item.bounds_radius)) {
                visible.push_back(&item);
            } else {
                ++culled_;
            }
        }
        std::stable_sort(visible.begin(), visible.end(), [](const DrawItem* a, const DrawItem* b) {
            if (a->material != b->material) return a->material < b->material;
            return a->mesh < b->mesh;
        });
        if (submit) {
            for (const DrawItem* item : visible) submit(*item);
        }
        submitted_ += visible.size();
        return visible.size();
    }

    usize Submitted() const { return submitted_; }
    usize Culled() const { return culled_; }

private:
    std::vector<DrawItem> items_;
    usize submitted_ = 0;
    usize culled_ = 0;
};

} // namespace aether::gfx