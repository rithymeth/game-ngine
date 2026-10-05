#include "world/foliage_document.h"

#include <algorithm>

namespace aether::editor {

using namespace aether::terrain;

namespace {
constexpr usize kMaxUndo = 64;
}

FoliagePaintDocument::FoliagePaintDocument(FoliageLayer layer, DensityMap density, HeightSampler ground, u32 seed)
    : layer_(std::move(layer)), density_(std::move(density)), ground_(std::move(ground)), rng_(seed) {}

void FoliagePaintDocument::Record() {
    undo_.push_back(layer_);
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
}

void FoliagePaintDocument::BeginStroke() {
    if (in_stroke_) EndStroke();
    in_stroke_ = true;
    stroke_start_ = layer_;
}

void FoliagePaintDocument::Dab(const Vec3& center) {
    if (!in_stroke_) return;
    if (erase) {
        // On the ground plane: the brush is a disc whatever the heights.
        std::erase_if(layer_.instances, [&](const FoliageInstance& i) {
            const f32 dx = i.position.x - center.x, dz = i.position.z - center.z;
            return dx * dx + dz * dz <= radius * radius;
        });
    } else {
        const usize before = layer_.instances.size();
        PaintInstances(layer_, density_, center, radius, rng_);
        // Onto the ground (keeping each type's anchor offset).
        if (ground_) {
            for (usize i = before; i < layer_.instances.size(); ++i) {
                FoliageInstance& inst = layer_.instances[i];
                const f32 anchor = inst.type_index < layer_.types.size() ? layer_.types[inst.type_index].anchor_offset : 0.0f;
                inst.position.y = ground_(inst.position.x, inst.position.z) + anchor;
            }
        }
    }
    Changed();
}

void FoliagePaintDocument::EndStroke() {
    if (!in_stroke_) return;
    in_stroke_ = false;
    // A stroke only adds (paint) or removes (erase): the same count means nothing changed.
    if (stroke_start_.instances.size() == layer_.instances.size()) return;
    undo_.push_back(std::move(stroke_start_));
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
}

void FoliagePaintDocument::DabOnce(const Vec3& center) {
    BeginStroke();
    Dab(center);
    EndStroke();
}

bool FoliagePaintDocument::Undo() {
    if (in_stroke_) EndStroke();
    if (undo_.empty()) return false;
    redo_.push_back(std::move(layer_));
    layer_ = std::move(undo_.back());
    undo_.pop_back();
    Changed();
    return true;
}

bool FoliagePaintDocument::Redo() {
    if (redo_.empty()) return false;
    undo_.push_back(std::move(layer_));
    layer_ = std::move(redo_.back());
    redo_.pop_back();
    Changed();
    return true;
}

usize FoliagePaintDocument::AddType(const FoliageType& type) {
    Record();
    layer_.types.push_back(type);
    Changed();
    return layer_.types.size() - 1;
}

bool FoliagePaintDocument::RemoveType(usize index) {
    if (index >= layer_.types.size()) return false;
    Record();
    layer_.types.erase(layer_.types.begin() + static_cast<std::ptrdiff_t>(index));
    std::erase_if(layer_.instances, [&](const FoliageInstance& i) { return i.type_index == index; });
    for (FoliageInstance& i : layer_.instances)
        if (i.type_index > index) --i.type_index;
    Changed();
    return true;
}

bool FoliagePaintDocument::SetType(usize index, const FoliageType& type) {
    if (index >= layer_.types.size()) return false;
    Record();
    layer_.types[index] = type;
    Changed();
    return true;
}

void FoliagePaintDocument::SetDensity(f32 d) {
    Record();
    layer_.density = std::max(0.0f, d);
    Changed();
}

void FoliagePaintDocument::SetMaxInstances(u32 max) {
    Record();
    layer_.max_instances = max;
    if (layer_.instances.size() > max) layer_.instances.resize(max);
    Changed();
}

void FoliagePaintDocument::Clear() {
    if (layer_.instances.empty()) return;
    Record();
    layer_.instances.clear();
    Changed();
}

std::vector<usize> FoliagePaintDocument::CountsByType() const {
    std::vector<usize> counts(layer_.types.size(), 0);
    for (const FoliageInstance& i : layer_.instances)
        if (i.type_index < counts.size()) ++counts[i.type_index];
    return counts;
}

} // namespace aether::editor
