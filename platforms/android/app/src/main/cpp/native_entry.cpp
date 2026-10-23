// The Android app shell's native side.
// - On start it plays Lantern Run headless (samples/frozen_api_game/smoke.h) and logs "AETHER_SMOKE PASS" or
//   "AETHER_SMOKE FAIL <check>" under the log tag "Aether".
// - When the SurfaceView has a surface, a render thread opens the Vulkan RHI on it, clears each frame to
//   kClearColor and draws a triangle over the middle of the screen in kTriangleColor with an HLSL pipeline
//   (compiled on the device by glslang). After kFramesToReport presented frames it logs
//   "AETHER_RENDER PRESENTED <n> <width>x<height>"; on failure, "AETHER_RENDER FAIL <reason>". The CI emulator job
//   checks both lines and samples the screen (tools/android/check_screen.py).
// - Touches from the SurfaceView reach the render thread as WindowEvents and go through ApplyWindowEvents into an
//   input::InputState, as on desktop. While the first finger is down (Key::MouseLeft) the triangle is drawn in
//   kPressedColor; each new finger logs "AETHER_INPUT DOWN <x> <y>".
// - Scene mode (the activity started with `--es mode scene`): the render thread mounts the .apak files the activity
//   copied to <files>/Paks, loads the startup scene into a player Game and draws it with player::SceneRenderer, the
//   same path as the desktop player. Touches go to the game's input. It logs "AETHER_SCENE LOADED <project> <scene>"
//   and, after kFramesToReport frames, "AETHER_SCENE PRESENTED <n> <width>x<height>"; on failure "AETHER_SCENE FAIL".
// - The "android" platform plugin: Activity pause, resume and onTrimMemory reach the engine's PlatformLifecycle
//   (dispatched on the UI thread, Android's main thread), which logs "AETHER_LIFECYCLE <event>"; user_data_dir is the
//   app's files directory. The surface goes away while paused, and the render thread starts again on the new one.
#include "smoke.h"

#include "aether/gfx/rhi/device.h"
#include "aether/input/keys.h"
#include "aether/platform/lifecycle.h"
#include "aether/platform/platform_plugin.h"
#include "aether/player/game.h"
#include "aether/player/scene_renderer.h"
#include "aether/player/user_paths.h"
#include "aether/platform/window.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace rhi = aether::gfx::rhi;

// A colour no Android default theme uses, so a screenshot shows whether the engine drew the frame.
constexpr rhi::ClearColor kClearColor{0.15f, 0.55f, 0.35f, 1.0f};
constexpr float kTriangleColor[4] = {0.90f, 0.45f, 0.10f, 1.0f};
constexpr float kPressedColor[4] = {0.20f, 0.35f, 0.90f, 1.0f};
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

std::mutex g_user_data_mutex;
std::string g_user_data_dir;
std::atomic<bool> g_scene_mode{false};

std::string UserDataDir() {
    std::lock_guard<std::mutex> lock(g_user_data_mutex);
    return g_user_data_dir;
}

class Renderer {
public:
    // From the UI thread; the render thread applies them at the start of its next frame.
    void QueueEvent(const aether::WindowEvent& event) {
        std::lock_guard<std::mutex> lock(events_mutex_);
        pending_events_.push_back(event);
    }

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
        if (g_scene_mode) {
            RunScene(window, width, height);
            ANativeWindow_release(window);
            return;
        }
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
            aether::input::InputState input;
            std::vector<aether::WindowEvent> events;
            int presented = 0;
            aether::u64 last_fence = 0;
            while (!stop_) {
                {
                    std::lock_guard<std::mutex> lock(events_mutex_);
                    events.swap(pending_events_);
                }
                aether::ApplyWindowEvents(events, input);
                events.clear();
                const float* color = input.IsDown(aether::input::Key::MouseLeft) ? kPressedColor : kTriangleColor;
                swap_chain->AcquireNextImage();
                cmd->Reset();
                cmd->BeginRenderPass(*swap_chain, kClearColor);
                cmd->BindPipeline(pipeline);
                cmd->SetPushConstants(color, sizeof(kTriangleColor));
                cmd->Draw(3);
                cmd->EndRenderPass();
                cmd->Close();
                last_fence = device->Submit(*cmd, swap_chain.get());
                device->WaitForFence(last_fence);
                swap_chain->Present(true);
                input.EndFrame();
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

    void DrainEvents(std::vector<aether::WindowEvent>& out) {
        std::lock_guard<std::mutex> lock(events_mutex_);
        out.swap(pending_events_);
    }

    // The cooked game in <files>/Paks, through the player runtime and SceneRenderer.
    void RunScene(ANativeWindow* window, int width, int height) {
        namespace player = aether::player;
        try {
            const std::string files = UserDataDir();
            const std::vector<std::string> paks = player::GamePackage::FindPaks(std::filesystem::path(files) / "Paks");
            if (paks.empty()) throw std::runtime_error("no .apak in " + files + "/Paks");
            player::GamePackage package;
            std::string error;
            for (size_t i = 0; i < paks.size(); ++i) {
                if (!package.Mount(paks[i], static_cast<int>(i), &error)) throw std::runtime_error(error);
            }
            if (!package.LoadManifest(&error)) throw std::runtime_error(error);
            player::Game game(package);
            game.UsePlatformLifecycle(aether::PlatformPlugins::Get().Lifecycle());
            game.SetUserPaths(player::ResolveUserPaths(package.Manifest().project, {}, files));
            if (!game.LoadStartupScene(&error)) throw std::runtime_error(error);
            Log(ANDROID_LOG_INFO, "AETHER_SCENE LOADED " + package.Manifest().project + " " + game.SceneName());

            std::unique_ptr<rhi::IDevice> device = rhi::CreateDevice(rhi::Backend::Vulkan, false);
            if (!device) throw std::runtime_error("no Vulkan device");
            std::unique_ptr<rhi::ISwapChain> swap_chain =
                device->CreateSwapChain(window, static_cast<aether::u32>(width), static_cast<aether::u32>(height), 2);
            const aether::u32 frame_count = std::max(swap_chain->BufferCount(), 1u);
            std::vector<std::unique_ptr<rhi::ICommandList>> commands;
            std::vector<aether::u64> fences(frame_count, 0);
            for (aether::u32 i = 0; i < frame_count; ++i) commands.push_back(device->CreateCommandList());
            player::SceneRenderer scene_renderer(*device, *swap_chain, package);
            game.BeginPlay();

            std::vector<aether::WindowEvent> events;
            auto last = std::chrono::steady_clock::now();
            aether::u32 slot = 0;
            int presented = 0;
            while (!stop_ && !game.ExitRequested()) {
                DrainEvents(events);
                aether::ApplyWindowEvents(events, game.Input());
                events.clear();
                const auto now = std::chrono::steady_clock::now();
                const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.25f);
                last = now;
                game.Tick(dt);
                device->WaitForFence(fences[slot]);
                swap_chain->AcquireNextImage();
                rhi::ICommandList& cmd = *commands[slot];
                cmd.Reset();
                scene_renderer.Draw(game, cmd, slot);
                cmd.Close();
                fences[slot] = device->Submit(cmd, swap_chain.get());
                swap_chain->Present(true);
                game.Input().EndFrame();
                slot = (slot + 1) % frame_count;
                if (++presented == kFramesToReport) {
                    Log(ANDROID_LOG_INFO, "AETHER_SCENE PRESENTED " + std::to_string(presented) + " " +
                                              std::to_string(swap_chain->Width()) + "x" +
                                              std::to_string(swap_chain->Height()));
                }
            }
            for (aether::u64 fence : fences) {
                if (fence != 0) device->WaitForFence(fence);
            }
            game.EndPlay();
        } catch (const std::exception& e) {
            Log(ANDROID_LOG_ERROR, std::string("AETHER_SCENE FAIL ") + e.what());
        }
    }

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex events_mutex_;
    std::vector<aether::WindowEvent> pending_events_;
};

