#pragma once

#include "aether/core/base.h"

#include <string>
#include <vector>

namespace aether::player {

// What `aether_player --report` prints about speed: the CPU time of each frame (from the top of the frame loop to after present, waits for the
// previous frame's fence included, so under vsync it is the display's cadence and the GPU line is the one that says how heavy the work is) and, where the
// device can time the GPU, the GPU's time by stage. The first frames are left out (shader and pipeline creation, first uploads), so the numbers say
// what steady play costs; a spike that is real still shows in `max`.
class FrameReport {
public:
    // `warmup` frames are ignored at the start.
    explicit FrameReport(u32 warmup = 30) : warmup_(warmup) {}

    void AddFrame(f32 cpu_ms);
    // One GPU timing sample (a finished frame): the sum and the three stages, milliseconds. Samples are only used after the warm-up, too.
    void AddGpu(f32 frame_ms, f32 shadows_ms, f32 scene_ms, f32 post_ms);

    struct Stats {
        u32 samples = 0;
        f32 average = 0, p50 = 0, p95 = 0, p99 = 0, max = 0;
    };
    Stats Cpu() const { return Summarise(cpu_); }
    Stats Gpu() const { return Summarise(gpu_); }
    // The GPU's mean time per stage over the samples (0 without samples).
    f32 GpuShadows() const { return Mean(shadows_); }
    f32 GpuScene() const { return Mean(scene_); }
    f32 GpuPost() const { return Mean(post_); }
    u32 FramesSeen() const { return seen_; }

    // One or two lines for the console: "Frame time: ..." and "GPU time: ..." (or "GPU time: not available").
    std::string Text() const;

    static Stats Summarise(std::vector<f32> values);

private:
    static f32 Mean(const std::vector<f32>& values);

    u32 warmup_ = 30;
    u32 seen_ = 0;
    u32 gpu_seen_ = 0;
    std::vector<f32> cpu_, gpu_, shadows_, scene_, post_;
};

} // namespace aether::player
