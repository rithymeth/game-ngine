add_library(aether_engine STATIC
    src/core/log.cpp
    src/core/cvar.cpp
    src/core/console.cpp
    src/core/profiler.cpp
    src/debug/debug_draw.cpp
    src/debug/stats.cpp
    src/debug/crash.cpp
    src/testing/functional_test.cpp
    src/memory/linear_allocator.cpp
    src/memory/pool_allocator.cpp
    src/memory/frame_allocator.cpp
    src/platform/filesystem.cpp
    src/platform/window_events.cpp
    src/platform/platform_plugin.cpp
    src/platform/process.cpp
    src/pak/pak.cpp
    src/pak/vfs.cpp
    src/cook/cooker.cpp
    src/docs/api_docs.cpp
    src/cook/package.cpp
    src/plugin/plugin.cpp
    src/cook/texture_cook.cpp
    src/math/math.cpp
    src/job/job_system.cpp
    src/ecs/archetype.cpp
    src/scene/serialization.cpp
    src/scene/entity_guid.cpp
    src/scene/hierarchy.cpp
    src/scene/prefab.cpp
    src/scene/prefab_library.cpp
    src/scene/gameplay.cpp
    src/scene/lifecycle.cpp
    src/scene/scheduler.cpp
    src/scene/script_component.cpp
    src/input/input.cpp
    src/input/bindings.cpp
    src/project/project.cpp
    src/scene/model_assets.cpp
    src/reflection/any.cpp
    src/reflection/registry.cpp
    src/reflection/serialize.cpp
    src/assets/stb_image_impl.cpp
    src/assets/image.cpp
    src/assets/gltf_loader.cpp
    src/assets/asset_database.cpp
    src/assets/derived_data_cache.cpp
    src/assets/importer.cpp
    src/assets/texture_importer.cpp
    src/assets/model_importer.cpp
    src/assets/file_watcher.cpp
    src/assets/hot_reload.cpp
    src/assets/gltf_references.cpp
    src/assets/content_browser.cpp
)

target_include_directories(aether_engine PRIVATE ${stb_SOURCE_DIR})
# PUBLIC: reflection/serialize.h exposes nlohmann::json (the in-memory form
# of the JSON archive, and what schema migration hooks edit).
target_link_libraries(aether_engine PUBLIC nlohmann_json::nlohmann_json)
# .apak compression (Phase 25).
target_link_libraries(aether_engine PRIVATE lz4_static libzstd_static aether_bcenc)

if(WIN32)
    target_sources(aether_engine PRIVATE
        src/platform/window.cpp
        src/gfx/device.cpp
        src/gfx/swap_chain.cpp
        src/gfx/command_list.cpp
        src/gfx/shader_compiler.cpp
        src/gfx/descriptor_heap.cpp
        src/gfx/buffer.cpp
        src/gfx/texture.cpp
        src/gfx/material.cpp
        src/gfx/render_graph.cpp
        src/gfx/rhi/device_factory.cpp
        src/gfx/rhi/d3d12/d3d12_backend.cpp
        src/assets/asset_manager.cpp
    )
endif()

if(WIN32)
    target_compile_definitions(aether_engine PUBLIC AETHER_HAS_WINDOW)
elseif(AETHER_BUILD_WINDOWING)
    include(FetchContent)
    FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND ${AETHER_GLFW_WAYLAND} CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
    target_sources(aether_engine PRIVATE src/platform/window_glfw.cpp)
    target_link_libraries(aether_engine PRIVATE glfw)
    target_compile_definitions(aether_engine PUBLIC AETHER_HAS_WINDOW PRIVATE AETHER_HAS_GLFW)
    if(AETHER_GLFW_WAYLAND)
        target_compile_definitions(aether_engine PRIVATE AETHER_GLFW_WAYLAND)
    endif()
endif()