const char* LifecycleName(const aether::platform::LifecycleMessage& m) {
    switch (m.event) {
        case aether::platform::LifecycleEvent::Suspend: return "Suspend";
        case aether::platform::LifecycleEvent::Resume: return "Resume";
        case aether::platform::LifecycleEvent::MemoryPressure:
            return m.level == aether::platform::MemoryLevel::Critical ? "MemoryPressure Critical" : "MemoryPressure";
    }
    return "?";
}

Renderer& TheRenderer() {
    static Renderer renderer;
    return renderer;
}

} // namespace

AETHER_PLATFORM_PLUGIN(android) {
    aether::PlatformPlugin p;
    p.name = "android";
    p.description = "Android app shell: Activity lifecycle and the app's files directory";
    p.connect_lifecycle = [](aether::platform::PlatformLifecycle& lifecycle) {
        lifecycle.Subscribe([](const aether::platform::LifecycleMessage& m) {
            Log(ANDROID_LOG_INFO, std::string("AETHER_LIFECYCLE ") + LifecycleName(m));
        });
    };
    p.user_data_dir = [] { return UserDataDir(); };
    return p;
}

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

extern "C" JNIEXPORT void JNICALL Java_com_aether_engine_MainActivity_nativeTouch(JNIEnv*, jclass, jint pointer_id,
                                                                                   jint action, jfloat x, jfloat y) {
    aether::WindowEvent event;
    event.type = action == 2 ? aether::WindowEventType::TouchMove : aether::WindowEventType::Touch;
    event.down = action == 0;
    event.x = x;
    event.y = y;
    event.pointer = static_cast<aether::u32>(pointer_id);
    if (action == 0) Log(ANDROID_LOG_INFO, "AETHER_INPUT DOWN " + std::to_string(static_cast<int>(x)) + " " +
                                               std::to_string(static_cast<int>(y)));
    TheRenderer().QueueEvent(event);
}

extern "C" JNIEXPORT void JNICALL Java_com_aether_engine_MainActivity_nativeSetUserDataDir(JNIEnv* env, jclass,
                                                                                            jstring path) {
    const char* chars = env->GetStringUTFChars(path, nullptr);
    {
        std::lock_guard<std::mutex> lock(g_user_data_mutex);
        g_user_data_dir = chars ? chars : "";
    }
    if (chars) env->ReleaseStringUTFChars(path, chars);
    aether::PlatformPlugins& plugins = aether::PlatformPlugins::Get();
    if (!plugins.IsStarted()) plugins.Startup();
    Log(ANDROID_LOG_INFO, "AETHER_USER_DATA " + plugins.UserDataDir("(none)"));
}

extern "C" JNIEXPORT void JNICALL Java_com_aether_engine_MainActivity_nativeLifecycle(JNIEnv*, jclass, jint event,
                                                                                       jboolean critical) {
    aether::platform::LifecycleMessage message;
    message.event = event == 0   ? aether::platform::LifecycleEvent::Suspend
                    : event == 1 ? aether::platform::LifecycleEvent::Resume
                                 : aether::platform::LifecycleEvent::MemoryPressure;
    message.level = critical ? aether::platform::MemoryLevel::Critical : aether::platform::MemoryLevel::Moderate;
    aether::PlatformPlugins::Get().Lifecycle().Dispatch(message);
}

extern "C" JNIEXPORT void JNICALL Java_com_aether_engine_MainActivity_nativeSetSceneMode(JNIEnv*, jclass,
                                                                                          jboolean scene) {
    g_scene_mode = scene != 0;
}
