#include "world/terrain_panel.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aether::editor {

using namespace terrain;

namespace {

constexpr int kRingSegments = 48;
constexpr f32 kTwoPi = 6.28318530718f;

f32 ExtentX(const Heightmap& hm) { return hm.width > 1 ? static_cast<f32>(hm.width - 1) * hm.cell_size : 0.0f; }
f32 ExtentZ(const Heightmap& hm) { return hm.height > 1 ? static_cast<f32>(hm.height - 1) * hm.cell_size : 0.0f; }

const char* kModeNames[] = {"Raise / Lower", "Flatten", "Smooth", "Noise", "Erode"};

} // namespace

TerrainPanel::TerrainPanel(TerrainData& data, SplatmapData& splat, const TerrainSettings& settings)
    : data_(data), splat_(splat), settings_(settings) {}

bool TerrainPanel::Pick(const Vec3& origin, const Vec3& direction, f32 max_distance) {
    const HeightfieldHit hit = HeightfieldRaycast(data_.heightmap, settings_.vertical_scale, origin, direction, max_distance);
    has_hover_ = hit.hit;
    if (hit.hit) hover_ = hit.point;
    return hit.hit;
}

void TerrainPanel::BeginStroke() {
    if (in_stroke_) EndStroke();
    in_stroke_ = true;
    heights_before_.clear();
    pixels_before_.clear();
    if (tool == TerrainTool::Sculpt) heights_before_ = data_.heightmap.heights;
    else pixels_before_ = splat_.pixels;
}

bool TerrainPanel::Stroke(const Vec3& point, f32 dt) {
    if (!in_stroke_) return false;
    Heightmap& hm = data_.heightmap;
    if (!HeightfieldContains(hm, point.x, point.z)) return false;

    TerrainBrush b = brush;
    b.strength *= std::clamp(dt * stroke_rate, 0.0f, 1.0f);
    if (tool == TerrainTool::Sculpt) {
        ApplySculptBrush(hm, point.x, point.z, b, 1.0f);
        MarkDirty(point.x, point.z, brush.radius);
        return true;
    }
    if (splat_.width == 0 || splat_.height == 0 || splat_.pixels.size() < static_cast<usize>(splat_.width) * splat_.height * 4) {
        status_ = "No splatmap to paint on";
        return false;
    }
    const i32 layers = static_cast<i32>(std::clamp<usize>(data_.layers.size(), 1, 4));
    b.layer_index = std::clamp(b.layer_index, 0, layers - 1);
    ApplyBrushStroke(splat_, point.x / ExtentX(hm), point.z / ExtentZ(hm), b);
    MarkDirty(point.x, point.z, brush.radius);
    return true;
}

bool TerrainPanel::EndStroke() {
    if (!in_stroke_) return false;
    in_stroke_ = false;

    Patch patch;
    if (tool == TerrainTool::Sculpt) {
        const Heightmap& hm = data_.heightmap;
        if (heights_before_.size() != hm.heights.size()) return false;
        u32 min_x = hm.width, min_z = hm.height, max_x = 0, max_z = 0;
        bool changed = false;
        for (u32 z = 0; z < hm.height; ++z) {
            for (u32 x = 0; x < hm.width; ++x) {
                const usize i = static_cast<usize>(z) * hm.width + x;
                if (hm.heights[i] == heights_before_[i]) continue;
                changed = true;
                min_x = std::min(min_x, x), max_x = std::max(max_x, x);
                min_z = std::min(min_z, z), max_z = std::max(max_z, z);
            }
        }
        if (!changed) return false;
        patch.x0 = min_x, patch.z0 = min_z, patch.w = max_x - min_x + 1, patch.h = max_z - min_z + 1;
        for (u32 z = 0; z < patch.h; ++z) {
            for (u32 x = 0; x < patch.w; ++x) {
                const usize i = static_cast<usize>(patch.z0 + z) * hm.width + patch.x0 + x;
                patch.height_before.push_back(heights_before_[i]);
                patch.height_after.push_back(hm.heights[i]);
            }
        }
        RefreshBounds();
    } else {
        patch.splat = true;
        if (pixels_before_.size() != splat_.pixels.size()) return false;
        u32 min_x = splat_.width, min_z = splat_.height, max_x = 0, max_z = 0;
        bool changed = false;
        for (u32 z = 0; z < splat_.height; ++z) {
            for (u32 x = 0; x < splat_.width; ++x) {
                const usize i = (static_cast<usize>(z) * splat_.width + x) * 4;
                if (std::equal(splat_.pixels.begin() + i, splat_.pixels.begin() + i + 4, pixels_before_.begin() + i)) continue;
                changed = true;
                min_x = std::min(min_x, x), max_x = std::max(max_x, x);
                min_z = std::min(min_z, z), max_z = std::max(max_z, z);
            }
        }
        if (!changed) return false;
        patch.x0 = min_x, patch.z0 = min_z, patch.w = max_x - min_x + 1, patch.h = max_z - min_z + 1;
        for (u32 z = 0; z < patch.h; ++z) {
            const usize row = (static_cast<usize>(patch.z0 + z) * splat_.width + patch.x0) * 4;
            patch.pixel_before.insert(patch.pixel_before.end(), pixels_before_.begin() + row, pixels_before_.begin() + row + patch.w * 4);
            patch.pixel_after.insert(patch.pixel_after.end(), splat_.pixels.begin() + row, splat_.pixels.begin() + row + patch.w * 4);
        }
    }
    heights_before_.clear();
    pixels_before_.clear();
    Push(std::move(patch));
    return true;
}

