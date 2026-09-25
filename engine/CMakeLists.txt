add_library(aether_engine STATIC
    src/core/log.cpp
    src/memory/linear_allocator.cpp
    src/memory/pool_allocator.cpp
    src/memory/frame_allocator.cpp
    src/platform/filesystem.cpp
    src/math/math.cpp
    src/job/job_system.cpp
    src/ecs/archetype.cpp
    src/scene/serialization.cpp
)

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
        src/gfx/render_graph.cpp
    )
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
    target_link_libraries(aether_engine PUBLIC d3d12 dxgi d3dcompiler)
elseif(APPLE)
    target_compile_definitions(aether_engine PUBLIC AETHER_PLATFORM_MACOS)
else()
    target_compile_definitions(aether_engine PUBLIC AETHER_PLATFORM_LINUX)
endif()

add_library(Aether::Engine ALIAS aether_engine)