if(AETHER_BUILD_VULKAN)
    target_sources(aether_engine PRIVATE src/gfx/rhi/vulkan/vulkan_backend.cpp)
    if(NOT WIN32)
        # Off Windows: the RHI's device factory, and glslang for HLSL to
        # SPIR-V (DXC's role on Windows).
        target_sources(aether_engine PRIVATE src/gfx/rhi/device_factory.cpp src/gfx/shader_compiler_glslang.cpp)
        target_link_libraries(aether_engine PRIVATE ${AETHER_GLSLANG_LIBRARIES})
        # `VkFooInfo info{VK_STRUCTURE_TYPE_...}` leaves the rest zeroed on
        # purpose, which -Wextra reports member by member.
        set_source_files_properties(src/gfx/rhi/vulkan/vulkan_backend.cpp PROPERTIES
                                    COMPILE_OPTIONS -Wno-missing-field-initializers)
        # Distribution glslang builds have no RTTI, which UBSan's vptr check
        # (the ASan+UBSan CI job) would need for glslang's classes.
        set_source_files_properties(src/gfx/shader_compiler_glslang.cpp PROPERTIES
                                    COMPILE_OPTIONS -fno-sanitize=vptr)
    endif()
    # PUBLIC, not PRIVATE: rhi_demo includes vulkan_backend.h directly (the
    # same NativeHandle()-based escape hatch the D3D12 backend header already
    # supports), so Vulkan-Headers' include dir must propagate to consumers.
    target_include_directories(aether_engine PUBLIC ${AETHER_VULKAN_INCLUDE_DIRS})
    target_link_libraries(aether_engine PRIVATE ${AETHER_VULKAN_LIBRARY})
    target_compile_definitions(aether_engine PUBLIC AETHER_HAS_VULKAN)
endif()