void TerrainPanel::Push(Patch patch) {
    undo_.push_back(std::move(patch));
    redo_.clear();
    if (undo_.size() > max_history) undo_.erase(undo_.begin(), undo_.begin() + (undo_.size() - max_history));
}

void TerrainPanel::Apply(const Patch& patch, bool after) {
    if (patch.splat) {
        const std::vector<u8>& src = after ? patch.pixel_after : patch.pixel_before;
        for (u32 z = 0; z < patch.h; ++z) {
            const usize dst = (static_cast<usize>(patch.z0 + z) * splat_.width + patch.x0) * 4;
            std::copy_n(src.begin() + static_cast<usize>(z) * patch.w * 4, patch.w * 4, splat_.pixels.begin() + dst);
        }
        return;
    }
    Heightmap& hm = data_.heightmap;
    const std::vector<f32>& src = after ? patch.height_after : patch.height_before;
    for (u32 z = 0; z < patch.h; ++z) {
        const usize dst = static_cast<usize>(patch.z0 + z) * hm.width + patch.x0;
        std::copy_n(src.begin() + static_cast<usize>(z) * patch.w, patch.w, hm.heights.begin() + dst);
    }
}

bool TerrainPanel::Undo() {
    if (in_stroke_ || undo_.empty()) return false;
    Patch patch = std::move(undo_.back());
    undo_.pop_back();
    Apply(patch, false);
    if (!patch.splat) RefreshBounds();
    const Heightmap& hm = data_.heightmap;
    const f32 sx = patch.splat ? ExtentX(hm) / static_cast<f32>(std::max(splat_.width, 2u) - 1) : hm.cell_size;
    const f32 sz = patch.splat ? ExtentZ(hm) / static_cast<f32>(std::max(splat_.height, 2u) - 1) : hm.cell_size;
    MarkDirty(patch.x0 * sx + 0.5f * patch.w * sx, patch.z0 * sz + 0.5f * patch.h * sz,
              0.5f * std::max(patch.w * sx, patch.h * sz));
    redo_.push_back(std::move(patch));
    return true;
}

bool TerrainPanel::Redo() {
    if (in_stroke_ || redo_.empty()) return false;
    Patch patch = std::move(redo_.back());
    redo_.pop_back();
    Apply(patch, true);
    if (!patch.splat) RefreshBounds();
    const Heightmap& hm = data_.heightmap;
    const f32 sx = patch.splat ? ExtentX(hm) / static_cast<f32>(std::max(splat_.width, 2u) - 1) : hm.cell_size;
    const f32 sz = patch.splat ? ExtentZ(hm) / static_cast<f32>(std::max(splat_.height, 2u) - 1) : hm.cell_size;
    MarkDirty(patch.x0 * sx + 0.5f * patch.w * sx, patch.z0 * sz + 0.5f * patch.h * sz,
              0.5f * std::max(patch.w * sx, patch.h * sz));
    undo_.push_back(std::move(patch));
    return true;
}

void TerrainPanel::MarkDirty(f32 world_x, f32 world_z, f32 radius) {
    if (data_.chunk_count_x == 0 || data_.chunk_count_z == 0 || !(settings_.chunk_world_size > 0.0f)) return;
    // A cell beyond the brush still shares normals with the edge, hence the margin.
    const f32 margin = radius + data_.heightmap.cell_size;
    const i32 cx0 = std::max(0, static_cast<i32>(std::floor((world_x - margin) / settings_.chunk_world_size)));
    const i32 cx1 = std::min(static_cast<i32>(data_.chunk_count_x) - 1, static_cast<i32>(std::floor((world_x + margin) / settings_.chunk_world_size)));
    const i32 cz0 = std::max(0, static_cast<i32>(std::floor((world_z - margin) / settings_.chunk_world_size)));
    const i32 cz1 = std::min(static_cast<i32>(data_.chunk_count_z) - 1, static_cast<i32>(std::floor((world_z + margin) / settings_.chunk_world_size)));
    for (i32 z = cz0; z <= cz1; ++z) {
        for (i32 x = cx0; x <= cx1; ++x) {
            const TerrainChunkCoord c{x, z};
            if (std::find(dirty_.begin(), dirty_.end(), c) == dirty_.end()) dirty_.push_back(c);
        }
    }
}

