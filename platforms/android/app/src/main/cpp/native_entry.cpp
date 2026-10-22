// The Android app shell's native side.
// - On start it plays Lantern Run headless (samples/frozen_api_game/smoke.h) and logs "AETHER_SMOKE PASS" or
//   "AETHER_SMOKE FAIL <check>" under the log tag "Aether".
// - When the SurfaceView has a surface, a render thread opens the Vulkan RHI on it, clears each frame to
//   kClearColor and draws a triangle over the middle of the screen in kTriangleColor with an HLSL pipeline
//   (compiled on the device by glslang). After kFramesToReport presented frames it logs
//   "AETHER_RENDER PRESENTED <n> <width>x<height>"; on failure, "AETHER_RENDER FAIL <reason>". The CI emulator job
//   checks both lines and samples the screen (tools/android/check_screen.py).
#include "smoke.h"

#include "aether/gfx/rhi/device.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

namespace rhi = aether::gfx::rhi;

// A colour no Android default theme uses, so a screenshot shows whether the engine drew the frame.
constexpr rhi::ClearColor kClearColor{0.15f, 0.55f, 0.35f, 1.0f};
constexpr float kTriangleColor[4] = {0.90f, 0.45f, 0.10f, 1.0f};
constexpr int kFramesToReport = 60;

// A triangle that covers the centre of the screen whichever way up clip space is.
constexpr const char* kTriangleShader = R"(
struct PushConstants { float4 color; };
[[vk::push_constant]]
ConstantBuffer<PushConstants> g_PC : register(b0);

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 corners[3] = { float2(-0.6, -0.6), float2(0.6, -0.6), float2(0.0, 0.7) };
    return float4(corners[id], 0.0, 1.0);
}

float4 PSMain() : SV_Target { return g_PC.color; }
)";

void Log(int priority, const std::string& line) { __android_log_print(priority, "Aether", "%s", line.c_str()); }

class Renderer {
public:
    void Start(ANativeWindow* window, int width, int height) {
        Stop();
        stop_ = false;
        thread_ = std::thread([this, window, width, height] { Run(window, width, height); });
    }

    void Stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

private:
    void Run(ANativeWindow* window, int width, int height) {
        try {
            std::unique_ptr<rhi::IDevice> device = rhi::CreateDevice(rhi::Backend::Vulkan, false);
            if (!device) throw std::runtime_error("no Vulkan device");
            std::unique_ptr<rhi::ISwapChain> swap_chain =
                device->CreateSwapChain(window, static_cast<aether::u32>(width), static_cast<aether::u32>(height), 3);
            rhi::PipelineDesc desc;
            desc.hlsl_source = kTriangleShader;
            desc.push_constant_size_bytes = sizeof(kTriangleColor);
            const rhi::PipelineHandle pipeline = device->CreatePipeline(desc, *swap_chain);
            if (!pipeline.IsValid()) throw std::runtime_error("triangle pipeline did not build");
            std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
            int presented = 0;
            aether::u64 last_fence = 0;
            while (!stop_) {
                swap_chain->AcquireNextImage();
                cmd->Reset();
                cmd->BeginRenderPass(*swap_chain, kClearColor);
                cmd->BindPipeline(pipeline);
                cmd->SetPushConstants(kTriangleColor, sizeof(kTriangleColor));
                cmd->Draw(3);
                cmd->EndRenderPass();
                cmd->Close();
                last_fence = device->Submit(*cmd, swap_chain.get());
                device->WaitForFence(last_fence);
                swap_chain->Present(true);
                if (++presented == kFramesToReport) {
                    Log(ANDROID_LOG_INFO, "AETHER_RENDER PRESENTED " + std::to_string(presented) + " " +
                                              std::to_string(swap_chain->Width()) + "x" +
                                              std::to_string(swap_chain->Height()));
                }
            }
            if (last_fence != 0) device->WaitForFence(last_fence); // idle before the swap chain goes
        } catch (const std::exception& e) {
            Log(ANDROID_LOG_ERROR, std::string("AETHER_RENDER FAIL ") + e.what());
        }
        ANativeWindow_release(window);
    }

    std::thread thread_;
    std::atomic<bool> stop_{false};
};

Renderer& TheRenderer() {
    static Renderer renderer;
    return renderer;
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_aether_engine_MainActivity_nativeEngineStatus(JNIEnv* env, jclass) {
    const std::string line = lantern::SmokeLine();
    Log(line == "AETHER_SMOKE PASS" ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, line);
    return env->NewStringUTF(("Aether Engine: Lantern Run self-test\n" + line).c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_aether_engine_MainActivity_nativeSurfaceChanged(JNIEnv* env, jclass, jobject surface, jint width,
                                                        jint height) {
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface); // released by the render thread
    if (!window) {
        Log(ANDROID_LOG_ERROR, "AETHER_RENDER FAIL no native window");
        return;
    }
    TheRenderer().Start(window, width, height);
}

extern "C" JNIEXPORT void JNICALL Java_com_aether_engine_MainActivity_nativeSurfaceDestroyed(JNIEnv*, jclass) {
    TheRenderer().Stop();
}
