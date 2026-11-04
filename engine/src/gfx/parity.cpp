#include "aether/gfx/parity.h"

#include "aether/core/log.h"

#if defined(_WIN32) || defined(AETHER_HAS_VULKAN)
#include "aether/gfx/rhi/device.h"
#define AETHER_PARITY_HAS_DEVICES 1
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>

namespace aether::gfx::parity {

ImageDiff CompareImages(const std::vector<u8>& a, const std::vector<u8>& b, u32 width, u32 height, const Tolerance& tolerance) {
    ImageDiff diff;
    diff.width = width;
    diff.height = height;
    const usize bytes = static_cast<usize>(width) * height * 4;
    if (width == 0 || height == 0 || a.size() != bytes || b.size() != bytes) return diff;
    diff.comparable = true;
    u64 total = 0;
    bool found = false;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const usize at = (static_cast<usize>(y) * width + x) * 4;
            u32 worst = 0;
            for (usize c = 0; c < 4; ++c) {
                const u32 d = static_cast<u32>(std::abs(static_cast<int>(a[at + c]) - static_cast<int>(b[at + c])));
                total += d;
                worst = std::max(worst, d);
            }
            diff.max_delta = std::max(diff.max_delta, worst);
            if (worst > tolerance.max_channel_delta) {
                ++diff.bad_pixels;
                if (!found) {
                    found = true;
                    diff.first_bad_x = x;
                    diff.first_bad_y = y;
                }
            }
        }
    }
    diff.mean_delta = static_cast<f64>(total) / static_cast<f64>(bytes);
    return diff;
}

bool Within(const ImageDiff& diff, const Tolerance& tolerance) {
    return diff.comparable && diff.BadFraction() <= tolerance.max_bad_fraction;
}

std::string Describe(const ImageDiff& diff) {
    if (!diff.comparable) return "not comparable (different sizes)";
    char text[160];
    if (diff.bad_pixels == 0) {
        std::snprintf(text, sizeof(text), "max %u, mean %.2f, no bad pixels", diff.max_delta, diff.mean_delta);
    } else {
        std::snprintf(text, sizeof(text), "max %u, mean %.2f, %zu bad pixels (first at %u,%u)", diff.max_delta, diff.mean_delta,
                      diff.bad_pixels, diff.first_bad_x, diff.first_bad_y);
    }
    return text;
}

const std::vector<Scene>& Scenes() {
    static const std::vector<Scene> scenes = {
        {SceneKind::Flat, "flat"},
        {SceneKind::Gradient, "gradient"},
        {SceneKind::Checker, "checker"},
    };
    return scenes;
}

namespace {

constexpr u32 kCheckerCell = 8; // pixels

u8 ToByte(f64 v) { return static_cast<u8>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

} // namespace

std::vector<u8> ReferenceImage(const Scene& scene, u32 width, u32 height) {
    if (scene.kind == SceneKind::Custom) return scene.reference ? scene.reference(width, height) : std::vector<u8>();
    std::vector<u8> out(static_cast<usize>(width) * height * 4);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            u8* p = &out[(static_cast<usize>(y) * width + x) * 4];
            switch (scene.kind) {
            case SceneKind::Flat:
                p[0] = ToByte(0.2), p[1] = ToByte(0.4), p[2] = ToByte(0.6), p[3] = 255;
                break;
            case SceneKind::Gradient: // red across, green down, from the pixel centres
                p[0] = ToByte((x + 0.5) / width), p[1] = ToByte((y + 0.5) / height), p[2] = ToByte(0.25), p[3] = 255;
                break;
            case SceneKind::Checker: {
                const bool dark = ((x / kCheckerCell) + (y / kCheckerCell)) % 2 == 0;
                const u8 v = dark ? ToByte(0.1) : ToByte(0.9);
                p[0] = v, p[1] = v, p[2] = v, p[3] = 255;
                break;
            }
            case SceneKind::Custom:
                break;
            }
        }
    }
    return out;
}

std::string SceneHlsl(const Scene& scene, u32 width, u32 height) {
    if (scene.kind == SceneKind::Custom) return scene.hlsl ? scene.hlsl(width, height) : std::string();
    // One triangle that covers the target; SV_Position is the pixel centre (x + 0.5, y + 0.5), y down, on every backend.
    std::string hlsl =
        "float4 VSMain(uint id : SV_VertexID) : SV_Position {\n"
        "    float2 uv = float2((id << 1) & 2, id & 2);\n"
        "    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        "}\n";
    switch (scene.kind) {
    case SceneKind::Flat:
        hlsl += "float4 PSMain(float4 pos : SV_Position) : SV_Target {\n    return float4(0.2, 0.4, 0.6, 1.0);\n}\n";
        break;
    case SceneKind::Gradient:
        hlsl += "float4 PSMain(float4 pos : SV_Position) : SV_Target {\n    return float4(pos.x / " + std::to_string(width) + ".0, pos.y / " +
                std::to_string(height) + ".0, 0.25, 1.0);\n}\n";
        break;
    case SceneKind::Checker:
        hlsl += "float4 PSMain(float4 pos : SV_Position) : SV_Target {\n"
                "    uint2 cell = uint2(pos.xy) / " + std::to_string(kCheckerCell) + ";\n"
                "    float v = ((cell.x + cell.y) % 2 == 0) ? 0.1 : 0.9;\n"
                "    return float4(v, v, v, 1.0);\n}\n";
        break;
    case SceneKind::Custom:
        break;
    }
    return hlsl;
}

