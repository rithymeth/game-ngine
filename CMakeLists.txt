cmake_minimum_required(VERSION 3.20)
project(Aether LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CMake 4 refuses projects declaring compatibility with CMake < 3.5, as some
# fetched dependencies (recastnavigation) still do; let them configure.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Oldest CMake policy version dependencies may ask for")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

option(AETHER_BUILD_TESTS "Build Aether unit tests" ON)
option(AETHER_BUILD_MCP "Build the MCP (Model Context Protocol) editor server" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /arch:AVX2 /Zc:__cplusplus)
    # Over-aligned types (16-byte math types) in std::stable_sort's temporary
    # buffers instantiate aligned_storage with an extended alignment; opt in
    # to the conforming behaviour rather than MSVC's old static_assert.
    add_compile_definitions(_ENABLE_EXTENDED_ALIGNED_STORAGE)
else()
    add_compile_options(-Wall -Wextra)
endif()

# x86 builds target AVX2/FMA. ARM builds (Apple Silicon, Android) take the
# same SSE intrinsics through sse2neon, which maps them to NEON.
set(_aether_arch "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(_aether_arch "${CMAKE_OSX_ARCHITECTURES}")
endif()
if(_aether_arch MATCHES "^(arm64|aarch64|ARM64)")
    include(FetchContent)
    FetchContent_Declare(sse2neon
        GIT_REPOSITORY https://github.com/DLTcollab/sse2neon.git
        GIT_TAG v1.9.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(sse2neon)
    include_directories(SYSTEM ${sse2neon_SOURCE_DIR})
elseif(NOT MSVC)
    add_compile_options(-mavx2 -mfma)
endif()

option(AETHER_BUILD_SANDBOX "Build the Aether sandbox app" ON)
option(AETHER_BUILD_PHYSICS "Build the Jolt-backed physics module" ON)
option(AETHER_BUILD_EDITOR "Build the Dear ImGui editor app" ON)
option(AETHER_BUILD_EDITOR_UI "Build the portable editor UI library (ImGui + reflection, no graphics backend; tested headless on every platform)" ON)
option(AETHER_BUILD_VULKAN "Build the Vulkan RHI backend" ON)
option(AETHER_BUILD_SCRIPTING "Build the Luau scripting module" ON)
# Phase 24: windows on Linux and macOS through GLFW (Windows uses Win32).
# Needs the X11 development headers on Linux (libx11-dev, libxrandr-dev,
# libxinerama-dev, libxcursor-dev, libxi-dev).
option(AETHER_BUILD_WINDOWING "Build the GLFW window backend on Linux and macOS" ON)
option(AETHER_GLFW_WAYLAND "Also build GLFW's Wayland backend (needs the Wayland development packages)" OFF)

include(FetchContent)

# Single-header stb_image (nothings/stb) for the asset pipeline's image
# decoding (PNG/JPG/BMP -> RGBA8). No build system of its own — just the
# header, wrapped by engine/src/assets/stb_image_impl.cpp to provide the
# implementation once.
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(stb)

# nlohmann/json for the glTF loader's .gltf JSON parsing — a real,
# well-tested parser (spec-correct string escaping/unicode/number handling)
# rather than a hand-rolled one just for this.
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# Phase 25: compression for .apak archives - LZ4 (fast) and zstd (small).
# Both are BSD-licensed and built static with no command-line tools.
set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG v1.10.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake
)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.7
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake
)
# Phase 25: block-compressed textures (BC1-BC5 through rgbcx, BC7 through
# bc7enc, with their decoders) - Rich Geldreich's bc7enc_rdo, MIT. Only the
# library sources are built (its own CMakeLists builds command-line tools).
FetchContent_Declare(
    bc7enc
    GIT_REPOSITORY https://github.com/richgel999/bc7enc_rdo.git
    GIT_TAG b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
    SOURCE_SUBDIR do-not-build
)
FetchContent_MakeAvailable(bc7enc)
add_library(aether_bcenc STATIC
    ${bc7enc_SOURCE_DIR}/bc7enc.cpp
    ${bc7enc_SOURCE_DIR}/bc7decomp.cpp
    ${bc7enc_SOURCE_DIR}/rgbcx.cpp
)
target_include_directories(aether_bcenc SYSTEM PUBLIC ${bc7enc_SOURCE_DIR})
if(MSVC)
    target_compile_options(aether_bcenc PRIVATE /w)
else()
    target_compile_options(aether_bcenc PRIVATE -w)
endif()

set(_aether_shared_libs ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(lz4 zstd)
set(BUILD_SHARED_LIBS ${_aether_shared_libs})

if(AETHER_BUILD_PHYSICS)
    # Jolt defaults to the static MSVC runtime (/MT) on MSVC; our own targets
    # use the default dynamic runtime (/MD), and linking a /MT static lib
    # into a /MD executable is a hard CRT conflict (duplicate symbols). Force
    # Jolt onto the dynamic runtime to match before it configures.
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
    # Jolt's GPU compute (hair, soft bodies on the GPU) isn't used; with the
    # Vulkan SDK half-installed (loader and headers but no glslc/dxc, as on
    # Linux CI) it would still try to compile its shaders.
    set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
    # Jolt builds without C++ RTTI by default, but JoltJobSystemAdapter derives
    # from JPH::JobSystemWithBarrier in our own (RTTI-enabled) code, so its
    # vtable references Jolt's typeinfo. MSVC tolerates the mismatch; GCC and
    # Clang fail to link ("undefined reference to typeinfo for
    # JPH::JobSystemWithBarrier"). Build Jolt with RTTI to match.
    set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
        JoltPhysics
        GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
        GIT_TAG v5.6.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR Build
    )
    FetchContent_MakeAvailable(JoltPhysics)
endif()

if(AETHER_BUILD_EDITOR OR AETHER_BUILD_EDITOR_UI)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.9
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(imgui)

    # Dear ImGui doesn't ship a CMakeLists.txt (it's meant to be dropped into
    # the consumer's build), so we define the libraries ourselves. imgui_core
    # is platform-independent (no window or graphics backend): the portable
    # editor UI library and its headless tests use it on every platform.
    add_library(imgui_core STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    )
    target_include_directories(imgui_core PUBLIC ${imgui_SOURCE_DIR})
endif()

if(AETHER_BUILD_EDITOR AND WIN32)
    # The Win32 + DX12 backends, matching this project's window/graphics layer.
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
    )
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC imgui_core d3d12 dxgi)
    add_library(ImGui::ImGui ALIAS imgui)