std::vector<TerrainChunkCoord> TerrainPanel::TakeDirtyChunks() {
    std::vector<TerrainChunkCoord> out = std::move(dirty_);
    dirty_.clear();
    std::sort(out.begin(), out.end(), [](const TerrainChunkCoord& a, const TerrainChunkCoord& b) { return a.z != b.z ? a.z < b.z : a.x < b.x; });
    return out;
}

void TerrainPanel::RefreshBounds() {
    const Heightmap& hm = data_.heightmap;
    if (hm.heights.empty()) return;
    const auto [lo, hi] = std::minmax_element(hm.heights.begin(), hm.heights.end());
    data_.bounds_min.y = *lo * settings_.vertical_scale;
    data_.bounds_max.y = *hi * settings_.vertical_scale;
}

void TerrainPanel::BuildCursor(std::vector<TerrainOverlayLine>& out) const {
    if (!show_cursor || !has_hover_) return;
    const Heightmap& hm = data_.heightmap;
    auto ring = [&](f32 radius, u32 color) {
        auto point = [&](int i) {
            const f32 a = kTwoPi * static_cast<f32>(i) / static_cast<f32>(kRingSegments);
            const f32 x = hover_.x + std::cos(a) * radius;
            const f32 z = hover_.z + std::sin(a) * radius;
            return Vec3(x, HeightfieldHeightAt(hm, settings_.vertical_scale, x, z) + 0.05f, z);
        };
        for (int i = 0; i < kRingSegments; ++i) out.push_back({point(i), point(i + 1), color});
    };
    ring(brush.radius, terrain_colors::kCursor);
    ring(brush.radius * 0.5f, terrain_colors::kCursorInner);
    out.push_back({hover_, hover_ + Vec3(0, 0.5f * brush.radius, 0), terrain_colors::kCursor});
}

void TerrainPanel::Draw() {
    int tool_index = tool == TerrainTool::Sculpt ? 0 : 1;
    ImGui::RadioButton("Sculpt", &tool_index, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Paint", &tool_index, 1);
    tool = tool_index == 0 ? TerrainTool::Sculpt : TerrainTool::Paint;

    if (ImGui::CollapsingHeader("Brush", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Radius", &brush.radius, 0.5f, 100.0f, "%.1f m");
        ImGui::SliderFloat("Strength", &brush.strength, -1.0f, 1.0f);
        ImGui::SliderFloat("Falloff", &brush.falloff, 0.0f, 1.0f);
        if (tool == TerrainTool::Sculpt) {
            int mode = static_cast<int>(brush.mode);
            if (ImGui::Combo("Mode", &mode, kModeNames, IM_ARRAYSIZE(kModeNames))) brush.mode = static_cast<SculptMode>(mode);
            if (brush.mode == SculptMode::Noise) {
                ImGui::SliderFloat("Noise scale", &brush.noise_scale, 0.5f, 64.0f);
                ImGui::InputInt("Noise seed", &brush.noise_seed);
            }
            if (brush.mode == SculptMode::Erode) {
                ImGui::SliderFloat("Deposition", &brush.erosion_deposition, 0.0f, 1.0f);
                ImGui::SliderInt("Iterations", &brush.erosion_iterations, 1, 8);
            }
        } else {
            const int layers = static_cast<int>(std::clamp<usize>(data_.layers.size(), 1, 4));
            brush.layer_index = std::clamp(brush.layer_index, 0, layers - 1);
            for (int i = 0; i < layers; ++i) {
                if (i > 0) ImGui::SameLine();
                const std::string& name = i < static_cast<int>(data_.layers.size()) ? data_.layers[i].name : std::string("Layer");
                ImGui::PushID(i);
                ImGui::RadioButton(name.empty() ? "Layer" : name.c_str(), &brush.layer_index, i);
                ImGui::PopID();
            }
        }
        ImGui::SliderFloat("Stroke rate", &stroke_rate, 1.0f, 60.0f);
        ImGui::Checkbox("Show cursor", &show_cursor);
    }

    ImGui::BeginDisabled(undo_.empty() || in_stroke_);
    if (ImGui::Button("Undo")) Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(redo_.empty() || in_stroke_);
    if (ImGui::Button("Redo")) Redo();
    ImGui::EndDisabled();

    ImGui::Text("History: %zu undo, %zu redo", undo_.size(), redo_.size());
    ImGui::Text("Chunks to rebuild: %zu", dirty_.size());
    if (has_hover_) ImGui::Text("Cursor: %.1f, %.1f, %.1f", hover_.x, hover_.y, hover_.z);
    else ImGui::TextDisabled("Cursor: off the terrain");
    if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());
}

} // namespace aether::editor