target_include_directories(aether_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(aether_engine PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(aether_engine PUBLIC Threads::Threads)

if(WIN32)
    target_compile_definitions(aether_engine PUBLIC AETHER_PLATFORM_WINDOWS)
    target_link_libraries(aether_engine PUBLIC d3d12 dxgi d3dcompiler dxcompiler)

    # dxcompiler.lib resolves via the Windows SDK lib dirs already on the
    # MSVC linker's search path (vcvars), but unlike d3dcompiler_47.dll
    # (present in System32 on Win10/11), dxcompiler.dll does NOT ship as a
    # redistributable system DLL — it only exists inside the SDK's bin dir.
    # Anything that calls CompileHLSLToSPIRV at runtime needs a copy of it
    # next to the executable. Find it once here so consumers (rhi_demo) can
    # add a post-build copy step.
    #
    # Crucially, the Windows SDK's own dxcompiler.dll is a stripped build
    # with SPIR-V codegen DISABLED (confirmed by actually running the demo:
    # "SPIR-V CodeGen not available. Please recompile with
    # -DENABLE_SPIRV_CODEGEN=ON.") — it's fine for CompileHLSL's D3DCompile
    # path (unrelated DLL) but useless for CompileHLSLToSPIRV. When building
    # the Vulkan backend, fetch Microsoft's own official DXC GitHub release
    # build instead, which does have SPIR-V codegen enabled; it exports the
    # same DxcCreateInstance entry point the SDK's dxcompiler.lib import
    # library resolves at link time, so no relinking is needed — only the
    # runtime DLL changes.
    if(AETHER_BUILD_VULKAN)
        set(_aether_dxc_release_zip "${CMAKE_BINARY_DIR}/dxc_spirv/dxc_2026_07_29.zip")
        set(_aether_dxc_release_dir "${CMAKE_BINARY_DIR}/dxc_spirv/extracted")
        if(NOT EXISTS "${_aether_dxc_release_dir}/bin/x64/dxcompiler.dll")
            file(DOWNLOAD
                "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/dxc_2026_07_29.zip"
                "${_aether_dxc_release_zip}"
                EXPECTED_HASH SHA256=a1dfb116ba3eeae6a1582291b53a8e7bf65ad760676bd3194685c8f7367cd241
                SHOW_PROGRESS
            )
            file(ARCHIVE_EXTRACT INPUT "${_aether_dxc_release_zip}" DESTINATION "${_aether_dxc_release_dir}")
        endif()
        if(EXISTS "${_aether_dxc_release_dir}/bin/x64/dxcompiler.dll")
            set(AETHER_DXCOMPILER_DLL "${_aether_dxc_release_dir}/bin/x64/dxcompiler.dll" CACHE FILEPATH "" FORCE)
            message(STATUS "dxcompiler.dll (SPIR-V-capable, from GitHub release): ${AETHER_DXCOMPILER_DLL}")
        else()
            message(WARNING "Failed to fetch SPIR-V-capable dxcompiler.dll; falling back to the SDK's (no SPIR-V)")
        endif()
    endif()

    if(NOT AETHER_DXCOMPILER_DLL)
        find_file(AETHER_DXCOMPILER_DLL
            NAMES dxcompiler.dll
            PATHS "$ENV{WindowsSdkDir}bin/$ENV{WindowsSDKVersion}x64"
                  "$ENV{WindowsSdkDir}bin/$ENV{WindowsSDKVersion}"
            PATH_SUFFIXES x64
        )
        if(NOT AETHER_DXCOMPILER_DLL)
            file(GLOB _aether_dxcompiler_candidates
                "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxcompiler.dll")
            if(_aether_dxcompiler_candidates)
                list(SORT _aether_dxcompiler_candidates)
                list(POP_BACK _aether_dxcompiler_candidates _aether_dxcompiler_latest)
                set(AETHER_DXCOMPILER_DLL "${_aether_dxcompiler_latest}" CACHE FILEPATH "" FORCE)
            endif()
        endif()
        if(AETHER_DXCOMPILER_DLL)
            message(STATUS "dxcompiler.dll (SDK, no SPIR-V): ${AETHER_DXCOMPILER_DLL}")
        endif()
    endif()

    if(NOT AETHER_DXCOMPILER_DLL)
        message(WARNING "dxcompiler.dll not found; SPIR-V/DXIL shader compilation will fail at runtime")
    endif()
elseif(APPLE)
    target_compile_definitions(aether_engine PUBLIC AETHER_PLATFORM_MACOS)
else()
    target_compile_definitions(aether_engine PUBLIC AETHER_PLATFORM_LINUX)
endif()

add_library(Aether::Engine ALIAS aether_engine)

# Phase 23: profiler zones can also feed Tracy (https://github.com/wolfpld/tracy).
# Off by default; when on, Tracy's client is fetched and AETHER_PROFILE_ZONE
# records into both the engine's profiler and Tracy.
option(AETHER_TRACY "Forward profiler zones to Tracy" OFF)
if(AETHER_TRACY)
    include(FetchContent)
    FetchContent_Declare(tracy GIT_REPOSITORY https://github.com/wolfpld/tracy.git GIT_TAG v0.11.1 GIT_SHALLOW TRUE)
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(tracy)
    target_link_libraries(aether_engine PUBLIC Tracy::TracyClient)
    target_compile_definitions(aether_engine PUBLIC AETHER_TRACY)
endif()

# Phase 23: the crash handler writes minidumps on Windows.
if(WIN32)
    target_link_libraries(aether_engine PRIVATE dbghelp)
endif()

# Phase 25 step 5: packaging finds the player this engine was built with
# (player/), when there's none beside the running executable.
target_compile_definitions(aether_engine PRIVATE AETHER_PLAYER_BUILD_PATH="$<TARGET_FILE:aether_player>")
# Phase 26 step 1: the engine's own plugins (plugins/: physics, audio, AI,
# networking), found by PluginManager::EnginePluginsDir().
target_compile_definitions(aether_engine PRIVATE AETHER_ENGINE_PLUGINS_DIR="${CMAKE_SOURCE_DIR}/plugins")