endif()

if(AETHER_BUILD_VULKAN AND WIN32)
    FetchContent_Declare(
        VulkanHeaders
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG v1.4.309
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(VulkanHeaders)

    # Prefer a real Vulkan SDK if one is installed (VULKAN_SDK env var or the
    # usual install locations). If not, this machine's GPU driver still
    # ships the loader DLL (C:\Windows\System32\vulkan-1.dll) — generate an
    # import library from it directly rather than requiring the ~1GB SDK
    # download just to get a .lib file. Either way we only need the headers
    # (fetched above) and something to link against; the loader itself
    # resolves ICD/layers at runtime regardless of how we linked it.
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        set(AETHER_VULKAN_INCLUDE_DIRS ${Vulkan_INCLUDE_DIRS})
        set(AETHER_VULKAN_LIBRARY ${Vulkan_LIBRARY})
        message(STATUS "Vulkan: using installed SDK (${Vulkan_LIBRARY})")
    else()
        set(_aether_vulkan_dll "C:/Windows/System32/vulkan-1.dll")
        find_program(AETHER_DUMPBIN_EXE dumpbin)
        find_program(AETHER_LIB_EXE lib)
        if(EXISTS ${_aether_vulkan_dll} AND AETHER_DUMPBIN_EXE AND AETHER_LIB_EXE)
            set(_aether_vulkan_lib_dir "${CMAKE_BINARY_DIR}/vulkan_lib")
            execute_process(
                COMMAND powershell -ExecutionPolicy Bypass -File
                        "${CMAKE_SOURCE_DIR}/tools/generate_vulkan_import_lib.ps1"
                        -DllPath ${_aether_vulkan_dll}
                        -OutputDir ${_aether_vulkan_lib_dir}
                        -DumpbinPath ${AETHER_DUMPBIN_EXE}
                        -LibPath ${AETHER_LIB_EXE}
                RESULT_VARIABLE _aether_vulkan_gen_result
            )
            if(_aether_vulkan_gen_result EQUAL 0 AND EXISTS "${_aether_vulkan_lib_dir}/vulkan-1.lib")
                set(AETHER_VULKAN_INCLUDE_DIRS "${vulkanheaders_SOURCE_DIR}/include")
                set(AETHER_VULKAN_LIBRARY "${_aether_vulkan_lib_dir}/vulkan-1.lib")
                message(STATUS "Vulkan: no SDK found; generated an import lib from the driver's loader DLL")
            else()
                message(WARNING "Vulkan: failed to generate import lib from ${_aether_vulkan_dll}; Vulkan backend disabled")
                set(AETHER_BUILD_VULKAN OFF)
            endif()
        else()
            message(WARNING "Vulkan: no SDK and no system vulkan-1.dll/dumpbin/lib found; Vulkan backend disabled")
            set(AETHER_BUILD_VULKAN OFF)
        endif()
    endif()
elseif(AETHER_BUILD_VULKAN)
    # Linux and macOS: the system Vulkan loader and headers (libvulkan-dev),
    # and glslang (glslang-dev) to compile HLSL to SPIR-V. Without either,
    # the Vulkan backend is left out and everything else builds as before.
    find_package(Vulkan QUIET)
    find_package(glslang CONFIG QUIET)
    if(Vulkan_FOUND AND glslang_FOUND)
        set(AETHER_VULKAN_INCLUDE_DIRS ${Vulkan_INCLUDE_DIRS})
        set(AETHER_VULKAN_LIBRARY ${Vulkan_LIBRARY})
        set(AETHER_GLSLANG_LIBRARIES glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
        message(STATUS "Vulkan: ${Vulkan_LIBRARY}, shaders through glslang ${glslang_VERSION}")
    else()
        message(STATUS "Vulkan: backend off (needs the Vulkan loader and headers, and glslang)")
        set(AETHER_BUILD_VULKAN OFF)
    endif()
else()
    set(AETHER_BUILD_VULKAN OFF)
endif()

add_subdirectory(engine)
# Platform plugins (platforms/README.md): directories listed here are built
# and linked into the tests, the functional runner and the editor shell.
set(AETHER_PLATFORM_PLUGINS "" CACHE STRING "Platform plugin directories (;-separated)")
include(cmake/AetherPlatform.cmake)
include(cmake/AetherModules.cmake)
aether_add_platform_plugins()
# Blueprints (Phase 12): graph model, compiler and VM; engine-only, no fetches.
add_subdirectory(blueprint)
# Phase 26 step 2: project templates (they place Blueprints).
add_subdirectory(templates)
# The renderer's CPU side (Phase 14); engine-only, no fetches.
add_subdirectory(renderer)
# Animation (Phase 16); engine-only, no fetches.
add_subdirectory(animation)
# dr_flac (public domain / MIT-0, mackron/dr_libs) for FLAC decoding in the
# audio library; stb_vorbis (from the stb fetch above) handles Ogg Vorbis.
# Header-only, compiled once in audio/src/decoders.cpp.
FetchContent_Declare(
    dr_libs
    GIT_REPOSITORY https://github.com/mackron/dr_libs.git
    GIT_TAG dfe8377631000664666519fdb83da193fd8037f4
    GIT_SHALLOW FALSE
    # Headers only: a subdirectory without a CMakeLists.txt keeps its own
    # build (tests, sample programs) out of ours.
    SOURCE_SUBDIR headers_only
)
FetchContent_MakeAvailable(dr_libs)
# miniaudio (public domain / MIT-0, mackron/miniaudio) for the audio output
# device (Phase 17 step 5). Header only, compiled once in
# audio/src/miniaudio_impl.cpp; it loads the platform's audio API at run time.
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.25
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR headers_only
)
FetchContent_MakeAvailable(miniaudio)
# Audio (Phase 17): the mixer, 3D, decoders, streaming, sound cues,
# components and the output device.
add_subdirectory(audio)
# Runtime game UI (Phase 18): widgets, layout, input and a draw list; engine-only.
add_subdirectory(ui)
# Particle systems (Phase 19): emitters, modules and the simulation; engine-only.
add_subdirectory(vfx)
# Terrain (Phase 21): heightmap-based terrain with chunked meshes and LOD.
add_subdirectory(terrain)
add_subdirectory(streaming)
add_subdirectory(sprite2d)
add_subdirectory(sequencer)
# Save games (Phase 28): save slots, versioned and atomic; engine-only.
add_subdirectory(save)

# Localization (Phase 29): LocText, string tables and message formatting; engine-only.
add_subdirectory(loc)

# Gameplay ability system and RPG toolkit (Phase 30): tags, attributes, effects, abilities; engine-only.
add_subdirectory(gameplay)
add_subdirectory(net)

# Recast/Detour (zlib, recastnavigation/recastnavigation) for navigation
# meshes, pathfinding and crowds (Phase 20). Only the libraries.
set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_DT_VIRTUAL_QUERYFILTER ON CACHE BOOL "" FORCE) # nav: per-area exclusion
FetchContent_Declare(
    recastnavigation
    GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
    GIT_TAG v1.6.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(recastnavigation)
# AI and navigation (Phase 20): navmeshes, agents, behaviour trees, perception.
add_subdirectory(nav)
add_subdirectory(ai)

if(AETHER_BUILD_SCRIPTING)
    # Luau (MIT, luau-lang/luau): the scripting VM and compiler (Phase 11).
    # Only the libraries; not its command-line tools or tests.
    set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
    set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        luau
        GIT_REPOSITORY https://github.com/luau-lang/luau.git
        GIT_TAG 0.740
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(luau)
    # Third-party code: don't hold it to our warning flags.
    foreach(luau_target Luau.Common Luau.Ast Luau.Compiler Luau.VM Luau.Bytecode)
        if(TARGET ${luau_target})
            if(MSVC)
                target_compile_options(${luau_target} PRIVATE /w)
            else()
                target_compile_options(${luau_target} PRIVATE -w)
            endif()
        endif()
    endforeach()
    # Only the VM and compiler are used; keep the rest (type checker, code
    # generator, ...) out of the default build.
    foreach(luau_target Luau.Analysis Luau.CodeGen Luau.Config Luau.Require Luau.RequireNavigator Luau.CLI.lib Luau.EqSat Luau.Inliner)
        if(TARGET ${luau_target})
            set_target_properties(${luau_target} PROPERTIES EXCLUDE_FROM_ALL ON)
        endif()
    endforeach()
    add_subdirectory(scripting)
endif()

if(AETHER_BUILD_PHYSICS)
    add_subdirectory(physics)
endif()

if(AETHER_BUILD_EDITOR_UI OR AETHER_BUILD_MCP)
    add_subdirectory(editor/src/core)
endif()

if(AETHER_BUILD_EDITOR_UI)
    add_subdirectory(editor/src/ui)
endif()

if(AETHER_BUILD_MCP)
    add_subdirectory(mcp)
endif()

add_subdirectory(samples/coin_run)
add_subdirectory(samples/gem_hop)

# Phase 24 step 5: the portable editor shell - the editor's panels in a
# GLFW (or Win32) window, or offscreen, drawn through the Vulkan RHI.
if(AETHER_BUILD_EDITOR_UI AND AETHER_BUILD_VULKAN)
    add_subdirectory(editor/shell)
endif()

# Phase 26 step 1: the engine's own plugins (their modules).
add_subdirectory(plugins)

# Phase 25 step 4: the player (the game without the editor).
add_subdirectory(player)

if(AETHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Phase 23: the functional test runner (headless, any platform).
add_subdirectory(tools/functional)
# Phase 25: the .apak archive tool and the cooker.
add_subdirectory(tools/pak)
add_subdirectory(tools/cook)
add_subdirectory(tools/docgen)
add_subdirectory(tools/loc)

if(AETHER_BUILD_SANDBOX)
    add_subdirectory(sandbox)
endif()

if(AETHER_BUILD_PHYSICS AND AETHER_BUILD_EDITOR AND WIN32)
    add_subdirectory(editor)
endif()

if(WIN32)
    add_subdirectory(rhi_demo)
    add_subdirectory(pbr_demo)
    add_subdirectory(gltf_demo)
endif()

# Phase 26 step 1: the executables that host plugins get every module this
# build has (each registers itself; the project's plugins decide which start).
foreach(host aether_player aether_editor_shell aether_editor aether_tests)
    if(TARGET ${host})
        aether_link_modules(${host})
    endif()
endforeach()