bool WriteBmp(const std::string& path, const std::vector<u8>& rgba, u32 width, u32 height) {
    if (width == 0 || height == 0 || rgba.size() != static_cast<usize>(width) * height * 4) return false;
    const u32 row = (width * 3 + 3) & ~3u;
    const u32 data_size = row * height;
    std::vector<u8> file(54 + data_size, 0);
    const auto put32 = [&](usize at, u32 v) {
        for (int i = 0; i < 4; ++i) file[at + i] = static_cast<u8>((v >> (8 * i)) & 0xFF);
    };
    file[0] = 'B', file[1] = 'M';
    put32(2, 54 + data_size);
    put32(10, 54);
    put32(14, 40);
    put32(18, width);
    put32(22, height); // positive: rows bottom to top
    file[26] = 1;
    file[28] = 24;
    put32(34, data_size);
    for (u32 y = 0; y < height; ++y) {
        const u8* src = &rgba[static_cast<usize>(height - 1 - y) * width * 4];
        u8* dst = &file[54 + static_cast<usize>(y) * row];
        for (u32 x = 0; x < width; ++x) dst[x * 3 + 0] = src[x * 4 + 2], dst[x * 3 + 1] = src[x * 4 + 1], dst[x * 3 + 2] = src[x * 4 + 0];
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(out);
}

const char* BackendName(rhi::Backend backend) { return backend == rhi::Backend::D3D12 ? "D3D12" : "Vulkan"; }

usize SceneReport::RenderedCount() const {
    usize n = 0;
    for (const BackendRun& r : runs) n += r.rendered ? 1 : 0;
    return n;
}

bool SceneReport::Ok(const Tolerance& tolerance) const {
    for (const BackendRun& r : runs) {
        if (r.available && !(r.rendered && Within(r.vs_reference, tolerance))) return false;
    }
    for (const Pair& p : pairs) {
        if (!Within(p.diff, tolerance)) return false;
    }
    return true;
}

#if defined(AETHER_PARITY_HAS_DEVICES)

bool RenderScene(rhi::IDevice& device, const Scene& scene, u32 width, u32 height, std::vector<u8>& pixels, std::string* error, f64* frame_ms) {
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    std::unique_ptr<rhi::ISwapChain> target = device.CreateSwapChain(nullptr, width, height, 2);
    if (!target) return fail("the device has no offscreen swap chain");
    rhi::PipelineDesc desc;
    desc.hlsl_source = SceneHlsl(scene, width, height);
    const rhi::PipelineHandle pipeline = device.CreatePipeline(desc, *target);
    if (!pipeline.IsValid()) return fail("the scene's pipeline did not compile");
    std::unique_ptr<rhi::ICommandList> cmd = device.CreateCommandList();
    target->AcquireNextImage();
    cmd->Reset();
    cmd->BeginRenderPass(*target, {0.0f, 0.0f, 0.0f, 1.0f});
    cmd->BindPipeline(pipeline);
    cmd->Draw(3);
    cmd->EndRenderPass();
    cmd->Close();
    device.WaitForFence(device.Submit(*cmd, target.get()));
    if (!target->ReadBack(pixels)) return fail("the swap chain can't be read back");
    target->Present(false);
    if (frame_ms) {
        constexpr int kFrames = 20;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kFrames; ++i) {
            target->AcquireNextImage();
            cmd->Reset();
            cmd->BeginRenderPass(*target, {0.0f, 0.0f, 0.0f, 1.0f});
            cmd->BindPipeline(pipeline);
            cmd->Draw(3);
            cmd->EndRenderPass();
            cmd->Close();
            device.WaitForFence(device.Submit(*cmd, target.get()));
            target->Present(false);
        }
        *frame_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count() / kFrames;
    }
    return true;
}

SceneReport RunScene(const Scene& scene, u32 width, u32 height, const std::vector<rhi::Backend>& backends, const Tolerance& tolerance) {
    SceneReport report;
    report.scene = scene.name;
    report.width = width;
    report.height = height;
    const std::vector<u8> reference = ReferenceImage(scene, width, height);
    for (const rhi::Backend backend : backends) {
        BackendRun run;
        run.backend = backend;
        run.available = rhi::IsBackendAvailable(backend);
        if (run.available) {
            std::unique_ptr<rhi::IDevice> device;
            try {
                device = rhi::CreateDevice(backend, false);
            } catch (const std::exception& e) {
                run.error = e.what();
            }
            if (!device) {
                run.available = false; // listed, but no usable device on this machine (no driver)
                if (run.error.empty()) run.error = "no device";
            } else {
                run.rendered = RenderScene(*device, scene, width, height, run.pixels, &run.error, &run.frame_ms);
                if (run.rendered) run.vs_reference = CompareImages(run.pixels, reference, width, height, tolerance);
            }
        }
        report.runs.push_back(std::move(run));
    }
    for (usize i = 0; i < report.runs.size(); ++i) {
        for (usize j = i + 1; j < report.runs.size(); ++j) {
            const BackendRun &a = report.runs[i], &b = report.runs[j];
            if (a.rendered && b.rendered) report.pairs.push_back({a.backend, b.backend, CompareImages(a.pixels, b.pixels, width, height, tolerance)});
        }
    }
    return report;
}

#else

bool RenderScene(rhi::IDevice&, const Scene&, u32, u32, std::vector<u8>&, std::string* error, f64*) {
    if (error) *error = "this build has no RHI backend";
    return false;
}

SceneReport RunScene(const Scene& scene, u32 width, u32 height, const std::vector<rhi::Backend>& backends, const Tolerance&) {
    SceneReport report;
    report.scene = scene.name;
    report.width = width;
    report.height = height;
    for (const rhi::Backend backend : backends) {
        BackendRun run;
        run.backend = backend; // not available: nothing is built
        report.runs.push_back(std::move(run));
    }
    return report;
}

#endif

} // namespace aether::gfx::parity
