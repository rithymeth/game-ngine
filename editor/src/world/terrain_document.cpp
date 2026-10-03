#include "world/terrain_document.h"

#include <algorithm>
#include <cmath>

namespace aether::editor {

using namespace aether::terrain;

namespace {
constexpr usize kMaxUndo = 64;
}

const char* TerrainToolName(TerrainTool tool) {
    switch (tool) {
    case TerrainTool::Raise: return "Raise";
    case TerrainTool::Lower: return "Lower";
    case TerrainTool::Smooth: return "Smooth";
    case TerrainTool::Flatten: return "Flatten";
    case TerrainTool::Paint: return "Paint";
    }
    return "";
}

TerrainEditDocument::TerrainEditDocument(TerrainData data, TerrainSettings settings, SplatmapData splatmap)
    : data_(std::move(data)), settings_(settings), splat_(std::move(splatmap)) {}

f32 TerrainEditDocument::WorldWidth() const {
    const Heightmap& h = data_.heightmap;
    return h.width > 1 ? static_cast<f32>(h.width - 1) * h.cell_size : 0.0f;
}

f32 TerrainEditDocument::WorldDepth() const {
    const Heightmap& h = data_.heightmap;
    return h.height > 1 ? static_cast<f32>(h.height - 1) * h.cell_size : 0.0f;
}

void TerrainEditDocument::BeginStroke() {
    if (in_stroke_) EndStroke();
    in_stroke_ = true;
    stroke_heights_ = data_.heightmap.heights;
    stroke_splat_ = splat_.pixels;
}

void TerrainEditDocument::Dab(f32 wx, f32 wz) {
    if (!in_stroke_) return;
    TerrainBrush b = brush;
    switch (tool) {
    case TerrainTool::Raise:
    case TerrainTool::Lower:
        b.smooth = b.flatten = false;
        b.strength = std::fabs(brush.strength) * (tool == TerrainTool::Lower ? -1.0f : 1.0f);
        ApplyHeightBrush(data_.heightmap, wx, wz, b, raise_height);
        break;
    case TerrainTool::Smooth:
        b.smooth = true, b.flatten = false;
        ApplyHeightBrush(data_.heightmap, wx, wz, b, raise_height);
        break;
    case TerrainTool::Flatten:
        b.flatten = true, b.smooth = false;
        ApplyHeightBrush(data_.heightmap, wx, wz, b, raise_height);
        break;
    case TerrainTool::Paint: {
        const f32 w = WorldWidth(), d = WorldDepth();
        if (splat_.width == 0 || w <= 0.0f || d <= 0.0f) return;
        // ApplyBrushStroke's radius is in its own scale (radius x resolution / 32 pixels): convert from world units.
        b.radius = brush.radius / w * 32.0f;
        ApplyBrushStroke(splat_, wx / w, wz / d, b);
        break;
    }
    }
    ++revision_;
}

void TerrainEditDocument::EndStroke() {
    if (!in_stroke_) return;
    in_stroke_ = false;
    Step step;
    // The changed rectangle of heights.
    const Heightmap& h = data_.heightmap;
    u32 x0 = h.width, z0 = h.height, x1 = 0, z1 = 0;
    for (u32 z = 0; z < h.height; ++z) {
        for (u32 x = 0; x < h.width; ++x) {
            const usize i = static_cast<usize>(z) * h.width + x;
            if (h.heights[i] != stroke_heights_[i]) x0 = std::min(x0, x), z0 = std::min(z0, z), x1 = std::max(x1, x + 1), z1 = std::max(z1, z + 1);
        }
    }
    const bool heights_changed = x1 > x0;
    const bool splat_changed = splat_.pixels != stroke_splat_;
    if (!heights_changed && !splat_changed) return; // nothing to undo
    if (heights_changed) {
        step.x0 = x0, step.z0 = z0, step.x1 = x1, step.z1 = z1;
        for (u32 z = z0; z < z1; ++z) {
            for (u32 x = x0; x < x1; ++x) {
                const usize i = static_cast<usize>(z) * h.width + x;
                step.heights_before.push_back(stroke_heights_[i]);
                step.heights_after.push_back(h.heights[i]);
            }
        }
        MarkDirty(x0, z0, x1, z1);
    }
    if (splat_changed) {
        step.splat_before = std::move(stroke_splat_);
        step.splat_after = splat_.pixels;
    }
    undo_.push_back(std::move(step));
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
    stroke_heights_.clear();
    stroke_splat_.clear();
}

void TerrainEditDocument::DabOnce(f32 wx, f32 wz) {
    BeginStroke();
    Dab(wx, wz);
    EndStroke();
}

void TerrainEditDocument::Apply(const Step& step, bool after) {
    Heightmap& h = data_.heightmap;
    const std::vector<f32>& src = after ? step.heights_after : step.heights_before;
    usize k = 0;
    for (u32 z = step.z0; z < step.z1; ++z)
        for (u32 x = step.x0; x < step.x1; ++x) h.heights[static_cast<usize>(z) * h.width + x] = src[k++];
    if (step.x1 > step.x0) MarkDirty(step.x0, step.z0, step.x1, step.z1);
    if (!step.splat_before.empty()) splat_.pixels = after ? step.splat_after : step.splat_before;
    ++revision_;
}

bool TerrainEditDocument::Undo() {
    if (in_stroke_) EndStroke();
    if (undo_.empty()) return false;
    Apply(undo_.back(), false);
    redo_.push_back(std::move(undo_.back()));
    undo_.pop_back();
    return true;
}

bool TerrainEditDocument::Redo() {
    if (redo_.empty()) return false;
    Apply(redo_.back(), true);
    undo_.push_back(std::move(redo_.back()));
    redo_.pop_back();
    return true;
}

void TerrainEditDocument::MarkDirty(u32 x0, u32 z0, u32 x1, u32 z1) {
    // Normals use the neighbouring samples, and chunks share their edge samples.
    const u32 step = data_.chunk_size > 1 ? data_.chunk_size - 1 : 1;
    const i32 sx0 = static_cast<i32>(x0) - 1, sz0 = static_cast<i32>(z0) - 1;
    const i32 sx1 = static_cast<i32>(x1), sz1 = static_cast<i32>(z1); // inclusive of one more
    const auto chunk_lo = [&](i32 s) { return std::max(0, (s - 1) / static_cast<i32>(step)); };
    const auto chunk_hi = [&](i32 s, u32 count) { return std::min(static_cast<i32>(count) - 1, std::max(0, s) / static_cast<i32>(step)); };
    for (i32 cz = chunk_lo(std::max(0, sz0)); cz <= chunk_hi(sz1, data_.chunk_count_z); ++cz)
        for (i32 cx = chunk_lo(std::max(0, sx0)); cx <= chunk_hi(sx1, data_.chunk_count_x); ++cx) dirty_.insert({cx, cz});
}

std::set<std::pair<i32, i32>> TerrainEditDocument::TakeDirtyChunks() {
    std::set<std::pair<i32, i32>> out;
    out.swap(dirty_);
    return out;
}

std::vector<Vec3> TerrainEditDocument::CursorRing(f32 wx, f32 wz, u32 segments) const {
    std::vector<Vec3> ring;
    segments = std::max(segments, 3u);
    for (u32 i = 0; i < segments; ++i) {
        const f32 a = static_cast<f32>(i) / static_cast<f32>(segments) * 6.2831853f;
        const f32 x = wx + std::cos(a) * brush.radius, z = wz + std::sin(a) * brush.radius;
        ring.push_back(Vec3(x, data_.heightmap.SampleLinear(x, z) * settings_.vertical_scale + 0.05f, z));
    }
    return ring;
}

} // namespace aether::editor
