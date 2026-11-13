#include "aether/player/frame_report.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aether::player {

void FrameReport::AddFrame(f32 cpu_ms) {
    ++seen_;
    if (seen_ <= warmup_ || !std::isfinite(cpu_ms)) return;
    cpu_.push_back(cpu_ms);
}

void FrameReport::AddGpu(f32 frame_ms, f32 shadows_ms, f32 scene_ms, f32 post_ms) {
    ++gpu_seen_;
    if (gpu_seen_ <= warmup_ || !std::isfinite(frame_ms)) return;
    gpu_.push_back(frame_ms);
    shadows_.push_back(shadows_ms);
    scene_.push_back(scene_ms);
    post_.push_back(post_ms);
}

f32 FrameReport::Mean(const std::vector<f32>& values) {
    if (values.empty()) return 0.0f;
    f64 sum = 0;
    for (const f32 v : values) sum += v;
    return static_cast<f32>(sum / static_cast<f64>(values.size()));
}

FrameReport::Stats FrameReport::Summarise(std::vector<f32> values) {
    Stats stats;
    stats.samples = static_cast<u32>(values.size());
    if (values.empty()) return stats;
    std::sort(values.begin(), values.end());
    stats.average = Mean(values);
    // Nearest-rank percentile: the smallest value that at least p of the samples are at or below.
    const auto at = [&](f64 p) {
        const usize rank = static_cast<usize>(std::ceil(p * static_cast<f64>(values.size())));
        return values[std::min(values.size() - 1, rank == 0 ? usize{0} : rank - 1)];
    };
    stats.p50 = at(0.50);
    stats.p95 = at(0.95);
    stats.p99 = at(0.99);
    stats.max = values.back();
    return stats;
}

std::string FrameReport::Text() const {
    char line[256];
    std::string text;
    const Stats cpu = Cpu();
    if (cpu.samples == 0) {
        std::snprintf(line, sizeof(line), "Frame time: no frames measured (%u run, the first %u are left out)\n", seen_, warmup_);
    } else {
        std::snprintf(line, sizeof(line), "Frame time: %u frames, cpu avg %.2f ms (%.0f fps), p50 %.2f, p95 %.2f, p99 %.2f, max %.2f\n", cpu.samples, cpu.average,
                      cpu.average > 0 ? 1000.0f / cpu.average : 0.0f, cpu.p50, cpu.p95, cpu.p99, cpu.max);
    }
    text += line;
    const Stats gpu = Gpu();
    if (gpu.samples == 0) {
        text += "GPU time: not available\n";
    } else {
        std::snprintf(line, sizeof(line), "GPU time: %u samples, avg %.2f ms (shadows %.2f, scene %.2f, post %.2f), p95 %.2f, max %.2f\n", gpu.samples, gpu.average,
                      GpuShadows(), GpuScene(), GpuPost(), gpu.p95, gpu.max);
        text += line;
    }
    return text;
}

} // namespace aether::player
