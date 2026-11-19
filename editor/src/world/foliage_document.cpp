#include "world/foliage_document.h"

#include "aether/terrain/asset_io.h"

#include <algorithm>
#include <fstream>

namespace aether::editor {

using namespace aether::terrain;

namespace {
constexpr usize kMaxUndo = 64;
}

FoliagePaintDocument::FoliagePaintDocument(FoliageLayer layer, DensityMap density, HeightSampler ground, u32 seed)
    : layer_(std::move(layer)), density_(std::move(density)), ground_(std::move(ground)), rng_(seed) {
    // The file records the seed painting uses, so a reload continues the same sequence.
    if (layer_.seed == 0) layer_.seed = seed;
}

bool FoliagePaintDocument::Load(const std::filesystem::path& file, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error != nullptr) *error = "can't read " + file.string();
        return false;
    }
    const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded()) {
        if (error != nullptr) *error = file.string() + " isn't valid JSON";
        return false;
    }
    FoliageLayer layer;
    DensityMap density;
    if (!FoliageFromJson(json, layer, density, error)) return false;
    if (in_stroke_) EndStroke();
    layer_ = std::move(layer);
    density_ = std::move(density);
    path_ = file;
    dirty_ = false;
    undo_.clear();
    redo_.clear();
    rng_.seed(layer_.seed);
    Changed();
    return true;
}

bool FoliagePaintDocument::Save(std::string* error) {
    if (path_.empty()) {
        if (error != nullptr) *error = "the foliage has no file yet; use Save As";
        return false;
    }
    return SaveAs(path_, error);
}

bool FoliagePaintDocument::SaveAs(const std::filesystem::path& file, std::string* error) {
    if (in_stroke_) EndStroke();
    std::error_code ec;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error != nullptr) *error = "can't write " + file.string();
        return false;
    }
    out << FoliageToJson(layer_, density_).dump(2) << "\n";
    if (!out) {
        if (error != nullptr) *error = "can't write " + file.string();
        return false;
    }
    path_ = file;
    dirty_ = false;
    return true;
}

void FoliagePaintDocument::MarkSaved() {
    dirty_ = false;
    undo_.clear();
    redo_.clear();
}

std::string FoliagePaintDocument::Name() const { return path_.empty() ? "Foliage" : path_.stem().string(); }

void FoliagePaintDocument::Record() {
    undo_.push_back(layer_);
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
    dirty_ = true;
}

void FoliagePaintDocument::ReplaceLayer(FoliageLayer layer) {
    if (in_stroke_) EndStroke();
    Record();
    layer_ = std::move(layer);
    Changed();
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
    dirty_ = true;
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
    dirty_ = true;
    Changed();
    return true;
}

bool FoliagePaintDocument::Redo() {
    if (redo_.empty()) return false;
    undo_.push_back(std::move(layer_));
    layer_ = std::move(redo_.back());
    redo_.pop_back();
    dirty_ = true;
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
