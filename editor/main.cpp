// Phase 5 editor: a Dear ImGui overlay on top of the Phase 3/4 D3D12
// platform layer, driving a live ECS + Jolt physics simulation. Every body
// is an entity with Transform + RigidBody; the render loop reads Transform
// each frame to place an instanced colored quad — no bindless textures or
// GPU-driven culling here (Phase 4 already proved those out), just plain
// DrawInstanced, so the new surface area this phase adds (ImGui, physics,
// serialization) stays easy to follow.
//
// Panels: live entity list, spawn/reset controls, play/pause, and
// Save/Load Scene buttons exercising the Phase 5 serializer. Loading
// reconnects each RigidBody entity to a freshly-created Jolt body (the
// serializer deliberately does not — and cannot — persist a live body
// handle; see aether::RegisterPhysicsComponentSerializers).
//
// Set AETHER_EDITOR_MAX_FRAMES=<N> to auto-close after N frames instead of
// waiting for the window to be closed, for scripted/automated verification.

#include "aether/core/log.h"
#include "aether/gfx/buffer.h"
#include "aether/gfx/command_list.h"
#include "aether/gfx/descriptor_heap.h"
#include "aether/gfx/device.h"
#include "aether/gfx/render_graph.h"
#include "aether/gfx/shader_compiler.h"
#include "aether/gfx/swap_chain.h"
#include "aether/math/math.h"
#include "aether/physics/physics_world.h"
#include "aether/platform/window.h"
#include "aether/scene/serialization.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

using namespace aether;
using namespace aether::gfx;

// imgui_impl_win32.h intentionally comments this declaration out (to avoid
// forcing <windows.h> on everyone who includes it) and asks callers to copy
// it in verbatim.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr u32 kMaxInstances = 256;
constexpr const char* kScenePath = "editor_scene.aesc";

struct EditorInstance {
    f32 center[3];
    f32 radius;
    f32 color[3];
    f32 pad;
};

constexpr const char* kShaderSource = R"(
struct InstanceData {
    float3 center;
    float radius;
    float3 color;
    float _pad;
};
StructuredBuffer<InstanceData> g_Instances : register(t0);
cbuffer ViewProj : register(b0) { float4x4 g_ViewProj; };

struct PSInput {
    float4 position : SV_POSITION;
    float3 color : COLOR;
};

static const float2 kQuadPositions[6] = {
    float2(-0.5, -0.5), float2(-0.5, 0.5), float2(0.5, -0.5),
    float2(0.5, -0.5), float2(-0.5, 0.5), float2(0.5, 0.5),
};

PSInput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) {
    InstanceData inst = g_Instances[instanceID];
    float2 localPos = kQuadPositions[vertexID] * inst.radius;
    float3 worldPos = inst.center + float3(localPos, 0.0);

    PSInput result;
    result.position = mul(g_ViewProj, float4(worldPos, 1.0));
    result.color = inst.color;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return float4(input.color, 1.0);
}
)";

ComPtr<ID3D12RootSignature> CreateRootSignature(Device& device) {
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor = {0, 0};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(params);
    desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_FATAL("D3D12", "Root signature serialization failed: %s", message);
        throw std::runtime_error("root signature serialization failed");
    }
    ComPtr<ID3D12RootSignature> root_signature;
    AETHER_D3D_CHECK(device.Handle()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                           IID_PPV_ARGS(&root_signature)));
    return root_signature;
}

ComPtr<ID3D12PipelineState> CreatePSO(Device& device, ID3D12RootSignature* root_signature, DXGI_FORMAT rtv_format) {
    ShaderBytecode vs = CompileHLSL(kShaderSource, "VSMain", "vs_5_0", "editor_vs");
    ShaderBytecode ps = CompileHLSL(kShaderSource, "PSMain", "ps_5_0", "editor_ps");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.VS = {vs.Data(), vs.Size()};
    desc.PS = {ps.Data(), ps.Size()};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = rtv_format;
    desc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    AETHER_D3D_CHECK(device.Handle()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
    return pso;
}

Entity SpawnSphere(World& world, PhysicsWorld& physics, const Vec3& position, f32 radius, f32 mass) {
    JPH::BodyID body_id = physics.CreateSphere(position, radius, mass, /*is_static=*/false);
    RigidBody body;
    body.body_id = body_id;
    body.radius = radius;
    body.mass = mass;
    body.is_static = false;
    return world.CreateEntity(Transform{position, Quaternion::Identity()}, body);
}

} // namespace

int main() {
    i32 max_frames = -1;
    if (const char* env = std::getenv("AETHER_EDITOR_MAX_FRAMES")) {
        max_frames = std::atoi(env);
    }

    try {
        RegisterPhysicsComponentSerializers();

        WindowDesc window_desc;
        window_desc.title = "Aether Editor - Phase 5";
        window_desc.width = 1280;
        window_desc.height = 800;
        Window window(window_desc);

        Device device(/*enable_debug_layer=*/true);
        SwapChain swap_chain(device, window.NativeHandle(), window.Width(), window.Height());
        window.on_resize = [&](u32 w, u32 h) { swap_chain.Resize(w, h); };

        window.native_message_hook = [](void* hwnd, u32 msg, u64 wparam, i64 lparam) {
            ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), msg, static_cast<WPARAM>(wparam),
                                            static_cast<LPARAM>(lparam));
        };

        // A dedicated heap for ImGui's own SRVs (font atlas plus whatever
        // dynamic textures 1.92's texture-update system needs), kept
        // separate from any future bindless texture heap so the two
        // allocation schemes never collide. Sized generously since this
        // backend version can allocate more than just the one font
        // descriptor older versions needed.
        DescriptorHeap imgui_srv_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, /*shader_visible=*/true);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(window.NativeHandle());

        ImGui_ImplDX12_InitInfo init_info{};
        init_info.Device = device.Handle();
        init_info.CommandQueue = device.Queue();
        init_info.NumFramesInFlight = static_cast<int>(swap_chain.BufferCount());
        init_info.RTVFormat = swap_chain.Format();
        init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        init_info.SrvDescriptorHeap = imgui_srv_heap.Heap();
        init_info.UserData = &imgui_srv_heap;
        init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
                                             D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
            auto* heap = static_cast<DescriptorHeap*>(info->UserData);
            u32 slot = heap->Allocate();
            *out_cpu = heap->CPUHandle(slot);
            *out_gpu = heap->GPUHandle(slot);
        };
        init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE,
                                            D3D12_GPU_DESCRIPTOR_HANDLE) {
            // No-op: this demo never unloads ImGui textures during its
            // lifetime, so leaking the (tiny, fixed-capacity) heap slot back
            // to DescriptorHeap isn't needed. A long-running editor reusing
            // dynamic textures would want a real CPU<->GPU handle lookup here.
        };
        ImGui_ImplDX12_Init(&init_info);

        ComPtr<ID3D12RootSignature> root_signature = CreateRootSignature(device);
        ComPtr<ID3D12PipelineState> pso = CreatePSO(device, root_signature.Get(), swap_chain.Format());

        Buffer view_proj_buffer(device, sizeof(f32) * 16, BufferKind::Upload);
        Buffer instance_buffer(device, sizeof(EditorInstance) * kMaxInstances, BufferKind::Upload);

        World world;
        PhysicsWorld physics;
        physics.CreateBox(Vec3(0, 0, 0), Vec3(10, 0.5f, 10));

        std::mt19937 rng(1234);
        std::uniform_real_distribution<f32> spread(-3.0f, 3.0f);
        std::uniform_real_distribution<f32> height(4.0f, 10.0f);
        for (int i = 0; i < 8; ++i) {
            SpawnSphere(world, physics, Vec3(spread(rng), height(rng), spread(rng)), 0.5f, 1.0f);
        }

        bool playing = true;
        std::vector<std::unique_ptr<CommandList>> command_lists;
        std::vector<u64> frame_fences(swap_chain.BufferCount(), 0);
        for (u32 i = 0; i < swap_chain.BufferCount(); ++i) {
            command_lists.push_back(std::make_unique<CommandList>(device));
        }

        RenderGraph graph;
        AETHER_LOG_INFO("Editor", "Entering main loop");

        i32 frame_index = 0;
        while (window.PumpMessages()) {
            if (window.IsMinimized()) {
                continue;
            }

            constexpr f32 kDt = 1.0f / 60.0f;
            if (playing) {
                SyncPhysicsToTransforms(world, physics, kDt);
            }

            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            std::vector<EditorInstance> instances;
            world.ForEach<Transform, RigidBody>([&](Transform& t, RigidBody& b) {
                if (instances.size() >= kMaxInstances) {
                    return;
                }
                EditorInstance inst{};
                inst.center[0] = t.position.x;
                inst.center[1] = t.position.y;
                inst.center[2] = t.position.z;
                inst.radius = b.radius;
                inst.color[0] = 0.3f;
                inst.color[1] = 0.6f + 0.05f * static_cast<f32>(instances.size() % 5);
                inst.color[2] = 0.9f;
                instances.push_back(inst);
            });

            ImGui::Begin("Aether Editor");
            ImGui::Text("Entities: %zu", world.EntityCount());
            ImGui::Checkbox("Playing", &playing);
            if (ImGui::Button("Spawn Sphere")) {
                SpawnSphere(world, physics, Vec3(spread(rng), 8.0f, spread(rng)), 0.5f, 1.0f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Save Scene")) {
                SaveScene(world, kScenePath);
            }
            ImGui::SameLine();
            if (ImGui::Button("Load Scene")) {
                World loaded;
                if (LoadScene(loaded, kScenePath)) {
                    world = std::move(loaded);
                    // The serializer never persists a live Jolt body handle
                    // (see RegisterPhysicsComponentSerializers) — reconnect
                    // every loaded RigidBody to a fresh body in this
                    // PhysicsWorld, seeded from its Transform + saved shape.
                    world.ForEach<Transform, RigidBody>([&](Transform& t, RigidBody& b) {
                        b.body_id = physics.CreateSphere(t.position, b.radius, b.mass, b.is_static);
                    });
                }
            }

            ImGui::Separator();
            ImGui::Text("Bodies (live-edit; changes apply to the running simulation immediately)");
            int index = 0;
            world.ForEach<Transform, RigidBody>([&](Transform& t, RigidBody& b) {
                ImGui::PushID(index);
                ImGui::Text("#%d", index);

                bool position_changed = ImGui::DragFloat3("Position", &t.position.x, 0.05f);
                bool radius_changed = ImGui::DragFloat("Radius", &b.radius, 0.01f, 0.05f, 5.0f, "%.2f");
                bool mass_changed = ImGui::DragFloat("Mass", &b.mass, 0.05f, 0.01f, 100.0f, "%.2f");
                bool static_changed = ImGui::Checkbox("Static", &b.is_static);

                if (position_changed) {
                    // Only re-teleport the body if the shape/mass didn't also
                    // change this frame — that branch below already recreates
                    // it at the (already updated) Transform position.
                    if (!radius_changed && !mass_changed && !static_changed) {
                        physics.SetPosition(b.body_id, t.position);
                    }
                }
                if (radius_changed || mass_changed || static_changed) {
                    // Jolt shapes and motion type are effectively immutable
                    // once a body is created; the simplest correct way to
                    // "edit" them live is to recreate the body in place.
                    physics.DestroyBody(b.body_id);
                    b.body_id = physics.CreateSphere(t.position, b.radius, b.mass, b.is_static);
                }

                ImGui::Separator();
                ImGui::PopID();
                ++index;
            });
            ImGui::End();

            ImGui::Render();

            Mat4 view = Mat4::LookAtRH(Vec3(0, 6, -14), Vec3(0, 1, 0), Vec3(0, 1, 0));
            Mat4 proj = Mat4::PerspectiveRH(Radians(60.0f),
                                             static_cast<f32>(swap_chain.Width()) / static_cast<f32>(swap_chain.Height()),
                                             0.1f, 100.0f);
            Mat4 view_proj = proj * view;
            view_proj_buffer.Update(&view_proj, sizeof(f32) * 16);
            if (!instances.empty()) {
                instance_buffer.Update(instances.data(), sizeof(EditorInstance) * instances.size());
            }

            u32 buffer_index = swap_chain.CurrentBackBufferIndex();
            device.WaitForFence(frame_fences[buffer_index]);

            CommandList& cmd = *command_lists[buffer_index];
            cmd.Reset();

            ID3D12Resource* back_buffer = swap_chain.CurrentBackBuffer();
            RenderGraph::ResourceHandle backbuffer_handle =
                graph.ImportResource(back_buffer, D3D12_RESOURCE_STATE_PRESENT, "BackBuffer");

            graph.AddPass(
                "EditorForward",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Write(backbuffer_handle, D3D12_RESOURCE_STATE_RENDER_TARGET);
                },
                [&](ID3D12GraphicsCommandList* cl) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rtv = swap_chain.CurrentBackBufferRTV();
                    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                    const f32 clear_color[4] = {0.03f, 0.03f, 0.05f, 1.0f};
                    cl->ClearRenderTargetView(rtv, clear_color, 0, nullptr);

                    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<f32>(swap_chain.Width()),
                                             static_cast<f32>(swap_chain.Height()), 0.0f, 1.0f};
                    D3D12_RECT scissor{0, 0, static_cast<LONG>(swap_chain.Width()),
                                        static_cast<LONG>(swap_chain.Height())};
                    cl->RSSetViewports(1, &viewport);
                    cl->RSSetScissorRects(1, &scissor);

                    if (!instances.empty()) {
                        cl->SetPipelineState(pso.Get());
                        cl->SetGraphicsRootSignature(root_signature.Get());
                        cl->SetGraphicsRootConstantBufferView(0, view_proj_buffer.GPUAddress());
                        cl->SetGraphicsRootShaderResourceView(1, instance_buffer.GPUAddress());
                        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        cl->DrawInstanced(6, static_cast<UINT>(instances.size()), 0, 0);
                    }

                    ID3D12DescriptorHeap* imgui_heaps[] = {imgui_srv_heap.Heap()};
                    cl->SetDescriptorHeaps(1, imgui_heaps);
                    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
                });

            graph.Execute(cmd.Get());
            graph.Reset();

            cmd.Close();
            ID3D12CommandList* lists[] = {cmd.Get()};
            frame_fences[buffer_index] = device.Submit(lists, 1);

            swap_chain.Present(/*vsync=*/true);

            ++frame_index;
            if (max_frames >= 0 && frame_index >= max_frames) {
                AETHER_LOG_INFO("Editor", "Reached AETHER_EDITOR_MAX_FRAMES=%d, exiting", max_frames);
                break;
            }
        }

        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        AETHER_LOG_INFO("Editor", "Shutting down cleanly");
    } catch (const std::exception& e) {
        AETHER_LOG_FATAL("Editor", "Unhandled exception: %s", e.what());
        return 1;
    }

    return 0;
}
